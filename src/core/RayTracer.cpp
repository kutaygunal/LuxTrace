#include "RayTracer.h"
#include "Optics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {

using namespace optics;

constexpr double kEps      = 1e-6;   // hit-distance / surface-offset epsilon
constexpr double kCut      = 1e-4;   // energy cutoff
constexpr int    kMaxDepth = 160;    // max interactions per path (TIR guides need many)

// A path is a binary tree (reflected + transmitted branch), walked depth first,
// so the pending stack never exceeds one entry per depth level.
constexpr int kPathStack = kMaxDepth + 8;

// Rays are handed out in chunks so threads stay busy on wildly uneven paths
// (a TIR bounce chain costs orders of magnitude more than an escaping ray)
// while cancellation still responds within a few milliseconds.
constexpr std::size_t kChunkRays = 512;

// Distinct constants so the emission stream and the interaction stream of the
// same ray index never coincide.
constexpr std::uint64_t kEmissionSalt    = 0xD1B54A32D192ED03ull;
constexpr std::uint64_t kInteractionSalt = 0x9E6C63D0676A9A99ull;

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

EmittedRay sampleEmission(const EmissionBasis& basis, const SourceConfig& src,
                          std::size_t index, std::uint64_t seed) {
    // The ray index is hashed into the stream state rather than used as a
    // stride. Striding by splitmix64's own increment would make each ray's
    // second draw identical to the next ray's first draw, correlating the
    // azimuth of every ray with the polar angle of the one after it.
    std::uint64_t state = mix64(seed ^ (std::uint64_t(index) * kEmissionSalt));
    const double u1 = uniform01(state);   // polar
    const double u2 = uniform01(state);   // azimuth
    const double u3 = uniform01(state);   // emitting area
    const double u4 = uniform01(state);

    EmittedRay ray;
    ray.wavelengthNm = (src.spectrum == SourceConfig::Spectrum::Rgb)
                           ? kBandNm[index % 3]
                           : src.wavelengthNm;

    const Vec3 centre(src.origin);

    if (src.shape == SourceConfig::Shape::Sphere && src.sizeA > 0.0) {
        // A real spherical emitter: pick a point on the surface, then emit about
        // the outward normal there. Etendue comes out right, which is what stops
        // a concentrator from reporting an impossible gain.
        const double cz  = 1.0 - 2.0 * u3;
        const double sz  = std::sqrt(std::max(0.0, 1.0 - cz * cz));
        const double phi = kTwoPi * u4;
        const Vec3 nrm(sz * std::cos(phi), sz * std::sin(phi), cz);
        ray.origin = centre + nrm * src.sizeA;
        ray.dir    = sampleAngular(nrm, src.type, src.halfAngleDeg, u1, u2);
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
    ray.dir    = sampleAngular(basis.axis, src.type, src.halfAngleDeg, u1, u2);
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
    double      fluxDetector  = 0.0;
    double      fluxAbsorbed  = 0.0;
    double      fluxBulk      = 0.0;
    double      fluxEscaped   = 0.0;
    double      fluxTruncated = 0.0;
    // Per-ray detector energy and its square, for the Monte Carlo error bar.
    double      sumDet        = 0.0;
    double      sumDet2       = 0.0;
    std::size_t raysHit       = 0;
    std::size_t raysDone      = 0;

    void add(const RayStats& o) {
        fluxDetector  += o.fluxDetector;
        fluxAbsorbed  += o.fluxAbsorbed;
        fluxBulk      += o.fluxBulk;
        fluxEscaped   += o.fluxEscaped;
        fluxTruncated += o.fluxTruncated;
        sumDet        += o.sumDet;
        sumDet2       += o.sumDet2;
        raysHit       += o.raysHit;
        raysDone      += o.raysDone;
    }
};

// Diagram segments and receiver arrivals are collected per chunk so they can be
// concatenated in ray order afterwards, whichever thread ran the chunk.
struct ChunkOutput {
    std::vector<RaySegment>      segments;
    // Parent of each segment within this chunk (-1 for the emitted leg). A path
    // is a tree, so marking the legs that reached the receiver means walking
    // back up from the arrival rather than knowing it on the way down.
    std::vector<int>             parent;
    std::vector<DetectorArrival> arrivals;
};

// One branch of a path still waiting to be traced.
struct PathState {
    Vec3   o, d;
    double energy = 0.0;
    // Surface index of the medium the ray is inside, -1 for vacuum. It is the
    // old inside/outside flag plus the identity of what it is inside, which is
    // what lets the bulk absorption and the dispersion of *that* glass be looked
    // up on the way out.
    int    medium = -1;
    int    depth  = 0;
    int    seg    = -1;   // segment index this branch grew from, -1 if unrecorded
};

// Everything the tracer needs that does not change from ray to ray. This
// replaces the fifteen-parameter recursive signature the tracer used to carry.
struct TraceContext {
    const TraceScene*    scene      = nullptr;
    const DetectorInfo*  det        = nullptr;
    std::vector<double>* grid       = nullptr;  // per-thread irradiance bins
    std::vector<double>* bandGrid   = nullptr;  // per-thread 3 x irradiance bins
    std::vector<double>* intensity  = nullptr;  // per-thread far-field bins
    RayStats*            stats      = nullptr;  // per-chunk scalar totals
    ChunkOutput*         chunk      = nullptr;
    std::size_t          segmentCap = 0;
    std::size_t          arrivalCap = 0;
    PhysicsOptions       physics;
    int                  nTheta = 0, nPhi = 0;
    // Per-ray state.
    std::uint64_t rng    = 0;
    double        lambda = 587.6;
    int           band   = -1;   // 0..2 on a spectral run, -1 otherwise
};

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
    (*ctx.intensity)[std::size_t(it) * std::size_t(ctx.nPhi) + std::size_t(ip)] += energy;
}

