// Native Steam Linux 35924 terrain_alignment_util::CalculateHeightMod at RVA 0xda1a70
// Port of the Windows kernel. Raster triangle coordinates use SysV SSE args.
// linux/tests/terrain_align_oracle.cpp compares the actual Linux functions.
// (game\terrain\terrain_alignment_list_util.cpp). One call per height-mod block
// from the ecs::TerrainAlignmentSystem::UpdateSubterrains thread-pool workers
// (0xaac460) and from 0x2146540: it re-applies the construction/street/track
// alignment triangles to a block of N = size.x * size.y height samples while the
// height cache is rebuilt.
//
// Bit-identical replacement (docs/terrain-alignment-speed.md). Stock work per call:
//  * three PredHeightModRasterizable targets, each holding two std::vector<uint16>
//    of N words. 0x3b0190 (twice) and 0x3af850 (six times) allocate six heap
//    blocks and fill them ONE WORD PER ITERATION with 0xffff, 0, 0xffff, 0, 0, 0;
//  * every alignment triangle rasterised into its target (0x31bd660 / 0x31bd6e0
//    with the predicates 0x3b7440 and 0x3b6ee0);
//  * a scalar pass over all N samples that blends the three targets into the
//    result wherever any of the three weights is non-zero.
// What changed:
//  * the six vectors come from a pooled buffer reused across calls and reset with
//    two memsets (same N words, same 0xffff / 0 values) -- no allocation, no
//    per-call first touch of fresh pages, no word-at-a-time fill;
//  * triangles are still rasterised by the ORIGINAL code: the targets are laid
//    out byte for byte as 0x3b0190 lays them out (vtable, sizes, scale, 1/scale,
//    offset, the two vectors) and the predicates read nothing else;
//  * the blend skips all-zero-weight samples eight at a time and evaluates the
//    rest four lanes wide with SSE2 -- the same IEEE single-precision operations
//    on the same operands in the same grouping, comparisons with the same
//    ordered/unordered outcome (cmpps matches comiss/ucomiss + the stock branch),
//    and floorf + cvttss2si reproduced exactly. No FMA, no reassociation.
// The stock size assert, blocks with a side < 2 or more than 1<<20 samples, an
// alignment type the stock code cannot index, malformed vectors and a failed
// buffer allocation all run the original.
#pragma once
#include <emmintrin.h>
#include <cstdint>
#include <mutex>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <malloc.h>
#include <string.h>

struct AlignU16Vector { uint16_t* first; uint16_t* last; uint16_t* end; };
struct AlignPointerVector { const uint8_t* const* first; const uint8_t* const* last;
                            const uint8_t* const* end; };
using CalculateHeightModFn = void (*)(const float*, const int32_t*, float, float,
    const AlignPointerVector*, AlignU16Vector*);
using AlignRasterInitFn = void* (*)(void*, void*, const float*, const float*,
    const int32_t*, uint8_t);
struct AlignVec2 { float x,y; };
using AlignRasterTriangleFn = uint8_t (*)(void*, AlignVec2, AlignVec2, AlignVec2);
using AlignAssertFn = void (*)(const char*, const char*, int, const char*);

// terrain::TerrainAlignment, only the fields 0xda1a70 reads.
static const size_t kAlignTriangles = 0x00;    // vector<CVec3f>: 3 vertices (36 bytes) per triangle
static const size_t kAlignWeights   = 0x18;    // vector<CVec3f>: one weight per vertex, may be empty
static const size_t kAlignType      = 0x30;    // 0, 1 or 2: which rasterisation target
static const int64_t kAlignMaxSamples = 1 << 20;

