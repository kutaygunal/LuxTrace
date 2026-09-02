#include "RayTracer.h"
#include "Material.h"
#include "Sampling.h"
#include "Metasurface.h"
#include "Optics.h"
#include "ThreadPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace optics;

// Surface offset and minimum hit distance, RELATIVE to the coordinate
// magnitude of the hit point. An absolute epsilon is right at one scale only:
// 1e-6 mm is generous near the origin and, a metre out, sits inside the last
// few bits a double has left -- which is how a grazing hit self-intersects or
// leaks straight through an optic.
constexpr double kEpsRel   = 1e-9;

// Energy below this is not discarded -- it is put to Russian roulette, so a
// surviving branch carries exactly this much and the estimator stays unbiased.
// The old hard cutoff always removed energy and never added any, which biased
// every efficiency figure low by the truncated share.
constexpr double kRoulette = 1e-4;

// Both branches of an interface are followed only when neither is a small
// fraction of the pair. Below that the tree is collapsed by picking one branch
// weighted by its share and handing it the whole amount -- unbiased, and on a
// ball lens it turns a binary tree into a single path for the great majority
// of rays.
constexpr double kSplitMin = 0.05;

constexpr int    kMaxDepth = 160;    // max interactions per path (TIR guides need many)

// How many media a branch can be inside at once. Cemented doublets, immersed
// optics and clad guides need two or three; eight is past anything real.
constexpr int    kMediumDepth = 8;

// Offset magnitude for a branch spawned at `p`, and the minimum hit distance
// for a ray starting there.
inline double surfaceEps(const Vec3& p) {
    const double m = std::max(std::max(std::fabs(p.x), std::fabs(p.y)), std::fabs(p.z));
    return kEpsRel * (m > 1.0 ? m : 1.0);
}

// Neumaier compensated summation. The flux totals accumulate over up to 10^7
// rays, so naive sequential summation loses digits that the determinism
// guarantee then faithfully reproduces. Compensating costs three flops and
// preserves the summation order exactly, so the guarantee is untouched.
struct Sum {
    double s = 0.0, c = 0.0;

    void add(double v) {
        const double t = s + v;
        if (std::fabs(s) >= std::fabs(v)) c += (s - t) + v;
        else                              c += (v - t) + s;
        s = t;
    }
    void merge(const Sum& o) { add(o.s); add(o.c); }
    double value() const { return s + c; }
};

// Welford online mean and variance. E[x^2] - E[x]^2 cancels catastrophically
// when the efficiency is high -- on a 90 % scene most of the significant digits
// are gone before the subtraction -- and this never forms that difference.
struct Moments {
    double n = 0.0, mean = 0.0, m2 = 0.0;

    void add(double x) {
        n += 1.0;
        const double d = x - mean;
        mean += d / n;
        m2   += d * (x - mean);
    }
    // Chan parallel combination, so chunks reduce in a fixed order.
    void merge(const Moments& o) {
        if (o.n == 0.0) return;
        if (n == 0.0) { *this = o; return; }
        const double tn = n + o.n;
        const double d  = o.mean - mean;
        mean += d * (o.n / tn);
        m2   += o.m2 + d * d * (n * o.n / tn);
        n     = tn;
    }
    double variance() const { return n > 1.0 ? m2 / n : 0.0; }
};

// The set of media a branch is currently inside, innermost last. Entries are
// surface indices, so the glass a ray is in keeps its identity -- which is what
// lets its dispersion and its bulk absorption be looked up on the way out.
struct MediumStack {
    std::int16_t e[kMediumDepth] = {};
    std::uint8_t n = 0;

    bool contains(int idx) const {
        for (std::uint8_t i = 0; i < n; ++i)
            if (e[i] == std::int16_t(idx)) return true;
        return false;
    }
    // False when the stack was already full, so the entry was dropped. The
    // recovery is right and the silence was not: a mesh that nests more than
    // kMediumDepth deep produces systematically wrong index pairs and a
    // perfectly clean-looking result.
    bool push(int idx) {
        if (n >= kMediumDepth) return false;
        e[n++] = std::int16_t(idx);
        return true;
    }
    // Removes the innermost entry matching `idx`. A stack that never saw it is
    // left alone: exiting a body one was never recorded as entering is a
    // modelling artefact, not a reason to corrupt the rest of the stack -- but
    // it is one worth counting, which is what the return value is for.
    bool popValue(int idx) {
        for (int i = int(n) - 1; i >= 0; --i) {
            if (e[i] != std::int16_t(idx)) continue;
            for (int j = i + 1; j < int(n); ++j) e[j - 1] = e[j];
            --n;
            return true;
        }
        return false;
    }
};


// A path is a binary tree (reflected + transmitted branch), walked depth first,
// so the pending stack never exceeds one entry per depth level.
constexpr int kPathStack = kMaxDepth + 8;

// How much memory the per-thread accumulators may take between them. Past this
// the run uses fewer threads rather than more memory -- see the note where it is
// applied.
constexpr std::size_t kGridBudgetBytes = 256ull * 1024ull * 1024ull;

// Rays are handed out in chunks so threads stay busy on wildly uneven paths
// (a TIR bounce chain costs orders of magnitude more than an escaping ray)
// while cancellation still responds within a few milliseconds.
constexpr std::size_t kChunkRays = 512;

// Distinct constants so the emission stream and the interaction stream of the
// same ray index never coincide.
constexpr std::uint64_t kEmissionSalt    = 0xD1B54A32D192ED03ull;
constexpr std::uint64_t kInteractionSalt = 0x9E6C63D0676A9A99ull;
constexpr std::uint64_t kReservoirSalt   = 0xC2B2AE3D27D4EB4Full;
// And one more so no two sources coincide with each other. Every source is
// indexed by its own local ray number -- which is what keeps the first source
// emitting exactly what it emitted when it was alone -- and that on its own
// handed every later source the identical sequence of draws. Four LEDs in an
// array emitted four copies of one angular pattern: the budget bought the
// variance of a quarter of it, and the replica spread the error bar is read
// from was measuring a run that had never been made.
constexpr std::uint64_t kSourceSalt      = 0xA24BAED4963EE407ull;

// ---- emission --------------------------------------------------------------

// An orthonormal basis around the emission axis.
struct EmissionBasis {
    Vec3 e1, e2, axis;

    explicit EmissionBasis(const gp_Dir& a) {
        axis = Vec3(a);
        axis.normalize();
        orthonormalBasis(axis, e1, e2);
    }
};

// The angular law, about an arbitrary unit normal rather than about the source
// axis, so a Sphere emitter can reuse it per surface point.
Vec3 sampleAngular(const Vec3& n, SourceConfig::Type type, double halfAngleDeg,
                   double u1, double u2) {
    Vec3 t, b;
    orthonormalBasis(n, t, b);
    const double phi = kTwoPi * u2;
    double cosT;
    if (type == SourceConfig::Type::Collimated) {
        cosT = 1.0;
    } else if (type == SourceConfig::Type::Lambertian) {
        // Cosine-weighted inside the cone: sin(theta) = sin(thetaMax) sqrt(u1).
        const double thMax = std::min(90.0, std::max(0.0, halfAngleDeg)) * kDegRad;
        const double s     = std::sin(thMax);
        cosT = std::sqrt(std::max(0.0, 1.0 - s * s * u1));
    } else {
        // Uniform over solid angle inside the cone. At 180 degrees this is
        // cosT = 1 - 2 u1, the whole sphere.
        const double thMax = std::min(180.0, std::max(0.0, halfAngleDeg)) * kDegRad;
        cosT = 1.0 - u1 * (1.0 - std::cos(thMax));
    }
    const double sinT = std::sqrt(std::max(0.0, 1.0 - cosT * cosT));
    Vec3 d = t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) + n * cosT;
    d.normalize();
    return d;
}

// The cone of directions the scene occupies, seen from one emission point.
// A direction outside it cannot intersect anything at all.
struct AimingCone {
    bool   active = false;
    Vec3   axis;              // unit, toward the scene
    double cosHalf = -1.0;
    double omega   = 0.0;     // solid angle of the cone, sr
};

// Solid angle of the source's own emission law, so aiming can be declined when
// it would not narrow anything.
inline double sourceSolidAngle(const SourceConfig& src) {
    if (src.type == SourceConfig::Type::Collimated) return 0.0;
    if (src.type == SourceConfig::Type::Lambertian) {
        const double th = std::min(90.0, std::max(0.0, src.halfAngleDeg)) * kDegRad;
        const double s  = std::sin(th);
        return kPi * s * s;                       // projected solid angle
    }
    const double th = std::min(180.0, std::max(0.0, src.halfAngleDeg)) * kDegRad;
    return kTwoPi * (1.0 - std::cos(th));
}

AimingCone aimingConeFor(const Vec3& origin, const Vec3& sceneCenter, double sceneRadius,
                         const SourceConfig& src) {
    AimingCone c;
    if (sceneRadius <= 0.0 || src.type == SourceConfig::Type::Collimated) return c;
    Vec3 to = sceneCenter - origin;
    const double dist = to.length();
    // Inside the scene's own bounds there is no cone to aim into.
    if (dist <= sceneRadius * 1.000001 || !to.normalize()) return c;

    const double sinHalf = sceneRadius / dist;
    const double cosHalf = std::sqrt(std::max(0.0, 1.0 - sinHalf * sinHalf));
    const double omega   = kTwoPi * (1.0 - cosHalf);
    // Only worth it when the cone is a real narrowing; below that the weighting
    // costs more variance than the aiming saves. The factor of two also keeps
    // the per-ray weight at or below one, so the escaped share it implies is
    // never negative.
    if (omega > 0.5 * sourceSolidAngle(src)) return c;

    c.active  = true;
    c.axis    = to;
    c.cosHalf = cosHalf;
    c.omega   = omega;
    return c;
}

// The source's own angular probability density at `d`, per steradian.
inline double sourceAngularPdf(const SourceConfig& src, const Vec3& axis, const Vec3& d) {
    const double c = std::clamp(axis.dot(d), -1.0, 1.0);
    if (src.type == SourceConfig::Type::Lambertian) {
        const double th = std::min(90.0, std::max(0.0, src.halfAngleDeg)) * kDegRad;
        if (c <= std::cos(th) - 1e-12 || c <= 0.0) return 0.0;
        const double s = std::sin(th);
        return s > 0.0 ? c / (kPi * s * s) : 0.0;
    }
    const double th = std::min(180.0, std::max(0.0, src.halfAngleDeg)) * kDegRad;
    if (c < std::cos(th) - 1e-12) return 0.0;
    const double omega = kTwoPi * (1.0 - std::cos(th));
    return omega > 0.0 ? 1.0 / omega : 0.0;
}

// Uniform inside a cone of half-angle acos(cosHalf) about `axis`.
inline Vec3 sampleCone(const Vec3& axis, double cosHalf, double u1, double u2) {
    Vec3 t, b;
    orthonormalBasis(axis, t, b);
    const double cosT = 1.0 - u1 * (1.0 - cosHalf);
    const double sinT = std::sqrt(std::max(0.0, 1.0 - cosT * cosT));
    const double phi  = kTwoPi * u2;
    Vec3 d = t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) + axis * cosT;
    d.normalize();
    return d;
}

EmittedRay sampleEmission(const EmissionBasis& basis, const SourceConfig& src,
                          const SampledSpectrum& spec,
                          std::size_t index, std::uint64_t seed,
                          bool lowDiscrepancy = false,
                          const Vec3* sceneCenter = nullptr, double sceneRadius = 0.0,
                          std::size_t qmcIndex = 0, std::uint32_t qmcScramble = 0) {
    // The ray index is hashed into the stream state rather than used as a
    // stride. Striding by splitmix64's own increment would make each ray's
    // second draw identical to the next ray's first draw, correlating the
    // azimuth of every ray with the polar angle of the one after it.
    std::uint64_t state = mix64(seed ^ (std::uint64_t(index) * kEmissionSalt));
    // Within a replica the Sobol index counts from zero and the scramble is the
    // replica's own, so each replica is a complete low-discrepancy sequence
    // rather than a slice of one.
    const std::uint32_t sobolSeed = qmcScramble;
    const std::uint32_t sobolIdx  = std::uint32_t(qmcIndex);

    // Sobol where it is asked for, independent uniforms otherwise. Both are
    // functions of the ray index alone, so either way the run reproduces.
    auto draw = [&](int dim) {
        return lowDiscrepancy ? sampling::sobol(sobolIdx, dim, sobolSeed)
                              : uniform01(state);
    };
    const double u1 = draw(0);   // polar
    const double u2 = draw(1);   // azimuth
    const double u3 = draw(2);   // emitting area
    const double u4 = draw(3);

    EmittedRay ray;

    // ---- a measured ray set ------------------------------------------------
    // Everything the analytic emitter samples, the file already carries: the
    // origin on the emitting surface, the direction, and the flux. So this is
    // not another emitter shape -- it is the emitter, read rather than
    // constructed, and it is why reading one needed no change to the engine.
    if (src.tracesRayFile()) {
        const RayFileData& rf = *src.rayFile;
        const std::size_t n = rf.size();
        // Cycled by index, so the run stays bit-reproducible and a budget
        // larger than the file is a whole extra pass rather than a random
        // resample. Each ray's own flux becomes its weight, and the run
        // normalises by the emitted weight it actually accumulated, so an
        // uneven number of passes over the set is accounted for exactly.
        const SourceRay& fr = rf.rays[index % n];

        // The file's frame placed by the source's: +Z onto `axis`, and the
        // in-plane axes onto the same basis every other emitter uses.
        const double s = src.rayFileScale;
        const Vec3 p = basis.e1 * (fr.origin.x * s)
                     + basis.e2 * (fr.origin.y * s)
                     + basis.axis * (fr.origin.z * s);
        ray.origin = Vec3(src.origin) + p;
        ray.dir    = (basis.e1 * fr.dir.x + basis.e2 * fr.dir.y + basis.axis * fr.dir.z)
                         .normalized();

        const bool fromFile = src.rayFileWavelengths && fr.wavelengthNm > 0.0;
        ray.wavelengthNm = fromFile ? fr.wavelengthNm
                         : (rf.headerWavelengthNm > 0.0 && src.rayFileWavelengths)
                               ? rf.headerWavelengthNm
                               : (spec.monochromatic() || spec.rgbBands()
                                      ? spec.sample(index, 0.0)
                                      : spec.sample(index, draw(4)));

        const double mean = rf.meanPower();
        ray.weight = (mean > 0.0) ? fr.power / mean : 1.0;
        // A set already quoted in lumens is already photometric; weighting it
        // by V(lambda) a second time would count the eye twice. A set in watts
        // traced photometrically gets the weighting like any other source.
        if (src.fluxUnit == FluxUnit::Lumen && rf.unit != FluxUnit::Lumen)
            ray.weight *= spec.photometricWeight(ray.wavelengthNm);
        // Aiming is meaningless here: the directions are measured, not drawn,
        // so there is no distribution to narrow.
        return ray;
    }

    // The spectral draw comes last, so adding it left every existing stream --
    // and therefore every monochromatic result -- bit-identical.
    ray.wavelengthNm = spec.monochromatic() || spec.rgbBands()
                           ? spec.sample(index, 0.0)
                           : spec.sample(index, draw(4));
    // A photometric run weights each ray by how much the eye sees of it, so the
    // whole result reads in lumens, lux and candela without a second grid.
    ray.weight = (src.fluxUnit == FluxUnit::Lumen)
                     ? spec.photometricWeight(ray.wavelengthNm)
                     : 1.0;

    const Vec3 centre(src.origin);

    // Emission aiming: sample the direction inside the cone the scene occupies
    // and weight it by the share of the source's own distribution that lands
    // there. The rest of the distribution points at nothing, so its energy is
    // booked as escaped by the caller rather than traced into the void.
    auto aim = [&](const Vec3& origin, const Vec3& lawAxis) -> bool {
        if (!sceneCenter) return false;
        const AimingCone cone = aimingConeFor(origin, *sceneCenter, sceneRadius, src);
        if (!cone.active) return false;
        const Vec3 d = sampleCone(cone.axis, cone.cosHalf, u1, u2);
        const double pdf = sourceAngularPdf(src, lawAxis, d);
        ray.dir   = d;
        // f(d) * Omega_cone: one when the source is uniform and the cone is the
        // whole of it, zero for a direction the source does not emit into.
        ray.aimWeight = std::min(1.0, pdf * cone.omega);

        // A free companion draw from the source's own law, on its own two
        // dimensions so it is independent of the aimed one. It is never traced
        // -- a direction outside the cone provably hits nothing -- but it is
        // binned, so the far field still shows where the light that misses the
        // optic went.
        const Vec3 lawDir = sampleAngular(lawAxis, src.type, src.halfAngleDeg,
                                          draw(5), draw(6));
        ray.missDir = lawDir;
        ray.missed  = (lawDir.dot(cone.axis) < cone.cosHalf);
        return true;
    };

    if (src.shape == SourceConfig::Shape::Sphere && src.sizeA > 0.0) {
        // A real spherical emitter: pick a point on the surface, then emit about
        // the outward normal there. Etendue comes out right, which is what stops
        // a concentrator from reporting an impossible gain.
        const double cz  = 1.0 - 2.0 * u3;
        const double sz  = std::sqrt(std::max(0.0, 1.0 - cz * cz));
        const double phi = kTwoPi * u4;
        const Vec3 nrm(sz * std::cos(phi), sz * std::sin(phi), cz);
        ray.origin = centre + nrm * src.sizeA;
        if (!aim(ray.origin, nrm))
            ray.dir = sampleAngular(nrm, src.type, src.halfAngleDeg, u1, u2);
        return ray;
    }

    Vec3 offset(0, 0, 0);
    switch (src.shape) {
    case SourceConfig::Shape::Disc:
        if (src.sizeA > 0.0) {
            const double r   = src.sizeA * std::sqrt(u3);   // area-uniform
            const double phi = kTwoPi * u4;
            offset = basis.e1 * (r * std::cos(phi)) + basis.e2 * (r * std::sin(phi));
        }
        break;
    case SourceConfig::Shape::Rect:
        offset = basis.e1 * (src.sizeA * (u3 - 0.5)) + basis.e2 * (src.sizeB * (u4 - 0.5));
        break;
    default:
        break;
    }

    if (src.type == SourceConfig::Type::Collimated) {
        // A plane of parallel rays: the aperture is the beam itself, so the
        // angular draws are spent on the disc instead.
        const double r   = src.beamRadius * std::sqrt(u1);
        const double phi = kTwoPi * u2;
        offset += basis.e1 * (r * std::cos(phi)) + basis.e2 * (r * std::sin(phi));
        ray.origin = centre + offset;
        ray.dir    = basis.axis;
        return ray;
    }

    ray.origin = centre + offset;
    if (!aim(ray.origin, basis.axis))
        ray.dir = sampleAngular(basis.axis, src.type, src.halfAngleDeg, u1, u2);
    return ray;
}

