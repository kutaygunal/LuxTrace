// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "Studies.h"
#include "MeshBuilder.h"
#include "Bsdf.h"
#include "Material.h"
#include "Optics.h"
#include "Polarisation.h"
#include "BackwardTracer.h"
#include "Spectrum.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

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



// ============================================================== metrics =====

QString metricName(Metric m) {
    switch (m) {
    case Metric::Efficiency:         return QStringLiteral("Efficiency");
    case Metric::RmsRadius:          return QStringLiteral("RMS spot radius");
    case Metric::D86Radius:          return QStringLiteral("D86 radius");
    case Metric::PeakIrradiance:     return QStringLiteral("Peak irradiance");
    case Metric::UniformityMinPeak:  return QStringLiteral("Uniformity (min/peak)");
    case Metric::UniformityMeanPeak: return QStringLiteral("Uniformity (mean/peak)");
    case Metric::BeamFwhmDeg:        return QStringLiteral("Beam FWHM");
    case Metric::PeakIntensity:      return QStringLiteral("Peak intensity");
    case Metric::SpotFwhmX:          return QStringLiteral("Spot FWHM (x)");
    case Metric::CentroidOffset:     return QStringLiteral("Centroid offset");
    case Metric::Count:              break;
    }
    return QStringLiteral("Efficiency");
}

QString metricUnit(Metric m, FluxUnit unit) {
    switch (m) {
    case Metric::Efficiency:         return QStringLiteral("%");
    case Metric::RmsRadius:
    case Metric::D86Radius:
    case Metric::SpotFwhmX:
    case Metric::CentroidOffset:     return QStringLiteral("mm");
    case Metric::PeakIrradiance:     return QLatin1String(irradianceUnitName(unit));
    case Metric::PeakIntensity:      return QLatin1String(intensityUnitName(unit));
    case Metric::BeamFwhmDeg:        return QStringLiteral("deg");
    case Metric::UniformityMinPeak:
    case Metric::UniformityMeanPeak:
    case Metric::Count:              break;
    }
    return QString();
}

QString metricTip(Metric m) {
    switch (m) {
    case Metric::Efficiency:
        return QStringLiteral("Fraction of the emitted flux that reaches the receiver.");
    case Metric::RmsRadius:
        return QStringLiteral("Flux-weighted RMS distance from the spot's own centroid, so an "
                              "off-axis spot is judged by its size rather than its offset.");
    case Metric::D86Radius:
        return QStringLiteral("Radius containing 86 % of the flux -- the beam-width convention "
                              "a laser datasheet uses.");
    case Metric::PeakIrradiance:
        return QStringLiteral("Brightest bin of the receiver, as a density.");
    case Metric::UniformityMinPeak:
        return QStringLiteral("Dimmest lit bin over the brightest. The strict specification an "
                              "illumination design is held to.");
    case Metric::UniformityMeanPeak:
        return QStringLiteral("Mean over peak. The gentler figure a beam is usually judged by.");
    case Metric::BeamFwhmDeg:
        return QStringLiteral("Full width of the far-field intensity at half its peak.");
    case Metric::PeakIntensity:
        return QStringLiteral("Brightest direction in the far field.");
    case Metric::SpotFwhmX:
        return QStringLiteral("Full width at half maximum of the profile through the centroid.");
    case Metric::CentroidOffset:
        return QStringLiteral("How far the spot's centre of flux sits from the receiver's.");
    case Metric::Count:
        break;
    }
    return QString();
}

bool metricBiggerIsBetter(Metric m) {
    switch (m) {
    case Metric::Efficiency:
    case Metric::PeakIrradiance:
    case Metric::UniformityMinPeak:
    case Metric::UniformityMeanPeak:
    case Metric::PeakIntensity:
        return true;
    default:
        return false;
    }
}

double metricStdErr(const SimulationResult& res, Metric m) {
    if (m == Metric::Efficiency) return 100.0 * res.efficiencyStdErr;
    return 0.0;
}

double metricValue(const SimulationResult& res, Metric m) {
    // Per cent, because that is the unit the metric is labelled with and the one
    // a reader compares against: an optimiser reporting "0.69 %" for a seventy
    // per cent design is a unit bug in a report, not a rounding detail.
    if (m == Metric::Efficiency) return 100.0 * res.efficiency;
    if (m == Metric::BeamFwhmDeg) return res.intensity.fwhmDeg;
    if (m == Metric::PeakIntensity) return res.intensity.peak;

    const analysis::SpotMetrics s = analysis::computeSpotMetrics(res);
    if (!s.valid) return 0.0;
    switch (m) {
    case Metric::RmsRadius:          return s.rmsRadius;
    case Metric::D86Radius:          return s.d86Radius;
    // Per square metre, so the number matches the one the readouts show.
    case Metric::PeakIrradiance:     return s.peak * 1e6;
    case Metric::UniformityMinPeak:  return s.uniformity;
    case Metric::UniformityMeanPeak: return s.meanToPeak;
    case Metric::SpotFwhmX:          return s.fwhmX;
    case Metric::CentroidOffset:
        return std::sqrt((s.centroidX - res.detCX) * (s.centroidX - res.detCX) +
                         (s.centroidY - res.detCY) * (s.centroidY - res.detCY));
    default:
        return 0.0;
    }
}

// ======================================================== parameter sweep ====

namespace {

// One point of a sweep, averaged over `repeats` independent seeds.
//
// A single seed gives a curve whose wiggles cannot be told from its noise. Two
// or more give a mean and a spread, and the spread is what says whether a bump
// is the design or the sampling.
SweepPoint evaluatePoint(SimConfig cfg, Metric metric, int repeats,
                         const TraceControl& ctl) {
    SweepPoint pt;
    repeats = std::max(1, repeats);
    std::vector<double> values;
    values.reserve(std::size_t(repeats));
    double effSum = 0.0, effErrSum = 0.0, seconds = 0.0;
    const std::uint64_t seed0 = cfg.seed;

    for (int r = 0; r < repeats; ++r) {
        if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) break;
        cfg.seed = seed0 + std::uint64_t(r) * 7919u;
        const SimulationResult res = Simulation::run(cfg);
        values.push_back(metricValue(res, metric));
        effSum    += res.efficiency;
        effErrSum += res.efficiencyStdErr * (metric == Metric::Efficiency ? 100.0 : 1.0);
        seconds   += res.traceSeconds;
    }
    if (values.empty()) return pt;

    for (double v : values) pt.value += v;
    pt.value /= double(values.size());
    pt.efficiency = effSum / double(values.size());
    pt.seconds    = seconds;

    if (values.size() > 1) {
        double m2 = 0.0;
        for (double v : values) m2 += (v - pt.value) * (v - pt.value);
        // Standard error of the mean over the seeds.
        pt.stdErr = std::sqrt(m2 / (double(values.size()) - 1.0) / double(values.size()));
    } else if (metric == Metric::Efficiency) {
        // One seed says nothing about a general metric, but the run's own
        // replica spread does say something about this one.
        pt.stdErr = effErrSum;
    }
    if (metric == Metric::Efficiency) {
        // Never claim less uncertainty than the trace itself reports.
        pt.stdErr = std::max(pt.stdErr, effErrSum / double(values.size()));
    }
    return pt;
}

} // namespace

std::vector<SweepPoint> parameterSweep(const SimConfig& base, int slotIndex,
                                       double from, double to, int steps,
                                       Metric metric, int repeats,
                                       const TraceControl& ctl) {
    std::vector<SweepPoint> out;
    // A sweep varies one of the registry's dimensions. Imported geometry has
    // none -- it is a file, not a parametric scene -- so every step would trace
    // the identical solid and the flat curve that came back would read as a
    // result rather than as the absence of one.
    if (base.tracesImport()) return out;
    const auto& infos = GeometryProvider::paramInfo(base.scene);
    if (slotIndex < 0 || slotIndex >= int(infos.size()) || steps < 1) return out;

    // Never outside what the scene says it can be built at.
    const SceneParamInfo& info = infos[std::size_t(slotIndex)];
    from = std::clamp(from, info.min, info.max);
    to   = std::clamp(to,   info.min, info.max);

    out.reserve(std::size_t(steps));
    for (int i = 0; i < steps; ++i) {
        if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) break;
        const double v = (steps == 1) ? from
                                      : from + (to - from) * double(i) / double(steps - 1);
        SimConfig cfg = base;
        cfg.useSceneDefaults = false;
        cfg.params = base.effectiveParams();
        cfg.params.v[slotIndex] = v;

        SweepPoint pt = evaluatePoint(cfg, metric, repeats, ctl);
        pt.parameter = v;
        out.push_back(pt);
        if (ctl.progress) ctl.progress(std::size_t(i + 1), std::size_t(steps));
    }
    return out;
}

Sweep2D parameterSweep2D(const SimConfig& base, int slotA, int slotB,
                         int stepsA, int stepsB, Metric metric,
                         const TraceControl& ctl) {
    Sweep2D out;
    const auto& infos = GeometryProvider::paramInfo(base.scene);
    if (slotA < 0 || slotB < 0 || slotA >= int(infos.size()) || slotB >= int(infos.size()) ||
        slotA == slotB || stepsA < 2 || stepsB < 2)
        return out;

    out.slotA = slotA;
    out.slotB = slotB;
    const SceneParamInfo& ia = infos[std::size_t(slotA)];
    const SceneParamInfo& ib = infos[std::size_t(slotB)];
    for (int i = 0; i < stepsA; ++i)
        out.a.push_back(ia.min + (ia.max - ia.min) * double(i) / double(stepsA - 1));
    for (int j = 0; j < stepsB; ++j)
        out.b.push_back(ib.min + (ib.max - ib.min) * double(j) / double(stepsB - 1));

    out.values.assign(out.a.size() * out.b.size(), 0.0);
    out.lo =  1e300;
    out.hi = -1e300;
    const std::size_t total = out.values.size();
    std::size_t done = 0;
    for (std::size_t jb = 0; jb < out.b.size(); ++jb)
        for (std::size_t ja = 0; ja < out.a.size(); ++ja) {
            if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) return out;
            SimConfig cfg = base;
            cfg.useSceneDefaults = false;
            cfg.params = base.effectiveParams();
            cfg.params.v[slotA] = out.a[ja];
            cfg.params.v[slotB] = out.b[jb];
            const double v = metricValue(Simulation::run(cfg), metric);
            out.values[jb * out.a.size() + ja] = v;
            out.lo = std::min(out.lo, v);
            out.hi = std::max(out.hi, v);
            if (ctl.progress) ctl.progress(++done, total);
        }
    return out;
}

