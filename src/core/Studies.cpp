#include "Studies.h"

#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace studies {

std::vector<ConvergencePoint> convergence(const SimConfig& base,
                                          int minRays, int maxRays, int points,
                                          const TraceControl& ctl) {
    std::vector<ConvergencePoint> out;
    minRays = std::max(16, minRays);
    maxRays = std::max(minRays, maxRays);
    points  = std::clamp(points, 2, 24);

    // Recording paths and arrivals is pure overhead here -- the study only reads
    // scalars -- and the far-field grid would be allocated per run for nothing.
    SimConfig cfg = base;
    cfg.nTheta = 0;
    cfg.nPhi   = 0;

    const double ratio = std::pow(double(maxRays) / double(minRays),
                                  1.0 / double(points - 1));
    out.reserve(std::size_t(points));
    for (int i = 0; i < points; ++i) {
        if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) break;

        const int rays = int(std::llround(double(minRays) * std::pow(ratio, double(i))));
        cfg.rays = std::max(1, rays);
        // A fresh seed per point: reusing one would make every point a prefix of
        // the same stream, so the curve would trend smoothly toward the answer
        // and hide exactly the scatter the error bars are there to show.
        cfg.seed = base.seed + 0x9E3779B9ull * std::uint64_t(i + 1);

        const SimulationResult res = Simulation::run(cfg);
        ConvergencePoint p;
        p.rays         = int(res.raysEmitted);
        p.efficiency   = res.efficiency;
        p.stdErr       = res.efficiencyStdErr;
        p.traceSeconds = res.traceSeconds;
        out.push_back(p);

        if (ctl.progress) ctl.progress(std::size_t(i + 1), std::size_t(points));
    }
    return out;
}

bool hasConverged(const std::vector<ConvergencePoint>& sweep, double sigmas) {
    if (sweep.size() < 2) return false;
    const ConvergencePoint& a = sweep[sweep.size() - 2];
    const ConvergencePoint& b = sweep.back();
    const double spread = sigmas * std::sqrt(a.stdErr * a.stdErr + b.stdErr * b.stdErr);
    return std::fabs(a.efficiency - b.efficiency) <= spread;
}

FocusStudy throughFocus(const SimulationResult& res, double spanMm, int steps) {
    FocusStudy s;
    s.receiverZ = res.detZ;
    if (res.arrivals.empty() || spanMm <= 0.0) return s;

    steps = std::clamp(steps, 3, 401);
    const double half = 0.5 * spanMm;
    s.samples = analysis::throughFocus(res, res.detZ - half, res.detZ + half, steps);
    if (s.samples.empty()) return s;

    s.bestZ = analysis::bestFocusZ(s.samples);
    s.bestRms = s.samples.front().rmsRadius;
    for (const auto& f : s.samples) s.bestRms = std::min(s.bestRms, f.rmsRadius);
    s.valid = true;
    return s;
}

QString convergenceCsv(const std::vector<ConvergencePoint>& sweep) {
    QString out;
    QTextStream ts(&out);
    ts << "rays,efficiency,std_err,trace_seconds\n";
    for (const auto& p : sweep)
        ts << p.rays << "," << p.efficiency << "," << p.stdErr << "," << p.traceSeconds << "\n";
    return out;
}

QString focusCsv(const FocusStudy& study) {
    QString out;
    QTextStream ts(&out);
    ts << "# receiver z = " << study.receiverZ << " mm, best focus z = " << study.bestZ
       << " mm, smallest RMS radius = " << study.bestRms << " mm\n";
    ts << "z_mm,rms_radius_mm,d86_radius_mm,relative_peak_density\n";
    for (const auto& f : study.samples)
        ts << f.z << "," << f.rmsRadius << "," << f.d86Radius << "," << f.peakDensity << "\n";
    return out;
}

} // namespace studies
