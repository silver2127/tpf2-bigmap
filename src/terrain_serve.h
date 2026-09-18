// Steam 35924: serve a tile's aligned 1 m height cache from the terrain
// sidecar (terrain_sidecar.h) the moment the load creates the tile, and keep
// the load's own publication from overwriting it.
//
// CTerrain::AddTile (0x33cb60, `void(CTerrain* this, int entity)`) is the one
// place a tile record comes to life: it computes the record inline
// (index = (x-x0) + (y-y0)*nx, record = records + index*40, 0x33cc60), stores
// the entity at record+0 (0x33cc70), detaches the record's control block
// (`lea rcx,[record+8]; call 0x33dd20` at 0x33cc90, which returns the vector
// at control+0x10), resizes that vector to 257*257 (0x33cca5; the pager's
// resize hook hands it a slot of the tile arena) and bumps record+0x20. It
// returns void and never exposes the record, so the post-hook finds it by
// entity: tiles are added in grid order, so a rotating cursor over the records
// hits on the first probe almost every time.
//
// When the sidecar is loaded for this save (TerrainSidecar::Loaded, filled by
// the LoadGame hook before the alignment pass) and holds this record, the
// hook decodes the saved cache straight into the fresh vector (ApplyTile: the
// vector is private and writable right after AddTile's own detach) and marks
// the pager slot `served`. The uint16 block copy replacement (terrain_minmax.h,
// 0x30a540) then skips every copy whose destination lies in a served slot:
// the refine and the alignment pass still compute their blocks, but the
// finished cache they would publish is the one already there.
//
// Off (stock behaviour) when the sidecar is not loaded, when the tile arena
// does not own the vector (no pager: the mark fails, publication proceeds and
// the tile ends up stock), or with terrain_sidecar=0.
#pragma once
static int g_terrainServe = 1;   // terrain_sidecar cfg: 0 = never apply a sidecar
namespace TerrainServe {
using AddTileFn = void(__fastcall*)(void* terrain, int entity, uint64_t a2, uint64_t a3);
static AddTileFn original = nullptr;
static uintptr_t base = 0;
constexpr uintptr_t kAddTileRva = 0x33cb60, kRecordStoreRva = 0x33cc60, kDetachRva = 0x33cc90;
static const uint8_t kAddTileBytes[18] = {0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff};
// lea rcx,[rax+rax*4]; mov rax,[r9+0x10]; lea rsi,[rax+rcx*8]; mov eax,[rsp+0x48]; mov [rsi],eax
static const uint8_t kRecordStoreBytes[18] = {0x48, 0x8d, 0x0c, 0x80, 0x49, 0x8b, 0x41, 0x10, 0x48, 0x8d, 0x34, 0xc8, 0x8b, 0x44, 0x24, 0x48, 0x89, 0x06};
// lea rcx,[rsi+8]; call 0x33dd20 (rel32 0x1087 from 0x33cc99)
static const uint8_t kDetachBytes[9] = {0x48, 0x8d, 0x4e, 0x08, 0xe8, 0x87, 0x10, 0x00, 0x00};
// Coupling to the tile arena (set by InstallTerrainServe; the offline test supplies its own).
static bool (*mark)(const void* first) = nullptr;
static volatile LONG64 calls = 0, applied = 0, unmarked = 0, absent = 0, notFound = 0, probes = 0, decodeFailed = 0;
static volatile LONG cursor = 0;   // record index after the last hit; a hint, races are benign
static thread_local BlockCodec::DecodeScratch* scratch = nullptr;

// The record AddTile just filled for `entity`, or -1: scan from the cursor.
static long FindRecord(const TerrainSidecar::Grid& g, int entity) {
    const uint32_t n = g.nx() > 0 && g.ny() > 0 ? uint32_t(g.nx()) * uint32_t(g.ny()) : 0;
    if (!n) return -1;
    uint32_t start = uint32_t(cursor) % n;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t i = start + k; if (i >= n) i -= n;
        InterlockedIncrement64(&probes);
        const uint8_t* r = g.record(i);
        if (*reinterpret_cast<const int32_t*>(r) == entity && *reinterpret_cast<uint8_t* const*>(r + 8)) {
            InterlockedExchange(&cursor, LONG(i + 1 < n ? i + 1 : 0));
            return long(i);
        }
    }
    return -1;
}
static void __fastcall Detour(void* terrain, int entity, uint64_t a2, uint64_t a3) {
    original(terrain, entity, a2, a3);
    InterlockedIncrement64(&calls);
    if (!g_terrainServe || !terrain || !TerrainSidecar::Loaded()) return;
    TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
    if (!g.base || !g.records()) return;
    long idx = FindRecord(g, entity);
    if (idx < 0) { InterlockedIncrement64(&notFound); return; }
    if (!TerrainSidecar::Has(uint32_t(idx))) { InterlockedIncrement64(&absent); return; }
    if (!scratch) scratch = new (std::nothrow) BlockCodec::DecodeScratch;
    if (!scratch) return;
    if (!TerrainSidecar::ApplyTile(g, uint32_t(idx), *scratch)) { InterlockedIncrement64(&decodeFailed); return; }
    const uint8_t* control = *reinterpret_cast<uint8_t* const*>(g.record(uint32_t(idx)) + 8);
    const auto* v = reinterpret_cast<const TerrainSidecar::TileVector*>(control + 0x10);
    if (mark && mark(v->first)) InterlockedIncrement64(&applied);
    else InterlockedIncrement64(&unmarked);
}
}  // namespace TerrainServe
static bool InstallTerrainServe() {
    using namespace TerrainServe;
    if (!g_terrainServe || g_gog) return false;
    if (!H->verifyBytes(kAddTileRva, kAddTileBytes, sizeof kAddTileBytes) ||
        !H->verifyBytes(kRecordStoreRva, kRecordStoreBytes, sizeof kRecordStoreBytes) ||
        !H->verifyBytes(kDetachRva, kDetachBytes, sizeof kDetachBytes)) {
        H->log("terrain sidecar: AddTile byte mismatch; serving OFF"); return false;
    }
    base = H->moduleBase();
    if (!H->installHook(base + kAddTileRva, reinterpret_cast<void*>(Detour), sizeof kAddTileBytes, reinterpret_cast<void**>(&original))) {
        H->log("terrain sidecar: AddTile hook failed; serving OFF"); return false;
    }
    mark = TerrainPager::SetServed;
    g_terrainServedCheck = TerrainPager::IsServed;
    H->log("terrain sidecar: a loaded sidecar's tiles are applied at AddTile and the load's publication into them is skipped");
    return true;
}
// One line for the pager's 30 s log, empty when nothing happened.
static int TerrainServeStatus(char* out, size_t cap) {
    using namespace TerrainServe;
    if (!calls) { if (cap) out[0] = 0; return 0; }
    return snprintf(out, cap, " sidecar: add_tile=%lld applied=%lld unmarked=%lld absent=%lld not_found=%lld decode_failed=%lld probes=%lld copies_skipped=%lld",
        calls, applied, unmarked, absent, notFound, decodeFailed, probes, g_terrainServedCopiesSkipped);
}
extern "C" __declspec(dllexport) void BigmapTestServeDetour(void* terrain, int entity, TerrainServe::AddTileFn fn, bool (*markFn)(const void*)) {
    TerrainServe::original = fn; TerrainServe::mark = markFn;
    TerrainServe::Detour(terrain, entity, 0, 0);
}
extern "C" __declspec(dllexport) void BigmapTestServeCounters(long long* out) {
    out[0] = TerrainServe::calls; out[1] = TerrainServe::applied; out[2] = TerrainServe::unmarked; out[3] = TerrainServe::absent;
    out[4] = TerrainServe::notFound; out[5] = TerrainServe::probes; out[6] = TerrainServe::decodeFailed;
}
