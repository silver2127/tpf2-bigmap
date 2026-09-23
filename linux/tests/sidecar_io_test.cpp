// Portable file operations only; no game hooks or live ownership assumptions.
#include <array>
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
    RemoveFile(file.c_str());
    RemoveFile(junk.c_str());
    assert(access(file.c_str(), F_OK) != 0 && access(junk.c_str(), F_OK) != 0);
    assert(rmdir(dir.c_str()) == 0 && rmdir(folder) == 0);
    delete enc; delete dec;
}
