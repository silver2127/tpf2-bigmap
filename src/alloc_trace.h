// alloc_trace.h -- DIAGNOSTIC: who allocates the game's memory (alloc_trace=1).
//
// Redirects the executable's own CRT heap imports (malloc, calloc, realloc,
// free in its import table; no code bytes change) to counters that sample one
// call stack per MiB allocated, byte-weighted. Each sampled block stays in a
// small lock-free table until it is freed, so the log shows, every 20 s, the
// stacks holding the most LIVE sampled memory and those that allocated the most
// since the last report. Frames are printed as exe+RVA (GOG or Steam, whichever
// runs) and module+offset. Allocations made by other DLLs through their own
// imports are not seen. Off by default; costs a few percent while on.
#pragma once
#include <intrin.h>

static int g_allocTrace = 0;

namespace AllocTrace {
using MallocFn = void* (__cdecl*)(size_t);
using CallocFn = void* (__cdecl*)(size_t, size_t);
using ReallocFn = void* (__cdecl*)(void*, size_t);
using FreeFn = void (__cdecl*)(void*);
static MallocFn realMalloc = nullptr;
static CallocFn realCalloc = nullptr;
static ReallocFn realRealloc = nullptr;
static FreeFn realFree = nullptr;

constexpr size_t kSampleBytes = size_t(1) << 20;   // one stack per MiB allocated
constexpr int kFrames = 20;
constexpr unsigned kStacks = 65536;                 // distinct stacks kept
constexpr unsigned kIndex = 1u << 18;               // hash index over them (power of two)
constexpr unsigned kLive = 1u << 17;                // sampled blocks tracked (power of two)

struct Stack { volatile LONG64 liveBytes, allocBytes, lastAllocBytes; ULONG hash; USHORT n; void* frames[kFrames]; };
static Stack* g_stacks = nullptr;                   // private heap, never freed
static volatile LONG g_stackCount = 0;
static LONG* g_index = nullptr;                     // stack hash -> stack number + 1 (0 = empty); under the insert lock
struct Live { void* volatile ptr; LONG stack; LONG64 bytes; };
static Live* g_live = nullptr;
static volatile LONG64 g_untilSample = kSampleBytes;
static volatile LONG64 g_totalAlloc = 0, g_dropped = 0;
static volatile LONG g_inHook = 0;

static unsigned LiveSlot(void* p) { return unsigned((uintptr_t(p) >> 4) * 0x9E3779B97F4A7C15ull >> 47) & (kLive - 1); }

static int StackFor(void** frames, USHORT n, ULONG hash) {   // caller holds the insert lock
    unsigned slot = (hash * 2654435761u) & (kIndex - 1);
    for (unsigned k = 0; k < kIndex; ++k, slot = (slot + 1) & (kIndex - 1)) {
        LONG e = g_index[slot];
        if (!e) break;
        const Stack& s = g_stacks[e - 1];
        if (s.hash == hash && s.n == n && !memcmp(s.frames, frames, n * sizeof(void*))) return int(e - 1);
    }
    LONG i = g_stackCount;
    if (i >= LONG(kStacks)) return -1;
    Stack& s = g_stacks[i];
    memcpy(s.frames, frames, n * sizeof(void*)); s.n = n; s.hash = hash;
    MemoryBarrier();
    g_index[slot] = i + 1;
    InterlockedIncrement(&g_stackCount);
    return int(i);
}

static void Record(void* p, size_t n) {
    if (!p || !n) return;
    InterlockedAdd64(&g_totalAlloc, LONG64(n));
    LONG64 left = InterlockedAdd64(&g_untilSample, -LONG64(n));
    if (left > 0) return;
    // byte-weighted: this block stands for the MiB(s) that ended here
    LONG64 weight = LONG64(kSampleBytes) + (-left / LONG64(kSampleBytes)) * LONG64(kSampleBytes);
    InterlockedAdd64(&g_untilSample, weight);
    void* frames[kFrames]; ULONG hash = 0;
    USHORT got = RtlCaptureStackBackTrace(2, kFrames, frames, &hash);
    static SRWLOCK lock = SRWLOCK_INIT;   // serialises stack-table inserts only (once per MiB)
    AcquireSRWLockExclusive(&lock);
    int s = StackFor(frames, got, hash);
    ReleaseSRWLockExclusive(&lock);
    if (s < 0) { InterlockedIncrement64(&g_dropped); return; }
    InterlockedAdd64(&g_stacks[s].allocBytes, weight);
    InterlockedAdd64(&g_stacks[s].liveBytes, weight);
    unsigned slot = LiveSlot(p);
    for (unsigned k = 0; k < 32; ++k, slot = (slot + 1) & (kLive - 1)) {
        if (InterlockedCompareExchangePointer(&g_live[slot].ptr, p, nullptr) == nullptr) {
            g_live[slot].stack = s; g_live[slot].bytes = weight; return;
        }
    }
    InterlockedAdd64(&g_stacks[s].liveBytes, -weight);   // table full here: count it as allocated only
    InterlockedIncrement64(&g_dropped);
}
static void Forget(void* p) {
    if (!p) return;
    unsigned slot = LiveSlot(p);
    for (unsigned k = 0; k < 32; ++k, slot = (slot + 1) & (kLive - 1)) {
        void* q = g_live[slot].ptr;
        if (q == p) {
            LONG s = g_live[slot].stack; LONG64 b = g_live[slot].bytes;
            if (InterlockedCompareExchangePointer(&g_live[slot].ptr, nullptr, p) == p && s >= 0)
                InterlockedAdd64(&g_stacks[s].liveBytes, -b);
            return;
        }
        if (!q) return;
    }
}
static void* __cdecl Malloc(size_t n) { void* p = realMalloc(n); Record(p, n); return p; }
static void* __cdecl Calloc(size_t c, size_t n) { void* p = realCalloc(c, n); Record(p, c * n); return p; }
static void* __cdecl Realloc(void* old, size_t n) {
    void* p = realRealloc(old, n);
    if (p || !n) Forget(old);
    Record(p, n);
    return p;
}
static void __cdecl Free(void* p) { Forget(p); realFree(p); }

static void Describe(void* a, char* out, size_t cap) {
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char path[MAX_PATH]; DWORD len = GetModuleFileNameA(m, path, MAX_PATH);
        const char* name = path; for (DWORD i = 0; i < len; ++i) if (path[i] == '\\' || path[i] == '/') name = path + i + 1;
        if (m == GetModuleHandleA(nullptr)) snprintf(out, cap, "exe+0x%llx", (unsigned long long)(uintptr_t(a) - uintptr_t(m)));
        else snprintf(out, cap, "%s+0x%llx", name, (unsigned long long)(uintptr_t(a) - uintptr_t(m)));
    } else snprintf(out, cap, "%p", a);
}
static void Report() {
    LONG count = g_stackCount; if (count > LONG(kStacks)) count = kStacks;
    int byLive[12], byNew[12]; LONG64 live[12], fresh[12];
    for (int i = 0; i < 12; ++i) { byLive[i] = byNew[i] = -1; live[i] = fresh[i] = 0; }
    LONG64 sumLive = 0;
    for (LONG i = 0; i < count; ++i) {
        LONG64 l = g_stacks[i].liveBytes, a = g_stacks[i].allocBytes, d = a - g_stacks[i].lastAllocBytes;
        g_stacks[i].lastAllocBytes = a; sumLive += l;
        for (int k = 0; k < 12; ++k) if (l > live[k]) { memmove(live + k + 1, live + k, (11 - k) * 8); memmove(byLive + k + 1, byLive + k, (11 - k) * 4); live[k] = l; byLive[k] = int(i); break; }
        for (int k = 0; k < 12; ++k) if (d > fresh[k]) { memmove(fresh + k + 1, fresh + k, (11 - k) * 8); memmove(byNew + k + 1, byNew + k, (11 - k) * 4); fresh[k] = d; byNew[k] = int(i); break; }
    }
    H->log("alloc trace: %lld MiB allocated in total, %lld MiB live in sampled stacks, %ld stacks, %lld dropped",
           (long long)(g_totalAlloc >> 20), (long long)(sumLive >> 20), count, (long long)g_dropped);
    for (int pass = 0; pass < 2; ++pass) {
        const int* idx = pass ? byNew : byLive; const LONG64* val = pass ? fresh : live;
        for (int k = 0; k < (pass ? 6 : 10) && idx[k] >= 0; ++k) {
            const Stack& s = g_stacks[idx[k]];
            char line[1600]; int w = snprintf(line, sizeof line, "alloc trace %s #%d %lld MiB:", pass ? "new(20s)" : "live", k + 1, (long long)(val[k] >> 20));
            for (USHORT f = 0; f < s.n && w < int(sizeof line) - 64; ++f) { char d[96]; Describe(s.frames[f], d, sizeof d); w += snprintf(line + w, sizeof line - w, " %s", d); }
            H->log("%s", line);
        }
    }
}
static DWORD WINAPI Reporter(void*) { for (;;) { Sleep(20000); Report(); } }

