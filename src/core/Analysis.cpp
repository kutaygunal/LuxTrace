#include "Analysis.h"
#include "Spectrum.h"

#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace analysis {
namespace {

// A focus sweep re-reduces every arrival once per plane, so a full 200 000-hit
// record would make dragging the sweep range feel sticky for no extra accuracy:
// the RMS radius of 60 000 samples is already converged well past the Monte
// Carlo noise of the trace that produced them.
constexpr double      kPi           = 3.14159265358979323846;
constexpr std::size_t kFocusSamples = 60000;
constexpr int         kRadiusBins   = 256;

// Linear interpolation of the radius at which a monotone cumulative curve first
// reaches `target`.
double radiusAtFraction(const std::vector<double>& cumulative, double binWidth,
                        double target) {
    if (cumulative.empty()) return 0.0;
    const double total = cumulative.back();
    if (total <= 0.0) return 0.0;
    const double want = target * total;
    for (std::size_t i = 0; i < cumulative.size(); ++i) {
        if (cumulative[i] >= want) {
            const double lo = (i == 0) ? 0.0 : cumulative[i - 1];
            const double f  = (cumulative[i] > lo) ? (want - lo) / (cumulative[i] - lo) : 0.0;
            return (double(i) + f) * binWidth;
        }
    }
    return double(cumulative.size()) * binWidth;
}

// Full width at half maximum of a sampled curve, around its peak.
double fwhmOf(const std::vector<double>& coord, const std::vector<double>& value) {
    if (coord.size() < 2 || coord.size() != value.size()) return 0.0;
    std::size_t peak = 0;
    for (std::size_t i = 1; i < value.size(); ++i)
        if (value[i] > value[peak]) peak = i;
    if (value[peak] <= 0.0) return 0.0;
    const double half = 0.5 * value[peak];

    auto edge = [&](int step) {
        std::size_t i = peak;
        for (;;) {
            const std::ptrdiff_t j = std::ptrdiff_t(i) + step;
            if (j < 0 || j >= std::ptrdiff_t(value.size())) return coord[i];
            if (value[std::size_t(j)] < half) {
                const double vi = value[i];
                const double vj = value[std::size_t(j)];
                const double f  = (vi > vj) ? (vi - half) / (vi - vj) : 0.0;
                return coord[i] + (coord[std::size_t(j)] - coord[i]) * f;
            }
            i = std::size_t(j);
        }
    };
    return std::fabs(edge(+1) - edge(-1));
}

} // namespace

SpotMetrics computeSpotMetrics(const SimulationResult& res) {
    SpotMetrics m;
    if (res.nx <= 0 || res.ny <= 0 || res.irradiance.empty()) return m;

    const double area = res.binArea();
    if (area <= 0.0) return m;

    m.valid     = true;
    m.totalBins = res.nx * res.ny;

    double sum = 0.0, sx = 0.0, sy = 0.0;
    double peak = 0.0, minLit = 0.0;
    int lit = 0;
    for (int iy = 0; iy < res.ny; ++iy) {
        for (int ix = 0; ix < res.nx; ++ix) {
            const double v = res.irradiance[std::size_t(iy) * std::size_t(res.nx) + std::size_t(ix)];
            if (v <= 0.0) continue;
            sum += v;
            sx  += v * res.binX(ix);
            sy  += v * res.binY(iy);
            peak = std::max(peak, v);
            minLit = (lit == 0) ? v : std::min(minLit, v);
            ++lit;
        }
    }

    m.total   = sum;
    m.litBins = lit;
    if (sum <= 0.0 || lit == 0) return m;

    m.centroidX = sx / sum;
    m.centroidY = sy / sum;
    // Densities, not raw bin flux: a 64 x 64 grid over a 320 mm receiver has
    // 25 mm^2 bins, and a number that changes when the binning changes is not a
    // measurement of the optic.
    m.peak       = peak / area;
    m.minLit     = minLit / area;
    m.mean       = (sum / double(lit)) / area;
    m.uniformity = (m.peak > 0.0) ? m.minLit / m.peak : 0.0;
    m.meanToPeak = (m.peak > 0.0) ? m.mean / m.peak : 0.0;

    // Second moment about the centroid, and the encircled-energy radii.
    double sumR2 = 0.0, rMax = 0.0;
    for (int iy = 0; iy < res.ny; ++iy) {
        const double dy = res.binY(iy) - m.centroidY;
        for (int ix = 0; ix < res.nx; ++ix) {
            const double v = res.irradiance[std::size_t(iy) * std::size_t(res.nx) + std::size_t(ix)];
            if (v <= 0.0) continue;
            const double dx = res.binX(ix) - m.centroidX;
            const double r2 = dx * dx + dy * dy;
            sumR2 += v * r2;
            rMax = std::max(rMax, std::sqrt(r2));
        }
    }
    m.rmsRadius = std::sqrt(sumR2 / sum);

    if (rMax > 0.0) {
        const double binW = rMax / double(kRadiusBins);
        std::vector<double> hist(kRadiusBins, 0.0);
        for (int iy = 0; iy < res.ny; ++iy) {
            const double dy = res.binY(iy) - m.centroidY;
            for (int ix = 0; ix < res.nx; ++ix) {
                const double v = res.irradiance[std::size_t(iy) * std::size_t(res.nx) + std::size_t(ix)];
                if (v <= 0.0) continue;
                const double dx = res.binX(ix) - m.centroidX;
                const int b = std::clamp(int(std::sqrt(dx * dx + dy * dy) / binW), 0, kRadiusBins - 1);
                hist[std::size_t(b)] += v;
            }
        }
        for (std::size_t i = 1; i < hist.size(); ++i) hist[i] += hist[i - 1];
        m.d50Radius = radiusAtFraction(hist, binW, 0.50);
        m.d86Radius = radiusAtFraction(hist, binW, 0.86);
    }

    // Chromaticity of the spot, from the arrivals' own wavelengths. This is the
    // reportable colour metric a spectral trace makes available: not "the
    // heatmap looks warm" but an (x, y) and a colour temperature.
    if (!res.arrivals.empty()) {
        double X = 0.0, Y = 0.0, Z = 0.0;
        for (const auto& a : res.arrivals) {
            double cx, cy, cz;
            spectrum::cie1931(double(a.wavelengthNm), cx, cy, cz);
            X += double(a.energy) * cx;
            Y += double(a.energy) * cy;
            Z += double(a.energy) * cz;
        }
        const double t = X + Y + Z;
        if (t > 0.0) {
            m.cieX = X / t;
            m.cieY = Y / t;
            m.cct  = spectrum::cctFromXy(m.cieX, m.cieY);
        }
    }

    const Profile px = crossSection(res, true,  m.centroidY);
    const Profile py = crossSection(res, false, m.centroidX);
    m.fwhmX = fwhmOf(px.coord, px.value);
    m.fwhmY = fwhmOf(py.coord, py.value);
    return m;
}

