#pragma once
#include "Mesh.h"
#include "SimulationResult.h"
#include "TraceScene.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

// Emission configuration for one run.
//
// The three axes are independent: an angular law (`type`), an emitting area
// (`shape` + size), and a spectrum. A real LED die is Lambertian over a
// rectangle; a laser is collimated over a disc; the textbook point source is
// isotropic over nothing at all.
struct SourceConfig {
    // Angular law about `axis`.
    //   Point       uniform over solid angle inside the cone
    //   Lambertian  cosine-weighted inside the cone
    //   Collimated  every ray parallel to `axis`
    enum class Type { Point, Lambertian, Collimated };

    // Emitting area. Anything but PointLike gives the source a real etendue,
    // which is what stops a concentrator from reporting an impossible gain.
    enum class Shape { PointLike, Disc, Rect, Sphere };

    enum class Spectrum { Monochrome, Rgb };

    Type   type   = Type::Point;
    Shape  shape  = Shape::PointLike;
    gp_Pnt origin{0, 0, 0};
    gp_Dir axis{0, 0, 1};   // emission axis (hemisphere normal for Lambertian)
    int    rays   = 100000;
    double power  = 1.0;

    // Emission cone half-angle about `axis`, degrees. Clamped to 180 for Point
    // and to 90 for Lambertian, so the defaults reproduce the full sphere and
    // the full hemisphere respectively. Ignored by Collimated.
    double halfAngleDeg = 180.0;

    // Emitter size, mm. Disc and Sphere use `sizeA` as the radius; Rect uses
    // sizeA x sizeB about `origin`, in the plane normal to `axis`.
    double sizeA = 0.0, sizeB = 0.0;

    // Radius of the parallel bundle, mm. Collimated only.
    double beamRadius = 25.0;

    Spectrum spectrum     = Spectrum::Monochrome;
    double   wavelengthNm = 587.6;   // d line; also the line the indices are quoted at
};

// One emitted ray: where it starts, where it goes, and what colour it is.
struct EmittedRay {
    Vec3   origin;
    Vec3   dir;
    double wavelengthNm = 587.6;
};

// Global switches over the per-surface optical properties, so the same scene can
// be traced with and without an effect and the two compared. Each one only gates
// a property that a surface has to opt into anyway -- a surface with
// `scatter == 0` costs nothing whether scattering is enabled or not.
struct PhysicsOptions {
    bool fresnel    = true;   // angle-dependent R/T at refractive surfaces
    bool absorption = true;   // Beer-Lambert bulk attenuation
    bool scattering = true;   // diffuse scatter fraction
    bool roughness  = true;   // surface slope error
    bool dispersion = true;   // Cauchy n(lambda); only bites on a spectral run

    // Scene-wide overrides, so roughness and scatter can be explored without
    // rebuilding geometry -- they are ray-time properties, and re-tessellating
    // an optic to change its polish would be absurd. Negative means "leave each
    // surface's own value alone". The receiver is never affected.
    double roughnessOverride = -1.0;   // radians
    double scatterOverride   = -1.0;   // 0..1
    double absorptionScale   = 1.0;    // multiplies every medium's alpha
};

// Knobs that trade run time against fidelity of the diagram overlay.
struct TraceOptions {
    // 0 == use every hardware thread. Results do not depend on this value.
    unsigned      threads     = 0;
    // Only the first `segmentRays` rays have their paths recorded; the diagram
    // subsamples anyway, and recording every path dominates the run otherwise.
    std::size_t   segmentRays = 2000;
    std::size_t   segmentCap  = 30000;
    // Receiver arrivals recorded for the spot metrics and the focus sweep.
    // These are cheap (one struct per arrival, no tree walk) so the budget is
    // much larger than the segment one.
    std::size_t   arrivalCap  = 200000;
    std::uint64_t seed        = 12345u;

    // Far-field intensity binning. 0 disables it.
    int nTheta = 90;
    int nPhi   = 72;

    PhysicsOptions physics;
};

// Progress reporting and cooperative cancellation. `cancel` is polled at chunk
// boundaries; `progress` is invoked from the calling thread only.
struct TraceControl {
    std::atomic<bool>* cancel = nullptr;
    std::function<void(std::size_t raysDone, std::size_t raysTotal)> progress;
};

// Monte Carlo ray tracer. Intersects rays against the BVH of a TraceScene using
// the Moller-Trumbore algorithm; applies Fresnel or fixed-split reflection,
// Snell refraction (with total internal reflection), diffuse scattering, surface
// roughness, Beer-Lambert bulk absorption and detector accumulation.
//
// Rays are independent, so the work is split into chunks handed out to a thread
// pool. Each ray seeds its own RNG from its global index, which keeps the
// result bit-identical no matter how many threads run it.
class RayTracer {
public:
    static void trace(const TraceScene& scene, const SourceConfig& src,
                      SimulationResult& out,
                      const TraceOptions& opt = TraceOptions{},
                      const TraceControl& ctl = TraceControl{});

    // Convenience overload: builds a throwaway TraceScene (and therefore a
    // throwaway BVH) from the meshes. Prefer the TraceScene overload when the
    // same geometry is traced more than once.
    static void trace(const MeshList& meshes, const SourceConfig& src,
                      SimulationResult& out,
                      const TraceOptions& opt = TraceOptions{},
                      const TraceControl& ctl = TraceControl{});

    // The full emitted ray for index `index`, exposed for the sampling tests.
    static EmittedRay sampleRay(const SourceConfig& src, std::size_t index,
                                std::uint64_t seed);
    // Emission direction only, for the tests that predate extended emitters.
    static Vec3 sampleDirection(const SourceConfig& src, std::size_t index,
                                std::uint64_t seed);

    // Traces one explicit ray and reduces it into `out` (which is reset first).
    // No sampling and no threading, so a test can pin down a single
    // interaction -- a given angle of incidence, a specific TIR case -- instead
    // of inferring it from Monte Carlo statistics.
    static void traceSingleRay(const TraceScene& scene, const Vec3& origin,
                               const Vec3& dir, SimulationResult& out,
                               double energy = 1.0,
                               const PhysicsOptions& physics = PhysicsOptions{},
                               double wavelengthNm = 587.6);
};