QString sweepCsv(const std::vector<SweepPoint>& sweep, const SimConfig& cfg,
                 int slotIndex, Metric metric) {
    QString out;
    QTextStream ts(&out);
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    const QString pname = (slotIndex >= 0 && slotIndex < int(infos.size()))
                              ? infos[std::size_t(slotIndex)].name : QStringLiteral("parameter");
    const QString punit = (slotIndex >= 0 && slotIndex < int(infos.size()))
                              ? infos[std::size_t(slotIndex)].unit : QString();
    ts << "# parameter sweep of " << metricName(metric) << " against " << pname << "\n";
    ts << "# scene " << GeometryProvider::info(cfg.scene).name << ", " << cfg.rays << " rays\n";
    ts << pname << " [" << punit << "]," << metricName(metric) << " ["
       << metricUnit(metric, cfg.fluxUnit) << "],std_err,efficiency,seconds\n";
    for (const auto& p : sweep)
        ts << p.parameter << "," << p.value << "," << p.stdErr << "," << p.efficiency
           << "," << p.seconds << "\n";
    return out;
}

QString sweep2dCsv(const Sweep2D& sweep, const SimConfig& cfg, Metric metric) {
    QString out;
    QTextStream ts(&out);
    if (!sweep.valid()) { ts << "# (no data)\n"; return out; }
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    ts << "# " << metricName(metric) << " over "
       << infos[std::size_t(sweep.slotA)].name << " (columns) and "
       << infos[std::size_t(sweep.slotB)].name << " (rows)\n";
    ts << "b\\a";
    for (double a : sweep.a) ts << "," << a;
    ts << "\n";
    for (std::size_t jb = 0; jb < sweep.b.size(); ++jb) {
        ts << sweep.b[jb];
        for (std::size_t ja = 0; ja < sweep.a.size(); ++ja)
            ts << "," << sweep.at(int(ja), int(jb));
        ts << "\n";
    }
    return out;
}

// =========================================================== optimisation ====

namespace {

// The search works in a normalised box: every parameter runs 0..1 over its own
// declared range, so a radius in millimetres and an angle in degrees take the
// same size step and the simplex is not stretched along whichever happens to
// have the larger numbers.
struct Space {
    std::vector<int>    paramSlots;
    std::vector<double> lo, hi;
    SceneParams         base;

    SceneParams toParams(const std::vector<double>& x) const {
        SceneParams p = base;
        for (std::size_t i = 0; i < paramSlots.size(); ++i) {
            const double t = std::clamp(x[i], 0.0, 1.0);
            p.v[paramSlots[i]] = lo[i] + (hi[i] - lo[i]) * t;
        }
        return p;
    }
    std::vector<double> fromParams(const SceneParams& p) const {
        std::vector<double> x(paramSlots.size(), 0.5);
        for (std::size_t i = 0; i < paramSlots.size(); ++i) {
            const double span = hi[i] - lo[i];
            x[i] = span > 0.0 ? std::clamp((p.v[paramSlots[i]] - lo[i]) / span, 0.0, 1.0) : 0.5;
        }
        return x;
    }
};

// One evaluation of the design: build, trace, read the metric.
struct Evaluator {
    SimConfig           cfg;
    Space               space;
    Objective           objective;
    const TraceControl* ctl = nullptr;
    OptimisationResult* out = nullptr;
    int                 budget = 0;

    bool exhausted() const { return out->evaluations >= budget; }
    bool cancelled() const {
        return ctl && ctl->cancel && ctl->cancel->load(std::memory_order_relaxed);
    }

    double operator()(const std::vector<double>& x) {
        if (exhausted() || cancelled()) return 1e300;
        SimConfig c = cfg;
        c.useSceneDefaults = false;
        c.params = space.toParams(x);
        const SimulationResult res = Simulation::run(c);
        const double value = metricValue(res, objective.metric);
        const double m     = objective.merit(value);

        ++out->evaluations;
        if (!out->valid || m < objective.merit(out->bestValue)) {
            out->valid     = true;
            out->best      = c.params;
            out->bestValue = value;
        }
        OptimisationStep step;
        step.params     = c.params;
        step.value      = value;
        step.stdErr     = metricStdErr(res, objective.metric);
        step.merit      = objective.merit(out->bestValue);
        step.evaluation = out->evaluations;
        out->history.push_back(step);
        if (ctl && ctl->progress)
            ctl->progress(std::size_t(out->evaluations), std::size_t(budget));
        return m;
    }
};

// Nelder-Mead, with the standard coefficients. Derivative-free, tolerant of an
// objective that is not perfectly smooth, and short enough to read.
void nelderMead(Evaluator& eval, std::vector<double> start) {
    const std::size_t n = start.size();
    if (n == 0) return;

    std::vector<std::vector<double>> simplex;
    std::vector<double> f;
    simplex.push_back(start);
    f.push_back(eval(start));
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<double> p = start;
        // A step of a fifth of the range, away from whichever edge is nearer.
        p[i] += (p[i] > 0.5) ? -0.2 : 0.2;
        p[i] = std::clamp(p[i], 0.0, 1.0);
        simplex.push_back(p);
        f.push_back(eval(p));
    }

    constexpr double kReflect = 1.0, kExpand = 2.0, kContract = 0.5, kShrink = 0.5;
    while (!eval.exhausted() && !eval.cancelled()) {
        // Order: best first, worst last.
        std::vector<std::size_t> idx(simplex.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return f[a] < f[b]; });

        std::vector<std::vector<double>> ns;
        std::vector<double> nf;
        for (std::size_t i : idx) { ns.push_back(simplex[i]); nf.push_back(f[i]); }
        simplex.swap(ns);
        f.swap(nf);

        // Converged when the simplex has collapsed to a point in the normalised
        // box -- a thousandth of every range, which is finer than any of these
        // parameters can be manufactured to.
        double extent = 0.0;
        for (std::size_t i = 1; i < simplex.size(); ++i)
            for (std::size_t k = 0; k < n; ++k)
                extent = std::max(extent, std::fabs(simplex[i][k] - simplex[0][k]));
        if (extent < 1e-3) break;

        std::vector<double> centroid(n, 0.0);
        for (std::size_t i = 0; i + 1 < simplex.size(); ++i)
            for (std::size_t k = 0; k < n; ++k) centroid[k] += simplex[i][k];
        for (double& c : centroid) c /= double(simplex.size() - 1);

        auto along = [&](double t) {
            std::vector<double> p(n);
            for (std::size_t k = 0; k < n; ++k)
                p[k] = std::clamp(centroid[k] + t * (centroid[k] - simplex.back()[k]), 0.0, 1.0);
            return p;
        };

        const std::vector<double> refl = along(kReflect);
        const double fr = eval(refl);
        if (fr < f[0]) {
            const std::vector<double> exp = along(kExpand);
            const double fe = eval(exp);
            if (fe < fr) { simplex.back() = exp;  f.back() = fe; }
            else         { simplex.back() = refl; f.back() = fr; }
            continue;
        }
        if (fr < f[f.size() - 2]) { simplex.back() = refl; f.back() = fr; continue; }

        const std::vector<double> con = along(-kContract);
        const double fc = eval(con);
        if (fc < f.back()) { simplex.back() = con; f.back() = fc; continue; }

        // Nothing helped: pull the whole simplex in toward the best vertex.
        for (std::size_t i = 1; i < simplex.size(); ++i) {
            for (std::size_t k = 0; k < n; ++k)
                simplex[i][k] = simplex[0][k] + kShrink * (simplex[i][k] - simplex[0][k]);
            f[i] = eval(simplex[i]);
            if (eval.exhausted() || eval.cancelled()) return;
        }
    }
}

