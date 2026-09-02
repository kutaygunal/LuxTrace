#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <QMetaType>
#include <QString>
#include "Vec3.h"

// The unit a run's flux figures carry. Radiometric watts, or photometric
// lumens weighted by the CIE V(lambda) curve -- which needs a real spectrum
// underneath it, not three fixed lines.
enum class FluxUnit : int { Watt = 0, Lumen = 1 };

inline const char* fluxUnitName(FluxUnit u)       { return u == FluxUnit::Lumen ? "lm"    : "W"; }
inline const char* irradianceUnitName(FluxUnit u) { return u == FluxUnit::Lumen ? "lx"    : "W/m^2"; }
inline const char* intensityUnitName(FluxUnit u)  { return u == FluxUnit::Lumen ? "cd"    : "W/sr"; }

// One straight leg of a ray path, in world space. Kept in 3D so the OCCT viewer
// can draw the real path; the 2D diagram projects it to (x, z).
//
// The payload beyond the endpoints is what lets the viewer colour a beam by
// where its energy went rather than drawing every leg the same amber: `energy`
// is the fraction of one emitted ray still travelling along this leg, `depth`
// counts the interactions before it, and `flags` marks the legs that belong to
// a branch which actually reached the receiver.
struct RaySegment {
    enum Flag : std::uint16_t {
        ReachedDetector = 1u << 0,
    };

    Vec3          a, b;
    float         energy       = 1.0f;
    float         wavelengthNm = 0.0f;
    std::uint16_t depth        = 0;
    std::uint16_t flags        = 0;
    // Which source emitted the ray this leg belongs to. Two bytes, and it fits
    // inside the padding the struct already had -- which is what lets the
    // viewer separate a stray-light source's rays from the signal's.
    std::uint16_t source       = 0;

    bool reachedDetector() const { return (flags & ReachedDetector) != 0; }
};

// One interaction along a single explicitly-traced ray.
//
// Every commercial tracer has a single-ray debugger and this one did not, even
// though the machinery was finished: traceSingleRay already turns the
// estimators off so the whole path tree is visible, and already records every
// segment. What was missing was the *why* -- at this surface, at this angle,
// against this index pair, this much reflected and this much went on.
//
// Filled only when a caller asks for it, so the Monte Carlo path pays one null
// check per interaction and nothing else.
struct RayInteraction {
    Vec3    point;
    QString label;              // the surface's name, resolved by the caller
    int     surface = -1;
    int     depth   = 0;

    // Against the normal the interaction actually used -- the microfacet where
    // the surface is rough, which is the number Fresnel was evaluated at rather
    // than the one the smooth surface would have given.
    double  angleDeg = 0.0;
    double  n1 = 1.0, n2 = 1.0;

    double  reflectance   = 0.0;
    double  transmittance = 0.0;
    double  energyIn      = 0.0;   // fraction of the emitted ray still travelling
    double  opl           = 0.0;   // accumulated optical path length, mm

    // Which body the ray was inside on the way in, and how many it was inside
    // at once. A cemented doublet is the case this exists for.
    int     mediumSurface = -1;
    int     mediumDepth   = 0;

    bool    tir       = false;
    bool    detector  = false;
    bool    scattered = false;     // the outgoing direction came from a lobe
};

// One route light took from a source to a receiver, and what it delivered.
//
// `ids` is the sequence of contributors in the order they were met, ending at
// the receiver: a surface index as itself, or -(set + 1) where the run grouped
// surfaces into sets. Consecutive hits on one contributor are one step, so a
// guide wall hit ninety times is one entry and not ninety.
struct StrayPath {
    std::vector<int> ids;
    double           flux = 0.0;   // reached the receiver by this route
    std::size_t      rays = 0;     // arrivals counted along it
    // The route was longer than the recorder can hold, so `ids` is its head.
    bool             truncated = false;
};

