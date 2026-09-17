// Steam 35924: the per-tile work blocks of the terrain alignment pass go
// through the terrain pager instead of the heap.
//
// MEASURED 2026-09-17 (ETW VirtualAllocation trace of a 103,680-tile save
// load that ended in the game's "Out of memory" assert, tools/re/etl_alloc.py):
// at the peak the process had 31.6 GiB live, of which 19.2 GiB were 99,230
// blocks of 132,098 bytes, one per tile, every one of them alive at once.
// The stack is operator new <- std::vector<uint16_t>(n) (0x310230, the
// aligned value-initialising constructor) <- terrain_util::GetBlock
// (0x3c4a20, return address 0x3c4ba7) <- terrain_util::BaseGetHeightmapRefined
// <- ecs::TerrainAlignmentSystem::UpdateSubterrains. The load computes a
// 257x257 block for every tile, keeps all of them until the publication
// pass copies each into its tile's height cache, and frees them afterwards.
//
// Here the constructor is detoured: when it is called from GetBlock for a
// block of exactly one tile (66,049 samples) and the pager is live, the
// vector gets a pager slot (a lazy zero one when that is on: no section until
// the first write). The blocks then compress and evict under commit pressure
// like tiles do, and the fill/publish sequence faults them back as needed.
// Their release comes through the CRT: the aligned delete reads the raw
// pointer at [-8] (the pager stores the slot base there, as MSVC does) and
// calls free. free is imported (api-ms-win-crt-heap-l1-1-0!free), so its IAT
// slot (0x2f0b5b8) is pointed at a detour that returns arena pointers to the
// pager and passes everything else on unchanged.
#pragma once
struct BlockVector { uint16_t *first, *last, *end; };
using BlockCtorFn = void(__fastcall*)(BlockVector*, size_t);
using FreeFn = void(__cdecl*)(void*);
static BlockCtorFn g_originalBlockCtor = nullptr;
static FreeFn g_originalFree = nullptr;
static uintptr_t g_blockBase = 0;
static const uintptr_t kBlockCtorRva = 0x310230, kBlockCtorReturn = 0x3c4ba7, kFreeIatRva = 0x2f0b5b8;
static const uint8_t kBlockCtorBytes[19] = {
    0x48, 0x89, 0x4c, 0x24, 0x08,                    // mov [rsp+8], rcx
    0x57,                                            // push rdi
    0x48, 0x83, 0xec, 0x30,                          // sub rsp, 0x30
    0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff   // mov qword [rsp+0x20], -2
};
static void __fastcall BlockCtorDetour(BlockVector* v, size_t n) {
    auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    InterlockedIncrement64(&g_blockCalls);
    if (n == TerrainPager::Samples) {
        InterlockedIncrement64(&g_blockSized);
        // Record up to eight distinct callers of one-tile-sized constructions.
        LONG64 rva = LONG64(ret - g_blockBase);
        for (int k = 0; k < 8; ++k) {
            LONG64 seen = InterlockedCompareExchange64(&g_blockCallers[k], rva, 0);
            if (seen == 0) { if (H) H->log("terrain blocks: one-tile block constructed from exe+%llx", (unsigned long long)rva); break; }
            if (seen == rva) break;
        }
    }
    if (n == TerrainPager::Samples && InterlockedCompareExchange(&g_terrainCompressActive, 0, 0) &&
        ret == g_blockBase + kBlockCtorReturn) {
        if (auto p = TerrainPager::Allocate()) {
            *v = {p, p + n, p + n};
            if (InterlockedIncrement64(&g_blockAllocations) == 1) H->log("terrain blocks: first alignment block routed to the pager");
            return;
        }
    }
    g_originalBlockCtor(v, n);
}
static void __cdecl FreeDetour(void* p) {
    if (p && TerrainPager::Contains(p)) {
        if (TerrainPager::ReleaseAny(p)) { InterlockedIncrement64(&g_blockReleases); return; }
        // An arena address that is not a live slot: never hand it to the CRT.
        InterlockedIncrement64(&g_blockStray);
        return;
    }
    g_originalFree(p);
}
static bool InstallTerrainBlocks() {
    if (!g_terrainBlocks || g_gog) return false;
    if (!InterlockedCompareExchange(&g_terrainCompressActive, 0, 0)) {
        H->log("terrain blocks: needs terrain_cache_compress=1; OFF"); return false;
    }
    if (!H->verifyBytes(kBlockCtorRva, kBlockCtorBytes, sizeof kBlockCtorBytes)) {
        H->log("terrain blocks: Steam byte mismatch at the block vector constructor; OFF"); return false;
    }
    g_blockBase = H->moduleBase();
    // The IAT slot must hold the CRT's free before it is replaced.
    auto slot = reinterpret_cast<void**>(g_blockBase + kFreeIatRva);
    auto ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto crtFree = ucrt ? reinterpret_cast<void*>(GetProcAddress(ucrt, "free")) : nullptr;
    if (!crtFree || *slot != crtFree) {
        H->log("terrain blocks: free import slot does not hold ucrtbase!free (%p vs %p); OFF", *slot, crtFree); return false;
    }
    g_originalFree = reinterpret_cast<FreeFn>(crtFree);
    void* detour = reinterpret_cast<void*>(FreeDetour);
    uint8_t bytes[8]; memcpy(bytes, &detour, 8);
    // free first: once the constructor hands out arena blocks, every release
    // must already be routed to the pager.
    if (!H->patchBytes(kFreeIatRva, bytes, 8)) { H->log("terrain blocks: free import patch failed; OFF"); return false; }
    if (!H->installHook(g_blockBase + kBlockCtorRva, reinterpret_cast<void*>(BlockCtorDetour), sizeof kBlockCtorBytes,
                        reinterpret_cast<void**>(&g_originalBlockCtor))) {
        uint8_t back[8]; memcpy(back, &crtFree, 8); H->patchBytes(kFreeIatRva, back, 8);
        H->log("terrain blocks: constructor hook failed; free import restored; OFF"); return false;
    }
    H->log("terrain blocks: alignment work blocks (257x257 from terrain_util::GetBlock) go through the terrain pager; free import routed");
    return true;
}
extern "C" __declspec(dllexport) void BigmapTestBlockCtor(BlockVector* v, size_t n, BlockCtorFn original) { g_originalBlockCtor = original; BlockCtorDetour(v, n); }
extern "C" __declspec(dllexport) void BigmapTestFree(void* p, FreeFn original) { g_originalFree = original; FreeDetour(p); }
extern "C" __declspec(dllexport) void BigmapTestBlockStats(uint64_t* out) { out[0] = g_blockAllocations; out[1] = g_blockReleases; out[2] = g_blockStray; }