// A compact CMA-ES: a population drawn from a Gaussian whose mean and covariance
// are updated from the best of each generation. It learns which combinations of
// parameters matter together, which is what a simplex cannot do -- and what a
// valley running diagonally across two dimensions demands.
void cmaes(Evaluator& eval, std::vector<double> start, std::uint64_t seed) {
    const int n = int(start.size());
    if (n == 0) return;

    const int lambda = std::max(6, 4 + int(3.0 * std::log(double(n))));
    const int mu     = lambda / 2;
    // Two arguments, not one: `vector<double> w(size_t(mu))` parses as a
    // function declaration, which is the most vexing parse doing what it does.
    std::vector<double> weights(std::size_t(mu), 0.0);
    double wsum = 0.0, wsum2 = 0.0;
    for (int i = 0; i < mu; ++i) {
        weights[std::size_t(i)] = std::log(0.5 * (lambda + 1.0)) - std::log(double(i + 1));
        wsum += weights[std::size_t(i)];
    }
    for (double& v : weights) v /= wsum;
    for (double v : weights) wsum2 += v * v;
    const double muEff = 1.0 / wsum2;

    const double cc    = (4.0 + muEff / n) / (n + 4.0 + 2.0 * muEff / n);
    const double cs    = (muEff + 2.0) / (n + muEff + 5.0);
    const double c1    = 2.0 / ((n + 1.3) * (n + 1.3) + muEff);
    const double cmu   = std::min(1.0 - c1, 2.0 * (muEff - 2.0 + 1.0 / muEff) /
                                                ((n + 2.0) * (n + 2.0) + muEff));
    const double damps = 1.0 + 2.0 * std::max(0.0, std::sqrt((muEff - 1.0) / (n + 1.0)) - 1.0) + cs;
    const double chiN  = std::sqrt(double(n)) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));

    std::vector<double> mean = start;
    std::vector<double> ps(std::size_t(n), 0.0), pc(std::size_t(n), 0.0);
    // Diagonal covariance only: the full matrix needs an eigendecomposition per
    // generation, and a separable model already captures the scale differences
    // that matter most here.
    std::vector<double> diag(std::size_t(n), 1.0);
    double sigma = 0.25;
    std::uint64_t rng = optics::mix64(seed ^ 0x9E3779B97F4A7C15ull);

    while (!eval.exhausted() && !eval.cancelled()) {
        std::vector<std::vector<double>> z(std::size_t(lambda), std::vector<double>{});
        std::vector<std::vector<double>> x(std::size_t(lambda), std::vector<double>{});
        std::vector<double> fit(std::size_t(lambda), 0.0);
        for (int k = 0; k < lambda; ++k) {
            z[std::size_t(k)].resize(std::size_t(n));
            x[std::size_t(k)].resize(std::size_t(n));
            for (int i = 0; i < n; ++i) {
                z[std::size_t(k)][std::size_t(i)] = optics::gaussian(rng);
                x[std::size_t(k)][std::size_t(i)] =
                    std::clamp(mean[std::size_t(i)] +
                                   sigma * std::sqrt(diag[std::size_t(i)]) *
                                       z[std::size_t(k)][std::size_t(i)],
                               0.0, 1.0);
            }
            fit[std::size_t(k)] = eval(x[std::size_t(k)]);
            if (eval.exhausted() || eval.cancelled()) return;
        }

        std::vector<int> order(std::size_t(lambda), 0);
        for (int i = 0; i < lambda; ++i) order[std::size_t(i)] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return fit[std::size_t(a)] < fit[std::size_t(b)]; });

        std::vector<double> oldMean = mean;
        std::fill(mean.begin(), mean.end(), 0.0);
        std::vector<double> zmean(std::size_t(n), 0.0);
        for (int i = 0; i < mu; ++i) {
            const int k = order[std::size_t(i)];
            for (int d = 0; d < n; ++d) {
                mean[std::size_t(d)]  += weights[std::size_t(i)] * x[std::size_t(k)][std::size_t(d)];
                zmean[std::size_t(d)] += weights[std::size_t(i)] * z[std::size_t(k)][std::size_t(d)];
            }
        }

        double psNorm = 0.0;
        for (int d = 0; d < n; ++d) {
            ps[std::size_t(d)] = (1.0 - cs) * ps[std::size_t(d)] +
                                 std::sqrt(cs * (2.0 - cs) * muEff) * zmean[std::size_t(d)];
            psNorm += ps[std::size_t(d)] * ps[std::size_t(d)];
        }
        psNorm = std::sqrt(psNorm);

        const bool hsig = psNorm / chiN < 1.4 + 2.0 / (n + 1.0);
        for (int d = 0; d < n; ++d) {
            pc[std::size_t(d)] = (1.0 - cc) * pc[std::size_t(d)] +
                                 (hsig ? std::sqrt(cc * (2.0 - cc) * muEff) *
                                             (mean[std::size_t(d)] - oldMean[std::size_t(d)]) / sigma
                                       : 0.0);
            double rank = 0.0;
            for (int i = 0; i < mu; ++i) {
                const int k = order[std::size_t(i)];
                rank += weights[std::size_t(i)] * z[std::size_t(k)][std::size_t(d)] *
                        z[std::size_t(k)][std::size_t(d)];
            }
            diag[std::size_t(d)] = (1.0 - c1 - cmu) * diag[std::size_t(d)] +
                                   c1 * pc[std::size_t(d)] * pc[std::size_t(d)] +
                                   cmu * rank * diag[std::size_t(d)];
            diag[std::size_t(d)] = std::clamp(diag[std::size_t(d)], 1e-12, 1e6);
        }
        sigma *= std::exp((cs / damps) * (psNorm / chiN - 1.0));
        sigma = std::clamp(sigma, 1e-6, 1.0);
        // Collapsed onto a point: nothing left to search.
        if (sigma < 2e-4) break;
    }
}

} // namespace

OptimisationResult optimise(const SimConfig& base, const std::vector<int>& paramSlots,
                            const Objective& objective, Optimiser method,
                            int maxEvaluations, const TraceControl& ctl) {
    OptimisationResult out;
    out.method = (method == Optimiser::Cmaes) ? QStringLiteral("CMA-ES")
                                              : QStringLiteral("Nelder-Mead");
    // Nothing to search: imported geometry exposes no dimensions to move.
    if (base.tracesImport()) return out;
    const auto& infos = GeometryProvider::paramInfo(base.scene);

    Space space;
    space.base = base.effectiveParams();
    for (int s : paramSlots) {
        if (s < 0 || s >= int(infos.size())) continue;
        space.paramSlots.push_back(s);
        space.lo.push_back(infos[std::size_t(s)].min);
        space.hi.push_back(infos[std::size_t(s)].max);
    }
    if (space.paramSlots.empty()) return out;

    const auto t0 = std::chrono::steady_clock::now();

    Evaluator eval;
    eval.cfg       = base;
    eval.space     = space;
    eval.objective = objective;
    eval.ctl       = &ctl;
    eval.out       = &out;
    eval.budget    = std::max(int(space.paramSlots.size()) * 6, maxEvaluations);

    const std::vector<double> start = space.fromParams(space.base);
    // The starting design, so the report can say what the search bought.
    out.startValue = 0.0;
    {
        SimConfig c = base;
        c.useSceneDefaults = false;
        c.params = space.base;
        out.startValue = metricValue(Simulation::run(c), objective.metric);
    }

    if (method == Optimiser::Cmaes) cmaes(eval, start, base.seed);
    else                            nelderMead(eval, start);

    out.cancelled = ctl.cancel && ctl.cancel->load(std::memory_order_relaxed);
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

QString optimisationCsv(const OptimisationResult& r, const SimConfig& cfg,
                        const std::vector<int>& paramSlots, const Objective& objective) {
    QString out;
    QTextStream ts(&out);
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    ts << "# optimisation of " << metricName(objective.metric) << " on "
       << GeometryProvider::info(cfg.scene).name << " by " << r.method << "\n";
    ts << "# " << r.evaluations << " evaluations in "
       << QString::number(r.seconds, 'f', 2) << " s"
       << (r.cancelled ? ", cancelled" : "") << "\n";
    ts << "# start " << r.startValue << " -> best " << r.bestValue << " "
       << metricUnit(objective.metric, cfg.fluxUnit) << "\n";
    for (int s : paramSlots)
        if (s >= 0 && s < int(infos.size()))
            ts << "# " << infos[std::size_t(s)].name << " = " << r.best.v[s] << " "
               << infos[std::size_t(s)].unit << "\n";
    ts << "evaluation," << metricName(objective.metric) << ",best_so_far";
    for (int s : paramSlots)
        if (s >= 0 && s < int(infos.size())) ts << "," << infos[std::size_t(s)].name;
    ts << "\n";
    for (const auto& step : r.history) {
        ts << step.evaluation << "," << step.value << ","
           << (objective.goal == Objective::Goal::Maximise ? -step.merit : step.merit);
        for (int s : paramSlots)
            if (s >= 0 && s < int(infos.size())) ts << "," << step.params.v[s];
        ts << "\n";
    }
    return out;
}


// ============================================================ tolerancing ====

namespace {

// One perturbation of one parameter, from its declared distribution.
double perturb(const Tolerance& t, std::uint64_t& rng) {
    switch (t.shape) {
    case Tolerance::Shape::Uniform:
        return t.amount * (2.0 * optics::uniform01(rng) - 1.0);
    case Tolerance::Shape::Bimodal:
        // The worst case a supplier can ship while still passing inspection:
        // everything at one limit or the other, nothing in between.
        return optics::uniform01(rng) < 0.5 ? -t.amount : t.amount;
    case Tolerance::Shape::Gaussian:
    default:
        // The limit is three sigma, which is what a process under control means
        // and what a capability index is written against -- and it is what makes
        // the three shapes comparable, since all of them then span the same
        // +/- amount and differ only in how they fill it.
        return std::clamp(optics::gaussian(rng), -3.0, 3.0) * (t.amount / 3.0);
    }
}

double percentile(std::vector<double> sorted, double fraction, bool ascending) {
    if (sorted.empty()) return 0.0;
    std::sort(sorted.begin(), sorted.end());
    if (!ascending) fraction = 1.0 - fraction;
    const double pos = std::clamp(fraction, 0.0, 1.0) * double(sorted.size() - 1);
    const std::size_t lo = std::size_t(pos);
    const std::size_t hi = std::min(sorted.size() - 1, lo + 1);
    const double f = pos - double(lo);
    return sorted[lo] * (1.0 - f) + sorted[hi] * f;
}

} // namespace

ToleranceStudy tolerance(const SimConfig& base, const std::vector<Tolerance>& tolerances,
                         Metric metric, double criterion, bool passIsAbove,
                         int samples, const TraceControl& ctl) {
    ToleranceStudy out;
    out.metric      = metric;
    out.criterion   = criterion;
    out.passIsAbove = passIsAbove;

    // A tolerance is a perturbation of a declared dimension; an imported solid
    // declares none, so there is nothing to perturb.
    if (base.tracesImport()) return out;
    const auto& infos = GeometryProvider::paramInfo(base.scene);
    std::vector<Tolerance> active;
    for (const auto& t : tolerances)
        if (t.active() && t.slotIndex < int(infos.size())) active.push_back(t);
    if (active.empty() || samples < 4) return out;

    const auto t0 = std::chrono::steady_clock::now();
    const SceneParams nominal = base.effectiveParams();

    // The nominal design first, so the ensemble has something to be measured
    // against.
    {
        SimConfig c = base;
        c.useSceneDefaults = false;
        c.params = nominal;
        out.nominal = metricValue(Simulation::run(c), metric);
    }

    // Every sample uses the same trace seed, so the spread reported is the
    // tolerance rather than the Monte Carlo. The perturbations have a stream of
    // their own, seeded from the config, so the whole production run reproduces.
    std::uint64_t rng = optics::mix64(base.seed ^ 0x5DEECE66Dull);

    std::vector<double> values;
    std::vector<std::vector<double>> offsets(active.size());
    values.reserve(std::size_t(samples));
    for (auto& o : offsets) o.reserve(std::size_t(samples));

    for (int i = 0; i < samples; ++i) {
        if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) {
            out.cancelled = true;
            break;
        }
        SimConfig c = base;
        c.useSceneDefaults = false;
        c.params = nominal;
        for (std::size_t k = 0; k < active.size(); ++k) {
            const Tolerance& t = active[k];
            const double d = perturb(t, rng);
            offsets[k].push_back(d);
            const SceneParamInfo& info = infos[std::size_t(t.slotIndex)];
            c.params.v[t.slotIndex] = std::clamp(nominal.v[t.slotIndex] + d,
                                                 info.min, info.max);
        }
        values.push_back(metricValue(Simulation::run(c), metric));
        if (ctl.progress) ctl.progress(std::size_t(i + 1), std::size_t(samples));
    }
    if (values.size() < 4) return out;

    out.samples = int(values.size());
    out.values  = values;

    for (double v : values) out.mean += v;
    out.mean /= double(values.size());
    double m2 = 0.0;
    out.best = values[0];
    out.worst = values[0];
    for (double v : values) {
        m2 += (v - out.mean) * (v - out.mean);
        const bool better = metricBiggerIsBetter(metric) ? v > out.best : v < out.best;
        const bool worse  = metricBiggerIsBetter(metric) ? v < out.worst : v > out.worst;
        if (better) out.best = v;
        if (worse)  out.worst = v;
    }
    out.stdDev = std::sqrt(m2 / (double(values.size()) - 1.0));

    // Percentiles in the direction that makes them a promise: "90 % of parts
    // will be at least this good".
    out.median = percentile(values, 0.5, !metricBiggerIsBetter(metric));
    out.p90    = percentile(values, 0.9, !metricBiggerIsBetter(metric));
    out.p99    = percentile(values, 0.99, !metricBiggerIsBetter(metric));

    int passed = 0;
    for (double v : values) if (passIsAbove ? (v >= criterion) : (v <= criterion)) ++passed;
    out.yield = double(passed) / double(values.size());

    // Histogram.
    {
        double lo = values[0], hi = values[0];
        for (double v : values) { lo = std::min(lo, v); hi = std::max(hi, v); }
        constexpr int kBins = 24;
        if (hi - lo < 1e-12) { hi = lo + 1e-12; }
        out.binCentre.assign(kBins, 0.0);
        out.binCount.assign(kBins, 0);
        for (int b = 0; b < kBins; ++b)
            out.binCentre[std::size_t(b)] = lo + (hi - lo) * (double(b) + 0.5) / kBins;
        for (double v : values) {
            const int b = std::clamp(int((v - lo) / (hi - lo) * kBins), 0, kBins - 1);
            ++out.binCount[std::size_t(b)];
        }
    }

    // Which tolerance dominates. The gradient is the regression of the metric on
    // each offset over the ensemble that was already traced, so it costs nothing
    // beyond the run -- and the share is how much of the spread it explains.
    double totalShare = 0.0;
    for (std::size_t k = 0; k < active.size(); ++k) {
        ToleranceStudy::Sensitivity sens;
        sens.slotIndex = active[k].slotIndex;
        sens.name      = infos[std::size_t(active[k].slotIndex)].name;
        sens.amount    = active[k].amount;

        double meanX = 0.0;
        for (double x : offsets[k]) meanX += x;
        meanX /= double(offsets[k].size());
        double sxy = 0.0, sxx = 0.0;
        for (std::size_t i = 0; i < offsets[k].size() && i < values.size(); ++i) {
            const double dx = offsets[k][i] - meanX;
            sxy += dx * (values[i] - out.mean);
            sxx += dx * dx;
        }
        sens.gradient = sxx > 1e-18 ? sxy / sxx : 0.0;
        // Variance this tolerance contributes: the gradient times its own spread.
        const double sd = sxx > 0.0 ? std::sqrt(sxx / double(offsets[k].size())) : 0.0;
        sens.share = sens.gradient * sens.gradient * sd * sd;
        totalShare += sens.share;
        out.sensitivity.push_back(sens);
    }
    if (totalShare > 1e-30)
        for (auto& sens : out.sensitivity) sens.share /= totalShare;
    std::sort(out.sensitivity.begin(), out.sensitivity.end(),
              [](const ToleranceStudy::Sensitivity& a, const ToleranceStudy::Sensitivity& b) {
                  return a.share > b.share;
              });

    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out.valid = true;
    return out;
}