Profile crossSection(const SimulationResult& res, bool alongX, double atMm) {
    Profile p;
    if (res.nx <= 0 || res.ny <= 0 || res.irradiance.empty()) return p;
    const double area = res.binArea();
    if (area <= 0.0) return p;

    if (alongX) {
        // Nearest row to `atMm` in y.
        int row = 0;
        double best = 1e300;
        for (int iy = 0; iy < res.ny; ++iy) {
            const double d = std::fabs(res.binY(iy) - atMm);
            if (d < best) { best = d; row = iy; }
        }
        p.coord.reserve(std::size_t(res.nx));
        p.value.reserve(std::size_t(res.nx));
        for (int ix = 0; ix < res.nx; ++ix) {
            p.coord.push_back(res.binX(ix));
            p.value.push_back(res.irradiance[std::size_t(row) * std::size_t(res.nx) +
                                             std::size_t(ix)] / area);
        }
        p.axisLabel = QStringLiteral("x [mm]");
    } else {
        int col = 0;
        double best = 1e300;
        for (int ix = 0; ix < res.nx; ++ix) {
            const double d = std::fabs(res.binX(ix) - atMm);
            if (d < best) { best = d; col = ix; }
        }
        p.coord.reserve(std::size_t(res.ny));
        p.value.reserve(std::size_t(res.ny));
        for (int iy = 0; iy < res.ny; ++iy) {
            p.coord.push_back(res.binY(iy));
            p.value.push_back(res.irradiance[std::size_t(iy) * std::size_t(res.nx) +
                                             std::size_t(col)] / area);
        }
        p.axisLabel = QStringLiteral("y [mm]");
    }

    for (double v : p.value) p.peak = std::max(p.peak, v);
    return p;
}

