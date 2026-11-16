// gog_rva.h -- run Steam 35924 modules on the GOG 2024-12-12 executable.
//
// The two builds carry the same code: every function compared so far is
// identical byte for byte once RIP-relative displacements and call targets are
// masked, and each region of the image is shifted by one constant. This table
// lists those regions. A module written against Steam RVAs is installed with
// the translating host below (RunWithSteamSites): its verifyBytes, patchBytes
// and installHook calls are moved to the GOG addresses, and every byte check it
// makes still runs -- so a region this table gets wrong refuses cleanly instead
// of patching the wrong place. An address outside every region translates to
// 0, which no verifyBytes accepts.
//
// How each region was established (tools/gog_map_sites.py, tools/gog_align_code.py):
//   * code 0x157390..0x662ce9, +0x220: 30 verified sites, and the full bodies of
//     InternBicubicRefine (0x3ac6d4, 2172 bytes), CalculateHeightMod (0x3b3485,
//     2132 bytes), PredHeightModRasterizable ctor (0x3b0190), the u16 _Resize
//     (0x3af850) and CMat4f product (0x2fadc0) compared instruction by instruction;
//   * code 0x853d30..0xaadc68, +0xf0: 6 verified sites;
//   * the assert handler 0x221adf0 (+0x2ae0) and the two alignment rasterisers
//     0x2375420 / 0x23754b0 (+0x2b30): the call targets of the aligned bodies;
//   * .rdata 0x2f1e988..0x2fb6658, -0x13ff0: every RIP-relative operand of the
//     aligned bodies (20 constants, vtables and strings) and the material-index
//     dither table, which the GOG MaterialIndex loop loads at 0x2f73d30.
// Do not widen a region past its evidence: add a measured one instead.
#pragma once

struct GogRegion { uintptr_t lo, hi; intptr_t delta; };
static const GogRegion kGogRegions[] = {
    { 0x157390,  0x663000,  +0x220 },
    { 0x853d30,  0xaae000,  +0x0f0 },
    { 0x221adf0, 0x221adf1, +0x2ae0 },
    { 0x2375420, 0x2375421, +0x2b30 },
    { 0x23754b0, 0x23754b1, +0x2b30 },
    { 0x2f1e988, 0x2fb7000, -0x13ff0 },
};

// The GOG RVA of a Steam RVA, or 0 when no measured region covers it.
static uintptr_t GogRva(uintptr_t steamRva)
{
    for (const auto& r : kGogRegions)
        if (steamRva >= r.lo && steamRva < r.hi) return uintptr_t(intptr_t(steamRva) + r.delta);
    return 0;
}

// The running address of a Steam RVA on whichever build is running.
static uintptr_t GameRva(uintptr_t steamRva) { return g_gog ? GogRva(steamRva) : steamRva; }

static const Tpf2mpHost* g_steamSitesRealHost = nullptr;
static Tpf2mpHost        g_steamSitesHost;

static int GogVerifyBytes(uintptr_t rva, const uint8_t* expected, uint32_t len)
{
    const uintptr_t g = GogRva(rva);
    return g ? g_steamSitesRealHost->verifyBytes(g, expected, len) : 0;
}
static int GogPatchBytes(uintptr_t rva, const uint8_t* bytes, uint32_t len)
{
    const uintptr_t g = GogRva(rva);
    return g ? g_steamSitesRealHost->patchBytes(g, bytes, len) : 0;
}
static int GogInstallHook(uintptr_t target, void* detour, int stealBytes, void** trampolineOut)
{
    const uintptr_t base = g_steamSitesRealHost->moduleBase();
    if (target < base) return 0;
    const uintptr_t g = GogRva(target - base);
    return g ? g_steamSitesRealHost->installHook(base + g, detour, stealBytes, trampolineOut) : 0;
}

// Run a module installer written against Steam RVAs. On Steam it runs as is;
// on GOG, H is swapped for a host that translates every address it is given.
// Installers run once, on the loader thread, before the game starts.
template <class F> static bool RunWithSteamSites(F install)
{
    if (!g_gog) return install();
    const Tpf2mpHost* real = H;
    g_steamSitesRealHost = real;
    g_steamSitesHost = *real;
    g_steamSitesHost.verifyBytes = &GogVerifyBytes;
    g_steamSitesHost.patchBytes  = &GogPatchBytes;
    g_steamSitesHost.installHook = &GogInstallHook;
    H = &g_steamSitesHost;
    const bool ok = install();
    H = real;
    return ok;
}

#include "gog_fixups.h"

extern "C" __declspec(dllexport)
uintptr_t BigmapTestGogRva(uintptr_t steamRva) { return GogRva(steamRva); }
