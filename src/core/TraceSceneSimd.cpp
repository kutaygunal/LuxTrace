// The four-wide leaf intersection.
//
// This translation unit is compiled for AVX2 and nothing else in the project
// is. Everything it touches is in this file: no shared inline helper is
// instantiated here, because the same function compiled twice under different
// instruction sets is exactly the way two halves of a program come to disagree
// about a floating-point result.
//
// Every arithmetic step below is written in the same order and the same
// associations as intersectTriangle() in TraceScene.cpp, with explicit _mul and
// _add intrinsics rather than source-level `a * b + c`, so the compiler has no
// multiply-add to fuse and no intermediate to re-round. That is what makes the
// wide path bit-identical rather than merely close, and there is a test that
// says so over a million random rays.
#include "TraceSceneSimd.h"

#include <immintrin.h>

namespace simd {
namespace {

// The same constants intersectTriangle() uses. Repeated rather than shared,
// because sharing them would mean including a header this unit must not pull
// in; the test that compares the two paths is what keeps them honest.
constexpr double kParallelDet = 1e-12;
constexpr double kBaryEps     = 1e-9;

inline __m256d dot3(__m256d ax, __m256d ay, __m256d az,
                    __m256d bx, __m256d by, __m256d bz) {
    // (x*bx + y*by) + z*bz -- left to right, as Vec3::dot evaluates it.
    return _mm256_add_pd(_mm256_add_pd(_mm256_mul_pd(ax, bx), _mm256_mul_pd(ay, by)),
                         _mm256_mul_pd(az, bz));
}

} // namespace

void intersect4(const Tri4& p, const double o[3], const double d[3],
                double t[4], double u[4], double v[4], bool hit[4]) {
    const __m256d v0x = _mm256_load_pd(p.v0[0]);
    const __m256d v0y = _mm256_load_pd(p.v0[1]);
    const __m256d v0z = _mm256_load_pd(p.v0[2]);
    const __m256d e1x = _mm256_load_pd(p.e1[0]);
    const __m256d e1y = _mm256_load_pd(p.e1[1]);
    const __m256d e1z = _mm256_load_pd(p.e1[2]);
    const __m256d e2x = _mm256_load_pd(p.e2[0]);
    const __m256d e2y = _mm256_load_pd(p.e2[1]);
    const __m256d e2z = _mm256_load_pd(p.e2[2]);

    const __m256d ox = _mm256_set1_pd(o[0]);
    const __m256d oy = _mm256_set1_pd(o[1]);
    const __m256d oz = _mm256_set1_pd(o[2]);
    const __m256d dx = _mm256_set1_pd(d[0]);
    const __m256d dy = _mm256_set1_pd(d[1]);
    const __m256d dz = _mm256_set1_pd(d[2]);

    // pv = d x e2
    const __m256d pvx = _mm256_sub_pd(_mm256_mul_pd(dy, e2z), _mm256_mul_pd(dz, e2y));
    const __m256d pvy = _mm256_sub_pd(_mm256_mul_pd(dz, e2x), _mm256_mul_pd(dx, e2z));
    const __m256d pvz = _mm256_sub_pd(_mm256_mul_pd(dx, e2y), _mm256_mul_pd(dy, e2x));

    const __m256d det = dot3(e1x, e1y, e1z, pvx, pvy, pvz);

    // |det| >= kParallelDet, computed as a sign-mask clear so a negative zero
    // behaves the way std::fabs does.
    const __m256d absMask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
    const __m256d absDet  = _mm256_and_pd(det, absMask);
    __m256d alive = _mm256_cmp_pd(absDet, _mm256_set1_pd(kParallelDet), _CMP_GE_OQ);

    // A lane the test already rejected must not divide by its own zero: the
    // result would be an infinity that the later comparisons handle correctly
    // but that a floating-point exception mask would not.
    const __m256d safeDet = _mm256_blendv_pd(_mm256_set1_pd(1.0), det, alive);
    const __m256d inv     = _mm256_div_pd(_mm256_set1_pd(1.0), safeDet);

    // s = o - v0
    const __m256d sx = _mm256_sub_pd(ox, v0x);
    const __m256d sy = _mm256_sub_pd(oy, v0y);
    const __m256d sz = _mm256_sub_pd(oz, v0z);

    const __m256d uu = _mm256_mul_pd(dot3(sx, sy, sz, pvx, pvy, pvz), inv);
    const __m256d negEps = _mm256_set1_pd(-kBaryEps);
    const __m256d onePlus = _mm256_set1_pd(1.0 + kBaryEps);
    alive = _mm256_and_pd(alive, _mm256_cmp_pd(uu, negEps,  _CMP_GE_OQ));
    alive = _mm256_and_pd(alive, _mm256_cmp_pd(uu, onePlus, _CMP_LE_OQ));

    // q = s x e1
    const __m256d qx = _mm256_sub_pd(_mm256_mul_pd(sy, e1z), _mm256_mul_pd(sz, e1y));
    const __m256d qy = _mm256_sub_pd(_mm256_mul_pd(sz, e1x), _mm256_mul_pd(sx, e1z));
    const __m256d qz = _mm256_sub_pd(_mm256_mul_pd(sx, e1y), _mm256_mul_pd(sy, e1x));

    const __m256d vv = _mm256_mul_pd(dot3(dx, dy, dz, qx, qy, qz), inv);
    alive = _mm256_and_pd(alive, _mm256_cmp_pd(vv, negEps, _CMP_GE_OQ));
    alive = _mm256_and_pd(alive,
                          _mm256_cmp_pd(_mm256_add_pd(uu, vv), onePlus, _CMP_LE_OQ));

    const __m256d tt = _mm256_mul_pd(dot3(e2x, e2y, e2z, qx, qy, qz), inv);
    alive = _mm256_and_pd(alive, _mm256_cmp_pd(tt, _mm256_setzero_pd(), _CMP_GE_OQ));

    alignas(32) double        tOut[4], uOut[4], vOut[4];
    alignas(32) long long int mask[4];
    _mm256_store_pd(tOut, tt);
    _mm256_store_pd(uOut, uu);
    _mm256_store_pd(vOut, vv);
    // Read as bits, not as a double: a passing comparison is all-ones, which is
    // a NaN, and asking whether a NaN differs from zero is a question about the
    // compiler rather than about the geometry.
    _mm256_store_si256(reinterpret_cast<__m256i*>(mask), _mm256_castpd_si256(alive));

    for (int i = 0; i < 4; ++i) {
        t[i]   = tOut[i];
        u[i]   = uOut[i];
        v[i]   = vOut[i];
        hit[i] = (mask[i] != 0) && (p.tri[i] >= 0);
    }
}

} // namespace simd