// Refractive index of the medium a branch is travelling through, at this
// wavelength.
inline double mediumIndex(const TraceContext& ctx, int medium) {
    if (medium < 0) return 1.0;
    const SceneSurface& m = ctx.scene->surfaces()[std::size_t(medium)];
    if (m.index <= 0.0) return 1.0;
    return ctx.physics.dispersion ? cauchyIndex(m.index, m.dispersionB, ctx.lambda)
                                  : m.index;
}

void tracePath(TraceContext& ctx, const Vec3& origin, const Vec3& dir, double energy) {
    const TraceScene&     scene = *ctx.scene;
    const DetectorInfo&   det   = *ctx.det;
    RayStats&             ts    = *ctx.stats;
    std::vector<double>&  grid  = *ctx.grid;
    const PhysicsOptions& phys  = ctx.physics;

    double rayDetector = 0.0;   // this ray's total contribution to the receiver

    // Explicit stack instead of recursion: a refractive hit spawns both a
    // reflected and a transmitted branch, and a light guide can reach the depth
    // limit, so the recursion would run 160 frames deep per branch.
    PathState stack[kPathStack];
    int sp = 0;
    stack[sp++] = PathState{origin, dir, energy, -1, 0, -1};

    // Pushes a branch, or books its energy as truncated if it cannot be taken.
    auto push = [&](const Vec3& o, const Vec3& d, double e, int medium, int depth, int seg) {
        if (sp + 1 < kPathStack) stack[sp++] = PathState{o, d, e, medium, depth, seg};
        else                     ts.fluxTruncated += e;
    };

    while (sp > 0) {
        const PathState s = stack[--sp];

        if (s.energy < kCut || s.depth > kMaxDepth) {
            ts.fluxTruncated += s.energy;
            continue;
        }

        RayHit h;
        if (!scene.nearestHit(s.o, s.d, h, kEps)) {
            ts.fluxEscaped += s.energy;
            binDirection(ctx, s.d, s.energy);
            continue;
        }

        const SceneTri&     tri  = scene.triangles()[std::size_t(h.tri)];
        const SceneSurface& surf = scene.surfaceOf(tri);
        const Vec3 p = s.o + s.d * h.t;

        // Beer-Lambert bulk attenuation over the leg just travelled. This is the
        // loss that scales with path length rather than with hit count, so it is
        // what makes a long light guide cost more than a short one.
        double e = s.energy;
        if (phys.absorption && s.medium >= 0) {
            const double alpha = scene.surfaces()[std::size_t(s.medium)].absorption
                                 * phys.absorptionScale;
            if (alpha > 0.0) {
                const double survived = e * std::exp(-alpha * h.t);
                const double lost     = e - survived;
                ts.fluxAbsorbed += lost;
                ts.fluxBulk     += lost;
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
            ctx.chunk->segments.push_back(rs);
            ctx.chunk->parent.push_back(s.seg);
        }

        if (e < kCut) { ts.fluxTruncated += e; continue; }

        if (surf.isDetector) {
            if (det.w > 0.0 && det.h > 0.0) {
                const double lx = p.x - (det.center.x - det.w * 0.5);
                const double ly = p.y - (det.center.y - det.h * 0.5);
                const int bx = int(lx / det.w * det.nx);
                const int by = int(ly / det.h * det.ny);
                if (bx >= 0 && bx < det.nx && by >= 0 && by < det.ny) {
                    const std::size_t cell =
                        std::size_t(by) * std::size_t(det.nx) + std::size_t(bx);
                    grid[cell] += e;
                    if (ctx.bandGrid && ctx.band >= 0)
                        (*ctx.bandGrid)[std::size_t(ctx.band) * grid.size() + cell] += e;
                }
            }
            ts.fluxDetector += e;
            rayDetector     += e;
            ++ts.raysHit;
            binDirection(ctx, s.d, e);

            if (ctx.chunk && ctx.chunk->arrivals.size() < ctx.arrivalCap) {
                DetectorArrival a;
                a.p            = p;
                a.d            = s.d;
                a.energy       = float(e);
                a.wavelengthNm = float(ctx.lambda);
                ctx.chunk->arrivals.push_back(a);
            }
            // Mark every leg back to the emitter, so the viewer can show only
            // the rays that actually delivered energy.
            for (int k = segIdx; k >= 0; k = ctx.chunk->parent[std::size_t(k)])
                ctx.chunk->segments[std::size_t(k)].flags |= RaySegment::ReachedDetector;
            continue;
        }

        // Orient the surface normal toward the incident ray, then apply the
        // polish tolerance to it. Perturbing the normal rather than the outgoing
        // ray gives reflection its factor of two for free and keeps refraction
        // consistent with it.
        Vec3 n = tri.n;
        if (s.d.dot(n) > 0.0) n = -n;
        if (!n.normalize()) { ts.fluxAbsorbed += e; continue; }
        const double surfRough = phys.roughnessOverride >= 0.0 ? phys.roughnessOverride
                                                                : surf.roughness;
        const double surfScatter = phys.scatterOverride >= 0.0 ? phys.scatterOverride
                                                               : surf.scatter;
        if (phys.roughness && surfRough > 0.0) {
            const Vec3 rough = perturbNormal(n, surfRough, ctx.rng);
            // A slope error large enough to tip the normal past the ray would put
            // the outgoing ray inside the surface; there, keep the smooth normal.
            if (rough.dot(s.d) < 0.0) n = rough;
        }

        const bool   refracts = surf.index > 0.0;
        const double cosI     = std::clamp(-s.d.dot(n), 0.0, 1.0);

        double reflE = 0.0, transE = 0.0;
        double n1 = 1.0, n2 = 1.0;
        bool   leaving = false;
        bool   tir     = false;

        if (refracts) {
            const double nSurf = phys.dispersion
                                     ? cauchyIndex(surf.index, surf.dispersionB, ctx.lambda)
                                     : surf.index;
            // Any transmission while already inside a medium is an exit to
            // vacuum. Comparing the medium's identity to the surface hit would
            // be sharper for genuinely nested solids, but it would also mean a
            // body modelled as two surfaces (an entrance sheet and an exit
            // sheet) could never refract out of itself -- and that is how a
            // half-space is set up when one wants an exact angle of incidence.
            leaving = (s.medium >= 0);
            n1 = mediumIndex(ctx, s.medium);
            n2 = leaving ? 1.0 : nSurf;

            const double eta   = n1 / n2;
            const double sinT2 = eta * eta * (1.0 - cosI * cosI);
            tir = (sinT2 >= 1.0);

            if (phys.fresnel && surf.fresnel) {
                // Angle-dependent split: ~4 % at normal incidence for air/glass,
                // rising to 1 at grazing, and exactly 1 beyond the critical
                // angle. No energy is lost at the interface itself -- surface
                // absorption is a coating property, and bulk loss is Beer-Lambert.
                const double R = fresnelReflectance(n1, n2, cosI);
                reflE  = e * R;
                transE = e - reflE;
            } else {
                reflE  = e * surf.reflectivity;
                transE = e * surf.transmissivity;
                ts.fluxAbsorbed += e - reflE - transE;
            }
        } else {
            reflE = e * surf.reflectivity;
            ts.fluxAbsorbed += e - reflE;
        }

        // Whether an outgoing branch scatters is decided by a coin flip rather
        // than by splitting it in two. Splitting would double the path tree at
        // every diffuse bounce, which an integrating sphere cannot afford; the
        // roulette is unbiased and keeps the tree binary.
        const bool canScatter = phys.scattering && surfScatter > 0.0;

        // Refraction (Snell), with total internal reflection. Pushed first so it
        // is popped last, matching the order the recursive tracer visited
        // branches in.
        if (refracts) {
            if (transE < kCut) {
                ts.fluxTruncated += transE;
            } else if (tir) {
                // Total internal reflection: stays in the same medium.
                Vec3 reD = reflect(s.d, n);
                if (canScatter && uniform01(ctx.rng) < surfScatter)
                    reD = cosineHemisphere(n, uniform01(ctx.rng), uniform01(ctx.rng));
                if (reD.normalize()) push(p + reD * kEps, reD, transE, s.medium, s.depth + 1, segIdx);
                else                 ts.fluxTruncated += transE;
            } else {
                const int newMedium = leaving ? -1 : tri.surf;
                Vec3 tD;
                if (refract(s.d, n, n1 / n2, cosI, tD)) {
                    if (canScatter && uniform01(ctx.rng) < surfScatter) {
                        // A transmissive diffuser re-emits cosine-weighted about
                        // the far side of the surface.
                        tD = cosineHemisphere(-n, uniform01(ctx.rng), uniform01(ctx.rng));
                    }
                    push(p + tD * kEps, tD, transE, newMedium, s.depth + 1, segIdx);
                } else {
                    ts.fluxTruncated += transE;
                }
            }
        }

        // Specular (or diffuse) reflection, staying in the current medium.
        if (reflE < kCut) {
            ts.fluxTruncated += reflE;
        } else {
            Vec3 refl = reflect(s.d, n);
            if (canScatter && uniform01(ctx.rng) < surfScatter)
                refl = cosineHemisphere(n, uniform01(ctx.rng), uniform01(ctx.rng));
            if (refl.normalize()) push(p + refl * kEps, refl, reflE, s.medium, s.depth + 1, segIdx);
            else                  ts.fluxTruncated += reflE;
        }
    }

    ts.sumDet  += rayDetector;
    ts.sumDet2 += rayDetector * rayDetector;
}

// ---- far-field reduction ---------------------------------------------------

void finishIntensity(IntensityGrid& g, std::vector<double> bins, int nTheta, int nPhi) {
    g.nTheta = nTheta;
    g.nPhi   = nPhi;
    g.bin    = std::move(bins);
    g.perSteradian.assign(g.bin.size(), 0.0);
    g.profile.assign(std::size_t(nTheta), 0.0);
    g.totalFlux = 0.0;

    const double dPhi   = kTwoPi / double(nPhi);
    const double dTheta = kPi / double(nTheta);
    for (int it = 0; it < nTheta; ++it) {
        // Solid angle of one (theta, phi) cell: dPhi * (cos t0 - cos t1). Without
        // this the poles would read as dark simply because their cells are small.
        const double t0    = double(it) * dTheta;
        const double t1    = double(it + 1) * dTheta;
        const double omega = dPhi * (std::cos(t0) - std::cos(t1));
        double ringSum = 0.0;
        for (int ip = 0; ip < nPhi; ++ip) {
            const std::size_t k = std::size_t(it) * std::size_t(nPhi) + std::size_t(ip);
            g.totalFlux += g.bin[k];
            if (omega > 1e-15) g.perSteradian[k] = g.bin[k] / omega;
            ringSum += g.perSteradian[k];
        }
        g.profile[std::size_t(it)] = ringSum / double(nPhi);
    }

    int peakBin = 0;
    for (int it = 0; it < nTheta; ++it)
        if (g.profile[std::size_t(it)] > g.profile[std::size_t(peakBin)]) peakBin = it;
    g.peak = g.profile[std::size_t(peakBin)];

    // Full width at half maximum, walking out from the peak and interpolating the
    // crossing so the answer is not quantised to the bin width.
    g.fwhmDeg = 0.0;
    if (g.peak > 0.0) {
        const double half = 0.5 * g.peak;
        auto edge = [&](int step) {
            int i = peakBin;
            while (i + step >= 0 && i + step < nTheta &&
                   g.profile[std::size_t(i + step)] >= half) i += step;
            const int j = i + step;
            double a = g.thetaCenterDeg(i);
            if (j >= 0 && j < nTheta) {
                const double vi = g.profile[std::size_t(i)];
                const double vj = g.profile[std::size_t(j)];
                if (vi > vj) a += (g.thetaCenterDeg(j) - a) * (vi - half) / (vi - vj);
            }
            return a;
        };
        g.fwhmDeg = std::fabs(edge(+1) - edge(-1));
    }
}

} // namespace