static const uintptr_t kAlignFunc            = 0xda1a70;
static const uintptr_t kAlignRasterInit      = 0x31bd660;
static const uintptr_t kAlignRasterTriangle  = 0x31bd6e0;
static const uintptr_t kAlignAssert          = 0x2fcb860;
static const uintptr_t kAlignVtableLessEqual = 0x59b3fb0;   // targets 0 and 1, heights start 0xffff
static const uintptr_t kAlignVtableGreaterEqual = 0x59b3fd8;// target 2, heights start 0
static const uintptr_t kAlignAssertExpr      = 0x3f2652a;
static const uintptr_t kAlignAssertFile      = 0x3e94c48;
static const uintptr_t kAlignAssertFunction  = 0x400b260;

// PredHeightModRasterizable<...>, exactly as 0x3b0190 builds it.
struct AlignTarget {
    uintptr_t vtable;                   // +0x00
    unsigned char triangle[0x30];       // +0x08 three vertices then three weights, zeroed by the ctor
    int32_t sizeX, sizeY;               // +0x38
    float scale, invScale, offset;      // +0x40
    uint32_t padding;                   // +0x4c untouched by the ctor and unread by the predicates
    AlignU16Vector heights;             // +0x50
    AlignU16Vector weights;             // +0x68
};
static_assert(offsetof(AlignTarget, sizeX) == 0x38, "target layout");
static_assert(offsetof(AlignTarget, scale) == 0x40, "target layout");
static_assert(offsetof(AlignTarget, heights) == 0x50, "target layout");
static_assert(offsetof(AlignTarget, weights) == 0x68, "target layout");
static_assert(sizeof(AlignTarget) == 0x80, "target layout");

static CalculateHeightModFn g_originalCalculateHeightMod = nullptr;
static uintptr_t g_terrainAlignBase = 0;


// ---------------------------------------------------------------- scratch pool
// One buffer of 6*N words per concurrent call: heightA, heightB, weightA,
// weightB, heightC, weightC. Pooled because the stock code's six allocations and
// their word-at-a-time fills are ~5% of a big save load.
struct AlignScratch { AlignScratch* next; size_t samples; uint16_t* words; };
static std::mutex g_alignScratchLock;
static AlignScratch* g_alignScratchFree = nullptr;

static void ReleaseAlignScratch(AlignScratch* scratch) {
    g_alignScratchLock.lock();
    scratch->next = g_alignScratchFree;
    g_alignScratchFree = scratch;
    g_alignScratchLock.unlock();
}

static AlignScratch* AcquireAlignScratch(size_t samples) {
    g_alignScratchLock.lock();
    AlignScratch* scratch = g_alignScratchFree;
    if (scratch) g_alignScratchFree = scratch->next;
    g_alignScratchLock.unlock();
    if (!scratch) {
        scratch = static_cast<AlignScratch*>(malloc(sizeof(AlignScratch)));
        if (!scratch) return nullptr;
        scratch->next = nullptr; scratch->samples = 0; scratch->words = nullptr;
    }
    if (scratch->samples < samples) {
        size_t want = samples < 66049 ? 66049 : samples;      // a 1 m tile block is 257x257
        want = (want + 0xfff) & ~size_t(0xfff);
        free(scratch->words);
        scratch->words = static_cast<uint16_t*>(aligned_alloc(64, 6 * want * sizeof(uint16_t)));
        scratch->samples = scratch->words ? want : 0;
        if (!scratch->words) { ReleaseAlignScratch(scratch); return nullptr; }
    }
    return scratch;
}

// ---------------------------------------------------------------- the blend
struct AlignBlendInput {
    const uint16_t* result;
    const uint16_t* heightA; const uint16_t* heightB; const uint16_t* heightC;
    const uint16_t* weightA; const uint16_t* weightB; const uint16_t* weightC;
};
struct AlignBlendConstants { __m128 scale, offset, invScale, zero, one, half, full; };

static inline __m128 AlignWiden(__m128i words, int upper) {
    const __m128i zero = _mm_setzero_si128();
    return _mm_cvtepi32_ps(upper ? _mm_unpackhi_epi16(words, zero) : _mm_unpacklo_epi16(words, zero));
}
static inline __m128 AlignSelect(__m128 mask, __m128 taken, __m128 other) {
    return _mm_or_ps(_mm_and_ps(mask, taken), _mm_andnot_ps(mask, other));
}

