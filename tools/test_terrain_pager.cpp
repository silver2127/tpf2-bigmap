// Real Windows memory mappings and real concurrent access violations, isolated
// from the game. Optional argv[1]: offline 257x257 uint16 terrain sample file.
#include "../src/terrain_pager.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <cassert>

int main(int argc,char** argv) {
    using namespace TerrainPager;
    assert(Init(4*SlotBytes));
    auto p=Allocate();assert(p && uintptr_t(p)%32==0);
    for(size_t i=0;i<Samples;++i)assert(p[i]==0);
    std::vector<uint16_t> expected(Samples);
    for(size_t y=0;y<257;++y)for(size_t x=0;x<257;++x)
        expected[y*257+x]=uint16_t(y*173+x*x+65500);
    memcpy(p,expected.data(),Bytes);
    for(int n=0;n<100;++n) {
        assert(Evict(Index(p),true));
        assert(memcmp(p,expected.data(),Bytes)==0);
        size_t direct=(n*313)%Samples;p[direct]^=0x3197;expected[direct]^=0x3197;
        assert(memcmp(p,expected.data(),Bytes)==0);
        // Write faults, including both ends and all row boundaries.
        assert(Evict(Index(p),true));
        size_t pos=(n*257)%Samples;p[pos]^=0x9753;expected[pos]^=0x9753;
        assert(memcmp(p,expected.data(),Bytes)==0);
    }
    assert(Evict(Index(p),true));assert(Release(p));
    auto reused=Allocate();assert(reused==p);
    for(size_t i=0;i<Samples;++i)assert(reused[i]==0);
    assert(Release(reused));assert(!Release(reused));
    // Incompressible data must remain resident and accessible.
    p=Allocate();std::mt19937 rng(42);
    for(auto& v:expected)v=uint16_t(rng());
    memcpy(p,expected.data(),Bytes);assert(!Evict(Index(p),true));
    assert(memcmp(p,expected.data(),Bytes)==0);assert(Release(p));

    // Normal worker policy (not forced eviction): honor resident budget and
    // the five-second grace period, then restore cold storage on demand.
    std::vector<uint16_t*> policy(16);
    for(auto& t:policy){t=Allocate();assert(t);}
    Tick();assert(Snapshot().resident==16);
    {Guard g;for(auto t:policy)slots[Index(t)].touched=GetTickCount64()-6000;}
    Tick();assert(Snapshot().resident==4);
    for(auto t:policy)assert(t[Samples-1]==0);
    for(auto t:policy)assert(Release(t));

    // Shared compressed COW snapshots: no decompression or payload duplication
    // to create a version; first write isolates it, even after parent release.
    p=Allocate();for(size_t i=0;i<Samples;++i)expected[i]=uint16_t(i*11);
    memcpy(p,expected.data(),Bytes);assert(Evict(Index(p),true));
    auto packedBefore=Snapshot();std::vector<uint16_t*> clones(32);
    for(auto& t:clones){t=Clone(p);assert(t && t!=p);}
    assert(Snapshot().resident==0);
    assert(Snapshot().compressedBytes==packedBefore.compressedBytes);
    assert(Snapshot().sharedClones==packedBefore.sharedClones+clones.size());
    assert(Release(p));
    for(size_t n=0;n<clones.size();++n) {
        auto t=clones[n];assert(memcmp(t,expected.data(),Bytes)==0);
        auto before=Snapshot();assert(Evict(Index(t),true));
        assert(Snapshot().encodes==before.encodes && Snapshot().reusedEvictions==before.reusedEvictions+1);
        // Read restores RO; direct write remaps the same section RW.
        volatile auto sample=t[n];assert(sample==expected[n]);t[n]=uint16_t(1234+n);
        assert(Evict(Index(t),true));assert(t[n]==uint16_t(1234+n));
        assert(Release(t));
    }
    assert(Snapshot().live==0 && Snapshot().compressedBytes==0);

    // Each writer owns a disjoint word; eviction races all readers/writers.
    // Addresses remain retained across every eviction. Atomic increments make
    // lost/duplicated writes independently observable.
    constexpr unsigned N=16,Threads=8,Iterations=500000;
    std::vector<uint16_t*> tiles(N);
    for(auto& t:tiles){t=Allocate();assert(t);}
    std::atomic<bool> start{false},done{false};
    std::thread evictor([&]{while(!start.load())SwitchToThread();
        while(!done.load())for(auto t:tiles)Evict(Index(t),true);});
    std::vector<std::thread> writers;
    for(unsigned n=0;n<Threads;++n)writers.emplace_back([&,n]{
        while(!start.load())SwitchToThread();
        for(unsigned k=0;k<Iterations;++k) {
            auto t=tiles[k%N];
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(t)+n*1024);
            if(!(k%127))SwitchToThread();
        }
    });
    start=true;for(auto& w:writers)w.join();done=true;evictor.join();
    for(auto t:tiles) {
        for(unsigned n=0;n<Threads;++n)assert(reinterpret_cast<LONG*>(t)[n*1024]==Iterations/N);
        assert(Release(t));
    }
    // Multiple concurrent COW users share one immutable compressed source.
    p=Allocate();memcpy(p,expected.data(),Bytes);assert(Evict(Index(p),true));
    writers.clear();start=false;done=false;
    std::thread cowEvictor([&]{while(!start.load())SwitchToThread();
        while(!done.load())for(unsigned i=0;i<128;++i)Evict(i,true);});
    for(unsigned n=0;n<Threads;++n)writers.emplace_back([&,n]{
        while(!start.load())SwitchToThread();
        for(unsigned k=0;k<200;++k) {
            auto copy=Clone(p);assert(copy);
            assert(memcmp(copy,expected.data(),Bytes)==0);
            copy[n]=uint16_t(k);assert(copy[n]==k);assert(Release(copy));
        }
    });
    start=true;for(auto& w:writers)w.join();done=true;cowEvictor.join();
    assert(memcmp(p,expected.data(),Bytes)==0);assert(Release(p));
    if(argc>1) {
        FILE* f=nullptr;fopen_s(&f,argv[1],"rb");assert(f);
        size_t total=0,compressed=0,committed=0;unsigned count=0;
        ULONGLONG elapsed=GetTickCount64();
        while(fread(expected.data(),Bytes,1,f)==1) {
            p=Allocate();assert(p);memcpy(p,expected.data(),Bytes);
            if(Evict(Index(p),true)){auto s=Snapshot();compressed+=s.compressedBytes;committed+=s.compressedCommit;}
            else {compressed+=Bytes;committed+=SlotBytes;}
            assert(memcmp(p,expected.data(),Bytes)==0);
            assert(Release(p));total+=Bytes;++count;
        }
        assert(feof(f));fclose(f);
        printf("real tiles=%u ratio=%.6f total_roundtrip_ms=%llu projected_64980_gib=%.3f cold_commit_gib=%.3f\n",
            count,double(compressed)/total,GetTickCount64()-elapsed,
            double(compressed)/total*64980*Bytes/(1024.*1024*1024),
            double(committed)/total*64980*Bytes/(1024.*1024*1024));
    }
    // Scale up together to exercise placeholder splitting, O(1) lookup and
    // complete release of both mapped and compressed backing at world teardown.
    DWORD beforeHandles=0,afterHandles=0;
    GetProcessHandleCount(GetCurrentProcess(),&beforeHandles);
    std::vector<uint16_t*> many(4096);
    for(auto& t:many){t=Allocate();assert(t);memcpy(t,expected.data(),Bytes);}
    for(auto t:many){Evict(Index(t),true);assert(memcmp(t,expected.data(),Bytes)==0);Evict(Index(t),true);}
    for(auto t:many)assert(Release(t));
    GetProcessHandleCount(GetCurrentProcess(),&afterHandles);assert(beforeHandles==afterHandles);
    auto s=Snapshot();assert(s.live==0&&s.resident==0&&s.compressedBytes==0&&s.compressedCommit==0&&s.failures==0);
    printf("PASS: exact roundtrips, write faults, address reuse, incompressible fallback, "
           "%u concurrent writes; faults=%llu evictions=%llu failures=%llu\n",
           Threads*Iterations,s.faults,s.evictions,s.failures);
}