// One ray arriving at the receiver, recorded with the direction it came in on.
//
// Keeping the direction (not just the hit point) is what makes a through-focus
// sweep a post-process: the arrivals can be propagated analytically to any
// plane near the receiver instead of re-tracing the scene once per plane.
struct DetectorArrival {
    Vec3   p;                 // hit point on the receiver
    Vec3   d;                 // unit direction of travel on arrival
    float  energy       = 0.0f;
    float  wavelengthNm = 0.0f;
    // Accumulated optical path length, sum of n * distance over every leg. The
    // spread of this across the arrivals is the wavefront error, which is what
    // an OPD map, a Strehl-style figure and a diffraction reference are built
    // from -- and it is one double per branch to carry.
    float  opl          = 0.0f;
    int    detector     = 0;  // index into SimulationResult::detectors
    // Which source emitted it. A luminaire with a signal source and a
    // stray-light source needs its spot metrics separable, and the arrival is
    // the only place that distinction survives to.
    int    source       = 0;
};

// Where a receiver sits and how it is binned, carried on the result so the
// analysis and the UI can map a bin back to world millimetres without asking
// the scene. Stored as a frame rather than as "assume +Z" so a tilted or
// off-axis receiver describes itself.
struct DetectorFrame {
    QString label;
    Vec3    center;
    Vec3    u{1, 0, 0};
    Vec3    v{0, 1, 0};
    Vec3    normal{0, 0, 1};
    double  w = 0.0, h = 0.0;
    int     nx = 0, ny = 0;
    double  acceptanceDeg = 180.0;

    // Flux per bin on this receiver, nx * ny. detectors[0].grid is mirrored into
    // SimulationResult::irradiance, which is what the plots read.
    std::vector<double> grid;
    double      flux     = 0.0;
    std::size_t arrivals = 0;
};

// Far-field intensity distribution: flux binned by the direction rays leave the
// system on, which is the candela distribution a luminaire is specified by.
//
// theta runs 0..180 degrees from +Z and phi 0..360 about it. `bin` holds raw
// flux per bin; `perSteradian` divides it by the bin's solid angle, which is
// the quantity that is actually intensity (and the one that stays flat for an
// isotropic source, where raw flux per bin does not).
struct IntensityGrid {
    int nTheta = 0, nPhi = 0;
    std::vector<double> bin;            // nTheta * nPhi, flux
    std::vector<double> perSteradian;   // nTheta * nPhi, flux / sr
    std::vector<double> profile;        // nTheta, flux / sr averaged over phi
    double peak       = 0.0;            // max of `profile`
    double fwhmDeg    = 0.0;            // full width of `profile` at half `peak`
    double totalFlux  = 0.0;

    // Beam-adaptive theta partition: the upper edge of every theta bin, in
    // radians, size nTheta+1, strictly increasing from 0 to pi. When the
    // far-field reduction fills this, the bins are NOT the old flat
    // `180.0 / nTheta` grid: they are equal-flux rings that concentrate where
    // the light goes, so a narrow (couple-of-degree) beam spans several bins
    // while dark directions share wide ones -- and every bin index accessor
    // below (centers, widths, solid angles, direction->bin) reads these edges,
    // which is what keeps the IES/EULUMDAT export, the polar plot and the
    // profile on one and the same grid. Empty means the legacy uniform-in-theta
    // grid (only ever a fallback now).
    std::vector<double> thetaEdges;

    static constexpr double kPi = 3.14159265358979323846264338327950288;

    bool valid()   const { return nTheta > 0 && nPhi > 0 && !bin.empty(); }
    bool adaptive() const { return thetaEdges.size() == std::size_t(nTheta) + 1; }

    // Mean theta bin width, degrees. For a beam-adaptive grid this is the
    // whole-span average; per-bin width is `thetaWidthDeg(i)`.
    double thetaStepDeg() const { return nTheta > 0 ? 180.0 / double(nTheta) : 0.0; }

