// Steam 35924: a terrain sidecar file, so a save's aligned 1 m height cache is
// restored on load instead of rebuilt by the refine + alignment pass.
//
// The .sav stores the 4 m base heightmap and every road/track/construction
// alignment, NOT the finished 1 m cache. On load the engine rebuilds the cache
// (bicubic refine, terrain_util 0x3c4620) and then re-cuts it with every
// alignment (ecs::TerrainAlignmentSystem, 0xaad6c0). On a 207,360-tile save
// that pass is the load's memory peak and a large share of its time. The
// finished cache is a pure function of the save, so writing it beside the save
// and splatting it back on load of the SAME save is exact.
//
// This header owns the FILE FORMAT and the GRID WALK only. It reads and writes
// the live terrain grid through the layout confirmed by RE (CTerrain::GetTile
// 0x33d580, AddTile 0x33cb60):
//
//   CTerrain* + 0x18            -> grid*
//   grid + 0x00 int32 x0        (window origin, unused here)
//   grid + 0x04 int32 y0
//   grid + 0x08 int32 nx        (record columns)
//   grid + 0x0c int32 ny        (record rows)
//   grid + 0x10 void* records   (nx*ny records, 40 bytes each)
//   record + 0x00 int32 entity  (the tile entity id AddTile stored; not a liveness flag)
//   record + 0x08 void* control (shared block; nullptr == no tile)
//   control + 0x10 {uint16* first, * last, * end}   the height cache vector
//   record + 0x20 int32 version
//
// record+8 is NOT the vector: it points at a control block shared between the
// two CTerrain versions, and the std::vector<uint16_t> sits at control+0x10.
// AddTile (0x33cb60) at 0x33cc90 does `lea rcx,[record+8]; call 0x33dd20`
// (detach/make-writable, returns the vector = control+0x10) then resize to
// Side*Side. A tile is eligible when its control block exists and that vector
// holds exactly Side*Side samples (66,049 for the 1 m cache).
//
// ApplyTile WRITES the vector. It is called only from the pass owner's AddTile
// post-hook, i.e. straight after AddTile has already detached (0x33dd20) and
// resized the block, so the vector is private and writable and ApplyTile needs
// no engine call. Its `first` may point into the terrain pager's arena; the
// Decode write faults it in as usual. Each is compressed with the block codec.
// The file is
// keyed to the save by a caller-supplied 64-bit fingerprint (the .sav hash);
// Apply refuses a file whose fingerprint or grid dimensions do not match, so a
// foreign, stale or plugin-less save is never touched.
//
// This header does not hook the game. The SaveGame/LoadGame hooks and the
// short-circuit of the alignment pass are integrated separately; see the
// exported test entry points and docs/terrain-sidecar.md.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "small_codec.h"
#include "terrain_codec.h"   // Side, Samples

namespace TerrainSidecar {
constexpr uint32_t Magic = 0x52524554;      // 'TERR'
constexpr uint32_t Version = 1;
constexpr size_t Side = TerrainCodec::Side;         // 257
constexpr size_t Samples = TerrainCodec::Samples;   // 66,049

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic, version;
    uint64_t fingerprint;       // the save's content hash (caller-supplied)
    int32_t nx, ny;             // grid dimensions, so a resized world is rejected
    uint32_t tiles;             // eligible tiles stored
    uint64_t headerHash;        // hash of the fields above, for a torn/truncated file
};
struct TileHeader { uint32_t index; uint32_t bytes; };   // record index in the grid, compressed length
#pragma pack(pop)

// The engine's terrain grid, as a raw view (never constructed by us).
struct Grid {
    uint8_t* base;
    int32_t x0() const { return *reinterpret_cast<const int32_t*>(base + 0); }
    int32_t y0() const { return *reinterpret_cast<const int32_t*>(base + 4); }
    int32_t nx() const { return *reinterpret_cast<const int32_t*>(base + 8); }
    int32_t ny() const { return *reinterpret_cast<const int32_t*>(base + 0xc); }
    uint8_t* records() const { return *reinterpret_cast<uint8_t* const*>(base + 0x10); }
    uint8_t* record(uint32_t i) const { return records() + size_t(i) * 40; }
};
// The record index for tile coordinate (x, y), the same value CTerrain::GetTile
// (0x33d580) and AddTile (0x33cb60) compute: (x - x0) + (y - y0) * nx. -1 when
// the coordinate is outside the grid window. This is the index the file stores
// and the argument Has()/ApplyTile() expect. A caller that already holds the
// engine's record pointer can use IndexOfRecord instead.
inline long RecordIndex(const Grid& g, int32_t x, int32_t y) {
    const int32_t x0 = g.x0(), y0 = g.y0(), nx = g.nx(), ny = g.ny();
    if (x < x0 || y < y0 || x >= x0 + nx || y >= y0 + ny) return -1;
    return long(x - x0) + long(y - y0) * nx;
}
inline long IndexOfRecord(const Grid& g, const uint8_t* record) {
    if (!g.records() || record < g.records()) return -1;
    size_t off = size_t(record - g.records());
    if (off % 40) return -1;
    long i = long(off / 40);
    return i < long(g.nx()) * g.ny() ? i : -1;
}
inline Grid GridOf(void* cterrain) { return Grid{*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(cterrain) + 0x18)}; }
struct TileVector { uint16_t* first; uint16_t* last; uint16_t* end; };
// record+8 -> control block; the vector is at control+0x10. nullptr control
// (no tile) yields a null vector, which Eligible rejects.
inline TileVector* VectorOf(uint8_t* record) {
    uint8_t* control = *reinterpret_cast<uint8_t**>(record + 8);
    return control ? reinterpret_cast<TileVector*>(control + 0x10) : nullptr;
}
inline bool Eligible(const TileVector* v) { return v && v->first && size_t(v->last - v->first) == Samples; }

