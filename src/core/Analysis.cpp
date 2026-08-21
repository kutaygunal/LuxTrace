#include "Analysis.h"

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

QString irradianceCsv(const SimulationResult& res) {
    QString out;
    QTextStream ts(&out);
    ts << "# Detector irradiance, flux per mm^2\n";
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
            ts << "," << (area > 0.0 ? v / area : 0.0);
        }
        ts << "\n";
    }
    return out;
}

QString intensityCsv(const SimulationResult& res) {
    QString out;
    QTextStream ts(&out);
    const IntensityGrid& g = res.intensity;
    ts << "# Far-field intensity, flux per steradian\n";
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
    ts << "metric,value,unit\n";
    ts << "rays_emitted," << res.raysEmitted << ",\n";
    ts << "detector_arrivals," << res.raysHitDetector << ",\n";
    ts << "detector_flux," << res.fluxDetector << ",W\n";
    ts << "efficiency," << res.efficiency << ",\n";
    ts << "efficiency_std_err," << res.efficiencyStdErr << ",\n";
    ts << "flux_absorbed," << res.fluxAbsorbed << ",W\n";
    ts << "flux_bulk_absorbed," << res.fluxBulkAbsorbed << ",W\n";
    ts << "flux_escaped," << res.fluxEscaped << ",W\n";
    ts << "flux_truncated," << res.fluxTruncated << ",W\n";
    if (m.valid) {
        ts << "peak_irradiance," << m.peak << ",W/mm^2\n";
        ts << "mean_irradiance," << m.mean << ",W/mm^2\n";
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
    }
    if (res.intensity.valid()) {
        ts << "intensity_peak," << res.intensity.peak << ",W/sr\n";
        ts << "beam_fwhm," << res.intensity.fwhmDeg << ",deg\n";
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