EncircledEnergy encircledEnergy(const SimulationResult& res, int steps) {
    EncircledEnergy ee;
    const SpotMetrics m = computeSpotMetrics(res);
    if (!m.valid || m.total <= 0.0 || steps < 2) return ee;

    double rMax = 0.0;
    for (int iy = 0; iy < res.ny; ++iy) {
        const double dy = res.binY(iy) - m.centroidY;
        for (int ix = 0; ix < res.nx; ++ix) {
            if (res.irradiance[std::size_t(iy) * std::size_t(res.nx) + std::size_t(ix)] <= 0.0)
                continue;
            const double dx = res.binX(ix) - m.centroidX;
            rMax = std::max(rMax, std::sqrt(dx * dx + dy * dy));
        }
    }
    if (rMax <= 0.0) return ee;

    const double binW = rMax / double(steps);
    std::vector<double> hist(std::size_t(steps), 0.0);
    for (int iy = 0; iy < res.ny; ++iy) {
        const double dy = res.binY(iy) - m.centroidY;
        for (int ix = 0; ix < res.nx; ++ix) {
            const double v = res.irradiance[std::size_t(iy) * std::size_t(res.nx) + std::size_t(ix)];
            if (v <= 0.0) continue;
            const double dx = res.binX(ix) - m.centroidX;
            const int b = std::clamp(int(std::sqrt(dx * dx + dy * dy) / binW), 0, steps - 1);
            hist[std::size_t(b)] += v;
        }
    }
    for (std::size_t i = 1; i < hist.size(); ++i) hist[i] += hist[i - 1];

    ee.radius.reserve(std::size_t(steps));
    ee.fraction.reserve(std::size_t(steps));
    const double total = hist.back();
    for (int i = 0; i < steps; ++i) {
        ee.radius.push_back(double(i + 1) * binW);
        ee.fraction.push_back(total > 0.0 ? hist[std::size_t(i)] / total : 0.0);
    }
    return ee;
}

std::vector<FocusSample> throughFocus(const SimulationResult& res,
                                      double z0, double z1, int steps) {
    std::vector<FocusSample> out;
    if (res.arrivals.empty() || steps < 1) return out;

    const std::size_t stride =
        std::max<std::size_t>(1, res.arrivals.size() / kFocusSamples);

    // The arrivals that can be propagated at all, gathered once: a ray running
    // parallel to the receiver never crosses another plane, and re-testing that
    // per plane would cost a branch per arrival per step.
    struct Sample { double px, py, pz, dx, dy, dz, w; };
    std::vector<Sample> samples;
    samples.reserve(res.arrivals.size() / stride + 1);
    for (std::size_t i = 0; i < res.arrivals.size(); i += stride) {
        const DetectorArrival& a = res.arrivals[i];
        if (std::fabs(a.d.z) < 1e-9) continue;
        samples.push_back({a.p.x, a.p.y, a.p.z, a.d.x, a.d.y, a.d.z, double(a.energy)});
    }
    if (samples.empty()) return out;

    std::vector<double> x(samples.size()), y(samples.size());
    std::vector<double> hist(kRadiusBins);
    out.reserve(std::size_t(steps));

    for (int k = 0; k < steps; ++k) {
        const double z = (steps == 1) ? z0
                                      : z0 + (z1 - z0) * double(k) / double(steps - 1);

        // Propagate every arrival to this plane and find the flux centroid there.
        double sum = 0.0, sx = 0.0, sy = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const Sample& s = samples[i];
            const double t = (z - s.pz) / s.dz;
            x[i] = s.px + s.dx * t;
            y[i] = s.py + s.dy * t;
            sum += s.w;
            sx  += s.w * x[i];
            sy  += s.w * y[i];
        }
        if (sum <= 0.0) continue;

        FocusSample fs;
        fs.z = z;
        const double cx = sx / sum, cy = sy / sum;

        double sumR2 = 0.0, rMax = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const double dx = x[i] - cx, dy = y[i] - cy;
            const double r2 = dx * dx + dy * dy;
            sumR2 += samples[i].w * r2;
            rMax = std::max(rMax, r2);
        }
        rMax = std::sqrt(rMax);
        fs.rmsRadius = std::sqrt(sumR2 / sum);

        if (rMax > 0.0) {
            const double binW = rMax / double(kRadiusBins);
            std::fill(hist.begin(), hist.end(), 0.0);
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const double dx = x[i] - cx, dy = y[i] - cy;
                const int b = std::clamp(int(std::sqrt(dx * dx + dy * dy) / binW),
                                         0, kRadiusBins - 1);
                hist[std::size_t(b)] += samples[i].w;
            }
            for (std::size_t i = 1; i < hist.size(); ++i) hist[i] += hist[i - 1];
            fs.d86Radius = radiusAtFraction(hist, binW, 0.86);
        }

        if (fs.rmsRadius > 1e-9)
            fs.peakDensity = sum / (3.14159265358979323846 * fs.rmsRadius * fs.rmsRadius);
        out.push_back(fs);
    }
    return out;
}

double bestFocusZ(const std::vector<FocusSample>& sweep) {
    if (sweep.empty()) return 0.0;
    std::size_t best = 0;
    for (std::size_t i = 1; i < sweep.size(); ++i)
        if (sweep[i].rmsRadius < sweep[best].rmsRadius) best = i;
    if (best == 0 || best + 1 >= sweep.size()) return sweep[best].z;

    // Parabola through the three samples around the minimum. The sweep step is
    // usually coarser than the depth of focus, so the sampled minimum alone
    // would quantise the answer to the step size.
    const double x0 = sweep[best - 1].z, y0 = sweep[best - 1].rmsRadius;
    const double x1 = sweep[best].z,     y1 = sweep[best].rmsRadius;
    const double x2 = sweep[best + 1].z, y2 = sweep[best + 1].rmsRadius;
    const double d = (x0 - x1) * (x0 - x2) * (x1 - x2);
    if (std::fabs(d) < 1e-12) return x1;
    const double a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / d;
    const double b = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / d;
    if (std::fabs(a) < 1e-15) return x1;
    const double vertex = -b / (2.0 * a);
    // Only trust the fit inside the bracket it was built from.
    return (vertex >= x0 && vertex <= x2) ? vertex : x1;
}