    double thetaLowRad(int i) const {
        if (adaptive()) return thetaEdges[std::size_t(i)];
        const double n = double(nTheta > 0 ? nTheta : 1);
        return kPi * double(i) / n;
    }
    double thetaHighRad(int i) const {
        if (adaptive()) return thetaEdges[std::size_t(i) + 1];
        const double n = double(nTheta > 0 ? nTheta : 1);
        return kPi * double(i + 1) / n;
    }
    double thetaCenterRad(int i) const {
        return 0.5 * (thetaLowRad(i) + thetaHighRad(i));
    }
    double thetaLowDeg(int i)  const { return thetaLowRad(i)  * 180.0 / kPi; }
    double thetaHighDeg(int i) const { return thetaHighRad(i) * 180.0 / kPi; }
    double thetaCenterDeg(int i) const { return thetaCenterRad(i) * 180.0 / kPi; }
    double thetaWidthDeg(int i) const { return (thetaHighRad(i) - thetaLowRad(i)) * 180.0 / kPi; }

    // Solid angle of a single (theta, phi) cell in ring i, steradians. With the
    // adaptive grid each ring is a cone frustum whose width varies with the
    // flux, so this is computed from the actual bin edges rather than a flat
    // dTheta.
    double cellSolidAngleRad(int i) const {
        if (nPhi <= 0) return 0.0;
        return (2.0 * kPi / double(nPhi)) *
               (std::cos(thetaLowRad(i)) - std::cos(thetaHighRad(i)));
    }

    // The theta bin a direction leaving -- as cos(theta), 1 at +Z -- falls
    // into. Binary search over the adaptive edges; the legacy path keeps the
    // old closed form.
    int thetaBinIndex(double cosTheta) const {
        if (nTheta <= 0) return 0;
        const double th = std::acos(std::clamp(cosTheta, -1.0, 1.0));
        if (!adaptive()) {
            return std::clamp(int(th / kPi * double(nTheta)), 0, nTheta - 1);
        }
        const int k = int(std::upper_bound(thetaEdges.begin(), thetaEdges.end(), th)
                          - thetaEdges.begin()) - 1;
        return std::clamp(k, 0, nTheta - 1);
    }

    // Builds a beam-adaptive theta partition -- nOut+1 strictly increasing
    // edges (radians, 0..pi) -- from a master theta-ring flux histogram. Each
    // output ring takes an equal share of the (lightly floored) emitted flux,
    // so bins concentrate exactly where the flux is. `baseline` is a tiny
    // per-master-bin floor that keeps the CDF inversion well-behaved across
    // silent directions and guarantees strictly increasing edges.
    static void buildBeamAdaptiveEdges(const std::vector<double>& ringFlux,
                                       int masterN, int nOut, double baseline,
                                       std::vector<double>& edgesOut) {
        edgesOut.assign(std::size_t(nOut) + 1, 0.0);
        if (nOut <= 0 || masterN <= 0) return;
        const double W = kPi / double(masterN);                 // master bin width, rad
        // Flux integral (with the floor) up to each master bin's upper edge.
        std::vector<double> cum(std::size_t(masterN) + 1, 0.0);
        for (int m = 0; m < masterN; ++m)
            cum[m + 1] = cum[m] + (ringFlux[m] + baseline) * W;
        const double total = cum[masterN];
        edgesOut[0] = 0.0;
        edgesOut[std::size_t(nOut)] = kPi;
        if (total <= 0.0) {                                     // silent run: uniform fallback
            for (int k = 1; k < nOut; ++k)
                edgesOut[std::size_t(k)] = kPi * double(k) / double(nOut);
            return;
        }
        for (int k = 1; k < nOut; ++k) {
            const double q = total * double(k) / double(nOut);
            const auto    it = std::upper_bound(cum.begin() + 1, cum.end(), q);
            const int     m  = std::clamp(int(it - cum.begin()) - 1, 0, masterN - 1);
            const double  below = cum[m];
            const double  seg   = cum[m + 1] - below;
            const double  t     = (seg > 0.0) ? W * (q - below) / seg : 0.5 * W;
            edgesOut[std::size_t(k)] =
                std::clamp(m * W + std::clamp(t, 0.0, W), 0.0, kPi);
            if (edgesOut[std::size_t(k)] <= edgesOut[std::size_t(k - 1)])
                edgesOut[std::size_t(k)] = edgesOut[std::size_t(k - 1)] + 1e-12;
        }
    }
};