QString toleranceCsv(const ToleranceStudy& study, const SimConfig& cfg) {
    QString out;
    QTextStream ts(&out);
    if (!study.valid) { ts << "# (no tolerance study)\n"; return out; }

    ts << "# tolerance analysis of " << metricName(study.metric) << " on "
       << GeometryProvider::info(cfg.scene).name << "\n";
    ts << "# " << study.samples << " samples in "
       << QString::number(study.seconds, 'f', 1) << " s, seed " << cfg.seed
       << " (the same seed reproduces the same production run)\n";
    ts << "# nominal " << study.nominal << ", mean " << study.mean
       << ", sd " << study.stdDev << "\n";
    ts << "# yield " << QString::number(100.0 * study.yield, 'f', 1) << " % "
       << (study.passIsAbove ? "at or above " : "at or below ") << study.criterion << "\n";
    ts << "# 50/90/99 % of parts beat " << study.median << " / " << study.p90
       << " / " << study.p99 << "\n";

    ts << "\nsensitivity,parameter,tolerance,gradient,variance_share\n";
    for (const auto& s : study.sensitivity)
        ts << "," << s.name << "," << s.amount << "," << s.gradient << "," << s.share << "\n";

    ts << "\nhistogram," << metricName(study.metric) << ",count\n";
    for (std::size_t i = 0; i < study.binCentre.size(); ++i)
        ts << "," << study.binCentre[i] << "," << study.binCount[i] << "\n";

    ts << "\nsample," << metricName(study.metric) << "\n";
    for (std::size_t i = 0; i < study.values.size(); ++i)
        ts << i + 1 << "," << study.values[i] << "\n";
    return out;
}

// ============================================================== validation ==
// Every case below compares the tracer against a closed form derived outside
// it. Recomputing the same equation the implementation uses would only prove it
// matches itself; these are the textbook results an evaluator already trusts.

namespace {

constexpr double kValPi = 3.14159265358979323846;

OpticalSurface planeDetector(double size, double z, int bins) {
    const double h = 0.5 * size;
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(-h, -h, z));
    poly.Add(gp_Pnt(h, -h, z));
    poly.Add(gp_Pnt(h, h, z));
    poly.Add(gp_Pnt(-h, h, z));
    poly.Close();
    OpticalSurface o;
    o.shape      = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    o.label      = QStringLiteral("Detector");
    o.isDetector = true;
    o.detNX = o.detNY = bins;
    return o;
}

// A thin plano-convex lens: flat face at z = 0, curved face of radius `R`
// bulging to z = t. With the flat side first the second principal plane sits on
// the curved vertex, so the focus is one focal length past z = t exactly.
OpticalSurface planoConvex(double R, double t, const OpticalMaterial& mat, double aperture) {
    const TopoDS_Shape sphere =
        BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, t - R), R).Shape();
    const TopoDS_Shape slab =
        BRepPrimAPI_MakeBox(gp_Pnt(-aperture, -aperture, 0.0), 2 * aperture, 2 * aperture, t).Shape();
    OpticalSurface o;
    o.shape          = BRepAlgoAPI_Common(sphere, slab).Shape();
    o.label          = QStringLiteral("Plano-Convex");
    o.material       = mat;
    o.index          = mat.nd;
    o.absorption     = 0.0;          // the closed form is geometric, not lossy
    o.transmissivity = 1.0;
    // Fresnel off on purpose. The lensmaker's equation is a statement about
    // refraction, and a 4 % reflection off the exit face returns a full-weight
    // ghost through the lens under the collapsed estimator -- a broad halo that
    // an RMS radius, which weights outliers by r squared, would then be
    // measuring instead of the focus.
    o.fresnel        = false;
    o.reflectivity   = 0.0;
    o.meshAngle      = 0.02;
    return o;
}

// A speck of geometry far outside everything, so a "scene" that is really just
// a source is still a scene the tracer will run. Without it the empty-scene
// short circuit reports every ray as escaped and bins no far field.
OpticalSurface distantSpeck() {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(1e6, 1e6, 1e6));
    poly.Add(gp_Pnt(1e6 + 0.1, 1e6, 1e6));
    poly.Add(gp_Pnt(1e6, 1e6 + 0.1, 1e6));
    poly.Close();
    OpticalSurface o;
    o.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    o.label = QStringLiteral("Far speck");
    return o;
}

ValidationCase makeCase(const QString& name, const QString& reference, const QString& unit,
                        double expected, double measured, double tolerance) {
    ValidationCase c;
    c.name      = name;
    c.reference = reference;
    c.unit      = unit;
    c.expected  = expected;
    c.measured  = measured;
    c.tolerance = tolerance;
    c.passed    = std::fabs(measured - expected) <= tolerance;
    return c;
}

// ---- 1. the lensmaker's equation -------------------------------------------

