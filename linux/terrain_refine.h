// Native Steam Linux 35924 InternBicubicRefine at RVA 0xd9b850.
// Port of the Windows kernel; CVec3f uses SysV xmm0/xmm1 aggregate passing.
// linux/tests/terrain_refine_oracle.cpp executes the unchanged Linux code.
// Called once per BaseGetHeightmapRefined (0x3c4620) from TerrainAlignment
// ThreadPool workers; refines the 4 m base heightmap into the 1 m/2 m cache.
//
// Bit-identical replacement (docs/terrain-refine.md): every float value is
// produced by the same IEEE single-precision operation on the same operands
// in the same order as the stock code. What changed:
//  * i/k and j/k (and their squares/cubes) are computed once per call
//    instead of per cell (20 divisions per cell at k=4);
//  * the two CMat4f products (0x2fadc0) with the constant Hermite matrices are
//    inlined; only terms multiplied by exactly 0.0 or 1.0 are dropped. With
//    finite inputs such a drop can change nothing but the sign of a zero, and
//    no later +, -, * or the final truncation can observe that sign;
//  * scalar ops on independent values are packed 4-wide (mulps/addps are the
//    same per-lane operations as mulss/addss). No FMA, no reassociation.
// Cells are visited in stock order, each cell reads its samples (and the
// vector's data pointer) before writing its pixels, so even aliasing buffers
// behave like the original. Stock asserts and k > 64 go to the original.
#pragma once
#include <emmintrin.h>
#include <cstdint>
#include <string.h>

struct RefineScale { float x,y,z; };
struct BicubicRefineVector { const uint16_t* first; const uint16_t* last; const uint16_t* end; };
using BicubicRefineFn = void (*)(int, const BicubicRefineVector*, int, int, int, int, int,
    RefineScale, uint16_t*, int, int, int);
static BicubicRefineFn g_originalBicubicRefine = nullptr;

static const int kBicubicRefineMaxFactor = 64;