// The estimator's bookkeeping residual, one named channel per estimator.
//
// Every one of these replaces a sampled quantity with an estimate of it, so the
// two differ run to run and agree in expectation. Booking them all to a single
// accumulator made the energy balance close even when one contributor was
// systematically wrong and another happened to cancel it -- which is the one
// respect in which the closed balance could close while being wrong. Named
// channels turn it into a real guarantee: each one's mean has to lie inside its
// own error bar, and the test asserts exactly that.
struct EstimatorResidual {
    // A killed branch's energy, less the energy handed to the survivor that
    // replaced it.
    double roulette      = 0.0;
    // The analytic share of the source's distribution aimed away, less the
    // companion draw that stood for it.
    double aiming        = 0.0;
    // Next-event estimation: the analytic direct term, entered negative
    // because it was added to the receiver without a ray carrying it.
    double nextEvent     = 0.0;
    // The sampled path's own direct hit, suppressed because next-event
    // estimation already accounted for it.
    double neeSuppressed = 0.0;
    // What a BSDF sampling weight created or destroyed.
    double bsdfWeight    = 0.0;

    double total() const {
        return roulette + aiming + nextEvent + neeSuppressed + bsdfWeight;
    }
    void scale(double f) {
        roulette *= f; aiming *= f; nextEvent *= f;
        neeSuppressed *= f; bsdfWeight *= f;
    }
};

// Why flux was truncated, rather than how much of it was.
//
// At a depth limit of 160 a light guide or an integrating sphere can put real
// flux here, and one number cannot say whether raising the limit would help or
// whether the geometry is the problem.
struct TruncationBreakdown {
    double depthLimit    = 0.0;   // ran past kMaxDepth interactions
    double stackOverflow = 0.0;   // the pending-branch stack was full
    double degenerate    = 0.0;   // an outgoing direction that would not normalise
    double refractFailed = 0.0;   // Snell refused where the energy split did not
    double energyCutoff  = 0.0;   // below the cutoff, with roulette switched off

    std::size_t depthLimitCount    = 0;
    std::size_t stackOverflowCount = 0;
    std::size_t degenerateCount    = 0;
    std::size_t refractFailedCount = 0;
    std::size_t energyCutoffCount  = 0;

    double total() const {
        return depthLimit + stackOverflow + degenerate + refractFailed + energyCutoff;
    }
    void scale(double f) {
        depthLimit *= f; stackOverflow *= f; degenerate *= f;
        refractFailed *= f; energyCutoff *= f;
    }
};

// Builds `g` from a fine uniform master grid of nThetaMaster x nPhi flux bins.
// The output theta partition is beam-adaptive and is derived from `master`, so
// every backend must come through here or their far fields are not comparable.
void finishIntensityGrid(IntensityGrid& g, std::vector<double> master,
                         int nTheta, int nPhi);

// Medium-tracking anomalies, counted rather than swallowed.
//
// The recoveries themselves are right: exiting a body the stack never recorded
// entering is a modelling artefact and corrupting the rest of the stack over it
// would be worse. But a self-intersecting or non-manifold imported mesh
// produces systematically wrong index pairs and a perfectly clean-looking
// result, and imported CAD is the path most likely to be geometrically
// imperfect. This is what a commercial tracer's ray-error report is.
struct MediumAnomalies {
    // A refractive surface was left that the branch was never recorded entering.
    std::size_t unmatchedExit = 0;
    // More than kMediumDepth nested media at once; the innermost was dropped.
    std::size_t stackOverflow = 0;
    // A face was crossed outward while the branch was recorded as being in
    // vacuum, so the incident index had to be guessed.
    std::size_t guessedIndex  = 0;

    std::size_t total() const { return unmatchedExit + stackOverflow + guessedIndex; }
    bool any() const { return total() > 0; }
};

