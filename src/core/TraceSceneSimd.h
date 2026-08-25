#pragma once
#include <cstddef>

// A four-wide leaf intersection.
//
// The hierarchy's leaves hold four triangles and used to test them one at a
// time in scalar double. The data layout is already exactly what a 4-wide AVX2
// double intersection wants -- (v0, e1, e2) per triangle, four of them -- so
// this is the option the layout favours, and it is the one that had not been
// tried. (The node header records the two that were: float bounds rounded
// outward, and a four-wide node with the slab tests as lanes. Both were built,
// benchmarked across the whole scene library, and lost.)
//
// Two properties are non-negotiable and both are tested:
//
//   The answer is *identical* to the scalar loop, bit for bit. The arithmetic
//   is written in the same order and the same associations as Moller-Trumbore
//   in TraceScene.cpp, using explicit mul and add intrinsics so no fused
//   multiply-add can quietly re-round an intermediate; and the caller picks the
//   winner by walking the four lanes in index order, which is what makes an
//   exact tie resolve the way the scalar loop resolved it.
//
//   The scalar path stays. It is what the brute-force reference walk uses, so
//   the equivalence test still means something, and it is what runs on a
//   machine without AVX2.
namespace simd {

// Four triangles, structure of arrays. Aligned so the loads are aligned loads.
struct alignas(32) Tri4 {
    // [component][lane]
    double v0[3][4] = {};
    double e1[3][4] = {};
    double e2[3][4] = {};
    int    tri[4]   = {-1, -1, -1, -1};   // index into the primitive array
};

// Whether the wide path may be taken: this build has the kernel, and this CPU
// can run it. Answered once and cached.
//
// Deliberately *not* in the translation unit the kernel lives in: that one is
// compiled for AVX2, and a probe that has to run before the check has been made
// cannot be allowed anywhere near it.
bool avx2Available();

// Moller-Trumbore over four triangles at once.
//
// Fills every lane; `hit` says which of them produced a real intersection. The
// selection is the caller's, in lane order, because that is what the scalar
// loop did and an exact tie has to break the same way.
void intersect4(const Tri4& p, const double o[3], const double d[3],
                double t[4], double u[4], double v[4], bool hit[4]);

} // namespace simd
