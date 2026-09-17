// The terrain sidecar format and grid walk against a synthetic terrain grid
// built to the game's exact layout (CTerrain+0x18 -> {x0,y0,nx,ny,records};
// 40-byte records; height vector at record+8; version at record+0x20). No game.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <random>
#include <cassert>
#include "../src/terrain_sidecar.h"

using namespace TerrainSidecar;

// A stand-in CTerrain: a pointer at +0x18 to a grid blob, which holds the
// record array inline after its 0x18-byte head.
struct FakeTerrain {
    uint8_t cterrain[0x20];
    std::vector<uint8_t> grid;
    std::vector<std::vector<uint16_t>> caches;   // backing store for each record's vector
    FakeTerrain(int nx, int ny) {
        memset(cterrain, 0, sizeof cterrain);
        grid.assign(0x18 + size_t(nx) * ny * 40, 0);
        *reinterpret_cast<int32_t*>(grid.data() + 0) = 0;
        *reinterpret_cast<int32_t*>(grid.data() + 4) = 0;
        *reinterpret_cast<int32_t*>(grid.data() + 8) = nx;
        *reinterpret_cast<int32_t*>(grid.data() + 0xc) = ny;
        caches.resize(size_t(nx) * ny);
        // records ptr and the CTerrain+0x18 grid ptr, filled after the vector
        // (reallocs of `grid`/`caches` are done; addresses are now stable).
    }
    void finalize() {
        *reinterpret_cast<uint8_t**>(grid.data() + 0x10) = grid.data() + 0x18;
        *reinterpret_cast<uint8_t**>(cterrain + 0x18) = grid.data();
    }
    uint8_t* record(uint32_t i) { return grid.data() + 0x18 + size_t(i) * 40; }
    // Give record i a height cache of `n` samples filled from `seed`.
    void makeTile(uint32_t i, size_t n, uint32_t seed) {
        auto& c = caches[i]; c.resize(n);
        std::mt19937 rng(seed); uint16_t h = uint16_t(20000 + rng() % 500);
        for (size_t k = 0; k < n; ++k) { h = uint16_t(h + int(rng() % 7) - 3); c[k] = h; }
        auto* v = VectorOf(record(i));
        v->first = c.data(); v->last = c.data() + n; v->end = c.data() + n;
        *reinterpret_cast<int32_t*>(record(i) + 0) = int32_t(i);       // entity
        *reinterpret_cast<int32_t*>(record(i) + 0x20) = 1;             // version
    }
};

int main() {
    const int nx = 40, ny = 30;   // 1,200 records
    const char* path = "test_sidecar.bin";
    FakeTerrain save(nx, ny);
    std::vector<uint32_t> live;
    std::mt19937 pick(1);
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
        // ~70% are full 1 m tiles; a few are wrong-sized (holes) to be skipped.
        if (pick() % 10 < 7) { save.makeTile(i, Samples, i * 7 + 1); live.push_back(i); }
        else if (pick() % 3 == 0) save.makeTile(i, 129 * 129, i);   // half-res: not eligible
    }
    save.finalize();
    auto* enc = new BlockCodec::EncodeScratch;
    uint64_t bytes = 0;
    long written = Write(GridOf(save.cterrain), 0xABCDEF12u, path, enc, &bytes);
    assert(written == long(live.size()));
    printf("sidecar: %ld tiles, %.2f MiB compressed, %.1f%% of raw\n",
           written, bytes / 1048576.0, 100.0 * bytes / (double(live.size()) * Samples * 2));

    // Apply into a fresh grid with the SAME tiles present but zeroed caches.
    FakeTerrain load(nx, ny);
    for (uint32_t i : live) { load.makeTile(i, Samples, 0); for (auto& x : load.caches[i]) x = 0; }
    // An eligible tile the save did NOT store must be left alone.
    std::vector<char> stored(nx * ny, 0); for (uint32_t i : live) stored[i] = 1;
    uint32_t unused = 0; while (unused < uint32_t(nx * ny) && stored[unused]) ++unused;
    load.makeTile(unused, Samples, 12345); auto probe = load.caches[unused];
    load.finalize();
    auto* dec = new BlockCodec::DecodeScratch;
    long applied = Apply(GridOf(load.cterrain), 0xABCDEF12u, path, dec);
    assert(applied == long(live.size()));
    for (uint32_t i : live) assert(load.caches[i] == save.caches[i]);   // exact restore
    assert(load.caches[unused] == probe);                               // untouched
    printf("apply: %ld tiles restored exactly\n", applied);

    // Fingerprint mismatch, dimension mismatch, and absent file are all no-ops.
    assert(Apply(GridOf(load.cterrain), 0xDEADBEEFu, path, dec) == 0);
    FakeTerrain small(nx, ny - 1); small.finalize();
    assert(Apply(GridOf(small.cterrain), 0xABCDEF12u, path, dec) == 0);
    assert(Apply(GridOf(load.cterrain), 0xABCDEF12u, "does_not_exist.bin", dec) == 0);

    // A truncated file is reported corrupt (-1), never a wrong restore.
    { FILE* f = nullptr; fopen_s(&f, path, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
      std::vector<uint8_t> buf(sz); fopen_s(&f, path, "rb"); assert(fread(buf.data(), 1, sz, f) == size_t(sz)); fclose(f);
      const char* tp = "test_sidecar_trunc.bin"; fopen_s(&f, tp, "wb"); fwrite(buf.data(), 1, sz - 100, f); fclose(f);
      assert(Apply(GridOf(load.cterrain), 0xABCDEF12u, tp, dec) == -1); remove(tp); }

    // A flipped byte in the compressed body is caught by the codec hash.
    { FILE* f = nullptr; fopen_s(&f, path, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
      std::vector<uint8_t> buf(sz); fopen_s(&f, path, "rb"); assert(fread(buf.data(), 1, sz, f) == size_t(sz)); fclose(f);
      buf[sizeof(FileHeader) + sizeof(TileHeader) + 20] ^= 0x40;
      const char* cp = "test_sidecar_corrupt.bin"; fopen_s(&f, cp, "wb"); fwrite(buf.data(), 1, sz, f); fclose(f);
      long r = Apply(GridOf(load.cterrain), 0xABCDEF12u, cp, dec); assert(r == -1); remove(cp); }

    remove(path); delete enc; delete dec;
    printf("PASS: write walks the grid, apply restores exactly, foreign/stale/absent are no-ops, truncation and corruption are rejected\n");
    return 0;
}