// What one source of a multi-source run contributed.
//
// SimConfig used to hold exactly one source, placed by the scene, so any
// luminaire with more than one LED -- and any system needing a stray-light
// source alongside the signal source -- could not be built at all. The tracer
// carries a source index on every emitted ray; this is where that index is
// reported back.
struct SourceSummary {
    QString     label;
    double      power   = 0.0;    // as configured, in the run's unit
    double      flux    = 0.0;    // what reached the receivers, same unit
    std::size_t rays    = 0;      // ray budget this source was given
    bool        rayFile = false;  // emitted from a measured ray set

    double efficiency() const { return power > 0.0 ? flux / power : 0.0; }
};

// Result of a Monte Carlo ray-trace run: detector irradiance grid + statistics
// + sampled ray segments (x,z) for the 2D diagram.
//
// All flux values are normalised to the emitted source power, so
//   fluxDetector + fluxAbsorbed + fluxEscaped + fluxTruncated == source power
// holds for a completed run (see the energy-conservation test).
struct SimulationResult {
    int nx = 0, ny = 0;
    double detW = 0.0, detH = 0.0;
    // Centre of the receiver rectangle, in the receiver's own in-plane axes:
    // detCX along `detU`, detCY along `detV`. The grid indexes from
    // (detCX - detW/2, detCY - detH/2), so an off-axis receiver still maps back
    // to millimetres correctly.
    //
    // For the ordinary receiver -- a plane facing +Z, whose axes are world +X
    // and +Y -- these are exactly the world x and y they always were. Stating
    // the frame is what makes a receiver facing any other way report against
    // itself rather than against a world plane it does not lie in, which is
    // what a part illuminated from its side needs.
    double detCX = 0.0, detCY = 0.0, detZ = 0.0;

    // The in-plane axes those coordinates are measured along, and the normal.
    Vec3 detU{1, 0, 0};
    Vec3 detV{0, 1, 0};
    Vec3 detN{0, 0, 1};

    // Accumulated flux per bin (physical-energy weighted counts).
    std::vector<double> irradiance;

    // Per-band irradiance for a spectral run: 3 * nx * ny, red first. Empty for
    // a monochromatic trace. The three bands sum to `irradiance`.
    std::vector<double> bandIrradiance;
    bool spectral = false;

    IntensityGrid intensity;

    // Every receiver in the scene, in surface order. The irradiance grid above
    // belongs to detectors[0]; the rest are recorded so a second receiver is a
    // reported surface rather than a silent mis-binning.
    std::vector<DetectorFrame> detectors;

    // Every source that emitted into this run, in configuration order. One
    // entry for the ordinary single-source case, so nothing has to branch on
    // the count to read it.
    std::vector<SourceSummary> sources;

    double fluxDetector  = 0.0;   // reached the receiver
    double fluxAbsorbed  = 0.0;   // lost to surface absorption and bulk attenuation
    double fluxEscaped   = 0.0;   // left the scene without hitting anything
    double fluxTruncated = 0.0;   // dropped at the depth limit or a degenerate branch

    // Refused by a receiver's acceptance cone. That light was not absorbed by
    // anything -- it was refused by a measurement condition -- and an
    // absorbed-flux figure that silently includes it is a wrong loss budget.
    double fluxRejected  = 0.0;

    // Bookkeeping residual of the estimators: the energy a killed branch took
    // with it, less the energy handed to the survivors that replaced it, and
    // the same for every other estimator. Its expectation is exactly zero -- it
    // is not a loss channel, it is the estimator's noise made visible -- and
    // including it is what keeps the physical buckets closed to the last bit.
    double fluxRoulette  = 0.0;

    // The same figure, split one channel per estimator, so a channel that is
    // systematically wrong cannot hide behind another that cancels it.
    EstimatorResidual   residual;
    // Why the truncated flux was truncated.
    TruncationBreakdown truncation;
    // Medium-tracking anomalies seen during the run.
    MediumAnomalies     anomalies;

