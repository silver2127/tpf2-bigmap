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
// so ONE TILE IS 250 m (64 px at 3.90625 m/px). Confirmed against real saves:
// the .sav header (after zstd) carries numTilesX/numTilesY at +0x10/+0x14, and
// five different maps all land exactly on 250 m/tile -- e.g. Megalomaniac 1:4
// reads (48, 192) = 12 x 48 km.
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
//   224 tiles = 56 km   the stock clamp; reachable with no patching at all
//   256 tiles = 64 km   heightmap 16385 px = 2^14+1, the largest dimension the
//                       heightmap-import documentation quotes -- the most likely
//                       real wall, and the interesting data point
//   448 tiles = 112 km  heightmap 28673 px; int32 pixel arithmetic still holds
//                       (28673^2 = 822M, well under 2^31)
// Nothing above 224 has been shown to work. This plugin is the instrument for
// finding out, not a claim that it does.
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
static int g_maxTiles    = 224;   // the stock clamp; raise deliberately
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
    if (sizeIndex == g_sizeIndex && formatIndex == g_formatIndex
        && g_tilesX > 0 && g_tilesY > 0) {
        uint64_t packed = ((uint64_t)(uint32_t)g_tilesY << 32) | (uint32_t)g_tilesX;
        if (g_logEvery) {
            H->log("size=%d format=%d -> %d x %d tiles (%.1f x %.1f km)",
                   sizeIndex, formatIndex, g_tilesX, g_tilesY,
                   g_tilesX * 0.25, g_tilesY * 0.25);
        }
        return packed;
    }
    // Not ours: the stock size, computed by the game's own code. Every preset
    // keeps working, including the one we did not claim.
    return g_orig(sizeIndex, formatIndex, cfg);
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

    if (g_tilesX <= 0 || g_tilesY <= 0) {
        H->log("tiles_x/tiles_y not set -- nothing to do "
               "(set them in [tpf2_bigmap] of tpf2mp.cfg)");
        return TPF2MP_ERR_DISABLED;
    }

    int wantX = Sanitise(g_tilesX), wantY = Sanitise(g_tilesY);
    if (wantX != g_tilesX || wantY != g_tilesY) {
        H->log("requested %d x %d adjusted to %d x %d (must be even, 2..%d)",
               g_tilesX, g_tilesY, wantX, wantY, g_maxTiles);
    }
    g_tilesX = wantX; g_tilesY = wantY;

    if (!H->moduleBase()) {
        H->log("not running inside TransportFever2.exe -- refusing to patch");
        return TPF2MP_ERR_BUILD;
    }
    if (!H->buildOk()) {
        H->log("game build is not 35924 -- every RVA here was measured on that "
               "build, so refusing to patch (re-run the recon if the game updated)");
        return TPF2MP_ERR_BUILD;
    }
    if (!H->verifyBytes(RVA_GETNUMTILES, EXPECTED, STEAL)) {
        H->log("prologue mismatch at RVA 0x%llx -- refusing to patch",
               (unsigned long long)RVA_GETNUMTILES);
        return TPF2MP_ERR_BUILD;
    }

    void* tramp = nullptr;
    uintptr_t target = H->moduleBase() + RVA_GETNUMTILES;
    if (!H->installHook(target, (void*)&Detour, STEAL, &tramp)) {
        H->log("installHook failed at %p", (void*)target);
        return TPF2MP_ERR_FAILED;
    }
    g_orig = (GetNumTilesFn)tramp;

    H->log("hooked GetNumTilesNew at %p (tramp %p)", (void*)target, tramp);
    H->log("size index %d, ratio index %d -> %d x %d tiles = %.1f x %.1f km "
           "(heightmap %d x %d px)",
           g_sizeIndex, g_formatIndex, g_tilesX, g_tilesY,
           g_tilesX * 0.25, g_tilesY * 0.25,
           g_tilesX * 64 + 1, g_tilesY * 64 + 1);
    return TPF2MP_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