inline uint64_t MixHash(uint64_t h, uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
inline uint64_t HashHeader(const FileHeader& h) {
    uint64_t v = 0;
    v = MixHash(v, h.magic); v = MixHash(v, h.version); v = MixHash(v, h.fingerprint);
    v = MixHash(v, uint64_t(uint32_t(h.nx))); v = MixHash(v, uint64_t(uint32_t(h.ny))); v = MixHash(v, h.tiles);
    return v;
}

// ---- Write: compress every eligible tile of `grid` to `path`. ----
// Returns the number of tiles written, or -1 on an I/O or allocation failure.
// A partial file is removed on failure so a later Apply never sees it.
inline long Write(const Grid& grid, uint64_t fingerprint, const char* path,
                  BlockCodec::EncodeScratch* scratch, uint64_t* compressedBytesOut = nullptr) {
    if (!grid.base || !grid.records()) return -1;
    const int32_t nx = grid.nx(), ny = grid.ny();
    if (nx <= 0 || ny <= 0 || int64_t(nx) * ny > (int64_t(1) << 24)) return -1;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return -1;
    FileHeader hdr{Magic, Version, fingerprint, nx, ny, 0, 0};
    // Header rewritten at the end with the true tile count and hash.
    if (fwrite(&hdr, sizeof hdr, 1, f) != 1) { fclose(f); remove(path); return -1; }
    std::vector<uint8_t> blob(Samples * 2 + 128);
    uint32_t tiles = 0; uint64_t compressed = 0; bool ok = true;
    const uint32_t count = uint32_t(nx) * uint32_t(ny);
    for (uint32_t i = 0; i < count && ok; ++i) {
        TileVector* v = VectorOf(grid.record(i));
        if (!Eligible(v)) continue;
        size_t n = BlockCodec::Encode(v->first, Samples, blob.data(), blob.size(), *scratch);
        if (!n) continue;   // an incompressible tile is simply omitted; Apply leaves it to the pass
        TileHeader th{i, uint32_t(n)};
        if (fwrite(&th, sizeof th, 1, f) != 1 || fwrite(blob.data(), 1, n, f) != n) { ok = false; break; }
        ++tiles; compressed += n;
    }
    if (ok) {
        hdr.tiles = tiles; hdr.headerHash = HashHeader(hdr);
        if (fseek(f, 0, SEEK_SET) || fwrite(&hdr, sizeof hdr, 1, f) != 1) ok = false;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(path); return -1; }
    if (compressedBytesOut) *compressedBytesOut = compressed;
    return long(tiles);
}

// ---- Apply: fill every stored tile of `path` into `grid`. ----
// Returns the number of tiles applied, 0 if the file is absent/foreign/stale
// (the caller then lets the pass run), or -1 on a corrupt-but-matching file
// (the caller must let the pass run and should discard the file). Only writes
// into a record whose vector is already the right size (the tile exists); it
// never allocates a tile, so it runs after the pass has created them.
inline long Apply(const Grid& grid, uint64_t fingerprint, const char* path,
                  BlockCodec::DecodeScratch* scratch) {
    if (!grid.base || !grid.records()) return 0;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return 0;
    FileHeader hdr{};
    if (fread(&hdr, sizeof hdr, 1, f) != 1) { fclose(f); return 0; }
    if (hdr.magic != Magic || hdr.version != Version || hdr.headerHash != HashHeader(hdr)) { fclose(f); return 0; }
    if (hdr.fingerprint != fingerprint || hdr.nx != grid.nx() || hdr.ny != grid.ny()) { fclose(f); return 0; }
    const uint32_t count = uint32_t(grid.nx()) * uint32_t(grid.ny());
    std::vector<uint8_t> blob(Samples * 2 + 128);
    long applied = 0; bool corrupt = false;
    for (uint32_t k = 0; k < hdr.tiles; ++k) {
        TileHeader th{};
        if (fread(&th, sizeof th, 1, f) != 1) { corrupt = true; break; }
        if (th.bytes == 0 || th.bytes > blob.size() || th.index >= count) { corrupt = true; break; }
        if (fread(blob.data(), 1, th.bytes, f) != th.bytes) { corrupt = true; break; }
        TileVector* v = VectorOf(grid.record(th.index));
        if (!Eligible(v)) continue;   // tile not present this load: skip, leave it to the pass
        if (!BlockCodec::Decode(blob.data(), th.bytes, v->first, Samples, *scratch)) { corrupt = true; break; }
        ++applied;
    }
    fclose(f);
    return corrupt ? -1 : applied;
}
}  // namespace TerrainSidecar