// ---- accumulation ----------------------------------------------------------
// Nothing here is shared between threads, so no two workers ever touch the same
// cache line and no atomics are needed on the hot path.
//
// The scalar totals are accumulated PER CHUNK and reduced in chunk order, not
// per thread. Chunks are handed out dynamically, so a thread's share varies run
// to run -- summing per thread would make the totals depend on scheduling in
// the last few ULPs. Per chunk, the order is fixed, so the reported flux and
// efficiency are bit-identical whatever the thread count.
struct RayStats {
    Sum         fluxDetector;
    Sum         fluxAbsorbed;
    Sum         fluxBulk;
    Sum         fluxEscaped;
    // Refused by a receiver's acceptance cone. Not a loss to anything physical:
    // a measurement condition declined to record it, and folding that into
    // absorption is a wrong loss budget.
    Sum         fluxRejected;
    std::size_t raysRejected = 0;

    // Estimator bookkeeping residual: positive for energy a killed branch took
    // away, negative for energy handed to a survivor. Zero in expectation, and
    // what keeps the physical buckets closed exactly.
    //
    // Russian roulette, emission aiming and next-event estimation all book here,
    // for the same reason: each replaces a sampled quantity with an estimate of
    // it, so the two differ run to run and agree in expectation.
    //
    // One channel each rather than one accumulator between them: the sum's
    // expectation is zero even when one contributor is systematically wrong and
    // another cancels it, and the closed energy balance is the codebase's
    // strongest correctness argument. Split, each channel's mean has to lie
    // inside its own error bar, and a test asserts it. It costs five doubles.
    Sum         resRoulette;    // roulette kill / promotion
    Sum         resAiming;      // emission aimed away, less the companion draw
    Sum         resNee;         // the analytic direct term, entered negative
    Sum         resSkipDet;     // the sampled direct hit NEE already counted
    Sum         resBsdf;        // what a BSDF sampling weight created or destroyed

    // Why flux was truncated. At a depth limit of 160 a light guide or an
    // integrating sphere can put real flux here, and one number cannot say
    // whether raising the limit would help.
    Sum         truncDepth, truncStack, truncDegenerate, truncRefract, truncCutoff;
    std::size_t nTruncDepth = 0, nTruncStack = 0, nTruncDegenerate = 0,
                nTruncRefract = 0, nTruncCutoff = 0;

    // Medium-tracking anomalies, counted rather than swallowed.
    std::size_t medUnmatchedExit = 0, medStackOverflow = 0, medGuessedIndex = 0;
    // Total emitted weight. For a radiometric run every ray weighs one and this
    // is just the ray count; for a photometric one it is what the result is
    // normalised by, so the requested lumens come out exactly.
    Sum         weightEmitted;
    // Per-ray detector energy, as a running mean and variance.
    Moments     det;
    std::size_t raysHit       = 0;
    std::size_t raysDone      = 0;

    void add(const RayStats& o) {
        fluxDetector .merge(o.fluxDetector);
        fluxAbsorbed .merge(o.fluxAbsorbed);
        fluxBulk     .merge(o.fluxBulk);
        fluxEscaped  .merge(o.fluxEscaped);
        fluxRejected .merge(o.fluxRejected);
        resRoulette  .merge(o.resRoulette);
        resAiming    .merge(o.resAiming);
        resNee       .merge(o.resNee);
        resSkipDet   .merge(o.resSkipDet);
        resBsdf      .merge(o.resBsdf);
        truncDepth     .merge(o.truncDepth);
        truncStack     .merge(o.truncStack);
        truncDegenerate.merge(o.truncDegenerate);
        truncRefract   .merge(o.truncRefract);
        truncCutoff    .merge(o.truncCutoff);
        weightEmitted.merge(o.weightEmitted);
        det          .merge(o.det);
        raysHit      += o.raysHit;
        raysDone     += o.raysDone;
        raysRejected += o.raysRejected;
        nTruncDepth      += o.nTruncDepth;
        nTruncStack      += o.nTruncStack;
        nTruncDegenerate += o.nTruncDegenerate;
        nTruncRefract    += o.nTruncRefract;
        nTruncCutoff     += o.nTruncCutoff;
        medUnmatchedExit += o.medUnmatchedExit;
        medStackOverflow += o.medStackOverflow;
        medGuessedIndex  += o.medGuessedIndex;
    }

    // The five estimator channels summed: what used to be `fluxRoulette`, and
    // what still closes the energy balance.
    double residualTotal() const {
        return resRoulette.value() + resAiming.value() + resNee.value()
             + resSkipDet.value() + resBsdf.value();
    }
    double truncatedTotal() const {
        return truncDepth.value() + truncStack.value() + truncDegenerate.value()
             + truncRefract.value() + truncCutoff.value();
    }
};

// Diagram segments and receiver arrivals are collected per chunk so they can be
// concatenated in ray order afterwards, whichever thread ran the chunk.
// How many contributors a recorded route can hold. A route longer than this is
// kept as its head and flagged, which is the honest degradation: a
// forty-bounce path is not a stray-light route anybody acts on, and pretending
// to hold it would cost every branch the memory.
constexpr int kStrayDepth = 12;

struct StraySig {
    std::int16_t e[kStrayDepth] = {};
    std::uint8_t n     = 0;
    bool         trunc = false;

    // Consecutive hits on one contributor are one step: that is what makes a
    // surface set collapse a guide wall to a single entry.
    void append(int id) {
        if (n > 0 && e[n - 1] == std::int16_t(id)) return;
        if (n >= kStrayDepth) { trunc = true; return; }
        e[n++] = std::int16_t(id);
    }
    bool sameAs(const StraySig& o) const {
        if (n != o.n || trunc != o.trunc) return false;
        for (std::uint8_t i = 0; i < n; ++i)
            if (e[i] != o.e[i]) return false;
        return true;
    }
    std::uint64_t hash() const {
        std::uint64_t h = 0xcbf29ce484222325ull ^ (trunc ? 0x9e3779b97f4a7c15ull : 0ull);
        for (std::uint8_t i = 0; i < n; ++i) {
            h ^= std::uint64_t(std::uint16_t(e[i]));
            h *= 0x100000001b3ull;
        }
        return h;
    }
};

struct ChunkOutput {
    std::vector<RaySegment>      segments;
    // Parent of each segment within this chunk (-1 for the emitted leg). A path
    // is a tree, so marking the legs that reached the receiver means walking
    // back up from the arrival rather than knowing it on the way down.
    std::vector<int>             parent;

    // Arrivals are a reservoir, not the first N seen. Keeping the first N let a
    // single trapped ray that arrives fifty times fill the chunk quota and
    // censor every later arrival in it -- which skewed the focus study and the
    // spot metrics toward multi-arrival paths, worst on exactly the scenes
    // (guides, spheres) where paths are longest. The reservoir gives every
    // arrival the same chance of being kept, and its RNG is seeded from the
    // chunk index so the choice is still bit-reproducible.
    std::vector<DetectorArrival> arrivals;
    std::uint64_t                arrivalRng  = 0;
    std::size_t                  arrivalSeen = 0;

    // Flux and arrival count per receiver. Scalars stay per chunk so they reduce
    // in a fixed order however the chunks were scheduled.
    std::vector<double>          detFlux;
    std::vector<std::size_t>     detHits;

    // Detected and emitted weight per Owen-scramble replica, for the
    // randomised-QMC error bar.
    std::vector<double>          replicaDet;
    std::vector<double>          replicaEmit;

    // Detected flux per source. Scalars, so they reduce in chunk order like
    // every other scalar and a two-source run stays bit-reproducible.
    std::vector<double>          sourceDet;

    // Stray-light routes seen in this chunk. Held as a vector in first-seen
    // order with a hash index beside it, rather than as a map: the vector's
    // order is what the chunk-order merge needs to be reproducible, and a hash
    // container's iteration order is not something to build that on.
    std::vector<StraySig>        pathSig;
    std::vector<double>          pathFlux;
    std::vector<std::size_t>     pathRays;
    std::unordered_map<std::uint64_t, std::vector<int>> pathIndex;
    std::size_t                  pathsDropped = 0;
    double                       pathFluxDropped = 0.0;

    // Books `energy` against `sig`, or against the dropped bucket once the
    // table is full.
    void addPath(const StraySig& sig, double energy, std::size_t maxPaths) {
        auto& bucket = pathIndex[sig.hash()];
        for (int i : bucket) {
            if (!pathSig[std::size_t(i)].sameAs(sig)) continue;
            pathFlux[std::size_t(i)] += energy;
            ++pathRays[std::size_t(i)];
            return;
        }
        if (pathSig.size() >= maxPaths) {
            ++pathsDropped;
            pathFluxDropped += energy;
            return;
        }
        bucket.push_back(int(pathSig.size()));
        pathSig.push_back(sig);
        pathFlux.push_back(energy);
        pathRays.push_back(1);
    }
};

// One branch of a path still waiting to be traced.
struct PathState {
    Vec3   o, d;
    double energy = 0.0;
    // Optical path length so far: sum of n * distance over every leg travelled.
    double opl    = 0.0;
    // Every medium the branch is inside, and the one that wins on priority --
    // cached because it is read on every leg and the stack itself is not.
    MediumStack media;
    int    medium = -1;   // effective medium surface index, -1 for vacuum
    int    depth  = 0;
    int    seg    = -1;   // segment index this branch grew from, -1 if unrecorded
    // This branch was spawned from a diffuse event whose direct contribution to
    // the receiver has already been estimated analytically. Its *first* hit on
    // the receiver is therefore that same contribution and must not be counted
    // twice; anything past that first hit is a different path and does count.
    bool   skipDetector = false;

    // Polarisation, carried only on a polarised trace. The Stokes vector's own
    // intensity is normalised to one; `energy` remains the single place a
    // branch's flux lives, so the unpolarised path is unchanged.
    polarisation::Stokes stokes;
    // Surface normal of the last interaction, so the next one knows how far the
    // plane of incidence has turned. A Stokes vector only means anything
    // relative to a frame.
    Vec3   polFrame;

    // The route this branch has taken. Written only while stray-light paths are
    // being recorded; otherwise it is 26 bytes that never move.
    StraySig stray;
};

// Everything the tracer needs that does not change from ray to ray. This
// replaces the fifteen-parameter recursive signature the tracer used to carry.
// A metasurface interaction: which diffraction order this ray leaves in, and
// what the ones it did not leave in were carrying.
//
// The generalised Snell law replaces the outgoing *direction*; it says nothing
// about how much energy goes each way, which is what the efficiency table is
// for. So the two are layered rather than mixed: the Fresnel term still decides
// what reflects at the interface, and the table then decides how the
// transmitted share is split between the orders. Whatever the orders do not
// take between them -- because the device does not diffract it, or because the
// order is evanescent at this angle and has no propagating direction at all --
// is absorbed at the surface and booked there. That is the difference between a
// budget that closes and one that is quietly topped up.
//
// Returns false when nothing propagates, in which case the caller books the
// whole branch as absorbed.
struct MetaPick {
    Vec3   direction;
    double efficiency = 0.0;   // what the orders took between them
    int    order = 0;
    // The state the diffracted branch leaves in, when the device is
    // polarising and the run is carrying polarisation.
    polarisation::Stokes state;
    bool   polarised = false;
};

bool sampleMetaOrder(const meta::Metasurface& ms, const Vec3& d, const Vec3& n,
                     const Vec3& p, double lambda, double n1, double n2,
                     std::uint64_t& rng, const Vec3& ordinary,
                     const polarisation::Stokes* inState, MetaPick& out) {
    const Vec3 grad = ms.gradientAt(p, n);

    double weight[meta::kOrderSlots] = {};
    Vec3   dir[meta::kOrderSlots];
    polarisation::Stokes state[meta::kOrderSlots];
    double total = 0.0;
    const bool flat = (grad.lengthSquared() <= 0.0);

    for (int m = -meta::kMaxOrder; m <= meta::kMaxOrder; ++m) {
        double es = 0.0, ep = 0.0;
        ms.efficiency.at(m, lambda, es, ep);
        // A polarising device diffracts s and p by different amounts, and on
        // a polarised run that split is decided by the *state* rather than by
        // the unpolarised average of the two. It is a diattenuator, so it is
        // the same Mueller machinery the Fresnel term uses -- the amplitudes
        // being the square roots of the efficiencies, and no retardance
        // between them, because an efficiency table carries moduli and the
        // phase would have to come from the EM solve that produced it.
        double e = 0.5 * (es + ep);
        if (inState && ms.efficiency.polarising) {
            const polarisation::Stokes o =
                polarisation::Mueller::fromFresnel(std::sqrt(std::max(0.0, es)),
                                                   std::sqrt(std::max(0.0, ep)),
                                                   0.0).apply(*inState);
            e = std::clamp(o.i / std::max(1e-18, inState->i), 0.0, 1.0);
            state[m + meta::kMaxOrder] =
                o.i > 1e-15 ? o.scaled(1.0 / o.i)
                            : polarisation::Stokes::unpolarised();
        }
        if (e <= 0.0) continue;
        const int slot = m + meta::kMaxOrder;
        if (flat) {
            // No gradient is no deflection, for every order at once. Keeping the
            // direction the ordinary law already produced is not an optimisation
            // -- it is what makes a metasurface with its gradient switched off
            // trace *bit-identically* to the glass it replaced, which is the
            // test that keeps this feature from perturbing the other scenes.
            dir[slot] = ordinary;
        } else if (!meta::deflect(d, n, grad, lambda, n1, n2, m, ms.reflective,
                                  dir[slot])) {
            continue;                        // evanescent here: no such order
        }
        weight[slot] = e;
        total += e;
    }
    if (!(total > 0.0)) return false;

    // One order, drawn in proportion to what it carries. The branch keeps the
    // whole transmitted share scaled by the total efficiency rather than by its
    // own order's, which is the standard unbiased estimator of following all of
    // them -- the same arrangement the refractive split already uses.
    double pick = uniform01(rng) * total, acc = 0.0;
    int chosen = -1;
    for (int slot = 0; slot < meta::kOrderSlots; ++slot) {
        if (weight[slot] <= 0.0) continue;
        acc += weight[slot];
        chosen = slot;
        if (pick < acc) break;
    }
    if (chosen < 0) return false;

    out.direction  = dir[chosen];
    out.efficiency = std::min(1.0, total);
    out.order      = chosen - meta::kMaxOrder;
    out.polarised  = (inState != nullptr) && ms.efficiency.polarising;
    out.state      = state[chosen];
    return true;
}

// Which refractive solids contain `p`.
//
// A ray has always been born in vacuum, which is right for every source the
// registry places -- they all sit in air in front of the optic -- and wrong for
// the one arrangement an LED is actually built as: a die immersed in its own
// encapsulant. Such a ray met the dome from the inside while recorded as being
// in air, so the crossing was read as an *entry* into glass, the incident index
// was guessed at 1, and no ray could ever reach the critical angle. The answer
// looked plausible and was arithmetic.
//
// Parity along one ray settles it: a closed solid is crossed an odd number of
// times by any ray leaving a point inside it, and an even number from outside.
// The direction is arbitrary but must not be axis-aligned -- half the geometry
// in this library has faces normal to an axis, and a probe that grazes one
// counts a crossing it should not.
//
// Cost is one walk per source per run, not per ray. An emitter is a point or a
// small patch, and the containment of its centre is the containment of all of
// it for every arrangement that is not a die half-buried in its own dome.
MediumStack mediaContaining(const TraceScene& scene,
                            const std::vector<SceneSurface>& surfs,
                            const Vec3& p) {
    MediumStack out;

    Vec3 d(0.21384, 0.34712, 0.91283);
    if (!d.normalize()) return out;

    // Crossings per surface, and how far away the nearest one was.
    std::vector<int>    crossings(surfs.size(), 0);
    std::vector<double> nearest(surfs.size(), 0.0);

    constexpr int kMaxCrossings = 256;
    double t = 1e-6;
    for (int guard = 0; guard < kMaxCrossings; ++guard) {
        RayHit h;
        if (!scene.nearestHit(p, d, h, t)) break;
        const int idx = (h.inst < 0) ? scene.triangles()[std::size_t(h.tri)].surf
                                     : scene.surfaceIndexOf(h);
        if (idx >= 0 && idx < int(surfs.size()) && surfs[std::size_t(idx)].index > 0.0) {
            if (crossings[std::size_t(idx)]++ == 0) nearest[std::size_t(idx)] = h.t;
        }
        // Past this hit, by enough that the same triangle is not found again.
        t = h.t + 1e-6;
    }

    // Outermost first, innermost last: the stack resolves a tie by taking the
    // last entry, and the solid whose boundary is nearest along the way out is
    // the one the point is deepest inside.
    std::vector<int> inside;
    for (std::size_t i = 0; i < crossings.size(); ++i)
        if (crossings[i] % 2 == 1) inside.push_back(int(i));
    std::sort(inside.begin(), inside.end(), [&](int a, int b) {
        return nearest[std::size_t(a)] > nearest[std::size_t(b)];
    });
    for (int idx : inside) out.push(idx);
    return out;
}

