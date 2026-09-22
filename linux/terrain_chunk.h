#pragma once
#include "terrain_chunk_store.h"
#include "terrain_align.h"

namespace terrain_chunk {
using Worker=void(*)(void*,int,int);
using ReadBase=void(*)(void*,uint64_t,uint64_t,AlignU16Vector*);
using Append=void(*)(AlignU16Vector*,size_t);
inline Worker original=nullptr;
inline ReadBase readBase=nullptr;
inline Append append=nullptr;
inline bool verify=true;
inline std::atomic<bool> poisoned{false};
inline std::atomic<uint64_t> calls{0},refused{0},checked{0},mismatches{0};
template<class T> T Field(const void* p,size_t off) {T v;memcpy(&v,static_cast<const uint8_t*>(p)+off,sizeof(v));return v;}
inline uint64_t Pair(int32_t x,int32_t y){return uint32_t(x)|(uint64_t(uint32_t(y))<<32);}

// 35924's 0x173b6e0 worker consumes 88-byte records. Key the whole finished
// 65x65 block to the 19x19 base samples (including the interpolation halo),
// both coordinate systems, terrain scale, floating controls and the ordered
// alignment geometry/weights. Reading the small base rectangle uses the same
// engine sampler as 0xdb5fa0, including edge clamping. No save-name shortcut.
inline bool Key(void* context,uint8_t* block,Bytes& key) {
    void* system=Field<void*>(context,0);
    void* terrain=Field<void*>(system,8);
    const int low=Field<int>(terrain,0x28),high=Field<int>(terrain,0x38);
    const int x0=Field<int>(block,0x30),y0=Field<int>(block,0x34);
    const int x1=Field<int>(block,0x38),y1=Field<int>(block,0x3c);
    const int nx=Field<int>(block,0x28),ny=Field<int>(block,0x2c);
    if(low!=6 || high!=8 || nx!=65 || ny!=65 || x0<0 || y0<0 || x1>256 || y1>256
        || x1-x0!=64 || y1-y0!=64 || (x0&3) || (y0&3))return false;
    const auto* list=reinterpret_cast<const AlignPointerVector*>(block+8);
    if(!AlignListSupported(list))return false;
    const int32_t tx=Field<int32_t>(block,0),ty=Field<int32_t>(block,4);
    const int64_t sx=int64_t(tx)*64+x0/4-1,sy=int64_t(ty)*64+y0/4-1;
    if(sx<INT32_MIN || sx>INT32_MAX || sy<INT32_MIN || sy>INT32_MAX)return false;
    const uint32_t prefix[]={1,35924,_mm_getcsr()&0xffc0};
    terrain_cache::Add(key,prefix,sizeof(prefix));
    terrain_cache::Add(key,static_cast<uint8_t*>(terrain)+0x28,24);
    terrain_cache::Add(key,static_cast<uint8_t*>(context)+8,8);
    terrain_cache::Add(key,block,8);
    terrain_cache::Add(key,block+0x20,32);
    const uint64_t count=list->last-list->first;
    terrain_cache::Add(key,&count,sizeof(count));
    for(auto it=list->first;it!=list->last;++it) {
        terrain_cache::Add(key,*it+0x30,4);
        for(size_t off:{size_t(0),size_t(0x18)}) {
            const auto first=Field<uintptr_t>(*it,off),last=Field<uintptr_t>(*it,off+8);
            if(last<first || last-first>1024*1024 || key.size()+last-first>1024*1024)return false;
            const uint64_t n=last-first;terrain_cache::Add(key,&n,8);
            terrain_cache::Add(key,reinterpret_cast<void*>(first),size_t(n));
        }
    }
    uint16_t samples[19*19]{};
    AlignU16Vector v{samples,samples+19*19,samples+19*19};
    readBase(terrain,Pair(int32_t(sx),int32_t(sy)),Pair(19,19),&v);
    terrain_cache::Add(key,samples,sizeof(samples));
    return true;
}
inline void Detour(void* context,int begin,int end) {
    if(poisoned || begin<0 || end<begin || end-begin>1000000){original(context,begin,end);return;}
    for(int i=begin;i<end;++i) {
        ++calls;
        auto* block=Field<uint8_t*>(Field<void*>(context,16),0)+size_t(i)*88;
        Bytes key,cached;bool valid=false,hit=false;
        try {valid=Key(context,block,key);if(valid)hit=Cache().Get(key,cached);}catch(...){valid=false;}
        auto* output=reinterpret_cast<AlignU16Vector*>(block+0x40);
        if(hit && !verify && cached.size()==65*65*2) {
            const size_t count=(uintptr_t(output->last)-uintptr_t(output->first))/2;
            if(count<65*65)append(output,65*65-count);
            output->last=output->first+65*65;
            memcpy(output->first,cached.data(),cached.size());
            continue;
        }
        // Keep engine exceptions outside our allocation-failure fallback.
        original(context,i,i+1);
        if(!valid){++refused;continue;}
        if(uintptr_t(output->last)-uintptr_t(output->first)!=65*65*2){++refused;continue;}
        if(hit) {
            ++checked;
            if(cached.size()!=65*65*2 || memcmp(output->first,cached.data(),cached.size())) {
                ++mismatches;poisoned=true;
            }
        } else try {
            Bytes result(65*65*2);memcpy(result.data(),output->first,result.size());Cache().Put(key,result);
        }catch(...){}
    }
}
}