EmittedRay RayTracer::sampleRay(const SourceConfig& src, std::size_t index,
                                std::uint64_t seed) {
    const EmissionBasis basis(src.axis);
    return sampleEmission(basis, src, index, seed);
}

Vec3 RayTracer::sampleDirection(const SourceConfig& src, std::size_t index,
                                std::uint64_t seed) {
    return sampleRay(src, index, seed).dir;
}

void RayTracer::traceSingleRay(const TraceScene& scene, const Vec3& origin,
                               const Vec3& dir, SimulationResult& out, double energy,
                               const PhysicsOptions& physics, double wavelengthNm) {
    const DetectorInfo& det = scene.detector();
    DetectorInfo localDet = det;
    localDet.nx = (det.valid && det.nx > 0) ? det.nx : 1;
    localDet.ny = (det.valid && det.ny > 0) ? det.ny : 1;

    out = SimulationResult{};
    out.nx    = localDet.nx;
    out.ny    = localDet.ny;
    out.detW  = det.w;
    out.detH  = det.h;
    out.detCX = det.center.x;
    out.detCY = det.center.y;
    out.detZ  = det.center.z;
    out.irradiance.assign(std::size_t(localDet.nx) * std::size_t(localDet.ny), 0.0);
    out.sourcePower = energy;
    out.raysEmitted = 1;

    RayStats stats;
    std::vector<double> grid(out.irradiance.size(), 0.0);
    ChunkOutput chunk;

    TraceContext ctx;
    ctx.scene      = &scene;
    ctx.det        = &localDet;
    ctx.grid       = &grid;
    ctx.stats      = &stats;
    ctx.chunk      = &chunk;
    ctx.segmentCap = 4096;
    ctx.arrivalCap = 4096;
    ctx.physics    = physics;
    ctx.lambda     = wavelengthNm;
    ctx.rng        = mix64(kInteractionSalt);

    Vec3 d = dir;
    if (!d.normalize()) { out.fluxEscaped = energy; return; }
    tracePath(ctx, origin, d, energy);

    out.fluxDetector     = stats.fluxDetector;
    out.fluxAbsorbed     = stats.fluxAbsorbed;
    out.fluxBulkAbsorbed = stats.fluxBulk;
    out.fluxEscaped      = stats.fluxEscaped;
    out.fluxTruncated    = stats.fluxTruncated;
    out.raysHitDetector  = stats.raysHit;
    out.irradiance       = grid;
    out.raySegments      = std::move(chunk.segments);
    out.arrivals         = std::move(chunk.arrivals);
    out.efficiency       = (energy > 0.0) ? (out.fluxDetector / energy) : 0.0;
}

