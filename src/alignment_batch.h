// Steam 35924: run the terrain alignment update in batches of tiles.
//
// ecs::TerrainAlignmentSystem's update (0xaac810) hands the set of terrain
// blocks dirtied since the last frame (a std::set at this+0xa0, one entry per
// sub-tile block: its CVec2i coordinate and a vector of the 16-byte alignment
// items that touched it; the node constructor is 0x6fba90, 0x40 bytes) to
// UpdateSubterrains (0xaad6c0), which for EVERY entry gathers the block's
// alignments, computes a work block and a result block per region on the
// thread pool (LoopImpl at 0xaadc50), and only then publishes all results
// into the height caches (0x33cd10 at 0xaadcf5) and frees them. In play the
// set holds a few entries. On a save load it holds the whole map: MEASURED
// 2026-09-17 on LONGBOI (207,360 tiles, 1,658,880 entries), 3.3 million
// result vectors and ~10 million work vectors alive at once, the game's own
// 34 GiB private peak, and the "Out of memory" assert on a 94 GiB machine
// with no page file.
//
// This detour splits that one call into calls of `alignment_batch_tiles`
// entries each, so compute and publication alternate and the live set is
// one batch's worth. It never touches the game's set: it walks it read-only
// with the same successor step the function uses (0xaadb74..0xaadbbf, the
// MSVC _Tree iterator), copies each entry's value (coordinate plus the three
// pointers of its vector, which the function only reads), and builds its own
// degenerate trees (a left chain, head as nil) for the original to iterate.
// The caller erases its own set, vectors included, afterwards, untouched.
// (A first version copied only the coordinate; CreateTileBlock then read the
// vector from uninitialised memory and asserted on its box.)
#pragma once
#include "memory_status.h"   // PagerMemoryStatus (auto batch size)
#include "gog_rva.h"         // RunWithSteamSites (needs H and g_gog declared first)
static int g_alignmentBatch = 512;   // tiles per UpdateSubterrains call; 0 = stock; -1 = sized from free memory
// AUTO SIZE (alignment_batch_tiles=-1, 2026-11-17). Every batch ends with a
// serial publication while the engine's workers wait: on a 224 x 224 GOG map,
// 802,816 blocks in 1,568 batches of 512 took ~12 s per pass, most threads
// idle (tools/profile_load.py). The cost a batch holds is bounded by the
// LONGBOI measurement: a 34 GiB private peak for 1,658,880 entries in ONE
// call, i.e. about 21 KiB per entry with the game's own baseline included.
// Auto sizing charges 64 KiB per entry (3x that) against a quarter of
// min(free RAM, free commit), re-read before every batch, clamped to
// 512..65,536 entries: a tight machine falls back to the 512 the fixed
// setting used, a roomy one runs the pass in a few large batches.
static const size_t kAlignBatchAutoMin = 512, kAlignBatchAutoMax = size_t(1) << 16;
static const uint64_t kAlignBatchEntryBytes = 64 * 1024;
// The CTerrain the alignment pass works on (UpdateSubterrains' this+8), as
// last seen by the detour; read by the terrain sidecar (terrain_sidecar.h).
static void* g_alignmentTerrain = nullptr;
// Wall time of the last batched pass (the load's), milliseconds.
static volatile LONG64 g_alignmentPassMs = 0;
// Called after a batched pass (a load's) has published its last batch; the
// terrain sidecar (terrain_serve.h) releases the loaded file here.
static void (*g_alignmentPassDone)(bool skipped) = nullptr;
// SKIPPING A PASS THE SIDECAR ALREADY ANSWERED (2026-09-28). When every tile of
// the terrain was served from the save's sidecar, the load's alignment pass
// computes ~800,000 blocks (44-75 s on a player's 49,928-tile map) whose
// publication into served tiles is skipped anyway. Its only other effect,
// publication's per-tile minZ/maxZ/version (0x33cf2c..0x33cf38), is written by
// g_alignmentAllServed from the served caches; it returns false -- and the pass
// runs as before -- unless every tile of this terrain is served.
static bool (*g_alignmentAllServed)(void* terrain) = nullptr;
static int g_alignmentSkipServed = 1;   // alignment_skip_served; 0 = always run the pass
namespace AlignmentBatch {
struct SetValue { uint64_t key; void* vfirst; void* vlast; void* vend; };   // CVec2i block + std::vector<16-byte item>
struct SetNode { SetNode* left; SetNode* parent; SetNode* right; uint8_t color, isnil, pad[6]; SetValue value; };
struct SetObject { SetNode* head; size_t size; };
static_assert(offsetof(SetNode, isnil) == 0x19 && offsetof(SetNode, value) == 0x20 && sizeof(SetNode) == 0x40, "the game's dirty-set node layout");
using UpdateFn = void(__fastcall*)(void* self, SetObject* set);
static UpdateFn original = nullptr;
static uintptr_t base = 0;
static volatile LONG64 calls = 0, batchedCalls = 0, batchedTiles = 0, skippedPasses = 0, skippedTiles = 0;
constexpr uintptr_t kUpdateRva = 0xaad6c0, kCallerSiteRva = 0xaac82b, kStepRva = 0xaadb78;
static const uint8_t kUpdateBytes[17] = {0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xa0};
static const uint8_t kCallerSiteBytes[11] = {0x48, 0x83, 0x7f, 0x08, 0x00, 0x74, 0x09, 0x48, 0x8b, 0xd7, 0xe8};   // cmp [rdi+8],0; je; mov rdx,rdi; call
static const uint8_t kStepBytes[4] = {0x80, 0x78, 0x19, 0x00};   // cmp byte [rax+0x19],0: the isnil test of the iterator step

// The MSVC _Tree iterator increment, as the function performs it.
static SetNode* Next(SetNode* n) {
    if (!n->right->isnil) { n = n->right; while (!n->left->isnil) n = n->left; return n; }
    SetNode* p = n->parent;
    while (!p->isnil && n == p->right) { n = p; p = p->parent; }
    return p;
}
static size_t CollectValues(SetObject* set, SetValue* out, size_t cap) {
    SetNode* head = set->head; size_t n = 0;
    for (SetNode* it = head->left; it != head && n < cap; it = Next(it)) out[n++] = it->value;
    return n;
}
// A left chain of n keys in nodes[1..n], nodes[0] the head (nil): iterating
// from head->left visits the keys in order and ends at head.
static void BuildChain(SetNode* nodes, const SetValue* values, size_t n) {
    SetNode* head = &nodes[0];
    head->isnil = 1; head->color = 1; head->value = SetValue{};
    for (size_t i = 0; i < n; ++i) {
        SetNode* nd = &nodes[1 + i];
        nd->left = i ? &nodes[i] : head;
        nd->parent = i + 1 < n ? &nodes[2 + i] : head;
        nd->right = head; nd->color = 1; nd->isnil = 0; nd->value = values[i];
    }
    head->left = n ? &nodes[1] : head; head->parent = n ? &nodes[n] : head; head->right = n ? &nodes[n] : head;
}
// The auto batch for the memory free right now (see kAlignBatchEntryBytes).
static size_t AutoBatchSize() {
    MEMORYSTATUSEX m{}; m.dwLength = sizeof m;
    if (!PagerMemoryStatus(&m)) return kAlignBatchAutoMin;
    const uint64_t freeBytes = m.ullAvailPhys < m.ullAvailPageFile ? m.ullAvailPhys : m.ullAvailPageFile;
    const uint64_t b = freeBytes / 4 / kAlignBatchEntryBytes;
    return b < kAlignBatchAutoMin ? kAlignBatchAutoMin : b > kAlignBatchAutoMax ? kAlignBatchAutoMax : size_t(b);
}
static void __fastcall Detour(void* self, SetObject* set) {
    InterlockedIncrement64(&calls);
    if (self) g_alignmentTerrain = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 8);
    size_t total = set && set->head ? set->size : 0;
    const bool autoSize = g_alignmentBatch < 0;
    size_t batch = autoSize ? kAlignBatchAutoMin : g_alignmentBatch > 0 ? size_t(g_alignmentBatch) : 0;
    if (!batch || total <= batch || total > (size_t(64) << 20)) { original(self, set); return; }
    if (g_alignmentSkipServed && g_alignmentAllServed && g_alignmentTerrain && g_alignmentAllServed(g_alignmentTerrain)) {
        InterlockedIncrement64(&skippedPasses); InterlockedAdd64(&skippedTiles, LONG64(total));
        InterlockedExchange64(&g_alignmentPassMs, 0);
        if (H) H->log("alignment pass: %llu blocks skipped -- every tile was served from the sidecar (its min/max and version written from the served cache)", (unsigned long long)total);
        if (g_alignmentPassDone) g_alignmentPassDone(true);
        return;
    }
    auto keys = static_cast<SetValue*>(HeapAlloc(GetProcessHeap(), 0, total * sizeof(SetValue)));
    const size_t nodeCap = autoSize ? kAlignBatchAutoMax : batch;
    auto nodes = static_cast<SetNode*>(HeapAlloc(GetProcessHeap(), 0, (nodeCap + 1) * sizeof(SetNode)));
    if (!keys || !nodes) {
        if (keys) HeapFree(GetProcessHeap(), 0, keys);
        if (nodes) HeapFree(GetProcessHeap(), 0, nodes);
        original(self, set); return;
    }
    size_t n = CollectValues(set, keys, total);
    InterlockedIncrement64(&batchedCalls); InterlockedAdd64(&batchedTiles, LONG64(n));
    LARGE_INTEGER f{}, t0{}, t1{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    size_t batches = 0, smallest = SIZE_MAX, largest = 0;
    for (size_t off = 0; off < n; off += batch) {
        if (autoSize) batch = AutoBatchSize();
        size_t k = n - off < batch ? n - off : batch;
        ++batches; if (k < smallest) smallest = k; if (k > largest) largest = k;
        BuildChain(nodes, keys + off, k);
        SetObject fake{&nodes[0], k};
        original(self, &fake);
    }
    QueryPerformanceCounter(&t1);
    LONG64 ms = f.QuadPart ? (t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart : 0;
    InterlockedExchange64(&g_alignmentPassMs, ms);
    if (H) H->log("alignment pass: %llu blocks in %llu batches of %llu..%llu%s, %lld ms (compute + publish)", (unsigned long long)n,
                  (unsigned long long)batches, (unsigned long long)(batches ? smallest : 0), (unsigned long long)largest,
                  autoSize ? " (sized from free memory)" : "", ms);
    HeapFree(GetProcessHeap(), 0, keys);
    HeapFree(GetProcessHeap(), 0, nodes);
    if (g_alignmentPassDone) g_alignmentPassDone(false);
}
}  // namespace AlignmentBatch
static bool InstallAlignmentBatch() {
    using namespace AlignmentBatch;
    if (g_alignmentBatch == 0) return false;   // -1 = auto size; GOG: installed through RunWithSteamSites
    if (!H->verifyBytes(kUpdateRva, kUpdateBytes, sizeof kUpdateBytes) ||
        !H->verifyBytes(kCallerSiteRva, kCallerSiteBytes, sizeof kCallerSiteBytes) ||
        !H->verifyBytes(kStepRva, kStepBytes, sizeof kStepBytes)) {
        H->log("alignment batch: byte mismatch; OFF"); return false;
    }
    base = H->moduleBase();
    if (!H->installHook(base + kUpdateRva, reinterpret_cast<void*>(Detour), sizeof kUpdateBytes, reinterpret_cast<void**>(&original))) {
        H->log("alignment batch: hook failed; OFF"); return false;
    }
    if (g_alignmentBatch < 0)
        H->log("alignment batch: terrain alignment updates run in batches sized from free memory (%llu..%llu tiles, %llu KiB charged per tile)",
               (unsigned long long)kAlignBatchAutoMin, (unsigned long long)kAlignBatchAutoMax, (unsigned long long)(kAlignBatchEntryBytes / 1024));
    else
        H->log("alignment batch: terrain alignment updates run in batches of %d tiles (compute, publish, free per batch)", g_alignmentBatch);
    return true;
}
extern "C" __declspec(dllexport) int BigmapTestInstallAlignmentBatch(const Tpf2mpHost* host, int gog, int batch) {
    const auto oldHost = H; const bool oldGog = g_gog; const int oldBatch = g_alignmentBatch;
    const auto oldOriginal = AlignmentBatch::original; const uintptr_t oldBase = AlignmentBatch::base;
    H = host; g_gog = gog != 0; g_alignmentBatch = batch;
    const bool result = RunWithSteamSites([] { return InstallAlignmentBatch(); });   // GOG: translated host
    H = oldHost; g_gog = oldGog; g_alignmentBatch = oldBatch;
    AlignmentBatch::original = oldOriginal; AlignmentBatch::base = oldBase;
    return result;
}
extern "C" __declspec(dllexport) void BigmapTestAlignmentDetour(void* self, AlignmentBatch::SetObject* set, AlignmentBatch::UpdateFn fn, int batch) {
    AlignmentBatch::original = fn; const int old = g_alignmentBatch; g_alignmentBatch = batch;
    AlignmentBatch::Detour(self, set); g_alignmentBatch = old;
}
extern "C" __declspec(dllexport) size_t BigmapTestAlignmentCollect(AlignmentBatch::SetObject* set, AlignmentBatch::SetValue* out, size_t cap) { return AlignmentBatch::CollectValues(set, out, cap); }