static void BicubicRefineImpl(BicubicRefineFn original, int k, const BicubicRefineVector* src,
    int srcDim, int x0, int y0, int x1, int y1, RefineScale scale, uint16_t* out,
    int stride, int dx, int dy)
{
    // Stock asserts: k > 1 && k % 2 == 0, x0 >= 0, y0 >= 0, x1 <= srcDim,
    // x1 >= x0, y1 >= y0. Those inputs keep the original's assert behaviour.
    if (k < 2 || (k & 1) || x0 < 0 || y0 < 0 || x1 > srcDim || x1 < x0 || y1 < y0 ||
        k > kBicubicRefineMaxFactor) {
        original(k, src, srcDim, x0, y0, x1, y1, scale, out, stride, dx, dy);
        return;
    }
    if (y0 >= y1 - 1 || x0 >= x1 - 1) return;   // stock loops run zero times

    // t = float(i)/float(k), t*t, (t*t)*t: the stock per-row and per-pixel values.
    alignas(16) float t1[kBicubicRefineMaxFactor + 4] = {};
    alignas(16) float t2[kBicubicRefineMaxFactor + 4] = {};
    alignas(16) float t3[kBicubicRefineMaxFactor + 4] = {};
    const __m128 kf = _mm_cvtsi32_ss(_mm_setzero_ps(), k);
    for (int i = 0; i < k; ++i) {
        const __m128 t = _mm_div_ss(_mm_cvtsi32_ss(_mm_setzero_ps(), i), kf);
        const __m128 tt = _mm_mul_ss(t, t);
        _mm_store_ss(t1 + i, t);
        _mm_store_ss(t2 + i, tt);
        _mm_store_ss(t3 + i, _mm_mul_ss(tt, t));
    }

    // Stock index arithmetic, including its 32-bit products and sign extension.
    const uint32_t uk = uint32_t(k), half = uint32_t(k >> 1);
    const uint32_t rowAdjust = uint32_t(dy) - uk * uint32_t(y0);
    const uint32_t colAdjust = uint32_t(dx) - uk * uint32_t(x0);
    const int64_t base0 = int32_t(uint32_t(srcDim) * uint32_t(y0) + uint32_t(x0));
    const int64_t off1 = int64_t(int32_t(uint32_t(y0 + 1) * uint32_t(srcDim) + uint32_t(x0))) - base0;
    const int64_t off2 = int64_t(int32_t(uint32_t(y0 + 2) * uint32_t(srcDim) + uint32_t(x0))) - base0;
    const uintptr_t outStep = uintptr_t(2 * int64_t(stride));

    const __m128 quarter = _mm_set1_ps(0.25f), halfF = _mm_set1_ps(0.5f);
    const __m128 m3 = _mm_set1_ps(-3.0f), p3 = _mm_set1_ps(3.0f);
    const __m128 m2 = _mm_set1_ps(-2.0f), p2 = _mm_set1_ps(2.0f);

    for (int y = y0; y < y1 - 1; ++y) {
        const int64_t row = base0 + int64_t(y - y0) * int64_t(srcDim);
        const uint32_t rowTerm = uk * uint32_t(y) + half + rowAdjust;
        for (int x = x0; x < x1 - 1; ++x) {
            const uintptr_t at = uintptr_t(src->first) + 2 * uintptr_t(row + (x - x0));
            const uint16_t* r0 = reinterpret_cast<const uint16_t*>(at);
            const uint16_t* r1 = reinterpret_cast<const uint16_t*>(at + 2 * uintptr_t(off1));
            const uint16_t* r2 = reinterpret_cast<const uint16_t*>(at + 2 * uintptr_t(off2));
            const float a00 = float(int(r0[0])), a01 = float(int(r0[1])), a02 = float(int(r0[2]));
            const float a10 = float(int(r1[0])), a11 = float(int(r1[1])), a12 = float(int(r1[2]));
            const float a20 = float(int(r2[0])), a21 = float(int(r2[1])), a22 = float(int(r2[2]));

            // G (stock 0x3ac920..0x3acb98), four lanes per operation:
            // S = G0,G1,G4,G5  H = G2,G3,G6,G7  V = G8,G9,G12,G13  X = G10,G11,G14,G15
            const __m128 tl = _mm_setr_ps(a00, a01, a10, a11);
            const __m128 tr = _mm_setr_ps(a01, a02, a11, a12);
            const __m128 bl = _mm_setr_ps(a10, a11, a20, a21);
            const __m128 br = _mm_setr_ps(a11, a12, a21, a22);
            const __m128 s = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_add_ps(tr, tl), bl), br), quarter);
            const __m128 hTop = _mm_sub_ps(tr, tl), hBottom = _mm_sub_ps(br, bl);
            const __m128 h = _mm_mul_ps(_mm_add_ps(hTop, hBottom), halfF);
            const __m128 xx = _mm_sub_ps(hBottom, hTop);
            const __m128 v = _mm_mul_ps(_mm_add_ps(_mm_sub_ps(bl, tl), _mm_sub_ps(br, tr)), halfF);
            const __m128 g0 = _mm_shuffle_ps(s, v, _MM_SHUFFLE(2, 0, 2, 0));   // G[r][0]
            const __m128 g1 = _mm_shuffle_ps(s, v, _MM_SHUFFLE(3, 1, 3, 1));   // G[r][1]
            const __m128 g2 = _mm_shuffle_ps(h, xx, _MM_SHUFFLE(2, 0, 2, 0));  // G[r][2]
            const __m128 g3 = _mm_shuffle_ps(h, xx, _MM_SHUFFLE(3, 1, 3, 1));  // G[r][3]

            // T = G*M1: T[r][c] = ((G[r][0]M1[0][c] + G[r][1]M1[1][c]) + G[r][2]M1[2][c]) + G[r][3]M1[3][c]
            // M1 columns: c0 (1,0,0,0) c1 (0,0,1,0) c2 (-3,3,-2,-1) c3 (2,-2,1,1)
            __m128 tc0 = g0, tc1 = g2;
            __m128 tc2 = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(g0, m3), _mm_mul_ps(g1, p3)),
                                               _mm_mul_ps(g2, m2)), g3);
            __m128 tc3 = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(g0, p2), _mm_mul_ps(g1, m2)), g2), g3);
            _MM_TRANSPOSE4_PS(tc0, tc1, tc2, tc3);   // now rows T[0..3][*]
            // C = M2*T: C[r][c] = ((M2[r][0]T[0][c] + M2[r][1]T[1][c]) + M2[r][2]T[2][c]) + M2[r][3]T[3][c]
            // M2 rows: (1,0,0,0) (0,0,1,0) (-3,3,-2,-1) (2,-2,1,1)
            const __m128 c0 = tc0, c1 = tc2;
            const __m128 c2 = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(tc0, m3), _mm_mul_ps(tc1, p3)),
                                                    _mm_mul_ps(tc2, m2)), tc3);
            const __m128 c3 = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(tc0, p2), _mm_mul_ps(tc1, m2)), tc2), tc3);

            const uint32_t pixel = rowTerm * uint32_t(stride) + uk * uint32_t(x) + half + colAdjust;
            uintptr_t dst = uintptr_t(out) + 2 * uintptr_t(int64_t(int32_t(pixel)));
            for (int i = 0; i < k; ++i, dst += outStep) {
                // P[c] = ((C[1][c]t + C[0][c]) + t2 C[2][c]) + C[3][c]t3
                const __m128 ti = _mm_set1_ps(t1[i]), ti2 = _mm_set1_ps(t2[i]), ti3 = _mm_set1_ps(t3[i]);
                const __m128 p = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(c1, ti), c0),
                                                       _mm_mul_ps(ti2, c2)), _mm_mul_ps(c3, ti3));
                const __m128 pp0 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(0, 0, 0, 0));
                const __m128 pp1 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(1, 1, 1, 1));
                const __m128 pp2 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 2, 2, 2));
                const __m128 pp3 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(3, 3, 3, 3));
                const auto row16 = reinterpret_cast<uint16_t*>(dst);
                for (int j = 0; j < k; j += 4) {
                    // value = ((P1 s + P0) + s2 P2) + P3 s3, stored as the low
                    // 16 bits of cvttss2si (no saturation; 0x80000000 -> 0).
                    const __m128 sj = _mm_load_ps(t1 + j), sj2 = _mm_load_ps(t2 + j), sj3 = _mm_load_ps(t3 + j);
                    const __m128 value = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(pp1, sj), pp0),
                                                               _mm_mul_ps(sj2, pp2)), _mm_mul_ps(pp3, sj3));
                    const __m128i n = _mm_cvttps_epi32(value);
                    const __m128i wide = _mm_srai_epi32(_mm_slli_epi32(n, 16), 16);   // in int16 range
                    const __m128i low = _mm_packs_epi32(wide, wide);
                    if (k - j >= 4) {
                        _mm_storel_epi64(reinterpret_cast<__m128i*>(row16 + j), low);
                    } else {
                        const int32_t two = _mm_cvtsi128_si32(low);   // k is even: tail is 2
                        memcpy(row16 + j, &two, 4);
                    }
                }
            }
        }
    }
}

inline void TerrainRefineFast(int k, const BicubicRefineVector* src, int srcDim,
    int x0, int y0, int x1, int y1, RefineScale scale, uint16_t* out, int stride, int dx, int dy) {
    BicubicRefineImpl(g_originalBicubicRefine,k,src,srcDim,x0,y0,x1,y1,scale,out,stride,dx,dy);
}
