#include "../bigmap_linux.cpp"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <cstdarg>
static std::map<std::string,int> config;
static std::map<uintptr_t,std::vector<uint8_t>> memory;
static int writes, failWrite, mismatch;
static bool build=true;
static int Int(const char*,const char* key,int fallback){auto it=config.find(key);return it==config.end()?fallback:it->second;}
static const char* Str(const char*,const char*,const char* fallback){return fallback;}
static uintptr_t Base(){return 0x40000000;}
static int Build(){return build;}
static void Log(const char*,...){}
static int Verify(uintptr_t rva,const uint8_t* bytes,uint32_t n){
    if(mismatch)return 0;
    memory[rva]=std::vector<uint8_t>(bytes,bytes+n);return 1;
}
static int PatchBytes(uintptr_t rva,const uint8_t* bytes,uint32_t n){
    if(++writes==failWrite)return 0;
    memory[rva]=std::vector<uint8_t>(bytes,bytes+n);return 1;
}
static uint64_t Stock(int size,int format,void*){assert(size<7 && format<5);return Pack(96,96);}
static int Hook(uintptr_t,void*,int n,void** out){assert(n==18);++writes;*out=reinterpret_cast<void*>(Stock);return 1;}
static const char* Data(){return "/tmp/";}
static Tpf2mpHost host={sizeof(host),1,Log,Int,Int,Str,Base,Build,Verify,Hook,PatchBytes,Data};
static void Reset(){config.clear();memory.clear();writes=failWrite=mismatch=0;rows=claimCount=patchCount=0;stockRows=7;build=true;}
int main(){
    Tpf2mpPluginInfo info{};
    assert(Tpf2mpPluginInit(nullptr,&info)==TPF2MP_ERR_ABI);
    Reset();build=false;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_BUILD && writes==0);
    Reset();config["enabled"]=0;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_DISABLED && writes==0);
    Reset();config["octree_depth"]=13;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_FAILED && writes==0);
    Reset();mismatch=1;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_BUILD && writes==0);
    Reset();assert(Tpf2mpPluginInit(&host,&info)==0 && rows==9 && cap==512);
    assert(Size(6,0,nullptr)==Pack(96,96));
    assert(Size(7,0,nullptr)==Pack(128,128));
    assert(Size(15,0,nullptr)==Pack(510,510));
    stockRows=4;assert(Size(4,0,nullptr)==Pack(128,128));
    for(int side:{96,128,224,512})for(int f=0;f<20;++f){
        uint64_t v=Shape(side,f);int x=uint32_t(v),y=uint32_t(v>>32);
        assert(x>=2 && y<=512 && x%2==0 && y%2==0 && y==x*(f+1) && Fits(x,y));
    }
    int x,y;assert(Parse(" 224 x 320 ",x,y) && x==224 && y==320);
    for(const char* bad:{"", "2x", "3x4junk", "-4x6", "999999999999999999999x6"})assert(!Parse(bad,x,y));
    x=INT_MAX;y=INT_MAX;Bound(x,y);assert(x==510 && y==512);
    assert(RasterCell(24576,24576,1)==1);
    assert(RasterCell(57344,57344,1)==2);
    float c=RasterCell(131072,131072,1);assert(c==4 && std::pow(std::floor(131072/c)+1,2)<=cellBudget);
    String text;RatioText(&text,19);assert(text.data==text.local && text.size==4 && !strcmp(text.data,"1:20"));
    claims[0]={7,1,128,256};claimCount=1;assert(Size(4,1,nullptr)==Pack(128,256));
    Reset();config["street_raster"]=0;assert(Tpf2mpPluginInit(&host,&info)==0 && cap==180 && rows==2);
    Reset();config["octree"]=0;assert(Tpf2mpPluginInit(&host,&info)==0 && cap==256 && rows==5);
    Reset();failWrite=4;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_FAILED);
    assert(memory[SizeRva]==std::vector<uint8_t>(SizeBytes,SizeBytes+sizeof(SizeBytes)));
    assert(memory[OctreeSite]==std::vector<uint8_t>(OctreeBytes,OctreeBytes+sizeof(OctreeBytes)));
    puts("PASS: map sizing, ratios, raster overflow, guards, safe caps and rollback");
}
