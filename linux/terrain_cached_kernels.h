#pragma once
#include "terrain_cache.h"
#include "terrain_refine.h"
#include "terrain_align.h"
inline void TerrainRefineCached(int k,const BicubicRefineVector* src,int dim,int x0,int y0,int x1,int y1,
    RefineScale scale,uint16_t* out,int stride,int dx,int dy) {
    using namespace terrain_cache;
    Bytes key,cached;size_t width=0,height=0;uint16_t* start=nullptr;
    try {
        if(!Cache().directory.empty() && k>=2 && k<=64 && !(k&1) && x0>=0 && y0>=0 && x1>x0+1 && y1>y0+1
            && x1<=dim && dim>0 && stride>0 && dx>=0 && dy>=0) {
            const uint64_t sourceWords=(uintptr_t(src->last)-uintptr_t(src->first))/2;
            const uint64_t end=uint64_t(y1)*dim+x1+1;
            width=uint64_t(x1-x0-1)*k;height=uint64_t(y1-y0-1)*k;
            const uint64_t offset=(uint64_t(dy)+k/2)*stride+uint64_t(dx)+k/2;
            const uint64_t span=(height-1)*stride+width;
            if(end<=sourceWords && width<=uint64_t(stride) && uint64_t(dx)+k/2+width<=uint64_t(stride)
                && width*height<=4*1024*1024 && uint64_t(x1-x0+1)*(y1-y0+1)<=4*1024*1024
                && offset+span<=SIZE_MAX/2 && !Overlap(src->first,sourceWords*2,out+offset,span*2)
                && !Overlap(src,sizeof(*src),out+offset,span*2)) {
                start=out+offset;
                const int args[]={1,35924,1,k,dim,x0,y0,x1,y1,stride,dx,dy};Add(key,args,sizeof(args));Add(key,&scale,sizeof(scale));
                const uint32_t control=_mm_getcsr()&0xffc0;Add(key,&control,sizeof(control));
                for(int y=y0;y<=y1;++y)Add(key,src->first+size_t(y)*dim+x0,size_t(x1-x0+1)*2);
                if(Cache().Get(key,cached,width*height*2)) {
                    for(size_t y=0;y<height;++y)memcpy(start+y*stride,cached.data()+y*width*2,width*2);
                    return;
                }
            }
        }
    } catch(...) {key.clear();start=nullptr;}
    TerrainRefineFast(k,src,dim,x0,y0,x1,y1,scale,out,stride,dx,dy);
    if(start && !key.empty())try {
        cached.resize(width*height*2);for(size_t y=0;y<height;++y)memcpy(cached.data()+y*width*2,start+y*stride,width*2);
        Cache().Put(key,cached);
    }catch(...){}
}
inline void TerrainAlignCached(const float* box,const int32_t* size,float scale,float offset,
    const AlignPointerVector* list,AlignU16Vector* result) {
    using namespace terrain_cache;
    if(list->first==list->last){CalculateHeightModDetour(box,size,scale,offset,list,result);return;}
    Bytes key,cached;const auto outputBytes=uintptr_t(result->last)-uintptr_t(result->first);
    try {
        bool valid=!Cache().directory.empty() && size[0]>=2 && size[1]>=2 && int64_t(size[0])*size[1]<=kAlignMaxSamples
            && outputBytes==uint64_t(size[0])*size[1]*2 && AlignListSupported(list)
            && !Overlap(result->first,outputBytes,list,sizeof(*list)) && !Overlap(result->first,outputBytes,box,16)
            && !Overlap(result->first,outputBytes,size,8)
            && !Overlap(result->first,outputBytes,list->first,uintptr_t(list->last)-uintptr_t(list->first));
        if(valid) {
            const uint64_t args[]={1,35924,2,uint64_t(list->last-list->first)};Add(key,args,sizeof(args));
            const uint32_t control=_mm_getcsr()&0xffc0;Add(key,&control,sizeof(control));
            Add(key,box,16);Add(key,size,8);Add(key,&scale,4);Add(key,&offset,4);Add(key,result->first,outputBytes);
            for(auto item=list->first;item!=list->last && valid;++item) {
                const auto* a=*item;valid=!Overlap(result->first,outputBytes,a,0x34);
                Add(key,a+0x30,4);
                for(size_t field:{size_t(0),size_t(0x18)}) {
                    uintptr_t first,last;memcpy(&first,a+field,8);memcpy(&last,a+field+8,8);
                    const uint64_t bytes=last-first;
                    if(last<first || bytes>8*1024*1024 || key.size()+bytes>8*1024*1024
                        || Overlap(result->first,outputBytes,reinterpret_cast<void*>(first),bytes)){valid=false;break;}
                    Add(key,&bytes,8);Add(key,reinterpret_cast<void*>(first),bytes);
                }
            }
            if(valid && Cache().Get(key,cached,outputBytes)){memcpy(result->first,cached.data(),outputBytes);return;}
        }
        if(!valid)key.clear();
    }catch(...){key.clear();}
    CalculateHeightModDetour(box,size,scale,offset,list,result);
    if(!key.empty())try{Add(cached,result->first,outputBytes);Cache().Put(key,cached);}catch(...){}
}