void RayTracer::trace(const MeshList& meshes, const SourceConfig& src,
                      SimulationResult& out, const TraceOptions& opt,
                      const TraceControl& ctl) {
    TraceScene scene;
    scene.build(meshes);
    trace(scene, src, out, opt, ctl);
}

void RayTracer::trace(const TraceScene& scene, const SourceConfig& src,
                      SimulationResult& out, const TraceOptions& opt,
                      const TraceControl& ctl) {
    const auto tStart = std::chrono::steady_clock::now();

    const DetectorInfo& det = scene.detector();
    const int  nx       = (det.valid && det.nx > 0) ? det.nx : 1;
    const int  ny       = (det.valid && det.ny > 0) ? det.ny : 1;
    const bool spectral = (src.spectrum == SourceConfig::Spectrum::Rgb);
    const int  nTheta   = std::max(0, opt.nTheta);
    const int  nPhi     = nTheta > 0 ? std::max(1, opt.nPhi) : 0;

    out = SimulationResult{};
    out.nx = nx; out.ny = ny;
    out.detW  = det.w;
    out.detH  = det.h;
    out.detCX = det.center.x;
    out.detCY = det.center.y;
    out.detZ  = det.center.z;
    out.spectral = spectral;
    out.irradiance.assign(std::size_t(nx) * std::size_t(ny), 0.0);
    if (spectral) out.bandIrradiance.assign(out.irradiance.size() * 3, 0.0);
    out.sourcePower = src.power;

    const std::size_t totalRays = src.rays > 0 ? std::size_t(src.rays) : 0;
    if (totalRays == 0 || scene.empty()) {
        // Nothing to intersect: all emitted energy escapes.
        out.fluxEscaped = (totalRays > 0) ? src.power : 0.0;
        out.raysEmitted = totalRays;
        if (ctl.progress) ctl.progress(totalRays, totalRays);
        out.traceSeconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - tStart).count();
        return;
    }

    // A detector frame is kept even when the scene has no detector, so the
    // tracer never has to branch on validity in the inner loop.
    DetectorInfo localDet = det;
    localDet.nx = nx;
    localDet.ny = ny;

    const EmissionBasis basis(src.axis);

    const std::size_t numChunks = (totalRays + kChunkRays - 1) / kChunkRays;
    unsigned threads = opt.threads;
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = unsigned(std::max<std::size_t>(1, std::min<std::size_t>(threads, numChunks)));

    // Irradiance and far-field bins stay per thread (a per-chunk grid would cost
    // tens of KB per chunk); the scalar totals are per chunk so they reduce in a
    // fixed order.
    const std::size_t cells  = std::size_t(nx) * std::size_t(ny);
    const std::size_t angles = std::size_t(nTheta) * std::size_t(nPhi);
    std::vector<std::vector<double>> grids(threads);
    std::vector<std::vector<double>> bandGrids(spectral ? threads : 0);
    std::vector<std::vector<double>> angleGrids(angles ? threads : 0);
    for (auto& g : grids)      g.assign(cells, 0.0);
    for (auto& g : bandGrids)  g.assign(cells * 3, 0.0);
    for (auto& g : angleGrids) g.assign(angles, 0.0);
    std::vector<RayStats> chunkStats(numChunks);

    // Path recording is limited to the leading chunks (it is a tree walk per
    // hit); arrivals are one push_back each, so they are recorded run-wide and
    // bounded by their own budget instead.
    const std::size_t segmentChunks =
        std::min(numChunks, (opt.segmentRays + kChunkRays - 1) / kChunkRays);
    std::vector<ChunkOutput> chunkOut(numChunks);
    // The caps are budgets for the whole run, but each chunk fills its own
    // vector, so they have to be divided up front.
    const std::size_t chunkSegmentCap =
        segmentChunks ? std::max<std::size_t>(1, (opt.segmentCap + segmentChunks - 1) / segmentChunks)
                      : 0;
    const std::size_t chunkArrivalCap =
        std::max<std::size_t>(1, (opt.arrivalCap + numChunks - 1) / numChunks);

    std::atomic<std::size_t> nextChunk{0};
    std::atomic<std::size_t> raysDone{0};
    std::atomic<bool>        stopped{false};

    // `reportProgress` is set for the calling thread only, so the callback is
    // never invoked concurrently.
    auto worker = [&](unsigned tid, bool reportProgress) {
        TraceContext ctx;
        ctx.scene     = &scene;
        ctx.det       = &localDet;
        ctx.grid      = &grids[tid];
        ctx.bandGrid  = spectral ? &bandGrids[tid] : nullptr;
        ctx.intensity = angles ? &angleGrids[tid] : nullptr;
        ctx.nTheta    = nTheta;
        ctx.nPhi      = nPhi;
        ctx.physics   = opt.physics;

        auto lastReport = std::chrono::steady_clock::now();

        for (;;) {
            if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) {
                stopped.store(true, std::memory_order_relaxed);
                break;
            }
            const std::size_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= numChunks) break;

            const std::size_t begin = chunk * kChunkRays;
            const std::size_t end   = std::min(begin + kChunkRays, totalRays);

            ctx.stats      = &chunkStats[chunk];
            ctx.chunk      = &chunkOut[chunk];
            ctx.arrivalCap = chunkArrivalCap;
            for (std::size_t i = begin; i < end; ++i) {
                const bool recordSeg = (chunk < segmentChunks) && (i < opt.segmentRays);
                ctx.segmentCap = recordSeg ? chunkSegmentCap : 0;
                ctx.rng        = mix64(opt.seed ^ (std::uint64_t(i) * kInteractionSalt));

                const EmittedRay ray = sampleEmission(basis, src, i, opt.seed);
                ctx.lambda = ray.wavelengthNm;
                ctx.band   = spectral ? int(i % 3) : -1;
                tracePath(ctx, ray.origin, ray.dir, 1.0);
            }

            chunkStats[chunk].raysDone = end - begin;
            const std::size_t done =
                raysDone.fetch_add(end - begin, std::memory_order_relaxed) + (end - begin);

            if (reportProgress) {
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReport).count() >= 40) {
                    lastReport = now;
                    ctl.progress(done, totalRays);
                }
            }
        }
    };

    // The calling thread takes part in the work instead of parking on a join.
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker, t, false);

    if (ctl.progress) ctl.progress(0, totalRays);
    worker(0, ctl.progress != nullptr);

    for (auto& t : pool) t.join();

    // ---- reduce ------------------------------------------------------------
    // Chunk order, so the totals do not depend on which thread ran what.
    RayStats total;
    for (const auto& cs : chunkStats) total.add(cs);
    out.fluxDetector     = total.fluxDetector;
    out.fluxAbsorbed     = total.fluxAbsorbed;
    out.fluxBulkAbsorbed = total.fluxBulk;
    out.fluxEscaped      = total.fluxEscaped;
    out.fluxTruncated    = total.fluxTruncated;
    out.raysHitDetector  = total.raysHit;
    out.raysEmitted      = total.raysDone;

    for (const auto& g : grids)
        for (std::size_t i = 0; i < cells; ++i) out.irradiance[i] += g[i];
    for (const auto& g : bandGrids)
        for (std::size_t i = 0; i < g.size(); ++i) out.bandIrradiance[i] += g[i];

    std::vector<double> angleBins(angles, 0.0);
    for (const auto& g : angleGrids)
        for (std::size_t i = 0; i < angles; ++i) angleBins[i] += g[i];

    std::size_t segs = 0, arrs = 0;
    for (const auto& c : chunkOut) { segs += c.segments.size(); arrs += c.arrivals.size(); }
    out.raySegments.reserve(segs);
    out.arrivals.reserve(arrs);
    for (const auto& c : chunkOut) {
        out.raySegments.insert(out.raySegments.end(), c.segments.begin(), c.segments.end());
        out.arrivals.insert(out.arrivals.end(), c.arrivals.begin(), c.arrivals.end());
    }

    out.cancelled = stopped.load(std::memory_order_relaxed);

    // Normalise to the emitted source power. A cancelled run is normalised by
    // the rays that actually finished, so its statistics stay meaningful.
    if (out.raysEmitted > 0) {
        const double n     = double(out.raysEmitted);
        const double scale = src.power / n;
        out.fluxDetector     *= scale;
        out.fluxAbsorbed     *= scale;
        out.fluxBulkAbsorbed *= scale;
        out.fluxEscaped      *= scale;
        out.fluxTruncated    *= scale;
        for (auto& v : out.irradiance)     v *= scale;
        for (auto& v : out.bandIrradiance) v *= scale;
        for (auto& v : angleBins)          v *= scale;
        out.efficiency = (src.power > 0.0) ? (out.fluxDetector / src.power) : 0.0;

        // Monte Carlo error on the mean: the ray-to-ray standard deviation over
        // sqrt(N). Rays carry unit energy here, so this is the error on the
        // detected fraction directly.
        if (n > 1.0) {
            const double mean = total.sumDet / n;
            const double var  = std::max(0.0, total.sumDet2 / n - mean * mean);
            out.efficiencyStdErr = std::sqrt(var / n);
        }
    }

    if (angles) finishIntensity(out.intensity, std::move(angleBins), nTheta, nPhi);

    out.traceSeconds = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - tStart).count();
    if (ctl.progress) ctl.progress(out.raysEmitted, totalRays);
}