// ---- export ----------------------------------------------------------------


// ---- image quality ---------------------------------------------------------

namespace {

// Line spread function along one axis of the receiver, from the arrivals.
//
// The arrivals are the point spread function sampled directly, so the line
// spread is a projection of them onto one axis -- no re-tracing, no assumption
// about the shape of the blur.
bool lineSpread(const SimulationResult& res, bool alongX, double centre,
                int samples, double& halfWidth, std::vector<double>& lsf) {
    if (res.arrivals.empty() || samples < 8) return false;

    // Wide enough to hold the whole blur, from the arrivals themselves rather
    // than from the receiver, which is usually far larger than the spot.
    double extent = 0.0;
    for (const auto& a : res.arrivals) {
        const double v = (alongX ? res.detu(a.p) : res.detv(a.p)) - centre;
        extent = std::max(extent, std::fabs(v));
    }
    if (!(extent > 0.0)) return false;
    halfWidth = extent * 1.05;

    lsf.assign(std::size_t(samples), 0.0);
    const double step = 2.0 * halfWidth / double(samples);
    double total = 0.0;
    for (const auto& a : res.arrivals) {
        const double v = (alongX ? res.detu(a.p) : res.detv(a.p)) - centre;
        const int b = int((v + halfWidth) / step);
        if (b < 0 || b >= samples) continue;
        lsf[std::size_t(b)] += double(a.energy);
        total += double(a.energy);
    }
    if (total <= 0.0) return false;
    for (double& v : lsf) v /= total;
    return true;
}

// |FFT| of a line spread function, evaluated directly at the frequencies wanted.
//
// A direct transform rather than a fast one: there are a hundred frequencies and
// a hundred samples, so it is ten thousand operations, and writing it out means
// the normalisation and the zero-frequency term are visible rather than implied.
void transform(const std::vector<double>& lsf, double halfWidth,
               const std::vector<double>& freq, std::vector<double>& mtf) {
    const std::size_t n = lsf.size();
    const double step = 2.0 * halfWidth / double(n);
    mtf.assign(freq.size(), 0.0);
    for (std::size_t k = 0; k < freq.size(); ++k) {
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double x = -halfWidth + (double(i) + 0.5) * step;
            const double phase = 2.0 * kPi * freq[k] * x;
            re += lsf[i] * std::cos(phase);
            im += lsf[i] * std::sin(phase);
        }
        // The line spread is already normalised to unit area, so the zero
        // frequency is one and no further scaling is needed.
        mtf[k] = std::min(1.0, std::sqrt(re * re + im * im));
    }
}

} // namespace

double airyRadiusMm(double fNumber, double wavelengthNm) {
    if (fNumber <= 0.0 || wavelengthNm <= 0.0) return 0.0;
    return 1.22 * (wavelengthNm * 1e-6) * fNumber;      // nm -> mm
}