struct TraceContext {
    const TraceScene*    scene      = nullptr;
    // The surfaces this run sees: the scene's own, or a copy with the caller's
    // edits applied. Never the scene's vector directly, so a run can differ from
    // the geometry it shares without touching it.
    const std::vector<SceneSurface>* surfs = nullptr;
    // Every receiver, and the flat grid holding all of them end to end. A scene
    // with two receivers used to bin the second one's arrivals against the
    // first one's frame; each now has its own block of cells.
    const std::vector<DetectorInfo>* dets = nullptr;
    std::vector<double>* grid       = nullptr;  // per-thread bins, all receivers
    // Large receivers: ONE shared atomic accumulation grid. Either this or
    // `grid` is used, never both -- the atomic path routes the same cell adds
    // here, so a large grid is a single copy no matter how many cores trace it.
    std::vector<std::atomic<double>>* atomicGrid = nullptr;
    // Per-thread sum of squared deposits, parallel to `grid`. Null when the
    // caller did not ask for a noise map.
    std::vector<double>* varGrid    = nullptr;
    std::vector<double>* bandGrid   = nullptr;  // per-thread 3 x bins of receiver 0
    std::vector<double>* intensity  = nullptr;  // per-thread far-field bins
    std::vector<double>* intensityVar = nullptr; // squared deposits, or null
    RayStats*            stats      = nullptr;  // per-chunk scalar totals
    ChunkOutput*         chunk      = nullptr;
    // Where to record what happened at each interaction, or null. Null on every
    // Monte Carlo path, so the cost there is one predictable branch.
    std::vector<RayInteraction>* log = nullptr;
    std::size_t          segmentCap = 0;
    std::size_t          arrivalCap = 0;
    std::size_t          bandCells  = 0;        // cells of receiver 0
    PhysicsOptions       physics;
    EstimatorOptions     estimator;
    int                  nTheta = 0, nPhi = 0;
    // Per-ray state.
    std::uint64_t rng    = 0;
    double        lambda = 587.6;
    // How this ray's arrivals paint into the three colour bands. They sum to
    // one, so the bands still add up to the irradiance grid exactly.
    double        bandW[3] = {0.0, 0.0, 0.0};
    bool          bands  = false;
    int           replica = -1;   // which Owen scramble this ray belongs to
    int           source  = 0;    // which source emitted this ray
    // Stray-light path recording, or null. Null on every run that did not ask
    // for it, which is the branch the hot loop pays.
    const StrayPathOptions* stray = nullptr;
    // The media the emitting source sits inside, and which of them governs.
    // Empty and -1 for a source in air, which is every source the registry
    // places; a source inside a solid -- an LED die in its own encapsulant --
    // starts its rays already in that solid rather than in vacuum.
    MediumStack   initialMedia;
    int           initialMedium = -1;
    // Flux-weighted Stokes sum of what reached the receiver, on a polarised run.
    double        detStokes[4] = {0.0, 0.0, 0.0, 0.0};
    // The state the source emits, normalised to unit intensity.
    polarisation::Stokes emitted;
};

// Relaxed atomic accumulation into a shared receiver bin. std::atomic has no
// fetch_add for floating point, so it is a compare-exchange loop; the addition
// order across threads is non-deterministic, but each single add is atomic and
// the sums agree with the deterministic path well inside tolerance.
inline void atomicAddRelaxed(std::atomic<double>& cell, double v) {
    double cur = cell.load(std::memory_order_relaxed);
    for (;;) {
        const double next = cur + v;
        if (cell.compare_exchange_weak(cur, next, std::memory_order_relaxed)) return;
    }
}

// Bins a direction leaving the system into the far-field intensity grid.
inline void binDirection(TraceContext& ctx, const Vec3& d, double energy) {
    if (!ctx.intensity || ctx.nTheta <= 0) return;
    const double cz    = std::clamp(d.z, -1.0, 1.0);
    const double theta = std::acos(cz);                 // 0 .. pi
    double phi = std::atan2(d.y, d.x);                  // -pi .. pi
    if (phi < 0.0) phi += kTwoPi;
    int it = int(theta / kPi * double(ctx.nTheta));
    int ip = int(phi / kTwoPi * double(ctx.nPhi));
    it = std::clamp(it, 0, ctx.nTheta - 1);
    ip = std::clamp(ip, 0, ctx.nPhi - 1);
    const std::size_t k = std::size_t(it) * std::size_t(ctx.nPhi) + std::size_t(ip);
    (*ctx.intensity)[k] += energy;
    if (ctx.intensityVar) (*ctx.intensityVar)[k] += energy * energy;
}

// Refractive index of the medium a branch is travelling through, at this
// wavelength.
inline double mediumIndex(const TraceContext& ctx, int medium) {
    if (medium < 0) return 1.0;
    const SceneSurface& m = (*ctx.surfs)[std::size_t(medium)];
    if (m.index <= 0.0) return 1.0;
    return m.indexAt(ctx.lambda, ctx.physics.dispersion);
}

// Which of the media a branch is inside actually governs it. Where two solids
// overlap, the higher `mediumPriority` wins; ties go to the innermost entry,
// which is the ordinary nesting case. -1 is vacuum.
inline int effectiveMedium(const TraceContext& ctx, const MediumStack& st) {
    int best = -1, bestPrio = 0;
    for (std::uint8_t i = 0; i < st.n; ++i) {
        const int idx = int(st.e[i]);
        const int prio = (*ctx.surfs)[std::size_t(idx)].mediumPriority;
        if (best < 0 || prio >= bestPrio) { best = idx; bestPrio = prio; }
    }
    return best;
}

// Connects a diffuse bounce at `p` to every receiver analytically: pick a point
// on each, test that nothing is in the way, and add the flux the cosine lobe
// puts through that solid angle.
//
// A random walk that has to *find* a receiver spends most of its rays missing;
// on an integrating sphere or behind a diffuser that is where nearly all of the
// variance lives, and connecting to it directly removes the search entirely.
//
// Returns the energy delivered, or -1 when the estimate was declined -- which
// happens where a receiver sits close enough to subtend more than the lobe can
// deliver into, and one area sample would carry more energy than the branch
// has. The caller must only suppress the sampled path's own direct hit when
// this returned something other than -1.
//
// The decision to decline is taken from the geometry of the bounce alone,
// before any point on the receiver is drawn. Deciding it from the drawn point
// instead would make the choice depend on the sample: the connections that
// survived would be the weak ones, the strong ones would fall back to path
// sampling, and the two halves would no longer add up to the integral. That is
// a bias, and on a transmissive diffuser it is a large one.
constexpr int kMaxNeeDetectors = 8;

double nextEventEstimate(TraceContext& ctx, const StraySig& route, const Vec3& p, const Vec3& n, double e,
                         double opl, int medium) {
    if (!ctx.estimator.nextEventEstimation || !ctx.dets || ctx.dets->empty() || e <= 0.0)
        return -1.0;
    const TraceScene& scene = *ctx.scene;
    const std::size_t nDet = std::min<std::size_t>(ctx.dets->size(), kMaxNeeDetectors);

    struct Connection {
        bool   hit = false;
        Vec3   q, w;
        double energy = 0.0;
        double range  = 0.0;
    };
    Connection conn[kMaxNeeDetectors];

    // Is a one-sample connection well conditioned here? The largest weight any
    // point on a receiver could produce is bounded by its area over pi times the
    // square of its nearest approach, with both cosines at one. If that bound
    // exceeds the branch's own energy for any receiver, decline the whole event
    // and let path sampling carry it.
    double bound = 0.0;
    for (std::size_t di = 0; di < nDet; ++di) {
        const DetectorInfo& det = (*ctx.dets)[di];
        if (!det.valid || det.w <= 0.0 || det.h <= 0.0) continue;
        const Vec3 rel = p - det.center;
        const double a = std::clamp(rel.dot(det.u), -0.5 * det.w, 0.5 * det.w);
        const double b = std::clamp(rel.dot(det.v), -0.5 * det.h, 0.5 * det.h);
        const Vec3 nearest = det.center + det.u * a + det.v * b;
        const double rMin2 = (p - nearest).lengthSquared();
        if (rMin2 < 1e-12) return -1.0;
        bound += det.w * det.h / (kPi * rMin2);
        if (bound > 1.0) return -1.0;
    }
    if (bound <= 0.0) return -1.0;

    // First pass: everything is computed before anything is committed, so the
    // grid never sees a half-finished event.
    for (std::size_t di = 0; di < nDet; ++di) {
        const DetectorInfo& det = (*ctx.dets)[di];
        if (!det.valid || det.w <= 0.0 || det.h <= 0.0) continue;

        // Uniform on the receiver rectangle.
        const double su = (uniform01(ctx.rng) - 0.5) * det.w;
        const double sv = (uniform01(ctx.rng) - 0.5) * det.h;
        const Vec3 q = det.center + det.u * su + det.v * sv;

        Vec3 w = q - p;
        const double r2 = w.lengthSquared();
        if (r2 < 1e-18 || !w.normalize()) continue;

        const double cosSurf = w.dot(n);
        if (cosSurf <= 1e-9) continue;                 // behind the scattering surface
        const double cosDet = std::fabs(w.dot(det.n));
        if (cosDet <= 1e-9) continue;                  // edge on: no projected area
        if (!det.accepts(w)) continue;                 // outside the acceptance cone

        // The cosine lobe is cos/pi per steradian; the sampled point stands for
        // a solid angle of cosDet * area / r^2. The bound above already
        // guarantees this stays inside the branch's energy, so nothing here
        // depends on the value it takes.
        const double factor = (cosSurf / kPi) * (cosDet * det.w * det.h / r2);
        if (!(factor > 0.0)) continue;

        // Is the receiver actually the first thing along the connection? A pane
        // of glass in the way means the direct term really is zero -- the
        // sampled path would hit the glass first too, and is not suppressed.
        //
        // An any-hit query, not a nearest-hit one. The question is whether
        // anything is in the way, and a single occluder anywhere along the
        // segment answers it: walking the hierarchy to completion to find the
        // *nearest* occluder is work spent learning something the first one
        // already said. On an integrating sphere or behind a diffuser -- the
        // scenes next-event estimation exists for -- a connection is fired at
        // nearly every bounce, so this is a large share of the whole trace.
        const double r = std::sqrt(r2);
        const Vec3   o = p + n * surfaceEps(p);
        // Just short of the receiver, so its own far face cannot occlude the
        // connection to its near one.
        if (scene.occluded(o, w, r * (1.0 - 1e-9), det.surf, surfaceEps(o))) continue;

        conn[di].hit    = true;
        conn[di].q      = q;
        conn[di].w      = w;
        conn[di].energy = e * factor;
        conn[di].range  = r;
    }

    // Second pass: commit.
    double delivered = 0.0;
    for (std::size_t di = 0; di < nDet; ++di) {
        if (!conn[di].hit) continue;
        const DetectorInfo& det = (*ctx.dets)[di];
        double contribution = conn[di].energy;

        // Bulk loss along the connection, in the medium the bounce is in.
        double travelIndex = 1.0;
        if (medium >= 0) {
            const SceneSurface& m = (*ctx.surfs)[std::size_t(medium)];
            travelIndex = m.indexAt(ctx.lambda, ctx.physics.dispersion);
            if (ctx.physics.absorption) {
                const double alpha = m.absorption * ctx.physics.absorptionScale;
                if (alpha > 0.0) {
                    const double survived = contribution * std::exp(-alpha * conn[di].range);
                    ctx.stats->fluxAbsorbed.add(contribution - survived);
                    ctx.stats->fluxBulk.add(contribution - survived);
                    contribution = survived;
                }
            }
        }
        if (contribution <= 0.0) continue;

        int bx = 0, by = 0;
        // The sampled point is on the receiver by construction; bin that.
        if (det.binOf(conn[di].q, bx, by)) {
            const std::size_t cell = scene.detectorOffsets()[di] +
                                     std::size_t(by) * std::size_t(det.nx) + std::size_t(bx);
            // One shared atomic grid above the size threshold, per-thread below.
            if (ctx.atomicGrid) atomicAddRelaxed((*ctx.atomicGrid)[cell], contribution);
            else                (*ctx.grid)[cell] += contribution;
            if (ctx.varGrid) (*ctx.varGrid)[cell] += contribution * contribution;
            if (ctx.bandGrid && ctx.bands && di == 0)
                for (int b = 0; b < 3; ++b)
                    (*ctx.bandGrid)[std::size_t(b) * ctx.bandCells + cell] +=
                        contribution * ctx.bandW[b];
        }
        ctx.stats->fluxDetector.add(contribution);
        // A connected contribution took the same route as the branch it left,
        // with the receiver as its last step -- otherwise the ranked list would
        // be missing exactly the light that next-event estimation is best at
        // finding, which is the diffuse stray light it exists to measure.
        if (ctx.stray && ctx.chunk) {
            StraySig r = route;
            const int set = ctx.stray->setOf(det.surf);
            r.append(set >= 0 ? -(set + 1) : det.surf);
            ctx.chunk->addPath(r, contribution, ctx.stray->maxPaths);
        }
        ++ctx.stats->raysHit;
        if (ctx.chunk && di < ctx.chunk->detFlux.size()) {
            ctx.chunk->detFlux[di] += contribution;
            ++ctx.chunk->detHits[di];
        }
        binDirection(ctx, conn[di].w, contribution);

        if (ctx.chunk && ctx.arrivalCap > 0) {
            DetectorArrival a;
            a.p            = conn[di].q;
            a.d            = conn[di].w;
            a.energy       = float(contribution);
            a.wavelengthNm = float(ctx.lambda);
            a.opl          = float(opl + travelIndex * conn[di].range);
            a.detector     = int(di);
            a.source       = ctx.source;
            ChunkOutput& co = *ctx.chunk;
            ++co.arrivalSeen;
            if (co.arrivals.size() < ctx.arrivalCap) {
                co.arrivals.push_back(a);
            } else {
                const std::size_t j =
                    std::size_t(uniform01(co.arrivalRng) * double(co.arrivalSeen));
                if (j < ctx.arrivalCap) co.arrivals[j] = a;
            }
        }
        delivered += contribution;
    }
    return delivered;
}