    // True when the truncated share of the source is large enough to be worth
    // saying so. "Where did the missing 4 % go?" is the question the energy
    // balance exists to answer.
    static constexpr double kTruncationWarn = 1e-3;   // 0.1 % of the source
    bool truncationSignificant() const {
        return sourcePower > 0.0 && fluxTruncated > kTruncationWarn * sourcePower;
    }

    // Bulk (Beer-Lambert) share of fluxAbsorbed. Reported separately because it
    // is the term that scales with path length rather than with hit count, so
    // it is the one that explains a long light guide.
    double fluxBulkAbsorbed = 0.0;

    std::size_t raysEmitted     = 0;
    // Number of path branches that landed on the receiver, not distinct rays:
    // one emitted ray can arrive several times after splitting at a refractive
    // surface. It is a heavy-tailed count (a single trapped ray can contribute
    // dozens of near-zero-energy arrivals), so judge a run by flux, not by this.
    std::size_t raysHitDetector = 0;
    double      efficiency      = 0.0;
    // One standard error on `efficiency` from the ray-to-ray spread. This is the
    // Monte Carlo uncertainty: it falls as 1/sqrt(rays), and it is what makes
    // "have I traced enough rays?" a question with an answer.
    double      efficiencyStdErr = 0.0;
    double      sourcePower     = 1.0;

    // The unit `sourcePower` and every flux, irradiance and intensity figure on
    // this result is expressed in.
    FluxUnit    unit = FluxUnit::Watt;
    // Luminous efficacy of the source spectrum, lm/W. Multiplying a radiometric
    // result by this gives the photometric one, so a run reports both from one
    // trace: watts and lumens, W/m^2 and lux, W/sr and candela.
    double      luminousEfficacy = 0.0;
    // Mean wavelength of the emitted spectrum, nm.
    double      meanWavelengthNm = 0.0;

    // Flux-weighted polarisation of what reached the receiver. Only filled on a
    // polarised trace; `polarised` says whether to believe it.
    bool   polarised        = false;
    double degreeOfPolarisation = 0.0;   // 0 unpolarised, 1 fully polarised
    double degreeLinear     = 0.0;
    double polarisationAngleDeg = 0.0;   // of the linear part, from the s axis

    // Same flux in the other unit, for the readouts that name both.
    double fluxDetectorIn(FluxUnit u) const {
        if (u == unit) return fluxDetector;
        if (u == FluxUnit::Lumen) return fluxDetector * luminousEfficacy;
        return luminousEfficacy > 0.0 ? fluxDetector / luminousEfficacy : 0.0;
    }

    // Surface overrides the scene could not be matched to, by label. An edit
    // that no longer resolves is reported rather than dropped or -- worse --
    // applied to whatever surface now occupies the slot it was saved against.
    std::vector<QString> unmatchedOverrides;

    // Wall-clock time of the trace itself (geometry/meshing excluded).
    double traceSeconds = 0.0;
    // Wall-clock time spent building geometry + mesh + BVH (0 when cached).
    double buildSeconds = 0.0;
    // True when the run stopped early because cancellation was requested.
    bool   cancelled = false;

    // True for a progressive snapshot: a consistent view of the work finished so
    // far, normalised by that work, delivered while the trace is still running.
    // Its scalars reduce in thread order rather than chunk order, so its last
    // digits depend on scheduling in a way the final result never does.
    bool        partial       = false;
    std::size_t raysRequested = 0;

    // Every interaction of a single explicitly-traced ray, in the order they
    // happened. Empty for a Monte Carlo run: it is filled only by
    // RayTracer::traceSingleRay, which is what the ray inspector calls.
    std::vector<RayInteraction> interactions;

    // Which routes delivered light to the receiver, ranked by how much. Empty
    // unless the run asked for stray-light paths.
    std::vector<StrayPath> strayPaths;
    // Arrivals whose route did not fit in the table, so a truncated list says
    // so instead of reading as a complete one.
    std::size_t strayPathsDropped = 0;
    double      strayFluxDropped  = 0.0;
    // What to call each id a route names, resolved at trace time so an exported
    // route file reads without the scene beside it.
    std::vector<QString> straySurfaceLabels;
    std::vector<QString> straySetNames;