ValidationCase validateLensmaker(int rays) {
    const OpticalMaterial bk7 = materials::byName(QStringLiteral("N-BK7"));
    const double R = 100.0, t = 6.0, aperture = 60.0;
    const double n = bk7.nd;
    const double f = R / (n - 1.0);
    const double focusZ = t + f;

    std::vector<OpticalSurface> surfaces{planoConvex(R, t, bk7, aperture),
                                         planeDetector(400.0, focusZ + 150.0, 64)};
    TraceScene scene;
    scene.build(MeshBuilder::build(surfaces));

    SourceConfig src;
    src.type       = SourceConfig::Type::Collimated;
    // A narrow bundle: the lensmaker's equation is paraxial, and a full-aperture
    // beam would be measuring spherical aberration instead.
    src.beamRadius = 4.0;
    src.origin     = gp_Pnt(0, 0, -40.0);
    src.axis       = gp_Dir(0, 0, 1);
    src.rays       = std::max(20000, rays / 4);

    TraceOptions opt;
    opt.nTheta = 0;
    opt.arrivalCap = 200000;
    SimulationResult res;
    RayTracer::trace(scene, src, res, opt);

    const auto sweep = analysis::throughFocus(res, focusZ - 40.0, focusZ + 40.0, 321);
    const double measured = analysis::bestFocusZ(sweep);

    return makeCase(QStringLiteral("Plano-convex back focus"),
                    QStringLiteral("f = R / (n - 1), n_d = %1, R = %2 mm")
                        .arg(n, 0, 'f', 4).arg(R, 0, 'f', 0),
                    QStringLiteral("mm"), focusZ, measured, 1.0);
}

// ---- 2. the integrating-sphere multiplier ----------------------------------

ValidationCase validateSphereMultiplier(int rays) {
    // A sphere of radius R with a flat receiver closing off a cap of depth
    // `delta`. The cap is exactly the port: its area is 2 pi R delta, so the
    // port fraction is f = delta / 2R -- which is also the solid-angle fraction
    // the cap subtends from the centre, where the source sits.
    const double R = 100.0, delta = 8.0, rho = 0.92;
    const double f = delta / (2.0 * R);
    const double M = rho / (1.0 - rho * (1.0 - f));
    // Straight out through the port, plus everything that comes back round.
    const double expected = f + (1.0 - f) * f * M;

    OpticalSurface wall;
    wall.shape        = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), R).Shape();
    wall.label        = QStringLiteral("Cavity");
    wall.reflectivity = rho;
    wall.scatter      = 1.0;          // perfectly Lambertian paint
    wall.meshAngle    = 0.03;

    // Wide enough to cover the whole chord circle of the cap.
    const double port = 1.2 * std::sqrt(2.0 * R * delta);
    std::vector<OpticalSurface> surfaces{wall, planeDetector(2.0 * port, R - delta, 16)};
    TraceScene scene;
    scene.build(MeshBuilder::build(surfaces));

    SourceConfig src;
    src.type         = SourceConfig::Type::Point;
    src.halfAngleDeg = 180.0;
    src.origin       = gp_Pnt(0, 0, 0);
    src.rays         = std::max(50000, rays);

    TraceOptions opt;
    opt.nTheta = 0;
    SimulationResult res;
    RayTracer::trace(scene, src, res, opt);

    return makeCase(QStringLiteral("Integrating-sphere throughput"),
                    QStringLiteral("M = rho / (1 - rho(1 - f)), rho = %1, f = %2")
                        .arg(rho, 0, 'f', 2).arg(f, 0, 'f', 4),
                    QStringLiteral("fraction"), expected, res.efficiency,
                    // Three standard errors, floored so a lucky run is not a
                    // tighter claim than the sampling supports.
                    std::max(0.002, 3.0 * res.efficiencyStdErr));
}

// ---- 3. inverse square, and the cosine-cubed falloff -----------------------

struct PointSourceRun {
    // Flux-weighted mean of measured / predicted over every bin with signal.
    // Reading two averaged patches instead put the whole answer on the few bins
    // near the axis, where a Monte Carlo grid is at its noisiest; every bin
    // carries the same law, so every bin should be used to test it.
    double lawRatio  = 0.0;
    double worstBin  = 0.0;   // worst relative departure among well-sampled bins
    double totalFlux = 0.0;
    double stdErr    = 0.0;
};

PointSourceRun runPointSourceOnPlane(double h, double halfSize, int bins, int rays) {
    std::vector<OpticalSurface> surfaces{planeDetector(2.0 * halfSize, h, bins)};
    TraceScene scene;
    scene.build(MeshBuilder::build(surfaces));

    SourceConfig src;
    src.type         = SourceConfig::Type::Point;
    src.halfAngleDeg = 180.0;
    src.origin       = gp_Pnt(0, 0, 0);
    src.rays         = rays;

    TraceOptions opt;
    opt.nTheta = 0;
    SimulationResult res;
    RayTracer::trace(scene, src, res, opt);

    PointSourceRun out;
    out.totalFlux = res.fluxDetector;
    out.stdErr    = res.efficiencyStdErr;
    const double area = res.binArea();
    if (area <= 0.0) return out;

    // E(r) = I h / (r^2 + h^2)^(3/2): the inverse square over the slant range,
    // times the cosine of the angle the plane is tilted through at that point.
    const double I = 1.0 / (4.0 * kValPi);
    const double perBin = res.raysEmitted > 0 ? res.fluxDetector / double(res.raysHitDetector)
                                              : 0.0;
    double wsum = 0.0, rsum = 0.0;
    for (int iy = 0; iy < res.ny; ++iy)
        for (int ix = 0; ix < res.nx; ++ix) {
            const double x = res.binX(ix), y = res.binY(iy);
            const double d2 = x * x + y * y + h * h;
            const double want = I * h / (d2 * std::sqrt(d2));
            const double got  = res.irradiance[std::size_t(iy) * std::size_t(res.nx) +
                                               std::size_t(ix)] / area;
            if (want <= 0.0 || got <= 0.0) continue;
            // Weighted by the PREDICTION, not by the measurement. Weighting a
            // ratio by its own numerator biases it upward by the relative
            // variance of the bins -- which on a Monte Carlo grid is exactly the
            // few per cent the law is being checked to.
            const double w = want;
            wsum += w;
            rsum += w * (got / want);
            // Only bins holding enough arrivals to say anything are eligible for
            // the worst-case figure.
            if (perBin > 0.0 && got * area / perBin > 400.0)
                out.worstBin = std::max(out.worstBin, std::fabs(got / want - 1.0));
        }
    if (wsum > 0.0) out.lawRatio = rsum / wsum;
    return out;
}

void validatePointSource(std::vector<ValidationCase>& cases, int rays) {
    const double h = 200.0, halfSize = 150.0;
    const PointSourceRun r = runPointSourceOnPlane(h, halfSize, 48, std::max(600000, 4 * rays));

    cases.push_back(makeCase(QStringLiteral("Inverse-square cosine law"),
                             QStringLiteral("E(r) = I h / (r^2 + h^2)^(3/2), I = Phi/4pi, "
                                            "h = %1 mm; flux-weighted over every bin")
                                 .arg(h, 0, 'f', 0),
                             QStringLiteral("ratio"), 1.0, r.lawRatio, 0.015));

    // And the total on the receiver is the solid angle it subtends.
    const double a = halfSize;
    const double omega = 4.0 * std::asin((a * a) / (a * a + h * h));
    const double expectedFlux = omega / (4.0 * kValPi);
    cases.push_back(makeCase(QStringLiteral("Flux into a square receiver"),
                             QStringLiteral("Omega = 4 asin(a^2 / (a^2 + h^2))"),
                             QStringLiteral("fraction"), expectedFlux, r.totalFlux,
                             std::max(0.002, 4.0 * r.stdErr)));
}

// ---- 4. the concentration limit --------------------------------------------

ValidationCase validateCpcConcentration() {
    // A CPC of half-acceptance theta concentrates by 1 / sin^2 theta in area,
    // so its entrance and exit radii stand in the ratio 1 / sin theta. That is a
    // property of the profile the app generates, checkable without a ray.
    const SceneParams p = GeometryProvider::defaultParams(GeometryProvider::Scene::Cpc);
    const double aExit = p.v[0], thetaDeg = p.v[1];
    const auto setup = GeometryProvider::build(GeometryProvider::Scene::Cpc, p);

    double rIn = 0.0;
    const MeshList meshes = MeshBuilder::build(setup.surfaces);
    for (const auto& m : meshes) {
        if (m.isDetector) continue;
        for (const auto& v : m.verts)
            rIn = std::max(rIn, std::sqrt(v.X() * v.X() + v.Y() * v.Y()));
    }

    const double expected = 1.0 / std::sin(thetaDeg * kValPi / 180.0);
    const double measured = aExit > 0.0 ? rIn / aExit : 0.0;
    return makeCase(QStringLiteral("CPC concentration ratio"),
                    QStringLiteral("a_in / a_out = 1 / sin(theta_max), theta_max = %1 deg")
                        .arg(thetaDeg, 0, 'f', 1),
                    QStringLiteral("ratio"), expected, measured, 0.05 * expected);
}

// ---- 5. minimum deviation through a prism ----------------------------------

