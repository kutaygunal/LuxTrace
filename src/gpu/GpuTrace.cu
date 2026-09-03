#include "gpu/GpuKernel.h"

#include <cuda_runtime.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

// The preview kernel.
//
// Single precision throughout, because a consumer GPU runs doubles at a
// sixty-fourth of its float rate and a preview that is not fast is not a
// preview. That is a real difference from the reference, not a rounding one,
// and it is exactly the kind of difference the comparison exists to size: if
// float is not enough for a scene, the receiver map says so in sigmas rather
// than somebody guessing.
//
// One branch per interface instead of two. The reference splits a refractive
// hit into a reflected and a transmitted branch and follows both; this samples
// one with probability equal to its share of the energy and carries the full
// weight down it. Both converge to the same answer -- the second is the
// standard unbiased estimator of the first -- and the tree the reference walks
// does not fit in a GPU thread.
namespace gpukernel {
namespace {

// The reference's own limits, and they have to be the reference's: an
// integrating sphere takes well over a hundred diffuse bounces before its
// light finds the port, and a preview that gave up at sixty-four booked
// three per cent of the sphere as truncated where the reference booked two
// hundredths of one. That is not a faster answer, it is a different one.
constexpr int kMaxDepth   = 160;
constexpr int kStackDepth = 64;    // BVH traversal, not path depth
// Cemented doublets, immersed optics and clad guides need two or three;
// eight is past anything real, and is what the reference carries.
constexpr int kMediumMax  = 8;
// How many receivers one connection may reach. The reference carries the
// same eight, and a scene with more of them is one nobody has built.
constexpr int kMaxNeeDets = 8;
// Where a branch stops being followed outright and starts being followed
// with a probability. The reference own threshold, in the same normalised
// units: every ray is emitted carrying one.
constexpr float kRoulette = 1e-4f;
// Two angular draws and two areal ones is the whole of the emission stream,
// and the host uploads direction numbers for exactly that many.
constexpr int kSobolDims  = 8;

// ---------------------------------------------------------------------------
// small vector helpers
// ---------------------------------------------------------------------------

__device__ __forceinline__ float3 mk(float x, float y, float z) { return make_float3(x, y, z); }
__device__ __forceinline__ float3 add3(float3 a, float3 b) { return mk(a.x+b.x, a.y+b.y, a.z+b.z); }
__device__ __forceinline__ float3 sub3(float3 a, float3 b) { return mk(a.x-b.x, a.y-b.y, a.z-b.z); }
__device__ __forceinline__ float3 mul3(float3 a, float s)  { return mk(a.x*s, a.y*s, a.z*s); }
__device__ __forceinline__ float  dot3(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
__device__ __forceinline__ float3 cross3(float3 a, float3 b) {
    return mk(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
__device__ __forceinline__ float3 norm3(float3 a) {
    return mul3(a, 1.0f / sqrtf(fmaxf(1e-30f, dot3(a, a))));
}
__device__ __forceinline__ float3 ld3(const float* p) { return mk(p[0], p[1], p[2]); }

// ---------------------------------------------------------------------------
// rng
// ---------------------------------------------------------------------------

__device__ __forceinline__ unsigned long long mix64(unsigned long long x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
__device__ __forceinline__ float rnd(unsigned long long& s) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    // 24 bits is every bit a float can hold in [0,1), so nothing is thrown away
    // and nothing is invented.
    return float(mix64(s) >> 40) * (1.0f / 16777216.0f);
}

// ---------------------------------------------------------------------------
// traversal
// ---------------------------------------------------------------------------

// Which triangle, and -- when the scene has a two-level hierarchy -- which
// placement of which part it belongs to. `inst` of -1 is geometry that is
// not instanced, which is every triangle in most scenes.
struct Hit { int tri; int inst; float t, u, v; };

__device__ __forceinline__ bool slab(const Node& n, float3 o, float3 inv,
                                     float tMin, float tMax, float& entry) {
    float t0 = (n.bmin[0] - o.x) * inv.x, t1 = (n.bmax[0] - o.x) * inv.x;
    float lo = fminf(t0, t1), hi = fmaxf(t0, t1);
    t0 = (n.bmin[1] - o.y) * inv.y; t1 = (n.bmax[1] - o.y) * inv.y;
    lo = fmaxf(lo, fminf(t0, t1)); hi = fminf(hi, fmaxf(t0, t1));
    t0 = (n.bmin[2] - o.z) * inv.z; t1 = (n.bmax[2] - o.z) * inv.z;
    lo = fmaxf(lo, fminf(t0, t1)); hi = fminf(hi, fmaxf(t0, t1));
    lo = fmaxf(lo, tMin); hi = fminf(hi, tMax);
    entry = lo;
    return lo <= hi;
}

// Bins a direction leaving the system into the master far-field grid. The same
// mapping the reference uses, so the two accumulate into the same rings.
__device__ __forceinline__ void binDirection(double* angles, double* anglesSq,
                                             int nTheta, int nPhi,
                                             float3 d, double energy) {
    if (!angles || nTheta <= 0) return;
    const float cz = fminf(1.0f, fmaxf(-1.0f, d.z));
    const float theta = acosf(cz);
    float phi = atan2f(d.y, d.x);
    if (phi < 0.0f) phi += 6.2831853071795864f;
    int it = int(theta / 3.14159265358979324f * float(nTheta));
    int ip = int(phi / 6.2831853071795864f * float(nPhi));
    it = min(max(it, 0), nTheta - 1);
    ip = min(max(ip, 0), nPhi - 1);
    const size_t k = (size_t)it * nPhi + ip;
    atomicAdd(angles + k, energy);
    if (anglesSq) atomicAdd(anglesSq + k, energy * energy);
}

// The per-ray half of a watertight intersection: which axis the ray runs most
// along, and the shear that makes it the unit z of a space the triangle test is
// done in. Computed once per traversal, like the reciprocal direction beside it.
struct RayShear {
    int   kx, ky, kz;
    float Sx, Sy, Sz;
};

__device__ __forceinline__ float axis3(float3 v, int k) {
    return (k == 0) ? v.x : ((k == 1) ? v.y : v.z);
}

__device__ __forceinline__ RayShear shearOf(float3 d) {
    RayShear r;
    const float ax = fabsf(d.x), ay = fabsf(d.y), az = fabsf(d.z);
    r.kz = (ax > ay) ? ((ax > az) ? 0 : 2) : ((ay > az) ? 1 : 2);
    r.kx = (r.kz + 1 == 3) ? 0 : r.kz + 1;
    r.ky = (r.kx + 1 == 3) ? 0 : r.kx + 1;
    const float dz = axis3(d, r.kz);
    // Winding is preserved by swapping the two minor axes rather than by
    // negating anything, which keeps every product below exact under negation.
    if (dz < 0.0f) { const int t = r.kx; r.kx = r.ky; r.ky = t; }
    r.Sx = axis3(d, r.kx) / dz;
    r.Sy = axis3(d, r.ky) / dz;
    r.Sz = 1.0f / dz;
    return r;
}

// Watertight ray/triangle intersection -- Woop, Benthin and Wald, JCGT 2013 --
// rather than the Moller-Trumbore the reference walks.
//
// The reference is watertight enough in double: its edge tests carry a 1e-9
// slack, which is five million ulp there and enough that no Monte Carlo run
// lands in the window where two neighbouring triangles could both reject a ray.
// In float the same slack is eighty ulp, and a ray sliding along the inside of
// an integrating sphere crosses hundreds of shared edges per path and finds the
// window. It then leaves through the wall of a closed cavity the reference
// never lets anything escape from -- five parts per million of the flux, which
// is small and is not zero, and the whole claim of this backend is that its
// disagreements with the reference are sampling and nothing else.
//
// The property that fixes it: each edge is tested by a two-term determinant
// built from the two vertices that edge joins, translated by the ray origin and
// sheared by quantities that depend only on the ray. The neighbouring triangle
// sees the same edge with its endpoints in the opposite order, so its
// determinant is the exact negation of this one -- floating point negates
// exactly -- and the two signs can never both say "outside". A ray through a
// shared edge hits one triangle or both, never neither.
//
// This needs the two triangles to agree bit-for-bit about where the shared
// vertices are, which is why the host rounds each edge from the rounded
// vertices rather than rounding the edge itself.
__device__ __forceinline__ bool intersectTri(const Tri& t, float3 o, const RayShear& rs,
                                             float tMin, float best,
                                             float& tOut, float& uOut, float& vOut) {
    const float3 v0 = ld3(t.v0);
    const float3 A = sub3(v0, o);
    const float3 B = sub3(add3(v0, ld3(t.e1)), o);
    const float3 C = sub3(add3(v0, ld3(t.e2)), o);

    const float Az = rs.Sz * axis3(A, rs.kz);
    const float Bz = rs.Sz * axis3(B, rs.kz);
    const float Cz = rs.Sz * axis3(C, rs.kz);
    const float Ax = axis3(A, rs.kx) - rs.Sx * axis3(A, rs.kz);
    const float Ay = axis3(A, rs.ky) - rs.Sy * axis3(A, rs.kz);
    const float Bx = axis3(B, rs.kx) - rs.Sx * axis3(B, rs.kz);
    const float By = axis3(B, rs.ky) - rs.Sy * axis3(B, rs.kz);
    const float Cx = axis3(C, rs.kx) - rs.Sx * axis3(C, rs.kz);
    const float Cy = axis3(C, rs.ky) - rs.Sy * axis3(C, rs.kz);

    // One scaled barycentric per edge. Zero is a hit on the edge and is kept:
    // both neighbours then report it and the nearer one wins on `best`, which
    // is the harmless side of the choice.
    const float U = Cx * By - Cy * Bx;
    const float V = Ax * Cy - Ay * Cx;
    const float W = Bx * Ay - By * Ax;
    if ((U < 0.0f || V < 0.0f || W < 0.0f) &&
        (U > 0.0f || V > 0.0f || W > 0.0f)) return false;

    const float det = U + V + W;
    if (det == 0.0f) return false;                 // the ray lies in the plane

    const float T   = U * Az + V * Bz + W * Cz;
    const float inv = 1.0f / det;
    const float tt  = T * inv;
    if (!(tt > tMin) || !(tt < best)) return false;

    // U, V, W weight A, B and C, and the caller's u and v are the weights of
    // v0 + e1 and v0 + e2 -- so the shading normals and the barycentric hit
    // point below read them exactly as Moller-Trumbore left them.
    tOut = tt;
    uOut = V * inv;
    vOut = W * inv;
    return true;
}

// Which of the media a branch is inside actually governs it. Where two
// solids overlap the higher priority wins; ties go to the innermost entry,
// which is the ordinary nesting case. -1 is vacuum.
struct DevScene;
__device__ __forceinline__ int effectiveMedium(const DevScene& sc,
                                               const int* stack, int n);

struct DevScene {
    const Tri*   tris;
    const Shade* shade;
    const Node*  nodes;
    const int*   order;
    const Surf*  surfs;
    const unsigned int* sobolDir;   // kSobolDims x 32 direction numbers
    const float* tableBeta;
    const float* tableValue;
    const Tri*   blasTris;
    const Shade* blasShade;
    const Node*  blasNodes;
    const int*   blasOrder;
    const Blas*  blas;
    const Inst*  insts;
    const Node*  tlas;
    const int*   tlasOrder;
    const Det*   dets;
    const int*   detOfSurf;
    int          nDets;
    int          nDetOfSurf;
    int          nNodes;
    int          nTlas;
    int          nextEvent;
    // What the scene occupies, so emission can be aimed into it.
    float3       boundsCentre;
    float        boundsRadius;
    int          aim;
    // The run spectrum, and every wavelength-dependent surface quantity
    // sampled across the band the host resolved it over.
    const float* invCdf;
    const float* specTable;
    int          spdBins;
    int          spdMono;
    int          spdRgb;
    float        monoLambda;
    float        meanV;
    int          unitLumen;
    int          spectral;
    // Whether this run carries a Stokes state per branch, and the state the
    // source emits. Four more floats per branch and a frame rotation per
    // interface, so it is carried only when asked for.
    int          polarised;
    float4       emittedState;
    int          specSamples;
    float        specMinNm, specMaxNm;
    size_t       bandCells;
    Src          src;
};

// One placement of a part, and where that part's own arrays begin.
//
// The scene carries a two-level hierarchy: parts are tessellated once and
// placed many times, which is what lets a five-by-five microlens array keep the
// same fine mesh every other optic in the library has instead of the coarsened
// one twenty-five copies would need. Walking only the single-level path meant
// refusing both scenes built that way.
__device__ bool hitInstances(const DevScene& sc, float3 o, float3 d,
                             float tMin, float& best, Hit& hit) {
    if (sc.nTlas <= 0) return false;
    const float3 inv = mk(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
    bool found = false;

    int stack[kStackDepth];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node n = sc.tlas[stack[--sp]];
        float e;
        if (!slab(n, o, inv, tMin, best, e)) continue;

        if (n.count <= 0) {
            if (sp + 2 <= kStackDepth) { stack[sp++] = n.right; stack[sp++] = n.left; }
            continue;
        }
        for (int k = n.left; k < n.left + n.count; ++k) {
            const Inst in = sc.insts[sc.tlasOrder[k]];
            // The placement's own box first: transforming a ray that misses it
            // would be work spent to find that out.
            Node ib;
            for (int a = 0; a < 3; ++a) { ib.bmin[a] = in.bmin[a]; ib.bmax[a] = in.bmax[a]; }
            float ie;
            if (!slab(ib, o, inv, tMin, best, ie)) continue;

            // Into the part's own frame. The transform is rigid, so a distance
            // means the same thing on both sides of it and `best` carries
            // across untouched.
            const float3 q  = sub3(o, ld3(in.origin));
            const float3 lo = mk(dot3(ld3(in.i0), q), dot3(ld3(in.i1), q),
                                 dot3(ld3(in.i2), q));
            const float3 ld = mk(dot3(ld3(in.i0), d), dot3(ld3(in.i1), d),
                                 dot3(ld3(in.i2), d));
            const Blas bl = sc.blas[in.blas];
            if (bl.nodeCount <= 0) continue;

            const float3 linv = mk(1.0f / ld.x, 1.0f / ld.y, 1.0f / ld.z);
            const RayShear lrs = shearOf(ld);
            int bstack[kStackDepth];
            int bsp = 0;
            bstack[bsp++] = 0;
            while (bsp > 0) {
                const Node bn = sc.blasNodes[bl.nodeFirst + bstack[--bsp]];
                float be;
                if (!slab(bn, lo, linv, tMin, best, be)) continue;
                if (bn.count <= 0) {
                    if (bsp + 2 <= kStackDepth) {
                        bstack[bsp++] = bn.right;
                        bstack[bsp++] = bn.left;
                    }
                    continue;
                }
                for (int j = 0; j < bn.count; ++j) {
                    const int ti = sc.blasOrder[bl.orderFirst + bn.left + j];
                    const Tri t  = sc.blasTris[bl.triFirst + ti];
                    float tt, tu, tv;
                    if (!intersectTri(t, lo, lrs, tMin, best, tt, tu, tv)) continue;
                    best = tt;
                    hit.tri  = ti;
                    hit.inst = sc.tlasOrder[k];
                    hit.t    = tt;
                    hit.u    = tu;
                    hit.v    = tv;
                    found = true;
                }
            }
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// polarisation
//
// A Stokes vector and the Mueller matrices that act on it, ported term for term
// from src/core/Polarisation. Carried only when a run asks for it -- four more
// floats per branch and a frame rotation per interface -- and the reason it is
// worth carrying is that the unpolarised average cannot express any of what it
// is for: s-polarised light meeting glass at Brewster reflects strongly and
// p-polarised light reflects nothing at all, and a stack of plates is a
// polariser only because of that difference.
//
// A Stokes vector means nothing without a frame. The plane of incidence turns
// from one surface to the next, so the state is rotated into the new plane
// before the interface acts on it -- which is the step that is easy to leave out
// and impossible to see the absence of in a single number.
// ---------------------------------------------------------------------------

struct Stokes4 { float i, q, u, v; };

__device__ __forceinline__ Stokes4 stokesUnpolarised() { return {1.0f, 0.0f, 0.0f, 0.0f}; }

__device__ __forceinline__ Stokes4 stokesScaled(Stokes4 s, float f) {
    return {s.i * f, s.q * f, s.u * f, s.v * f};
}

// The reflection or transmission of an interface, applied to a state already in
// the plane of incidence. The general form, which reduces to a scale by
// (Rs + Rp)/2 for unpolarised light -- exactly what the unpolarised path does.
__device__ __forceinline__ Stokes4 applyFresnelMueller(Stokes4 s, float rs, float rp,
                                                       float phaseDelta) {
    const float Rs = rs * rs;
    const float Rp = rp * rp;
    const float c  = rs * rp * cosf(phaseDelta);
    const float sn = rs * rp * sinf(phaseDelta);
    Stokes4 o;
    o.i = 0.5f * (Rs + Rp) * s.i + 0.5f * (Rs - Rp) * s.q;
    o.q = 0.5f * (Rs - Rp) * s.i + 0.5f * (Rs + Rp) * s.q;
    o.u =  c * s.u + sn * s.v;
    o.v = -sn * s.u + c  * s.v;
    return o;
}

// Rotates the reference frame by `angle` radians.
__device__ __forceinline__ Stokes4 rotateStokes(Stokes4 s, float angle) {
    const float c = cosf(2.0f * angle);
    const float sn = sinf(2.0f * angle);
    Stokes4 o;
    o.i = s.i;
    o.q =  c * s.q + sn * s.u;
    o.u = -sn * s.q + c * s.u;
    o.v = s.v;
    return o;
}

// The angle between two planes of incidence, about the ray. The s axis of each
// frame is perpendicular to that surface plane of incidence: d x n, normalised.
__device__ __forceinline__ float frameRotation(float3 d, float3 nOld, float3 nNew) {
    float3 sOld = cross3(d, nOld);
    float3 sNew = cross3(d, nNew);
    const float lo = dot3(sOld, sOld), ln = dot3(sNew, sNew);
    if (!(lo > 1e-24f) || !(ln > 1e-24f)) return 0.0f;
    sOld = mul3(sOld, rsqrtf(lo));
    sNew = mul3(sNew, rsqrtf(ln));
    const float c = fminf(1.0f, fmaxf(-1.0f, dot3(sOld, sNew)));
    const float s = dot3(cross3(sOld, sNew), norm3(d));
    return atan2f(s, c);
}

// Fresnel as amplitude reflectances and the phase between them. Past the
// critical angle both amplitudes have unit magnitude and what is left is the
// phase -- which is the whole reason a Fresnel rhomb turns linear light circular,
// and which an unpolarised model has no way to express.
__device__ __forceinline__ void fresnelAmplitudes(float n1, float n2, float cosI,
                                                  float& rs, float& rp, float& phase) {
    cosI = fminf(1.0f, fmaxf(0.0f, fabsf(cosI)));
    const float eta = n1 / n2;
    const float sinT2 = eta * eta * (1.0f - cosI * cosI);
    if (sinT2 >= 1.0f) {
        rs = 1.0f;
        rp = 1.0f;
        const float sinI = sqrtf(fmaxf(0.0f, 1.0f - cosI * cosI));
        const float root = sqrtf(fmaxf(0.0f, sinI * sinI - (n2 / n1) * (n2 / n1)));
        const float ds = -2.0f * atan2f(root, cosI);
        const float dp = -2.0f * atan2f((n1 / n2) * (n1 / n2) * root, cosI);
        phase = dp - ds;
        return;
    }
    const float cosT = sqrtf(1.0f - sinT2);
    const float as = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    const float ap = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
    rs = fabsf(as);
    rp = fabsf(ap);
    // Below the critical angle the coefficients are real, so the only phase is
    // whichever of them changed sign. That sign flip through Brewster is what
    // makes a stack of plates a polariser.
    const float ps = (as < 0.0f) ? 3.14159265358979324f : 0.0f;
    const float pp = (ap < 0.0f) ? 3.14159265358979324f : 0.0f;
    phase = pp - ps;
}

__device__ __forceinline__ float2 cmul(float2 a, float2 b) {
    return make_float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}
__device__ __forceinline__ float2 cdiv(float2 a, float2 b) {
    const float d = b.x * b.x + b.y * b.y;
    if (!(d > 0.0f)) return make_float2(0.0f, 0.0f);
    return make_float2((a.x * b.x + a.y * b.y) / d, (a.y * b.x - a.x * b.y) / d);
}
// Principal branch, so the transmitted wave decays into the metal rather than
// growing out of it.
__device__ __forceinline__ float2 csqrt(float2 z) {
    const float m = sqrtf(z.x * z.x + z.y * z.y);
    const float re = sqrtf(fmaxf(0.0f, 0.5f * (m + z.x)));
    float im = sqrtf(fmaxf(0.0f, 0.5f * (m - z.x)));
    if (z.y < 0.0f) im = -im;
    return make_float2(re, im);
}

// The same interface onto an absorbing medium, from its complex index. Metal
// reflection is the largest single source of polarisation change in most
// reflective systems: aluminium at sixty degrees turns linear light elliptical,
// so a periscope of two mirrors is not a null. The complex form is the
// reference own, which is what reproduces the dielectric limit -- zero
// retardance below Brewster and pi above it -- rather than special-casing it.
__device__ __forceinline__ void metalAmplitudes(float n1, float n2, float k2, float cosI,
                                                float& rs, float& rp, float& phase) {
    if (n1 <= 0.0f) n1 = 1.0f;
    cosI = fminf(1.0f, fmaxf(0.0f, fabsf(cosI)));
    const float2 nt  = make_float2(n2 / n1, -k2 / n1);
    const float2 nt2 = cmul(nt, nt);
    const float2 ct  = csqrt(make_float2(nt2.x - (1.0f - cosI * cosI), nt2.y));
    const float2 ci  = make_float2(cosI, 0.0f);

    const float2 as = cdiv(make_float2(ci.x - ct.x, -ct.y), make_float2(ci.x + ct.x, ct.y));
    const float2 n2c = cmul(nt2, ci);
    const float2 ap = cdiv(make_float2(n2c.x - ct.x, n2c.y - ct.y),
                           make_float2(n2c.x + ct.x, n2c.y + ct.y));

    rs = sqrtf(as.x * as.x + as.y * as.y);
    rp = sqrtf(ap.x * ap.x + ap.y * ap.y);
    phase = atan2f(ap.y, ap.x) - atan2f(as.y, as.x);
}

// ---------------------------------------------------------------------------
// the spectrum
//
// A spectral run draws a wavelength per ray and every wavelength-dependent
// quantity is read at it: the refractive index, a metal complex index, a
// measured coating table. The reference evaluates those exactly -- Sellmeier,
// Cauchy, a measured table -- and porting four dispersion models plus their
// tables to the device would be four more chances to disagree about what a
// glass is.
//
// So the host samples each of them once per run onto a fixed grid across the
// band, and the device interpolates. At 256 samples the spacing is a nanometre
// and a half and the interpolation error in n is under a part in a million,
// which is below what a float carries -- so it is not a second dispersion model,
// it is the reference's own curve read at a resolution the arithmetic cannot
// tell from exact.
// ---------------------------------------------------------------------------

// Which quantity a slice of the sampled table holds.
enum SpecQuantity { kSpecIndex = 0, kSpecMetalN, kSpecMetalK, kSpecCoat, kSpecCount };


__device__ __forceinline__ float specAt(const DevScene& sc, int surf, int quantity,
                                        float lambda) {
    if (!sc.specTable || sc.specSamples <= 1) return 0.0f;
    const float span = sc.specMaxNm - sc.specMinNm;
    const float f = span > 0.0f
                        ? (lambda - sc.specMinNm) / span * float(sc.specSamples - 1)
                        : 0.0f;
    const int i = min(sc.specSamples - 2, max(0, (int)f));
    const float a = fminf(1.0f, fmaxf(0.0f, f - float(i)));
    const int base = (surf * kSpecCount + quantity) * sc.specSamples;
    return sc.specTable[base + i] * (1.0f - a) + sc.specTable[base + i + 1] * a;
}

// The wavelength this ray carries, from the source own distribution. The same
// inverse CDF the reference builds, uploaded rather than rebuilt.
__device__ __forceinline__ float sampleWavelength(const DevScene& sc, long long ray,
                                                  float u) {
    if (sc.spdMono) return sc.monoLambda;
    if (sc.spdRgb) {
        const float bands[3] = {620.0f, 546.1f, 460.0f};
        return bands[(int)(ray % 3)];
    }
    if (!sc.invCdf || sc.spdBins <= 0) return sc.monoLambda;
    const float f = fminf(1.0f, fmaxf(0.0f, u)) * float(sc.spdBins);
    const int b = min(sc.spdBins - 1, (int)f);
    const float a = f - float(b);
    return sc.invCdf[b] * (1.0f - a) + sc.invCdf[b + 1] * a;
}

// The refractive index of the medium a branch is travelling through, at this
// wavelength.
__device__ __forceinline__ float mediumIndex(const DevScene& sc, int medium,
                                             float lambda) {
    if (medium < 0) return 1.0f;
    const float n = sc.spectral ? specAt(sc, medium, kSpecIndex, lambda)
                                : sc.surfs[medium].index;
    return n > 0.0f ? n : 1.0f;
}

__device__ __forceinline__ float gaussAsym(float x, float mu, float s1, float s2) {
    const float t = (x - mu) * (x < mu ? 1.0f / s1 : 1.0f / s2);
    return expf(-0.5f * t * t);
}

// V(lambda), which is the CIE Y colour-matching function by definition. The same
// analytic fit the reference carries.
__device__ __forceinline__ float photopic(float l) {
    if (l < 360.0f || l > 830.0f) return 0.0f;
    return fmaxf(0.0f, 0.821f * gaussAsym(l, 568.8f, 46.9f, 40.5f) +
                       0.286f * gaussAsym(l, 530.9f, 16.3f, 31.1f));
}

// How this ray paints into the three colour bands, as weights that sum to one,
// so the bands still add up to the irradiance grid exactly.
__device__ __forceinline__ void bandWeights(const DevScene& sc, float l, float w[3]) {
    w[0] = w[1] = w[2] = 0.0f;
    if (sc.spdRgb) {
        int b = 1;
        float best = 1e18f;
        const float bands[3] = {620.0f, 546.1f, 460.0f};
        for (int i = 0; i < 3; ++i) {
            const float d = fabsf(l - bands[i]);
            if (d < best) { best = d; b = i; }
        }
        w[b] = 1.0f;
        return;
    }
    const float X = fmaxf(0.0f, 1.056f * gaussAsym(l, 599.8f, 37.9f, 31.0f) +
                                0.362f * gaussAsym(l, 442.0f, 16.0f, 26.7f) -
                                0.065f * gaussAsym(l, 501.1f, 20.4f, 26.2f));
    const float Y = photopic(l);
    const float Z = fmaxf(0.0f, 1.217f * gaussAsym(l, 437.0f, 11.8f, 36.0f) +
                                0.681f * gaussAsym(l, 459.0f, 26.0f, 13.8f));
    // XYZ -> linear sRGB (IEC 61966-2-1). A monochromatic stimulus sits outside
    // the gamut, so one channel comes out negative; clamping and renormalising
    // is what keeps the three band grids summing to the irradiance grid.
    float r = fmaxf(0.0f,  3.2406f * X - 1.5372f * Y - 0.4986f * Z);
    float g = fmaxf(0.0f, -0.9689f * X + 1.8758f * Y + 0.0415f * Z);
    float b = fmaxf(0.0f,  0.0557f * X - 0.2040f * Y + 1.0570f * Z);
    const float sum = r + g + b;
    if (sum > 1e-12f) { w[0] = r / sum; w[1] = g / sum; w[2] = b / sum; }
    else              { w[1] = 1.0f; }
}

__device__ __forceinline__ int effectiveMedium(const DevScene& sc,
                                               const int* stack, int n) {
    int best = -1, bestPrio = 0;
    for (int i = 0; i < n; ++i) {
        const int idx = stack[i];
        const int prio = sc.surfs[idx].mediumPriority;
        if (best < 0 || prio >= bestPrio) { best = idx; bestPrio = prio; }
    }
    return best;
}

__device__ bool nearestHit(const DevScene& sc, float3 o, float3 d, float tMin, Hit& hit) {
    hit.tri  = -1;
    hit.inst = -1;
    float best = FLT_MAX;
    // The placements first: they tighten `best` for the walk over the rest,
    // and a scene that is entirely instanced has no such walk to do.
    const bool instHit = hitInstances(sc, o, d, tMin, best, hit);
    if (sc.nNodes <= 0) return instHit;
    const float3 inv = mk(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
    const RayShear rs = shearOf(d);

    int   stack[kStackDepth];
    float entryOf[kStackDepth];
    int   sp = 0;
    {
        float e;
        if (slab(sc.nodes[0], o, inv, tMin, best, e)) { stack[sp] = 0; entryOf[sp] = e; ++sp; }
    }
    while (sp > 0) {
        --sp;
        // The box was tested at push time; `best` may have tightened past it
        // since, and one comparison is cheaper than re-running the slab test.
        if (entryOf[sp] >= best) continue;
        const Node n = sc.nodes[stack[sp]];
        if (n.count > 0) {
            for (int k = 0; k < n.count; ++k) {
                const int ti = sc.order[n.left + k];
                float tt, tu, tv;
                if (!intersectTri(sc.tris[ti], o, rs, tMin, best, tt, tu, tv)) continue;
                best = tt;
                hit.tri = ti; hit.inst = -1; hit.t = tt; hit.u = tu; hit.v = tv;
            }
            continue;
        }
        const float dAxis = (n.axis == 0) ? d.x : (n.axis == 1) ? d.y : d.z;
        const int nearC = (dAxis < 0.0f) ? n.right : n.left;
        const int farC  = (dAxis < 0.0f) ? n.left  : n.right;
        float eN, eF;
        const bool hN = slab(sc.nodes[nearC], o, inv, tMin, best, eN);
        const bool hF = slab(sc.nodes[farC],  o, inv, tMin, best, eF);
        if (hF && sp < kStackDepth) { stack[sp] = farC;  entryOf[sp] = eF; ++sp; }
        if (hN && sp < kStackDepth) { stack[sp] = nearC; entryOf[sp] = eN; ++sp; }
    }
    return hit.tri >= 0;
}

// ---------------------------------------------------------------------------
// physics
// ---------------------------------------------------------------------------

// Fresnel, s and p kept apart.
//
// The unpolarised answer is their average and that is all an uncoated surface
// needs -- but a coating is applied to each of them separately, and the two
// operations do not commute, so the pair has to survive as far as the coating.
__device__ __forceinline__ void fresnelSP(float n1, float n2, float cosI,
                                          float& Rs, float& Rp) {
    const float eta = n1 / n2;
    const float s2  = eta * eta * fmaxf(0.0f, 1.0f - cosI * cosI);
    if (s2 >= 1.0f) { Rs = 1.0f; Rp = 1.0f; return; }   // past the critical angle
    const float cosT = sqrtf(fmaxf(0.0f, 1.0f - s2));
    const float rs = (n1 * cosI - n2 * cosT) / fmaxf(1e-20f, n1 * cosI + n2 * cosT);
    const float rp = (n2 * cosI - n1 * cosT) / fmaxf(1e-20f, n2 * cosI + n1 * cosT);
    Rs = fminf(1.0f, rs * rs);
    Rp = fminf(1.0f, rp * rp);
}

// Unpolarised Fresnel: the same average of s and p the reference takes.
__device__ __forceinline__ float fresnelR(float n1, float n2, float cosI) {
    float Rs, Rp;
    fresnelSP(n1, n2, cosI, Rs, Rp);
    return fminf(1.0f, 0.5f * (Rs + Rp));
}

// Reflectance of an absorbing medium: the same p, q form the reference uses,
// where p and q are the real and imaginary parts of the transmitted index times
// the cosine of the transmitted angle.
__device__ __forceinline__ float metalR(float n1, float n2, float k2, float cosI) {
    if (n1 <= 0.0f) n1 = 1.0f;
    cosI = fminf(1.0f, fmaxf(0.0f, fabsf(cosI)));
    if (k2 <= 0.0f) return fresnelR(n1, n2, cosI);

    const float n = n2 / n1;
    const float k = k2 / n1;
    const float s2 = 1.0f - cosI * cosI;
    const float a  = n * n - k * k - s2;
    const float r  = sqrtf(a * a + 4.0f * n * n * k * k);
    const float p  = sqrtf(fmaxf(0.0f, 0.5f * (r + a)));
    const float q  = sqrtf(fmaxf(0.0f, 0.5f * (r - a)));

    const float denS = (cosI + p) * (cosI + p) + q * q;
    if (denS <= 0.0f) return 1.0f;
    const float Rs = ((cosI - p) * (cosI - p) + q * q) / denS;
    if (cosI >= 1.0f - 1e-7f) return fminf(1.0f, Rs);

    const float sTan = s2 / cosI;
    const float denP = (p + sTan) * (p + sTan) + q * q;
    if (denP <= 0.0f) return 1.0f;
    const float Rp = Rs * (((p - sTan) * (p - sTan) + q * q) / denP);
    return fminf(1.0f, fmaxf(0.0f, 0.5f * (Rs + Rp)));
}

// An ideal coating over a bare Fresnel reflectance: the specified residual,
// growing with angle the way the bare interface does, and never worse than the
// bare surface. Coating::reflectanceSP, in the same order it evaluates.
//
// It is applied to `Rs` and `Rp` one at a time, and clamping each before
// averaging is not the same as clamping the average -- which is what this used
// to do. Near Brewster's angle the p reflectance is already far below what any
// coating leaves behind while the s reflectance is well above it, so the
// average of the two clamps to the residual where only the s half should. The
// coating then reflects roughly twice what the reference says it does, at every
// oblique refracting surface in the scene, and the light it turns back is
// missing from the receiver: about a third of a percent on the axicon, whose
// cone is met at thirty-four degrees on the way out.
__device__ __forceinline__ void coated(float& Rs, float& Rp, float n1, float n2,
                                       float residual, int highReflector) {
    if (residual < 0.0f) return;
    // Past the critical angle the interface reflects everything whatever is on
    // it: a coating cannot make light leave a medium it cannot leave. The
    // reference's margin here is 1e-12, which a float cannot resolve; 1e-6 is
    // the same test at this precision.
    if (Rs >= 1.0f - 1e-6f && Rp >= 1.0f - 1e-6f) return;
    const float rn = (n1 - n2) / (n1 + n2);
    const float bareNormal = rn * rn;
    // The roll-off is driven by the unpolarised bare reflectance, so both
    // polarisations grow with angle at the same rate the surface does.
    const float roll = (bareNormal <= 1e-12f)
                           ? 1.0f
                           : fmaxf(1.0f, 0.5f * (Rs + Rp) / bareNormal);
    if (highReflector) {
        // A high reflector loses a little at angle rather than gaining.
        const float r = fminf(1.0f, fmaxf(0.0f, 1.0f - (1.0f - residual) * roll));
        Rs = fmaxf(Rs, r);
        Rp = fmaxf(Rp, r);
        return;
    }
    Rs = fminf(1.0f, fmaxf(0.0f, fminf(Rs, residual * roll)));
    Rp = fminf(1.0f, fmaxf(0.0f, fminf(Rp, residual * roll)));
}

// Snell, in the same form the reference evaluates (optics::refract). `d` and
// `n` must be unit with `n` facing the incident ray, so cosI = -d.n >= 0.
// False past the critical angle, and `out` is then untouched.
__device__ __forceinline__ bool refractDir(float3 d, float3 n, float eta, float cosI,
                                           float3& out) {
    const float sinT2 = eta * eta * (1.0f - cosI * cosI);
    if (sinT2 >= 1.0f) return false;
    const float3 t = add3(mul3(d, eta), mul3(n, eta * cosI - sqrtf(1.0f - sinT2)));
    const float l2 = dot3(t, t);
    if (!(l2 > 0.0f)) return false;
    out = mul3(t, 1.0f / sqrtf(l2));
    return true;
}

__device__ __forceinline__ float3 reflectDir(float3 d, float3 n) {
    return sub3(d, mul3(n, 2.0f * dot3(d, n)));
}

// Steps a spawned branch off the surface it was born on.
//
// Along the facet normal, and to whichever side the outgoing ray is actually
// going -- not to the side the incoming ray implies. Where the interpolated
// normal and the facet disagree, a refracted ray can come out on the same
// geometric side it went in on; assuming otherwise buries the new origin inside
// the solid, and the branch spends its life re-hitting the face it just left.
__device__ __forceinline__ float3 offsetFrom(float3 p, float3 outDir, float3 facetN,
                                             float eps) {
    return add3(p, mul3(facetN, copysignf(eps, dot3(outDir, facetN))));
}

__device__ __forceinline__ void basisOf(float3 n, float3& t, float3& b) {
    const float3 a = (fabsf(n.z) < 0.9f) ? mk(0.0f, 0.0f, 1.0f) : mk(1.0f, 0.0f, 0.0f);
    t = norm3(cross3(a, n));
    b = cross3(n, t);
}

// ---------------------------------------------------------------------------
// scattering
//
// The same four models bsdf::Surface carries, sampled the same way. They are
// here rather than refused because the receiver already handled diffuse
// arrivals, and because three of the library's scenes -- the integrating
// sphere, the diffuser plate, the showcase luminaire -- are nothing else.
//
// Next-event estimation is here as well, and is the reference's term for term:
// nextEventEstimate below connects every diffuse bounce analytically to every
// receiver and declines on the same geometric test the reference declines on.
// Sampling and waiting is what this kernel did before the gate opened on the
// diffuse scenes, and on an integrating sphere it cost an order of magnitude in
// rays for the same error bar -- a variance difference the comparison would
// have sized rather than refused, but not one worth paying.
// ---------------------------------------------------------------------------

__device__ __forceinline__ float3 cosineHemisphere(float3 n, float u1, float u2) {
    float3 t, b;
    basisOf(n, t, b);
    const float r   = sqrtf(u1);
    const float phi = 6.2831853071795864f * u2;
    const float z   = sqrtf(fmaxf(0.0f, 1.0f - u1));
    return norm3(add3(add3(mul3(t, r * cosf(phi)), mul3(b, r * sinf(phi))),
                      mul3(n, z)));
}

// Smith height-correlated masking. Without it, energy a neighbouring microfacet
// should have shadowed is counted twice at grazing.
__device__ __forceinline__ float smithG1(float cosV, float alpha) {
    if (cosV <= 0.0f) return 0.0f;
    const float a2 = alpha * alpha;
    const float c2 = cosV * cosV;
    const float t  = (1.0f - c2) / fmaxf(1e-18f, c2);
    return 2.0f / (1.0f + sqrtf(1.0f + a2 * t));
}

__device__ __forceinline__ float3 sampleGgxHalf(float3 n, float alpha, float u1, float u2) {
    const float theta = atanf(alpha * sqrtf(u1) / sqrtf(fmaxf(1e-12f, 1.0f - u1)));
    const float phi   = 6.2831853071795864f * u2;
    float3 t, b;
    basisOf(n, t, b);
    const float st = sinf(theta), ct = cosf(theta);
    return norm3(add3(add3(mul3(t, st * cosf(phi)), mul3(b, st * sinf(phi))),
                      mul3(n, ct)));
}

// The microfacet this interaction happens at, the specular direction off it,
// and the visibility weight that makes the lobe energy conserving.
//
// Separate from the lobe sampler for the reason the reference keeps it
// separate: the facet is *where the interface is*, so Fresnel and Snell are
// evaluated there and a rough interface is rough for the transmitted branch too.
__device__ bool sampleMicrofacet(float3 wi, float3 n, float alpha,
                                 unsigned long long& rng,
                                 float3& micronormal, float3& reflected, float& weight) {
    weight = 1.0f;
    if (alpha <= 1e-6f) return false;
    const float3 h = sampleGgxHalf(n, alpha, rnd(rng), rnd(rng));
    // A facet turned past the incoming ray is on its back, which is not a facet
    // this ray can meet.
    if (dot3(h, wi) >= 0.0f) return false;
    float3 o = sub3(wi, mul3(h, 2.0f * dot3(wi, h)));
    const float l2 = dot3(o, o);
    if (!(l2 > 0.0f)) return false;
    o = mul3(o, 1.0f / sqrtf(l2));
    const float cosO = dot3(o, n);
    const float cosI = -dot3(wi, n);
    if (cosO <= 1e-9f || cosI <= 1e-9f) return false;
    const float cosH = dot3(h, n);
    if (cosH <= 1e-9f) return false;
    const float g     = smithG1(cosI, alpha) * smithG1(cosO, alpha);
    const float denom = fmaxf(1e-12f, cosI * cosH);
    weight      = fminf(4.0f, fmaxf(0.0f, g * fabsf(dot3(wi, h)) / denom));
    micronormal = h;
    reflected   = o;
    return true;
}

__device__ __forceinline__ float abgValue(const Surf& s, float dbeta) {
    const float b = fmaxf(1e-9f, s.abgB);
    const float g = fmaxf(0.05f, s.abgG);
    return s.abgA / (powf(b, g) + powf(fmaxf(0.0f, dbeta), g));
}

// A measured BSDF, interpolated in the log because it spans decades and a linear
// interpolation between two of them describes neither. Bisection rather than the
// reference's linear scan: five hundred samples walked one at a time would
// serialise a warp whose lanes all land in different places.
__device__ float tableValueAt(const float* beta, const float* val,
                              int first, int count, float dbeta) {
    if (count <= 0) return 0.0f;
    if (count == 1 || dbeta <= beta[first]) return val[first];
    if (dbeta >= beta[first + count - 1]) return val[first + count - 1];
    int lo = 0, hi = count - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) >> 1;
        if (beta[first + mid] <= dbeta) lo = mid; else hi = mid;
    }
    const float span = beta[first + lo + 1] - beta[first + lo];
    if (span <= 0.0f) return val[first + lo];
    const float f = (dbeta - beta[first + lo]) / span;
    const float a = fmaxf(1e-30f, val[first + lo]);
    const float b = fmaxf(1e-30f, val[first + lo + 1]);
    return expf(logf(a) * (1.0f - f) + logf(b) * f);
}

// An outgoing direction from the lobe, and the ratio of the BSDF to the density
// it was drawn from. bsdf::Surface::sample, term for term.
__device__ bool lobeSample(const Surf& s, float3 wi, float3 n, float3 spec,
                           const float* tBeta, const float* tVal,
                           unsigned long long& rng, float3& out, float& weight) {
    weight = 1.0f;
    if (s.bsdfModel == 3) {                        // Lambertian
        out = cosineHemisphere(n, rnd(rng), rnd(rng));
        return dot3(out, n) > 0.0f;
    }
    if (s.bsdfModel == 1) {                        // GGX
        float3 h;
        return sampleMicrofacet(wi, n, s.bsdfAlpha, rng, h, out, weight);
    }
    if (s.bsdfModel != 2 && s.bsdfModel != 4) return false;

    // ABg and the measured table share a sampler: draw the scatter angle about
    // the specular direction from a heavy-tailed density of the core's own
    // width, so the wings are covered without every sample being spent in them.
    const float b  = fmaxf(1e-9f, s.abgB);
    const float u1 = rnd(rng);
    const float gp = fmaxf(0.5f, (s.bsdfModel == 2 ? s.abgG : 2.0f) - 1.0f);
    const float db = b * (powf(fmaxf(1e-12f, u1), -1.0f / gp) - 1.0f);
    if (!(db >= 0.0f) || db > 2.0f) return false;

    const float phi = 6.2831853071795864f * rnd(rng);
    float3 t, bt;
    basisOf(spec, t, bt);
    // db is a direction-cosine offset, so the polar angle it stands for is its
    // arcsine.
    const float theta = asinf(fminf(1.0f, fmaxf(0.0f, db)));
    const float st    = sinf(theta);
    float3 o = add3(add3(mul3(t, st * cosf(phi)), mul3(bt, st * sinf(phi))),
                    mul3(spec, cosf(theta)));
    const float l2 = dot3(o, o);
    if (!(l2 > 0.0f)) return false;
    o = mul3(o, 1.0f / sqrtf(l2));
    if (dot3(o, n) <= 1e-9f) return false;         // below the surface

    const float pdf = (gp / b) * powf(1.0f + db / b, -gp - 1.0f) /
                      fmaxf(1e-12f, 6.2831853071795864f * fmaxf(1e-9f, db));
    const float f = (s.bsdfModel == 2)
                        ? abgValue(s, db)
                        : tableValueAt(tBeta, tVal, s.tableFirst, s.tableCount, db);
    weight = fminf(4.0f, fmaxf(0.0f, f / fmaxf(1e-30f, pdf)));
    out    = o;
    return true;
}

// Distance to the next scattering event inside a medium, exponentially
// distributed.
__device__ __forceinline__ float volSampleDistance(const Surf& s,
                                                   unsigned long long& rng) {
    if (s.volCoeff <= 0.0f) return 3.0e30f;
    return -logf(fmaxf(1e-12f, rnd(rng))) / s.volCoeff;
}

// A new direction from the phase function, by the exact inverse of its
// cumulative distribution -- two powf calls and no rejection loop, which is what
// makes volume scattering cheap enough to leave switched on.
__device__ float3 volScatter(const Surf& s, float3 d, unsigned long long& rng) {
    const float g = fminf(0.99f, fmaxf(-0.99f, s.volG));
    const float u = rnd(rng);
    float cosT;
    if (fabsf(g) < 1e-4f) {
        cosT = 1.0f - 2.0f * u;                    // isotropic
    } else if (s.volPhase == 1) {                  // Gegenbauer
        const float a    = fminf(10.0f, fmaxf(0.01f, s.volAlpha));
        const float a2   = 2.0f * a;
        const float span = powf(1.0f + g, a2) - powf(1.0f - g, a2);
        const float D    = powf(1.0f - g * g, a2) / span;
        const float base = u / D + powf(1.0f + g, -a2);
        cosT = (1.0f + g * g - powf(base, -1.0f / a)) / (2.0f * g);
    } else {                                       // Henyey-Greenstein
        const float q = (1.0f - g * g) / (1.0f - g + 2.0f * g * u);
        cosT = (1.0f + g * g - q * q) / (2.0f * g);
    }
    cosT = fminf(1.0f, fmaxf(-1.0f, cosT));
    const float sinT = sqrtf(fmaxf(0.0f, 1.0f - cosT * cosT));
    const float phi  = 6.2831853071795864f * rnd(rng);
    float3 t, b;
    basisOf(d, t, b);
    return norm3(add3(add3(mul3(t, sinT * cosf(phi)), mul3(b, sinT * sinf(phi))),
                      mul3(d, cosT)));
}

// ---------------------------------------------------------------------------
// the emission stream
//
// Independent uniforms scatter samples at random, so the error falls as
// 1/sqrt(N) with a constant set by how unevenly they happened to land. An
// Owen-scrambled Sobol sequence lands them evenly in every projection at once,
// and on the smooth integrand a source distribution actually is, the same error
// bar arrives from several times fewer rays. This is sampling::sobol, term for
// term, over the direction numbers the host built and uploaded -- one table
// rather than two, so the two backends cannot drift apart about what the
// sequence is.
//
// It costs nothing in determinism: the sequence is indexed by ray number, which
// is what the counter-based generator beside it was already doing.
// ---------------------------------------------------------------------------

__device__ __forceinline__ unsigned int reverseBits32(unsigned int x) {
    return __brev(x);
}

__device__ __forceinline__ unsigned int laineKarras(unsigned int x, unsigned int seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

__device__ __forceinline__ unsigned int owenScramble(unsigned int x, unsigned int seed) {
    return reverseBits32(laineKarras(reverseBits32(x), seed));
}

__device__ __forceinline__ unsigned int hashCombine(unsigned int x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The `dim`-th coordinate of Sobol point `index`, Owen-scrambled with `seed`.
__device__ __forceinline__ float sobol(const unsigned int* dirs, unsigned int index,
                                       int dim, unsigned int seed) {
    if (!dirs || dim < 0 || dim >= kSobolDims) {
        // Past the tabulated dimensions, a hashed value: still deterministic,
        // just no longer stratified.
        return float(hashCombine(index ^ ((unsigned int)dim * 0x9e3779b9u) ^ seed) >> 8) *
               (1.0f / 16777216.0f);
    }
    // Shuffling the index decorrelates the sequences of different rays;
    // scrambling the output decorrelates the dimensions.
    const unsigned int shuffled = owenScramble(index, hashCombine(seed ^ 0x51633e2du));
    const unsigned int* v = dirs + dim * 32;
    unsigned int x = 0;
    unsigned int i = shuffled;
    for (int bit = 0; i != 0u; ++bit, i >>= 1)
        if (i & 1u) x ^= v[bit];
    x = owenScramble(x, hashCombine(seed ^ ((unsigned int)dim * 0x68bc21ebu)));
    return float(x >> 8) * (1.0f / 16777216.0f);
}

// The four numbers one emitted ray needs: two for where on the emitter it
// starts and two for which way it leaves. Drawn from the low-discrepancy
// sequence where it is switched on and from the counter-based generator
// otherwise, so a run can be compared against itself with and without it.
// The four emission dimensions, and the spectral one. The wavelength shares the
// low-discrepancy stream rather than taking a fresh random draw, and on a
// photometric run that is not a refinement: every ray is weighted by V at its
// own wavelength and the run divides by the mean V of the distribution, so a
// spectral draw whose sampled mean is a fraction of a percent off the true one
// leaves the whole budget a fraction of a percent from closing. Sobol on the
// same dimension the reference uses is what makes the two agree.
struct EmitDraw { float a0, a1, d0, d1, w; };

__device__ __forceinline__ EmitDraw emitDraw(const unsigned int* dirs,
                                             unsigned int qmcIndex, unsigned int scramble,
                                             bool lowDiscrepancy,
                                             unsigned long long& rng) {
    EmitDraw e;
    if (lowDiscrepancy) {
        e.a0 = sobol(dirs, qmcIndex, 0, scramble);
        e.a1 = sobol(dirs, qmcIndex, 1, scramble);
        e.d0 = sobol(dirs, qmcIndex, 2, scramble);
        e.d1 = sobol(dirs, qmcIndex, 3, scramble);
        e.w  = sobol(dirs, qmcIndex, 4, scramble);
    } else {
        e.a0 = rnd(rng); e.a1 = rnd(rng);
        e.d0 = rnd(rng); e.d1 = rnd(rng);
        e.w  = rnd(rng);
    }
    return e;
}

// Is anything other than `ignoreSurf` in the way between here and there?
//
// An any-hit query, not a nearest-hit one. The question a connection asks is
// whether anything is in the way, and one occluder anywhere along the segment
// answers it -- walking the hierarchy to completion to find the *nearest*
// occluder is work spent learning something the first one already said. On an
// integrating sphere or behind a diffuser, which are the scenes next-event
// estimation exists for, a connection is fired at nearly every bounce, so this
// is a large share of the whole trace.
__device__ bool anyHit(const DevScene& sc, float3 o, float3 d, float tMin, float tMax,
                       int ignoreSurf) {
    const float3 inv = mk(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
    const RayShear rs = shearOf(d);

    if (sc.nNodes > 0) {
        int stack[kStackDepth];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node n = sc.nodes[stack[--sp]];
            float e;
            if (!slab(n, o, inv, tMin, tMax, e)) continue;
            if (n.count > 0) {
                for (int k = 0; k < n.count; ++k) {
                    const int ti = sc.order[n.left + k];
                    if (sc.tris[ti].surf == ignoreSurf) continue;
                    float tt, tu, tv;
                    if (intersectTri(sc.tris[ti], o, rs, tMin, tMax, tt, tu, tv))
                        return true;
                }
                continue;
            }
            // No near/far ordering: without a nearest hit to tighten, which
            // child is visited first changes nothing but the order of the
            // answer.
            if (sp + 2 <= kStackDepth) { stack[sp++] = n.right; stack[sp++] = n.left; }
        }
    }
    if (sc.nTlas <= 0) return false;

    int stack[kStackDepth];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node n = sc.tlas[stack[--sp]];
        float e;
        if (!slab(n, o, inv, tMin, tMax, e)) continue;
        if (n.count <= 0) {
            if (sp + 2 <= kStackDepth) { stack[sp++] = n.right; stack[sp++] = n.left; }
            continue;
        }
        for (int k = n.left; k < n.left + n.count; ++k) {
            const Inst in = sc.insts[sc.tlasOrder[k]];
            if (in.surf == ignoreSurf) continue;
            Node ib;
            for (int a = 0; a < 3; ++a) { ib.bmin[a] = in.bmin[a]; ib.bmax[a] = in.bmax[a]; }
            float ie;
            if (!slab(ib, o, inv, tMin, tMax, ie)) continue;

            const float3 q  = sub3(o, ld3(in.origin));
            const float3 lo = mk(dot3(ld3(in.i0), q), dot3(ld3(in.i1), q),
                                 dot3(ld3(in.i2), q));
            const float3 ld = mk(dot3(ld3(in.i0), d), dot3(ld3(in.i1), d),
                                 dot3(ld3(in.i2), d));
            const Blas bl = sc.blas[in.blas];
            if (bl.nodeCount <= 0) continue;
            const float3 linv = mk(1.0f / ld.x, 1.0f / ld.y, 1.0f / ld.z);
            const RayShear lrs = shearOf(ld);

            int bstack[kStackDepth];
            int bsp = 0;
            bstack[bsp++] = 0;
            while (bsp > 0) {
                const Node bn = sc.blasNodes[bl.nodeFirst + bstack[--bsp]];
                float be;
                if (!slab(bn, lo, linv, tMin, tMax, be)) continue;
                if (bn.count <= 0) {
                    if (bsp + 2 <= kStackDepth) {
                        bstack[bsp++] = bn.right;
                        bstack[bsp++] = bn.left;
                    }
                    continue;
                }
                for (int j = 0; j < bn.count; ++j) {
                    const int ti = sc.blasOrder[bl.orderFirst + bn.left + j];
                    float tt, tu, tv;
                    if (intersectTri(sc.blasTris[bl.triFirst + ti], lo, lrs,
                                     tMin, tMax, tt, tu, tv))
                        return true;
                }
            }
        }
    }
    return false;
}

// Connects a diffuse bounce at `p` to every receiver analytically: pick a point
// on each, test that nothing is in the way, and add the flux the cosine lobe
// puts through that solid angle.
//
// A random walk that has to *find* a receiver spends most of its rays missing;
// on an integrating sphere or behind a diffuser that is where nearly all of the
// variance lives, and connecting to it directly removes the search entirely.
// This is nextEventEstimate from the reference, term for term, including the
// part that is easy to leave out.
//
// Returns the energy delivered, or -1 when the estimate was declined -- which
// happens where a receiver sits close enough to subtend more than the lobe can
// deliver into, and one area sample would carry more energy than the branch
// has. The caller must only suppress the sampled path's own direct arrival when
// this returned something other than -1.
//
// The decision to decline is taken from the geometry of the bounce alone,
// before any point on a receiver is drawn. Deciding it from the drawn point
// instead would make the choice depend on the sample: the connections that
// survived would be the weak ones, the strong ones would fall back to path
// sampling, and the two halves would no longer add up to the integral. That is
// a bias, and on a transmissive diffuser it is a large one.
__device__ float nextEventEstimate(const DevScene& sc, float3 p, float3 n, float e,
                                   int medium, int fromSurf, unsigned long long& rng,
                                   double* grid, double* gridSq, double* totals,
                                   double* angles, double* anglesSq,
                                   int nTheta, int nPhi,
                                   double* bandGrid, const float* bandW,
                                   unsigned long long* hits, int wantSq) {
    if (!sc.nextEvent || sc.nDets <= 0 || e <= 0.0f) return -1.0f;
    const int nDet = min(sc.nDets, kMaxNeeDets);

    // Is a one-sample connection well conditioned here? The largest weight any
    // point on a receiver could produce is bounded by its area over pi times the
    // square of its nearest approach, with both cosines at one. If that bound
    // exceeds the branch's own energy for any receiver, decline the whole event
    // and let path sampling carry it.
    float bound = 0.0f;
    for (int di = 0; di < nDet; ++di) {
        const Det d = sc.dets[di];
        if (d.w <= 0.0f || d.h <= 0.0f) continue;
        const float3 rel = sub3(p, ld3(d.center));
        const float a = fminf(0.5f * d.w, fmaxf(-0.5f * d.w, dot3(rel, ld3(d.u))));
        const float b = fminf(0.5f * d.h, fmaxf(-0.5f * d.h, dot3(rel, ld3(d.v))));
        const float3 nearest = add3(ld3(d.center),
                                    add3(mul3(ld3(d.u), a), mul3(ld3(d.v), b)));
        const float3 gap = sub3(p, nearest);
        const float rMin2 = dot3(gap, gap);
        if (rMin2 < 1e-12f) return -1.0f;
        bound += d.w * d.h / (3.14159265358979324f * rMin2);
        if (bound > 1.0f) return -1.0f;
    }
    if (bound <= 0.0f) return -1.0f;

    const float eps = 4e-6f * fmaxf(1.0f, fmaxf(fabsf(p.x), fmaxf(fabsf(p.y), fabsf(p.z))));
    float delivered = 0.0f;
    for (int di = 0; di < nDet; ++di) {
        const Det d = sc.dets[di];
        if (d.w <= 0.0f || d.h <= 0.0f) continue;

        // Uniform on the receiver rectangle.
        const float su = (rnd(rng) - 0.5f) * d.w;
        const float sv = (rnd(rng) - 0.5f) * d.h;
        const float3 q = add3(ld3(d.center),
                              add3(mul3(ld3(d.u), su), mul3(ld3(d.v), sv)));

        float3 w = sub3(q, p);
        const float r2 = dot3(w, w);
        if (!(r2 > 1e-18f)) continue;
        const float r = sqrtf(r2);
        w = mul3(w, 1.0f / r);

        const float cosSurf = dot3(w, n);
        if (cosSurf <= 1e-9f) continue;              // behind the scattering surface
        const float cosDet = fabsf(dot3(w, ld3(d.n)));
        if (cosDet <= 1e-9f) continue;               // edge on: no projected area
        if (d.cosAcceptance > -1.0f && cosDet < d.cosAcceptance) continue;

        // The cosine lobe is cos/pi per steradian; the sampled point stands for
        // a solid angle of cosDet * area / r^2. The bound above already
        // guarantees this stays inside the branch's energy.
        const float factor = (cosSurf / 3.14159265358979324f) *
                             (cosDet * d.w * d.h / r2);
        if (!(factor > 0.0f)) continue;

        // Just short of the receiver, so its own far face cannot occlude the
        // connection to its near one.
        if (anyHit(sc, add3(p, mul3(n, eps)), w, eps, r * (1.0f - 1e-6f), d.surf))
            continue;

        float contribution = e * factor;
        // Bulk loss along the connection, in the medium the bounce is in.
        if (medium >= 0) {
            const float alpha = sc.surfs[medium].absorption;
            if (alpha > 0.0f) {
                const float survived = contribution * expf(-alpha * r);
                atomicAdd(totals + 1, (double)(contribution - survived));
                atomicAdd(totals + 5, (double)(contribution - survived));
                contribution = survived;
            }
        }
        if (contribution <= 0.0f) continue;

        const float3 rel = sub3(q, ld3(d.center));
        const float lu = dot3(rel, ld3(d.u)) + 0.5f * d.w;
        const float lv = dot3(rel, ld3(d.v)) + 0.5f * d.h;
        if (lu < 0.0f || lv < 0.0f) continue;
        const int bx = (int)(lu / d.w * (float)d.nx);
        const int by = (int)(lv / d.h * (float)d.ny);
        if (bx < 0 || bx >= d.nx || by < 0 || by >= d.ny) continue;
        const int cell = d.cellFirst + by * d.nx + bx;
        atomicAdd(grid + cell, (double)contribution);
        if (wantSq) atomicAdd(gridSq + cell, (double)contribution * (double)contribution);
        if (bandGrid && di == 0)
            for (int b = 0; b < 3; ++b)
                atomicAdd(bandGrid + (size_t)b * sc.bandCells + cell,
                          (double)contribution * (double)bandW[b]);
        atomicAdd(totals + 0, (double)contribution);           // detected
        // The far field is what the light travelled along, and an analytic
        // connection travelled along `w`. Leaving it out would take every
        // diffuse arrival out of the intensity distribution while leaving it
        // in the receiver map -- the two would then describe different runs.
        binDirection(angles, anglesSq, nTheta, nPhi, w, (double)contribution);
        atomicAdd(hits, 1ULL);
        delivered += contribution;
    }
    return delivered;
}

// Where on the emitter this ray starts, and about which normal it radiates.
// The same area-uniform draws the reference makes: sqrt for a disc so the
// samples are uniform per unit area rather than per unit radius, and a point on
// the sphere's surface with the outward normal there.
__device__ void emitOrigin(const Src& s, float u3, float u4,
                           float3& origin, float3& axisOut) {
    const float3 axis = norm3(ld3(s.axis));
    origin  = ld3(s.origin);
    axisOut = axis;
    if (s.shape == 0) return;

    float3 t, b;
    basisOf(axis, t, b);

    if (s.shape == 3) {                                   // sphere
        if (s.sizeA <= 0.0f) return;
        const float cz  = 1.0f - 2.0f * u3;
        const float sz  = sqrtf(fmaxf(0.0f, 1.0f - cz * cz));
        const float phi = 6.2831853071795864f * u4;
        const float3 nrm = mk(sz * cosf(phi), sz * sinf(phi), cz);
        origin  = add3(origin, mul3(nrm, s.sizeA));
        axisOut = nrm;                                    // radiates outward
        return;
    }
    if (s.shape == 1) {                                   // disc
        if (s.sizeA <= 0.0f) return;
        const float r   = s.sizeA * sqrtf(u3);            // area-uniform
        const float phi = 6.2831853071795864f * u4;
        origin = add3(origin, add3(mul3(t, r * cosf(phi)), mul3(b, r * sinf(phi))));
        return;
    }
    // rect
    origin = add3(origin, add3(mul3(t, s.sizeA * (u3 - 0.5f)),
                               mul3(b, s.sizeB * (u4 - 0.5f))));
}

__device__ float3 emitDirection(const Src& s, float3 axis, float u1, float u2) {
    if (s.type == 2) return axis;                          // collimated
    float3 t, b;
    basisOf(axis, t, b);
    const float phi = 6.2831853071795864f * u1;
    float cosT;
    if (s.type == 1) {                                     // cosine weighted
        const float m = 1.0f - s.cosHalfAngle * s.cosHalfAngle;
        cosT = sqrtf(fmaxf(0.0f, 1.0f - u2 * m));
    } else {                                               // uniform in solid angle
        cosT = 1.0f - u2 * (1.0f - s.cosHalfAngle);
    }
    const float sinT = sqrtf(fmaxf(0.0f, 1.0f - cosT * cosT));
    return norm3(add3(add3(mul3(t, sinT * cosf(phi)), mul3(b, sinT * sinf(phi))),
                      mul3(axis, cosT)));
}

// ---------------------------------------------------------------------------
// emission aiming
//
// A source that radiates into a hemisphere at an optic filling a twentieth of
// the sky spends most of its rays on directions that provably hit nothing. So
// the direction is drawn inside the cone the scene actually occupies and
// weighted by the share of the source's own law that lands there; the rest of
// the law points at nothing, and its energy is booked straight to escaped.
//
// That is exact rather than merely unbiased -- a direction outside the cone
// cannot reach the geometry at all -- and it is the largest single win on a bare
// emitter in front of a small optic.
// ---------------------------------------------------------------------------

struct AimCone {
    bool   active;
    float3 axis;
    float  cosHalf;
    float  omega;              // solid angle of the cone, sr
};

// Solid angle of the source's own emission law, so aiming can be declined when
// it would not narrow anything.
__device__ __forceinline__ float sourceSolidAngle(const Src& s) {
    if (s.type == 2) return 0.0f;                       // collimated
    const float c = fminf(1.0f, fmaxf(-1.0f, s.cosHalfAngle));
    if (s.type == 1) {                                  // Lambertian: projected
        const float sn = sqrtf(fmaxf(0.0f, 1.0f - c * c));
        return 3.14159265358979324f * sn * sn;
    }
    return 6.2831853071795864f * (1.0f - c);
}

// The source's own angular probability density along `d`, per steradian.
__device__ __forceinline__ float sourceAngularPdf(const Src& s, float3 axis, float3 d) {
    const float c = fminf(1.0f, fmaxf(-1.0f, dot3(axis, d)));
    const float ch = fminf(1.0f, fmaxf(-1.0f, s.cosHalfAngle));
    if (s.type == 1) {
        if (c <= ch - 1e-12f || c <= 0.0f) return 0.0f;
        const float sn = sqrtf(fmaxf(0.0f, 1.0f - ch * ch));
        return sn > 0.0f ? c / (3.14159265358979324f * sn * sn) : 0.0f;
    }
    if (c < ch - 1e-12f) return 0.0f;
    const float omega = 6.2831853071795864f * (1.0f - ch);
    return omega > 0.0f ? 1.0f / omega : 0.0f;
}

__device__ __forceinline__ AimCone aimingConeFor(const Src& s, float3 origin,
                                                 float3 centre, float radius) {
    AimCone c;
    c.active = false;
    c.axis = mk(0.0f, 0.0f, 1.0f);
    c.cosHalf = -1.0f;
    c.omega = 0.0f;
    if (radius <= 0.0f || s.type == 2) return c;
    float3 to = sub3(centre, origin);
    const float dist = sqrtf(dot3(to, to));
    // Inside the scene's own bounds there is no cone to aim into.
    if (!(dist > radius * 1.000001f)) return c;
    to = mul3(to, 1.0f / dist);

    const float sinHalf = radius / dist;
    const float cosHalf = sqrtf(fmaxf(0.0f, 1.0f - sinHalf * sinHalf));
    const float omega   = 6.2831853071795864f * (1.0f - cosHalf);
    // Only worth it when the cone is a real narrowing; below that the weighting
    // costs more variance than the aiming saves. The factor of two also keeps
    // the per-ray weight at or below one, so the escaped share it implies is
    // never negative.
    if (omega > 0.5f * sourceSolidAngle(s)) return c;

    c.active  = true;
    c.axis    = to;
    c.cosHalf = cosHalf;
    c.omega   = omega;
    return c;
}

// Uniform inside a cone of half-angle acos(cosHalf) about `axis`.
__device__ __forceinline__ float3 sampleCone(float3 axis, float cosHalf,
                                             float u1, float u2) {
    float3 t, b;
    basisOf(axis, t, b);
    const float cosT = 1.0f - u1 * (1.0f - cosHalf);
    const float sinT = sqrtf(fmaxf(0.0f, 1.0f - cosT * cosT));
    const float phi  = 6.2831853071795864f * u2;
    return norm3(add3(add3(mul3(t, sinT * cosf(phi)), mul3(b, sinT * sinf(phi))),
                      mul3(axis, cosT)));
}

// ---------------------------------------------------------------------------
// the kernel
// ---------------------------------------------------------------------------

__global__ void traceKernel(DevScene sc, double* grid, double* gridSq, double* angles, double* anglesSq,
                            int nTheta, int nPhi,
                            double* bandGrid, int bands,
                            double* totals, double* moments, double* residual,
                            double* anomalies, double* repDet, double* repEmit,
                            double* detStokes,
                            unsigned long long* hits,
                            unsigned long long seed, long long rays, int wantSq,
                            int nReplicas, int lowDiscrepancy, int roulette) {
    const long long stride = (long long)blockDim.x * gridDim.x;
    for (long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
         i < rays; i += stride) {

        // Seeded from the ray index, so a run reproduces itself whatever the
        // launch geometry -- the same rule the reference follows.
        unsigned long long rng = mix64(seed ^ ((unsigned long long)i * 0x9e3779b97f4a7c15ULL));

        // Which independent Owen scramble this ray belongs to, and where it
        // sits inside it. A Sobol sequence is not a set of independent
        // samples, so the ray-to-ray spread would report the error a plain
        // Monte Carlo run of the same size would have had -- the whole gain
        // thrown away in the reporting. The spread across replicas is what
        // the estimator actually achieved, and it is the same arrangement
        // the reference makes.
        const long long perRep = (rays + (long long)nReplicas - 1) /
                                 (long long)nReplicas;
        const int rep = (int)min((long long)nReplicas - 1,
                                 perRep > 0 ? i / perRep : 0);
        const unsigned int qmcIndex = (unsigned int)(i - (long long)rep * perRep);
        const unsigned int scramble =
            (unsigned int)(mix64(seed ^ (0x9e3779b97f4a7c15ULL * (unsigned long long)(rep + 1))) >> 32);

        const EmitDraw ed = emitDraw(sc.sobolDir, qmcIndex, scramble,
                                     lowDiscrepancy != 0, rng);
        // One wavelength per ray, from the source own distribution. A run in
        // lumens gives every ray the photometric weight of its own
        // wavelength at emission, which is what makes every downstream
        // quantity photometric without a second grid anywhere.
        const float lambda = sampleWavelength(sc, i, ed.w);
        float bandW[3] = {0.0f, 0.0f, 0.0f};
        if (bands) bandWeights(sc, lambda, bandW);
        float3 o, emitAxis;
        emitOrigin(sc.src, ed.a0, ed.a1, o, emitAxis);
        float3 d = emitDirection(sc.src, emitAxis, ed.d0, ed.d1);
        float  w = 1.0f;
        if (sc.unitLumen && sc.meanV > 1e-12f) w = photopic(lambda) / sc.meanV;
        const float emitted = w;
        if (repEmit) atomicAdd(repEmit + rep, (double)emitted);

        // The polarisation state this branch carries, and the frame it is
        // expressed in. A Stokes vector means nothing without a frame: the
        // plane of incidence turns from one surface to the next, so the state
        // is rotated into the new plane before the interface acts on it. The
        // frame starts empty, which is how the first interface knows there is
        // nothing to rotate from.
        Stokes4 stokes  = {sc.emittedState.x, sc.emittedState.y,
                           sc.emittedState.z, sc.emittedState.w};
        float3 polFrame = mk(0.0f, 0.0f, 0.0f);

        // Aim the direction into the cone the scene occupies, and book what the
        // rest of the source law was carrying. A direction outside that cone
        // provably hits nothing, so this is exact and not merely unbiased.
        if (sc.aim) {
            const AimCone cone = aimingConeFor(sc.src, o, sc.boundsCentre,
                                               sc.boundsRadius);
            if (cone.active) {
                d = sampleCone(cone.axis, cone.cosHalf, ed.d0, ed.d1);
                const float pdf = sourceAngularPdf(sc.src, emitAxis, d);
                const float aimWeight = fminf(1.0f, pdf * cone.omega);
                // A free companion draw from the source's own law, on its own
                // two dimensions so it is independent of the aimed one. It is
                // never traced -- a direction outside the cone provably hits
                // nothing -- but it is binned, so the far field still shows
                // where the light that misses the optic went.
                const float3 lawDir =
                    emitDirection(sc.src, emitAxis, rnd(rng), rnd(rng));
                double booked = 0.0;
                if (dot3(lawDir, cone.axis) < cone.cosHalf) {
                    atomicAdd(totals + 2, (double)emitted);   // escaped
                    binDirection(angles, anglesSq, nTheta, nPhi, lawDir,
                                 (double)emitted);
                    booked = (double)emitted;
                }
                atomicAdd(residual + 3,
                          (double)emitted * (1.0 - (double)aimWeight) - booked);
                w = emitted * aimWeight;
                if (!(w > 0.0f)) continue;                   // nothing left to trace
            }
        }

        // Seeded from the source containment rather than empty: an emitter
        // inside a solid starts its rays already in that solid.
        int media[kMediumMax];
        int mediaN = min(sc.src.mediaN, kMediumMax);
        for (int k = 0; k < mediaN; ++k) media[k] = sc.src.media[k];
        int medium = effectiveMedium(sc, media, mediaN);

        double detected = 0.0;
        bool   alive = true;
        // Set when the bounce this leg came from already connected to the
        // receivers analytically, so its own arrival must not be counted.
        bool   skipDetector = false;

        for (int depth = 0; depth < kMaxDepth && alive; ++depth) {
            // Russian roulette rather than a hard cutoff. A survivor is
            // promoted to exactly kRoulette, so its weight can never
            // explode, and the residual is booked so the buckets still
            // close to the last bit. Without it a dim branch is simply
            // dropped and the run is quietly dark by whatever it dropped.
            if (w < kRoulette) {
                if (!roulette) {
                    atomicAdd(totals + 3, (double)w);   // cut off
                    alive = false;
                    break;
                }
                const float pSurvive = w / kRoulette;
                if (rnd(rng) >= pSurvive) {
                    atomicAdd(residual + 4, (double)w);
                    alive = false;
                    break;
                }
                atomicAdd(residual + 4, (double)(-(kRoulette - w)));
                w = kRoulette;
            }
            // Relative to the coordinate magnitude, like the reference, but it
            // cannot be the reference's 1e-9: that is below what a float can
            // resolve, so the offset would vanish and every ray would
            // self-intersect.
            //
            // 4e-6 is about thirty times a float's own relative resolution, and
            // the margin is what it is because of what happens when a branch
            // lands on the *outside* of the facet it just left. The next hit is
            // then the same facet from the far side, the geometry says the ray
            // is leaving rather than entering, and a diffuse bounce is sampled
            // about the outward normal -- so the ray walks out of a closed
            // cavity. An integrating sphere is where that shows: the reference
            // escapes exactly nothing from it, and at 2e-7 the preview leaked
            // six rays in a thousand, at 1e-6 two in ten thousand, and here two
            // in a hundred thousand. Larger buys nothing and only risks
            // stepping over a thin feature.
            const float eps = 4e-6f * fmaxf(1.0f,
                                  fmaxf(fabsf(o.x), fmaxf(fabsf(o.y), fabsf(o.z))));
            // How far along the new ray the search starts, as a multiple of
            // the offset it was spawned with. They are not the same number,
            // and the ratio is measured rather than assumed.
            //
            // A branch offset along its facet's normal is inside that
            // facet's half-space -- but near a shared edge it can be outside
            // the *neighbouring* facet's, because offsetting by e from a
            // point on the edge lands e*sin(dihedral) beyond the neighbour
            // whatever e is. The ray re-enters through that neighbour
            // immediately, arrives on its outward side, and a diffuse bounce
            // is then sampled about the outward normal -- so the ray walks
            // out of a closed cavity. Starting the search past that wedge is
            // what removes it, and four is where the integrating sphere stops
            // leaking: at 1x it lost sixteen rays in four million, at 2x
            // three, at 4x none, and at 10x it starts stepping over real hits
            // at normal incidence instead.
            constexpr float kSkip = 4.0f;
            Hit h;
            if (!nearestHit(sc, o, d, kSkip * eps, h)) {
                atomicAdd(totals + 2, (double)w);              // escaped
                // The far field is what leaves the system, so an escaping ray
                // is binned by the direction it left along.
                binDirection(angles, anglesSq, nTheta, nPhi, d, (double)w);
                alive = false;
                break;
            }
            // Which triangle, in whose frame, and belonging to which surface.
            // An instanced hit does not live in the scene's own triangle list
            // at all: its triangle is the part's, in the part's coordinates,
            // and the surface is the placement's rather than the triangle's.
            const bool  inst = (h.inst >= 0);
            const Inst  pl   = inst ? sc.insts[h.inst] : Inst{};
            const Tri   t    = inst ? sc.blasTris[sc.blas[pl.blas].triFirst + h.tri]
                                    : sc.tris[h.tri];
            const int   surfIx = inst ? pl.surf : t.surf;
            Surf sf = sc.surfs[surfIx];
            // Read at this ray wavelength on a spectral run, off the curve
            // the host sampled from the reference own dispersion model.
            if (sc.spectral) {
                if (sf.index > 0.0f) sf.index = specAt(sc, surfIx, kSpecIndex, lambda);
                if (sf.metalK > 0.0f) {
                    sf.metalN = specAt(sc, surfIx, kSpecMetalN, lambda);
                    sf.metalK = specAt(sc, surfIx, kSpecMetalK, lambda);
                }
                if (sf.coatResidual >= 0.0f)
                    sf.coatResidual = specAt(sc, surfIx, kSpecCoat, lambda);
            }
            // Both normals are produced in the part's own frame and rotated
            // together, so every comparison between them is made where they
            // are comparable.
            auto toWorldDir = [&](float3 v) {
                return inst ? mk(dot3(ld3(pl.r0), v), dot3(ld3(pl.r1), v),
                                 dot3(ld3(pl.r2), v))
                            : v;
            };
            const float3 fn = toWorldDir(ld3(t.n));
            // The hit point from the barycentrics, not from o + d*t.
            //
            // They agree in double and they do not agree in float. `t` comes out
            // of Moller-Trumbore as a ratio of two triple products built from
            // vectors the length of the whole ray, so its relative error is a
            // few ulp of the *distance travelled*: a ray crossing 250 mm to
            // reach a surface lands with a couple of ten-thousandths of a
            // millimetre of error along its own direction. That is the same size
            // as the offset the next leg is spawned with, so the branch starts
            // on the wrong side of the surface it just left, immediately
            // re-hits it, and refracts a second time at a face it has already
            // crossed. The ray leaves in a direction no interface produced and
            // is booked as escaped -- about a percent of the flux through a
            // lens, growing with obliquity, and invisible at normal incidence
            // because the reference's double-precision `t` has six orders of
            // margin over the same epsilon.
            //
            // v0 + e1*u + e2*v is anchored on the triangle instead of on the
            // ray, so its error is a few ulp of the *vertex* coordinates and
            // does not grow with how far the ray came. On a planar face whose
            // vertices share a coordinate -- the flat back of a lens -- that
            // coordinate comes out exact.
            const float3 pLocal = add3(ld3(t.v0), add3(mul3(ld3(t.e1), h.u),
                                                       mul3(ld3(t.e2), h.v)));
            const float3 p = inst ? add3(ld3(pl.origin), toWorldDir(pLocal))
                                  : pLocal;

            // Scattering inside the medium, before the boundary is reached.
            // Every white diffusing plastic in every luminaire is a volume
            // scatterer, and a surface model has no way to say so.
            if (medium >= 0) {
                const Surf ms = sc.surfs[medium];
                if (ms.volCoeff > 0.0f) {
                    const float free = volSampleDistance(ms, rng);
                    if (free < h.t) {
                        // Attenuate over the leg actually travelled, then turn.
                        if (ms.absorption > 0.0f) {
                            const float survived = w * expf(-ms.absorption * free);
                            atomicAdd(totals + 1, (double)(w - survived));
                            atomicAdd(totals + 5, (double)(w - survived));
                            w = survived;
                        }
                        o = add3(o, mul3(d, free));
                        d = volScatter(ms, d, rng);
                        continue;
                    }
                }
            }

            // Beer-Lambert over the leg just travelled, in whichever medium the
            // branch was inside.
            if (medium >= 0) {
                const float a = sc.surfs[medium].absorption;
                if (a > 0.0f) {
                    const float survived = w * expf(-a * h.t);
                    atomicAdd(totals + 1, (double)(w - survived));
                    // Booked twice on purpose: once into the absorbed channel
                    // the budget closes on, and once into the bulk sub-total
                    // the reference reports separately. A budget that cannot
                    // say how much of its loss was the glass rather than the
                    // mirrors is half a budget.
                    atomicAdd(totals + 5, (double)(w - survived));
                    w = survived;
                }
            }

            // Two normals, and which of them answers which question is the whole
            // of it. The facet normal decides *geometry* -- which side of the
            // surface the ray is on, and which way a spawned branch steps off
            // it. The interpolated normal decides *physics* -- the Fresnel angle
            // and the refracted direction -- and is what keeps a coarse mesh
            // from deciding how sharply the scene focuses.
            //
            // `ngF` is the facet normal turned to face the incident ray, and it
            // is the arbiter wherever the two disagree. Orienting the shading
            // normal by its own sign instead -- flipping it whenever it happens
            // to point along the ray -- reverses it at a silhouette, where a
            // vertex normal has tipped past the incident direction. The
            // interface then refracts about a normal pointing into the solid,
            // the ray leaves on a direction no interface produces, and the
            // energy wanders off and is booked as escaped. That is a bias, not
            // noise: it fires only on oblique hits at curved refracting faces,
            // so it is invisible on a dome at normal incidence and costs about
            // a percent of the flux through a lens or an axicon cone.
            const bool   geoLeaving = dot3(d, fn) > 0.0f;
            const float3 ngF = geoLeaving ? mul3(fn, -1.0f) : fn;
            float3 n = ngF;
            const Shade* shadeSrc = inst ? sc.blasShade : sc.shade;
            if (shadeSrc) {
                const int si = inst ? (sc.blas[pl.blas].triFirst + h.tri) : h.tri;
                const Shade s3 = shadeSrc[si];
                const float bw = 1.0f - h.u - h.v;
                float3 ni = norm3(add3(add3(mul3(ld3(s3.n0), bw),
                                            mul3(ld3(s3.n1), h.u)),
                                       mul3(ld3(s3.n2), h.v)));
                // Only ever a refinement of the facet normal, never a reversal:
                // a mesh whose vertex normals disagree with its winding would
                // otherwise turn an entering ray into a leaving one. Compared
                // in the part's frame, where both normals were produced.
                if (dot3(ni, ld3(t.n)) < 0.0f) ni = ld3(t.n);
                ni = toWorldDir(ni);
                if (geoLeaving) ni = mul3(ni, -1.0f);
                // Past the incident ray at a silhouette: keep the facet, which
                // is the only normal that still describes the side the ray is on.
                if (dot3(ni, d) <= 0.0f) n = ni;
            }

            if (sf.isDetector) {
                // Which of the scene's receivers this surface is. One grid holds
                // them all end to end, the way the reference lays them out, so a
                // second receiver costs one more slice rather than a second run.
                const int di = (surfIx >= 0 && surfIx < sc.nDetOfSurf)
                                   ? sc.detOfSurf[surfIx] : -1;
                const Det dt = (di >= 0 && di < sc.nDets) ? sc.dets[di] : Det{};
                const bool accepted = (di < 0) || (dt.cosAcceptance <= -1.0f) ||
                                      (fabsf(dot3(d, ld3(dt.n))) >= dt.cosAcceptance);
                if (!accepted && !dt.rejectPasses) {
                    atomicAdd(totals + 4, (double)w);          // refused by the cone
                    alive = false;
                    break;
                }
                if (!accepted) { o = add3(p, mul3(d, eps)); continue; }

                // The diffuse bounce this branch came from already estimated its
                // direct contribution analytically. Counting this arrival too
                // would count that path twice, so its energy goes to the
                // estimator residual -- zero in expectation, and the budget only
                // closes with it in.
                if (skipDetector) {
                    atomicAdd(residual + 2, (double)w);
                    alive = false;
                    break;
                }

                if (di >= 0) {
                    const float3 r = sub3(p, ld3(dt.center));
                    const float lu = dot3(r, ld3(dt.u)) + 0.5f * dt.w;
                    const float lv = dot3(r, ld3(dt.v)) + 0.5f * dt.h;
                    if (lu >= 0.0f && lv >= 0.0f) {
                        const int bx = (int)(lu / dt.w * (float)dt.nx);
                        const int by = (int)(lv / dt.h * (float)dt.ny);
                        if (bx >= 0 && bx < dt.nx && by >= 0 && by < dt.ny) {
                            const int cell = dt.cellFirst + by * dt.nx + bx;
                            atomicAdd(grid + cell, (double)w);
                            if (wantSq)
                                atomicAdd(gridSq + cell, (double)w * (double)w);
                            // The colour bands describe the first receiver,
                            // which is the one the heatmap draws. They sum
                            // to the irradiance grid exactly.
                            if (bands && di == 0)
                                for (int b = 0; b < 3; ++b)
                                    atomicAdd(bandGrid + (size_t)b * sc.bandCells + cell,
                                              (double)w * (double)bandW[b]);
                        }
                    }
                }
                atomicAdd(totals + 0, (double)w);              // detected
                binDirection(angles, anglesSq, nTheta, nPhi, d, (double)w);
                if (sc.polarised && detStokes) {
                    // Flux-weighted, so the reported degree is the degree of
                    // the beam that arrived rather than the average over rays
                    // of very different weights.
                    atomicAdd(detStokes + 0, (double)w * (double)stokes.i);
                    atomicAdd(detStokes + 1, (double)w * (double)stokes.q);
                    atomicAdd(detStokes + 2, (double)w * (double)stokes.u);
                    atomicAdd(detStokes + 3, (double)w * (double)stokes.v);
                }
                atomicAdd(hits, 1ULL);
                detected += (double)w;
                if (repDet) atomicAdd(repDet + rep, (double)w);
                alive = false;
                break;
            }

            // One scattering model, resolved on the host, and the two forms it
            // takes are different kinds of thing.
            //
            //   a microfacet lobe is a *geometry*. It says which facet of the
            //   rough interface this ray actually met, so Fresnel and Snell are
            //   evaluated at that facet's normal and the branch leaves from it.
            //   A rough interface is rough for the transmitted ray too.
            //
            //   a redistribution lobe -- Lambertian, ABg, a measured table --
            //   leaves the interface smooth and changes where the energy goes
            //   after the split.
            const bool lobeSpecular  = (sf.bsdfModel == 0) || (sf.bsdfFraction <= 0.0f);
            const bool microfacet    = (sf.bsdfModel == 1) && !lobeSpecular;
            const bool redistributes = !lobeSpecular && !microfacet;
            const float lobeTis      = lobeSpecular ? 0.0f : sf.bsdfTis;

            // The facet this interaction happens at, and the specular direction
            // off it. `n` becomes that facet, so everything below reads one
            // normal.
            float3 microRefl = mk(0.0f, 0.0f, 0.0f);
            bool   haveMicro = false;
            if (microfacet && lobeTis > 0.0f && rnd(rng) < fminf(1.0f, lobeTis)) {
                float3 hN;
                float  wgt = 1.0f;
                if (sampleMicrofacet(d, n, sf.bsdfAlpha, rng, hN, microRefl, wgt)) {
                    n = hN;
                    haveMicro = true;
                    // How much of that facet is visible, shadowing and masking
                    // included. It scales the whole interaction rather than one
                    // branch of it, because it is a property of the facet and
                    // not of where the light goes next.
                    if (wgt != 1.0f) {
                        const float before = w;
                        w *= wgt;
                        atomicAdd(residual, (double)(before - w));
                    }
                }
            }

            const float cosI = fminf(1.0f, fmaxf(0.0f, -dot3(d, n)));

            // The reflected branch, shared by the refractive surface, its total
            // internal reflection case and the opaque surface, because the
            // reference shares it too: whatever decided the split, what leaves
            // a surface leaves through one piece of code.
            auto leaveReflected = [&]() {
                float3 refl = haveMicro ? microRefl : reflectDir(d, n);
                // An interpolated (or sampled) normal can put the reflected ray
                // under the facet, i.e. back into the solid it was meant to
                // bounce off. The facet decides which side is which.
                if (dot3(refl, ngF) <= 0.0f) refl = reflectDir(d, ngF);
                bool diffuse = false;
                if (redistributes && lobeTis > 0.0f && rnd(rng) < fminf(1.0f, lobeTis)) {
                    float3 out;
                    float  wgt = 1.0f;
                    // About the geometric normal, not the facet: a
                    // redistribution lobe belongs to the surface rather than to
                    // its microfacets.
                    if (lobeSample(sf, d, ngF, refl, sc.tableBeta, sc.tableValue,
                                   rng, out, wgt)) {
                        refl = out;
                        const float before = w;
                        w *= wgt;
                        atomicAdd(residual, (double)(before - w));
                        diffuse = (sf.bsdfModel == 3);
                    }
                }
                // A Lambertian bounce is where a random walk spends most of its
                // rays missing the receiver, so it is connected to every receiver
                // analytically instead and the arrival the sampled path would
                // have made is suppressed. Only when the estimate was actually
                // made: a declined one has to leave path sampling to carry it.
                skipDetector = false;
                if (diffuse) {
                    const float direct =
                        nextEventEstimate(sc, p, ngF, w, medium, surfIx, rng, grid,
                                          gridSq, totals, angles, anglesSq,
                                          nTheta, nPhi, bands ? bandGrid : nullptr,
                                          bandW, hits, wantSq);
                    if (direct >= 0.0f) {
                        skipDetector = true;
                        if (direct > 0.0f) {
                            atomicAdd(residual + 1, (double)(-direct));
                            detected += (double)direct;
                            if (repDet) atomicAdd(repDet + rep, (double)direct);
                        }
                    }
                }
                d = norm3(refl);
                o = offsetFrom(p, d, fn, eps);
            };

            if (sf.index > 0.0f) {
                // Which side of the interface this is, by the same three cases
                // the reference resolves: recorded inside this body, travelling
                // through vacuum, or inside some other body.
                bool onStack = false;
                for (int k = 0; k < mediaN; ++k) if (media[k] == surfIx) onStack = true;
                const bool leaving = onStack || (medium >= 0 && geoLeaving);
                // Crossing a face outward while recorded as being in vacuum:
                // the incident index has to be guessed. Legitimate for a bare
                // sheet, a symptom of a non-manifold mesh otherwise -- which
                // is what imported CAD produces. Counted rather than
                // silently recovered from, like the reference counts it.
                if (medium < 0 && geoLeaving && !onStack) atomicAdd(anomalies + 2, 1.0);

                float n1 = (medium >= 0) ? mediumIndex(sc, medium, lambda)
                                         : (leaving ? sf.index : 1.0f);
                if (n1 <= 0.0f) n1 = 1.0f;

                int afterMedia[kMediumMax];
                int afterN = 0;
                for (int k = 0; k < mediaN; ++k) afterMedia[afterN++] = media[k];
                if (leaving) {
                    const int drop = onStack ? surfIx : medium;
                    int m = 0;
                    for (int k = 0; k < afterN; ++k)
                        if (afterMedia[k] != drop) afterMedia[m++] = afterMedia[k];
                    if (m == afterN) atomicAdd(anomalies + 0, 1.0);   // nothing popped
                    afterN = m;
                } else if (afterN < kMediumMax) {
                    afterMedia[afterN++] = surfIx;
                } else {
                    atomicAdd(anomalies + 1, 1.0);                    // stack full
                }
                const int afterMed = effectiveMedium(sc, afterMedia, afterN);
                float n2 = (afterMed >= 0) ? mediumIndex(sc, afterMed, lambda) : 1.0f;
                if (n2 <= 0.0f) n2 = 1.0f;

                // The coating rides on the Fresnel split and only on it: with
                // the Fresnel term switched off the reference splits by the
                // surface's own reflectivity and never consults the coating, so
                // neither does this.
                float R;
                Stokes4 reflState = stokes, transState = stokes;
                bool havePol = false;
                if (sf.fresnel) {
                    float Rs, Rp;
                    fresnelSP(n1, n2, cosI, Rs, Rp);
                    coated(Rs, Rp, n1, n2, sf.coatResidual, sf.coatHighReflector);
                    R = fminf(1.0f, 0.5f * (Rs + Rp));
                    if (sc.polarised) {
                        // The split comes from the state rather than from the
                        // unpolarised average of the two: s-polarised light
                        // meeting glass at Brewster reflects strongly and
                        // p-polarised light reflects nothing at all, and that
                        // difference is the whole reason to carry a state.
                        float as, ap, phase;
                        fresnelAmplitudes(n1, n2, cosI, as, ap, phase);
                        if (sf.coatResidual >= 0.0f) {
                            as = sqrtf(fmaxf(0.0f, Rs));
                            ap = sqrtf(fmaxf(0.0f, Rp));
                        }
                        Stokes4 inState = stokes;
                        if (dot3(polFrame, polFrame) > 0.0f)
                            inState = rotateStokes(
                                inState, frameRotation(d, polFrame, ngF));
                        const Stokes4 outS =
                            applyFresnelMueller(inState, as, ap, phase);
                        const float total = fmaxf(1e-18f, inState.i);
                        R = fminf(1.0f, fmaxf(0.0f, outS.i / total));
                        // Transmission takes what reflection did not, component
                        // by component: that difference is what makes a stack of
                        // plates a polariser.
                        Stokes4 tS;
                        tS.i = inState.i - outS.i;
                        tS.q = inState.q - outS.q;
                        tS.u = inState.u - outS.u;
                        tS.v = inState.v - outS.v;
                        // Renormalised, because flux lives in the ray weight and
                        // only the direction of polarisation lives here.
                        reflState  = (outS.i > 1e-15f)
                                         ? stokesScaled(outS, 1.0f / outS.i)
                                         : stokesUnpolarised();
                        transState = (tS.i > 1e-15f)
                                         ? stokesScaled(tS, 1.0f / tS.i)
                                         : stokesUnpolarised();
                        havePol = true;
                    }
                } else {
                    R = 1.0f - sf.transmissivity;
                }
                // One branch, chosen with the probability its share of the
                // energy deserves. The weight is not scaled: that is what makes
                // this an unbiased estimator of the tree rather than a darker
                // picture of it.
                if (rnd(rng) < R) {
                    if (havePol) { stokes = reflState; polFrame = ngF; }
                    leaveReflected();
                } else {
                    const float eta = n1 / n2;
                    float3 tD;
                    if (!refractDir(d, n, eta, cosI, tD)) {
                        // Snell refused where the Fresnel term did not. With
                        // Fresnel on that is only reachable on rounding, since
                        // the Fresnel term is already 1 past the critical
                        // angle. With the Fresnel switch off it is instead every
                        // ray past the critical angle, so it is the ordinary
                        // total-internal-reflection path and not a corner. The
                        // reference folds the transmitted share back into the
                        // reflected branch; with one branch sampled, that is
                        // simply reflecting -- through the same branch every
                        // other bounce leaves by, scatter lobe included.
                        if (havePol) { stokes = reflState; polFrame = ngF; }
                        leaveReflected();
                    } else {
                        // Same arbitration as the reflected branch, and the same
                        // one the reference makes: a transmitted ray that comes
                        // out on the near side was bent there by the
                        // interpolation, so re-solve about the facet. If the
                        // facet refuses it too, the energy is truncated and
                        // counted rather than left to wander off as escaped.
                        bool ok = true;
                        if (dot3(tD, ngF) >= 0.0f) {
                            ok = refractDir(d, ngF, eta,
                                            fminf(1.0f, fmaxf(0.0f, -dot3(d, ngF))), tD);
                        }
                        if (!ok) {
                            atomicAdd(totals + 3, (double)w);   // refraction failed
                            alive = false;
                            break;
                        }
                        // A transmissive diffuser re-emits about the far side
                        // of the surface, through the same lobe the reflected
                        // branch uses.
                        bool tDiffuse = false;
                        if (redistributes && lobeTis > 0.0f &&
                            rnd(rng) < fminf(1.0f, lobeTis)) {
                            float3 out;
                            float  wgt = 1.0f;
                            if (lobeSample(sf, d, mul3(ngF, -1.0f), tD,
                                           sc.tableBeta, sc.tableValue,
                                           rng, out, wgt)) {
                                tD = out;
                                const float before = w;
                                w *= wgt;
                                atomicAdd(residual, (double)(before - w));
                                tDiffuse = (sf.bsdfModel == 3);
                            }
                        }
                        // The far side of a transmissive diffuser connects the
                        // same way the reflected side does, about the normal it
                        // left through.
                        skipDetector = false;
                        if (tDiffuse) {
                            const float direct =
                                nextEventEstimate(sc, p, mul3(ngF, -1.0f), w,
                                                  afterMed, surfIx, rng, grid,
                                                  gridSq, totals, angles, anglesSq,
                                                  nTheta, nPhi,
                                                  bands ? bandGrid : nullptr, bandW,
                                                  hits, wantSq);
                            if (direct >= 0.0f) {
                                skipDetector = true;
                                if (direct > 0.0f) {
                                    atomicAdd(residual + 1, (double)(-direct));
                                    detected += (double)direct;
                                    if (repDet) atomicAdd(repDet + rep, (double)direct);
                                }
                            }
                        }
                        if (havePol) { stokes = transState; polFrame = ngF; }
                        d = tD;
                        o = offsetFrom(p, d, fn, eps);
                        for (int k2 = 0; k2 < afterN; ++k2) media[k2] = afterMedia[k2];
                        mediaN = afterN;
                        medium = afterMed;
                    }
                }
            } else {
                // Opaque: reflect what the surface reflects, book the rest. A
                // metal's share comes from its complex index and the angle.
                const float R = (sf.metalK > 0.0f)
                                    ? metalR(medium >= 0 ? mediumIndex(sc, medium, lambda)
                                                         : 1.0f,
                                             sf.metalN, sf.metalK, cosI)
                                    : sf.reflectivity;
                float Rstate = R;
                if (sc.polarised && sf.metalK > 0.0f) {
                    // A mirror is where a polarised run gains most of its state:
                    // aluminium at sixty degrees turns linear light elliptical, so
                    // a periscope of two mirrors is not a null. Without this the
                    // reflected branch would inherit the incoming state verbatim
                    // and the run would report the source back to itself.
                    float as, ap, phase;
                    metalAmplitudes(medium >= 0 ? mediumIndex(sc, medium, lambda)
                                                : 1.0f,
                                    sf.metalN, sf.metalK, cosI, as, ap, phase);
                    Stokes4 inState = stokes;
                    if (dot3(polFrame, polFrame) > 0.0f)
                        inState = rotateStokes(inState,
                                               frameRotation(d, polFrame, ngF));
                    const Stokes4 outS = applyFresnelMueller(inState, as, ap, phase);
                    // The reflectance the state sees, which is what makes an
                    // s-polarised beam and a p-polarised one leave a mirror with
                    // different energies rather than the same average.
                    const float total = fmaxf(1e-18f, inState.i);
                    Rstate = fminf(1.0f, fmaxf(0.0f, outS.i / total));
                    stokes = (outS.i > 1e-15f) ? stokesScaled(outS, 1.0f / outS.i)
                                               : stokesUnpolarised();
                    polFrame = ngF;
                }
                if (rnd(rng) >= Rstate) {
                    atomicAdd(totals + 1, (double)w);          // absorbed
                    alive = false;
                    break;
                }
                leaveReflected();
            }
        }

        if (alive) atomicAdd(totals + 3, (double)w);           // ran out of depth
        atomicAdd(moments + 0, detected);
        atomicAdd(moments + 1, detected * detected);
    }
}

void say(char* buf, int cap, const char* msg) {
    if (!buf || cap <= 0) return;
    std::snprintf(buf, (size_t)cap, "%s", msg);
}

} // namespace

bool probe(char* name, int nameCap, char* why, int whyCap) {
    int n = 0;
    const cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess) { say(why, whyCap, cudaGetErrorString(e)); return false; }
    if (n <= 0) { say(why, whyCap, "no CUDA device"); return false; }
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) {
        say(why, whyCap, "device 0 did not answer");
        return false;
    }
    if (name && nameCap > 0)
        std::snprintf(name, (size_t)nameCap, "%s (sm_%d%d, %d SMs)",
                      prop.name, prop.major, prop.minor, prop.multiProcessorCount);
    return true;
}

bool run(const Input& in, Output& out, char* err, int errCap) {
    void* dTris = nullptr; void* dShade = nullptr; void* dNodes = nullptr;
    void* dOrder = nullptr; void* dSurfs = nullptr;
    void* dGrid = nullptr; void* dGridSq = nullptr; void* dScalars = nullptr;
    void* dAngles = nullptr; void* dAnglesSq = nullptr;
    void* dTabB = nullptr; void* dTabV = nullptr;
    void* dSobol = nullptr; void* dRepDet = nullptr; void* dRepEmit = nullptr;
    void* dDets = nullptr; void* dDetOfSurf = nullptr;
    void* dInvCdf = nullptr; void* dSpec = nullptr; void* dBand = nullptr;
    void* dBTris = nullptr; void* dBShade = nullptr; void* dBNodes = nullptr;
    void* dBOrder = nullptr; void* dBlas = nullptr; void* dInsts = nullptr;
    void* dTlas = nullptr; void* dTlasOrder = nullptr;

    auto cleanup = [&]() {
        cudaFree(dTris); cudaFree(dShade); cudaFree(dNodes); cudaFree(dOrder);
        cudaFree(dSurfs); cudaFree(dGrid); cudaFree(dGridSq); cudaFree(dScalars);
        cudaFree(dAngles); cudaFree(dAnglesSq);
        cudaFree(dTabB); cudaFree(dTabV);
        cudaFree(dSobol); cudaFree(dRepDet); cudaFree(dRepEmit);
        cudaFree(dDets); cudaFree(dDetOfSurf);
        cudaFree(dInvCdf); cudaFree(dSpec); cudaFree(dBand);
        cudaFree(dBTris); cudaFree(dBShade); cudaFree(dBNodes); cudaFree(dBOrder);
        cudaFree(dBlas); cudaFree(dInsts); cudaFree(dTlas); cudaFree(dTlasOrder);
    };
    auto fail = [&](const char* m) { say(err, errCap, m); cleanup(); return false; };

    auto upload = [&](void** dst, const void* host, size_t bytes) {
        if (bytes == 0) { *dst = nullptr; return true; }
        if (cudaMalloc(dst, bytes) != cudaSuccess) return false;
        return cudaMemcpy(*dst, host, bytes, cudaMemcpyHostToDevice) == cudaSuccess;
    };

    if (!upload(&dTris,  in.tris,  size_t(in.nTris)  * sizeof(Tri)))   return fail("upload: triangles");
    if (!upload(&dNodes, in.nodes, size_t(in.nNodes) * sizeof(Node)))  return fail("upload: hierarchy");
    if (!upload(&dOrder, in.order, size_t(in.nOrder) * sizeof(int)))   return fail("upload: order");
    if (!upload(&dSurfs, in.surfs, size_t(in.nSurfs) * sizeof(Surf)))  return fail("upload: surfaces");
    if (in.shade && !upload(&dShade, in.shade, size_t(in.nTris) * sizeof(Shade)))
        return fail("upload: shading normals");
    if (in.nTable > 0) {
        if (!upload(&dTabB, in.tableBeta,  size_t(in.nTable) * sizeof(float)) ||
            !upload(&dTabV, in.tableValue, size_t(in.nTable) * sizeof(float)))
            return fail("upload: measured BSDF tables");
    }
    if (!upload(&dDets, in.dets, size_t(in.nDets) * sizeof(Det)))
        return fail("upload: receivers");
    if (!upload(&dDetOfSurf, in.detOfSurf, size_t(in.nDetOfSurf) * sizeof(int)))
        return fail("upload: receiver map");
    if (in.invCdf && in.spdBins > 0 &&
        !upload(&dInvCdf, in.invCdf, size_t(in.spdBins + 1) * sizeof(float)))
        return fail("upload: spectrum");
    if (in.specTable && in.specSamples > 0 &&
        !upload(&dSpec, in.specTable,
                size_t(in.nSurfs) * 4 * size_t(in.specSamples) * sizeof(float)))
        return fail("upload: dispersion curves");
    if (in.sobolDir &&
        !upload(&dSobol, in.sobolDir, 8 * 32 * sizeof(unsigned int)))
        return fail("upload: Sobol direction numbers");
    const int nRep = (in.replicas > 0) ? in.replicas : 1;
    if (out.repDet && out.repEmit) {
        if (cudaMalloc(&dRepDet,  size_t(nRep) * sizeof(double)) != cudaSuccess ||
            cudaMalloc(&dRepEmit, size_t(nRep) * sizeof(double)) != cudaSuccess)
            return fail("alloc: replica accumulators");
        cudaMemset(dRepDet,  0, size_t(nRep) * sizeof(double));
        cudaMemset(dRepEmit, 0, size_t(nRep) * sizeof(double));
    }
    if (in.nInsts > 0) {
        if (!upload(&dBTris,  in.blasTris,  size_t(in.nBlasTris)  * sizeof(Tri))   ||
            !upload(&dBNodes, in.blasNodes, size_t(in.nBlasNodes) * sizeof(Node))  ||
            !upload(&dBOrder, in.blasOrder, size_t(in.nBlasOrder) * sizeof(int))   ||
            !upload(&dBlas,   in.blas,      size_t(in.nBlas)      * sizeof(Blas))  ||
            !upload(&dInsts,  in.insts,     size_t(in.nInsts)     * sizeof(Inst))  ||
            !upload(&dTlas,   in.tlas,      size_t(in.nTlas)      * sizeof(Node))  ||
            !upload(&dTlasOrder, in.tlasOrder, size_t(in.nTlasOrder) * sizeof(int)))
            return fail("upload: instanced geometry");
        if (in.blasShade &&
            !upload(&dBShade, in.blasShade, size_t(in.nBlasTris) * sizeof(Shade)))
            return fail("upload: instanced shading normals");
    }

    const size_t cells = size_t(in.gridCells);
    if (cudaMalloc(&dGrid, cells * sizeof(double)) != cudaSuccess) return fail("alloc: receiver grid");
    cudaMemset(dGrid, 0, cells * sizeof(double));
    if (out.bandGrid) {
        if (cudaMalloc(&dBand, 3 * cells * sizeof(double)) != cudaSuccess)
            return fail("alloc: colour bands");
        cudaMemset(dBand, 0, 3 * cells * sizeof(double));
    }
    if (in.wantVariance) {
        if (cudaMalloc(&dGridSq, cells * sizeof(double)) != cudaSuccess) return fail("alloc: noise map");
        cudaMemset(dGridSq, 0, cells * sizeof(double));
    }
    const size_t angleCells = size_t(in.nThetaMaster) * size_t(in.nPhi);
    if (angleCells > 0 && out.angles) {
        if (cudaMalloc(&dAngles, angleCells * sizeof(double)) != cudaSuccess)
            return fail("alloc: far field");
        cudaMemset(dAngles, 0, angleCells * sizeof(double));
        if (out.anglesSq) {
            if (cudaMalloc(&dAnglesSq, angleCells * sizeof(double)) != cudaSuccess)
                return fail("alloc: far-field noise");
            cudaMemset(dAnglesSq, 0, angleCells * sizeof(double));
        }
    }
    // totals[6], moments[2], residual[5], anomalies[3], hits[1] in one
    // allocation. The three residual channels are the BSDF sampling weight,
    // the analytic direct term and the arrival it suppressed -- named apart
    // rather than summed, because a channel that is systematically wrong
    // must not be able to hide behind one that cancels it.
    if (cudaMalloc(&dScalars, 21 * sizeof(double)) != cudaSuccess) return fail("alloc: accumulators");
    cudaMemset(dScalars, 0, 21 * sizeof(double));

    DevScene sc;
    sc.tris   = (const Tri*)   dTris;
    sc.shade  = (const Shade*) dShade;
    sc.nodes  = (const Node*)  dNodes;
    sc.order  = (const int*)   dOrder;
    sc.surfs  = (const Surf*)  dSurfs;
    sc.sobolDir   = (const unsigned int*) dSobol;
    sc.tableBeta  = (const float*) dTabB;
    sc.tableValue = (const float*) dTabV;
    sc.blasTris   = (const Tri*)   dBTris;
    sc.blasShade  = (const Shade*) dBShade;
    sc.blasNodes  = (const Node*)  dBNodes;
    sc.blasOrder  = (const int*)   dBOrder;
    sc.blas       = (const Blas*)  dBlas;
    sc.insts      = (const Inst*)  dInsts;
    sc.tlas       = (const Node*)  dTlas;
    sc.tlasOrder  = (const int*)   dTlasOrder;
    sc.dets       = (const Det*) dDets;
    sc.detOfSurf  = (const int*) dDetOfSurf;
    sc.nDets      = in.nDets;
    sc.nDetOfSurf = in.nDetOfSurf;
    sc.nNodes     = in.nNodes;
    sc.nTlas      = in.nTlas;
    sc.nextEvent  = in.nextEvent;
    sc.invCdf     = (const float*) dInvCdf;
    sc.specTable  = (const float*) dSpec;
    sc.spdBins    = in.spdBins;
    sc.spdMono    = in.spdMono;
    sc.spdRgb     = in.spdRgb;
    sc.monoLambda = in.monoLambda;
    sc.meanV      = in.meanV;
    sc.unitLumen  = in.unitLumen;
    sc.spectral   = in.spectral;
    sc.polarised  = in.polarised;
    sc.emittedState = make_float4(in.emittedState[0], in.emittedState[1],
                                  in.emittedState[2], in.emittedState[3]);
    sc.specSamples = in.specSamples;
    sc.specMinNm  = in.specMinNm;
    sc.specMaxNm  = in.specMaxNm;
    sc.bandCells  = cells;
    sc.boundsCentre = make_float3(in.boundsCentre[0], in.boundsCentre[1],
                                  in.boundsCentre[2]);
    sc.boundsRadius = in.boundsRadius;
    sc.aim          = in.aim;
    sc.src        = in.src;

    double* totals    = (double*)dScalars;
    double* moments   = totals + 6;
    // bsdf weight, the analytic direct term, the arrival it suppressed,
    // aiming, roulette.
    double* residual  = totals + 8;
    double* anomalies = totals + 13;
    unsigned long long* hits = (unsigned long long*)(totals + 16);
    double* detStokes = totals + 17;

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0);
    traceKernel<<<4096, 256>>>(sc, (double*)dGrid, (double*)dGridSq, (double*)dAngles, (double*)dAnglesSq,
                               dAngles ? in.nThetaMaster : 0, in.nPhi,
                               (double*)dBand, out.bandGrid ? 1 : 0,
                               totals, moments, residual, anomalies,
                               (double*)dRepDet, (double*)dRepEmit,
                               in.polarised ? detStokes : nullptr,
                               hits, in.seed, in.rays, in.wantVariance,
                               nRep, in.lowDiscrepancy, in.roulette);
    cudaEventRecord(t1);
    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) {
        cudaEventDestroy(t0); cudaEventDestroy(t1);
        return fail(cudaGetErrorString(launch));
    }
    const cudaError_t sync = cudaDeviceSynchronize();
    if (sync != cudaSuccess) {
        cudaEventDestroy(t0); cudaEventDestroy(t1);
        return fail(cudaGetErrorString(sync));
    }
    cudaEventElapsedTime(&out.milliseconds, t0, t1);
    cudaEventDestroy(t0); cudaEventDestroy(t1);

    cudaMemcpy(out.grid, dGrid, cells * sizeof(double), cudaMemcpyDeviceToHost);
    if (dBand && out.bandGrid)
        cudaMemcpy(out.bandGrid, dBand, 3 * cells * sizeof(double),
                   cudaMemcpyDeviceToHost);
    if (in.wantVariance && out.gridSq)
        cudaMemcpy(out.gridSq, dGridSq, cells * sizeof(double), cudaMemcpyDeviceToHost);
    if (dAngles && out.angles)
        cudaMemcpy(out.angles, dAngles, angleCells * sizeof(double), cudaMemcpyDeviceToHost);
    if (dAnglesSq && out.anglesSq)
        cudaMemcpy(out.anglesSq, dAnglesSq, angleCells * sizeof(double), cudaMemcpyDeviceToHost);
    if (dRepDet && out.repDet)
        cudaMemcpy(out.repDet, dRepDet, size_t(nRep) * sizeof(double),
                   cudaMemcpyDeviceToHost);
    if (dRepEmit && out.repEmit)
        cudaMemcpy(out.repEmit, dRepEmit, size_t(nRep) * sizeof(double),
                   cudaMemcpyDeviceToHost);
    double host[21] = {0};
    cudaMemcpy(host, dScalars, 21 * sizeof(double), cudaMemcpyDeviceToHost);
    for (int i = 0; i < 6; ++i) out.totals[i] = host[i];
    out.moments[0]         = host[6];
    out.moments[1]         = host[7];
    out.residualBsdf       = host[8];
    out.residualNee        = host[9];
    out.residualSkipDet    = host[10];
    out.residualAiming     = host[11];
    out.residualRoulette   = host[12];
    for (int i = 0; i < 3; ++i) out.anomalies[i] = (unsigned long long)host[13 + i];
    std::memcpy(&out.hits, &host[16], sizeof(unsigned long long));
    for (int i = 0; i < 4; ++i) out.detStokes[i] = host[17 + i];

    cleanup();
    return true;
}

} // namespace gpukernel
