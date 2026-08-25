#pragma once
#include "Vec3.h"
#include <cmath>
#include <cstdint>

// Physics primitives shared by the tracer, the sampler and the tests.
//
// Everything here is a small pure function on doubles and sits in the innermost
// loop of the trace, so it is header-only and inlined rather than hidden behind
// a translation-unit boundary.
namespace optics {

constexpr double kPi     = 3.14159265358979323846;
constexpr double kTwoPi  = 6.28318530717958647692;
constexpr double kDegRad = kPi / 180.0;

// Reference line the stored refractive indices are quoted at.
constexpr double kLambdaD = 587.6;      // nm, helium d line

// The three bands an RGB trace splits the source into. Roughly the sRGB
// primaries, which is what makes the resulting heatmap readable as a colour.
constexpr double kBandNm[3] = {620.0, 546.1, 460.0};

// ---- RNG -------------------------------------------------------------------
// Counter-based, so a stream can be reconstructed from an index alone. That is
// what keeps a trace bit-identical no matter how the chunks were scheduled.

inline std::uint64_t mix64(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline std::uint64_t splitmix64(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ull;
    return mix64(state);
}

// 53 significant bits, in [0, 1).
inline double uniform01(std::uint64_t& state) {
    return double(splitmix64(state) >> 11) * (1.0 / 9007199254740992.0);
}

// Standard normal, Box-Muller. Only one of the pair is kept: caching the second
// would make the draw depend on how many draws came before it, and the whole
// point of the counter-based stream is that it does not.
inline double gaussian(std::uint64_t& state) {
    const double u1 = uniform01(state);
    const double u2 = uniform01(state);
    return std::sqrt(-2.0 * std::log(u1 > 1e-300 ? u1 : 1e-300)) * std::cos(kTwoPi * u2);
}

// ---- geometry --------------------------------------------------------------

// An orthonormal pair spanning the plane perpendicular to a unit vector `n`.
inline void orthonormalBasis(const Vec3& n, Vec3& u, Vec3& v) {
    // Duff et al., branchless and stable for every n including n.z == -1.
    const double sign = std::copysign(1.0, n.z);
    const double a    = -1.0 / (sign + n.z);
    const double b    = n.x * n.y * a;
    u = Vec3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    v = Vec3(b, sign + n.y * n.y * a, -n.y);
}

// Cosine-weighted direction in the hemisphere about the unit normal `n`.
// This is the emission law of a Lambertian surface, so it is what a diffuse
// scatter re-emits into.
inline Vec3 cosineHemisphere(const Vec3& n, double u1, double u2) {
    Vec3 t, b;
    orthonormalBasis(n, t, b);
    const double r   = std::sqrt(u1);
    const double phi = kTwoPi * u2;
    const double z   = std::sqrt(std::max(0.0, 1.0 - u1));
    Vec3 d = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * z;
    d.normalize();
    return d;
}

// The Gaussian normal tilt that used to stand in for surface roughness is gone.
// It decided a direction while the energy stayed whatever specular Fresnel gave
// it at the *smooth* normal, so a rough dielectric reflected as though it were
// polished and then left in a diffuse direction; it had no shadowing-masking
// term; and its rejection branch biased it toward specular by however often the
// tilt tipped past the incident ray. Roughness is a GGX microfacet now -- see
// SurfaceOptics::effectiveBsdf, which reads the same RMS-slope number into one.

// ---- interfaces ------------------------------------------------------------

// Cauchy dispersion. `nd` is the index at the d line and `B` the coefficient in
// um^2; A is derived so that n(kLambdaD) == nd exactly, which keeps a
// non-dispersive trace and a dispersive one identical at the reference line.
inline double cauchyIndex(double nd, double B, double lambdaNm) {
    if (B == 0.0 || lambdaNm <= 0.0) return nd;
    const double lu = lambdaNm * 1e-3;                 // nm -> um
    const double ld = kLambdaD * 1e-3;
    return nd + B * (1.0 / (lu * lu) - 1.0 / (ld * ld));
}

// Unpolarised Fresnel reflectance at an interface n1 -> n2, given the cosine of
// the angle of incidence (>= 0, measured against the normal facing the incident
// ray). Returns 1 for total internal reflection.
//
// R = (Rs + Rp) / 2 with
//   Rs = ((n1 cosI - n2 cosT) / (n1 cosI + n2 cosT))^2
//   Rp = ((n1 cosT - n2 cosI) / (n1 cosT + n2 cosI))^2
// At normal incidence both collapse to ((n1-n2)/(n1+n2))^2 -- 4 % for air/glass.
inline double fresnelReflectance(double n1, double n2, double cosI) {
    cosI = cosI < 0.0 ? -cosI : cosI;
    if (cosI > 1.0) cosI = 1.0;
    const double eta   = n1 / n2;
    const double sinT2 = eta * eta * (1.0 - cosI * cosI);
    if (sinT2 >= 1.0) return 1.0;                       // total internal reflection
    const double cosT = std::sqrt(1.0 - sinT2);
    const double rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    const double rp = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
    return 0.5 * (rs * rs + rp * rp);
}

// Snell refraction. `d` and `n` must be unit, with `n` facing the incident ray
// (so cosI = -d.n >= 0). Returns false on total internal reflection, in which
// case `out` is left untouched.
inline bool refract(const Vec3& d, const Vec3& n, double eta, double cosI, Vec3& out) {
    const double sinT2 = eta * eta * (1.0 - cosI * cosI);
    if (sinT2 >= 1.0) return false;
    Vec3 t = d * eta + n * (eta * cosI - std::sqrt(1.0 - sinT2));
    if (!t.normalize()) return false;
    out = t;
    return true;
}

inline Vec3 reflect(const Vec3& d, const Vec3& n) {
    return d - n * (2.0 * d.dot(n));
}

} // namespace optics
