// Runs the unchanged native game function in a private ELF image, never a live game.
#include "../terrain_cached_kernels.h"
#include <elf.h>
#include <sys/mman.h>
#include <fstream>
#include <vector>
#include <random>
#include <cassert>
#include <cstdio>
#include <chrono>
int main(int argc,char** argv) {
    assert(argc==2);
    std::ifstream f(argv[1],std::ios::binary);
    Elf64_Ehdr eh{}; f.read(reinterpret_cast<char*>(&eh),sizeof(eh));
    assert(f && !memcmp(eh.e_ident,ELFMAG,SELFMAG) && eh.e_machine==EM_X86_64);
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    f.seekg(eh.e_phoff);f.read(reinterpret_cast<char*>(ph.data()),ph.size()*sizeof(ph[0]));
    size_t size=0;for(auto& p:ph)if(p.p_type==PT_LOAD)size=std::max(size,size_t(p.p_vaddr+p.p_memsz));
    auto* image=static_cast<char*>(mmap(nullptr,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(image!=MAP_FAILED);
    for(auto& p:ph)if(p.p_type==PT_LOAD){f.seekg(p.p_offset);f.read(image+p.p_vaddr,p.p_filesz);assert(f);}
    const auto original=reinterpret_cast<BicubicRefineFn>(image+0xd9b850);
    g_originalBicubicRefine=original;
    char temp[]="/tmp/bigmap-refine.XXXXXX";assert(mkdtemp(temp));
    assert(terrain_cache::Cache().Open(temp,64*1024*1024));
    std::mt19937 rng(35924);size_t comparisons=0,samples=0;
    for(int k:{2,4,6,8,16,32,64})for(int dim:{4,7,17,67})for(int pattern=0;pattern<12;++pattern) {
        std::vector<uint16_t> src((dim+2)*(dim+2));
        for(auto& v:src)v=pattern==0?0:pattern==1?65535:pattern==2?32768:uint16_t(rng());
        BicubicRefineVector v{src.data(),src.data()+src.size(),src.data()+src.size()};
        const int x0=pattern%2,y0=pattern%3,x1=dim-1,y1=dim-1,stride=k*(dim+3);
        std::vector<uint16_t> a(stride*stride,0xabcd),b=a;
        RefineScale scale{1.f,4.f,655.35f};
        original(k,&v,dim,x0,y0,x1,y1,scale,a.data(),stride,3,5);
        TerrainRefineFast(k,&v,dim,x0,y0,x1,y1,scale,b.data(),stride,3,5);
        if(a!=b){for(size_t n=0;n<a.size();++n)if(a[n]!=b[n]){fprintf(stderr,"mismatch k=%d dim=%d pattern=%d sample=%zu stock=%u fast=%u\n",k,dim,pattern,n,a[n],b[n]);return 1;}}
        for(int pass=0;pass<2;++pass){std::fill(b.begin(),b.end(),0xabcd);
            TerrainRefineCached(k,&v,dim,x0,y0,x1,y1,scale,b.data(),stride,3,5);assert(a==b);}
        ++comparisons;samples+=a.size();
    }
    for(int shift:{0,64,510,512,514,800})for(int pattern=0;pattern<16;++pattern){
        std::vector<uint16_t> a(4096);for(auto& v:a)v=uint16_t(rng());auto b=a;
        BicubicRefineVector av{a.data()+512,a.data()+561,a.data()+561},bv{b.data()+512,b.data()+561,b.data()+561};
        RefineScale scale{1,4,655.35f};
        original(4,&av,7,0,0,6,6,scale,a.data()+shift,40,3,5);
        TerrainRefineCached(4,&bv,7,0,0,6,6,scale,b.data()+shift,40,3,5);assert(a==b);
        ++comparisons;samples+=a.size();
    }
    printf("native refinement: %zu exact comparisons, %zu samples\n",comparisons,samples);
    {
        std::vector<uint16_t> src(19*19),dst(76*76);for(auto& x:src)x=uint16_t(rng());
        BicubicRefineVector v{src.data(),src.data()+src.size(),src.data()+src.size()};RefineScale scale{1,4,655.35f};
        auto bench=[&](BicubicRefineFn fn){BicubicRefineFn volatile target=fn;auto begin=std::chrono::steady_clock::now();
            for(int i=0;i<10000;++i)target(4,&v,19,0,0,18,18,scale,dst.data(),76,0,0);
            return std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();};
        const auto stock=bench(original),fast=bench(TerrainRefineFast);
        printf("native 19x19 refinement, 10000 calls: stock %.6f s, fast %.6f s\n",stock,fast);
    }
    assert(terrain_cache::Cache().hits>0);
    DIR* dir=opendir(temp);assert(dir);while(auto* e=readdir(dir))if(e->d_name[0]!='.')assert(!unlink((std::string(temp)+"/"+e->d_name).c_str()));closedir(dir);assert(!rmdir(temp));
    munmap(image,size);
}
