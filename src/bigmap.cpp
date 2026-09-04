// tpf2_bigmap -- maps larger than the game will build on its own.
//
// WHAT THE GAME DOES
// The New Game menu turns (size dropdown, ratio dropdown) into a tile count via
//
//     CVec2i UI::`anonymous-namespace'::GetNumTilesNew(int sizeIndex,
//                                                      int formatIndex,
//                                                      const AppConfig&)
//     RVA 0x674aa0, MenuUI.cpp:264-273  (build 35924)
//
// and the caller immediately expands that to a heightmap:
//
//     dim = 1 << terrainLevels          (= 64)
//     heightmapPx = tiles * dim + 1
//
// so ONE TILE IS 256 m (64 px at exactly 4.0 m/px). Confirmed twice: against real
// saves (the .sav header after zstd carries numTilesX/numTilesY at +0x10/+0x14;
// Megalomaniac 1:4 reads (48, 192) = 12288 x 49152 m), and against the live bbox
// the streets pass hands us: 224 tiles -> 57344 m = 224 * 256 exactly.
// The wiki's "24 km" for Megalomaniac is 24576 m rounded down.
//
// WHY A HOOK AND NOT A BYTE PATCH
// GetNumTilesNew has a hard clamp of 224 tiles per axis:
//
//     0x674afa:  B9 E0 00 00 00     mov ecx, 0xE0        ; 224
//                cmp edx, ecx / cmovle ...               ; both axes
//
// You could raise that immediate, but a detour is strictly better: it replaces
// the clamp AND the size lookup in one place, never executes the clamp at all
// for the sizes we care about, and leaves every stock size untouched because we
// call the original for anything we do not claim.
//
// There is also a settings.lua route -- `newGameMenuState.worldDimensionsOverride`
// is a shipped, undocumented escape hatch read at AppConfig+0x28/+0x2c which
// bypasses the preset table entirely. It is subject to the same 224 clamp, so it
// tops out at 56 x 56 km. Use it to sanity-check the game's own behaviour; use
// this plugin to go past it.
//
// LIMITS, MEASURED AND INFERRED
//   180 tiles = 46.1 km  largest even size the STOCK 1 m street raster survives
//   224 tiles = 57.3 km  the game's own clamp; needs street_raster=1 (MEASURED OK)
// 224 tiles with the raster hook is measured working, streets included.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "tpf2mp_plugin.h"

static const Tpf2mpHost* H = nullptr;

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------
static const uintptr_t RVA_GETNUMTILES = 0x674aa0;
static const int       STEAL           = 20;

// The exact prologue, so a shifted RVA is a loud refusal instead of a jmp into
// the middle of some other instruction. 20 bytes lands on an instruction
// boundary (the next is `mov [r11+8], rbx`) and none of it is RIP-relative, so
// it relocates verbatim into the trampoline.
//
//   89 54 24 10              mov  [rsp+0x10], edx
//   4C 8B DC                 mov  r11, rsp
//   57                       push rdi
//   48 83 EC 70              sub  rsp, 0x70
//   49 C7 43 A8 FE FF FF FF  mov  qword [r11-0x58], -2
static const uint8_t EXPECTED[STEAL] = {
    0x89, 0x54, 0x24, 0x10, 0x4C, 0x8B, 0xDC, 0x57,
    0x48, 0x83, 0xEC, 0x70, 0x49, 0xC7, 0x43, 0xA8,
    0xFE, 0xFF, 0xFF, 0xFF,
};

// ---------------------------------------------------------------------------
// The street occupancy raster (RVA 0x90d410)
// ---------------------------------------------------------------------------
// "Creating streets" allocates a std::vector<bool> with ONE BIT PER SQUARE
// METRE across the whole map bounding box, and sizes it with a 32-bit multiply.
// That is the int32 overflow described above: >46.1 km square and it aborts.
//
// The fix is NOT to widen the multiply. nx and ny are stored as int32 at
// +0x40/+0x44 and every access computes an index like y*nx + x, so a correctly
// sized vector would still be addressed with wrapped negative indices past
// 2^31 cells -- silent corruption instead of a clean abort, which is worse.
//
// Instead we change the INPUT. cellSize is an argument (xmm2), so scaling it
// with map size shrinks nx and ny themselves and every downstream int32 index
// stays in range untouched. No audit, no corruption risk.
//
//   ctor(void* this /*rcx*/, const CVec4f* bbox /*rdx*/, float cellSize /*xmm2*/)
//   bbox = { minX, minY, maxX, maxY }        (read at +0x00/+0x04/+0x08/+0x0c)
//   this+0x08 bbox copy, +0x18 cellSize, +0x20 vector<bool>, +0x40 nx, +0x44 ny
//
//   24.6 km stock : 1 m -> 0.60e9 cells (28% of INT_MAX)  -- untouched
//   57.3 km       : 2 m -> 0.82e9 cells                   -- 411 MB -> 103 MB
//   114.7 km      : 3 m -> 1.46e9 cells
//
// Below the threshold this is a no-op: stock maps keep their 1 m raster and
// behave exactly as before.
static const uintptr_t RVA_RASTER   = 0x90d410;
static const int       STEAL_RASTER = 19;

