#pragma once
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

    bool reachedDetector() const { return (flags & ReachedDetector) != 0; }
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

    bool valid() const { return nTheta > 0 && nPhi > 0 && !bin.empty(); }
    double thetaStepDeg() const { return nTheta > 0 ? 180.0 / nTheta : 0.0; }
    // Centre angle of theta bin i, degrees.
    double thetaCenterDeg(int i) const { return (double(i) + 0.5) * thetaStepDeg(); }
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

    double fluxDetector  = 0.0;   // reached the receiver
    double fluxAbsorbed  = 0.0;   // lost to surface absorption and bulk attenuation
    double fluxEscaped   = 0.0;   // left the scene without hitting anything
    double fluxTruncated = 0.0;   // dropped at the depth limit or a degenerate branch

    // Bookkeeping residual of the Russian-roulette estimator: the energy a
    // killed branch took with it, less the energy handed to the survivors that
    // replaced it. Its expectation is exactly zero -- it is not a loss channel,
    // it is the estimator's noise made visible -- and including it is what keeps
    // the four physical buckets closed to the last bit.
    double fluxRoulette  = 0.0;

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

    // Sampled ray path legs, for the 3D viewer and the 2D diagram.
    std::vector<RaySegment> raySegments;
    // Sampled receiver arrivals, for the spot metrics and the focus sweep.
    std::vector<DetectorArrival> arrivals;

    // Sum of every energy bucket; equals sourcePower for a conserving run.
    double fluxAccounted() const {
        return fluxDetector + fluxAbsorbed + fluxEscaped + fluxTruncated + fluxRoulette;
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