ValidationCase validatePrismDeviation() {
    // An isosceles prism of apex angle A. Sweeping the angle of incidence and
    // taking the smallest deviation the tracer produces has to land on
    // delta_min = 2 asin(n sin(A/2)) - A.
    const double A = 60.0 * kValPi / 180.0;
    const double n = 1.5168;               // N-BK7 at the d line
    const double halfBase = 40.0;
    const double height   = halfBase / std::tan(0.5 * A);

    BRepBuilderAPI_MakePolygon tri;
    tri.Add(gp_Pnt(-halfBase, -60.0, 0.0));
    tri.Add(gp_Pnt(halfBase, -60.0, 0.0));
    tri.Add(gp_Pnt(0.0, -60.0, height));
    tri.Close();
    OpticalSurface prism;
    prism.shape = BRepPrimAPI_MakePrism(
        BRepBuilderAPI_MakeFace(tri.Wire()).Face(), gp_Vec(0, 120.0, 0)).Shape();
    prism.label          = QStringLiteral("Prism");
    prism.index          = n;
    prism.transmissivity = 1.0;
    prism.reflectivity   = 0.0;
    prism.fresnel        = false;    // the deviation is refraction, not reflection

    // The apex points up, so a deviated ray leaves downward: the receiver goes
    // below the prism, far enough that what it records is the exit direction.
    std::vector<OpticalSurface> surfaces{prism, planeDetector(40000.0, -4000.0, 8)};
    TraceScene scene;
    scene.build(MeshBuilder::build(surfaces));

    double best = 1e9;
    for (int i = 0; i <= 240; ++i) {
        const double inc = (25.0 + 0.25 * double(i)) * kValPi / 180.0;   // 25..85 degrees
        // The left face runs from the base corner to the apex, so its outward
        // normal points up and to the left. The incidence angle is measured
        // against that normal.
        const Vec3 faceN(-std::cos(0.5 * A), 0.0, std::sin(0.5 * A));
        const Vec3 tangent(std::sin(0.5 * A), 0.0, std::cos(0.5 * A));   // base -> apex
        Vec3 dir = -faceN * std::cos(inc) + tangent * std::sin(inc);
        dir.normalize();
        // Halfway up the face.
        const Vec3 target(-halfBase, 0.0, 0.0);
        const Vec3 entry  = target + tangent * (0.5 * std::sqrt(halfBase * halfBase +
                                                                height * height));
        const Vec3 origin = entry - dir * 200.0;

        SimulationResult one;
        RayTracer::traceSingleRay(scene, origin, dir, one, 1.0, PhysicsOptions{}, 587.6);
        if (one.arrivals.empty()) continue;
        // The transmitted path is the one carrying the energy; a stray internal
        // branch that happened to land is not the deviation being measured.
        const DetectorArrival* strongest = &one.arrivals.front();
        for (const auto& a : one.arrivals)
            if (a.energy > strongest->energy) strongest = &a;
        if (double(strongest->energy) < 0.5) continue;
        const double dev = std::acos(std::clamp(strongest->d.dot(dir), -1.0, 1.0));
        if (dev > 1e-4) best = std::min(best, dev);
    }

    const double expected = (2.0 * std::asin(n * std::sin(0.5 * A)) - A) * 180.0 / kValPi;
    const double measured = best < 1e8 ? best * 180.0 / kValPi : 0.0;
    return makeCase(QStringLiteral("Prism minimum deviation"),
                    QStringLiteral("delta = 2 asin(n sin(A/2)) - A, A = 60 deg, n = %1")
                        .arg(n, 0, 'f', 4),
                    QStringLiteral("deg"), expected, measured, 0.5);
}

// ---- 6. Lambert's cosine law -----------------------------------------------

ValidationCase validateLambertianFarField(int rays) {
    std::vector<OpticalSurface> surfaces{distantSpeck()};
    TraceScene scene;
    scene.build(MeshBuilder::build(surfaces));

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.shape        = SourceConfig::Shape::Disc;
    src.sizeA        = 5.0;
    src.halfAngleDeg = 90.0;
    src.axis         = gp_Dir(0, 0, 1);
    src.rays         = std::max(600000, 4 * rays);

    TraceOptions opt;
    opt.nTheta = 90;
    opt.nPhi   = 36;
    SimulationResult res;
    RayTracer::trace(scene, src, res, opt);

    // Worst relative departure from I(theta) = I0 cos(theta), out to 70 degrees.
    // The rings are grouped in fives first: a single 2-degree ring near the pole
    // holds few enough rays that its scatter, not the cosine law, would be what
    // the number reports.
    constexpr int kGroup = 5;
    double worst = 0.0;
    // I0 from the innermost group rather than from the single brightest ring,
    // for the same reason.
    double I0 = 0.0;
    {
        double sum = 0.0;
        for (int i = 0; i < kGroup && i < res.intensity.nTheta; ++i)
            sum += res.intensity.profile[std::size_t(i)] /
                   std::cos(res.intensity.thetaCenterDeg(i) * kValPi / 180.0);
        I0 = sum / double(kGroup);
    }
    if (I0 > 0.0) {
        for (int g = 0; g + kGroup <= res.intensity.nTheta; g += kGroup) {
            double got = 0.0, want = 0.0;
            bool tooFar = false;
            for (int i = g; i < g + kGroup; ++i) {
                const double th = res.intensity.thetaCenterDeg(i) * kValPi / 180.0;
                if (th > 70.0 * kValPi / 180.0) { tooFar = true; break; }
                got  += res.intensity.profile[std::size_t(i)];
                want += I0 * std::cos(th);
            }
            if (tooFar) break;
            if (want > 0.0) worst = std::max(worst, std::fabs(got - want) / want);
        }
    }
    return makeCase(QStringLiteral("Lambertian far field"),
                    QStringLiteral("I(theta) = I0 cos(theta), worst departure to 70 deg, rings grouped in fives"),
                    QStringLiteral("relative"), 0.0, worst, 0.04);
}

} // namespace

// ---- physics checks against the closed forms they were built from ----------
//
// The seven original cases test the tracer end to end: build a scene, fire rays,
// compare the answer to optics derived outside it. These six test the layers
// underneath, where a wrong number is hardest to see because nothing about the
// picture looks wrong. All six are deterministic or nearly so, and none of them
// costs a trace.

// Fresnel at an air/glass interface, against the closed form evaluated
// independently. The engine's own fresnelReflectance is the thing under test, so
// the expectation is written out longhand from Born and Wolf rather than by
// calling it a second time.
ValidationCase validateFresnelCurve() {
    const double n1 = 1.0, n2 = 1.5168;              // air -> N-BK7
    double worst = 0.0;
    for (int i = 0; i <= 80; ++i) {
        const double th   = double(i) * 1.0 * kValPi / 180.0;   // 0..80 degrees
        const double cosI = std::cos(th), sinI = std::sin(th);
        const double sinT = n1 * sinI / n2;
        if (sinT >= 1.0) continue;
        const double cosT = std::sqrt(1.0 - sinT * sinT);
        const double rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
        const double rp = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
        const double want = 0.5 * (rs * rs + rp * rp);
        worst = std::max(worst, std::fabs(optics::fresnelReflectance(n1, n2, cosI) - want));
    }
    return makeCase(QStringLiteral("Fresnel R(theta) residual"),
                    QStringLiteral("worst |R_traced - R_analytic| over 0-80 deg at "
                                   "air -> n 1.5168"),
                    QStringLiteral("reflectance"), 0.0, worst, 1e-12);
}

// Brewster's angle: p-polarised light reflects nothing at all there. The
// unpolarised average has a minimum but never a zero, so this checks the
// polarised path specifically -- the amplitude coefficients and the Mueller
// matrix built from them, not the scalar curve.
ValidationCase validateBrewsterNull() {
    const double n1 = 1.0, n2 = 1.5168;
    const double brewster = std::atan(n2 / n1);
    double as = 0.0, ap = 0.0, phase = 0.0;
    polarisation::fresnelAmplitudes(n1, n2, std::cos(brewster), as, ap, phase);

    // A p-polarised state through the interface Mueller matrix: the reflected
    // intensity is what should vanish.
    const polarisation::Stokes in  = polarisation::Stokes::linearP();
    const polarisation::Stokes out =
        polarisation::Mueller::fromFresnel(as, ap, phase).apply(in);
    return makeCase(QStringLiteral("Brewster p-reflectance"),
                    QStringLiteral("R_p = 0 at theta_B = atan(n2/n1) = %1 deg")
                        .arg(brewster * 180.0 / kValPi, 0, 'f', 3),
                    QStringLiteral("reflectance"), 0.0, std::fabs(out.i), 1e-9);
}

// Total internal reflection is a retarder, and the retardance has a closed form.
// At the angle where it peaks in glass of index 1.5168 a Fresnel rhomb takes two
// such bounces to make a quarter wave, so the peak has to be an eighth of a wave
// -- 45 degrees of phase -- for the rhomb to work at all.
ValidationCase validateTirRetardance() {
    const double n = 1.5168;                          // glass -> air
    double peak = 0.0;
    const double crit = std::asin(1.0 / n);
    // Strictly inside the totally-reflecting region. At exactly the critical
    // angle sin^2(theta_t) lands on either side of one depending on the last
    // bit, and the sub-critical branch legitimately returns a pi phase from the
    // p-amplitude changing sign. That discontinuity is real and it is not what
    // this case is about.
    for (int i = 1; i <= 400; ++i) {
        const double th = crit + (0.5 * kValPi - crit) * double(i) / 400.0;
        double rs = 0.0, rp = 0.0, phase = 0.0;
        polarisation::fresnelAmplitudes(n, 1.0, std::cos(th), rs, rp, phase);
        peak = std::max(peak, std::fabs(phase));
    }
    // cos(delta/2) = ... maximised at cos^2 th = (n^2 - 1) / (n^2 + 1); the
    // resulting peak retardance is 2 atan((n^2 - 1) / (2n)).
    const double expected = 2.0 * std::atan((n * n - 1.0) / (2.0 * n));
    return makeCase(QStringLiteral("TIR peak retardance"),
                    QStringLiteral("delta_max = 2 atan((n^2-1)/2n) at glass -> air, "
                                   "n = %1").arg(n, 0, 'f', 4),
                    QStringLiteral("degrees"),
                    expected * 180.0 / kValPi, peak * 180.0 / kValPi, 0.5);
}