// Eight samples. Returns a bitmask of lanes to store; *failLane, if set, is the
// lane where the stock "totalW > .0f" assert fires (lanes above it are dropped).
static int AlignBlendEight(const AlignBlendInput& in, const AlignBlendConstants& k,
                           int32_t* values, int* failLane)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i wordsA = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.weightA));
    const __m128i wordsB = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.weightB));
    const __m128i wordsC = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.weightC));
    // Stock skips a sample when (int)wA + (int)wB + (int)wC == 0, i.e. all three zero.
    const int nonzero = ~_mm_movemask_epi8(
        _mm_cmpeq_epi16(_mm_or_si128(_mm_or_si128(wordsA, wordsB), wordsC), zero)) & 0xffff;
    if (!nonzero) return 0;
    const __m128i rWords  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.result));
    const __m128i haWords = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.heightA));
    const __m128i hbWords = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.heightB));
    const __m128i hcWords = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.heightC));
    int written = 0;
    for (int upper = 0; upper < 2; ++upper) {
        int lanes = 0;
        for (int lane = 0; lane < 4; ++lane)
            if ((nonzero >> (2 * (4 * upper + lane))) & 3) lanes |= 1 << lane;
        if (!lanes) continue;
        // height = (float)word * scale + offset, as the stock cvtdq2ps/mulss/addss.
        const __m128 heightR = _mm_add_ps(_mm_mul_ps(AlignWiden(rWords, upper), k.scale), k.offset);
        const __m128 heightA = _mm_add_ps(_mm_mul_ps(AlignWiden(haWords, upper), k.scale), k.offset);
        const __m128 heightB = _mm_add_ps(_mm_mul_ps(AlignWiden(hbWords, upper), k.scale), k.offset);
        const __m128 heightC = _mm_add_ps(_mm_mul_ps(AlignWiden(hcWords, upper), k.scale), k.offset);
        const __m128 fractionA = _mm_div_ps(AlignWiden(wordsA, upper), k.full);
        __m128 fractionB = _mm_div_ps(AlignWiden(wordsB, upper), k.full);
        __m128 fractionC = _mm_div_ps(AlignWiden(wordsC, upper), k.full);
        // lower = min(fC > 0 ? max(result, C) : result, B); upper = max(fB > 0 ? min(result, B) : result, C)
        __m128 lower = AlignSelect(_mm_and_ps(_mm_cmpgt_ps(fractionC, k.zero),
                                              _mm_cmpgt_ps(heightC, heightR)), heightC, heightR);
        lower = AlignSelect(_mm_cmpgt_ps(lower, heightB), heightB, lower);
        __m128 higher = AlignSelect(_mm_and_ps(_mm_cmpgt_ps(fractionB, k.zero),
                                               _mm_cmpgt_ps(heightR, heightB)), heightB, heightR);
        higher = AlignSelect(_mm_cmpgt_ps(heightC, higher), heightC, higher);
        fractionB = _mm_and_ps(_mm_cmpeq_ps(lower, heightB), fractionB);
        fractionC = _mm_and_ps(_mm_cmpeq_ps(higher, heightC), fractionC);
        const __m128 sum = _mm_add_ps(_mm_add_ps(fractionB, fractionA), fractionC);
        int active = lanes & ~_mm_movemask_ps(_mm_cmpeq_ps(sum, k.zero));
        if (!active) continue;
        // A weight of exactly 1 in two of the three drops the later one.
        const __m128 oneA = _mm_cmpeq_ps(fractionA, k.one);
        const __m128 oneB = _mm_cmpeq_ps(fractionB, k.one);
        const __m128 oneC = _mm_cmpeq_ps(fractionC, k.one);
        fractionB = _mm_andnot_ps(_mm_and_ps(oneA, oneB), fractionB);
        fractionC = _mm_andnot_ps(_mm_and_ps(oneC, _mm_or_ps(oneA, oneB)), fractionC);
        const __m128 restA = _mm_sub_ps(k.one, fractionA);
        const __m128 restB = _mm_sub_ps(k.one, fractionB);
        const __m128 restC = _mm_sub_ps(k.one, fractionC);
        const __m128 weightA = _mm_mul_ps(_mm_mul_ps(restB, fractionA), restC);
        const __m128 weightB = _mm_mul_ps(_mm_mul_ps(restA, fractionB), restC);
        const __m128 weightC = _mm_mul_ps(_mm_mul_ps(restA, restB), fractionC);
        const __m128 total = _mm_add_ps(_mm_add_ps(weightB, weightA), weightC);
        const __m128 positive = _mm_cmpgt_ps(total, k.zero);
        const int bad = active & ~_mm_movemask_ps(positive);
        // Inactive lanes divide by 1: no stock operation is added, and an
        // unmasked divide-by-zero cannot fire where stock would not divide.
        const __m128 divisor = AlignSelect(positive, total, k.one);
        __m128 value = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lower, weightB), _mm_mul_ps(weightA, heightA)),
                                  _mm_mul_ps(higher, weightC));
        value = _mm_div_ps(value, divisor);
        value = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(value, k.offset), k.invScale), k.half);
        // floorf then cvttss2si: truncation, minus one where truncation rounded
        // up, and the 0x80000000 "integer indefinite" of NaN and out-of-range
        // values kept as is (stock floors them to themselves).
        __m128i truncated = _mm_cvttps_epi32(value);
        const __m128i indefinite = _mm_cmpeq_epi32(truncated, _mm_set1_epi32(INT32_MIN));
        const __m128i rounded = _mm_castps_si128(_mm_cmplt_ps(value, _mm_cvtepi32_ps(truncated)));
        truncated = _mm_add_epi32(truncated, _mm_andnot_si128(indefinite, rounded));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(values + 4 * upper), truncated);
        if (bad) {
            const unsigned lane = unsigned(__builtin_ctz(unsigned(bad)));
            written |= (active & ((1 << lane) - 1)) << (4 * upper);
            *failLane = 4 * upper + static_cast<int>(lane);
            return written;
        }
        written |= active << (4 * upper);
    }
    return written;
}