//   48 89 4C 24 08           mov   [rsp+8], rcx
//   53                       push  rbx
//   48 83 EC 30              sub   rsp, 0x30
//   48 C7 44 24 20 FE..FF    mov   qword [rsp+0x20], -2
static const uint8_t EXPECTED_RASTER[STEAL_RASTER] = {
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x30,
    0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,
};

typedef void* (__fastcall *RasterCtorFn)(void* self, const float* bbox, float cellSize);
static RasterCtorFn g_origRaster = nullptr;

// Cell budget. The hard wall is INT_MAX (2.147e9) for the resize, but every
// downstream index is int32 too, so leave real headroom rather than sitting
// just under the cliff.
static double g_cellBudget = 1.5e9;
static int    g_rasterOn   = 1;

// CVec2i is 8 bytes and trivially copyable, so it comes back packed in rax:
// x in the low 32 bits, y in the high 32. Verified at the call site
// (0x140654bc6): `mov rbx, rax` then `mov ecx, ebx` / `shr r15, 0x20`.
typedef uint64_t (__fastcall *GetNumTilesFn)(int sizeIndex, int formatIndex, void* cfg);
static GetNumTilesFn g_orig = nullptr;

// ---------------------------------------------------------------------------
// Config  (section [tpf2_bigmap] in tpf2mp.cfg)
// ---------------------------------------------------------------------------
static int g_tilesX      = 0;     // 0 = plugin does nothing
static int g_tilesY      = 0;
static int g_sizeIndex   = 6;     // which dropdown entry we take over
static int g_formatIndex = 0;     // 0 = 1:1

// ---------------------------------------------------------------------------
// A LADDER, not a single size.
//
// GetNumTilesNew is asked for every (sizeIndex 0..6, formatIndex 0..4) pair, so
// the two dropdowns the game already draws are a 7x5 grid we can answer however
// we like. Claiming one cell gives one big map and no choice; claiming a ROW
// turns the ratio dropdown into a size selector, which is a usable UI without
// adding a single widget.
//
// Config form, one key per claimed cell:
//     size<S>_format<F> = <tilesX>x<tilesY>
// e.g.  size6_format0 = 96x96      (the stock Megalomaniac 1:1, 24.6 x 24.6 km)
//       size6_format1 = 160x160    (41.0 x 41.0 km)
//       size6_format2 = 224x224    (57.3 x 57.3 km)
//
// Anything not claimed falls through to the game's own function, so every other
// preset keeps its stock behaviour exactly.
//
// The labels still say "1:1 / 1:2 / 1:3", which will not match what they now
// produce. That is a real wart and it is why the mapping is config-driven and
// documented rather than hardcoded: whether the strings can be changed cheaply
// is still being established, and until then the honest thing is to keep the
// mapping in one visible place.
struct Claim { int size, format, tx, ty; };
static const int MAX_CLAIMS = 35;      // 7 sizes x 5 formats, the whole grid
static Claim g_claims[MAX_CLAIMS];
static int   g_numClaims = 0;

