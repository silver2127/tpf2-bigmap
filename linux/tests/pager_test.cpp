#include "../terrain_pager.h"
#include <cassert>
#include <cstdio>
int main(){
    linux_pager::TerrainPager pager;
    if(!pager.Start(64,0,true)){puts("SKIP: userfaultfd unavailable");return 77;}
    // Untouched allocations consume no resident pages, including reuse and
    // release before the first touch. First access may be a read OR a write.
    auto* untouched=pager.Allocate();assert(untouched);
    assert(pager.Get().resident==0);
    unsigned char zeroResidency[linux_pager::TerrainPager::Stride/4096];
    assert(mincore(untouched,linux_pager::TerrainPager::Stride,zeroResidency)==0);
    for(auto page:zeroResidency)assert(!(page&1));
    auto zeroProbe=pager.Probe();assert(zeroProbe.hashed==1 && zeroProbe.zero==1 && pager.Get().resident==0);
    assert(pager.Release(untouched));
    untouched=pager.Allocate();untouched[17]=321;
    assert(untouched[0]==0 && untouched[17]==321 && untouched[TerrainCodec::Samples-1]==0);
    assert(pager.Release(untouched));
    assert(pager.Get().resident==0);
    std::vector<uint16_t*> tiles;
    for(int n=0;n<32;++n){auto* p=pager.Allocate();assert(p);for(size_t i=0;i<TerrainCodec::Samples;++i)p[i]=uint16_t(i/257+i%257+n%16);tiles.push_back(p);}
    for(int tries=0;tries<100 && pager.Get().evictions<32;++tries)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(pager.Get().evictions>=32 && pager.Get().resident==0);
    assert(pager.Get().dedupHits>=16);
    auto probe=pager.Probe();assert(probe.hashed==32 && probe.distinct==16 && probe.duplicates==16 && probe.pairs==16 && probe.zero==0);
    assert(pager.Get().resident==0); // probing a cold pool does not restore it
    unsigned char residency[linux_pager::TerrainPager::Stride/4096];
    assert(mincore(tiles[0],linux_pager::TerrainPager::Stride,residency)==0);
    for(auto page:residency)assert(!(page&1));
    // A private write to one restored twin cannot modify the other's blob.
    tiles[0][7]=54321;assert(tiles[16][7]==7);tiles[0][7]=7;
    std::vector<std::thread> readers;
    for(int n=0;n<8;++n)readers.emplace_back([&,n]{for(int k=n;k<32;k+=8)for(size_t i=0;i<TerrainCodec::Samples;++i)assert(tiles[k][i]==uint16_t(i/257+i%257+k%16));});
    for(auto& t:readers)t.join();
    std::atomic<bool> done{false};
    std::thread writer([&]{uint16_t value=0;while(!done){for(int n=0;n<32;++n)tiles[n][0]=value;++value;}for(int n=0;n<32;++n)tiles[n][0]=1234;});
    std::this_thread::sleep_for(std::chrono::seconds(5));done=true;writer.join();
    for(int n=0;n<32;++n){auto* p=tiles[n];assert(p[0]==1234);for(size_t i=1;i<TerrainCodec::Samples;++i)assert(p[i]==uint16_t(i/257+i%257+n%16));assert(pager.Release(p));}
    for(int n=0;n<1000;++n){auto p=pager.Allocate();assert(p && p[0]==0 && p[TerrainCodec::Samples-1]==0);p[0]=42;assert(pager.Release(p));}
    auto stats=pager.Get();assert(stats.live==0 && stats.packed==0 && stats.resident==0);printf("PASS: lossless paging, parallel restores, write/evict race, reuse; %llu evictions, %llu faults\n",(unsigned long long)stats.evictions,(unsigned long long)stats.faults);
    linux_pager::TerrainPager eager;
    assert(eager.Start(2,2*linux_pager::TerrainPager::Stride,false,false));
    auto* a=eager.Allocate();auto* b=eager.Allocate();assert(a && b && eager.Get().resident==2);
    assert(a[0]==0 && b[TerrainCodec::Samples-1]==0);
    auto eagerProbe=eager.Probe();assert(eagerProbe.hashed==2 && eagerProbe.zero==2 && eagerProbe.pairs==1);
    assert(eager.Release(a) && eager.Release(b));
    return 0;
}