// The executable's IAT slot for one CRT heap function, by import name.
static void** IatSlot(const char* want) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        const char* dll = reinterpret_cast<const char*>(base + d->Name);
        if (_strnicmp(dll, "api-ms-win-crt-heap", 19) && _stricmp(dll, "ucrtbase.dll")) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!strcmp(reinterpret_cast<const char*>(ibn->Name), want)) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}
static bool Swap(void** slot, void* hook, void** original) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = *slot; MemoryBarrier(); InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}
}  // namespace AllocTrace

static bool InstallAllocTrace() {
    using namespace AllocTrace;
    if (!g_allocTrace) return false;
    void** sm = IatSlot("malloc"); void** sc = IatSlot("calloc"); void** sr = IatSlot("realloc"); void** sf = IatSlot("free");
    if (!sm || !sr || !sf) { H->log("alloc trace: CRT heap imports not found (malloc %p calloc %p realloc %p free %p); OFF", sm, sc, sr, sf); return false; }
    HANDLE heap = HeapCreate(0, 0, 0);
    g_stacks = heap ? static_cast<Stack*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Stack) * kStacks)) : nullptr;
    g_live = heap ? static_cast<Live*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Live) * kLive)) : nullptr;
    g_index = heap ? static_cast<LONG*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(LONG) * kIndex)) : nullptr;
    if (!g_stacks || !g_live || !g_index) { H->log("alloc trace: table allocation failed; OFF"); return false; }
    // free first, so a block allocated through the hook is always freed through it
    // (Swap stores the original before the slot points at the hook)
    if (!Swap(sf, (void*)&Free, reinterpret_cast<void**>(&realFree)) ||
        !Swap(sr, (void*)&Realloc, reinterpret_cast<void**>(&realRealloc)) ||
        (sc && !Swap(sc, (void*)&Calloc, reinterpret_cast<void**>(&realCalloc))) ||
        !Swap(sm, (void*)&Malloc, reinterpret_cast<void**>(&realMalloc))) { H->log("alloc trace: import patch failed (partially on)"); return false; }
    if (HANDLE t = CreateThread(nullptr, 0, Reporter, nullptr, 0, nullptr)) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
    H->log("alloc trace: DIAGNOSTIC ON -- the exe's malloc/calloc/realloc/free imports are sampled (one stack per MiB); report every 20 s");
    return true;
}

extern "C" __declspec(dllexport) int BigmapTestInstallAllocTrace(const Tpf2mpHost* host) { H = host; g_allocTrace = 1; return InstallAllocTrace(); }
extern "C" __declspec(dllexport) void BigmapTestAllocTraceReport() { AllocTrace::Report(); }