void tracePath(TraceContext& ctx, const Vec3& origin, const Vec3& dir, double energy) {
    const TraceScene&     scene = *ctx.scene;
    RayStats&             ts    = *ctx.stats;
    std::vector<double>*  grid  = ctx.grid;  // null on the atomic-receiver path
    const PhysicsOptions& phys  = ctx.physics;

    double rayDetector = 0.0;   // this ray's total contribution to the receiver

    // Explicit stack instead of recursion: a refractive hit can spawn both a
    // reflected and a transmitted branch, and a light guide can reach the depth
    // limit, so the recursion would run 160 frames deep per branch.
    PathState stack[kPathStack];
    int sp = 0;
    stack[sp] = PathState{};
    stack[sp].o      = origin;
    stack[sp].d      = dir;
    stack[sp].energy = energy;
    stack[sp].stokes = ctx.emitted;
    // Born inside something, or in air. Everything downstream -- the Fresnel
    // pair, Beer-Lambert, the critical angle -- reads the stack, so seeding it
    // here is the whole of what "immersed" means to the tracer.
    stack[sp].media  = ctx.initialMedia;
    stack[sp].medium = ctx.initialMedium;
    ++sp;

    // Pushes a branch, or books its energy as truncated if it cannot be taken.
    auto push = [&](const PathState& st) {
        if (sp < kPathStack) { stack[sp++] = st; return; }
        ts.truncStack.add(st.energy);
        ++ts.nTruncStack;
    };

    while (sp > 0) {
        PathState s = stack[--sp];

        if (s.depth > kMaxDepth) {
            ts.truncDepth.add(s.energy);
            ++ts.nTruncDepth;
            continue;
        }
        if (s.energy <= 0.0) continue;
        // With the estimator switches off there is no roulette to thin the
        // tree, so the old hard cutoff is the only thing that ends a path.
        if (!phys.varianceReduction && s.energy < kRoulette) {
            ts.truncCutoff.add(s.energy);
            ++ts.nTruncCutoff;
            continue;
        }

        // Russian roulette rather than a hard cutoff. A survivor is promoted to
        // exactly kRoulette, so its energy can never explode, and the residual
        // is booked so the buckets still close to the last bit.
        if (phys.varianceReduction && s.energy < kRoulette) {
            const double p = s.energy / kRoulette;
            if (uniform01(ctx.rng) >= p) {
                ts.resRoulette.add(s.energy);
                continue;
            }
            ts.resRoulette.add(-(kRoulette - s.energy));
            s.energy = kRoulette;
        }

        RayHit h;
        if (!scene.nearestHit(s.o, s.d, h, surfaceEps(s.o))) {
            ts.fluxEscaped.add(s.energy);
            binDirection(ctx, s.d, s.energy);
            continue;
        }

        // Resolved through the scene: an instanced hit is a triangle of a part
        // placed somewhere, and does not appear in the scene-wide list at all.
        // One branch, not three: a light guide takes a hundred hits per ray.
        int  surfIdx;
        Vec3 triN;
        if (h.inst < 0) {
            const SceneTri& t = scene.triangles()[std::size_t(h.tri)];
            surfIdx = t.surf;
            triN    = t.n;
        } else {
            surfIdx = scene.surfaceIndexOf(h);
            triN    = scene.geometricNormal(h);
        }
        const SceneSurface& surf = (*ctx.surfs)[std::size_t(surfIdx)];
        const Vec3 p = s.o + s.d * h.t;

        const double nTravel = mediumIndex(ctx, s.medium);

        // Scattering inside the medium, before the boundary is reached. Every
        // white diffusing plastic in every luminaire is a volume scatterer, and
        // a surface model has no way to say so.
        if (phys.volumeScattering && s.medium >= 0) {
            const bsdf::Volume& vol = (*ctx.surfs)[std::size_t(s.medium)].volume;
            if (vol.active()) {
                const double free = vol.sampleDistance(ctx.rng);
                if (free < h.t) {
                    // Attenuate over the leg actually travelled, then turn.
                    double e = s.energy;
                    if (phys.absorption) {
                        const double alpha = (*ctx.surfs)[std::size_t(s.medium)].absorption
                                             * phys.absorptionScale;
                        if (alpha > 0.0) {
                            const double survived = e * std::exp(-alpha * free);
                            ts.fluxAbsorbed.add(e - survived);
                            ts.fluxBulk.add(e - survived);
                            e = survived;
                        }
                    }
                    const Vec3 p0 = s.o + s.d * free;
                    PathState st = s;
                    st.o      = p0;
                    st.d      = vol.scatter(s.d, ctx.rng);
                    st.energy = e;
                    st.opl    = s.opl + nTravel * free;
                    st.depth  = s.depth + 1;
                    st.skipDetector = false;
                    if (ctx.chunk && ctx.chunk->segments.size() < ctx.segmentCap) {
                        RaySegment rs;
                        rs.a = s.o;
                        rs.b = p0;
                        rs.energy = float(e);
                        rs.wavelengthNm = float(ctx.lambda);
                        rs.depth = std::uint16_t(std::min(s.depth, 65535));
                        rs.source = std::uint16_t(std::max(0, ctx.source));
                        st.seg = int(ctx.chunk->segments.size());
                        ctx.chunk->segments.push_back(rs);
                        ctx.chunk->parent.push_back(s.seg);
                    }
                    push(st);
                    continue;
                }
            }
        }

        const double opl     = s.opl + nTravel * h.t;

        // Beer-Lambert bulk attenuation over the leg just travelled. This is the
        // loss that scales with path length rather than with hit count, so it is
        // what makes a long light guide cost more than a short one.
        double e = s.energy;
        if (phys.absorption && s.medium >= 0) {
            const double alpha = (*ctx.surfs)[std::size_t(s.medium)].absorption
                                 * phys.absorptionScale;
            if (alpha > 0.0) {
                const double survived = e * std::exp(-alpha * h.t);
                const double lost     = e - survived;
                ts.fluxAbsorbed.add(lost);
                ts.fluxBulk.add(lost);
                e = survived;
            }
        }

        int segIdx = -1;
        if (ctx.chunk && ctx.chunk->segments.size() < ctx.segmentCap) {
            segIdx = int(ctx.chunk->segments.size());
            RaySegment rs;
            rs.a            = s.o;
            rs.b            = p;
            rs.energy       = float(e);
            rs.wavelengthNm = float(ctx.lambda);
            rs.depth        = std::uint16_t(std::min(s.depth, 65535));
            rs.source       = std::uint16_t(std::max(0, ctx.source));
            ctx.chunk->segments.push_back(rs);
            ctx.chunk->parent.push_back(s.seg);
        }

        // The route, one step per contributor met. Placed after the volume
        // block, so a scatter inside a medium does not record the surface the
        // branch was merely heading towards, and before the receiver branch, so
        // the receiver is the route's last step.
        if (ctx.stray) {
            const int set = ctx.stray->setOf(surfIdx);
            s.stray.append(set >= 0 ? -(set + 1) : surfIdx);
        }

        if (surf.isDetector) {
            const int di = scene.detectorOfSurface(surfIdx);
            const DetectorInfo& det =
                (di >= 0) ? (*ctx.dets)[std::size_t(di)] : (*ctx.dets)[0];

            // A receiver only counts what arrives inside its acceptance cone,
            // which is how a real photometer and a real fibre behave.
            // Refused by the acceptance cone. That light was not absorbed by
            // anything -- it was refused by a measurement condition -- so it
            // gets its own channel, and whether the branch stops here is the
            // receiver's to say. An absorbing photometer head stops it; a
            // recording plane, a photometer behind a window, lets it through.
            if (!det.accepts(s.d)) {
                if (!det.rejectPasses) {
                    ts.fluxRejected.add(e);
                    ++ts.raysRejected;
                    continue;
                }
                PathState st = s;
                st.o      = p + s.d * surfaceEps(p);
                st.energy = e;
                st.opl    = opl;
                st.depth  = s.depth + 1;
                st.seg    = segIdx;
                push(st);
                continue;
            }
            // The diffuse bounce this branch came from already estimated its
            // direct contribution analytically. Counting this arrival too would
            // count that path twice, so its energy goes to the estimator's
            // residual -- the same place the roulette's does, and zero in
            // expectation for the same reason.
            if (s.skipDetector) {
                ts.resSkipDet.add(e);
                continue;
            }
            int bx = 0, by = 0;
            if (di >= 0 && det.binOf(p, bx, by)) {
                const std::size_t cell = scene.detectorOffsets()[std::size_t(di)] +
                                         std::size_t(by) * std::size_t(det.nx) +
                                         std::size_t(bx);
                if (ctx.atomicGrid) atomicAddRelaxed((*ctx.atomicGrid)[cell], e);
                else                (*grid)[cell] += e;
                if (ctx.varGrid) (*ctx.varGrid)[cell] += e * e;
                // The colour bands describe the first receiver, which is the one
                // the heatmap draws.
                if (ctx.bandGrid && ctx.bands && di == 0) {
                    for (int b = 0; b < 3; ++b)
                        (*ctx.bandGrid)[std::size_t(b) * ctx.bandCells + cell] +=
                            e * ctx.bandW[b];
                }
            }
            if (ctx.log) {
                RayInteraction in;
                in.point         = p;
                in.surface       = surfIdx;
                in.depth         = s.depth;
                in.angleDeg      = std::acos(std::clamp(std::fabs(s.d.dot(triN)), 0.0, 1.0))
                                   / kDegRad;
                in.energyIn      = e;
                in.opl           = opl;
                in.mediumSurface = s.medium;
                in.mediumDepth   = int(s.media.n);
                in.detector      = true;
                ctx.log->push_back(in);
            }
            ts.fluxDetector.add(e);
            rayDetector     += e;
            ++ts.raysHit;
            if (ctx.stray && ctx.chunk)
                ctx.chunk->addPath(s.stray, e, ctx.stray->maxPaths);
            if (phys.polarised) {
                // Flux weighted, because a polarisation is a property of the
                // light that arrives and not of the rays that carried it.
                ctx.detStokes[0] += e * s.stokes.i;
                ctx.detStokes[1] += e * s.stokes.q;
                ctx.detStokes[2] += e * s.stokes.u;
                ctx.detStokes[3] += e * s.stokes.v;
            }
            if (ctx.chunk && di >= 0 && std::size_t(di) < ctx.chunk->detFlux.size()) {
                ctx.chunk->detFlux[std::size_t(di)] += e;
                ++ctx.chunk->detHits[std::size_t(di)];
            }
            binDirection(ctx, s.d, e);

            if (ctx.chunk && ctx.arrivalCap > 0) {
                DetectorArrival a;
                a.p            = p;
                a.d            = s.d;
                a.energy       = float(e);
                a.wavelengthNm = float(ctx.lambda);
                a.opl          = float(opl);
                a.detector     = di < 0 ? 0 : di;
                a.source       = ctx.source;
                ChunkOutput& co = *ctx.chunk;
                ++co.arrivalSeen;
                if (co.arrivals.size() < ctx.arrivalCap) {
                    co.arrivals.push_back(a);
                } else {
                    // Algorithm R: the k-th kept slot is replaced with
                    // probability cap/seen, so every arrival is equally likely
                    // to survive however late in the chunk it happened.
                    const std::size_t j =
                        std::size_t(uniform01(co.arrivalRng) * double(co.arrivalSeen));
                    if (j < ctx.arrivalCap) co.arrivals[j] = a;
                }
            }
            // Mark every leg back to the emitter, so the viewer can show only
            // the rays that actually delivered energy.
            //
            // Stopping at the first leg that is already marked is not an
            // optimisation that trades anything away: the walk always runs to
            // the emitter, so a marked leg means every leg above it is marked
            // too. On a path that arrives fifty times -- a light guide, an
            // integrating sphere -- the walk was re-marking the same chain from
            // the receiver to the source on every one of them.
            for (int k = segIdx; k >= 0; k = ctx.chunk->parent[std::size_t(k)]) {
                RaySegment& leg = ctx.chunk->segments[std::size_t(k)];
                if (leg.reachedDetector()) break;
                leg.flags |= RaySegment::ReachedDetector;
            }
            continue;
        }

        // ---- normals -------------------------------------------------------
        // The facet normal decides geometry (which side of the surface the ray
        // is on, and which way to offset a spawned branch); the interpolated
        // normal decides physics. Keeping them apart is what lets a tessellated
        // optic behave like the smooth surface it approximates without the
        // approximation leaking into the medium bookkeeping.
        const bool geoLeaving = (s.d.dot(triN) > 0.0);
        Vec3 ng = geoLeaving ? -triN : triN;        // faces the incident ray
        if (!ng.normalize()) { ts.fluxAbsorbed.add(e); continue; }

        Vec3 n = scene.shadingNormal(h);
        if (geoLeaving) n = -n;
        // A vertex normal can tip past the incident ray at a silhouette; there
        // the facet normal is the only one that keeps the ray outside the solid.
        if (n.dot(s.d) > 0.0 || !n.normalize()) n = ng;

        // ---- one scattering model -------------------------------------------
        //
        // Resolved at trace setup, so there is a single model here rather than
        // three running side by side. The two forms it can take are different
        // kinds of thing and are treated as such:
        //
        //   a microfacet lobe is a *geometry*. It says which facet of the rough
        //   interface this ray actually met, so Fresnel and Snell are evaluated
        //   at that facet's normal and both branches leave from it. A rough
        //   interface is rough for the transmitted ray too.
        //
        //   a redistribution lobe -- Lambertian, ABg, a measured table -- leaves
        //   the interface smooth and changes where the energy goes after the
        //   split.
        //
        // What used to happen was neither: the normal was jittered, the energy
        // was whatever specular Fresnel gave it at the *smooth* normal, and the
        // ray then left in a diffuse direction.
        const bsdf::Surface& lobe = surf.bsdf;
        const bool microfacet   = (lobe.model == bsdf::Model::Microfacet) &&
                                  !lobe.isSpecular();
        const bool redistributes = !lobe.isSpecular() && !microfacet;
        const double lobeTis = lobe.isSpecular() ? 0.0 : lobe.totalIntegratedScatter();

        // The facet this interaction happens at, and the specular direction off
        // it. `n` becomes that facet, so everything below reads one normal.
        Vec3 microRefl;
        bool haveMicro = false;
        if (microfacet && lobeTis > 0.0 &&
            uniform01(ctx.rng) < (lobeTis < 1.0 ? lobeTis : 1.0)) {
            Vec3 h;
            double w = 1.0;
            if (lobe.sampleMicrofacet(s.d, n, ctx.rng, h, microRefl, w)) {
                n = h;
                haveMicro = true;
                // How much of that facet is actually visible, shadowing and
                // masking included. It scales the whole interaction rather than
                // one branch of it, because it is a property of the facet and
                // not of where the light goes next.
                if (w != 1.0) {
                    const double before = e;
                    e *= w;
                    ts.resBsdf.add(before - e);
                }
            }
        }

        const bool   refracts = surf.index > 0.0;
        const double cosI     = std::clamp(-s.d.dot(n), 0.0, 1.0);

        double reflE = 0.0, transE = 0.0;
        double n1 = 1.0, n2 = 1.0;
        bool   tir = false;
        // Which side of the interface this crossing is on, hoisted out of the
        // refractive block below because a metasurface reads it too: a pattern
        // lives on one face of a wafer and not on both, and a solid whose
        // surface carries one meets it twice unless somebody says which.
        bool   metaLeaving = false;

        polarisation::Stokes polState = s.stokes;
        polarisation::Stokes reflState = s.stokes;
        polarisation::Stokes transState = s.stokes;
        bool havePol = false;

        // The media the transmitted branch would find itself in.
        MediumStack afterStack = s.media;
        int         afterMed   = s.medium;

        if (refracts) {
            const double nSurf = surf.indexAt(ctx.lambda, phys.dispersion);

            // Entering or leaving, in three cases that between them cover every
            // way a surface can be modelled:
            //
            //   recorded inside this body   -> leaving it, whatever the winding
            //   travelling through vacuum   -> this crossing enters something
            //   inside some *other* body    -> the face orientation decides,
            //                                  which is what makes a cemented
            //                                  doublet refract glass -> glass
            //                                  instead of glass -> air
            //
            // The middle case is what keeps a half-space modelled as a single
            // sheet working: a bare sheet has no inside for the winding to
            // describe, so a ray in vacuum meeting one has entered it.
            const bool onStack = s.media.contains(surfIdx);
            const bool leaving = onStack || (s.medium >= 0 && geoLeaving);
            metaLeaving = leaving;

            // Exiting a body that was never recorded as entered -- the sheet
            // idiom again, where the entrance and the exit are different
            // surfaces -- pops whatever the ray *is* in rather than a body the
            // stack never held.
            if (leaving) {
                if (!afterStack.popValue(onStack ? surfIdx : s.medium))
                    ++ts.medUnmatchedExit;
            } else {
                if (!afterStack.push(surfIdx)) ++ts.medStackOverflow;
            }
            afterMed = effectiveMedium(ctx, afterStack);

            // Crossing a face outward while recorded as being in vacuum: the
            // incident index has to be guessed. Legitimate for a bare sheet,
            // and a symptom of a self-intersecting or non-manifold mesh
            // otherwise -- which is exactly what imported CAD produces.
            if (s.medium < 0 && geoLeaving && !onStack) ++ts.medGuessedIndex;

            n1 = (s.medium >= 0) ? mediumIndex(ctx, s.medium)
                                 : (leaving ? nSurf : 1.0);
            n2 = mediumIndex(ctx, afterMed);
            // Entering a body while already inside a higher-priority one leaves
            // n2 unchanged, which is what priority is for; entering from vacuum
            // gives n2 == nSurf.

            const double eta   = n1 / n2;
            const double sinT2 = eta * eta * (1.0 - cosI * cosI);
            tir = (sinT2 >= 1.0);

            if (phys.fresnel && surf.fresnel) {
                // Angle-dependent split: ~4 % at normal incidence for air/glass,
                // rising to 1 at grazing, and exactly 1 beyond the critical
                // angle. No energy is lost at the interface itself -- surface
                // absorption is a coating property, and bulk loss is Beer-Lambert.
                double R = fresnelReflectance(n1, n2, cosI);

                // The thin film on top of it. An uncoated multi-element system
                // overstates its loss by roughly 3.5 % per surface against any
                // real lens, all of which are coated.
                if (phys.coatings && surf.coating.active()) {
                    double as = 0.0, ap = 0.0, phase = 0.0;
                    polarisation::fresnelAmplitudes(n1, n2, cosI, as, ap, phase);
                    double rs = 0.0, rp = 0.0;
                    surf.coating.reflectanceSP(n1, n2, cosI, ctx.lambda,
                                               as * as, ap * ap, rs, rp);
                    R = 0.5 * (rs + rp);
                }
                reflE  = e * R;
                transE = e - reflE;

                // The polarised path: a Mueller matrix at the interface, with
                // the frame first rotated into its plane of incidence, because
                // a Stokes vector only means anything relative to a frame.
                //
                // The split itself then comes from the state rather than from
                // the unpolarised average: s-polarised light meeting glass at
                // Brewster's angle reflects strongly and p-polarised light
                // reflects nothing at all, and that difference is the whole
                // reason to carry the state.
                if (phys.polarised) {
                    double as = 0.0, ap = 0.0, phase = 0.0;
                    polarisation::fresnelAmplitudes(n1, n2, cosI, as, ap, phase);
                    if (phys.coatings && surf.coating.active()) {
                        double rs = 0.0, rp = 0.0;
                        surf.coating.reflectanceSP(n1, n2, cosI, ctx.lambda,
                                                   as * as, ap * ap, rs, rp);
                        as = std::sqrt(std::max(0.0, rs));
                        ap = std::sqrt(std::max(0.0, rp));
                    }
                    polState = s.stokes;
                    if (s.polFrame.lengthSquared() > 0.0) {
                        const double turn = polarisation::frameRotation(s.d, s.polFrame, ng);
                        polState = polarisation::Mueller::rotation(turn).apply(polState);
                    }
                    const polarisation::Stokes out =
                        polarisation::Mueller::fromFresnel(as, ap, phase).apply(polState);
                    const double total = std::max(1e-18, polState.i);
                    R = std::clamp(out.i / total, 0.0, 1.0);

                    reflState = out;
                    // Transmission takes what reflection did not, component by
                    // component: that difference is what makes a stack of plates
                    // a polariser.
                    transState.i = polState.i - out.i;
                    transState.q = polState.q - out.q;
                    transState.u = polState.u - out.u;
                    transState.v = polState.v - out.v;
                    // Renormalised, because flux lives in `energy` and only the
                    // direction of polarisation lives here.
                    if (reflState.i  > 1e-15) reflState  = reflState.scaled(1.0 / reflState.i);
                    else                      reflState  = polarisation::Stokes::unpolarised();
                    if (transState.i > 1e-15) transState = transState.scaled(1.0 / transState.i);
                    else                      transState = polarisation::Stokes::unpolarised();
                    havePol = true;

                    reflE  = e * R;
                    transE = e - reflE;
                }
            } else {
                reflE  = e * surf.reflectivity;
                transE = e * surf.transmissivity;
                ts.fluxAbsorbed.add(e - reflE - transE);
            }

            if (tir) {
                // Past the critical angle everything goes back into the same
                // medium. Booking that as one reflected branch rather than as a
                // reflected branch plus a coincident "transmitted" one is the
                // difference between a surface and a surface with a meaningless
                // energy split across two identical rays.
                reflE += transE;
                transE = 0.0;
            }
        } else {
            // An opaque surface. A metal one gets its reflectance from the
            // complex Fresnel equations: aluminium at 45 degrees is not
            // aluminium at normal incidence, and neither is the same at 460 nm
            // as at 620. A plain mirror keeps its fixed number.
            double R = surf.reflectivity;
            const bool metal = phys.fresnel && surf.material.isMetal();
            const double nIn  = mediumIndex(ctx, s.medium);
            const double nMet = surf.material.indexAt(ctx.lambda);
            const double kMet = surf.material.extinctionAt(ctx.lambda);
            if (metal) R = metalReflectance(nIn, nMet, kMet, cosI);

            // The polarised path at a mirror. This branch used to leave
            // `havePol` false, so the reflected branch inherited the incoming
            // Stokes vector verbatim: a polarised run bouncing off an aluminium
            // reflector reported the source's polarisation rather than the
            // mirror's. Metal reflection is the largest single source of
            // polarisation change in most reflective systems, and the machinery
            // it needs was already written and already tested.
            if (phys.polarised && metal) {
                double as = 0.0, ap = 0.0, phase = 0.0;
                metalAmplitudes(nIn, nMet, kMet, cosI, as, ap, phase);

                polState = s.stokes;
                if (s.polFrame.lengthSquared() > 0.0) {
                    const double turn = polarisation::frameRotation(s.d, s.polFrame, ng);
                    polState = polarisation::Mueller::rotation(turn).apply(polState);
                }
                const polarisation::Stokes outS =
                    polarisation::Mueller::fromFresnel(as, ap, phase).apply(polState);
                // The reflectance the *state* sees, which is what makes an
                // s-polarised beam and a p-polarised one leave a mirror with
                // different energies rather than the same average.
                const double total = std::max(1e-18, polState.i);
                R = std::clamp(outS.i / total, 0.0, 1.0);
                reflState = (outS.i > 1e-15) ? outS.scaled(1.0 / outS.i)
                                             : polarisation::Stokes::unpolarised();
                havePol = true;
            }

            reflE = e * R;
            ts.fluxAbsorbed.add(e - reflE);
        }

        if (ctx.log) {
            // Before the splitting control below, deliberately: what a reader
            // wants to see is the reflectance the surface actually has, not the
            // collapsed single branch the estimator chose to follow.
            RayInteraction in;
            in.point         = p;
            in.surface       = surfIdx;
            in.depth         = s.depth;
            in.angleDeg      = std::acos(std::clamp(cosI, 0.0, 1.0)) / kDegRad;
            in.n1            = n1;
            in.n2            = n2;
            in.reflectance   = e > 0.0 ? reflE / e : 0.0;
            in.transmittance = e > 0.0 ? transE / e : 0.0;
            in.energyIn      = e;
            in.opl           = opl;
            in.mediumSurface = s.medium;
            in.mediumDepth   = int(s.media.n);
            in.tir           = tir;
            in.scattered     = redistributes || haveMicro;
            ctx.log->push_back(in);
        }

        // ---- splitting control ---------------------------------------------
        // Following both branches doubles the tree at every refractive hit. When
        // one of them carries almost everything, pick one weighted by its share
        // and give it the pair total instead: same expectation, one path.
        bool doRefl  = reflE  > 0.0;
        bool doTrans = transE > 0.0;
        if (phys.varianceReduction && doRefl && doTrans) {
            const double tot = reflE + transE;
            const double fr  = reflE / tot;
            if (std::min(fr, 1.0 - fr) < kSplitMin) {
                if (uniform01(ctx.rng) < fr) { reflE = tot; doTrans = false; }
                else                         { transE = tot; doRefl  = false; }
            }
        }

        // Spawn a branch offset along the geometric normal, on the side it is
        // actually leaving on. Offsetting along the ray instead leaves a grazing
        // branch a hair above the surface it just left.
        const double eps = surfaceEps(p);
        auto spawn = [&](Vec3 outDir, double outE, const MediumStack& media,
                         int med, bool skipDet = false,
                         const polarisation::Stokes* pol = nullptr) {
            if (!outDir.normalize()) {
                ts.truncDegenerate.add(outE);
                ++ts.nTruncDegenerate;
                return;
            }
            const Vec3 off = ng * std::copysign(eps, outDir.dot(ng));
            PathState st;
            st.o      = p + off;
            st.d      = outDir;
            st.energy = outE;
            st.opl    = opl;
            st.media  = media;
            st.medium = med;
            st.depth  = s.depth + 1;
            st.seg    = segIdx;
            st.skipDetector = skipDet;
            // The branch inherits the route that reached this surface. It is
            // built fresh rather than copied from `s`, so this is the one place
            // the route has to be carried over by hand.
            st.stray  = s.stray;
            if (phys.polarised) {
                st.stokes   = pol ? *pol : s.stokes;
                st.polFrame = ng;
            }
            push(st);
        };

        // Refraction (Snell). Pushed first so it is popped last, matching the
        // order the recursive tracer visited branches in.
        if (doTrans) {
            Vec3 tD;
            if (refract(s.d, n, n1 / n2, cosI, tD)) {
                // An interpolated normal can bend the transmitted ray back to
                // the near side; the facet normal is the arbiter of which side
                // is which.
                if (tD.dot(ng) >= 0.0 && !refract(s.d, ng, n1 / n2,
                                                  std::clamp(-s.d.dot(ng), 0.0, 1.0), tD)) {
                    ts.truncRefract.add(transE);
                    ++ts.nTruncRefract;
                    tD = Vec3();
                }
                // A designed phase gradient replaces the direction Snell just
                // produced. The law it obeys is the generalised one -- the
                // tangential wavevector picks up the gradient of the surface
                // phase -- and the order it leaves in is drawn from the
                // efficiency table rather than assumed to be the first.
                if (tD.lengthSquared() > 0.0 && surf.metasurface.active() &&
                    !surf.metasurface.reflective &&
                    surf.metasurface.patternsThisCrossing(metaLeaving)) {
                    MetaPick pick;
                    if (sampleMetaOrder(surf.metasurface, s.d, n, p, ctx.lambda,
                                        n1, n2, ctx.rng, tD,
                                        phys.polarised ? &transState : nullptr,
                                        pick)) {
                        tD = pick.direction;
                        if (pick.polarised) { transState = pick.state; havePol = true; }
                        // What no propagating order took. Absorbed at the
                        // surface and booked there, so the budget closes on
                        // the light the device did not diffract instead of
                        // gaining it back.
                        const double kept = transE * pick.efficiency;
                        ts.fluxAbsorbed.add(transE - kept);
                        transE = kept;
                    } else {
                        // Nothing propagates at this angle at all.
                        ts.fluxAbsorbed.add(transE);
                        transE = 0.0;
                        tD = Vec3();
                    }
                }
                if (tD.lengthSquared() > 0.0 && transE > 0.0) {
                    bool diffuse = false;
                    // A transmissive diffuser re-emits about the far side of the
                    // surface, through the same lobe the reflected branch uses.
                    // The geometric normal, not the facet: a redistribution lobe
                    // belongs to the surface rather than to its microfacets.
                    if (redistributes && lobeTis > 0.0 &&
                        uniform01(ctx.rng) < (lobeTis < 1.0 ? lobeTis : 1.0)) {
                        Vec3 o;
                        double w = 1.0;
                        if (lobe.sample(s.d, -ng, tD, ctx.rng, o, w)) {
                            tD = o;
                            const double before = transE;
                            transE *= w;
                            ts.resBsdf.add(before - transE);
                            diffuse = (lobe.model == bsdf::Model::Lambertian);
                        }
                    }
                    bool estimated = false;
                    if (diffuse) {
                        const double direct =
                            nextEventEstimate(ctx, s.stray, p, -ng, transE, opl, afterMed);
                        estimated = (direct >= 0.0);
                        if (direct > 0.0) {
                            ts.resNee.add(-direct);
                            rayDetector += direct;
                        }
                    }
                    spawn(tD, transE, afterStack, afterMed, estimated,
                          havePol ? &transState : nullptr);
                }
            } else {
                // Snell refused where the Fresnel term did not: fold it back
                // into the reflected branch rather than losing it.
                reflE += transE;
                doRefl = true;
            }
        }

        // Specular (or diffuse) reflection, staying in the current medium.
        if (doRefl && reflE > 0.0) {
            // Off the facet where there was one, off the smooth normal where
            // there was not.
            Vec3 refl = haveMicro ? microRefl : reflect(s.d, n);
            if (refl.dot(ng) <= 0.0) refl = reflect(s.d, ng);
            // A reflective metasurface -- a reflectarray -- deflects the
            // branch that comes back off it rather than the one that goes
            // through, and the law is the same one with the outgoing medium
            // being the incident one.
            if (surf.metasurface.active() && surf.metasurface.reflective) {
                MetaPick pick;
                if (sampleMetaOrder(surf.metasurface, s.d, n, p, ctx.lambda,
                                    n1, n2, ctx.rng, refl,
                                    phys.polarised ? &reflState : nullptr, pick)) {
                    refl = pick.direction;
                    if (pick.polarised) { reflState = pick.state; havePol = true; }
                    const double kept = reflE * pick.efficiency;
                    ts.fluxAbsorbed.add(reflE - kept);
                    reflE = kept;
                } else {
                    ts.fluxAbsorbed.add(reflE);
                    reflE = 0.0;
                }
            }
            bool diffuse = false;
            if (redistributes && lobeTis > 0.0 &&
                uniform01(ctx.rng) < (lobeTis < 1.0 ? lobeTis : 1.0)) {
                Vec3 o;
                double w = 1.0;
                if (lobe.sample(s.d, ng, refl, ctx.rng, o, w)) {
                    refl = o;
                    // The weight is a ratio of the lobe to its own sampling
                    // density, so it multiplies the branch's energy. Anything it
                    // creates or destroys is the estimator's, and booked where
                    // the rest of the estimator's residual goes.
                    const double before = reflE;
                    reflE *= w;
                    ts.resBsdf.add(before - reflE);
                    diffuse = (lobe.model == bsdf::Model::Lambertian);
                }
            }
            bool estimated = false;
            if (diffuse) {
                const double direct = nextEventEstimate(ctx, s.stray, p, ng, reflE, opl, s.medium);
                estimated = (direct >= 0.0);
                if (direct > 0.0) {
                    ts.resNee.add(-direct);
                    rayDetector += direct;
                }
            }
            if (reflE > 0.0)
                spawn(refl, reflE, s.media, s.medium, estimated,
                      havePol ? &reflState : nullptr);
        }
    }

    ts.det.add(rayDetector);
    if (ctx.chunk && ctx.replica >= 0 &&
        std::size_t(ctx.replica) < ctx.chunk->replicaDet.size())
        ctx.chunk->replicaDet[std::size_t(ctx.replica)] += rayDetector;
    if (ctx.chunk && ctx.source >= 0 &&
        std::size_t(ctx.source) < ctx.chunk->sourceDet.size())
        ctx.chunk->sourceDet[std::size_t(ctx.source)] += rayDetector;
}