Mtf modulationTransfer(const SimulationResult& res, int samples,
                       double apertureRadius, double focalLength) {
    Mtf out;
    const SpotMetrics m = computeSpotMetrics(res);
    if (!m.valid || res.arrivals.size() < 32) return out;

    double hx = 0.0, hy = 0.0;
    std::vector<double> lx, ly;
    const bool okX = lineSpread(res, true,  m.centroidX, samples, hx, lx);
    const bool okY = lineSpread(res, false, m.centroidY, samples, hy, ly);
    if (!okX && !okY) return out;

    // Out to where the blur itself stops carrying contrast: a few cycles across
    // the spot is where an MTF is read, and beyond it the curve is noise.
    const double scale = std::max(1e-6, std::max(hx, hy));
    const double fMax = 4.0 / scale;
    const int nf = 96;
    out.frequency.reserve(std::size_t(nf));
    for (int i = 0; i < nf; ++i)
        out.frequency.push_back(fMax * double(i) / double(nf - 1));

    if (okX) transform(lx, hx, out.frequency, out.tangential);
    else     out.tangential.assign(out.frequency.size(), 0.0);
    if (okY) transform(ly, hy, out.frequency, out.sagittal);
    else     out.sagittal.assign(out.frequency.size(), 0.0);

    // Where the average falls through a half.
    for (std::size_t i = 1; i < out.frequency.size(); ++i) {
        const double a = 0.5 * (out.tangential[i - 1] + out.sagittal[i - 1]);
        const double b = 0.5 * (out.tangential[i] + out.sagittal[i]);
        if (a >= 0.5 && b < 0.5) {
            const double t = (a - 0.5) / std::max(1e-12, a - b);
            out.cutoff50 = out.frequency[i - 1] +
                           t * (out.frequency[i] - out.frequency[i - 1]);
            break;
        }
    }

    // The diffraction limit, as the reference the geometric curve is judged
    // against: past it no design does better, however well corrected.
    if (apertureRadius > 0.0 && focalLength > 0.0) {
        const double lambdaMm = (res.meanWavelengthNm > 0.0 ? res.meanWavelengthNm : 587.6) * 1e-6;
        const double fNumber  = focalLength / (2.0 * apertureRadius);
        out.diffractionCutoff = 1.0 / (lambdaMm * fNumber);
        out.diffractionLimit.reserve(out.frequency.size());
        for (double f : out.frequency) {
            // The incoherent MTF of a circular aperture.
            const double nu = std::clamp(f / out.diffractionCutoff, 0.0, 1.0);
            out.diffractionLimit.push_back(
                (2.0 / kPi) * (std::acos(nu) - nu * std::sqrt(std::max(0.0, 1.0 - nu * nu))));
        }
    }

    out.valid = true;
    return out;
}

Wavefront wavefrontError(const SimulationResult& res, int bins) {
    Wavefront out;
    if (res.arrivals.size() < 16) return out;

    // Only the bundle that images. A wide receiver under a small optic collects
    // light that never went through it -- straight from the source, or round the
    // rim -- and those rays belong to no wavefront: their path lengths differ by
    // the geometry of the scene rather than by any aberration. Measuring inside
    // the encircled-energy radius keeps the core and leaves that halo out.
    const SpotMetrics spot = computeSpotMetrics(res);
    const double coreRadius = (spot.valid && spot.d86Radius > 0.0)
                                  ? 1.25 * spot.d86Radius : 0.0;
    const double cx = spot.valid ? spot.centroidX : res.detCX;
    const double cy = spot.valid ? spot.centroidY : res.detCY;
    auto inCore = [&](const DetectorArrival& a) {
        if (coreRadius <= 0.0) return true;
        const double dx = res.detu(a.p) - cx, dy = res.detv(a.p) - cy;
        return dx * dx + dy * dy <= coreRadius * coreRadius;
    };
    out.total = res.arrivals.size();
    for (const auto& a : res.arrivals)
        if (a.opl > 0.0f && inCore(a)) ++out.used;
    if (out.used < 16) return out;

    // Where the arrivals converge: the point closest to all of them, in the
    // least-squares sense. Minimising sum |(I - d d^T)(x - p)|^2 is a 3x3 solve.
    //
    // Fermat says every ray of a perfect image travels the same optical path
    // from source to image point, so measuring to the convergence point is what
    // makes the spread of those paths the *aberration* rather than the defocus
    // of wherever the receiver happens to sit.
    {
        double A[3][3] = {};
        double b[3] = {};
        for (const auto& a : res.arrivals) {
            if (!inCore(a)) continue;
            const double d[3] = {a.d.x, a.d.y, a.d.z};
            const double p[3] = {a.p.x, a.p.y, a.p.z};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    const double m = (i == j ? 1.0 : 0.0) - d[i] * d[j];
                    A[i][j] += m;
                    b[i]    += m * p[j];
                }
            }
        }
        // Cramer's rule on a 3x3; a collimated bundle makes it singular, and
        // there the mean path is the only reference available.
        const double det =
            A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
            A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
            A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        if (std::fabs(det) > 1e-9 * double(res.arrivals.size())) {
            auto solve = [&](int col) {
                double M[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) M[i][j] = (j == col) ? b[i] : A[i][j];
                return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                       M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                       M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
            };
            out.focus = Vec3(solve(0) / det, solve(1) / det, solve(2) / det);
            out.focusFound = true;
        }
    }

    // Each arrival's optical path carried on to that point. For a perfect image
    // this is the same number for every ray, whatever the receiver's position.
    auto pathOf = [&](const DetectorArrival& a) {
        const double extra = out.focusFound ? (out.focus - a.p).length() : 0.0;
        return double(a.opl) + extra;
    };

    double sum = 0.0, weight = 0.0;
    for (const auto& a : res.arrivals) {
        if (!(a.opl > 0.0) || !inCore(a)) continue;
        sum    += double(a.energy) * pathOf(a);
        weight += double(a.energy);
    }
    if (weight <= 0.0) return out;
    const double mean = sum / weight;

    const double lambdaMm = (res.meanWavelengthNm > 0.0 ? res.meanWavelengthNm : 587.6) * 1e-6;
    double m2 = 0.0, lo = 1e300, hi = -1e300;
    for (const auto& a : res.arrivals) {
        if (!(a.opl > 0.0) || !inCore(a)) continue;
        const double d = pathOf(a) - mean;
        m2 += double(a.energy) * d * d;
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    out.referenceOpl = mean;
    out.rmsWaves = std::sqrt(m2 / weight) / lambdaMm;
    out.ptvWaves = (hi - lo) / lambdaMm;
    // Marechal: good to about a quarter wave, which is exactly the region where
    // "diffraction limited" is the question being asked. Past that the
    // approximation collapses to zero and stops discriminating, so it is
    // reported alongside the flag that says whether to read it.
    const double phase = 2.0 * kPi * out.rmsWaves;
    out.strehl = std::exp(-std::min(phase * phase, 700.0));
    out.strehlMeaningful = out.rmsWaves <= 0.25;

    // The map, binned over the receiver.
    if (bins >= 4 && res.detW > 0.0 && res.detH > 0.0) {
        out.nx = bins;
        out.ny = bins;
        out.opd.assign(std::size_t(bins) * std::size_t(bins), 0.0);
        out.counts.assign(out.opd.size(), 0);
        for (const auto& a : res.arrivals) {
            if (!(a.opl > 0.0) || !inCore(a)) continue;
            const int bx =
                int((res.detu(a.p) - (res.detCX - 0.5 * res.detW)) / res.detW * bins);
            const int by =
                int((res.detv(a.p) - (res.detCY - 0.5 * res.detH)) / res.detH * bins);
            if (bx < 0 || bx >= bins || by < 0 || by >= bins) continue;
            const std::size_t k = std::size_t(by) * std::size_t(bins) + std::size_t(bx);
            out.opd[k] += (pathOf(a) - mean) / lambdaMm;
            ++out.counts[k];
        }
        for (std::size_t k = 0; k < out.opd.size(); ++k)
            if (out.counts[k] > 0) out.opd[k] /= double(out.counts[k]);
    }

    out.valid = true;
    return out;
}

