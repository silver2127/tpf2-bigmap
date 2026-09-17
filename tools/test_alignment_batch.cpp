// The alignment batch detour against the real MSVC std::set layout, and the
// hook's byte anchors against the real executable. No game is touched.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include <random>
#include <cassert>
#include "../src/tpf2mp_plugin.h"
static const Tpf2mpHost* H = nullptr;
static bool g_gog = false;
#include "../src/alignment_batch.h"

static std::vector<std::vector<uint64_t>> g_calls;
static void __fastcall Recorder(void* self, AlignmentBatch::SetObject* set) {
    // Iterate the way the game does: from head->left with the MSVC successor step.
    assert(self == reinterpret_cast<void*>(0x1234));
    std::vector<uint64_t> keys;
    AlignmentBatch::SetNode* head = set->head;
    for (auto* it = head->left; it != head; it = AlignmentBatch::Next(it)) keys.push_back(it->key);
    assert(keys.size() == set->size);
    g_calls.push_back(keys);
}

int main() {
    // The game's node layout is the STL's: value at +0x20, isnil at +0x19.
    std::set<uint64_t> s;
    std::mt19937_64 rng(2026);
    for (int i = 0; i < 10007; ++i) s.insert(rng());
    auto* obj = reinterpret_cast<AlignmentBatch::SetObject*>(&s);
    assert(obj->size == s.size());
    std::vector<uint64_t> expected(s.begin(), s.end());
    std::vector<uint64_t> walked(s.size());
    assert(AlignmentBatch::CollectKeys(obj, walked.data(), walked.size()) == s.size() && walked == expected);
    // Batches of 100: 101 calls, each a chain the successor step visits in order; concatenation equals the set.
    auto self = reinterpret_cast<void*>(0x1234);
    g_calls.clear();
    BigmapTestAlignmentDetour(self, obj, Recorder, 100);
    assert(g_calls.size() == (s.size() + 99) / 100);
    std::vector<uint64_t> got;
    for (auto& c : g_calls) { assert(c.size() <= 100); got.insert(got.end(), c.begin(), c.end()); }
    assert(got == expected);
    // A set at or under the batch size, and batch 0, go through untouched.
    g_calls.clear(); BigmapTestAlignmentDetour(self, obj, Recorder, 20000);
    assert(g_calls.size() == 1 && g_calls[0] == expected);
    g_calls.clear(); BigmapTestAlignmentDetour(self, obj, Recorder, 0);
    assert(g_calls.size() == 1 && g_calls[0] == expected);
    // A single-element chain and an exact multiple.
    std::set<uint64_t> one{42}; g_calls.clear();
    BigmapTestAlignmentDetour(self, reinterpret_cast<AlignmentBatch::SetObject*>(&one), Recorder, 1);
    assert(g_calls.size() == 1 && g_calls[0] == std::vector<uint64_t>{42});
    std::set<uint64_t> six{1, 2, 3, 4, 5, 6}; g_calls.clear();
    BigmapTestAlignmentDetour(self, reinterpret_cast<AlignmentBatch::SetObject*>(&six), Recorder, 3);
    assert(g_calls.size() == 2 && g_calls[0] == (std::vector<uint64_t>{1, 2, 3}) && g_calls[1] == (std::vector<uint64_t>{4, 5, 6}));
    // The game's set is untouched by the batching: still iterates and erases normally.
    assert(std::vector<uint64_t>(s.begin(), s.end()) == expected);
    s.clear();
    // Byte anchors in the real executable.
    FILE* f = nullptr; fopen_s(&f, "C:\\tools\\bin\\TransportFever2.exe", "rb"); assert(f);
    auto check = [&](uintptr_t rva, const uint8_t* bytes, size_t n) {
        // .text: file offset = rva - 0x1000 + 0x400 for this image (section 1 at RVA 0x1000, raw 0x400).
        uint8_t buf[32]; fseek(f, long(rva - 0x1000 + 0x400), SEEK_SET); assert(fread(buf, 1, n, f) == n);
        assert(memcmp(buf, bytes, n) == 0);
    };
    check(AlignmentBatch::kUpdateRva, AlignmentBatch::kUpdateBytes, sizeof AlignmentBatch::kUpdateBytes);
    check(AlignmentBatch::kCallerSiteRva, AlignmentBatch::kCallerSiteBytes, sizeof AlignmentBatch::kCallerSiteBytes);
    check(AlignmentBatch::kStepRva, AlignmentBatch::kStepBytes, sizeof AlignmentBatch::kStepBytes);
    fclose(f);
    printf("PASS: MSVC set walk matches std::set, batches concatenate to the set in order, pass-through cases, chains of 1 and exact multiples, byte anchors\n");
    return 0;
}
