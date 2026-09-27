// Linux userfaultfd backend for the shared Windows lossless terrain codec.
// No signal handlers. Writers are stopped by UFFD write protection while a
// tile is encoded; readers fault back to identical bytes at the same address.
#pragma once
#include "../src/terrain_codec.h"
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <memory>
#include <vector>
#include <unordered_map>
#include <cerrno>
#include <cstdlib>
// Stable Linux UAPI definitions absent from the soldier SDK headers.
#ifndef UFFDIO_WRITEPROTECT
struct uffdio_writeprotect { struct uffdio_range range; uint64_t mode; };
#define UFFDIO_WRITEPROTECT _IOWR(UFFDIO, 0x06, struct uffdio_writeprotect)
#define UFFDIO_WRITEPROTECT_MODE_WP (1ULL << 0)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFDIO_COPY_MODE_WP
#define UFFDIO_COPY_MODE_WP (1ULL << 1)
#endif
namespace linux_pager {
class TerrainPager {
public:
    static constexpr size_t Bytes=TerrainCodec::RawBytes, Stride=(Bytes+4095)&~size_t(4095);
    struct Stats {uint64_t live,resident,packed,faults,evictions,refusals,dedupHits;};
private:
    struct Blob {void* data;size_t size,mapped,refs;uint64_t hash;};
    struct Slot {std::mutex lock;bool active=false,cold=false,zero=false;Blob* packed=nullptr;uint64_t touched=0;};
    std::mutex blobLock;std::unordered_multimap<uint64_t,Blob*> blobs;bool dedup=false,lazyZero=true;
    std::atomic<uint64_t> dedupHits{0};
    int fd=-1;uint8_t* base=nullptr;size_t count=0;unsigned next=0,cursor=0;std::atomic<unsigned> highWater{0};
    std::unique_ptr<Slot[]> slots;std::mutex poolLock;std::vector<unsigned> free;
    std::thread handler,policy;std::atomic<bool> stop{false};
    std::atomic<uint64_t> live{0},resident{0},packedBytes{0},faults{0},evictions{0},refusals{0};
    std::atomic<size_t> budget{0};
    static uint64_t Now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    uint8_t* Address(size_t i){return base+i*Stride;}
    static void Fatal(){ssize_t ignored=write(2,"tpf2_bigmap: terrain pager invariant failed\n",42);(void)ignored;abort();}
    void Check(bool ok){if(!ok)Fatal();}
    void Protect(unsigned i,bool on){uffdio_writeprotect wp{};wp.range={uintptr_t(Address(i)),Stride};wp.mode=on?UFFDIO_WRITEPROTECT_MODE_WP:0;Check(ioctl(fd,UFFDIO_WRITEPROTECT,&wp)==0);}
    void Wake(unsigned i){uffdio_range range{uintptr_t(Address(i)),Stride};Check(ioctl(fd,UFFDIO_WAKE,&range)==0);}
    // Lock order is always slot -> blob index. The index never owns a blob.
    void DropBlob(Slot& s){
        if(!s.packed)return;
        std::lock_guard<std::mutex> lock(blobLock);auto* b=s.packed;s.packed=nullptr;
        if(--b->refs)return;
        auto range=blobs.equal_range(b->hash);
        for(auto it=range.first;it!=range.second;++it)if(it->second==b){blobs.erase(it);break;}
        packedBytes-=b->mapped;munmap(b->data,b->mapped);delete b;
    }
    void Serve(){
        auto scratch=std::unique_ptr<TerrainCodec::DecodeScratch>(new TerrainCodec::DecodeScratch);
        void* decoded=mmap(nullptr,Stride,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);Check(decoded!=MAP_FAILED);
        pollfd p{fd,POLLIN,0};
        while(!stop){
            int r=poll(&p,1,100);if(r<0 && errno==EINTR)continue;Check(r>=0 && !(p.revents&(POLLERR|POLLHUP|POLLNVAL)));if(!r)continue;
            uffd_msg msg{};ssize_t n=read(fd,&msg,sizeof(msg));if(n<0 && (errno==EAGAIN || errno==EINTR))continue;
            Check(n==sizeof(msg) && msg.event==UFFD_EVENT_PAGEFAULT);
            unsigned i=unsigned((msg.arg.pagefault.address-uintptr_t(base))/Stride);Check(i<count);
            auto& s=slots[i];std::lock_guard<std::mutex> lock(s.lock);Check(s.active);++faults;
            if(s.cold){
                if(s.zero)memset(decoded,0,Stride);
                else Check(TerrainCodec::Decode(static_cast<uint8_t*>(s.packed->data),s.packed->size,static_cast<uint16_t*>(decoded),*scratch));
                uffdio_copy copy{};copy.src=uintptr_t(decoded);copy.dst=uintptr_t(Address(i));copy.len=Stride;copy.mode=UFFDIO_COPY_MODE_DONTWAKE|UFFDIO_COPY_MODE_WP;
                Check(ioctl(fd,UFFDIO_COPY,&copy)==0 && copy.copy==ssize_t(Stride));
                s.cold=false;s.zero=false;++resident;Protect(i,false);DropBlob(s);
            }
            s.touched=Now();Wake(i);
        }
        munmap(decoded,Stride);
    }
    void Policy(){
        auto scratch=std::unique_ptr<TerrainCodec::EncodeScratch>(new TerrainCodec::EncodeScratch);
        auto encoded=std::unique_ptr<uint8_t[]>(new uint8_t[Bytes]);
        auto decodeScratch=std::unique_ptr<TerrainCodec::DecodeScratch>(new TerrainCodec::DecodeScratch);
        auto compare=std::unique_ptr<uint16_t[]>(new uint16_t[TerrainCodec::Samples]);
        while(!stop){
            for(unsigned work=0;work<256 && resident*Stride>budget;++work){
                unsigned i=cursor++%std::max(1u,highWater.load());auto& s=slots[i];std::unique_lock<std::mutex> lock(s.lock,std::try_to_lock);
                if(!lock || !s.active || s.cold || Now()-s.touched<2000)continue;
                Protect(i,true);
                auto* raw=reinterpret_cast<uint16_t*>(Address(i));
                uint64_t hash=TerrainCodec::Hash(raw);
                if(dedup){
                    std::lock_guard<std::mutex> blobGuard(blobLock);
                    auto range=blobs.equal_range(hash);
                    for(auto it=range.first;it!=range.second;++it){
                        auto* b=it->second;
                        Check(TerrainCodec::Decode(static_cast<uint8_t*>(b->data),b->size,compare.get(),*decodeScratch));
                        // Full equality, not hash equality, authorizes sharing.
                        if(!memcmp(raw,compare.get(),Bytes)){s.packed=b;++b->refs;++dedupHits;break;}
                    }
                }
                if(s.packed){
                    Check(madvise(Address(i),Stride,MADV_DONTNEED)==0);s.cold=true;--resident;++evictions;continue;
                }
                size_t n=TerrainCodec::Encode(reinterpret_cast<uint16_t*>(Address(i)),encoded.get(),Bytes*95/100,*scratch);
                if(!n){Protect(i,false);s.touched=Now();++refusals;continue;}
                size_t len=(n+4095)&~size_t(4095);void* blob=mmap(nullptr,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                if(blob==MAP_FAILED){Protect(i,false);s.touched=Now();++refusals;continue;}
                memcpy(blob,encoded.get(),n);
                {std::lock_guard<std::mutex> blobGuard(blobLock);
                    s.packed=new(std::nothrow) Blob{blob,n,len,1,hash};
                    if(!s.packed){munmap(blob,len);Protect(i,false);s.touched=Now();++refusals;continue;}
                    if(dedup){
                        try {blobs.emplace(hash,s.packed);}
                        catch(const std::bad_alloc&){delete s.packed;s.packed=nullptr;munmap(blob,len);Protect(i,false);s.touched=Now();++refusals;continue;}
                    }
                    packedBytes+=len;
                }
                Check(madvise(Address(i),Stride,MADV_DONTNEED)==0);s.cold=true;--resident;++evictions;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
public:
    bool Start(size_t capacity,size_t hotBytes,bool contentDedup=false,bool lazy=true){
        if(!capacity || capacity>(1u<<20) || fd>=0)return false;
        dedup=contentDedup;lazyZero=lazy;
        fd=int(syscall(SYS_userfaultfd,O_CLOEXEC|O_NONBLOCK|UFFD_USER_MODE_ONLY));if(fd<0)return false;
        uffdio_api api{};api.api=UFFD_API;api.features=UFFD_FEATURE_PAGEFAULT_FLAG_WP;
        if(ioctl(fd,UFFDIO_API,&api)){close(fd);fd=-1;return false;}
        base=static_cast<uint8_t*>(mmap(nullptr,capacity*Stride,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));
        if(base==MAP_FAILED){base=nullptr;close(fd);fd=-1;return false;}
        uffdio_register reg{};reg.range={uintptr_t(base),capacity*Stride};reg.mode=UFFDIO_REGISTER_MODE_MISSING|UFFDIO_REGISTER_MODE_WP;
        if(ioctl(fd,UFFDIO_REGISTER,&reg)){munmap(base,capacity*Stride);base=nullptr;close(fd);fd=-1;return false;}
        count=capacity;budget=hotBytes;slots.reset(new Slot[count]);
        handler=std::thread([this]{Serve();});policy=std::thread([this]{Policy();});return true;
    }
    bool Contains(const void* p)const{return base && uintptr_t(p)>=uintptr_t(base) && uintptr_t(p)<uintptr_t(base)+count*Stride;}
    uint16_t* Allocate(){
        unsigned i;
        {std::lock_guard<std::mutex> lock(poolLock);if(!free.empty()){i=free.back();free.pop_back();}else if(next<count){i=next++;highWater=next;}else{++refusals;return nullptr;}}
        auto& s=slots[i];std::lock_guard<std::mutex> lock(s.lock);Check(!s.active);s.active=true;s.cold=true;s.zero=true;s.touched=Now();
        // Leave the registered pages missing. The first read or write restores
        // a whole zero tile through the same serialized fault path as a blob.
        if(!lazyZero){
            uffdio_zeropage zero{};zero.range={uintptr_t(Address(i)),Stride};
            Check(ioctl(fd,UFFDIO_ZEROPAGE,&zero)==0 && zero.zeropage==ssize_t(Stride));
            s.cold=false;s.zero=false;++resident;
        }
        ++live;return reinterpret_cast<uint16_t*>(Address(i));
    }
    bool Release(void* p){
        if(!Contains(p))return false;
        unsigned i=unsigned((uintptr_t(p)-uintptr_t(base))/Stride);Check(p==Address(i));
        {auto& s=slots[i];std::lock_guard<std::mutex> lock(s.lock);Check(s.active);s.active=false;--live;if(!s.cold)--resident;DropBlob(s);Check(madvise(Address(i),Stride,MADV_DONTNEED)==0);}
        std::lock_guard<std::mutex> lock(poolLock);free.push_back(i);return true;
    }
    struct ProbeResult {uint64_t hashed=0,skipped=0,distinct=0,duplicates=0,zero=0,pairs=0,largest=0;};
    // Diagnostic hash groups, not a coherent whole-world snapshot. Protect
    // resident pages while hashing; cold/lazy slots never fault back in.
    ProbeResult Probe(){
        ProbeResult r;std::unordered_map<uint64_t,uint64_t> groups;
        auto zeros=std::unique_ptr<uint16_t[]>(new uint16_t[TerrainCodec::Samples]{});
        const uint64_t zeroHash=TerrainCodec::Hash(zeros.get());
        for(unsigned i=0,limit=highWater.load();i<limit;++i){
            auto& s=slots[i];std::unique_lock<std::mutex> lock(s.lock,std::try_to_lock);
            if(!lock){++r.skipped;continue;}
            if(!s.active)continue;
            uint64_t hash;
            if(s.cold)hash=s.zero?zeroHash:s.packed->hash;
            else {Protect(i,true);hash=TerrainCodec::Hash(reinterpret_cast<uint16_t*>(Address(i)));Protect(i,false);}
            ++groups[hash];++r.hashed;if(hash==zeroHash)++r.zero;
        }
        r.distinct=groups.size();
        for(auto& group:groups){r.duplicates+=group.second-1;r.pairs+=group.second==2;r.largest=std::max(r.largest,group.second);}
        return r;
    }
    Stats Get()const{return {live.load(),resident.load(),packedBytes.load(),faults.load(),evictions.load(),refusals.load(),dedupHits.load()};}
    ~TerrainPager(){stop=true;if(policy.joinable())policy.join();if(handler.joinable())handler.join();if(fd>=0)close(fd);if(base)munmap(base,count*Stride);if(slots)for(size_t i=0;i<count;++i)DropBlob(slots[i]);}
};
}
