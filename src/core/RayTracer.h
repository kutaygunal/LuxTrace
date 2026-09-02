#pragma once
#include "Mesh.h"
#include "SimulationResult.h"
#include "Polarisation.h"
#include "RayFile.h"
#include "Spectrum.h"
#include "TraceScene.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
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

    // The spectrum is a distribution now, not a choice of three lines; the old
    // enum survives as its Kind so existing call sites keep reading.
    using Spectrum = SpectrumConfig::Kind;

    Type   type   = Type::Point;
    Shape  shape  = Shape::PointLike;
    gp_Pnt origin{0, 0, 0};
    gp_Dir axis{0, 0, 1};   // emission axis (hemisphere normal for Lambertian)
    int    rays   = 100000;

    // Total emitted flux, in `fluxUnit`. Every quantity the run reports is in
    // that unit: the irradiance grid in W/m^2 or lux, the far field in W/sr or
    // candela, the encircled energy in real flux. A dimensionless fraction of an
    // unnamed unit is not a number an engineer can put in a specification.
    double   power    = 1.0;
    FluxUnit fluxUnit = FluxUnit::Watt;

    // Emission cone half-angle about `axis`, degrees. Clamped to 180 for Point
    // and to 90 for Lambertian, so the defaults reproduce the full sphere and
    // the full hemisphere respectively. Ignored by Collimated.
    double halfAngleDeg = 180.0;

    // Emitter size, mm. Disc and Sphere use `sizeA` as the radius; Rect uses
    // sizeA x sizeB about `origin`, in the plane normal to `axis`.
    double sizeA = 0.0, sizeB = 0.0;

    // Radius of the parallel bundle, mm. Collimated only.
    double beamRadius = 25.0;

    // Emission spectrum. One wavelength is sampled per ray from it, so a
    // spectral trace costs what a monochromatic one costs and resolves the
    // spectrum to whatever the ray budget supports.
    SpectrumConfig spectrum;

    // The polarisation state the source emits, used only on a polarised trace.
    //   0  unpolarised
    //   1  linear, s (perpendicular to the first plane of incidence)
    //   2  linear, p
    //   3  circular
    int polarisationState = 0;

    // What to call this source where a run reports more than one.
    QString label;

    // A measured ray set standing in for the analytic emitter.
    //
    // With one set, `type`, `shape`, `sizeA`/`sizeB`, `beamRadius` and
    // `halfAngleDeg` are not consulted at all: the file already carries every
    // ray's origin, direction and flux, which is the whole point of buying a
    // measurement rather than approximating one. `power` still says what the
    // source is worth in the run's unit, and the spectrum is still consulted
    // for the rays the file gives no wavelength to.
    //
    // The file's own frame is placed by `origin` and `axis`, exactly as an
    // analytic emitter's is: its +Z is rotated onto `axis` and its origin
    // translated to `origin`, so a ray set drops into whatever place the scene
    // puts its emitter.
    std::shared_ptr<const RayFileData> rayFile;

    // Extra scale on the ray-file positions, on top of whatever its header's
    // dimension code already applied. 1 is the ordinary case.
    double rayFileScale = 1.0;

    // Take each ray's wavelength from the file where it has one. Off pins the
    // whole set to the configured spectrum, which is what comparing a measured
    // set against an idealised one needs.
    bool   rayFileWavelengths = true;

    bool tracesRayFile() const { return rayFile && rayFile->valid(); }
};

// One emitted ray: where it starts, where it goes, and what colour it is.
struct EmittedRay {
    Vec3   origin;
    Vec3   dir;
    double wavelengthNm = 587.6;
    // Share of the source's flux this ray carries, relative to the mean. It is
    // 1 for a radiometric run; for a photometric one it is V(lambda) normalised
    // over the spectrum, which is what makes every downstream quantity read in
    // lumens without a second accumulator anywhere.
    double weight = 1.0;

    // Share of the source's angular distribution this sample represents, when
    // emission was aimed at the scene. 1 means "not aimed"; anything less means
    // the complementary share points where nothing can be hit, and its energy
    // belongs in the escaped bucket without being traced.
    double aimWeight = 1.0;

    // A second, untraced draw from the source's own angular law, used only when
    // aiming is active. The energy aimed away still has to appear in the far
    // field with the distribution the source would have given it -- otherwise
    // aiming would quietly delete the part of the beam that misses the optic,
    // which is exactly the part a luminaire designer cares about. `missed` says
    // the draw fell outside the aiming cone, so this ray stands for that share.
    Vec3 missDir;
    bool missed = false;
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
    bool coatings   = true;   // thin films on refractive surfaces
    bool volumeScattering = true;   // scattering inside a medium, not at it

    // Carry a Stokes vector on every branch and apply a Mueller matrix at every
    // interaction. Roughly four times the per-ray state, and most illumination
    // work does not need it, so the unpolarised fast path stays the default.
    bool polarised  = false;