QString irradianceCsv(const SimulationResult& res) {
    QString out;
    QTextStream ts(&out);
    const char* iu = irradianceUnitName(res.unit);
    ts << "# Detector irradiance, " << iu << "\n";
    ts << "# receiver " << res.detW << " x " << res.detH << " mm centred ("
       << res.detCX << ", " << res.detCY << ") at z = " << res.detZ << "\n";
    ts << "# rays " << res.raysEmitted << ", detector flux " << res.fluxDetector
       << ", efficiency " << res.efficiency << "\n";
    const double area = res.binArea();
    ts << "y\\x";
    for (int ix = 0; ix < res.nx; ++ix) ts << "," << res.binX(ix);
    ts << "\n";
    for (int iy = 0; iy < res.ny; ++iy) {
        ts << res.binY(iy);
        for (int ix = 0; ix < res.nx; ++ix) {
            const double v = res.irradiance[std::size_t(iy) * std::size_t(res.nx) + std::size_t(ix)];
            // Per square metre, not per square millimetre: W/m^2 and lux are the
            // units an irradiance is specified in.
            ts << "," << (area > 0.0 ? v / area * 1e6 : 0.0);
        }
        ts << "\n";
    }
    return out;
}

QString intensityCsv(const SimulationResult& res) {
    QString out;
    QTextStream ts(&out);
    const IntensityGrid& g = res.intensity;
    ts << "# Far-field intensity, " << intensityUnitName(res.unit) << "\n";
    if (!g.valid()) { ts << "# (no intensity data in this run)\n"; return out; }
    ts << "# peak " << g.peak << " /sr, FWHM " << g.fwhmDeg << " deg, total flux "
       << g.totalFlux << "\n";
    ts << "theta_deg,mean_over_phi";
    for (int ip = 0; ip < g.nPhi; ++ip)
        ts << ",phi_" << (double(ip) + 0.5) * 360.0 / double(g.nPhi);
    ts << "\n";
    for (int it = 0; it < g.nTheta; ++it) {
        ts << g.thetaCenterDeg(it) << "," << g.profile[std::size_t(it)];
        for (int ip = 0; ip < g.nPhi; ++ip)
            ts << "," << g.perSteradian[std::size_t(it) * std::size_t(g.nPhi) + std::size_t(ip)];
        ts << "\n";
    }
    return out;
}