// ---- Streaming load-side API, for the alignment pass to consume per tile. ----
// The pass creates each tile (AddTile) and would then compute its cache. When
// a sidecar for this exact save is loaded, the pass owner skips that compute
// for a tile Has(index) reports, and calls ApplyTile to splat the saved cache
// instead. BeginApply verifies EVERY stored blob decodes before reporting any
// tile, so Has(index)==true guarantees ApplyTile(index) succeeds; the pass can
// therefore skip the compute without a fallback. The compressed file is held
// in memory (~1.2 GiB for a full map, far under the pass's own former peak)
// until EndApply. All of BeginApply/EndApply run once; Has/ApplyTile are
// lock-free reads of the immutable index and may run on any pass thread
// (ApplyTile takes the caller's own DecodeScratch).
namespace TerrainSidecar {
struct LoadState {
    std::vector<uint8_t> file;                       // the whole sidecar in memory
    std::vector<std::pair<uint32_t, uint32_t>> byIndex;   // record index -> {file offset, bytes}, offset 0 == absent
    bool loaded = false;
    uint32_t tiles = 0;
};
static LoadState g_load;

// Open, validate against the live grid's fingerprint and dimensions, index
// every tile and verify each blob decodes. Returns the number of tiles ready,
// or 0 (nothing held) if the file is absent/foreign/stale/corrupt.
inline long BeginApply(const Grid& grid, uint64_t fingerprint, const char* path, BlockCodec::DecodeScratch* verify) {
    g_load = LoadState{};
    if (!grid.base || !grid.records()) return 0;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < long(sizeof(FileHeader))) { fclose(f); return 0; }
    std::vector<uint8_t> buf; buf.resize(size_t(sz));
    bool read = fread(buf.data(), 1, size_t(sz), f) == size_t(sz);
    fclose(f);
    if (!read) return 0;
    FileHeader hdr{}; memcpy(&hdr, buf.data(), sizeof hdr);
    if (hdr.magic != Magic || hdr.version != Version || hdr.headerHash != HashHeader(hdr)) return 0;
    if (hdr.fingerprint != fingerprint || hdr.nx != grid.nx() || hdr.ny != grid.ny()) return 0;
    const uint32_t count = uint32_t(grid.nx()) * uint32_t(grid.ny());
    std::vector<std::pair<uint32_t, uint32_t>> index(count, {0u, 0u});
    std::vector<uint16_t> scratchOut(Samples);
    size_t p = sizeof(FileHeader);
    for (uint32_t k = 0; k < hdr.tiles; ++k) {
        if (p + sizeof(TileHeader) > size_t(sz)) return 0;
        TileHeader th{}; memcpy(&th, buf.data() + p, sizeof th); p += sizeof(TileHeader);
        if (th.bytes == 0 || p + th.bytes > size_t(sz) || th.index >= count) return 0;
        if (!BlockCodec::Decode(buf.data() + p, th.bytes, scratchOut.data(), Samples, *verify)) return 0;
        index[th.index] = {uint32_t(p), th.bytes};   // p != 0 always (past the header)
        p += th.bytes;
    }
    g_load.file = std::move(buf);
    g_load.byIndex = std::move(index);
    g_load.tiles = hdr.tiles;
    g_load.loaded = true;
    return long(hdr.tiles);
}
inline bool Loaded() { return g_load.loaded; }
inline bool Has(uint32_t recordIndex) {
    return g_load.loaded && recordIndex < g_load.byIndex.size() && g_load.byIndex[recordIndex].first != 0;
}
// Decode the saved cache for `recordIndex` into that tile's height vector.
// False if not loaded, not stored, the tile is not present/eligible, or (not
// possible after BeginApply verified it) the blob fails to decode.
inline bool ApplyTile(const Grid& grid, uint32_t recordIndex, BlockCodec::DecodeScratch& scratch) {
    if (!Has(recordIndex)) return false;
    TileVector* v = VectorOf(grid.record(recordIndex));
    if (!Eligible(v)) return false;
    const auto& e = g_load.byIndex[recordIndex];
    return BlockCodec::Decode(g_load.file.data() + e.first, e.second, v->first, Samples, scratch);
}
inline void EndApply() { g_load = LoadState{}; }

