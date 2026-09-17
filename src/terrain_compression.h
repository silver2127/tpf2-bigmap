// Steam 35924 integration. Only CTerrain's shared 257x257 uint16 vectors are
// eligible. Existing vectors, source heightmaps and rendering buffers stay on
// their stock allocators. Enable before loading; never retrofit live pointers.
#pragma once
#include "terrain_pager.h"
#include "terrain_warmup.h"
static int g_terrainCompress=0, g_terrainHotMB=1024;
static int g_terrainWarmMB=4096;
// Measurement stage of docs/terrain-cow-sharing.md: share the section between
// the two CTerrain versions instead of copying, privatizing on first write.
static int g_terrainCowShare=0;
// Is the COW copy hook even reached during a load? The first measured run shared
// nothing, and `1dedd0` only runs when the detach at `33dd20` finds refs > 1.
static volatile LONG64 g_cowCopyCalls=0, g_cowCopyUnmanaged=0;
// Content-dedup probe (measurement only): hash every live tile and log how many
// are byte-identical to another. Every 10 s while loading, every 2 min otherwise.
static int g_terrainDedupProbe=0;
// Content dedup: an eviction whose bytes match a stored blob shares it instead
// of encoding (see pager_impl.inl). Measured need: the two CTerrain versions of
// a save load are byte-identical tile for tile.
static int g_terrainDedup=0;
static void ProbeYield(){TerrainPager::Tick();}
static void LogDedupProbe() {
    TerrainPager::ProbeResult r{};
    if(!TerrainPager::Probe(&r,ProbeYield)){H->log("terrain dedup probe: table allocation failed");return;}
    H->log("terrain dedup probe: live=%llu hashed=%llu (resident=%llu cold=%llu) skipped=%llu distinct=%llu duplicates=%llu zero_tiles=%llu pairs=%llu largest_group=%llu low_half=%llu ms=%llu",
           r.live,r.hashedResident+r.hashedPacked,r.hashedResident,r.hashedPacked,r.skipped,r.distinct,r.duplicated,r.zero,r.pairs,r.largestGroup,r.lowHalf,r.ms);
}
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
        if(g_terrainCowShare) {
            InterlockedIncrement64(&g_cowCopyCalls);
            if(!TerrainPager::Contains(src->first))InterlockedIncrement64(&g_cowCopyUnmanaged);
            // Map the source's section a second time rather than copying 132 KiB.
            // Both views become read-only; whichever version is written first
            // takes a private copy. Falls through to the eager paths on refusal.
            if(auto p=TerrainPager::Share(src->first)) {
                *dst={p,p+TerrainPager::Samples,p+TerrainPager::Samples};return dst;
            }
        }
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
// Budgets sized from installed RAM when the cfg value asks for it (hot 0,
// warm -1). The tuned 3072/4096 MiB pair suits a 94 GiB machine; the same
// fractions give ~1 GiB/2.6 GiB on 32 GiB and ~546/1365 MiB on 16 GiB, where
// fixed values would push the game into paging.
static int AutoBudgetMB(uint64_t totalBytes,int divisor,int lo,int hi) {
    uint64_t mb=(totalBytes>>20)/uint64_t(divisor);
    if(mb<uint64_t(lo))mb=uint64_t(lo);
    if(mb>uint64_t(hi))mb=uint64_t(hi);
    return int(mb);
}
static void AutoTerrainBudgets(uint64_t totalBytes,int* hot,int* warm) {
    int h=AutoBudgetMB(totalBytes,30,256,4096);
    *hot=h;*warm=AutoBudgetMB(totalBytes,12,h,8192);
}
static uint64_t InstalledPhysicalBytes() {
    MEMORYSTATUSEX m{};m.dwLength=sizeof m;
    return GlobalMemoryStatusEx(&m)?m.ullTotalPhys:0;
}
static int TerrainBudgetMB(int hot,int warm,bool busy,bool bulk,uint64_t available,uint64_t liveMB=0) {
    // Memory pressure overrides the warmup allowance. Available RAM is sampled
    // outside the pager lock; this is a conservative policy, not an allocation.
    constexpr uint64_t GiB=1024ull*1024*1024;
    if(!((busy||bulk) && available>=4*GiB && warm>hot))return hot;
    // While loading, keep every live allocation resident when RAM allows. A
    // 256x256 save reload with a 4 GiB allowance decoded ~290k terrain tiles on
    // the loading threads (the loader re-reads what was just evicted). The
    // target never exceeds available RAM minus 12 GiB, so it shrinks by itself
    // as memory fills, and never drops below the configured warm allowance.
    // Keep a quarter of what is free, at least 2 GiB, for everything else. A
    // flat 12 GiB reserve left 16 and 32 GiB machines with no allowance at all.
    uint64_t reserve=available/4>2*GiB?available/4:2*GiB;
    uint64_t capMB=available>reserve?(available-reserve)>>20:0;
    uint64_t want=liveMB>uint64_t(warm)?liveMB:uint64_t(warm);
    uint64_t floor=capMB>uint64_t(warm)?capMB:uint64_t(warm);
    uint64_t target=want<floor?want:floor;
    if(target>65536)target=65536;
    return int(target>uint64_t(hot)?target:uint64_t(hot));
}
// Steady state (not loading, commit not tight), once per second. `next` is the
// policy's own target for this second (the hot budget), `prev` the target in
// force. MEASURED 2026-09-17 on a 103,680-tile world: when the load ended the
// target snapped from 13.4 GiB to the 3.2 GiB hot budget, the pager evicted
// 67,000 tiles inside a minute, and the engine, which keeps ~36,000 tiles
// (4.6 GiB) in use on that map, faulted 5,900 evicted tiles per second back in
// through a decode each: visible freezes. Three rules replace the snap:
// - Ramp: the target drops by at most 1/16 of itself (>= 64 MiB) per second.
// - Stutter feedback: `decodesPerSec` is the number of cold restores in the
//   last second. At >= 300 the engine is re-reading what was just evicted:
//   grow by 1/8 (>= 128 MiB). At >= 100 hold. Below that, drift down.
// - Ceiling: the hot budget plus half of what is free above a quarter reserve
//   (`available` = min(free RAM, free commit)), never above 65536 MiB.
// The result never goes below `next`, so the configured budget stays a floor.
static int TerrainBudgetSteady(int prev,int next,int hot,uint64_t decodesPerSec,uint64_t available) {
    constexpr uint64_t GiB=1024ull*1024*1024;
    uint64_t reserve=available/4>2*GiB?available/4:2*GiB;
    uint64_t spareMB=available>reserve?(available-reserve)>>21:0;   // half of the spare
    uint64_t ceil=uint64_t(hot)+spareMB;if(ceil>65536)ceil=65536;
    int ceiling=int(ceil);
    int target=next;
    if(decodesPerSec>=300){int grow=prev/8>128?prev/8:128;target=prev+grow;}
    else if(decodesPerSec>=100){target=prev>next?prev:next;}
    else if(next<prev){int step=prev/16>64?prev/16:64;target=prev-step>next?prev-step:next;}
    if(target>ceiling)target=ceiling;
    if(target<next)target=next;
    return target;
}
// Extra eviction threads: encoding runs outside the pool lock, so several
// threads keep up with bulk allocation during loading. Policy and logging stay
// on the main worker.
static unsigned PagerHelperThreads() {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    return info.dwNumberOfProcessors>=16?3:info.dwNumberOfProcessors>=8?1:0;
}
static DWORD WINAPI TerrainEvictionHelper(void*) {
    for(;;){Sleep(25);if(InterlockedCompareExchange(&g_terrainCompressActive,0,0))TerrainPager::Tick();}
}
static DWORD WINAPI TerrainCompressionWorker(void*) {
    uint64_t lastLog=GetTickCount64(),lastPolicy=0;int effectiveMB=g_terrainHotMB;
    uint64_t lastProbe=0;bool probeFast=false;
    uint64_t lastDecodes=0,lastStutterLog=0;
    TerrainWarmup warmup;
    using UiTickFn=uint64_t(*)();UiTickFn uiTick=nullptr;
    for(;;) {
        Sleep(25);
        if(!InterlockedCompareExchange(&g_terrainCompressActive,0,0))continue;
        auto now=GetTickCount64();
        if(now-lastPolicy>=1000) {
            lastPolicy=now;auto s=TerrainPager::Snapshot();MEMORYSTATUSEX m{};m.dwLength=sizeof m;
            // Pager sections are page-file-backed: they count against the system
            // commit limit (RAM + page file) exactly like private memory. Use the
            // smaller of free RAM and free commit, and back off hard when commit is
            // nearly exhausted (a 512x512 desert preview hit std::bad_alloc at the
            // 114.6 GB commit limit while tiles were held uncompressed).
            bool haveStatus=GlobalMemoryStatusEx(&m)!=0;
            uint64_t available=haveStatus?(m.ullAvailPhys<m.ullAvailPageFile?m.ullAvailPhys:m.ullAvailPageFile):0;
            bool commitTight=haveStatus && m.ullAvailPageFile<6ull*1024*1024*1024;
            bool busy=InterlockedCompareExchange(&g_worldEntryActive,0,0)!=0;
            bool bulk=s.lastBulkAllocation && now-s.lastBulkAllocation<15000;
            if(!uiTick) {
                if(auto menu=GetModuleHandleW(L"tpf2_menu.dll"))
                    uiTick=reinterpret_cast<UiTickFn>(GetProcAddress(menu,"Tpf2mpLastGameUiTick"));
            }
            bool loading=warmup.Update(now,busy,s.lastBulkAllocation,uiTick?uiTick():0,uiTick!=nullptr);
            // Growth with the live size while loading, capped by the smaller of free
            // RAM and free commit (above). Measured on the same 256x256 save: with a
            // fixed 4 GiB allowance the loading thread spent 69-71% of two profile
            // windows restoring tiles (85 s); with growth 41-53% (73 s). The earlier
            // RAM-only cap hit std::bad_alloc at the commit limit on a 512x512 preview.
            int next=TerrainBudgetMB(g_terrainHotMB,g_terrainWarmMB,busy,bulk||loading,available,(s.live*TerrainPager::SlotBytes)>>20);
            // Cold restores in the last second: faults minus the ones a
            // protection change alone satisfied.
            uint64_t decodes=s.faults-s.softRescues,decodesPerSec=decodes-lastDecodes;lastDecodes=decodes;
            if(commitTight && next>256)next=256;
            else if(!(busy||bulk||loading)) {
                int steady=TerrainBudgetSteady(effectiveMB,next,g_terrainHotMB,decodesPerSec,available);
                if(steady>effectiveMB && decodesPerSec>=300 && now-lastStutterLog>=10000) {
                    lastStutterLog=now;
                    H->log("terrain compression: %llu cold restores/s, resident target %d -> %d MiB (working set exceeds the budget)",decodesPerSec,effectiveMB,steady);
                }
                next=steady;
            }
            TerrainPager::SetBudget(size_t(next)*1024*1024);
            if(next!=effectiveMB && (next==g_terrainHotMB||effectiveMB==g_terrainHotMB||next-effectiveMB>=1024||effectiveMB-next>=1024))H->log("terrain compression: resident target %d -> %d MiB (generation=%d bulk_allocation=%d loading_tail=%d ui_signal=%d commit_tight=%d)",effectiveMB,next,int(busy),int(bulk),int(loading),int(uiTick!=nullptr),int(commitTight));
            effectiveMB=next;
            probeFast=busy||bulk||loading;
        }
        TerrainPager::Tick();
        if(g_terrainDedupProbe && now-lastProbe>=(probeFast?10000ull:120000ull)) {
            lastProbe=now;
            if(TerrainPager::Snapshot().live)LogDedupProbe();
        }
        if(now-lastLog>=30000) {
            lastLog=now;auto s=TerrainPager::Snapshot();
            if(s.live)H->log("terrain compression: live=%llu resident=%llu backing=%.1f MiB compressed=%.1f MiB encoded_commit=%.1f MiB faults=%llu evictions=%llu failures=%llu encodes=%llu reused=%llu writes=%llu shared_clones=%llu slot_overflows=%llu soft_blocked=%llu soft_rescues=%llu cancelled=%llu cow_shared=%llu cow_slots=%llu cow_privatized=%llu cow_privatize_mb=%.1f dedup_hits=%llu dedup_rebuilds=%llu",
                s.live,s.resident,double(s.resident*TerrainPager::SlotBytes)/(1024*1024),
                double(s.compressedBytes)/(1024*1024),double(s.compressedCommit)/(1024*1024),s.faults,s.evictions,s.failures,
                s.encodes,s.reusedEvictions,s.writeFaults,s.sharedClones,s.overflows,s.softBlocked,s.softRescues,s.cancelledEvictions,
                s.sharedViews,s.sharedSlots,s.privatizations,double(s.privatizeBytes)/(1024*1024),s.dedupHits,s.dedupRebuilds);
            if(g_terrainCowShare)H->log("terrain cow: copy_hook_calls=%lld unmanaged_src=%lld shared=%llu refused_not_slot=%llu refused_cold=%llu refused_packed=%llu refused_busy=%llu",
                g_cowCopyCalls,g_cowCopyUnmanaged,s.sharedViews,
                s.shareRefusedNotSlot,s.shareRefusedCold,s.shareRefusedPacked,s.shareRefusedBusy);
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
    if(!g_terrainHotMB || g_terrainWarmMB==-1) {   // 0 / -1 mean auto; other negatives stay invalid
        uint64_t total=InstalledPhysicalBytes();int hot=0,warm=0;
        AutoTerrainBudgets(total,&hot,&warm);
        if(!g_terrainHotMB)g_terrainHotMB=hot;
        if(g_terrainWarmMB==-1)g_terrainWarmMB=warm;
        H->log("terrain compression: auto budgets for %llu MiB RAM -> hot %d MiB, warm %d MiB",
               (unsigned long long)(total>>20),g_terrainHotMB,g_terrainWarmMB);
    }
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
    if(g_terrainDedup && !TerrainPager::EnableDedup()){H->log("terrain compression: dedup index allocation failed; dedup OFF");g_terrainDedup=0;}
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
    for(unsigned n=PagerHelperThreads();n--;)
        if(HANDLE helperThread=CreateThread(nullptr,0,TerrainEvictionHelper,nullptr,0,nullptr)){SetThreadPriority(helperThread,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(helperThread);}
    InterlockedExchange(&g_terrainCompressActive,1);
    H->log("terrain compression: lossless 1 m cache enabled, %d MiB resident target, cow_share=%d dedup=%d dedup_probe=%d; restart to disable",g_terrainHotMB,g_terrainCowShare,g_terrainDedup,g_terrainDedupProbe);
    return true;
}

extern "C" __declspec(dllexport) int BigmapTestCompressionInit(){return TerrainPager::Init(1024*1024);}
extern "C" __declspec(dllexport) void BigmapTestCompressionResize(TerrainOwnedVector* v,size_t n,int eligible,TerrainResizeFn fn){g_originalTerrainResize=fn;ResizeTerrainOwned(v,n,eligible!=0);}
extern "C" __declspec(dllexport) void* BigmapTestCompressionCopy(TerrainOwnedVector* dst,const TerrainOwnedVector* src,int eligible,TerrainCopyFn fn){g_originalTerrainCopy=fn;return CopyTerrainOwned(dst,src,eligible!=0);}
extern "C" __declspec(dllexport) void BigmapTestCompressionDestroy(void* control,TerrainDestroyFn fn){g_originalTerrainDestroy=fn;TerrainCompressedDestroy(control);}
extern "C" __declspec(dllexport) int BigmapTestCompressionEvict(void* p){return TerrainPager::Contains(p)&&TerrainPager::Evict(TerrainPager::Index(p),true);}
extern "C" __declspec(dllexport) void BigmapTestSetCowShare(int on){g_terrainCowShare=on;}
extern "C" __declspec(dllexport) void BigmapTestCompressionStats(uint64_t* out) {
    auto s=TerrainPager::Snapshot();
    out[0]=s.live;out[1]=s.resident;out[2]=s.sharedViews;out[3]=s.sharedSlots;
    out[4]=s.privatizations;out[5]=s.failures;
}
extern "C" __declspec(dllexport) int BigmapTestTerrainBudget(int hot,int warm,int busy,int bulk,uint64_t available){return TerrainBudgetMB(hot,warm,busy!=0,bulk!=0,available);}
extern "C" __declspec(dllexport) void BigmapTestAutoTerrainBudgets(uint64_t totalBytes,int* hot,int* warm){AutoTerrainBudgets(totalBytes,hot,warm);}
extern "C" __declspec(dllexport) int BigmapTestTerrainBudgetSteady(int prev,int next,int hot,uint64_t decodesPerSec,uint64_t available){return TerrainBudgetSteady(prev,next,hot,decodesPerSec,available);}
extern "C" __declspec(dllexport) int BigmapTestTerrainBudgetLive(int hot,int warm,int busy,int bulk,uint64_t available,uint64_t liveMB){return TerrainBudgetMB(hot,warm,busy!=0,bulk!=0,available,liveMB);}
extern "C" __declspec(dllexport) int BigmapTestWarmUpdate(TerrainWarmup* state,uint64_t now,int busy,uint64_t bulk,uint64_t ui,int signal){return state->Update(now,busy!=0,bulk,ui,signal!=0);}
extern "C" __declspec(dllexport) int BigmapTestInstallCompression(const Tpf2mpHost* host,int gog,int spacing,int enabled,int hotMB) {
    H=host;g_gog=gog!=0;g_terrainCacheSpacing=spacing;g_terrainCompress=enabled;g_terrainHotMB=hotMB;
    return InstallTerrainCompression();
}
