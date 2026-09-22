#include "../terrain_copy.h"
#include <array>
#include <cassert>
#include <cstring>
#include <random>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
extern "C" void TestTerrainCopy(const void*,void*,size_t,void*,uint64_t*);
extern "C" void TestTerrainCopyDone();
int main(){
    auto* stock=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(stock!=MAP_FAILED);
    memcpy(stock,TerrainCopyBytes,sizeof(TerrainCopyBytes));
    const uint8_t jump[]={0xff,0x25,0,0,0,0};
    memcpy(stock+sizeof(TerrainCopyBytes),jump,6);
    auto back=uintptr_t(TestTerrainCopyDone);
    memcpy(stock+sizeof(TerrainCopyBytes)+6,&back,8);
    assert(!mprotect(stock,4096,PROT_READ|PROT_EXEC));
    std::mt19937 rng(71291);
    for(size_t n:{1u,2u,3u,4u,7u,8u,9u,31u,32u,257u,66049u}){
        for(int delta:{-519,-17,-2,-1,0,1,2,17,519,140000}){
            std::vector<uint8_t> a(410000),b;
            for(auto& x:a)x=uint8_t(rng());
            b=a;
            size_t src=2000+(rng()%8),dst=size_t(int(src)+delta);
            std::array<uint64_t,9> ar{},br{};
            TestTerrainCopy(a.data()+src,a.data()+dst,n*2,stock,ar.data());
            TestTerrainCopy(b.data()+src,b.data()+dst,n*2,reinterpret_cast<void*>(TerrainCopyBridge),br.data());
            assert(a==b);
            ar[2]-=uintptr_t(a.data());ar[3]-=uintptr_t(a.data());
            br[2]-=uintptr_t(b.data());br[3]-=uintptr_t(b.data());
            assert(ar==br);
            assert(br[0]==n*2 && br[4]==n*2);
        }
    }
    // Exact end-of-page reads and writes: no vectorized overrun allowed.
    const size_t page=size_t(sysconf(_SC_PAGESIZE));
    auto alloc=[&](){auto* p=static_cast<uint8_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(p!=MAP_FAILED);assert(!mprotect(p+page,page,PROT_NONE));return p;};
    auto* src=alloc();auto* dst=alloc();
    for(size_t n=1;n<=257;++n){
        for(size_t i=0;i<n*2;++i)src[page-n*2+i]=uint8_t(rng());
        uint64_t regs[9]{};
        TestTerrainCopy(src+page-n*2,dst+page-n*2,n*2,reinterpret_cast<void*>(TerrainCopyBridge),regs);
        assert(!memcmp(src+page-n*2,dst+page-n*2,n*2));
    }
    munmap(src,page*2);munmap(dst,page*2);munmap(stock,4096);
}
