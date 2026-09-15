// Steam 35924 integration. Only CTerrain's shared 257x257 uint16 vectors are
// eligible. Existing vectors, source heightmaps and rendering buffers stay on
// their stock allocators. Enable before loading; never retrofit live pointers.
#pragma once
#include "terrain_pager.h"
#include "terrain_warmup.h"
static int g_terrainCompress=0, g_terrainHotMB=1024;
static int g_terrainWarmMB=4096;
static volatile LONG g_terrainCompressActive=0;
struct TerrainOwnedVector {uint16_t *first,*last,*end;};
using TerrainResizeFn=void(__fastcall*)(TerrainOwnedVector*,size_t);
using TerrainCopyFn=TerrainOwnedVector*(__fastcall*)(TerrainOwnedVector*,const TerrainOwnedVector*);
using TerrainDestroyFn=void(__fastcall*)(void*);
static TerrainResizeFn g_originalTerrainResize;
static TerrainCopyFn g_originalTerrainCopy;
static TerrainDestroyFn g_originalTerrainDestroy;
static uintptr_t g_terrainCompressionBase;
static void ResizeTerrainOwned(TerrainOwnedVector* v,size_t n,bool eligible) {
    using namespace TerrainPager;
    if(Contains(v->first)) {
        size_t oldSize=size_t(v->last-v->first);
        if(n<=Samples){v->last=v->first+n; if(n>oldSize)memset(v->first+oldSize,0,(n-oldSize)*2);return;}
        // A future engine path grows this vector: migrate to the stock heap
        // before its normal reallocation/free code can see our backing memory.
        TerrainOwnedVector replacement{};
        g_originalTerrainResize(&replacement,n);
        memcpy(replacement.first,v->first,oldSize*2);
        Release(v->first);*v=replacement;return;
    }
    if(eligible && !v->first && !v->last && !v->end && n==Samples) {
        if(auto p=Allocate()){*v={p,p+n,p+n};return;}
    }
    g_originalTerrainResize(v,n);
}
static void __fastcall TerrainCompressedResize(TerrainOwnedVector* v,size_t n) {
    bool eligible=InterlockedCompareExchange(&g_terrainCompressActive,0,0) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress())==g_terrainCompressionBase+0x33ccaa;
    ResizeTerrainOwned(v,n,eligible);
}
static TerrainOwnedVector* CopyTerrainOwned(TerrainOwnedVector* dst,const TerrainOwnedVector* src,bool eligible) {
    if(eligible && src->first && uintptr_t(src->last)-uintptr_t(src->first)==TerrainPager::Bytes) {
        if(auto p=TerrainPager::Clone(src->first)) {
            // Immutable compressed bytes can be shared between independently
            // owned vectors. First write restores a private backing section.
            *dst={p,p+TerrainPager::Samples,p+TerrainPager::Samples};return dst;
        }
        if(auto p=TerrainPager::Allocate()) {
            // COW source may be compressed; the ordinary memcpy faults back in
            // without changing source bytes or shared ownership.
            memcpy(p,src->first,TerrainPager::Bytes);
            *dst={p,p+TerrainPager::Samples,p+TerrainPager::Samples};return dst;
        }
    }
    return g_originalTerrainCopy(dst,src);
}
static TerrainOwnedVector* __fastcall TerrainCompressedCopy(TerrainOwnedVector* dst,const TerrainOwnedVector* src) {
    bool eligible=InterlockedCompareExchange(&g_terrainCompressActive,0,0) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress())==g_terrainCompressionBase+0x33dd91;
    return CopyTerrainOwned(dst,src,eligible);
}
static void __fastcall TerrainCompressedDestroy(void* control) {
    auto v=reinterpret_cast<TerrainOwnedVector*>(static_cast<uint8_t*>(control)+0x10);
    if(TerrainPager::Release(v->first)){*v={};return;}
    g_originalTerrainDestroy(control);
}
static int TerrainBudgetMB(int hot,int warm,bool busy,bool bulk,uint64_t available) {
    // Memory pressure overrides the warmup allowance. Available RAM is sampled
    // outside the pager lock; this is a conservative policy, not an allocation.
    return (busy||bulk) && available>=8ull*1024*1024*1024 && warm>hot ? warm : hot;
}
static DWORD WINAPI TerrainCompressionWorker(void*) {
    uint64_t lastLog=GetTickCount64(),lastPolicy=0;int effectiveMB=g_terrainHotMB;
    TerrainWarmup warmup;
    using UiTickFn=uint64_t(*)();UiTickFn uiTick=nullptr;
    for(;;) {
        Sleep(25);
        if(!InterlockedCompareExchange(&g_terrainCompressActive,0,0))continue;
        auto now=GetTickCount64();
        if(now-lastPolicy>=1000) {
            lastPolicy=now;auto s=TerrainPager::Snapshot();MEMORYSTATUSEX m{};m.dwLength=sizeof m;
            uint64_t available=GlobalMemoryStatusEx(&m)?m.ullAvailPhys:0;
            bool busy=InterlockedCompareExchange(&g_worldEntryActive,0,0)!=0;
            bool bulk=s.lastBulkAllocation && now-s.lastBulkAllocation<15000;
            if(!uiTick) {
                if(auto menu=GetModuleHandleW(L"tpf2_menu.dll"))
                    uiTick=reinterpret_cast<UiTickFn>(GetProcAddress(menu,"Tpf2mpLastGameUiTick"));
            }
            bool loading=warmup.Update(now,busy,s.lastBulkAllocation,uiTick?uiTick():0,uiTick!=nullptr);
            int next=TerrainBudgetMB(g_terrainHotMB,g_terrainWarmMB,busy,bulk||loading,available);
            TerrainPager::SetBudget(size_t(next)*1024*1024);
            if(next!=effectiveMB)H->log("terrain compression: resident target %d -> %d MiB (generation=%d bulk_allocation=%d loading_tail=%d ui_signal=%d)",effectiveMB,next,int(busy),int(bulk),int(loading),int(uiTick!=nullptr));
            effectiveMB=next;
        }
        TerrainPager::Tick();
        if(now-lastLog>=30000) {
            lastLog=now;auto s=TerrainPager::Snapshot();
            if(s.live)H->log("terrain compression: live=%llu resident=%llu backing=%.1f MiB compressed=%.1f MiB encoded_commit=%.1f MiB faults=%llu evictions=%llu failures=%llu encodes=%llu reused=%llu writes=%llu shared_clones=%llu",
                s.live,s.resident,double(s.resident*TerrainPager::SlotBytes)/(1024*1024),
                double(s.compressedBytes)/(1024*1024),double(s.compressedCommit)/(1024*1024),s.faults,s.evictions,s.failures,
                s.encodes,s.reusedEvictions,s.writeFaults,s.sharedClones);
        }
    }
}
static const uint8_t kTerrainResizeBytes[]={0x48,0x89,0x4c,0x24,0x08,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30};
static const uint8_t kTerrainCopyBytes[]={0x48,0x89,0x4c,0x24,0x08,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30};
static const uint8_t kTerrainDestroyBytes[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x49,0x10,0x48,0x85,0xc9};
static const uint8_t kTerrainResizeCall[]={0xe8,0xa6,0x8f,0xe9,0xff};
static const uint8_t kTerrainCopyCall[]={0xe8,0x3f,0x10,0xea,0xff};
static bool InstallTerrainCompression() {
    if(!g_terrainCompress)return false;
    if(g_gog || g_terrainCompress!=1 || g_terrainCacheSpacing!=1 || g_terrainHotMB<128 || g_terrainHotMB>8192 || g_terrainWarmMB<0 || g_terrainWarmMB>8192) {
        H->log("terrain compression: requires Steam, spacing=1, compress=1, hot_mb=128..8192, warm_mb=0..8192; OFF");return false;
    }
    if(!H->verifyBytes(0x1d5c50,kTerrainResizeBytes,sizeof kTerrainResizeBytes) ||
       !H->verifyBytes(0x1dedd0,kTerrainCopyBytes,sizeof kTerrainCopyBytes) ||
       !H->verifyBytes(0x33de30,kTerrainDestroyBytes,sizeof kTerrainDestroyBytes) ||
       !H->verifyBytes(0x33cca5,kTerrainResizeCall,sizeof kTerrainResizeCall) ||
       !H->verifyBytes(0x33dd8c,kTerrainCopyCall,sizeof kTerrainCopyCall)) {
        H->log("terrain compression: Steam byte mismatch; OFF");return false;
    }
    if(!TerrainPager::Init(size_t(g_terrainHotMB)*1024*1024)) {
        H->log("terrain compression: placeholder/handler initialization failed; OFF");return false;
    }
    g_terrainCompressionBase=H->moduleBase();
    // Destruction first; allocation remains disabled until EVERY hook and the
    // worker succeed. Partial installation cannot create managed allocations.
    if(!H->installHook(H->moduleBase()+0x33de30,reinterpret_cast<void*>(TerrainCompressedDestroy),sizeof kTerrainDestroyBytes,reinterpret_cast<void**>(&g_originalTerrainDestroy)) ||
       !H->installHook(H->moduleBase()+0x1dedd0,reinterpret_cast<void*>(TerrainCompressedCopy),sizeof kTerrainCopyBytes,reinterpret_cast<void**>(&g_originalTerrainCopy)) ||
       !H->installHook(H->moduleBase()+0x1d5c50,reinterpret_cast<void*>(TerrainCompressedResize),sizeof kTerrainResizeBytes,reinterpret_cast<void**>(&g_originalTerrainResize))) {
        H->log("terrain compression: hook failed; allocation remains OFF");return false;
    }
    HANDLE worker=CreateThread(nullptr,0,TerrainCompressionWorker,nullptr,0,nullptr);
    if(!worker){H->log("terrain compression: worker failed; allocation remains OFF");return false;}
    SetThreadPriority(worker,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(worker);
    InterlockedExchange(&g_terrainCompressActive,1);
    H->log("terrain compression: lossless 1 m cache enabled, %d MiB resident target; restart to disable",g_terrainHotMB);
    return true;
}

extern "C" __declspec(dllexport) int BigmapTestCompressionInit(){return TerrainPager::Init(1024*1024);}
extern "C" __declspec(dllexport) void BigmapTestCompressionResize(TerrainOwnedVector* v,size_t n,int eligible,TerrainResizeFn fn){g_originalTerrainResize=fn;ResizeTerrainOwned(v,n,eligible!=0);}
extern "C" __declspec(dllexport) void* BigmapTestCompressionCopy(TerrainOwnedVector* dst,const TerrainOwnedVector* src,int eligible,TerrainCopyFn fn){g_originalTerrainCopy=fn;return CopyTerrainOwned(dst,src,eligible!=0);}
extern "C" __declspec(dllexport) void BigmapTestCompressionDestroy(void* control,TerrainDestroyFn fn){g_originalTerrainDestroy=fn;TerrainCompressedDestroy(control);}
extern "C" __declspec(dllexport) int BigmapTestCompressionEvict(void* p){return TerrainPager::Contains(p)&&TerrainPager::Evict(TerrainPager::Index(p),true);}
extern "C" __declspec(dllexport) int BigmapTestTerrainBudget(int hot,int warm,int busy,int bulk,uint64_t available){return TerrainBudgetMB(hot,warm,busy!=0,bulk!=0,available);}
extern "C" __declspec(dllexport) int BigmapTestWarmUpdate(TerrainWarmup* state,uint64_t now,int busy,uint64_t bulk,uint64_t ui,int signal){return state->Update(now,busy!=0,bulk,ui,signal!=0);}
extern "C" __declspec(dllexport) int BigmapTestInstallCompression(const Tpf2mpHost* host,int gog,int spacing,int enabled,int hotMB) {
    H=host;g_gog=gog!=0;g_terrainCacheSpacing=spacing;g_terrainCompress=enabled;g_terrainHotMB=hotMB;
    return InstallTerrainCompression();
}