QString metricsCsv(const SimulationResult& res, const SpotMetrics& m) {
    QString out;
    QTextStream ts(&out);
    const QString fu = QLatin1String(fluxUnitName(res.unit));
    const QString iu = QLatin1String(irradianceUnitName(res.unit));
    const QString au = QLatin1String(intensityUnitName(res.unit));
    ts << "metric,value,unit\n";
    ts << "rays_emitted," << res.raysEmitted << ",\n";
    ts << "detector_arrivals," << res.raysHitDetector << ",\n";
    ts << "source_flux," << res.sourcePower << "," << fu << "\n";
    ts << "luminous_efficacy," << res.luminousEfficacy << ",lm/W\n";
    ts << "mean_wavelength," << res.meanWavelengthNm << ",nm\n";
    ts << "detector_flux," << res.fluxDetector << "," << fu << "\n";
    ts << "efficiency," << res.efficiency << ",\n";
    ts << "efficiency_std_err," << res.efficiencyStdErr << ",\n";
    ts << "flux_absorbed," << res.fluxAbsorbed << "," << fu << "\n";
    ts << "flux_bulk_absorbed," << res.fluxBulkAbsorbed << "," << fu << "\n";
    ts << "flux_escaped," << res.fluxEscaped << "," << fu << "\n";
    ts << "flux_truncated," << res.fluxTruncated << "," << fu << "\n";
    ts << "flux_roulette_residual," << res.fluxRoulette << "," << fu << "\n";
    if (m.valid) {
        ts << "peak_irradiance," << m.peak * 1e6 << "," << iu << "\n";
        ts << "mean_irradiance," << m.mean * 1e6 << "," << iu << "\n";
        ts << "uniformity_min_over_peak," << m.uniformity << ",\n";
        ts << "uniformity_mean_over_peak," << m.meanToPeak << ",\n";
        ts << "centroid_x," << m.centroidX << ",mm\n";
        ts << "centroid_y," << m.centroidY << ",mm\n";
        ts << "rms_radius," << m.rmsRadius << ",mm\n";
        ts << "d50_radius," << m.d50Radius << ",mm\n";
        ts << "d86_radius," << m.d86Radius << ",mm\n";
        ts << "fwhm_x," << m.fwhmX << ",mm\n";
        ts << "fwhm_y," << m.fwhmY << ",mm\n";
        ts << "lit_bins," << m.litBins << ",\n";
        if (m.cieY > 0.0) {
            ts << "cie_x," << m.cieX << ",\n";
            ts << "cie_y," << m.cieY << ",\n";
            ts << "cct," << m.cct << ",K\n";
        }
    }
    if (res.intensity.valid()) {
        ts << "intensity_peak," << res.intensity.peak << "," << au << "\n";
        ts << "beam_fwhm," << res.intensity.fwhmDeg << ",deg\n";
    }
    return out;
}

namespace {

// Photometric intensity in candela, whatever unit the run was made in. A
// radiometric run converts through the source spectrum's luminous efficacy,
// which is the number that turns watts into lumens for *that* spectrum.
double toCandela(const SimulationResult& res, double perSr) {
    if (res.unit == FluxUnit::Lumen) return perSr;
    return perSr * res.luminousEfficacy;
}

double totalLumens(const SimulationResult& res) {
    const double f = res.intensity.valid() ? res.intensity.totalFlux : res.fluxDetector;
    return (res.unit == FluxUnit::Lumen) ? f : f * res.luminousEfficacy;
}

} // namespace

bool canExportPhotometry(const SimulationResult& res, QString* whyNot) {
    if (!res.intensity.valid()) {
        if (whyNot) *whyNot = QStringLiteral(
            "This run has no far-field data. Set the intensity binning above zero and trace again.");
        return false;
    }
    if (res.unit != FluxUnit::Lumen && res.luminousEfficacy <= 0.0) {
        if (whyNot) *whyNot = QStringLiteral(
            "The source spectrum carries no luminous flux, so there are no candela to write. "
            "Photometric files need a spectrum inside the visible band.");
        return false;
    }
    return true;
}