// Parse "<w>x<h>", tolerating "<w>X<h>" and surrounding spaces. Returns false
// on anything it does not fully understand -- a half-parsed size would silently
// build the wrong map, which is worse than ignoring the line.
static bool ParseWxH(const char* s, int* w, int* h)
{
    if (!s || !*s) return false;
    char* end = nullptr;
    long a = strtol(s, &end, 10);
    if (end == s || a <= 0) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != 'x' && *end != 'X') return false;
    ++end;
    const char* p2 = end;
    long b = strtol(p2, &end, 10);
    if (end == p2 || b <= 0) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return false;                 // trailing junk: refuse
    *w = (int)a; *h = (int)b;
    return true;
}
// The engine cannot survive its own 224 clamp with the stock street raster.
//
// "Creating streets" builds a 1-metre occupancy raster over the whole map
// bounding box (RVA 0x90d410) and sizes it with a 32-bit signed multiply:
//
//     mov    eax, [rbx+0x44]          ; ny
//     imul   eax, dword [rbx+0x40]    ; nx * ny   <-- 32-bit
//     movsxd rdx, eax                 ; sign-extend into size_t
//     call   vector<bool>::resize
//
// At 224 tiles the map is 56000 m per side, so nx*ny = 56001^2 =
// 3,136,112,001 > INT_MAX. It wraps to -1,158,855,295, sign-extends to ~1.8e19,
// and vector<bool>::resize throws std::length_error. Nothing catches it:
// std::terminate -> abort -> SIGABRT with no message (it is an uncaught C++
// exception, not an assert, which is why the game's assert handler prints
// nothing). Confirmed by resolving the thrown object's RTTI in the minidump:
// .?AVlength_error@std@@
//
// The rule is (width_m + 1) * (height_m + 1) <= 2147483647, i.e. <= 46339 m on a
// square map. A tile is 256 m, so 180 tiles = 46080 m -> 46081^2 =
// 2,123,458,561 fits and 182 tiles = 46592 m -> 2,170,907,649 does not.
// 180 is therefore the stock ceiling -- but street_raster=1 lifts it.
//
// Non-square maps get more in one axis under the same product rule: 300 x 114
// tiles (75 x 28.5 km) is legal.
static int g_maxTiles    = 184;
static int g_logEvery    = 1;

// Even, and inside [2, max]. The engine's own override path asserts on
// `worldDimensions.x % 2 == 0`, so something downstream assumes it; we are past
// that assert here, which makes honouring the rule our job rather than the
// engine's.
static int Sanitise(int tiles)
{
    if (tiles < 2) tiles = 2;
    if (tiles > g_maxTiles) tiles = g_maxTiles;
    if (tiles & 1) tiles -= 1;
    return tiles;
}

// ---------------------------------------------------------------------------
static uint64_t __fastcall Detour(int sizeIndex, int formatIndex, void* cfg)
{
    // The ladder is checked first: an explicit size<S>_format<F> entry is a
    // deliberate statement about one cell and should beat the older single
    // tiles_x/tiles_y pair, which is kept only so existing configs keep working.
    for (int i = 0; i < g_numClaims; ++i) {
        if (g_claims[i].size == sizeIndex && g_claims[i].format == formatIndex) {
            int tx = g_claims[i].tx, ty = g_claims[i].ty;
            uint64_t packed = ((uint64_t)(uint32_t)ty << 32) | (uint32_t)tx;
            if (g_logEvery) {
                H->log("size=%d format=%d -> %d x %d tiles (%.1f x %.1f km) [ladder]",
                       sizeIndex, formatIndex, tx, ty, tx * 0.256, ty * 0.256);
            }
            return packed;
        }
    }
    if (sizeIndex == g_sizeIndex && formatIndex == g_formatIndex
        && g_tilesX > 0 && g_tilesY > 0) {
        uint64_t packed = ((uint64_t)(uint32_t)g_tilesY << 32) | (uint32_t)g_tilesX;
        if (g_logEvery) {
            H->log("size=%d format=%d -> %d x %d tiles (%.1f x %.1f km)",
                   sizeIndex, formatIndex, g_tilesX, g_tilesY,
                   g_tilesX * 0.256, g_tilesY * 0.256);
        }
        return packed;
    }
    // Not ours: the stock size, computed by the game's own code. Every preset
    // keeps working, including the ones we did not claim.
    return g_orig(sizeIndex, formatIndex, cfg);
}

// Number of cells the ctor will produce for a given cell size, using the
// engine's own rounding: n = floor(extent / cell) + 1 per axis.
static double CellsFor(float w, float h, float cell)
{
    double nx = (double)(int)(w / cell) + 1.0;
    double ny = (double)(int)(h / cell) + 1.0;
    return nx * ny;
}