// ---- far-field reduction ---------------------------------------------------


} // namespace

// Resolves every surface's scattering into the one model the tracer reads, and
// writes it back over the run's own copy of the surfaces.
//
// Done once per run rather than per hit, so the inner loop reads one model
// instead of choosing between three. It also puts the scene-wide overrides and
// the physics switches in one place: `scattering` gates the redistribution
// lobes and `roughness` gates the microfacet one, which is the split those two
// switches always drove.
//
// After this, `SurfaceOptics::scatter` and `::roughness` are not read again by
// the tracer at all. They are the way a scene *says* what it is; `bsdf` is what
// the tracer *does*.
std::vector<int> mediaContainingPoint(const TraceScene& scene,
                                      const std::vector<SceneSurface>& surfs,
                                      const Vec3& p) {
    const MediumStack st = mediaContaining(scene, surfs, p);
    std::vector<int> out;
    out.reserve(st.n);
    for (std::uint8_t i = 0; i < st.n; ++i) out.push_back(int(st.e[i]));
    return out;
}

void resolveScattering(std::vector<SceneSurface>& surfs, const PhysicsOptions& phys) {
    for (SceneSurface& s : surfs) {
        SurfaceOptics t = s;
        if (phys.roughnessOverride >= 0.0) t.roughness = phys.roughnessOverride;
        if (phys.scatterOverride   >= 0.0) t.scatter   = phys.scatterOverride;

        bsdf::Surface b = t.effectiveBsdf();
        const bool micro = (b.model == bsdf::Model::Microfacet);
        if (( micro && !phys.roughness) ||
            (!micro && !phys.scattering))
            b = bsdf::Surface{};

        s.bsdf = b;
    }
}

// The receiver frames of a scene, copied onto a result. Every readout that maps
// a bin back to world millimetres reads these rather than assuming +Z.
// Copies the named channels off the accumulators onto a result. Shared by the
// single-ray path, the final reduction and every progressive snapshot, so a
// preview reports the same breakdown the finished run does.
void fillDiagnostics(SimulationResult& r, const RayStats& t) {
    r.fluxTruncated = t.truncatedTotal();
    r.fluxRejected  = t.fluxRejected.value();
    r.fluxRoulette  = t.residualTotal();

    r.residual.roulette      = t.resRoulette.value();
    r.residual.aiming        = t.resAiming.value();
    r.residual.nextEvent     = t.resNee.value();
    r.residual.neeSuppressed = t.resSkipDet.value();
    r.residual.bsdfWeight    = t.resBsdf.value();

    r.truncation.depthLimit    = t.truncDepth.value();
    r.truncation.stackOverflow = t.truncStack.value();
    r.truncation.degenerate    = t.truncDegenerate.value();
    r.truncation.refractFailed = t.truncRefract.value();
    r.truncation.energyCutoff  = t.truncCutoff.value();
    r.truncation.depthLimitCount    = t.nTruncDepth;
    r.truncation.stackOverflowCount = t.nTruncStack;
    r.truncation.degenerateCount    = t.nTruncDegenerate;
    r.truncation.refractFailedCount = t.nTruncRefract;
    r.truncation.energyCutoffCount  = t.nTruncCutoff;

    r.anomalies.unmatchedExit = t.medUnmatchedExit;
    r.anomalies.stackOverflow = t.medStackOverflow;
    r.anomalies.guessedIndex  = t.medGuessedIndex;
}

void fillDetectorFrames(const TraceScene& scene, SimulationResult& out) {
    const auto& dets = scene.detectors();
    out.detectors.assign(dets.size(), DetectorFrame{});
    for (std::size_t i = 0; i < dets.size(); ++i) {
        const DetectorInfo& d = dets[i];
        DetectorFrame& f = out.detectors[i];
        f.center = d.center;
        f.u = d.u; f.v = d.v; f.normal = d.n;
        f.w = d.w; f.h = d.h;
        f.nx = d.nx; f.ny = d.ny;
        f.acceptanceDeg = (d.cosAcceptance <= -1.0)
                              ? 180.0
                              : std::acos(std::clamp(d.cosAcceptance, -1.0, 1.0)) / kDegRad;
        if (d.surf >= 0 && d.surf < int(scene.surfaces().size()))
            f.label = QStringLiteral("Detector %1").arg(i + 1);
        f.grid.assign(std::size_t(d.nx) * std::size_t(d.ny), 0.0);
    }
}

// Splits the flat all-receivers grid back out per receiver, and mirrors the
// first one into the irradiance grid the plots read.
void scatterDetectorGrids(const TraceScene& scene, const std::vector<double>& flat,
                          SimulationResult& out) {
    const auto& off = scene.detectorOffsets();
    for (std::size_t i = 0; i < out.detectors.size() && i < off.size(); ++i) {
        DetectorFrame& f = out.detectors[i];
        const std::size_t n = f.grid.size();
        for (std::size_t k = 0; k < n && off[i] + k < flat.size(); ++k)
            f.grid[k] = flat[off[i] + k];
    }
    if (!out.detectors.empty()) out.irradiance = out.detectors[0].grid;
}

namespace {


} // namespace

EmittedRay RayTracer::sampleRay(const SourceConfig& src, std::size_t index,
                                std::uint64_t seed) {
    SampledSpectrum spec;
    spec.build(src.spectrum);
    return sampleRay(src, spec, index, seed);
}

EmittedRay RayTracer::sampleRay(const SourceConfig& src, const SampledSpectrum& spec,
                                std::size_t index, std::uint64_t seed,
                                bool lowDiscrepancy) {
    const EmissionBasis basis(src.axis);
    // No scene here, so no aiming: this is the source's own distribution, which
    // is what a sampling test wants to see.
    return sampleEmission(basis, src, spec, index, seed, lowDiscrepancy);
}