// Returns the sample index where the stock assert fires, or -1.
static int64_t AlignBlend(uint16_t* out, const uint16_t* words, size_t samples,
                          const AlignBlendConstants& k)
{
    AlignBlendInput in;
    const uint16_t* heightA = words;
    const uint16_t* heightB = words + samples;
    const uint16_t* weightA = words + 2 * samples;
    const uint16_t* weightB = words + 3 * samples;
    const uint16_t* heightC = words + 4 * samples;
    const uint16_t* weightC = words + 5 * samples;
    uint16_t tail[7][8];
    for (size_t i = 0; i < samples; i += 8) {
        const size_t count = samples - i < 8 ? samples - i : 8;
        const uint16_t* source[7] = {out + i, heightA + i, heightB + i, heightC + i,
                                     weightA + i, weightB + i, weightC + i};
        if (count < 8) {
            for (int s = 0; s < 7; ++s) {
                memset(tail[s], 0, sizeof tail[s]);         // padding lanes: weight 0, never written
                memcpy(tail[s], source[s], count * sizeof(uint16_t));
                source[s] = tail[s];
            }
        }
        in.result = source[0]; in.heightA = source[1]; in.heightB = source[2]; in.heightC = source[3];
        in.weightA = source[4]; in.weightB = source[5]; in.weightC = source[6];
        int32_t values[8];
        int failLane = -1;
        const int written = AlignBlendEight(in, k, values, &failLane);
        for (int lane = 0; lane < 8; ++lane)
            if ((written >> lane) & 1) out[i + lane] = static_cast<uint16_t>(values[lane]);
        if (failLane >= 0) return static_cast<int64_t>(i) + failLane;
    }
    return -1;
}

