// Lossless, fixed-address terrain storage. Windows 10 1803+ placeholders.
// Public mappings are made inaccessible BEFORE snapshotting through a private
// alias of the same section. A fault waits for that operation and restores the
// exact bytes at the original address. No engine reader can see a moving buffer.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#define LZ4_HEAPMODE 0
#include "vendor/lz4/lz4.c"

namespace TerrainPager {
constexpr size_t Samples=257*257, Bytes=Samples*2, Offset=32;
constexpr size_t PlaneBytes=(Samples+1)/2, EncodedBytes=PlaneBytes*4;
constexpr size_t SlotBytes=(Bytes+Offset+4095)&~size_t(4095);
constexpr unsigned MaxSlots=131072;
struct Packed {unsigned refs,bytes;char data[1];};
struct Slot {
    HANDLE section;
    Packed* packed;
    uint64_t touched;
    unsigned next;
    bool active;
    bool blocked;
    bool readOnly;
    bool viewMissing;
};
struct Stats {
    uint64_t live, resident, compressedBytes, compressedCommit, faults, evictions, failures;
    uint64_t encodes, reusedEvictions, writeFaults, sharedClones;
    uint64_t lastBulkAllocation;
};
using Alloc2=void*(WINAPI*)(HANDLE,void*,SIZE_T,ULONG,ULONG,void*,ULONG);
using Map3=void*(WINAPI*)(HANDLE,HANDLE,void*,ULONG64,SIZE_T,ULONG,ULONG,void*,ULONG);
using Unmap2=BOOL(WINAPI*)(HANDLE,void*,ULONG);
static Alloc2 alloc2;
static Map3 map3;
static Unmap2 unmap2;
static SRWLOCK lock=SRWLOCK_INIT;
static uint8_t* arena;
static Slot* slots;
static void* veh;
static unsigned allocated, freeHead=MaxSlots, cursor;
static Stats stats{};
static size_t budget=1024ull*1024*1024;
static uint64_t allocationWindow;
static unsigned windowAllocations;
static uint8_t* delta;
static char* packedScratch;
static bool ready=false;
static thread_local bool inFault=false;
// Fault code never calls the game, its allocator, logging, or STL containers.
// Metadata/compressed bytes use VirtualAlloc and are never themselves paged by
// this mechanism. Immutable compressed copies survive read-only restoration;
// VirtualFree releases them after their last owner writes or is destroyed.
struct Guard { Guard(){AcquireSRWLockExclusive(&lock);} ~Guard(){ReleaseSRWLockExclusive(&lock);} };
static uint8_t* Base(unsigned i){return arena+size_t(i)*SlotBytes;}
static bool Contains(const void* p) {
    return arena && uintptr_t(p)>=uintptr_t(arena) &&
        uintptr_t(p)-uintptr_t(arena)<size_t(MaxSlots)*SlotBytes;
}
static unsigned Index(const void* p){return unsigned((uintptr_t(p)-uintptr_t(arena))/SlotBytes);}
static size_t Committed(size_t n){return (n+4095)&~size_t(4095);}
static size_t PackedSize(unsigned bytes){return offsetof(Packed,data)+bytes;}
static void DropPacked(Slot& s) {
    if(!s.packed)return;
    auto p=s.packed;s.packed=nullptr;
    if(--p->refs==0) {
        stats.compressedBytes-=p->bytes;stats.compressedCommit-=Committed(PackedSize(p->bytes));
        VirtualFree(p,0,MEM_RELEASE);
    }
}
static void Encode(const uint16_t* src) {
    for(size_t y=0;y<257;++y) {
        uint16_t prev=0;
        for(size_t x=0;x<257;++x) {
            size_t i=y*257+x;uint16_t v=src[i],d=uint16_t(v-prev);prev=v;
            d=uint16_t((unsigned(d)<<1)^uint16_t(int16_t(d)>>15));
            for(unsigned plane=0;plane<4;++plane) {
                auto& b=delta[plane*PlaneBytes+i/2];uint8_t nibble=uint8_t((d>>(plane*4))&15);
                if(i&1)b|=uint8_t(nibble<<4);else b=nibble;
            }
        }
    }
}
static void Decode(uint16_t* dst) {
    for(size_t y=0;y<257;++y) {
        uint16_t prev=0;
        for(size_t x=0;x<257;++x) {
            size_t i=y*257+x;unsigned d=0,shift=(i&1)*4;
            for(unsigned plane=0;plane<4;++plane)d|=((delta[plane*PlaneBytes+i/2]>>shift)&15)<<(plane*4);
            d=(d>>1)^unsigned(-int(d&1));prev=uint16_t(prev+d);dst[i]=prev;
        }
    }
}
static bool Restore(unsigned i,bool writing=false) {
    auto& s=slots[i];
    if(s.section) {
        if(s.viewMissing || (writing&&s.readOnly)) {
            // A read-only section view cannot be upgraded to writable with
            // VirtualProtect. Remap the SAME backing section with write access;
            // its contents survive, and competing faults remain behind lock.
            if(!s.viewMissing && !unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER)) {
                ++stats.failures;return false;
            }
            s.viewMissing=true;
            DWORD protection=writing||!s.packed?PAGE_READWRITE:PAGE_READONLY;
            if(map3(s.section,GetCurrentProcess(),Base(i),0,SlotBytes,MEM_REPLACE_PLACEHOLDER,
                    protection,nullptr,0)!=Base(i)){++stats.failures;return false;}
            s.viewMissing=false;s.blocked=false;s.readOnly=protection==PAGE_READONLY;
            if(writing)DropPacked(s);return true;
        }
        if(!s.blocked && !(writing&&s.readOnly))return true;
        DWORD old;
        DWORD protection=writing||!s.packed?PAGE_READWRITE:PAGE_READONLY;
        if(!VirtualProtect(Base(i),SlotBytes,protection,&old)){++stats.failures;return false;}
        s.blocked=false;s.readOnly=protection==PAGE_READONLY;
        if(writing)DropPacked(s);return true;
    }
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(SlotBytes),nullptr);
    if(!section){++stats.failures;return false;}
    auto alias=static_cast<uint8_t*>(MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,SlotBytes));
    if(!alias){CloseHandle(section);++stats.failures;return false;}
    bool ok=true;
    if(s.packed) {
        ok=LZ4_decompress_safe(s.packed->data,reinterpret_cast<char*>(delta),s.packed->bytes,int(EncodedBytes))==EncodedBytes;
        if(ok)Decode(reinterpret_cast<uint16_t*>(alias+Offset));
    }
    // Match the stock MSVC large-allocation header. Normally only our destroy
    // and resize hooks consume this allocation; keeping the header aids audit.
    *reinterpret_cast<void**>(alias+Offset-8)=Base(i);
    DWORD protection=writing||!s.packed?PAGE_READWRITE:PAGE_READONLY;
    if(ok)ok=map3(section,GetCurrentProcess(),Base(i),0,SlotBytes,
                 MEM_REPLACE_PLACEHOLDER,protection,nullptr,0)==Base(i);
    UnmapViewOfFile(alias);
    if(!ok){CloseHandle(section);++stats.failures;return false;}
    s.section=section;s.blocked=false;s.readOnly=protection==PAGE_READONLY;
    s.touched=GetTickCount64();++stats.resident;
    if(writing)DropPacked(s);
    return true;
}
static LONG CALLBACK Fault(EXCEPTION_POINTERS* e) {
    auto r=e->ExceptionRecord;
    if(inFault || r->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || r->NumberParameters<2 ||
       r->ExceptionInformation[0]>1 || !Contains(reinterpret_cast<void*>(r->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_SEARCH;
    // The original last-error value belongs to the interrupted engine code.
    DWORD last=GetLastError();inFault=true;
    bool ok=false;
    {
        Guard g;
        unsigned i=Index(reinterpret_cast<void*>(r->ExceptionInformation[1]));
        if(i<allocated && slots[i].active) {
            bool writing=r->ExceptionInformation[0]==1;ok=Restore(i,writing);
            if(ok){++stats.faults;if(writing)++stats.writeFaults;slots[i].touched=GetTickCount64();}
        }
    }
    inFault=false;SetLastError(last);
    return ok?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;
}
static bool Init(size_t budgetBytes) {
    if(ready)return true;
    auto kernel=GetModuleHandleW(L"kernelbase.dll");
    alloc2=reinterpret_cast<Alloc2>(GetProcAddress(kernel,"VirtualAlloc2"));
    map3=reinterpret_cast<Map3>(GetProcAddress(kernel,"MapViewOfFile3"));
    unmap2=reinterpret_cast<Unmap2>(GetProcAddress(kernel,"UnmapViewOfFile2"));
    if(!alloc2||!map3||!unmap2)return false;
    arena=static_cast<uint8_t*>(alloc2(GetCurrentProcess(),nullptr,size_t(MaxSlots)*SlotBytes,
        MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,nullptr,0));
    slots=static_cast<Slot*>(VirtualAlloc(nullptr,sizeof(Slot)*MaxSlots,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    delta=static_cast<uint8_t*>(VirtualAlloc(nullptr,EncodedBytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    packedScratch=static_cast<char*>(VirtualAlloc(nullptr,LZ4_compressBound(int(EncodedBytes)),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(arena&&slots&&delta&&packedScratch)veh=AddVectoredExceptionHandler(1,Fault);
    ready=veh!=nullptr;budget=budgetBytes;
    // Failed initialization has no live game allocations. Release everything.
    if(!ready) {
        if(arena)VirtualFree(arena,0,MEM_RELEASE);
        if(slots)VirtualFree(slots,0,MEM_RELEASE);
        if(delta)VirtualFree(delta,0,MEM_RELEASE);
        if(packedScratch)VirtualFree(packedScratch,0,MEM_RELEASE);
        arena=nullptr;slots=nullptr;delta=nullptr;packedScratch=nullptr;
    }
    return ready;
}
// Caller holds lock. Slot addresses are recycled only after the engine has
// destroyed the owning shared vector; compressed blobs have separate refs.
static unsigned NewSlot() {
    unsigned i;
    if(freeHead!=MaxSlots){i=freeHead;freeHead=slots[i].next;}
    else {
        if(allocated==MaxSlots)return MaxSlots;
        i=allocated;
        // Split the front of the remaining placeholder. The final slot is
        // already an exact-size placeholder and requires no split.
        if(i+1<MaxSlots && !VirtualFree(Base(i),SlotBytes,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) {
            ++stats.failures;return MaxSlots;
        }
        ++allocated;
    }
    auto now=GetTickCount64();
    if(now-allocationWindow>=1000){allocationWindow=now;windowAllocations=0;}
    if(++windowAllocations>=1024)stats.lastBulkAllocation=now;
    slots[i]={};slots[i].active=true;slots[i].touched=now;++stats.live;
    return i;
}
static uint16_t* Allocate() {
    if(!ready)return nullptr;
    Guard g;unsigned i=NewSlot();if(i==MaxSlots)return nullptr;
    auto& s=slots[i];
    if(!Restore(i,true)){s.active=false;s.next=freeHead;freeHead=i;--stats.live;return nullptr;}
    return reinterpret_cast<uint16_t*>(Base(i)+Offset);
}
static uint16_t* Clone(const void* src) {
    if(!Contains(src))return nullptr;
    Guard g;unsigned source=Index(src);
    if(source>=allocated || src!=Base(source)+Offset || !slots[source].active || !slots[source].packed)return nullptr;
    unsigned i=NewSlot();if(i==MaxSlots)return nullptr;
    slots[i].packed=slots[source].packed;++slots[i].packed->refs;++stats.sharedClones;
    return reinterpret_cast<uint16_t*>(Base(i)+Offset);
}
static bool Release(void* p) {
    if(!Contains(p))return false;
    Guard g;unsigned i=Index(p);auto& s=slots[i];
    if(i>=allocated || p!=Base(i)+Offset || !s.active)return false;
    if(s.section) {
        if(!s.viewMissing && !unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER)) {
            // An owned pointer MUST NOT reach the game's heap free. Retain it
            // on OS failure; safe leak with diagnostics instead of wrong free.
            ++stats.failures;return true;
        }
        CloseHandle(s.section);--stats.resident;
    }
    DropPacked(s);
    s={};s.next=freeHead;freeHead=i;--stats.live;
    return true;
}
static bool Evict(unsigned i,bool force=false) {
    Guard g;
    if(i>=allocated)return false;
    auto& s=slots[i];
    if(!s.active||!s.section || (!force &&
       (stats.resident*SlotBytes<=budget || GetTickCount64()-s.touched<5000)))return false;
    if(s.packed) {
        // No writes since restoration: the read-only mapping guarantees this
        // immutable blob is still exact. Re-eviction needs no encoding or copy.
        DWORD old;
        if(!VirtualProtect(Base(i),SlotBytes,PAGE_NOACCESS,&old)){++stats.failures;return false;}
        s.blocked=true;
        if(unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER)) {
            CloseHandle(s.section);s.section=nullptr;--stats.resident;
            ++stats.evictions;++stats.reusedEvictions;return true;
        }
        DWORD ignored;if(VirtualProtect(Base(i),SlotBytes,old,&ignored))s.blocked=false;
        ++stats.failures;s.touched=GetTickCount64();return false;
    }
    // Alias does not fault and stays accessible after revoking the game view.
    auto alias=static_cast<uint8_t*>(MapViewOfFile(s.section,FILE_MAP_READ,0,0,SlotBytes));
    if(!alias){++stats.failures;s.touched=GetTickCount64();return false;}
    DWORD old=0;
    if(!VirtualProtect(Base(i),SlotBytes,PAGE_NOACCESS,&old)) {
        UnmapViewOfFile(alias);++stats.failures;s.touched=GetTickCount64();return false;
    }
    s.blocked=true;
    Encode(reinterpret_cast<const uint16_t*>(alias+Offset));
    ++stats.encodes;
    int count=LZ4_compress_default(reinterpret_cast<const char*>(delta),packedScratch,int(EncodedBytes),int(Bytes));
    Packed* compressed=nullptr;
    if(count>0 && count<int(Bytes*95/100)) {
        compressed=static_cast<Packed*>(VirtualAlloc(nullptr,PackedSize(count),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(!compressed)++stats.failures;
    }
    if(compressed){compressed->refs=1;compressed->bytes=count;memcpy(compressed->data,packedScratch,count);}
    bool ok=compressed && unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER);
    if(ok) {
        UnmapViewOfFile(alias);CloseHandle(s.section);s.section=nullptr;
        s.packed=compressed;stats.compressedBytes+=count;stats.compressedCommit+=Committed(PackedSize(count));
        --stats.resident;++stats.evictions;
    } else {
        if(compressed){VirtualFree(compressed,0,MEM_RELEASE);++stats.failures;}
        DWORD ignored;
        if(!VirtualProtect(Base(i),SlotBytes,old,&ignored)) {
            // Inaccessible but intact resident backing: Fault must retry the
            // protection change rather than swallowing an unresolvable fault.
            ++stats.failures;
        } else s.blocked=false;
        UnmapViewOfFile(alias);s.touched=GetTickCount64();
    }
    return ok;
}
static Stats Snapshot(){Guard g;return stats;}
static void SetBudget(size_t bytes){Guard g;budget=bytes;}
static void Tick(unsigned attempts=256) {
    for(unsigned n=0;n<attempts;++n) {
        unsigned i;
        {Guard g;if(!allocated||stats.resident*SlotBytes<=budget)return;
         if(cursor>=allocated)cursor=0;i=cursor++;}
        Evict(i);
    }
}
}