// The white-furnace test: a scattering model that conserves energy must return
// everything it was given. Sampling the GGX lobe many times and averaging the
// weights the sampler hands back has to come out at one, or the surface is
// creating or destroying light at every bounce.
ValidationCase validateGgxWhiteFurnace() {
    bsdf::Surface s;
    s.model    = bsdf::Model::Microfacet;
    s.alpha    = 0.25;
    s.fraction = 1.0;

    const Vec3 n(0, 0, 1);
    std::uint64_t rng = optics::mix64(0x5EEDu);
    double sum = 0.0;
    int    taken = 0;
    constexpr int kDraws = 200000;
    for (int i = 0; i < kDraws; ++i) {
        // A fixed 30-degree incidence, which is where masking and shadowing
        // both matter and neither dominates.
        const double th = 30.0 * kValPi / 180.0;
        const Vec3 wi(std::sin(th), 0.0, -std::cos(th));
        const Vec3 spec = optics::reflect(wi, n);
        Vec3 out;
        double w = 1.0;
        if (!s.sample(wi, n, spec, rng, out, w)) continue;
        sum += w;
        ++taken;
    }
    // What the tracer actually multiplies a branch by. A draw the sampler
    // refuses keeps the specular direction and its whole energy, so it counts
    // as a weight of one -- measuring only the draws that fired would report a
    // number the engine never applies.
    const double measured = (sum + double(kDraws - taken)) / double(kDraws);
    return makeCase(QStringLiteral("GGX white furnace"),
                    QStringLiteral("mean energy multiplier at a rough reflection "
                                   "(alpha %1, 30 deg). Single-scattering GGX loses "
                                   "the few per cent that multiple microfacet "
                                   "bounces would return, so the shortfall is the "
                                   "model and not the sampler -- what must not "
                                   "happen is energy being created")
                        .arg(s.alpha, 0, 'f', 2),
                    QStringLiteral("albedo"), 1.0, measured, 0.05);
}

// The Henyey-Greenstein phase function is a probability density over the sphere,
// so it integrates to one there. It is quoted per steradian, so the integral is
// 2 pi times the integral over cos(theta) from -1 to 1.
ValidationCase validateHgNormalisation() {
    double worst = 0.0;
    for (double g : {-0.6, -0.2, 0.0, 0.3, 0.7, 0.9}) {
        // Simpson over cos(theta). The forward peak at g = 0.9 is sharp, so the
        // grid is fine enough that the quadrature is not the thing being tested.
        constexpr int kN = 20000;                     // even, for Simpson
        const double h = 2.0 / double(kN);
        double sum = bsdf::henyeyGreenstein(-1.0, g) + bsdf::henyeyGreenstein(1.0, g);
        for (int i = 1; i < kN; ++i) {
            const double c = -1.0 + h * double(i);
            sum += bsdf::henyeyGreenstein(c, g) * ((i % 2) ? 4.0 : 2.0);
        }
        const double integral = 2.0 * kValPi * sum * h / 3.0;
        worst = std::max(worst, std::fabs(integral - 1.0));
    }
    return makeCase(QStringLiteral("Henyey-Greenstein normalisation"),
                    QStringLiteral("worst |integral over the sphere - 1| over "
                                   "g = -0.6 .. 0.9"),
                    QStringLiteral("probability"), 0.0, worst, 2e-4);
}

// Luminous efficacy of a blackbody, against the number the definition gives.
// 683 lm/W at the peak of V(lambda) is the SI definition, so a 5500 K Planckian
// sampled over the visible must land where an independent integration of the
// same two curves does. The point is that the run's efficacy -- the number that
// turns every watt in the result into a lumen -- is not a fitted constant.
ValidationCase validateBlackbodyEfficacy() {
    SpectrumConfig cfg;
    cfg.kind  = SpectrumConfig::Kind::Blackbody;
    cfg.cct   = 5500.0;
    cfg.minNm = 380.0;
    cfg.maxNm = 780.0;
    SampledSpectrum spec;
    spec.build(cfg);

    // Planck at 5500 K, weighted by V(lambda), integrated over the same band by
    // a quadrature that knows nothing about the sampler under test.
    const double T = 5500.0;
    const double c1 = 3.741771852e-16, c2 = 1.438776877e-2;   // SI, W m^2 and m K
    auto planck = [&](double nm) {
        const double l = nm * 1e-9;
        return c1 / (std::pow(l, 5.0) * (std::exp(c2 / (l * T)) - 1.0));
    };
    double num = 0.0, den = 0.0;
    constexpr int kN = 4001;
    for (int i = 0; i < kN; ++i) {
        const double nm = 380.0 + (780.0 - 380.0) * double(i) / double(kN - 1);
        const double w  = (i == 0 || i == kN - 1) ? 0.5 : 1.0;   // trapezium
        const double p  = planck(nm);
        num += w * p * spectrum::photopic(nm);
        den += w * p;
    }
    const double expected = den > 0.0 ? 683.0 * num / den : 0.0;
    return makeCase(QStringLiteral("Blackbody luminous efficacy"),
                    QStringLiteral("683 * <V(lambda)> for a 5500 K Planckian over "
                                   "380-780 nm"),
                    QStringLiteral("lm/W"), expected, spec.efficacy(),
                    0.02 * expected);
}

// A Lambertian emitter of exitance M shows a luminance of M / pi, from any
// direction and any distance.
//
// The fourteenth closed form, and the first one that is a *camera* measurement
// rather than a receiver one. It is worth having beside the other thirteen for
// the reason they are there at all: it is the number a renderer gets wrong by
// exactly pi if it treats a source power as a radiance, and a picture that is
// out by pi looks entirely convincing.
//
// The two inverse squares -- one in the flux falling off with distance, one in
// the solid angle the emitter subtends -- cancel exactly, which is why the
// camera is placed at two distances and two angles and all four have to give the
// same answer.
ValidationCase validateLambertianLuminance() {
    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.shape        = SourceConfig::Shape::Rect;
    src.sizeA        = 120.0;                 // mm
    src.sizeB        = 120.0;
    src.halfAngleDeg = 90.0;
    src.power        = 2500.0;                // lumens
    src.fluxUnit     = FluxUnit::Lumen;
    src.origin       = gp_Pnt(0, 0, 0);
    src.axis         = gp_Dir(0, 0, 1);

    const double areaM2   = (src.sizeA * 1e-3) * (src.sizeB * 1e-3);
    const double expected = backward::lambertianLuminance(src.power / areaM2);

    TraceScene empty;                          // nothing to occlude or reflect
    double worst = expected;
    for (double dist : {400.0, 1500.0}) {
        for (double tiltDeg : {0.0, 55.0}) {
            const double t = tiltDeg * 3.14159265358979323846 / 180.0;
            backward::CameraConfig cam;
            cam.eye    = Vec3(dist * std::sin(t), 0.0, dist * std::cos(t));
            cam.target = Vec3(0, 0, 0);
            cam.up     = Vec3(0, 1, 0);
            cam.width  = cam.height = 24;
            cam.samplesPerPixel = 4;
            // A narrow enough field that every pixel lands on the emitter.
            cam.sensorWidthMm = 36.0;
            cam.focalLengthMm = 36.0 * dist / 40.0;

            backward::LuminanceImage img;
            backward::render(empty, {}, {src}, cam, PhysicsOptions{},
                             FluxUnit::Lumen, img);
            double sum = 0.0;
            int    n   = 0;
            for (int y = 6; y < img.height - 6; ++y)
                for (int x = 6; x < img.width - 6; ++x) { sum += img.at(x, y); ++n; }
            const double seen = n > 0 ? sum / double(n) : 0.0;
            if (std::fabs(seen - expected) > std::fabs(worst - expected)) worst = seen;
        }
    }
    return makeCase(QStringLiteral("Lambertian luminance"),
                    QStringLiteral("L = M / pi for an emitter of exitance M, worst of "
                                   "two distances x two view angles"),
                    QStringLiteral("cd/m^2"), expected, worst, 1e-6 * expected);
}

// A metalens converges at the numerical aperture its phase profile encodes.
//
// The fifteenth closed form, and the only one in the table whose surface does
// not obey Snell's law. Phi(r) = -(2 pi / lambda_d)(sqrt(r^2 + f^2) - f) is the
// phase that turns a plane wave into one converging on a point f away, so a ray
// crossing at radius r has to leave along a direction whose sine to the axis is
// exactly r / sqrt(r^2 + f^2) -- which is the design NA at the rim.
//
// Measured on the scene the library actually ships, at three quarters of the
// aperture, so what is checked is the device somebody can open rather than a
// wafer built for the occasion.
ValidationCase validateMetalensNa() {
    const auto scene = GeometryProvider::buildScene(GeometryProvider::Scene::Metalens);
    const SceneParams P = GeometryProvider::defaultParams(GeometryProvider::Scene::Metalens);
    const double lensR = P.v[0], focal = P.v[1];
    const double r = 0.75 * lensR;

    TraceScene traced;
    traced.build(MeshBuilder::build(scene));

    SimulationResult res;
    PhysicsOptions phys;
    // Parallel to the axis from well in front, at the design wavelength.
    RayTracer::traceSingleRay(traced, Vec3(r, 0.0, -50.0), Vec3(0, 0, 1), res, 1.0,
                              phys, 550.0);

    // The branch that carried the most energy to the receiver is the one that
    // went through: the Fresnel reflections are a few per cent and go backwards.
    double best = -1.0;
    Vec3   arrived;
    for (const DetectorArrival& a : res.arrivals) {
        if (double(a.energy) <= best) continue;
        best = double(a.energy);
        arrived = a.d;
    }
    // Sine of the angle to the axis: the numerical aperture it converged at.
    const double measured =
        best > 0.0 ? std::sqrt(std::max(0.0, 1.0 - arrived.z * arrived.z)) : 0.0;
    const double expected = r / std::sqrt(r * r + focal * focal);

    return makeCase(QStringLiteral("Metalens numerical aperture"),
                    QStringLiteral("sin(theta) = r / sqrt(r^2 + f^2) at three quarters "
                                   "of the aperture, from the designed phase gradient "
                                   "alone -- the plate is flat"),
                    QStringLiteral("NA"), expected, measured, 1e-6);
}