    // Scene-wide overrides, so roughness and scatter can be explored without
    // rebuilding geometry -- they are ray-time properties, and re-tessellating
    // an optic to change its polish would be absurd. Negative means "leave each
    // surface's own value alone". The receiver is never affected.
    double roughnessOverride = -1.0;   // radians
    double scatterOverride   = -1.0;   // 0..1
    double absorptionScale   = 1.0;    // multiplies every medium's alpha

    // Estimator switches, not physics. Russian roulette and branch collapsing
    // are unbiased over a run but turn a single ray's answer into a draw rather
    // than the whole path tree, so anything reading one interaction (the
    // single-ray path, a validation case) turns them off.
    bool varianceReduction = true;
};

// Variance reduction. None of these change what a run converges to; they change
// how many rays it takes to get there.
struct EstimatorOptions {
    // Owen-scrambled Sobol instead of independent uniforms for the emission
    // stream. Indexed by ray number, so determinism is untouched.
    bool lowDiscrepancy = true;

    // Emit only into the cone the scene actually occupies, weighting each ray by
    // the share of the source's own distribution that lands in it. A direction
    // outside that cone cannot hit anything, so the energy it would have carried
    // is booked straight to "escaped" -- this is exact, not merely unbiased, and
    // it is the largest win on a source that radiates in every direction at an
    // optic that fills a fraction of the sky.
    bool aimAtScene = true;

    // Connect every diffuse bounce to the receiver analytically instead of
    // hoping a random walk finds it. Order-of-magnitude variance reduction on an
    // integrating sphere or a diffuser; nothing at all on a purely specular
    // scene, where there are no diffuse bounces to connect.
    bool nextEventEstimation = true;

    // Independent Owen scrambles the ray budget is split between. A Sobol
    // sequence is not a set of independent samples, so the ray-to-ray spread no
    // longer measures the error of the mean -- it would report the error a plain
    // Monte Carlo run of the same size would have had, which is the whole gain
    // thrown away in the reporting. Replicating the scramble and taking the
    // spread across replicas measures what the estimator actually achieved.
    // Ignored when `lowDiscrepancy` is off.
    int replicas = 16;
};

// Stray-light path analysis: which sequences of surfaces deliver light to the
// receiver, and how much each one carries.
//
// "Where is the veiling glare coming from" is not answerable from an irradiance
// map, because the map is the sum over every route the light took. It is
// answerable from a ranked list of routes, which is what a stray-light review
// actually reads, and it is the one analysis this tracer had the machinery for
// and no way to ask for: every interaction was already visited, and the surface
// index at each one was already resolved.
struct StrayPathOptions {
    bool enabled = false;

    // Which contributor each surface counts as, indexed by surface. A negative
    // entry -- or an index past the end -- leaves the surface as its own.
    //
    // This is what keeps the list readable. A light guide's wall is one surface
    // hit ninety times and a baffle stack is forty surfaces doing one job; both
    // produce thousands of distinct sequences that mean the same thing. Grouping
    // them collapses those to one, and consecutive hits within a set collapse to
    // a single step, which is the difference between a list a person can read
    // and a list nobody can.
    std::vector<int> surfaceSet;
    // Display names for the sets, indexed by set id.
    std::vector<QString> setNames;

    // How many distinct routes to keep. Beyond it, arrivals are counted into
    // the result's `strayPathsDropped` rather than silently discarded.
    std::size_t maxPaths = 4096;

    int setOf(int surface) const {
        return (surface >= 0 && surface < int(surfaceSet.size())) ? surfaceSet[std::size_t(surface)]
                                                                  : -1;
    }
};

// A change to one surface's optics, applied for the length of a run.
//
// Roughness, scatter and absorption used to be adjustable only as blunt
// scene-wide multipliers -- a decision that made sense while every property was
// hardcoded per scene, and stops making sense the moment somebody wants one
// matte mirror in a polished system.
//
// None of these are geometry, so applying one costs a trace and not a rebuild:
// the tessellation and the hierarchy are untouched and the geometry cache stays
// warm. That is what makes editing the surface you just clicked on feel
// immediate.
struct SurfaceOverride {
    // Which surface this edit belongs to, keyed by what the surface *is*.
    //
    // It used to be an index into TraceScene::surfaces() and nothing else, and
    // a config persisted that index. Reopen a saved config after the scene
    // registry changes, or against a different CAD import, and every optical
    // edit silently lands on whatever surface now occupies the slot -- a
    // plausible-looking wrong answer, which is the only kind that costs anyone
    // money. The scene enum was stored by name for exactly this reason.
    //
    // `label` is the primary key, `identity` (TraceScene::surfaceIdentity)
    // disambiguates repeated labels and vetoes a stale index, and `surface` is
    // a hint used only when there is no label to go on. An override that
    // resolves to nothing is reported on the result rather than dropped.
    QString       label;
    std::uint64_t identity = 0;
    int           surface  = -1;
    SurfaceOptics optics;
};