QString iesLm63(const SimulationResult& res, const QString& luminaire) {
    QString out;
    QTextStream ts(&out);
    QString why;
    if (!canExportPhotometry(res, &why)) {
        ts << "# " << why << "\n";
        return out;
    }
    const IntensityGrid& g = res.intensity;

    // IES LM-63-2002. Type C photometry: vertical angles from nadir, horizontal
    // angles around the axis -- which is exactly how the far-field grid is
    // binned, so no resampling is needed.
    ts << "IESNA:LM-63-2002\n";
    ts << "[TEST] LuxTrace Monte Carlo ray trace\n";
    ts << "[MANUFAC] LuxTrace\n";
    ts << "[LUMINAIRE] " << (luminaire.isEmpty() ? QStringLiteral("Traced optic") : luminaire) << "\n";
    ts << "[_RAYS] " << res.raysEmitted << "\n";
    ts << "[_EFFICIENCY] " << QString::number(100.0 * res.efficiency, 'f', 3) << " %\n";
    ts << "[MORE] Vertical angles are measured from +Z, which is the emission axis.\n";
    ts << "TILT=NONE\n";

    const double lumens = totalLumens(res);
    // One lamp, its rated lumens, a multiplier of one: the candela table is
    // absolute, so nothing here has to scale it.
    ts << "1 " << QString::number(lumens, 'f', 4) << " 1.0 "
       << g.nTheta << " " << g.nPhi << " 1 2 0 0 0\n";
    ts << "1.0 1.0 0.0\n";

    for (int it = 0; it < g.nTheta; ++it) {
        ts << QString::number(g.thetaCenterDeg(it), 'f', 3);
        ts << ((it + 1) % 12 == 0 || it + 1 == g.nTheta ? "\n" : " ");
    }
    for (int ip = 0; ip < g.nPhi; ++ip) {
        ts << QString::number((double(ip) + 0.5) * 360.0 / double(g.nPhi), 'f', 3);
        ts << ((ip + 1) % 12 == 0 || ip + 1 == g.nPhi ? "\n" : " ");
    }
    // Candela values, all vertical angles for the first horizontal angle, then
    // the next -- the order LM-63 specifies.
    for (int ip = 0; ip < g.nPhi; ++ip) {
        for (int it = 0; it < g.nTheta; ++it) {
            const double cd = toCandela(
                res, g.perSteradian[std::size_t(it) * std::size_t(g.nPhi) + std::size_t(ip)]);
            ts << QString::number(cd, 'f', 4);
            ts << ((it + 1) % 10 == 0 || it + 1 == g.nTheta ? "\n" : " ");
        }
    }
    return out;
}

QString eulumdat(const SimulationResult& res, const QString& luminaire) {
    QString out;
    QTextStream ts(&out);
    QString why;
    if (!canExportPhotometry(res, &why)) {
        ts << "# " << why << "\n";
        return out;
    }
    const IntensityGrid& g = res.intensity;
    const double lumens = totalLumens(res);

    auto line = [&](const QString& v) { ts << v << "\n"; };
    auto num  = [&](double v, int d = 2) { ts << QString::number(v, 'f', d) << "\n"; };

    line(QStringLiteral("LuxTrace"));                                  // 1 manufacturer
    line(QStringLiteral("1"));                                         // 2 Ityp: point source, symmetry about the axis
    line(QStringLiteral("0"));                                         // 3 Isym: no symmetry -- the full table is written
    ts << g.nPhi << "\n";                                              // 4 Mc: C-planes
    num(360.0 / double(g.nPhi));                                       // 5 Dc
    ts << g.nTheta << "\n";                                            // 6 Ng: gamma angles
    num(180.0 / double(g.nTheta));                                     // 7 Dg
    line(QStringLiteral("LuxTrace"));                                  // 8 measurement report
    line(luminaire.isEmpty() ? QStringLiteral("Traced optic") : luminaire);   // 9 luminaire
    line(QStringLiteral("-"));                                         // 10 luminaire number
    line(QStringLiteral("-"));                                         // 11 file name
    line(QStringLiteral("-"));                                         // 12 date/user
    for (int i = 0; i < 6; ++i) num(0.0);                              // 13..18 geometry, unknown
    num(100.0);                                                        // 19 downward flux fraction
    num(100.0);                                                        // 20 light output ratio
    num(1.0, 3);                                                       // 21 conversion factor
    num(0.0);                                                          // 22 tilt
    line(QStringLiteral("1"));                                         // 23 lamp sets
    line(QStringLiteral("1"));                                         // 24 lamps
    line(QStringLiteral("Traced"));                                    // 25 lamp type
    num(lumens, 3);                                                    // 26 total luminous flux
    line(QStringLiteral("-"));                                         // 27 colour appearance
    line(QStringLiteral("-"));                                         // 28 colour rendering group
    num(0.0);                                                          // 29 wattage
    for (int i = 0; i < 10; ++i) num(0.0);                             // 30 direct ratios

    for (int ip = 0; ip < g.nPhi; ++ip)
        num((double(ip) + 0.5) * 360.0 / double(g.nPhi));
    for (int it = 0; it < g.nTheta; ++it)
        num(g.thetaCenterDeg(it));

    // EULUMDAT carries intensity per 1000 lm, not absolute candela.
    const double per1000 = lumens > 0.0 ? 1000.0 / lumens : 0.0;
    for (int ip = 0; ip < g.nPhi; ++ip)
        for (int it = 0; it < g.nTheta; ++it) {
            const double cd = toCandela(
                res, g.perSteradian[std::size_t(it) * std::size_t(g.nPhi) + std::size_t(ip)]);
            num(cd * per1000, 3);
        }
    return out;
}

bool writeTextFile(const QString& path, const QString& text, QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts << text;
    ts.flush();
    if (f.error() != QFileDevice::NoError) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return true;
}

} // namespace analysis
