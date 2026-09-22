#include "../terrain_cached_kernels.h"
#include "game_image.h"
#include <random>
#include <cstdio>
#include <chrono>
int main(int argc,char** argv) {
    assert(argc==2);GameImage game(argv[1]);
    g_terrainAlignBase=uintptr_t(game.data);
    g_originalCalculateHeightMod=reinterpret_cast<CalculateHeightModFn>(game.data+0xda1a70);
    char temp[]="/tmp/bigmap-align.XXXXXX";assert(mkdtemp(temp));
    assert(terrain_cache::Cache().Open(temp,64*1024*1024));
    std::mt19937 rng(35924);size_t comparisons=0,samples=0;
    struct Alignment { std::vector<float> triangles,weights;int32_t type; };
    static_assert(offsetof(Alignment,type)==0x30,"native alignment ABI");
    for(int dim:{2,3,7,16,33,65,129,257})for(int pattern=0;pattern<192;++pattern){
        float box[]={0,0,float(dim-1),float(dim-1)};int32_t size[]={dim,dim};
        const float scales[]={0.1f,1.f,1.f/256,.00762939453125f,.0152587890625f,.0001f,10.f,655.35f};
        const float offsets[]={-100.f,-1000.f,0.f,.25f};
        float scale=scales[pattern%8],offset=offsets[(pattern/8)%4];
        std::vector<uint16_t> a(dim*dim);for(auto& v:a)v=uint16_t(rng());auto b=a;
        const auto input=a;
        Alignment align[6];std::vector<const uint8_t*> ptrs;
        for(int t=0;t<pattern%7;++t){
            auto& v=align[t];v.type=t%3;
            v.triangles={0.f,0.f,float(10+t),float(dim-1),0.f,float(20+t),0.f,float(dim-1),float(30+t)};
            if(pattern%4)v.weights={0.25f,0.5f,1.f};
            if(pattern>=48){for(size_t n=0;n<v.triangles.size();++n)v.triangles[n]=n%3==2?float(rng()%65536)*scale+offset:float(int(rng()%(2*dim))-dim/2);
                if(pattern%4)for(auto& w:v.weights)w=float(rng()%65536)/65535.f;}
            ptrs.push_back(reinterpret_cast<const uint8_t*>(&v));
        }
        AlignPointerVector list{ptrs.data(),ptrs.data()+ptrs.size(),ptrs.data()+ptrs.size()};
        AlignU16Vector av{a.data(),a.data()+a.size(),a.data()+a.size()},bv{b.data(),b.data()+b.size(),b.data()+b.size()};
        g_originalCalculateHeightMod(box,size,scale,offset,&list,&av);
        CalculateHeightModDetour(box,size,scale,offset,&list,&bv);
        if(a!=b){for(size_t n=0;n<a.size();++n)if(a[n]!=b[n]){fprintf(stderr,"mismatch dim=%d pattern=%d sample=%zu stock=%u fast=%u\n",dim,pattern,n,a[n],b[n]);return 1;}}
        for(int pass=0;pass<2;++pass){std::copy(input.begin(),input.end(),b.begin());
            TerrainAlignCached(box,size,scale,offset,&list,&bv);assert(a==b);}
        ++comparisons;samples+=a.size();
    }
    printf("native alignment: %zu exact comparisons, %zu samples\n",comparisons,samples);
    for(int count:{0,6}){
        float box[]={0,0,64,64};int32_t size[]={65,65};std::vector<uint16_t> out(65*65,10000);
        AlignU16Vector v{out.data(),out.data()+out.size(),out.data()+out.size()};
        Alignment a[6];std::vector<const uint8_t*> ptrs;
        for(int i=0;i<count;++i){a[i].type=i%3;a[i].triangles={0,0,10,64,0,20,0,64,30};a[i].weights={.25f,.5f,1.f};ptrs.push_back(reinterpret_cast<const uint8_t*>(&a[i]));}
        AlignPointerVector list{ptrs.data(),ptrs.data()+ptrs.size(),ptrs.data()+ptrs.size()};
        auto bench=[&](CalculateHeightModFn fn){CalculateHeightModFn volatile target=fn;auto begin=std::chrono::steady_clock::now();
            for(int i=0;i<1000;++i){std::fill(out.begin(),out.end(),10000);target(box,size,.1f,-100.f,&list,&v);}
            return std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();};
        const auto stock=bench(g_originalCalculateHeightMod),fast=bench(CalculateHeightModDetour);
        printf("native 65x65 alignment (%d triangles), 1000 calls: stock %.6f s, fast %.6f s\n",count,stock,fast);
    }
    assert(terrain_cache::Cache().hits>0);
    DIR* dir=opendir(temp);assert(dir);while(auto* e=readdir(dir))if(e->d_name[0]!='.')assert(!unlink((std::string(temp)+"/"+e->d_name).c_str()));closedir(dir);assert(!rmdir(temp));
}