// ---------------------------------------------------------------- the call
static bool AlignListSupported(const AlignPointerVector* list) {
    const uintptr_t first = reinterpret_cast<uintptr_t>(list->first);
    const uintptr_t last = reinterpret_cast<uintptr_t>(list->last);
    if (last < first || (last - first) % sizeof(void*)) return false;
    for (const uint8_t* const* item = list->first; item != list->last; ++item) {
        const uint8_t* alignment = *item;
        uintptr_t begin = 0, end = 0;
        memcpy(&begin, alignment + kAlignTriangles, sizeof begin);
        memcpy(&end, alignment + kAlignTriangles + sizeof(void*), sizeof end);
        if (end < begin) return false;
        const uint64_t count = (end - begin) / 36;
        if (count > 0x7fffffff) return false;
        if (count) {                       // stock reads the type only when it rasterises
            int32_t type = 0;
            memcpy(&type, alignment + kAlignType, sizeof type);
            if (type < 0 || type > 2) return false;
        }
    }
    return true;
}

static void CalculateHeightModImpl(CalculateHeightModFn original, uintptr_t base, const float* box,
    const int32_t* size, float scale, float offset, const AlignPointerVector* alignments,
    AlignU16Vector* result)
{
    const int32_t sizeX = size[0], sizeY = size[1];
    const int64_t resultWords =
        (reinterpret_cast<intptr_t>(result->last) - reinterpret_cast<intptr_t>(result->first)) >> 1;
    if (int32_t(uint32_t(sizeX) * uint32_t(sizeY)) != int32_t(resultWords) ||   // the stock assert
        sizeX < 2 || sizeY < 2 || int64_t(sizeX) * int64_t(sizeY) > kAlignMaxSamples ||
        !base || !AlignListSupported(alignments)) {
        original(box, size, scale, offset, alignments, result);
        return;
    }
    const size_t samples = size_t(sizeX) * size_t(sizeY);
    // No alignment triangles means all three stock weight planes stay zero:
    // every output sample is unchanged. Avoid six fills and the blend scan.
    if(alignments->first==alignments->last && std::isfinite(scale) && scale!=0.f)return;
    AlignScratch* scratch = AcquireAlignScratch(samples);
    if (!scratch) {
        original(box, size, scale, offset, alignments, result);
        return;
    }
    struct ScratchLease { AlignScratch* value; ~ScratchLease(){ReleaseAlignScratch(value);} } lease{scratch};
    uint16_t* words = scratch->words;
    memset(words, 0xff, 4 * samples);                       // heights of targets 0 and 1: 0xffff
    memset(words + 2 * samples, 0, 8 * samples);            // their weights, and target 2 entirely

    const float invScale = _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(1.0f), _mm_set_ss(scale)));
    AlignTarget targets[3] = {};
    for (int t = 0; t < 3; ++t) {
        targets[t].vtable = base + (t == 2 ? kAlignVtableGreaterEqual : kAlignVtableLessEqual);
        targets[t].sizeX = sizeX;
        targets[t].sizeY = sizeY;
        targets[t].scale = scale;
        targets[t].invScale = invScale;
        targets[t].offset = offset;
    }
    const size_t slot[3][2] = {{0, 2}, {1, 3}, {4, 5}};     // heights, weights
    for (int t = 0; t < 3; ++t) {
        uint16_t* height = words + slot[t][0] * samples;
        uint16_t* weight = words + slot[t][1] * samples;
        targets[t].heights.first = height;
        targets[t].heights.last = targets[t].heights.end = height + samples;
        targets[t].weights.first = weight;
        targets[t].weights.last = targets[t].weights.end = weight + samples;
    }

    alignas(16) unsigned char rasterizers[3][0x30] = {};
    const auto rasterInit = reinterpret_cast<AlignRasterInitFn>(base + kAlignRasterInit);
    const auto rasterTriangle = reinterpret_cast<AlignRasterTriangleFn>(base + kAlignRasterTriangle);
    for (int t = 0; t < 3; ++t) rasterInit(rasterizers[t], &targets[t], box, box + 2, size, 1);

    static const float kDefaultWeights[3] = {1.0f, 1.0f, 1.0f};   // the stock (1,1,1) scratch vector
    for (const uint8_t* const* item = alignments->first; item != alignments->last; ++item) {
        const uint8_t* alignment = *item;
        uintptr_t begin = 0, end = 0, weightBegin = 0, weightEnd = 0;
        memcpy(&begin, alignment + kAlignTriangles, sizeof begin);
        memcpy(&end, alignment + kAlignTriangles + sizeof(void*), sizeof end);
        memcpy(&weightBegin, alignment + kAlignWeights, sizeof weightBegin);
        memcpy(&weightEnd, alignment + kAlignWeights + sizeof(void*), sizeof weightEnd);
        const int32_t count = int32_t((end - begin) / 36);
        if (count <= 0) continue;
        int32_t type = 0;
        memcpy(&type, alignment + kAlignType, sizeof type);
        AlignTarget& target = targets[type];
        void* rasterizer = rasterizers[type];
        for (int32_t i = 0; i < count; ++i) {
            const uint8_t* vertex = reinterpret_cast<const uint8_t*>(begin) + size_t(i) * 36;
            const uint8_t* weight = weightBegin == weightEnd
                ? reinterpret_cast<const uint8_t*>(kDefaultWeights)
                : reinterpret_cast<const uint8_t*>(weightBegin) + size_t(i) * 12;
            memcpy(target.triangle + 0x00, vertex + 0x18, 12);   // +0x08: vertex 2
            memcpy(target.triangle + 0x0c, vertex + 0x0c, 12);   // +0x14: vertex 1
            memcpy(target.triangle + 0x18, vertex + 0x00, 12);   // +0x20: vertex 0
            memcpy(target.triangle + 0x24, weight + 8, 4);       // +0x2c: weight of vertex 2
            memcpy(target.triangle + 0x28, weight + 4, 4);       // +0x30: weight of vertex 1
            memcpy(target.triangle + 0x2c, weight + 0, 4);       // +0x34: weight of vertex 0
            AlignVec2 second{}, first{}, zeroth{};
            memcpy(&second, vertex + 0x18, 8);
            memcpy(&first, vertex + 0x0c, 8);
            memcpy(&zeroth, vertex + 0x00, 8);
            rasterTriangle(rasterizer, second, first, zeroth);
        }
    }

    AlignBlendConstants k;
    k.scale = _mm_set1_ps(scale);
    k.offset = _mm_set1_ps(offset);
    k.invScale = _mm_set1_ps(invScale);
    k.zero = _mm_setzero_ps();
    k.one = _mm_set1_ps(1.0f);
    k.half = _mm_set1_ps(0.5f);
    k.full = _mm_set1_ps(65535.0f);
    const int64_t failed = AlignBlend(result->first, words, samples, k);
    if (failed >= 0) {          // unreachable: see the exactness argument in the doc
        const auto fail = reinterpret_cast<AlignAssertFn>(base + kAlignAssert);
        fail(reinterpret_cast<const char*>(base + kAlignAssertExpr),
             reinterpret_cast<const char*>(base + kAlignAssertFile), 0x4d7,
             reinterpret_cast<const char*>(base + kAlignAssertFunction));
    }
}

static void CalculateHeightModDetour(const float* box, const int32_t* size, float scale,
    float offset, const AlignPointerVector* alignments, AlignU16Vector* result)
{
    CalculateHeightModImpl(g_originalCalculateHeightMod, g_terrainAlignBase, box, size, scale,
                           offset, alignments, result);
}