static void* __fastcall RasterDetour(void* self, const float* bbox, float cellSize)
{
    float cell = cellSize;
    if (!(cell > 0.0f)) cell = 1.0f;            // also catches NaN
    float w = bbox[2] - bbox[0];
    float h = bbox[3] - bbox[1];
    if (!(w > 0.0f) || !(h > 0.0f))
        return g_origRaster(self, bbox, cellSize);   // degenerate: leave alone

    double cells = CellsFor(w, h, cell);
    if (cells > g_cellBudget) {
        float grown = cell;
        // Integer cell sizes only -- a fractional grid buys nothing and makes
        // the raster harder to reason about. 64 is a sanity stop, not a limit
        // we expect to reach (it would be a ~3000 km map).
        for (int i = 0; i < 64 && CellsFor(w, h, grown) > g_cellBudget; ++i)
            grown += 1.0f;
        H->log("street raster: %.0f x %.0f m at %.0f m/cell = %.3fe9 cells "
               "(over budget) -> %.0f m/cell = %.3fe9 cells, %.0f MB",
               w, h, cell, cells / 1e9, grown,
               CellsFor(w, h, grown) / 1e9, CellsFor(w, h, grown) / 8.0 / 1e6);
        cell = grown;
    }
    return g_origRaster(self, bbox, cell);
}

// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport)
int Tpf2mpPluginInit(const Tpf2mpHost* host, Tpf2mpPluginInfo* out)
{
    out->name    = "bigmap";
    out->version = "0.1";
    out->summary = "larger maps than the New Game menu offers (build 35924)";

    if (!host || host->abiMajor != TPF2MP_ABI_MAJOR
        || host->size < sizeof(Tpf2mpHost)) return TPF2MP_ERR_ABI;
    H = host;

    g_tilesX      = H->cfgInt ("tpf2_bigmap", "tiles_x",      0);
    g_tilesY      = H->cfgInt ("tpf2_bigmap", "tiles_y",      0);
    g_sizeIndex   = H->cfgInt ("tpf2_bigmap", "size_index",   6);
    g_formatIndex = H->cfgInt ("tpf2_bigmap", "format_index", 0);
    g_maxTiles    = H->cfgInt ("tpf2_bigmap", "max_tiles",    224);
    g_logEvery    = H->cfgBool("tpf2_bigmap", "log",          1);
    g_rasterOn    = H->cfgBool("tpf2_bigmap", "street_raster", 1);
    {
        int budgetM = H->cfgInt("tpf2_bigmap", "cell_budget_millions", 1500);
        if (budgetM > 0) g_cellBudget = (double)budgetM * 1e6;
    }

    // Build checks come FIRST: both hooks need them, and the raster hook is
    // useful even when this plugin is not choosing the map size (the game's own
    // settings.lua worldDimensionsOverride reaches sizes that overflow too).
    // Read the ladder: one key per (size, format) cell we claim.
    for (int s = 0; s < 7; ++s) {
        for (int f = 0; f < 5; ++f) {
            char key[32];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "size%d_format%d", s, f);
            const char* v = H->cfgStr("tpf2_bigmap", key, nullptr);
            if (!v || !*v) continue;
            int tx = 0, ty = 0;
            if (!ParseWxH(v, &tx, &ty)) {
                H->log("%s = '%s' is not <tiles>x<tiles> -- IGNORED", key, v);
                continue;
            }
            int sx = Sanitise(tx), sy = Sanitise(ty);
            if (sx != tx || sy != ty) {
                H->log("%s: %dx%d adjusted to %dx%d (even, 2..%d)",
                       key, tx, ty, sx, sy, g_maxTiles);
            }
            if (g_numClaims < MAX_CLAIMS) {
                g_claims[g_numClaims].size = s;
                g_claims[g_numClaims].format = f;
                g_claims[g_numClaims].tx = sx;
                g_claims[g_numClaims].ty = sy;
                ++g_numClaims;
            }
        }
    }

    if (!H->moduleBase()) {
        H->log("not running inside TransportFever2.exe -- refusing to patch");
        return TPF2MP_ERR_BUILD;
    }
    if (!H->buildOk()) {
        H->log("game build is not 35924 -- every RVA here was measured on that "
               "build, so refusing to patch (re-run the recon if the game updated)");
        return TPF2MP_ERR_BUILD;
    }

    int installed = 0;

    // ---- street occupancy raster: scale cell size with map size -----------
    if (g_rasterOn) {
        if (!H->verifyBytes(RVA_RASTER, EXPECTED_RASTER, STEAL_RASTER)) {
            H->log("raster: prologue mismatch at RVA 0x%llx -- NOT hooked; maps "
                   "over ~46.1 km will abort in Creating streets",
                   (unsigned long long)RVA_RASTER);
        } else {
            void* t = nullptr;
            if (H->installHook(H->moduleBase() + RVA_RASTER, (void*)&RasterDetour,
                               STEAL_RASTER, &t)) {
                g_origRaster = (RasterCtorFn)t;
                installed++;
                H->log("raster: hooked street occupancy ctor at %p, budget %.2fe9 "
                       "cells (1 m grid kept below that; coarsened above)",
                       (void*)(H->moduleBase() + RVA_RASTER), g_cellBudget / 1e9);
            } else {
                H->log("raster: installHook failed -- NOT hooked");
            }
        }
    } else {
        H->log("raster: disabled (street_raster=0); the int32 overflow at ~46.1 km "
               "is live -- keep makeInitialStreets=false above that size");
    }

    // ---- map size ----------------------------------------------------------
    if (g_numClaims > 0) {
        H->log("map-size ladder: %d cell(s) claimed", g_numClaims);
        for (int i = 0; i < g_numClaims; ++i) {
            H->log("   size=%d format=%d -> %d x %d tiles = %.1f x %.1f km "
                   "(heightmap %d x %d px)",
                   g_claims[i].size, g_claims[i].format,
                   g_claims[i].tx, g_claims[i].ty,
                   g_claims[i].tx * 0.256, g_claims[i].ty * 0.256,
                   g_claims[i].tx * 64 + 1, g_claims[i].ty * 64 + 1);
        }
    }
    if (g_numClaims == 0 && (g_tilesX <= 0 || g_tilesY <= 0)) {
        H->log("no map size configured -- set size<S>_format<F> = <w>x<h> "
               "(or tiles_x/tiles_y) in [tpf2_bigmap] of tpf2mp.cfg");
        return installed ? TPF2MP_OK : TPF2MP_ERR_DISABLED;
    }

    int wantX = Sanitise(g_tilesX > 0 ? g_tilesX : 2);
    int wantY = Sanitise(g_tilesY > 0 ? g_tilesY : 2);
    if (g_tilesX > 0 && g_tilesY > 0 && (wantX != g_tilesX || wantY != g_tilesY)) {
        H->log("requested %d x %d adjusted to %d x %d (must be even, 2..%d)",
               g_tilesX, g_tilesY, wantX, wantY, g_maxTiles);
    }
    g_tilesX = wantX; g_tilesY = wantY;

    // 184 tiles = 46 km is where the 1 m raster overflows int32. Past that we
    // are relying on the raster hook, so say so loudly if it is not there.
    int biggest = (g_tilesX > g_tilesY) ? g_tilesX : g_tilesY;
    for (int i = 0; i < g_numClaims; ++i) {
        if (g_claims[i].tx > biggest) biggest = g_claims[i].tx;
        if (g_claims[i].ty > biggest) biggest = g_claims[i].ty;
    }
    if (!g_origRaster && biggest > 180) {
        H->log("WARNING: %d x %d tiles exceeds the 180-tile (46.1 km) limit of the "
               "stock 1 m street raster and the raster hook is NOT active. "
               "Generation will abort unless makeInitialStreets=false in "
               "res/config/base_config.lua",
               biggest, biggest);
    }

    if (!H->verifyBytes(RVA_GETNUMTILES, EXPECTED, STEAL)) {
        H->log("prologue mismatch at RVA 0x%llx -- refusing to patch",
               (unsigned long long)RVA_GETNUMTILES);
        return installed ? TPF2MP_OK : TPF2MP_ERR_BUILD;
    }

    void* tramp = nullptr;
    uintptr_t target = H->moduleBase() + RVA_GETNUMTILES;
    if (!H->installHook(target, (void*)&Detour, STEAL, &tramp)) {
        H->log("installHook failed at %p", (void*)target);
        return installed ? TPF2MP_OK : TPF2MP_ERR_FAILED;
    }
    g_orig = (GetNumTilesFn)tramp;
    installed++;

    H->log("hooked GetNumTilesNew at %p (tramp %p)", (void*)target, tramp);
    H->log("size index %d, ratio index %d -> %d x %d tiles = %.1f x %.1f km "
           "(heightmap %d x %d px)",
           g_sizeIndex, g_formatIndex, g_tilesX, g_tilesY,
           g_tilesX * 0.256, g_tilesY * 0.256,
           g_tilesX * 64 + 1, g_tilesY * 64 + 1);
    return TPF2MP_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
