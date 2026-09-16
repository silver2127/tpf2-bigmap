// Native Steam Linux build 35924. See docs/linux/PORT.md for measured sites.
#include "../src/tpf2mp_plugin.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <sys/mman.h>

namespace {
const Tpf2mpHost* H;
constexpr const char* Section = "tpf2_bigmap";
constexpr uintptr_t SizeRva = 0x11232c0, RasterRva = 0x14e05a0;
constexpr uintptr_t ComboRva = 0x31443a0, ComboCall = 0x1149945;
constexpr uintptr_t RatioCall = 0x1149ae7, RatioRva = 0x14276c0;
constexpr uintptr_t OctreeSite = 0xa84234;
constexpr uintptr_t RasterCalls[] = {0x1066d80,0x150af14,0x151e175,0x15269cf};
constexpr uint8_t SizeBytes[] = {0x55,0x48,0x89,0xe5,0x41,0x55,0x41,0x89,0xf5,0x41,0x54,0x53,0x89,0xfb,0x48,0x83,0xec,0x58};
constexpr uint8_t OctreeBytes[] = {0xf3,0x0f,0x10,0x05,0x48,0x7c,0x40,0x03,0xbe,0x0a,0,0,0};
constexpr uint8_t RatioLoop[] = {0x83,0xfb,0x05};
constexpr uint8_t RatioGate[] = {0x83,0xfb,0x02};
using SizeFn = uint64_t (*)(int,int,void*);
using RasterFn = void (*)(void*,const float*,float);
using ComboFn = void* (*)(const void*,void*,int,void*,int);
SizeFn originalSize;
int cap=512, maxRatio=20, rows=0;
std::atomic<int> stockRows{7};
double cellBudget=1.5e9;
struct Row { int side,index; char label[64]; };
Row extra[12];
struct Claim { int size,format,x,y; };
Claim claims[19*20]; int claimCount=0;
int tilesX=0,tilesY=0,sizeIndex=6,formatIndex=0;

uint64_t Pack(int x,int y) { return uint32_t(x)|(uint64_t(uint32_t(y))<<32); }
bool Fits(int x,int y) {
    // Until the Linux placement-distance implementation is widened, keep its
    // heightmap-coordinate diagonal squared inside int32 too (512^2 is just over).
    const int64_t px=int64_t(x)*64,py=int64_t(y)*64;
    return (px+1)*(py+1)<=INT_MAX && px*px+py*py<=INT_MAX;
}
void Bound(int& x,int& y) {
    x=std::clamp(x,2,cap)&~1; y=std::clamp(y,2,cap)&~1;
    while (!Fits(x,y)) { if(x>=y)x-=2;else y-=2; }
}
uint64_t Shape(int side,int format) {
    int ratio=std::clamp(format+1,1,std::min(maxRatio,cap/2));
    int x=std::max(2,2*int(std::floor(side/std::sqrt(double(ratio))/2+0.5)));
    x=std::min(x,2*((cap/ratio)/2));
    while(x>2 && !Fits(x,x*ratio))x-=2;
    return Pack(x,x*ratio);
}
bool Parse(const char* s,int& x,int& y) {
    if(!s || !*s)return false;
    char* end; long a=std::strtol(s,&end,10);
    if(end==s || a<2 || a>INT_MAX)return false;
    while(*end==' ' || *end=='\t')++end;
    if(*end!='x' && *end!='X')return false;
    const char* second=end+1;long b=std::strtol(second,&end,10);
    if(end==second || b<2 || b>INT_MAX)return false;
    while(*end==' ' || *end=='\t' || *end=='\n' || *end=='\r')++end;
    if(*end)return false;
    x=int(a);y=int(b);return true;
}
uint64_t Size(int size,int format,void* cfg) {
    size=std::max(size,0);format=std::clamp(format,0,maxRatio-1);
    const int row=size-stockRows.load();
    if(rows && row>=0 && row<rows) {
        for(int i=0;i<claimCount;++i)if(claims[i].size==extra[row].index && claims[i].format==format)
            return Pack(claims[i].x,claims[i].y);
        return Shape(extra[row].side,format);
    }
    for(int i=0;i<claimCount;++i)
        if(claims[i].size==size && claims[i].format==format)return Pack(claims[i].x,claims[i].y);
    if(size==sizeIndex && format==formatIndex && tilesX && tilesY)return Pack(tilesX,tilesY);
    if(size>=7)return Shape(96,format);
    if(format>=5) {
        const uint64_t square=originalSize(size,0,cfg);
        if(cfg) { int32_t xy[2];std::memcpy(xy,static_cast<uint8_t*>(cfg)+0x28,8);
            if(xy[0]>0 && xy[1]>0)return square; }
        return Shape(int(std::sqrt(double(uint32_t(square))*uint32_t(square>>32))),format);
    }
    return originalSize(size,format,cfg);
}
float RasterCell(float w,float h,float cell) {
    if(!std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0 || !std::isfinite(cell) || cell<=0)return cell;
    // Match the game's float division/truncation, using double for the product.
    auto cells=[&](float c){return (std::floor(double(w/c))+1)*(std::floor(double(h/c))+1);};
    while(cells(cell)>cellBudget)cell+=1.f;
    return cell;
}
void Raster(void* self,const float* box,float cell) {
    const float grown=RasterCell(box[2]-box[0],box[3]-box[1],cell);
    if(grown!=cell)H->log("street raster: %.0f -> %.0f m cells",double(cell),double(grown));
    reinterpret_cast<RasterFn>(H->moduleBase()+RasterRva)(self,box,grown);
}
// Borrow libstdc++ string storage; the game copies labels into its widget.
// No allocator or ownership crosses the ABI. Stock SSO pointers remain valid.
struct String { const char* data; size_t size; char local[16]; };
struct Vector { const String* begin; const String* end; const String* capacity; };
static_assert(sizeof(String)==32 && sizeof(Vector)==24,"libstdc++ x86-64 ABI");
void* Combo(const Vector* items,void* change,int selection,void* aux,int flags) {
    const auto original=reinterpret_cast<ComboFn>(H->moduleBase()+ComboRva);
    const size_t n=items->end-items->begin;
    if(n!=4 && n!=7) { H->log("unexpected stock size row count %zu; extra rows disabled",n);rows=0;return original(items,change,selection,aux,flags); }
    stockRows=int(n);
    String combined[32]{};
    std::memcpy(combined,items->begin,n*sizeof(String));
    for(int i=0;i<rows;++i){combined[n+i].data=extra[i].label;combined[n+i].size=std::strlen(extra[i].label);}
    Vector view{combined,combined+n+rows,combined+n+rows};
    H->log("size dropdown: %zu stock + %d native Linux rows",n,rows);
    return original(&view,change,selection,aux,flags);
}
String* RatioText(String* out,int format) {
    std::memset(out,0,sizeof(*out));out->data=out->local;
    out->size=std::snprintf(out->local,sizeof(out->local),"1:%d",std::clamp(format+1,1,maxRatio));
    return out;
}
void Jump(uint8_t* p,uintptr_t dest) {
    p[0]=0xff;p[1]=0x25;std::memset(p+2,0,4);std::memcpy(p+6,&dest,8);
}
void* Near(uintptr_t anchor) {
    for(uintptr_t d=0x100000;d<0x70000000;d+=0x100000)for(int sign:{1,-1}) {
        const uintptr_t at=(anchor+sign*d)&~uintptr_t(4095);
        void* p=mmap(reinterpret_cast<void*>(at),4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if(p==MAP_FAILED)continue;
        if(uintptr_t(p)==at)return p;
        munmap(p,4096);
    }
    return nullptr;
}
struct Patch { uintptr_t rva; uint8_t before[18]{},after[18]{}; uint32_t len; };
Patch patches[12];int patchCount=0;
bool Plan(uintptr_t rva,const uint8_t* before,const uint8_t* after,uint32_t len) {
    if(patchCount>=12 || len>18 || !H->verifyBytes(rva,before,len))return false;
    auto& p=patches[patchCount++];p.rva=rva;p.len=len;
    std::memcpy(p.before,before,len);std::memcpy(p.after,after,len);return true;
}
bool PlanCall(uintptr_t rva,uintptr_t callee,void* target,uint8_t*& stub) {
    uint8_t before[5]={0xe8},after[5]={0xe8};
    int32_t a=int32_t(callee-rva-5),b=int32_t(uintptr_t(stub)-(H->moduleBase()+rva+5));
    std::memcpy(before+1,&a,4);std::memcpy(after+1,&b,4);Jump(stub,uintptr_t(target));stub+=16;
    return Plan(rva,before,after,5);
}
}

extern "C" __attribute__((visibility("default")))
int Tpf2mpPluginInit(const Tpf2mpHost* host,Tpf2mpPluginInfo* info) {
    if(!host || !info || host->abiMajor!=TPF2MP_ABI_MAJOR || host->size<sizeof(Tpf2mpHost))return TPF2MP_ERR_ABI;
    H=host;*info={"tpf2_bigmap","0.4.0-linux-dev.1","Native Linux large-map sizes, ratios and street raster"};
    if(!H->buildOk() || !H->moduleBase())return TPF2MP_ERR_BUILD;
    if(!H->cfgBool(Section,"enabled",1))return TPF2MP_ERR_DISABLED;
    const int depth=H->cfgInt(Section,"octree_depth",11);
    if(depth!=11){H->log("Linux currently requires octree_depth=11; refusing unsupported depth %d",depth);return TPF2MP_ERR_FAILED;}
    cap=std::clamp(H->cfgInt(Section,"max_tiles",512),2,512)&~1;
    maxRatio=std::clamp(H->cfgInt(Section,"max_ratio",20),5,20);
    cellBudget=double(std::clamp(H->cfgInt(Section,"cell_budget_millions",1500),1,2000))*1e6;
    const bool octree=H->cfgBool(Section,"octree",1),raster=H->cfgBool(Section,"street_raster",1);
    if(!octree)cap=std::min(cap,256);
    if(!raster)cap=std::min(cap,180); // never offer overflowing generation
    tilesX=H->cfgInt(Section,"tiles_x",0);tilesY=H->cfgInt(Section,"tiles_y",0);
    if(tilesX>0 && tilesY>0)Bound(tilesX,tilesY);else tilesX=tilesY=0;
    sizeIndex=H->cfgInt(Section,"size_index",6);formatIndex=H->cfgInt(Section,"format_index",0);
    for(int s=0;s<19;++s)for(int f=0;f<20;++f) {
        char key[32];std::snprintf(key,sizeof(key),"size%d_format%d",s,f);int x,y;
        if(Parse(H->cfgStr(Section,key,""),x,y)){Bound(x,y);claims[claimCount++]={s,f,x,y};}
    }
    const int defaults[]={128,160,192,224,256,320,384,448,512};
    if(H->cfgBool(Section,"add_size_rows",1))for(int index=7;index<19;++index) {
        int side=index<16?defaults[index-7]:0;
        for(int i=0;i<claimCount;++i)if(claims[i].size==index && claims[i].format==0)
            side=int(std::sqrt(double(claims[i].x)*claims[i].y));
        if(!side || side>cap)continue;
        auto& row=extra[rows++];row.side=side;row.index=index;
        char key[32],fallback[64];std::snprintf(key,sizeof(key),"size_label%d",index);
        const auto square=Shape(side,0);
        std::snprintf(fallback,sizeof(fallback),"%.2f x %.2f km",uint32_t(square)*.256,uint32_t(square>>32)*.256);
        std::snprintf(row.label,sizeof(row.label),"%s",H->cfgStr(Section,key,fallback));
    }
    if(!H->verifyBytes(SizeRva,SizeBytes,sizeof(SizeBytes)))return TPF2MP_ERR_BUILD;
    auto* page=static_cast<uint8_t*>(Near(H->moduleBase()+SizeRva));if(!page)return TPF2MP_ERR_FAILED;
    uint8_t* stub=page;
    // A relocated constant keeps the stock instruction shape and register ABI.
    const float extent=65536.f;std::memcpy(page+4000,&extent,4);
    if(octree) {
        uint8_t patch[13];std::memcpy(patch,OctreeBytes,13);
        int32_t rel=int32_t(uintptr_t(page+4000)-(H->moduleBase()+OctreeSite+8));
        std::memcpy(patch+4,&rel,4);patch[9]=11;
        if(!Plan(OctreeSite,OctreeBytes,patch,13))return TPF2MP_ERR_BUILD;
    }
    if(raster)for(auto call:RasterCalls)if(!PlanCall(call,RasterRva,reinterpret_cast<void*>(Raster),stub))return TPF2MP_ERR_BUILD;
    if(rows && !PlanCall(ComboCall,ComboRva,reinterpret_cast<void*>(Combo),stub))return TPF2MP_ERR_BUILD;
    if(maxRatio>5) {
        uint8_t loop[3]={0x83,0xfb,uint8_t(maxRatio)},gate[3]={0x83,0xfb,uint8_t(maxRatio-1)};
        if(!PlanCall(RatioCall,RatioRva,reinterpret_cast<void*>(RatioText),stub) ||
           !Plan(0x1149bf4,RatioLoop,loop,3) || !Plan(0x1149c01,RatioGate,gate,3))return TPF2MP_ERR_BUILD;
    }
    if(mprotect(page,4096,PROT_READ|PROT_EXEC))return TPF2MP_ERR_FAILED;
    void* trampoline=nullptr;
    if(!H->installHook(H->moduleBase()+SizeRva,reinterpret_cast<void*>(Size),sizeof(SizeBytes),&trampoline))return TPF2MP_ERR_FAILED;
    originalSize=reinterpret_cast<SizeFn>(trampoline);
    for(int i=0;i<patchCount;++i)if(!H->patchBytes(patches[i].rva,patches[i].after,patches[i].len)) {
        // Keep code and original trampoline mapped even after rollback.
        for(int j=i;j>=0;--j)H->patchBytes(patches[j].rva,patches[j].before,patches[j].len);
        H->patchBytes(SizeRva,SizeBytes,sizeof(SizeBytes));
        H->log("patch failed; attempted rollback, restart before using big maps");return TPF2MP_ERR_FAILED;
    }
    H->log("Linux map controls active: %d-tile edge cap, depth %d, %d added sizes, ratios 1:1..1:%d",cap,octree?11:10,rows,maxRatio);
    H->log("Experimental port: Windows memory compression and depth 12/13 are not enabled");
    return TPF2MP_OK;
}
