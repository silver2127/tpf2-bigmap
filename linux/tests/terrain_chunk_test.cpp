#include "../terrain_chunk.h"
#include <cassert>
#include <thread>
#include <iostream>
using namespace terrain_chunk;
static uint16_t height=20;
static uint64_t origin,dimensions;
static unsigned computations=0;
static void Compute(void* context,int begin,int end) {
    auto* blocks=Field<uint8_t*>(Field<void*>(context,16),0);
    for(int i=begin;i<end;++i) {
        ++computations;
        auto* v=reinterpret_cast<AlignU16Vector*>(blocks+88*i+0x40);
        v->last=v->first+65*65;
        for(auto* p=v->first;p!=v->last;++p)*p=height;
    }
}
static void Grow(AlignU16Vector* v,size_t n){assert(v->last+n<=v->end);v->last+=n;}
static void Sample(void*,uint64_t xy,uint64_t size,AlignU16Vector* out) {
    origin=xy;dimensions=size;
    for(auto* p=out->first;p!=out->last;++p)*p=height;
}
template<class T> void Set(void* p,size_t off,T v){memcpy(static_cast<uint8_t*>(p)+off,&v,sizeof(v));}
int main() {
    (void)&CalculateHeightModDetour;
    Store store;store.limit=1<<20;
    Bytes key{1,2},other{1,3},out{4,5},got;
    store.Put(key,out);assert(store.Get(key,got)&&got==out);assert(!store.Get(other,got));
    const auto used=store.used.load();store.Put(key,Bytes{9});assert(store.used==used);
    std::vector<std::thread> workers;
    for(int i=0;i<8;++i)workers.emplace_back([&]{for(int j=0;j<100;++j){store.Put(other,out);Bytes v;assert(store.Get(other,v)&&v==out);}});
    for(auto& t:workers)t.join();assert(store.writes==2);
    Store small;small.limit=260;small.Put(key,out);small.Put(other,out);
    assert(small.used<=small.limit&&small.writes==1&&small.full==1);
    alignas(16) uint8_t terrain[80]{},system[32]{},context[24]{},block[88]{};
    Set(system,8,static_cast<void*>(terrain));Set(context,0,static_cast<void*>(system));
    Set(terrain,0x28,6);Set(terrain,0x38,8);Set(block,0,2);Set(block,4,-3);
    Set(block,0x28,65);Set(block,0x2c,65);Set(block,0x38,64);Set(block,0x3c,64);
    readBase=Sample;Bytes a,b;
    assert(Key(context,block,a));assert(origin==Pair(127,-193)&&dimensions==Pair(19,19));
    assert(Key(context,block,b)&&a==b);
    ++height;b.clear();assert(Key(context,block,b)&&a!=b);--height;
    Set(context,8,1.f);b.clear();assert(Key(context,block,b)&&a!=b);Set(context,8,0.f);
    Set(block,0x20,1);b.clear();assert(Key(context,block,b)&&a!=b);Set(block,0x20,0);
    Set(terrain,0x2c,1.f);b.clear();assert(Key(context,block,b)&&a!=b);Set(terrain,0x2c,0.f);
    Set(block,0x28,66);b.clear();assert(!Key(context,block,b));Set(block,0x28,65);
    alignas(16) uint8_t alignment[0x38]{};const uint8_t* items[]={alignment};
    Set(block,8,items);Set(block,16,items+1);Set(block,24,items+1);
    b.clear();assert(Key(context,block,b)&&a!=b);a=b;
    Set(alignment,0x30,1);b.clear();assert(Key(context,block,b)&&a!=b);
    float triangle[9]{},weights[3]{1,1,1};
    Set(alignment,0,triangle);Set(alignment,8,triangle+9);Set(alignment,16,triangle+9);
    Set(alignment,24,weights);Set(alignment,32,weights+3);Set(alignment,40,weights+3);
    a.clear();assert(Key(context,block,a));
    triangle[2]=4;b.clear();assert(Key(context,block,b)&&a!=b);triangle[2]=0;
    weights[1]=.5f;b.clear();assert(Key(context,block,b)&&a!=b);weights[1]=1;
    const auto control=_mm_getcsr();_mm_setcsr(control^0x2000);
    b.clear();assert(Key(context,block,b)&&a!=b);_mm_setcsr(control);
    uint16_t result[65*65]{};AlignU16Vector v{result,result,result+65*65};
    Set(block,0x40,v);void* blocks=block;Set(context,16,&blocks);
    original=Compute;append=Grow;Cache().limit=1<<20;verify=false;
    Detour(context,0,1);assert(computations==1&&result[4000]==height);
    memset(result,0,sizeof(result));Set(block,0x48,result);
    Detour(context,0,1);assert(computations==1&&result[4000]==height);
    ++height;Detour(context,0,1);assert(computations==2&&result[4000]==height);
    verify=true;Detour(context,0,1);assert(computations==3&&checked==1&&mismatches==0);
    std::cout<<"chunk exact keys, budget and concurrent lookup passed\n";
}