Vec3 RayTracer::sampleDirection(const SourceConfig& src, std::size_t index,
                                std::uint64_t seed) {
    return sampleRay(src, index, seed).dir;
}

void RayTracer::traceSingleRay(const TraceScene& scene, const Vec3& origin,
                               const Vec3& dir, SimulationResult& out, double energy,
                               const PhysicsOptions& physics, double wavelengthNm) {
    const DetectorInfo& det = scene.detector();

    out = SimulationResult{};
    out.nx    = (det.valid && det.nx > 0) ? det.nx : 1;
    out.ny    = (det.valid && det.ny > 0) ? det.ny : 1;
    out.detW  = det.w;
    out.detH  = det.h;
    out.detCX = det.center.dot(det.u);
    out.detCY = det.center.dot(det.v);
    out.detZ  = det.center.z;
    out.detU  = det.u;
    out.detV  = det.v;
    out.detN  = det.n;
    out.sourcePower = energy;
    out.raysEmitted = 1;
    out.meanWavelengthNm = wavelengthNm;
    fillDetectorFrames(scene, out);
    out.irradiance.assign(std::size_t(out.nx) * std::size_t(out.ny), 0.0);

    // A scene with no receiver at all still needs one frame for the tracer to
    // read, so it never has to branch on validity in the inner loop.
    std::vector<DetectorInfo> dets = scene.detectors();
    if (dets.empty()) dets.push_back(DetectorInfo{});

    std::vector<SceneSurface> surfs = scene.surfaces();
    resolveScattering(surfs, physics);

    RayStats stats;
    std::vector<double> grid(std::max<std::size_t>(1, scene.detectorCells()), 0.0);
    ChunkOutput chunk;
    chunk.detFlux.assign(dets.size(), 0.0);
    chunk.detHits.assign(dets.size(), 0);

    TraceContext ctx;
    ctx.scene      = &scene;
    ctx.surfs      = &surfs;
    ctx.dets       = &dets;
    ctx.grid       = &grid;
    ctx.stats      = &stats;
    ctx.chunk      = &chunk;
    ctx.segmentCap = 4096;
    ctx.arrivalCap = 4096;
    ctx.physics    = physics;
    // Variance reduction is off for a single ray for the same reason the
    // estimator switches are: what is wanted here is the whole path tree.
    ctx.estimator.lowDiscrepancy      = false;
    ctx.estimator.aimAtScene          = false;
    ctx.estimator.nextEventEstimation = false;
    // One ray is traced to pin down one interaction, so it follows the whole
    // path tree with the energy the laws actually give each branch. Roulette
    // and branch collapsing are unbiased over a run and meaningless over a
    // single draw, so they are off here whatever the caller asked for.
    ctx.physics.varianceReduction = false;
    ctx.lambda     = wavelengthNm;
    ctx.log        = &out.interactions;
    ctx.emitted    = polarisation::Stokes::unpolarised();
    ctx.rng        = mix64(kInteractionSalt);
    chunk.arrivalRng = mix64(kReservoirSalt);

    Vec3 d = dir;
    if (!d.normalize()) { out.fluxEscaped = energy; return; }
    tracePath(ctx, origin, d, energy);

    out.fluxDetector     = stats.fluxDetector.value();
    out.fluxAbsorbed     = stats.fluxAbsorbed.value();
    out.fluxBulkAbsorbed = stats.fluxBulk.value();
    out.fluxEscaped      = stats.fluxEscaped.value();
    fillDiagnostics(out, stats);
    out.raysHitDetector  = stats.raysHit;
    scatterDetectorGrids(scene, grid, out);
    for (std::size_t i = 0; i < out.detectors.size() && i < chunk.detFlux.size(); ++i) {
        out.detectors[i].flux     = chunk.detFlux[i];
        out.detectors[i].arrivals = chunk.detHits[i];
    }
    if (out.irradiance.empty())
        out.irradiance.assign(std::size_t(out.nx) * std::size_t(out.ny), 0.0);
    out.raySegments      = std::move(chunk.segments);
    out.arrivals         = std::move(chunk.arrivals);
    out.efficiency       = (energy > 0.0) ? (out.fluxDetector / energy) : 0.0;
    // The tracer knows surfaces by index; a reader knows them by name.
    for (RayInteraction& in : out.interactions)
        in.label = scene.surfaceLabel(in.surface);
}

void RayTracer::trace(const MeshList& meshes, const SourceConfig& src,
                      SimulationResult& out, const TraceOptions& opt,
                      const TraceControl& ctl) {
    TraceScene scene;
    scene.build(meshes);
    trace(scene, src, out, opt, ctl);
}

namespace {

// One worker's published view of its own progress, taken between chunks so a
// snapshot never catches a half-written grid.
//
// A preview reduces per thread rather than per chunk, so its last digits depend
// on how the chunks happened to be scheduled. That is the deliberate difference
// between the picture forming on screen and the result the run returns: the
// preview is for looking at, the result is the bit-reproducible one.
struct PreviewSlot {
    std::atomic<std::uint32_t> published{0};   // publication counter, acquire/release
    // Cumulative: the scalars are a handful of doubles and re-reading them is
    // cheaper than differencing them.
    RayStats            stats;
    // What this worker has accumulated *since its last publication*. A snapshot
    // of the whole grid meant the preview copied it once into the slot, once
    // again into a per-tick temporary, and then reduced every thread's entire
    // history from scratch -- three passes over threads x cells, every tick,
    // for a picture that had changed by one tick's worth of rays. A delta is
    // added straight into a running total the previewer keeps.
    std::vector<double> grid;
    std::vector<double> bandGrid;
    std::vector<double> angleGrid;
};

} // namespace

void RayTracer::trace(const TraceScene& scene, const SourceConfig& src,
                      SimulationResult& out, const TraceOptions& opt,
                      const TraceControl& ctl) {
    trace(scene, std::vector<SourceConfig>{src}, out, opt, ctl);
}

