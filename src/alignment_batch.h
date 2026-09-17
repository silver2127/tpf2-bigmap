// Steam 35924: run the terrain alignment update in batches of tiles.
//
// ecs::TerrainAlignmentSystem's update (0xaac810) hands the set of tiles
// dirtied since the last frame (a std::set of 8-byte tile keys at this+0xa0)
// to UpdateSubterrains (0xaad6c0), which for EVERY tile in the set gathers
// its alignments, computes a work block and a result block per region on the
// thread pool (LoopImpl at 0xaadc50), and only then publishes all results
// into the height caches (0x33cd10 at 0xaadcf5) and frees them. In play the
// set holds a few tiles. On a save load it holds the whole map: MEASURED
// 2026-09-17 on LONGBOI (207,360 tiles), 3.3 million result vectors and ~10
// million work vectors alive at once, the game's own 34 GiB private peak,
// and the "Out of memory" assert on a 94 GiB machine with no page file.
//
// This detour splits that one call into calls of `alignment_batch_tiles`
// tiles each, so compute and publication alternate and the live set is one
// batch's worth. It never touches the game's set: it walks it read-only with
// the same successor step the function uses (0xaadb74..0xaadbbf, the MSVC
// _Tree iterator), copies the keys, and builds its own degenerate trees (a
// left chain, head as nil) for the original to iterate. The function reads
// only the head pointer, the nodes' left/parent/right/isnil fields and the
// key at +0x20; the caller erases its own set afterwards, untouched.
#pragma once
static int g_alignmentBatch = 512;   // tiles per UpdateSubterrains call; 0 = stock
namespace AlignmentBatch {
struct SetNode { SetNode* left; SetNode* parent; SetNode* right; uint8_t color, isnil, pad[6]; uint64_t key; };
struct SetObject { SetNode* head; size_t size; };
static_assert(offsetof(SetNode, isnil) == 0x19 && offsetof(SetNode, key) == 0x20 && sizeof(SetNode) == 0x28, "MSVC std::set node layout");
using UpdateFn = void(__fastcall*)(void* self, SetObject* set);
static UpdateFn original = nullptr;
static uintptr_t base = 0;
static volatile LONG64 calls = 0, batchedCalls = 0, batchedTiles = 0;
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
static size_t CollectKeys(SetObject* set, uint64_t* out, size_t cap) {
    SetNode* head = set->head; size_t n = 0;
    for (SetNode* it = head->left; it != head && n < cap; it = Next(it)) out[n++] = it->key;
    return n;
}
// A left chain of n keys in nodes[1..n], nodes[0] the head (nil): iterating
// from head->left visits the keys in order and ends at head.
static void BuildChain(SetNode* nodes, const uint64_t* keys, size_t n) {
    SetNode* head = &nodes[0];
    head->isnil = 1; head->color = 1; head->key = 0;
    for (size_t i = 0; i < n; ++i) {
        SetNode* nd = &nodes[1 + i];
        nd->left = i ? &nodes[i] : head;
        nd->parent = i + 1 < n ? &nodes[2 + i] : head;
        nd->right = head; nd->color = 1; nd->isnil = 0; nd->key = keys[i];
    }
    head->left = n ? &nodes[1] : head; head->parent = n ? &nodes[n] : head; head->right = n ? &nodes[n] : head;
}
static void __fastcall Detour(void* self, SetObject* set) {
    InterlockedIncrement64(&calls);
    size_t total = set && set->head ? set->size : 0;
    size_t batch = g_alignmentBatch > 0 ? size_t(g_alignmentBatch) : 0;
    if (!batch || total <= batch || total > (size_t(64) << 20)) { original(self, set); return; }
    auto keys = static_cast<uint64_t*>(HeapAlloc(GetProcessHeap(), 0, total * sizeof(uint64_t)));
    auto nodes = static_cast<SetNode*>(HeapAlloc(GetProcessHeap(), 0, (batch + 1) * sizeof(SetNode)));
    if (!keys || !nodes) {
        if (keys) HeapFree(GetProcessHeap(), 0, keys);
        if (nodes) HeapFree(GetProcessHeap(), 0, nodes);
        original(self, set); return;
    }
    size_t n = CollectKeys(set, keys, total);
    InterlockedIncrement64(&batchedCalls); InterlockedAdd64(&batchedTiles, LONG64(n));
    if (H) H->log("alignment pass: %llu tiles in %llu batches of %llu", (unsigned long long)n, (unsigned long long)((n + batch - 1) / batch), (unsigned long long)batch);
    for (size_t off = 0; off < n; off += batch) {
        size_t k = n - off < batch ? n - off : batch;
        BuildChain(nodes, keys + off, k);
        SetObject fake{&nodes[0], k};
        original(self, &fake);
    }
    HeapFree(GetProcessHeap(), 0, keys);
    HeapFree(GetProcessHeap(), 0, nodes);
}
}  // namespace AlignmentBatch
static bool InstallAlignmentBatch() {
    using namespace AlignmentBatch;
    if (g_alignmentBatch <= 0 || g_gog) return false;
    if (!H->verifyBytes(kUpdateRva, kUpdateBytes, sizeof kUpdateBytes) ||
        !H->verifyBytes(kCallerSiteRva, kCallerSiteBytes, sizeof kCallerSiteBytes) ||
        !H->verifyBytes(kStepRva, kStepBytes, sizeof kStepBytes)) {
        H->log("alignment batch: Steam byte mismatch; OFF"); return false;
    }
    base = H->moduleBase();
    if (!H->installHook(base + kUpdateRva, reinterpret_cast<void*>(Detour), sizeof kUpdateBytes, reinterpret_cast<void**>(&original))) {
        H->log("alignment batch: hook failed; OFF"); return false;
    }
    H->log("alignment batch: terrain alignment updates run in batches of %d tiles (compute, publish, free per batch)", g_alignmentBatch);
    return true;
}
extern "C" __declspec(dllexport) void BigmapTestAlignmentDetour(void* self, AlignmentBatch::SetObject* set, AlignmentBatch::UpdateFn fn, int batch) {
    AlignmentBatch::original = fn; const int old = g_alignmentBatch; g_alignmentBatch = batch;
    AlignmentBatch::Detour(self, set); g_alignmentBatch = old;
}
extern "C" __declspec(dllexport) size_t BigmapTestAlignmentCollect(AlignmentBatch::SetObject* set, uint64_t* out, size_t cap) { return AlignmentBatch::CollectKeys(set, out, cap); }