std::vector<ValidationCase> validate(int rays) {
    std::vector<ValidationCase> cases;
    cases.push_back(validateLensmaker(rays));
    cases.push_back(validateSphereMultiplier(rays));
    validatePointSource(cases, rays);
    cases.push_back(validateCpcConcentration());
    cases.push_back(validatePrismDeviation());
    cases.push_back(validateLambertianFarField(rays));

    // The layers underneath, each against the closed form it was built from. A
    // wrong number here is the hardest kind to see, because nothing about the
    // picture the tracer draws looks wrong.
    cases.push_back(validateFresnelCurve());
    cases.push_back(validateBrewsterNull());
    cases.push_back(validateTirRetardance());
    cases.push_back(validateGgxWhiteFurnace());
    cases.push_back(validateHgNormalisation());
    cases.push_back(validateBlackbodyEfficacy());
    // The camera side, checked the same way everything else here is.
    cases.push_back(validateLambertianLuminance());
    // And the one surface in the library that does not obey Snell law.
    cases.push_back(validateMetalensNa());
    return cases;
}

// ---- regression corpus -----------------------------------------------------

namespace {

// The corpus file's own format version. A change to what is recorded has to
// invalidate every stored file rather than be read as drift.
constexpr int kCorpusVersion = 1;

// Every quantity a reference point holds, named once so writing, reading and
// comparing cannot disagree about the order.
struct Quantity {
    const char* name;
    double ReferencePoint::*member;
};

const Quantity kQuantities[] = {
    {"efficiency",    &ReferencePoint::efficiency},
    {"fluxDetector",  &ReferencePoint::fluxDetector},
    {"fluxAbsorbed",  &ReferencePoint::fluxAbsorbed},
    {"fluxEscaped",   &ReferencePoint::fluxEscaped},
    {"fluxTruncated", &ReferencePoint::fluxTruncated},
    {"fluxRejected",  &ReferencePoint::fluxRejected},
    {"residual",      &ReferencePoint::residual},
};
constexpr int kQuantityCount = int(sizeof(kQuantities) / sizeof(kQuantities[0]));

} // namespace

std::vector<ReferencePoint> referenceCorpus(int rays, std::uint64_t seed,
                                            const TraceControl& ctl) {
    std::vector<ReferencePoint> out;
    out.reserve(std::size_t(GeometryProvider::count()));

    for (int i = 0; i < GeometryProvider::count(); ++i) {
        if (ctl.cancel && ctl.cancel->load(std::memory_order_relaxed)) break;

        const auto scene = GeometryProvider::Scene(i);
        SimConfig cfg;
        cfg.scene  = scene;
        cfg.rays   = std::max(1, rays);
        cfg.seed   = seed;
        // The far field, the path recording and the arrivals are not part of
        // what is asserted, and building them makes the corpus slower without
        // making it stricter.
        cfg.nTheta = 0;
        cfg.nPhi   = 0;

        const SimulationResult res = Simulation::run(cfg);

        ReferencePoint p;
        p.scene         = GeometryProvider::info(scene).name;
        p.rays          = int(res.raysEmitted);
        p.seed          = seed;
        p.efficiency    = res.efficiency;
        p.fluxDetector  = res.fluxDetector;
        p.fluxAbsorbed  = res.fluxAbsorbed;
        p.fluxEscaped   = res.fluxEscaped;
        p.fluxTruncated = res.fluxTruncated;
        p.fluxRejected  = res.fluxRejected;
        p.residual      = res.fluxRoulette;
        out.push_back(p);

        if (ctl.progress) ctl.progress(std::size_t(i + 1),
                                       std::size_t(GeometryProvider::count()));
    }
    return out;
}

bool writeCorpus(const QString& path, const std::vector<ReferencePoint>& pts,
                 QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts << "# LuxTrace reference corpus, version " << kCorpusVersion << "\n";
    ts << "# Bit-reproducible results at a fixed seed and ray budget. Regenerate\n";
    ts << "# with: LuxTrace --regress bless <path>\n";
    ts << "# scene\trays\tseed";
    for (const Quantity& q : kQuantities) ts << "\t" << q.name;
    ts << "\n";

    // Seventeen significant digits round-trips a double exactly, which is what
    // makes the stored value the value and not an approximation of it.
    for (const ReferencePoint& p : pts) {
        ts << p.scene << "\t" << p.rays << "\t" << p.seed;
        for (const Quantity& q : kQuantities)
            ts << "\t" << QString::number(p.*(q.member), 'g', 17);
        ts << "\n";
    }
    ts.flush();
    if (f.error() != QFileDevice::NoError) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return true;
}

bool readCorpus(const QString& path, std::vector<ReferencePoint>& out, QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    out.clear();
    QTextStream ts(&f);
    int version = 0;
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.startsWith(QLatin1Char('#'))) {
            if (version == 0 && line.contains(QLatin1String("version")))
                version = line.section(QLatin1String("version"), 1).trimmed().toInt();
            continue;
        }
        if (line.trimmed().isEmpty()) continue;

        const QStringList tok = line.split(QLatin1Char('\t'));
        if (tok.size() < 3 + kQuantityCount) continue;

        ReferencePoint p;
        p.scene = tok[0];
        p.rays  = tok[1].toInt();
        p.seed  = tok[2].toULongLong();
        for (int q = 0; q < kQuantityCount; ++q)
            p.*(kQuantities[q].member) = tok[3 + q].toDouble();
        out.push_back(p);
    }

    if (version != kCorpusVersion) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 is a version %2 corpus and this build writes "
                                       "version %3; regenerate it")
                            .arg(path).arg(version).arg(kCorpusVersion);
        out.clear();
        return false;
    }
    if (out.empty()) {
        if (errorOut) *errorOut = QStringLiteral("%1 holds no reference points").arg(path);
        return false;
    }
    return true;
}

std::vector<RegressionDrift> compareCorpus(const std::vector<ReferencePoint>& reference,
                                           const std::vector<ReferencePoint>& measured,
                                           double tolerance) {
    std::vector<RegressionDrift> drift;

    auto find = [](const std::vector<ReferencePoint>& v, const QString& name)
        -> const ReferencePoint* {
        for (const ReferencePoint& p : v)
            if (p.scene == name) return &p;
        return nullptr;
    };

    for (const ReferencePoint& r : reference) {
        const ReferencePoint* m = find(measured, r.scene);
        if (!m) {
            drift.push_back({r.scene, QStringLiteral("missing from this run"),
                             0.0, 0.0, 0.0});
            continue;
        }
        if (m->rays != r.rays || m->seed != r.seed) {
            // Comparing a different budget against the stored one would report
            // Monte Carlo noise as drift, which is worse than not comparing.
            drift.push_back({r.scene, QStringLiteral("traced at a different budget or seed"),
                             double(r.rays), double(m->rays), 0.0});
            continue;
        }
        for (const Quantity& q : kQuantities) {
            const double a = r.*(q.member), b = m->*(q.member);
            // Relative where there is something to be relative to, absolute at
            // zero -- an escaped-flux bucket that was exactly zero and is now
            // 1e-16 is not a hundred per cent drift.
            const double scale = std::max(std::fabs(a), 1e-12);
            const double rel   = (b - a) / scale;
            if (std::fabs(rel) > tolerance)
                drift.push_back({r.scene, QLatin1String(q.name), a, b, rel});
        }
    }

    for (const ReferencePoint& m : measured)
        if (!find(reference, m.scene))
            drift.push_back({m.scene, QStringLiteral("not in the reference corpus"),
                             0.0, 0.0, 0.0});

    return drift;
}

QString regressionTable(const std::vector<RegressionDrift>& drift,
                        std::size_t scenesChecked) {
    QString out;
    QTextStream ts(&out);
    if (drift.empty()) {
        ts << scenesChecked << " scene(s) match the reference corpus to "
           << QString::number(kRegressionTolerance, 'g', 2) << " relative.\n";
        return out;
    }
    ts << "scene                           quantity          reference      measured"
          "     relative\n";
    ts << "----------------------------------------------------------------------------"
          "---------\n";
    for (const RegressionDrift& d : drift) {
        ts << d.scene.leftJustified(30).left(30) << "  "
           << d.quantity.leftJustified(16).left(16) << "  "
           << QString::number(d.reference, 'g', 10).rightJustified(13) << "  "
           << QString::number(d.measured,  'g', 10).rightJustified(13) << "  "
           << QString::number(100.0 * d.relative, 'g', 4).rightJustified(10) << "%\n";
    }
    ts << "----------------------------------------------------------------------------"
          "---------\n";
    ts << scenesChecked << " scene(s) checked, " << drift.size() << " drift(s).\n";
    ts << "If the change was intended, bless the new values:\n";
    ts << "    LuxTrace --regress bless <path>\n";
    return out;
}

QString validationTable(const std::vector<ValidationCase>& cases) {
    QString out;
    QTextStream ts(&out);
    ts << "case                            expected      measured      residual    rel      result\n";
    ts << "---------------------------------------------------------------------------------------\n";
    int failed = 0;
    for (const auto& c : cases) {
        if (!c.passed) ++failed;
        ts << c.name.leftJustified(30).left(30) << "  "
           << QString::number(c.expected, 'g', 6).rightJustified(11) << "  "
           << QString::number(c.measured, 'g', 6).rightJustified(11) << "  "
           << QString::number(c.residual(), 'g', 3).rightJustified(10) << "  "
           << QString::number(100.0 * c.relative(), 'f', 2).rightJustified(6) << "%  "
           << (c.passed ? QStringLiteral("PASS") : QStringLiteral("FAIL")) << "\n";
        ts << "    " << c.reference << " [" << c.unit << ", tolerance "
           << QString::number(c.tolerance, 'g', 3) << "]\n";
    }
    ts << "---------------------------------------------------------------------------------------\n";
    ts << cases.size() << " case(s), " << failed << " failure(s)\n";
    return out;
}

} // namespace studies