// Knobs that trade run time against fidelity of the diagram overlay.
struct TraceOptions {
    // 0 == use every hardware thread.
    //
    // Every scalar the run reports -- efficiency, the energy budget, the flux
    // per receiver and per source -- reduces in chunk order and is bit-identical
    // whatever this is set to. The binned grids reduce per thread, so a single
    // bin can differ in its last place between two thread counts; see
    // `SimulationResult::gridsReduceInThreadOrder`.
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

    PhysicsOptions   physics;
    EstimatorOptions estimator;

    // Per-surface optical edits, applied over the scene's own values for this
    // run only. The scene itself is shared between runs and never modified.
    std::vector<SurfaceOverride> surfaceOverrides;

    // Stray-light path analysis. Off by default, and off it costs one
    // predictable branch per interaction and nothing else.
    StrayPathOptions strayPaths;

    // Deal chunks to threads by a fixed stride, so which partial grid a ray
    // lands in -- and therefore the last bit of every binned result -- does not
    // depend on who asked for work first. Off, chunks come from a shared
    // counter, which balances better under an oversubscribed machine and gives
    // up run-to-run reproducibility of the grids. The scalars reduce per chunk
    // and are unaffected either way.
    // Accumulate a per-bin variance estimate for the receiver grid: the sum of
    // the squares of the deposits. One more grid per thread and one
    // multiply-add per deposit, so it is off unless asked for.
    bool noiseMap = false;

    bool deterministicGrids = true;
};

// Progress reporting, partial results and cooperative cancellation. `cancel` is
// polled at chunk boundaries; the callbacks are invoked from the calling thread
// only.
struct TraceControl {
    std::atomic<bool>* cancel = nullptr;
    std::function<void(std::size_t raysDone, std::size_t raysTotal)> progress;

    // A snapshot of the run so far, delivered every `partialIntervalMs`. The
    // image forms while the trace runs and the error bar visibly shrinks,
    // instead of a progress bar and then an answer.
    //
    // A snapshot reduces the per-thread accumulators in thread order rather than
    // in chunk order, so its last few digits depend on scheduling in a way the
    // final result never does. It is a preview; the result the run returns is
    // the bit-reproducible one.
    std::function<void(const SimulationResult& partial)> partial;
    int partialIntervalMs = 200;
};

// Resolves every surface's scattering into the one model a tracer reads: a real
// BSDF where one is set, `scatter` as a Lambertian lobe, `roughness` as a GGX
// microfacet, the scene-wide overrides applied over all three, and the physics
// switches gating each. Writes back over the caller's own copy of the surfaces.
//
// Exposed rather than private to the tracer because the preview backend has to
// resolve them the same way. Two backends that disagree about what a surface
// *is* cannot be compared, and a second copy of this decision would drift from
// the first the moment either changed.
void resolveScattering(std::vector<SceneSurface>& surfs, const PhysicsOptions& phys);

// Copies each receiver frame onto a result, and splits the flat all-receivers
// grid back out per receiver, mirroring the first into the irradiance grid the
// plots read.
//
// Exposed for the same reason resolveScattering above is: the preview backend
// accumulates into the same flat layout and has to unpack it the same way. Two
// unpackings of one layout are two chances to disagree about which bin is which.
// Which refractive solids contain `p`, outermost first.
//
// Parity along one probe ray settles it: a closed solid is crossed an odd
// number of times by any ray leaving a point inside it, and an even number
// from outside. A source inside a solid -- an LED die in its own encapsulant --
// has to start its rays already in that solid rather than in vacuum, or the
// first interface it meets refracts against the wrong pair of indices.
//
// Exposed because the preview backend has to seed its emission the same way,
// and one walk per source per run is not a thing worth writing twice. Returned
// as plain indices rather than as the tracer own stack type, which is private
// to that translation unit.
std::vector<int> mediaContainingPoint(const TraceScene& scene,
                                      const std::vector<SceneSurface>& surfs,
                                      const Vec3& p);

void fillDetectorFrames(const TraceScene& scene, SimulationResult& out);
void scatterDetectorGrids(const TraceScene& scene, const std::vector<double>& flat,
                          SimulationResult& out);

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

    // The same, with more than one source.
    //
    // Each source keeps its own placement, angular law, spectrum, polarisation
    // state, ray budget and power; the run reports the total and, per source,
    // what that source delivered. Rays are partitioned by source over one
    // global index space, so a run is still bit-identical at any thread count.
    //
    // The powers are combined at emission rather than at the end: a ray from
    // source s is emitted carrying power_s / rays_s, so one shared receiver
    // grid holds every source's contribution correctly weighted, and a
    // two-source run costs one grid rather than two. For a single source this
    // is exactly the normalisation the one-source overload always used, which
    // is why that overload is now a call to this one.
    static void trace(const TraceScene& scene, const std::vector<SourceConfig>& sources,
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
    // The same, against an already-resolved spectrum -- which is what the run
    // uses, so a test can reproduce a run's emission exactly.
    static EmittedRay sampleRay(const SourceConfig& src, const SampledSpectrum& spec,
                                std::size_t index, std::uint64_t seed,
                                bool lowDiscrepancy = false);
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