// ---- Fingerprint and the save/load glue. ----
// The fingerprint ties a sidecar to one save: a 64-bit hash of the .sav bytes.
// The aligned terrain is a pure function of those bytes, so an exact hash match
// means the stored caches are correct, and any other save (even the same map
// with different roads) hashes differently and is rejected.
inline uint64_t HashFile(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return 0;
    uint64_t h = 0xCBF29CE484222325ull; uint64_t total = 0;
    uint8_t buf[1 << 16];
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (!n) break;
        total += n;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, buf + i, 8); h = TerrainCodec::Rotl(h ^ (w * 0xC2B2AE3D27D4EB4Full), 31) * 0x9E3779B185EBCA87ull; }
        for (; i < n; ++i) h = TerrainCodec::Rotl(h ^ (uint64_t(buf[i]) * 0xC2B2AE3D27D4EB4Full), 27) * 0x9E3779B185EBCA87ull;
    }
    fclose(f);
    if (!total) return 0;              // empty/unreadable -> 0, which callers treat as "no fingerprint"
    h ^= total; h ^= h >> 33; h *= 0xC2B2AE3D27D4EB4Full; h ^= h >> 29;
    return h ? h : 1;                  // never 0 for a real file
}
inline void SidecarPath(const char* savPath, char* out, size_t cap) {
    // "<save>.sav" -> "<save>.terr"; otherwise "<path>.terr".
    size_t n = strlen(savPath);
    const char* ext = (n >= 4 && !_stricmp(savPath + n - 4, ".sav")) ? savPath + n - 4 : savPath + n;
    size_t base = size_t(ext - savPath);
    if (base + 6 >= cap) { if (cap) out[0] = 0; return; }
    memcpy(out, savPath, base); memcpy(out + base, ".terr", 6);
}

static uint64_t g_saveFingerprint = 0;
static char g_sidecarPath[520] = {0};
static bool g_pending = false;         // a fingerprint is armed; BeginApply not yet run this load (set on
                                       // the load thread before the pass, read once at its entry)

// Called from the LoadGame hook once the .sav path is known, before the world
// builds. Hashes the save and arms the sidecar; the actual BeginApply waits for
// the CTerrain, which BeginIfPending supplies.
inline void ArmForLoad(const char* savPath) {
    g_saveFingerprint = HashFile(savPath);
    SidecarPath(savPath, g_sidecarPath, sizeof g_sidecarPath);
    g_pending = g_saveFingerprint && g_sidecarPath[0];
}
// Called with the CTerrain the pass will populate, before its first AddTile
// (the pass owner's Detour entry is the natural spot). Runs BeginApply once.
// Returns true if a valid sidecar is now loaded.
inline bool BeginIfPending(void* cterrain, BlockCodec::DecodeScratch* verify) {
    if (!g_pending || !cterrain) return Loaded();
    g_pending = false;
    return BeginApply(GridOf(cterrain), g_saveFingerprint, g_sidecarPath, verify) > 0;
}
// Called from the SaveGame hook after the save is written, with the CTerrain and
// the .sav just written. Hashes the save and writes the sidecar beside it.
inline long WriteForSave(void* cterrain, const char* savPath, BlockCodec::EncodeScratch* scratch, uint64_t* bytesOut = nullptr) {
    uint64_t fp = HashFile(savPath);
    if (!fp || !cterrain) return -1;
    char path[520]; SidecarPath(savPath, path, sizeof path);
    if (!path[0]) return -1;
    return Write(GridOf(cterrain), fp, path, scratch, bytesOut);
}
}  // namespace TerrainSidecar

// ---- Offline test entry points (no game state). ----
extern "C" __declspec(dllexport) long BigmapTestSidecarWrite(void* cterrain, uint64_t fp, const char* path, uint64_t* bytesOut) {
    auto* s = new BlockCodec::EncodeScratch;
    long r = TerrainSidecar::Write(TerrainSidecar::GridOf(cterrain), fp, path, s, bytesOut);
    delete s; return r;
}
extern "C" __declspec(dllexport) long BigmapTestSidecarApply(void* cterrain, uint64_t fp, const char* path) {
    auto* s = new BlockCodec::DecodeScratch;
    long r = TerrainSidecar::Apply(TerrainSidecar::GridOf(cterrain), fp, path, s);
    delete s; return r;
}