    // What a route's id means: a surface, or a set of them.
    QString strayName(int id) const {
        if (id < 0) {
            const std::size_t k = std::size_t(-(id + 1));
            if (k < straySetNames.size() && !straySetNames[k].isEmpty())
                return straySetNames[k];
            return QStringLiteral("set %1").arg(-(id + 1));
        }
        const std::size_t k = std::size_t(id);
        if (k < straySurfaceLabels.size() && !straySurfaceLabels[k].isEmpty())
            return straySurfaceLabels[k];
        return QStringLiteral("surface %1").arg(id);
    }

    // Per-bin variance of the irradiance grid: the sum of the squares of the
    // deposits that made each bin, in the same units as `irradiance` squared.
    //
    // A bin's value is a sum of independent weighted contributions, so the sum
    // of their squares is an unbiased estimate of that sum's variance -- which
    // is the only honest way to say how noisy a pixel is. Ray counts cannot say
    // it: next-event estimation deposits a highly variable weight at nearly
    // every diffuse bounce, so on an integrating sphere the count per bin and
    // the noise in that bin are barely related.
    //
    // sqrt() of an entry is a one-sigma error bar on the bin beside it.
    std::vector<double> irradianceVar;

    // The far field before it was folded into beam-adaptive rings: the fine
    // uniform grid it was accumulated on, and the sum of squared deposits that
    // says how noisy each of its cells is.
    //
    // Kept, when a noise map was asked for, because the *reported* far field is
    // binned on edges derived from each run's own histogram -- so two correct
    // runs land on slightly different rings and cannot be compared cell for
    // cell. The master grid has the same shape whatever the beam did, which
    // makes it the thing two backends can actually be held against each other
    // on. Empty otherwise.
    // Which backend produced this. Every export and every report carries it,
    // because "how fast was it" and "what is it" are the same question once
    // there is more than one tracer in the building.
    QString backend = QStringLiteral("cpu");

    std::vector<double> intensityMaster;
    std::vector<double> intensityMasterVar;
    int                 intensityMasterTheta = 0;
    int                 intensityMasterPhi   = 0;

    // The binned grids -- irradiance, colour bands, far-field intensity -- are
    // accumulated per thread and summed in thread order, so which rays landed
    // in which partial sum depends on how the chunks were scheduled. Every
    // scalar reduces in chunk order and does not. In practice a bin differs in
    // its last place or not at all; it is stated because a claim of
    // reproducibility that is not exactly true is worth less than a smaller one
    // that is.
    static constexpr bool gridsReduceInThreadOrder = true;

    // Sampled ray path legs, for the 3D viewer and the 2D diagram.
    std::vector<RaySegment> raySegments;
    // Sampled receiver arrivals, for the spot metrics and the focus sweep.
    std::vector<DetectorArrival> arrivals;

    // Sum of every energy bucket; equals sourcePower for a conserving run.
    double fluxAccounted() const {
        return fluxDetector + fluxAbsorbed + fluxEscaped + fluxTruncated
             + fluxRejected + fluxRoulette;
    }

    // Where a point on the receiver falls in the frame the bins are indexed in.
    // A ray's arrival point is in world space; these are what put it on the grid
    // however the receiver is oriented.
    double detu(const Vec3& p) const { return p.dot(detU); }
    double detv(const Vec3& p) const { return p.dot(detV); }

    // Centre of irradiance bin (ix, iy), in the receiver's own axes.
    double binX(int ix) const { return detCX - 0.5 * detW + (double(ix) + 0.5) * detW / double(nx > 0 ? nx : 1); }
    double binY(int iy) const { return detCY - 0.5 * detH + (double(iy) + 0.5) * detH / double(ny > 0 ? ny : 1); }
    double binArea() const {
        if (nx <= 0 || ny <= 0) return 0.0;
        return (detW / double(nx)) * (detH / double(ny));
    }
};

Q_DECLARE_METATYPE(SimulationResult)