void RayTracer::trace(const TraceScene& scene, const std::vector<SourceConfig>& sourcesIn,
                      SimulationResult& out, const TraceOptions& opt,
                      const TraceControl& ctl) {
    const auto tStart = std::chrono::steady_clock::now();

    // At least one source, so nothing downstream has to branch on the count.
    std::vector<SourceConfig> srcs = sourcesIn;
    if (srcs.empty()) srcs.push_back(SourceConfig{});
    const std::size_t nSrc = srcs.size();
    // Every source reports in the same unit. Mixing watts and lumens in one run
    // is not something a single result can be expressed in, so the first
    // source governs and the rest are read in its unit.
    const FluxUnit runUnit = srcs.front().fluxUnit;
    for (SourceConfig& s : srcs) s.fluxUnit = runUnit;

    const DetectorInfo& det = scene.detector();
    const int  nx       = (det.valid && det.nx > 0) ? det.nx : 1;
    const int  ny       = (det.valid && det.ny > 0) ? det.ny : 1;
    const int  nTheta   = std::max(0, opt.nTheta);
    const int  nPhi     = nTheta > 0 ? std::max(1, opt.nPhi) : 0;
    // The far field is accumulated on a finer, fixed theta resolution (the
    // "master") so a narrow beam is actually resolved before the beam-adaptive
    // reduction of the requested output grid below. `nTheta` stays the count
    // the user asked for -- the adaptive output bins concentrate within it.
    const int  nThetaMaster = nTheta > 0 ? std::max(nTheta, 720) : 0;
    const int  nPhiMaster   = nPhi;

    // Each source resolves its own spectrum once per run: an inverse CDF, a
    // luminous efficacy and a mean wavelength, all of which its rays then read.
    std::vector<SampledSpectrum> specs(nSrc);
    bool spectral = false;
    for (std::size_t s = 0; s < nSrc; ++s) {
        specs[s].build(srcs[s].spectrum);
        if (!specs[s].monochromatic()) spectral = true;
    }

    // Where each source occupies the global ray index space, and what one of
    // its rays is worth.
    //
    // Emitting a ray of source s already carrying power_s / rays_s is what lets
    // one shared receiver grid hold every source correctly weighted -- a
    // two-source run costs one grid rather than two. For a single source the
    // factor divides straight back out in the normalisation below, so the
    // one-source answer is bit-identical to what it was before there were
    // several.
    std::vector<std::size_t> offset(nSrc + 1, 0);
    std::vector<double>      kScale(nSrc, 1.0);
    double totalPower = 0.0;
    for (std::size_t s = 0; s < nSrc; ++s) {
        const std::size_t r = srcs[s].rays > 0 ? std::size_t(srcs[s].rays) : 0;
        offset[s + 1] = offset[s] + r;
        totalPower   += srcs[s].power;
    }
    // The weight is *relative* to what a ray of the run as a whole is worth,
    // not the absolute power per ray. Two reasons, and the second is the one
    // that bites: Russian roulette's cutoff is an absolute energy, so scaling
    // every ray by the source power would make the estimator's behaviour --
    // and therefore the random stream -- depend on whether the user typed
    // watts or milliwatts. Relative weights leave a one-source run carrying
    // exactly 1, which is what it always carried.
    {
        const std::size_t totalR = offset[nSrc];
        const double mean = (totalR > 0 && totalPower > 0.0)
                                ? totalPower / double(totalR) : 0.0;
        for (std::size_t s = 0; s < nSrc; ++s) {
            const std::size_t r = offset[s + 1] - offset[s];
            kScale[s] = (mean > 0.0 && r > 0)
                            ? (srcs[s].power / double(r)) / mean
                            : 1.0;
        }
    }

    // Power-weighted, because that is what the combined beam is: two sources of
    // different colour give the run one efficacy, and it is the one a
    // photometer would read.
    double efficacy = 0.0, meanLambda = 0.0;
    for (std::size_t s = 0; s < nSrc; ++s) {
        const double w = totalPower > 0.0 ? srcs[s].power / totalPower
                                          : 1.0 / double(nSrc);
        efficacy   += w * specs[s].efficacy();
        meanLambda += w * specs[s].meanWavelength();
    }

    // Which source a global ray index belongs to. Linear over the source list,
    // which is the right shape: a run has a handful of sources, and a binary
    // search over three entries costs more than it saves.
    auto sourceOfRay = [&](std::size_t i) {
        std::size_t s = 0;
        while (s + 1 < nSrc && i >= offset[s + 1]) ++s;
        return s;
    };

    out = SimulationResult{};
    out.nx = nx; out.ny = ny;
    out.detW  = det.w;
    out.detH  = det.h;
    out.detCX = det.center.dot(det.u);
    out.detCY = det.center.dot(det.v);
    out.detZ  = det.center.z;
    out.detU  = det.u;
    out.detV  = det.v;
    out.detN  = det.n;
    out.spectral = spectral;
    fillDetectorFrames(scene, out);
    out.irradiance.assign(std::size_t(nx) * std::size_t(ny), 0.0);
    if (spectral) out.bandIrradiance.assign(out.irradiance.size() * 3, 0.0);
    out.sourcePower      = totalPower;
    out.unit             = runUnit;
    out.luminousEfficacy = efficacy;
    out.meanWavelengthNm = meanLambda;

    out.sources.resize(nSrc);
    for (std::size_t s = 0; s < nSrc; ++s) {
        SourceSummary& sum = out.sources[s];
        sum.label   = srcs[s].label;
        sum.power   = srcs[s].power;
        sum.rays    = offset[s + 1] - offset[s];
        sum.rayFile = srcs[s].tracesRayFile();
    }

    const std::size_t totalRays = offset[nSrc];
    if (totalRays == 0 || scene.empty()) {
        // Nothing to intersect: all emitted energy escapes.
        out.fluxEscaped = (totalRays > 0) ? totalPower : 0.0;
        out.raysEmitted = totalRays;
        if (ctl.progress) ctl.progress(totalRays, totalRays);
        out.traceSeconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - tStart).count();
        return;
    }

    // The surfaces this run sees. Copying them is a handful of structs and it is
    // what lets a user edit the optic they just clicked on and re-trace without
    // re-tessellating anything.
    std::vector<SceneSurface> surfs = scene.surfaces();
    // Overrides resolve by label first and by stored index only as a hint, so a
    // config saved against an older registry -- or against a different CAD
    // import -- cannot land an optical edit on whatever surface now occupies
    // the slot. An edit that matches nothing is reported, not dropped silently.
    for (const auto& ov : opt.surfaceOverrides) {
        const int idx = scene.resolveSurface(ov.label, ov.identity, ov.surface);
        if (idx >= 0 && idx < int(surfs.size())) surfs[std::size_t(idx)] = ov.optics;
        else out.unmatchedOverrides.push_back(
            ov.label.isEmpty() ? QStringLiteral("surface %1").arg(ov.surface) : ov.label);
    }
    // After the edits, not before: a surface whose scatter a user just changed
    // has to resolve to the model that change implies.
    resolveScattering(surfs, opt.physics);

    // A detector frame is kept even when the scene has no detector, so the
    // tracer never has to branch on validity in the inner loop.
    std::vector<DetectorInfo> dets = scene.detectors();
    if (dets.empty()) {
        DetectorInfo d = det;
        d.nx = nx; d.ny = ny;
        dets.push_back(d);
    }

    // A receiver's acceptance cone and what it does with a ray it refuses are
    // ray-time properties, exactly like roughness -- so an override of them has
    // to reach the frame the tracer reads, not just the surface record. It did
    // not, which made "narrow the acceptance angle" an edit with no effect.
    //
    // The bin grid is not here on purpose: it is baked into the flat
    // all-receivers layout, so changing it is a geometry rebuild and goes
    // through detectorBins instead.
    for (DetectorInfo& d : dets) {
        if (d.surf < 0 || d.surf >= int(surfs.size())) continue;
        const SceneSurface& s = surfs[std::size_t(d.surf)];
        d.cosAcceptance = (s.detAcceptanceDeg >= 180.0)
                              ? -1.0
                              : std::cos(std::clamp(s.detAcceptanceDeg, 0.0, 180.0) * kDegRad);
        d.rejectPasses  = (s.detRejectMode == SurfaceOptics::RejectMode::Pass);
    }

    std::vector<EmissionBasis> bases;
    bases.reserve(nSrc);
    for (const SourceConfig& s : srcs) bases.emplace_back(s.axis);

    // What each source is immersed in, resolved once. A source in air -- every
    // one the registry places -- comes back empty, and the run is then bit for
    // bit what it was before there was a probe at all.
    std::vector<MediumStack> srcMedia(nSrc);
    std::vector<int>         srcMedium(nSrc, -1);
    for (std::size_t s = 0; s < nSrc; ++s) {
        srcMedia[s] = mediaContaining(scene, surfs, Vec3(srcs[s].origin));
        if (srcMedia[s].n == 0) continue;
        TraceContext probe;                 // effectiveMedium reads surfs through one
        probe.surfs  = &surfs;
        srcMedium[s] = effectiveMedium(probe, srcMedia[s]);
    }

    // One emission stream per source, so two sources are two samples of the
    // scene rather than one sample counted twice. The first source's salt is
    // the identity, which is what keeps a one-source run -- and therefore every
    // reference value in the corpus -- bit-identical to what it always was.
    std::vector<std::uint64_t> srcSeed(nSrc, opt.seed);
    for (std::size_t s = 1; s < nSrc; ++s)
        srcSeed[s] = mix64(opt.seed ^ (std::uint64_t(s) * kSourceSalt));

    // The ray budget is split into independent Owen scrambles, so the spread
    // across them measures the error the quasi-Monte Carlo estimator actually
    // achieved rather than the one an equivalent random run would have had.
    const int replicas = opt.estimator.lowDiscrepancy
                             ? int(std::min<std::size_t>(
                                   std::max(2, std::min(opt.estimator.replicas, 256)),
                                   totalRays))
                             : 1;
    // Replicas partition each source block rather than the run as a whole, so
    // every replica holds a proportional share of every source and the spread
    // across them still measures the error of the combined answer.
    std::vector<std::size_t> perReplica(nSrc, 1);
    for (std::size_t s = 0; s < nSrc; ++s) {
        const std::size_t r = offset[s + 1] - offset[s];
        perReplica[s] = std::max<std::size_t>(1, (r + std::size_t(replicas) - 1) /
                                                     std::size_t(replicas));
    }

    // What each source emits, on a polarised run. Unpolarised is the default
    // and the only state an ordinary illumination source is in.
    std::vector<polarisation::Stokes> emittedState(nSrc,
                                                   polarisation::Stokes::unpolarised());
    for (std::size_t s = 0; s < nSrc; ++s) {
        switch (srcs[s].polarisationState) {
        case 1: emittedState[s] = polarisation::Stokes::linearS(); break;
        case 2: emittedState[s] = polarisation::Stokes::linearP(); break;
        case 3: emittedState[s] = polarisation::Stokes::circular(); break;
        default: break;
        }
    }

    // Emission aiming needs the extent of everything a ray could hit. A source
    // reading a measured ray file declines it: those directions were measured,
    // not drawn, so there is no distribution left to narrow.
    const Vec3   sceneCenter = scene.boundsCenter();
    const double aimRadius   = opt.estimator.aimAtScene ? scene.boundsRadius() : 0.0;
    std::vector<const Vec3*> aimCenter(nSrc, nullptr);
    for (std::size_t s = 0; s < nSrc; ++s)
        aimCenter[s] = (opt.estimator.aimAtScene && !srcs[s].tracesRayFile())
                           ? &sceneCenter : nullptr;

    const std::size_t numChunks = (totalRays + kChunkRays - 1) / kChunkRays;
    unsigned threads = opt.threads;
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = unsigned(std::max<std::size_t>(1, std::min<std::size_t>(threads, numChunks)));

    // Receiver bins are per-thread below a size threshold and shared atomically
    // above it. A 256x256 grid is 512 KB per thread -- harmless. A 1024x1024
    // grid is 8 MB per thread, and on a many-core machine those copies alone
    // can exceed any sane budget before a single ray is traced; receiver
    // resolution was effectively bounded by how many cores happened to be
    // present. Above the threshold we accumulate into ONE atomic grid instead
    // of a copy per thread, so resolution stops depending on the hardware.
    // Below it, the per-thread copies are cheap, cache-local and bit-exact --
    // strictly better than contended atomics -- so they are kept.
    const std::size_t allCells = std::max<std::size_t>(1, scene.detectorCells());
    constexpr std::size_t kAtomicThresholdCells = 64u * 1024u;  // one 256x256
    const bool atomicGrid = allCells >= kAtomicThresholdCells;

    // Bound what the remaining per-thread accumulators can cost. In the atomic
    // path the all-receivers grid is a single shared copy, so it is not counted
    // per thread here; the band and far-field grids always stay per-thread.
    //
    // The cap is to spend fewer threads rather than more memory, a trade this
    // tracer can make: the scalar totals reduce in chunk order and the grids are
    // reduced in a fixed order, so the answer does not depend on the thread
    // count -- there is a test that says so. A capped run is slower and
    // identical, not slower and different.
    {
        const std::size_t perThreadCells =
            (atomicGrid ? 0 : allCells) +
            (spectral ? std::size_t(nx) * std::size_t(ny) * 3 : 0) +
            std::size_t(nThetaMaster) * std::size_t(nPhi);
        // A preview doubles it: the worker's own accumulator plus the delta it
        // publishes.
        const std::size_t copies = ctl.partial ? 2 : 1;
        const std::size_t bytes  = perThreadCells * copies * sizeof(double);
        if (bytes > 0) {
            const std::size_t affordable = std::max<std::size_t>(1, kGridBudgetBytes / bytes);
            threads = unsigned(std::min<std::size_t>(threads, affordable));
        }
    }

    // Irradiance bands and far-field bins stay per thread (a per-chunk grid
    // would cost tens of KB per chunk); the scalar totals are per chunk so they
    // reduce in a fixed order. On the atomic path the all-receivers grid IS the
    // shared accumulator; on the small-receiver path it is one per thread.
    const std::size_t cells  = std::size_t(nx) * std::size_t(ny);
    const std::size_t angles = std::size_t(nThetaMaster) * std::size_t(nPhi);
    // The shared atomic all-receivers grid (large receivers), or an empty one.
    std::vector<std::atomic<double>> atomicBins(atomicGrid ? allCells : 0);
    for (auto& a : atomicBins) a.store(0.0, std::memory_order_relaxed);
    std::vector<std::vector<double>> grids(atomicGrid ? 0 : threads);
    // The noise map costs one more grid per thread and one multiply-add per
    // deposit, so it is only built when a caller wants it.
    const bool wantVar = opt.noiseMap && !atomicGrid;
    std::vector<std::vector<double>> varGrids(wantVar ? threads : 0);
    std::vector<std::vector<double>> bandGrids(spectral ? threads : 0);
    std::vector<std::vector<double>> angleGrids(angles ? threads : 0);
    std::vector<std::vector<double>> angleVarGrids((angles && opt.noiseMap) ? threads : 0);
    for (auto& g : grids)      g.assign(allCells, 0.0);
    for (auto& g : varGrids)   g.assign(allCells, 0.0);
    for (auto& g : bandGrids)  g.assign(cells * 3, 0.0);
    for (auto& g : angleGrids)    g.assign(angles, 0.0);
    for (auto& g : angleVarGrids) g.assign(angles, 0.0);
    std::vector<RayStats> chunkStats(numChunks);
    // One Stokes accumulator per thread. Reduced in thread order rather than
    // chunk order, which is enough for a reported polarisation and is why it is
    // reported to three digits rather than to the last bit.
    std::vector<std::array<double, 4>> threadStokes(threads, std::array<double, 4>{});

    // Path recording is a tree walk per hit, so only the leading rays of each
    // source have their paths kept; arrivals are one push_back each, so they
    // are recorded run-wide and bounded by their own budget instead.
    //
    // The budget is split per source rather than taken off the front of the
    // global index space. Sources own consecutive blocks of that space, so a
    // front-loaded budget was spent entirely inside the first source's block
    // and every other emitter drew no rays at all -- a two-lamp scene looked
    // like one lamp. A single-source run is unchanged: it owns the whole block
    // and takes the whole budget.
    std::vector<std::size_t> segRays(nSrc, 0);
    for (std::size_t s = 0; s < nSrc; ++s) {
        const std::size_t r = offset[s + 1] - offset[s];
        if (r == 0) continue;
        // Proportional to the ray count, but never zero: an emitter carrying a
        // hundredth of the light still has to appear in the diagram.
        const std::size_t want = totalRays > 0 ? (opt.segmentRays * r) / totalRays
                                               : opt.segmentRays;
        segRays[s] = std::min(r, std::max<std::size_t>(1, want));
    }
    // Which chunks hold recorded rays. With one source these are the leading
    // chunks, as before; with several they are the leading chunks of each
    // source's block.
    std::vector<char> recordChunk(numChunks, 0);
    std::size_t       segmentChunks = 0;
    for (std::size_t s = 0; s < nSrc; ++s) {
        if (segRays[s] == 0) continue;
        const std::size_t first = offset[s] / kChunkRays;
        const std::size_t last  = (offset[s] + segRays[s] - 1) / kChunkRays;
        for (std::size_t c = first; c <= last && c < numChunks; ++c)
            if (!recordChunk[c]) { recordChunk[c] = 1; ++segmentChunks; }
    }
    std::vector<ChunkOutput> chunkOut(numChunks);
    // The caps are budgets for the whole run, but each chunk fills its own
    // vector, so they have to be divided up front.
    const std::size_t chunkSegmentCap =
        segmentChunks ? std::max<std::size_t>(1, (opt.segmentCap + segmentChunks - 1) / segmentChunks)
                      : 0;
    const std::size_t chunkArrivalCap =
        std::max<std::size_t>(1, (opt.arrivalCap + numChunks - 1) / numChunks);

    // Progressive results. The buffers exist only when someone asked for them:
    // a snapshot is a copy of every per-thread accumulator, which at a fine
    // receiver grid is not free.
    const bool wantPartial = static_cast<bool>(ctl.partial);
    std::vector<PreviewSlot> preview(wantPartial ? threads : 0);
    std::atomic<std::uint32_t> previewEpoch{0};

    // Compensated across the per-thread grids too, in thread order: the counts
    // per cell span orders of magnitude between a focused core and its wings.
    auto reduceGrid = [](const std::vector<std::vector<double>>& src,
                         std::vector<double>& dst) {
        if (src.empty() || dst.empty()) return;
        std::vector<double> comp(dst.size(), 0.0);
        for (const auto& g : src)
            for (std::size_t i = 0; i < dst.size() && i < g.size(); ++i) {
                const double t = dst[i] + g[i];
                if (std::fabs(dst[i]) >= std::fabs(g[i])) comp[i] += (dst[i] - t) + g[i];
                else                                      comp[i] += (g[i] - t) + dst[i];
                dst[i] = t;
            }
        for (std::size_t i = 0; i < dst.size(); ++i) dst[i] += comp[i];
    };

    // Everything a finished set of accumulators becomes: buckets, per-receiver
    // grids, normalisation and the far field. Shared by the final reduction and
    // by every progressive snapshot, so a preview is the same computation on
    // less data rather than a second, differently-wrong one.
    auto finalise = [&](SimulationResult& r, const RayStats& total,
                        std::vector<double> allBins,
                        std::vector<double> bandBins,
                        std::vector<double> angleBins,
                        const std::vector<double>& detFlux,
                        const std::vector<std::size_t>& detHits,
                        const std::vector<double>& repDetIn,
                        const std::vector<double>& repEmitIn,
                        const std::vector<double>& srcDetIn) {
        r.fluxDetector     = total.fluxDetector.value();
        r.fluxAbsorbed     = total.fluxAbsorbed.value();
        r.fluxBulkAbsorbed = total.fluxBulk.value();
        r.fluxEscaped      = total.fluxEscaped.value();
        fillDiagnostics(r, total);
        r.raysHitDetector  = total.raysHit;
        r.raysEmitted      = total.raysDone;
        r.bandIrradiance   = std::move(bandBins);

        scatterDetectorGrids(scene, allBins, r);
        if (r.irradiance.empty())
            r.irradiance.assign(std::size_t(nx) * std::size_t(ny), 0.0);
        for (std::size_t i = 0; i < r.detectors.size() && i < detFlux.size(); ++i) {
            r.detectors[i].flux     = detFlux[i];
            r.detectors[i].arrivals = detHits[i];
        }

        // Normalise to the emitted source power. A partial or cancelled run is
        // normalised by the work that actually finished, so its statistics stay
        // meaningful -- which is what makes a progressive preview readable from
        // the first frame rather than only at the end.
        //
        // The divisor is the emitted *weight*, not the ray count: a photometric
        // run gives each ray V(lambda) at emission, so dividing by the count
        // would leave the total short of the lumens the user asked for.
        for (std::size_t s = 0; s < r.sources.size() && s < srcDetIn.size(); ++s)
            r.sources[s].flux = srcDetIn[s];

        const double emittedWeight = total.weightEmitted.value();
        if (r.raysEmitted > 0 && emittedWeight > 0.0) {
            const double n     = double(r.raysEmitted);
            const double scale = totalPower / emittedWeight;
            r.fluxDetector     *= scale;
            r.fluxAbsorbed     *= scale;
            r.fluxBulkAbsorbed *= scale;
            r.fluxEscaped      *= scale;
            r.fluxTruncated    *= scale;
            r.fluxRejected     *= scale;
            r.fluxRoulette     *= scale;
            r.residual.scale(scale);
            r.truncation.scale(scale);
            for (auto& v : r.irradiance)     v *= scale;
            // Squared, because it is a sum of squared deposits.
            for (auto& v : r.irradianceVar)  v *= scale * scale;
            for (auto& v : r.intensityMaster)    v *= scale;
            for (auto& v : r.intensityMasterVar) v *= scale * scale;
            for (auto& v : r.bandIrradiance) v *= scale;
            for (auto& v : angleBins)        v *= scale;
            for (auto& d : r.detectors) {
                d.flux *= scale;
                for (auto& v : d.grid) v *= scale;
            }
            for (auto& s : r.sources) s.flux *= scale;
            // Routes carry flux like every other bucket and are normalised with
            // them, so a route's share of the receiver is a share of watts and
            // not of raw ray weight.
            for (auto& sp : r.strayPaths) sp.flux *= scale;
            r.strayFluxDropped *= scale;
            r.efficiency = (totalPower > 0.0) ? (r.fluxDetector / totalPower) : 0.0;

            // The Welford moments are over per-ray *detected weight*, so the
            // error on the detected fraction divides by the mean emitted weight.
            if (n > 1.0) {
                const double meanW = emittedWeight / n;
                r.efficiencyStdErr =
                    meanW > 0.0 ? std::sqrt(total.det.variance() / n) / meanW : 0.0;
            }

            // With a low-discrepancy sequence the samples are deliberately not
            // independent, so the ray-to-ray spread above overstates the error.
            // The spread across independent scrambles is the honest measure.
            if (replicas > 1 && !repDetIn.empty()) {
                std::vector<double> repDet = repDetIn;
                int used = 0;
                double mean = 0.0;
                for (std::size_t k = 0; k < repDet.size(); ++k) {
                    if (k >= repEmitIn.size() || repEmitIn[k] <= 0.0) { repDet[k] = 0.0; continue; }
                    repDet[k] /= repEmitIn[k];
                    mean += repDet[k];
                    ++used;
                }
                if (used > 1) {
                    mean /= double(used);
                    double m2 = 0.0;
                    for (std::size_t k = 0; k < repDet.size(); ++k) {
                        if (k >= repEmitIn.size() || repEmitIn[k] <= 0.0) continue;
                        const double d = repDet[k] - mean;
                        m2 += d * d;
                    }
                    r.efficiencyStdErr = std::sqrt(m2 / (double(used) - 1.0) / double(used));
                }
            }
        }

        // A run that emitted nothing accounts for nothing. Reporting the power
        // that was *asked* for beside buckets that are all zero is an energy
        // balance that does not close, and this is reachable: cancelling
        // between the opening progress callback and the first chunk leaves
        // every worker breaking out before it traces anything. Pooled workers
        // start fast enough that it happens in practice.
        if (r.raysEmitted == 0) r.sourcePower = 0.0;

        if (angles) finishIntensityGrid(r.intensity, std::move(angleBins), nTheta, nPhi);
    };

    std::atomic<std::size_t> nextChunk{0};
    // Which chunks have finished. With chunks dealt by a fixed stride they no
    // longer complete as a prefix, so a progressive snapshot has to ask rather
    // than assume -- and asking is also what stops it reading a chunk that is
    // still being written, which counting dispatches never actually prevented.
    std::vector<std::atomic<char>> chunkDone(numChunks);
    for (auto& d : chunkDone) d.store(0, std::memory_order_relaxed);
    std::atomic<std::size_t> raysDone{0};
    std::atomic<bool>        stopped{false};

    // Progressive snapshots, driven from the calling thread only. It asks every
    // worker to publish, gives them a moment to reach a chunk boundary, and
    // reduces whatever has been published. A worker that has not published yet
    // simply contributes its previous snapshot, which is a slightly older but
    // entirely consistent view of its own work.
    auto lastPartial = std::chrono::steady_clock::now();
    // Running totals, carried across ticks so a snapshot adds one tick's worth
    // of work rather than re-reducing the whole run.
    std::vector<double>        previewGrid(wantPartial ? allCells : 0, 0.0);
    std::vector<double>        previewBand(wantPartial && spectral ? cells * 3 : 0, 0.0);
    std::vector<double>        previewAngle(wantPartial ? angles : 0, 0.0);
    std::vector<RayStats>      previewStats(wantPartial ? threads : 0);

    std::function<void(std::chrono::steady_clock::time_point, std::size_t)> emitPartial;
    if (wantPartial) {
        emitPartial = [&](std::chrono::steady_clock::time_point now, std::size_t) {
            // The interval backs off as the run gets longer. Early on the
            // picture changes fast and a tick is worth its cost; ten seconds in,
            // one tick's rays move nothing a human can see, and the copy is
            // pure overhead taken out of the trace. Doubling every two seconds
            // to a two-second ceiling.
            const double elapsed =
                std::chrono::duration<double>(now - tStart).count();
            const double growth  = 1.0 + 0.5 * elapsed;
            const long long interval = std::min<long long>(
                2000, static_cast<long long>(std::max(50, ctl.partialIntervalMs) * growth));

            const auto since =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPartial).count();
            if (since < interval) return;
            lastPartial = now;

            const std::uint32_t epoch =
                previewEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
            // The calling thread is a worker too, so it publishes its own slot
            // directly rather than waiting for itself.
            for (int spin = 0; spin < 200; ++spin) {
                bool allIn = true;
                for (unsigned t = 1; t < threads; ++t)
                    if (preview[t].published.load(std::memory_order_acquire) < epoch)
                        allIn = false;
                if (allIn) break;
                std::this_thread::yield();
            }

            // Only a slot published for *this* epoch is safe to read: a worker
            // publishes at most once per epoch, so one that has answered the
            // current one is provably not writing its buffer. A worker that has
            // not answered yet contributes the cumulative scalars from its last
            // publication, and its unpublished work arrives in the next delta --
            // nothing is lost, and nothing is counted twice.
            auto accumulate = [](std::vector<double>& into, const std::vector<double>& add) {
                const std::size_t n = std::min(into.size(), add.size());
                for (std::size_t i = 0; i < n; ++i) into[i] += add[i];
            };

            RayStats total;
            for (unsigned t = 0; t < threads; ++t) {
                PreviewSlot& slot = preview[t];
                if (slot.published.load(std::memory_order_acquire) == epoch) {
                    previewStats[t] = slot.stats;
                    // The all-receivers grid is per-thread on the small path;
                    // on the atomic path the single shared grid is read
                    // directly below, after the workers have had a moment to
                    // publish their epochs.
                    if (!atomicGrid) accumulate(previewGrid, slot.grid);
                    if (spectral) accumulate(previewBand, slot.bandGrid);
                    if (angles)   accumulate(previewAngle, slot.angleGrid);
                }
                total.add(previewStats[t]);
            }
            // The large-receiver grid is one shared atomic accumulator; snapshot
            // it by relaxing each cell. A worker may still be adding to it, but
            // an atomic load is never torn, so this is a consistent-enough view
            // for a progressive preview.
            if (atomicGrid) {
                for (std::size_t i = 0; i < allCells; ++i)
                    previewGrid[i] = atomicBins[i].load(std::memory_order_relaxed);
            }
            if (total.raysDone == 0) return;

            std::vector<double> allBins  = previewGrid;
            std::vector<double> bandBins = previewBand;
            std::vector<double> angleBins = previewAngle;

            // Per-receiver and per-replica scalars come from the chunks that
            // have finished; a chunk in flight simply is not in the sum yet.
            std::vector<double>      detFlux(dets.size(), 0.0);
            std::vector<std::size_t> detHits(dets.size(), 0);
            std::vector<double>      repDet(std::size_t(replicas), 0.0);
            std::vector<double>      repEmit(std::size_t(replicas), 0.0);
            std::vector<double>      srcDet(nSrc, 0.0);
            for (std::size_t c = 0; c < numChunks; ++c) {
                if (!chunkDone[c].load(std::memory_order_acquire)) continue;
                const ChunkOutput& co = chunkOut[c];
                for (std::size_t i = 0; i < detFlux.size() && i < co.detFlux.size(); ++i) {
                    detFlux[i] += co.detFlux[i];
                    detHits[i] += co.detHits[i];
                }
                for (std::size_t k = 0; k < repDet.size(); ++k) {
                    if (k < co.replicaDet.size())  repDet[k]  += co.replicaDet[k];
                    if (k < co.replicaEmit.size()) repEmit[k] += co.replicaEmit[k];
                }
                for (std::size_t k = 0; k < srcDet.size() && k < co.sourceDet.size(); ++k)
                    srcDet[k] += co.sourceDet[k];
            }

            SimulationResult snap;
            snap.nx = nx; snap.ny = ny;
            snap.detW = det.w; snap.detH = det.h;
            snap.detCX = det.center.dot(det.u); snap.detCY = det.center.dot(det.v);
            snap.detZ  = det.center.z;
            snap.detU  = det.u; snap.detV = det.v; snap.detN = det.n;
            snap.spectral         = spectral;
            snap.sourcePower      = totalPower;
            snap.unit             = runUnit;
            snap.luminousEfficacy = efficacy;
            snap.meanWavelengthNm = meanLambda;
            snap.partial          = true;
            snap.raysRequested    = totalRays;
            snap.sources          = out.sources;
            fillDetectorFrames(scene, snap);
            finalise(snap, total, std::move(allBins), std::move(bandBins),
                     std::move(angleBins), detFlux, detHits, repDet, repEmit, srcDet);
            snap.traceSeconds = std::chrono::duration<double>(now - tStart).count();
            ctl.partial(snap);
        };
    }

    // `reportProgress` is set for the calling thread only, so the callback is
    // never invoked concurrently.
    auto worker = [&](unsigned tid, bool reportProgress) {
        TraceContext ctx;
        ctx.scene     = &scene;
        ctx.surfs     = &surfs;
        ctx.dets      = &dets;
        ctx.grid        = atomicGrid ? nullptr : &grids[tid];
        ctx.atomicGrid  = atomicGrid ? &atomicBins : nullptr;
        ctx.bandGrid    = spectral ? &bandGrids[tid] : nullptr;
        ctx.bandCells = cells;
        ctx.bands     = spectral;
        ctx.intensity = angles ? &angleGrids[tid] : nullptr;
        ctx.intensityVar = angleVarGrids.empty() ? nullptr : &angleVarGrids[tid];
        ctx.stray     = opt.strayPaths.enabled ? &opt.strayPaths : nullptr;
        ctx.varGrid   = wantVar ? &varGrids[tid] : nullptr;
        std::size_t myChunk = std::size_t(tid);
        ctx.nTheta    = nThetaMaster;
        ctx.nPhi      = nPhi;
        ctx.physics   = opt.physics;
        ctx.estimator = opt.estimator;

        auto lastReport = std::chrono::steady_clock::now();
        // Running total of the chunks this thread has finished, so a snapshot is
        // one copy rather than a walk over every chunk in the run.
        RayStats     mine;
        std::uint32_t seenEpoch = 0;
        // What this worker had at its last publication, so the next one can be
        // expressed as a difference.
        std::vector<double> lastGrid, lastBand, lastAngle;

        // Copied back at the end of the worker, so the inner loop touches only
        // its own context.
        for (;;) {
            if (wantPartial) {
                const std::uint32_t epoch = previewEpoch.load(std::memory_order_relaxed);
                if (epoch != seenEpoch) {
                    seenEpoch = epoch;
                    PreviewSlot& slot = preview[tid];
                    slot.stats = mine;
                    // The difference since the last publication, and the new
                    // baseline, in one pass over each accumulator.
                    auto publishDelta = [](const std::vector<double>& now,
                                           std::vector<double>& last,
                                           std::vector<double>& delta) {
                        if (now.empty()) return;
                        if (last.size() != now.size()) last.assign(now.size(), 0.0);
                        delta.resize(now.size());
                        for (std::size_t i = 0; i < now.size(); ++i) {
                            delta[i] = now[i] - last[i];
                            last[i]  = now[i];
                        }
                    };
                    // The all-receivers grid publishes per-thread only on the
                    // small-receiver path; the atomic path reads the one shared
                    // grid straight from the workers at snapshot time.
                    if (!atomicGrid) publishDelta(grids[tid], lastGrid, slot.grid);
                    if (spectral) publishDelta(bandGrids[tid], lastBand, slot.bandGrid);
                    if (angles)   publishDelta(angleGrids[tid], lastAngle, slot.angleGrid);
                    // Release: everything above is visible to whoever sees this.
                    slot.published.store(epoch, std::memory_order_release);
                }
            }
            if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) {
                stopped.store(true, std::memory_order_relaxed);
                break;
            }
            // Chunks are dealt to threads by a fixed stride rather than taken
            // from a shared counter.
            //
            // The counter balanced the load and cost the pictures their
            // reproducibility: which thread's partial grid a ray landed in
            // followed whoever asked first, so two identical runs summed the
            // same numbers in a different order and the irradiance map differed
            // in its last place. The scalars never did -- they reduce per chunk
            // -- which is exactly why the discrepancy could sit there unnoticed.
            //
            // A stride costs balance only when chunk costs are correlated with
            // chunk index, and they are not: a chunk is 512 consecutive rays of
            // one source, and the expensive rays are scattered through the
            // sequence rather than gathered at one end of it.
            const std::size_t chunk = opt.deterministicGrids
                                          ? myChunk
                                          : nextChunk.fetch_add(1, std::memory_order_relaxed);
            myChunk += threads;
            if (chunk >= numChunks) break;

            const std::size_t begin = chunk * kChunkRays;
            const std::size_t end   = std::min(begin + kChunkRays, totalRays);

            ctx.stats      = &chunkStats[chunk];
            ctx.chunk      = &chunkOut[chunk];
            ctx.arrivalCap = chunkArrivalCap;
            // Seeded from the chunk index, not from the thread: which thread
            // ran the chunk must not change which arrivals it kept.
            chunkOut[chunk].arrivalRng =
                mix64(opt.seed ^ (std::uint64_t(chunk + 1) * kReservoirSalt));
            chunkOut[chunk].detFlux.assign(dets.size(), 0.0);
            chunkOut[chunk].detHits.assign(dets.size(), 0);
            chunkOut[chunk].replicaDet.assign(std::size_t(replicas), 0.0);
            chunkOut[chunk].replicaEmit.assign(std::size_t(replicas), 0.0);
            chunkOut[chunk].sourceDet.assign(nSrc, 0.0);
            for (std::size_t i = begin; i < end; ++i) {
                ctx.rng = mix64(opt.seed ^ (std::uint64_t(i) * kInteractionSalt));

                // Which source emitted this ray, and where it sits inside that
                // source own block. Everything sampled from here on is indexed
                // by the local number, so adding a second source leaves the
                // first one emitting exactly the rays it emitted alone.
                const std::size_t sIdx  = sourceOfRay(i);
                const std::size_t local = i - offset[sIdx];
                const SourceConfig& srcS = srcs[sIdx];
                ctx.source  = int(sIdx);
                ctx.emitted = emittedState[sIdx];
                ctx.initialMedia  = srcMedia[sIdx];
                ctx.initialMedium = srcMedium[sIdx];

                // Paths are kept for the leading rays *of this source*, so
                // every emitter contributes to the drawn bundle.
                const bool recordSeg = recordChunk[chunk] && local < segRays[sIdx];
                ctx.segmentCap = recordSeg ? chunkSegmentCap : 0;

                const std::size_t rep = std::min<std::size_t>(std::size_t(replicas) - 1,
                                                              local / perReplica[sIdx]);
                ctx.replica = int(rep);
                const EmittedRay ray = sampleEmission(
                    bases[sIdx], srcS, specs[sIdx], local, srcSeed[sIdx],
                    opt.estimator.lowDiscrepancy,
                    aimCenter[sIdx], aimRadius, local - rep * perReplica[sIdx],
                    std::uint32_t(mix64(srcSeed[sIdx] ^ ((rep + 1) * kEmissionSalt)) >> 32));
                // A source ray is worth power_s / rays_s, applied at emission so
                // one shared grid holds every source correctly weighted.
                const double k = kScale[sIdx];
                ctx.chunk->replicaEmit[rep] += ray.weight * k;
                ctx.lambda = ray.wavelengthNm;
                if (spectral) specs[sIdx].bandWeights(ray.wavelengthNm, ctx.bandW);
                ctx.stats->weightEmitted.add(ray.weight * k);
                // The share of the source's distribution that was aimed away
                // points at nothing at all, so it escapes by construction rather
                // than by being traced into empty space. It is booked through
                // the companion draw, which carries the direction the far field
                // needs; the difference between that estimate and the analytic
                // share goes to the residual, so the buckets still close exactly
                // and the residual still averages to nothing.
                if (ray.aimWeight < 1.0) {
                    double booked = 0.0;
                    if (ray.missed) {
                        ctx.stats->fluxEscaped.add(ray.weight * k);
                        binDirection(ctx, ray.missDir, ray.weight * k);
                        booked = ray.weight * k;
                    }
                    ctx.stats->resAiming.add(ray.weight * k * (1.0 - ray.aimWeight) - booked);
                }
                tracePath(ctx, ray.origin, ray.dir, ray.weight * k * ray.aimWeight);
            }

            chunkStats[chunk].raysDone = end - begin;
            // Release: everything this chunk wrote is visible to a snapshot
            // that sees the flag.
            chunkDone[chunk].store(1, std::memory_order_release);
            if (wantPartial) mine.add(chunkStats[chunk]);
            const std::size_t done =
                raysDone.fetch_add(end - begin, std::memory_order_relaxed) + (end - begin);

            if (reportProgress) {
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count() >= 40) {
                    lastReport = now;
                    if (ctl.progress) ctl.progress(done, totalRays);
                    if (wantPartial && emitPartial) emitPartial(now, done);
                }
            }
        }
        for (int k = 0; k < 4; ++k) threadStokes[tid][std::size_t(k)] = ctx.detStokes[k];
    };

    // The calling thread takes part in the work instead of parking on a join,
    // and the rest come from a pool the process keeps rather than from threads
    // spawned and destroyed per trace. A tolerance study of 200 samples paid
    // that spawn 200 times, against traces short enough for it to matter.
    if (ctl.progress) ctl.progress(0, totalRays);
    ThreadPool::shared().runParallel(threads, [&](unsigned tid) {
        worker(tid, tid == 0 && ctl.progress != nullptr);
    });

    // ---- reduce ------------------------------------------------------------
    // Chunk order, so the totals do not depend on which thread ran what.
    RayStats total;
    for (const auto& cs : chunkStats) total.add(cs);

    // Stray-light routes, merged in chunk order and for the same reason the
    // scalars are: a ranked list whose ranking moved with the thread count
    // would be a worse answer than no list.
    std::vector<StraySig>    mergedSig;
    std::vector<double>      mergedFlux;
    std::vector<std::size_t> mergedRays;
    std::size_t              droppedPaths = 0;
    double                   droppedFlux  = 0.0;
    if (opt.strayPaths.enabled) {
        std::unordered_map<std::uint64_t, std::vector<int>> index;
        for (const ChunkOutput& co : chunkOut) {
            for (std::size_t i = 0; i < co.pathSig.size(); ++i) {
                const StraySig& sig = co.pathSig[i];
                auto& bucket = index[sig.hash()];
                int at = -1;
                for (int j : bucket)
                    if (mergedSig[std::size_t(j)].sameAs(sig)) { at = j; break; }
                if (at < 0) {
                    if (mergedSig.size() >= opt.strayPaths.maxPaths) {
                        ++droppedPaths;
                        droppedFlux += co.pathFlux[i];
                        continue;
                    }
                    at = int(mergedSig.size());
                    bucket.push_back(at);
                    mergedSig.push_back(sig);
                    mergedFlux.push_back(0.0);
                    mergedRays.push_back(0);
                }
                mergedFlux[std::size_t(at)] += co.pathFlux[i];
                mergedRays[std::size_t(at)] += co.pathRays[i];
            }
            droppedPaths += co.pathsDropped;
            droppedFlux  += co.pathFluxDropped;
        }
    }

    std::vector<double> allBins(allCells, 0.0);
    std::vector<double> bandBins(spectral ? cells * 3 : 0, 0.0);
    std::vector<double> angleBins(angles, 0.0);
    if (atomicGrid) {
        // The shared grid already holds the converged total; read it out.
        for (std::size_t i = 0; i < allCells; ++i)
            allBins[i] = atomicBins[i].load(std::memory_order_relaxed);
    } else {
        reduceGrid(grids, allBins);
    }
    std::vector<double> varBins(wantVar ? allCells : 0, 0.0);
    if (wantVar) reduceGrid(varGrids, varBins);
    std::vector<double> angleVarBins(angleVarGrids.empty() ? 0 : angles, 0.0);
    if (!angleVarGrids.empty()) reduceGrid(angleVarGrids, angleVarBins);
    reduceGrid(bandGrids, bandBins);
    reduceGrid(angleGrids, angleBins);

    // Per-receiver and per-replica scalars, reduced in chunk order like every
    // other scalar.
    std::vector<double>      detFlux(dets.size(), 0.0);
    std::vector<std::size_t> detHits(dets.size(), 0);
    std::vector<double>      repDet(std::size_t(replicas), 0.0);
    std::vector<double>      repEmit(std::size_t(replicas), 0.0);
    std::vector<double>      srcDet(nSrc, 0.0);
    for (const auto& c : chunkOut) {
        for (std::size_t i = 0; i < detFlux.size() && i < c.detFlux.size(); ++i) {
            detFlux[i] += c.detFlux[i];
            detHits[i] += c.detHits[i];
        }
        for (std::size_t k = 0; k < repDet.size(); ++k) {
            if (k < c.replicaDet.size())  repDet[k]  += c.replicaDet[k];
            if (k < c.replicaEmit.size()) repEmit[k] += c.replicaEmit[k];
        }
        for (std::size_t k = 0; k < srcDet.size() && k < c.sourceDet.size(); ++k)
            srcDet[k] += c.sourceDet[k];
    }

    std::size_t segs = 0, arrs = 0;
    for (const auto& c : chunkOut) { segs += c.segments.size(); arrs += c.arrivals.size(); }
    out.raySegments.reserve(segs);
    out.arrivals.reserve(arrs);
    for (const auto& c : chunkOut) {
        out.raySegments.insert(out.raySegments.end(), c.segments.begin(), c.segments.end());
        out.arrivals.insert(out.arrivals.end(), c.arrivals.begin(), c.arrivals.end());
    }

    // Ranked by what each route delivers, which is the order a stray-light
    // review reads. Ties break on the route itself so the ranking is total: two
    // routes carrying identical flux must not swap places between runs.
    if (opt.strayPaths.enabled) {
        out.strayPaths.reserve(mergedSig.size());
        for (std::size_t i = 0; i < mergedSig.size(); ++i) {
            StrayPath sp;
            sp.ids.assign(mergedSig[i].e, mergedSig[i].e + mergedSig[i].n);
            sp.flux      = mergedFlux[i];
            sp.rays      = mergedRays[i];
            sp.truncated = mergedSig[i].trunc;
            out.strayPaths.push_back(std::move(sp));
        }
        std::sort(out.strayPaths.begin(), out.strayPaths.end(),
                  [](const StrayPath& a, const StrayPath& b) {
                      if (a.flux != b.flux) return a.flux > b.flux;
                      if (a.ids.size() != b.ids.size()) return a.ids.size() < b.ids.size();
                      return a.ids < b.ids;
                  });
        out.strayPathsDropped = droppedPaths;
        out.strayFluxDropped  = droppedFlux;
        out.straySurfaceLabels = scene.surfaceLabels();
        out.straySetNames      = opt.strayPaths.setNames;
    }

    // The squares scale with the square of the flux normalisation, and the
    // grid holds every receiver end to end while `irradiance` is the first one.
    // Left unscaled here on purpose: finalise() owns the flux normalisation and
    // applies it to this alongside the grid, squared, so the two cannot drift
    // apart. Scaling it here meant reading a ray count finalise had not set yet.
    if (wantVar) {
        out.irradianceVar.assign(std::size_t(nx) * std::size_t(ny), 0.0);
        for (std::size_t i = 0; i < out.irradianceVar.size() && i < varBins.size(); ++i)
            out.irradianceVar[i] = varBins[i];
    }
    // The master far field, kept before finishIntensityGrid folds it into
    // adaptive rings. Unscaled here for the same reason the receiver's variance
    // is: finalise() owns the flux normalisation.
    if (!angleVarBins.empty()) {
        out.intensityMasterTheta = nThetaMaster;
        out.intensityMasterPhi   = nPhi;
        out.intensityMaster      = angleBins;
        out.intensityMasterVar   = angleVarBins;
    }

    out.cancelled = stopped.load(std::memory_order_relaxed);
    finalise(out, total, std::move(allBins), std::move(bandBins), std::move(angleBins),
             detFlux, detHits, repDet, repEmit, srcDet);

    if (opt.physics.polarised) {
        polarisation::Stokes arrived;
        arrived.i = 0.0; arrived.q = 0.0; arrived.u = 0.0; arrived.v = 0.0;
        for (const auto& t : threadStokes) {
            arrived.i += t[0];
            arrived.q += t[1];
            arrived.u += t[2];
            arrived.v += t[3];
        }
        out.polarised = true;
        out.degreeOfPolarisation = arrived.degree();
        out.degreeLinear         = arrived.degreeLinear();
        out.polarisationAngleDeg = arrived.angle() / kDegRad;
    }

    out.traceSeconds = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - tStart).count();
    if (ctl.progress) ctl.progress(out.raysEmitted, totalRays);
}
