// Portable file operations only; no game hooks or live ownership assumptions.
#include <array>
#include <atomic>
#include <thread>
#include <memory>
#include <fcntl.h>
#include "../sidecar_files.h"
#include <cassert>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include "../../src/terrain_sidecar.h"

int main() {
    using namespace TerrainSidecar;
    char folder[] = "sidecar-XXXXXX";
    assert(mkdtemp(folder));
    std::string dir = std::string(folder) + u8"/Spielstände Ж";
    assert(mkdir(dir.c_str(), 0700) == 0);
    const std::string file = dir + "/world.terr";
    const std::string missing = dir + "/mp_shared.terr";
    std::vector<uint16_t> samples(Samples, 12345);
    TileVector vector{samples.data(), samples.data() + Samples, samples.data() + Samples};
    alignas(8) std::array<uint8_t, 40> record{};
    alignas(8) std::array<uint8_t, 24> header{};
    *reinterpret_cast<TileVector**>(record.data() + 8) = &vector;
    // Deliberately unrelated control pointer: VectorOf must use the object pointer.
    assert(VectorOf(record.data()) == &vector);
    *reinterpret_cast<int32_t*>(header.data() + 8) = 1;
    *reinterpret_cast<int32_t*>(header.data() + 12) = 1;
    *reinterpret_cast<uint8_t**>(header.data() + 16) = record.data();
    Grid grid{header.data()};
    auto enc = new BlockCodec::EncodeScratch;
    auto dec = new BlockCodec::DecodeScratch;
    constexpr uint64_t fp = 0x1234567812345678;
    assert(Write(grid, 0, file.c_str(), enc) == 1);
    assert(Apply(grid, fp, file.c_str(), dec) == 0);
    assert(Refingerprint(file.c_str(), fp));
    assert(FingerprintOf(file.c_str()) == fp);
    std::fill(samples.begin(), samples.end(), 0);
    assert(Apply(grid, fp, file.c_str(), dec) == 1);
    for (auto sample : samples) assert(sample == 12345);
    // Four independent writable tiles race eight pass-completion callbacks.
    // The file must be released once, with no decode observing freed storage.
    unsigned decoded = 0;
    for (int round = 0; round < 200; ++round) {
        assert(BeginApply(grid, fp, file.c_str()) == 1);
        assert(Loaded() && Has(0) && !Has(1));
        assert(ApplyTile(grid, 0, *dec));
        std::atomic<bool> go{false};
        std::atomic<unsigned> ready{0}, released{0}, intact{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
            auto scratch = std::make_unique<BlockCodec::DecodeScratch>();
            auto data = samples;
            TileVector v{data.data(), data.data()+Samples, data.data()+Samples};
            auto r = record;
            auto h = header;
            *reinterpret_cast<TileVector**>(r.data()+8) = &v;
            *reinterpret_cast<uint8_t**>(h.data()+16) = r.data();
            ++ready;
            while (!go.load()) std::this_thread::yield();
            for (int j = 0; j < 8; ++j) {
                std::fill(data.begin(), data.end(), 0);
                if (ApplyTile(Grid{h.data()}, 0, *scratch)) {
                    assert(data == samples);
                    ++intact;
                } else for (auto value : data) assert(value == 0);
            }
        });
        for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
            ++ready;
            while (!go.load()) std::this_thread::yield();
            // Half the rounds release immediately; half guarantee that readers
            // have started decoding before the competing completion callbacks.
            if (round % 2) while (!intact.load()) std::this_thread::yield();
            if (EndApply()) ++released;
        });
        while (ready.load() != 12) std::this_thread::yield();
        go = true;
        for (auto& thread : threads) thread.join();
        decoded += intact.load();
        assert(released == 1 && !Loaded() && !Has(0));
        assert(!EndApply() && !ApplyTile(grid, 0, *dec));
    }
    assert(decoded >= 100);
    printf("sidecar release race: 200 rounds, 4 readers/8 releasers, %u concurrent decodes intact\n", decoded);
    // BeginApply replaces a prior file and invalid input clears the old state.
    assert(BeginApply(grid, fp, file.c_str()) == 1);
    assert(BeginApply(grid, fp, file.c_str()) == 1);
    assert(EndApply() && !EndApply());
    assert(BeginApply(grid, fp, file.c_str()) == 1);
    assert(BeginApply(grid, fp+1, file.c_str()) == 0 && !Loaded());

    char path[1024];
    strcpy(path, missing.c_str());
    assert(FindByFingerprint(fp, path, sizeof path) && path == file);
    assert(FindByFingerprint(fp, path, sizeof path) && path == file);
    strcpy(path, missing.c_str());
    assert(!FindByFingerprint(fp + 1, path, sizeof path) && path == missing);
    assert(!FindByFingerprint(0, path, sizeof path) && path == missing);
    assert(!FindByFingerprint(fp, path, 3) && path == missing);
    // Candidate would not fit: do not modify the caller's path.
    strcpy(path, (dir + "/x").c_str());
    std::string shortPath = path;
    assert(!FindByFingerprint(fp, path, shortPath.size() + 1) && path == shortPath);
    const std::string junk = dir + "/junk.terr";
    FILE* f = OpenFile(junk.c_str(), L"wb");
    assert(f && fwrite("junk", 4, 1, f) == 1 && fclose(f) == 0);
    assert(!Refingerprint(junk.c_str(), fp) && !FingerprintOf(junk.c_str()));
    // Torn header hash must be rejected by discovery and stamping.
    f = OpenFile(file.c_str(), L"r+b");
    assert(f && fseek(f, 8, SEEK_SET) == 0 && fputc(0, f) != EOF && fclose(f) == 0);
    assert(!FingerprintOf(file.c_str()) && !Refingerprint(file.c_str(), fp));
    strcpy(path, missing.c_str());
    assert(!FindByFingerprint(fp, path, sizeof path) && path == missing);
    // The autosave name is chosen by the engine, not the SaveGameId name.
    const std::string expected = dir + "/autosave.sav";
    const std::string autosave = dir + "/autosave_World_1850-01-04.sav";
    const std::string older = dir + "/world.sav";
    const std::string other = dir + "/mp_shared.sav";
    const std::string metadata = autosave + ".lua";
    const std::string subdir = dir + "/directory.sav";
    const std::string link = dir + "/link.sav";
    timespec start{10000, 500000000};
    auto touch = [&](const std::string& name, timespec stamp) {
        FILE* stream = fopen(name.c_str(), "wb");
        assert(stream && fclose(stream) == 0);
        timespec times[] = {stamp, stamp};
        assert(utimensat(AT_FDCWD, name.c_str(), times, 0) == 0);
    };
    auto find = [&](size_t capacity = 1024) {
        strcpy(path, "unchanged");
        bool result = linux_sidecar::WrittenDuringCall(expected.c_str(), start, path, capacity);
        if (!result) assert(!strcmp(path, "unchanged"));
        return result;
    };
    touch(older, {9000, 0});
    assert(!find());
    touch(autosave, {10000, 500000000});
    touch(metadata, start);
    assert(mkdir(subdir.c_str(), 0700) == 0);
    assert(symlink("autosave_World_1850-01-04.sav", link.c_str()) == 0);
    assert(find() && path == autosave);
    assert(!find(3));
    touch(autosave, {9998, 499999999});
    assert(!find());
    touch(autosave, {9998, 500000000});
    assert(find() && path == autosave); // exact two-second tolerance boundary
    touch(other, start);
    assert(!find()); // never choose arbitrarily between two fresh saves
    for (const auto& name : {older, autosave, other, metadata, link}) RemoveFile(name.c_str());
    assert(rmdir(subdir.c_str()) == 0);
    RemoveFile(file.c_str());
    RemoveFile(junk.c_str());
    assert(access(file.c_str(), F_OK) != 0 && access(junk.c_str(), F_OK) != 0);
    assert(rmdir(dir.c_str()) == 0 && rmdir(folder) == 0);
    delete enc; delete dec;
}
