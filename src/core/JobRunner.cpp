#include "core/JobRunner.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "gp_Dir.hxx"

#include "core/Analysis.h"
#include "core/CadImport.h"
#include "core/ConfigIO.h"
#include "core/GeometryProvider.h"
#include "core/Material.h"
#include "core/RayFile.h"
#include "core/Simulation.h"
#include "core/Studies.h"

namespace jobrunner {

namespace {

// ---------------------------------------------------------------------------
// small JSON helpers
// ---------------------------------------------------------------------------

double num(const QJsonObject& o, const char* key, double fallback) {
    return o.value(QLatin1String(key)).toDouble(fallback);
}

QString str(const QJsonObject& o, const char* key, const QString& fallback = {}) {
    const QString v = o.value(QLatin1String(key)).toString();
    return v.isEmpty() ? fallback : v;
}

double frac(double flux, double sourcePower) {
    return sourcePower > 0.0 ? flux / sourcePower : 0.0;
}

// The study metrics are an enum with a name; operations take the metric by
// string, matched against the registry's own names first and a snake_case
// alias set second, so a script can say "efficiency" or "rms_radius" and the
// same word round-trips back to it in results.
struct MetricAlias { studies::Metric m; const char* id; };
constexpr MetricAlias kMetricIds[int(studies::Metric::Count)] = {
    { studies::Metric::Efficiency,        "efficiency"       },
    { studies::Metric::RmsRadius,         "rms_radius_mm"    },
    { studies::Metric::D86Radius,         "d86_radius_mm"    },
    { studies::Metric::PeakIrradiance,    "peak_irradiance"  },
    { studies::Metric::UniformityMinPeak, "uniformity_min"   },
    { studies::Metric::UniformityMeanPeak,"uniformity_mean"  },
    { studies::Metric::BeamFwhmDeg,       "beam_fwhm_deg"    },
    { studies::Metric::PeakIntensity,     "peak_intensity"   },
    { studies::Metric::SpotFwhmX,         "spot_fwhm_x_mm"   },
    { studies::Metric::CentroidOffset,    "centroid_offset_mm" },
};

bool parseMetric(const QString& name, studies::Metric& out) {
    for (const MetricAlias& a : kMetricIds) {
        if (name.compare(a.id, Qt::CaseInsensitive) == 0) { out = a.m; return true; }
    }
    for (int i = 0; i < int(studies::Metric::Count); ++i) {
        if (studies::metricName(studies::Metric(i)).compare(name, Qt::CaseInsensitive) == 0) {
            out = studies::Metric(i);
            return true;
        }
    }
    return false;
}

QString metricId(studies::Metric m) {
    for (const MetricAlias& a : kMetricIds)
        if (a.m == m) return QLatin1String(a.id);
    return studies::metricName(m);
}

QJsonArray loadParamInfo(GeometryProvider::Scene scene) {
    QJsonArray arr;
    for (const SceneParamInfo& p : GeometryProvider::paramInfo(scene)) {
        QJsonObject o;
        o["name"]     = p.name;
        o["unit"]     = p.unit;
        o["min"]      = p.min;
        o["max"]      = p.max;
        o["default"]  = p.def;
        o["step"]     = p.step;
        o["decimals"] = p.decimals;
        o["tip"]      = p.tip;
        arr.append(o);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------

// Every tracing operation carries a "config" object in exactly the shape the
// desktop app saves -- configio is the writer, so a file the UI produced feeds
// in unchanged. Missing keys keep their defaults, and numbers arrive clamped,
// so a hand-written {"scene":{"name":"EllipticalReflector"},"run":{"rays":...}}
// is a valid configuration rather than a parse error.
bool loadConfig(const QJsonObject& params, SimConfig& cfg, QStringList& warnings,
                QString* errCode, QString* errMsg) {
    const QJsonValue cv = params.value(QStringLiteral("config"));
    if (cv.isUndefined()) {
        *errCode = "missing_config"; *errMsg = "operation needs a 'config' object";
        return false;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(cv.toObject()).toJson(QJsonDocument::Compact));
    QString errOut;
    if (!configio::fromJson(json, cfg, &errOut, &warnings)) {
        *errCode = "bad_config"; *errMsg = errOut;
        return false;
    }
    // Direct overrides, for the "same scene, one number changed" case where a
    // full round trip through the config document is ceremony.
    if (params.contains("rays"))      cfg.rays    = int(num(params, "rays", double(cfg.rays)));
    if (params.contains("seed"))      cfg.seed    = std::uint64_t(std::max(0.0, num(params, "seed", double(cfg.seed))));
    if (params.contains("threads"))   cfg.threads = unsigned(std::max(0.0, num(params, "threads", double(cfg.threads))));
    return true;
}

// ---------------------------------------------------------------------------
// result serialisation
// ---------------------------------------------------------------------------

QJsonObject runMetrics(const SimulationResult& res) {
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);

    QJsonObject o;
    o["unit"]              = fluxUnitName(res.unit);
    o["source_power"]      = res.sourcePower;
    o["efficiency"]        = res.efficiency;
    o["efficiency_std_err"] = res.efficiencyStdErr;
    o["rays_emitted"]      = qlonglong(res.raysEmitted);
    o["rays_hit_detector"] = qlonglong(res.raysHitDetector);
    o["build_seconds"]     = res.buildSeconds;
    o["trace_seconds"]     = res.traceSeconds;
    o["luminous_efficacy_lm_per_w"] = res.luminousEfficacy;
    o["mean_wavelength_nm"] = res.meanWavelengthNm;

    // The loss budget, each channel as a fraction of the source. These are the
    // numbers a design review reads first.
    QJsonObject budget;
    budget["detector"]     = frac(res.fluxDetector,  res.sourcePower);
    budget["absorbed"]     = frac(res.fluxAbsorbed - res.fluxBulkAbsorbed, res.sourcePower);
    budget["bulk_absorbed"] = frac(res.fluxBulkAbsorbed, res.sourcePower);
    budget["escaped"]      = frac(res.fluxEscaped,   res.sourcePower);
    budget["truncated"]    = frac(res.fluxTruncated, res.sourcePower);
    budget["rejected"]     = frac(res.fluxRejected,  res.sourcePower);
    budget["accounted"]    = frac(res.fluxAccounted(), res.sourcePower);
    o["energy_budget"]     = budget;

    QJsonObject spot;
    spot["peak"]        = m.peak;
    spot["mean"]        = m.mean;
    spot["min_lit"]     = m.minLit;
    spot["uniformity"]  = m.uniformity;
    spot["mean_to_peak"] = m.meanToPeak;
    spot["centroid_x_mm"] = m.centroidX;
    spot["centroid_y_mm"] = m.centroidY;
    spot["rms_radius_mm"] = m.rmsRadius;
    spot["d50_radius_mm"] = m.d50Radius;
    spot["d86_radius_mm"] = m.d86Radius;
    spot["fwhm_x_mm"]   = m.fwhmX;
    spot["fwhm_y_mm"]   = m.fwhmY;
    spot["lit_bins"]    = m.litBins;
    spot["total_bins"]  = m.totalBins;
    o["spot"] = spot;

    QJsonObject far;
    far["beam_fwhm_deg"] = studies::metricValue(res, studies::Metric::BeamFwhmDeg);
    far["peak_intensity"] = studies::metricValue(res, studies::Metric::PeakIntensity);
    o["far_field"] = far;

    QJsonObject colour;
    colour["x"]     = m.cieX;
    colour["y"]     = m.cieY;
    colour["cct_k"] = m.cct;
    o["chromaticity"] = colour;

    if (res.polarised) {
        QJsonObject pol;
        pol["degree"]       = res.degreeOfPolarisation;
        pol["degree_linear"] = res.degreeLinear;
        pol["angle_deg"]    = res.polarisationAngleDeg;
        o["polarisation"] = pol;
    }
    return o;
}

QJsonArray exportCsv(const SimulationResult& res, const analysis::SpotMetrics& m,
                     const QString& path, QString* errOut) {
    const QString csv = analysis::metricsCsv(res, m);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        *errOut = QStringLiteral("cannot open %1 for writing").arg(path);
        return {};
    }
    f.write(csv.toUtf8());
    QJsonArray out;
    out.append(path);
    return out;
}

// ---------------------------------------------------------------------------
// operations
// ---------------------------------------------------------------------------

bool opFeatures(const QJsonObject&, QJsonObject& data, QString*, QString*) {
    data["app"]          = "LuxTrace";
    data["version"]      = "1.0.0";
    data["protocol"]     = kProtocolVersion;
    data["operations"]   = [](){ QJsonArray a; for (const QString& s : opNames()) a.append(s); return a; }();
    data["scene_count"]  = GeometryProvider::count();
    return true;
}

bool opScenes(const QJsonObject&, QJsonObject& data, QString*, QString*) {
    QJsonArray arr;
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto scene = static_cast<GeometryProvider::Scene>(i);
        const GeometryProvider::SceneInfo& info = GeometryProvider::info(scene);
        QJsonObject o;
        o["index"]       = i;
        o["name"]        = info.name;
        o["description"] = info.description;
        o["params"]      = loadParamInfo(scene);
        arr.append(o);
    }
    data["scenes"] = arr;
    return true;
}

bool opMaterials(const QJsonObject&, QJsonObject& data, QString*, QString*) {
    QJsonArray arr;
    const int builtin = materials::builtinCount();
    for (int i = 0; i < materials::count(); ++i) {
        const OpticalMaterial mat = materials::at(i);
        QJsonObject o;
        o["index"]       = i;
        o["name"]        = materials::name(i);
        o["description"] = materials::description(i);
        o["source"]      = materials::source(i);
        o["builtin"]     = (i < builtin);
        o["nd"]          = mat.nd;
        o["abbe"]        = mat.abbe();
        o["index_at_550nm"] = mat.nd > 0.0 ? mat.indexAt(550.0) : 0.0;
        arr.append(o);
    }
    data["materials"] = arr;
    return true;
}

bool opRun(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    SimConfig cfg; QStringList warnings;
    if (!loadConfig(params, cfg, warnings, errCode, errMsg)) return false;

    const SimulationResult res = Simulation::run(cfg);
    data["scene"]         = cfg.sceneName();
    data["rays"]          = cfg.rays;
    data["seed"]          = static_cast<double>(cfg.seed);
    data["metrics"]       = runMetrics(res);
    if (!warnings.isEmpty()) {
        QJsonArray w;
        for (const QString& s : warnings) w.append(s);
        data["warnings"] = w;
    }
    const QString csvPath = str(params, "export_csv");
    if (!csvPath.isEmpty()) {
        QString err;
        const QJsonArray written = exportCsv(res, analysis::computeSpotMetrics(res), csvPath, &err);
        if (!err.isEmpty()) { *errCode = "csv_write_failed"; *errMsg = err; return false; }
        data["exported_csv"] = written;
    }
    return true;
}

bool opFocus(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    SimConfig cfg; QStringList warnings;
    if (!loadConfig(params, cfg, warnings, errCode, errMsg)) return false;
    const double span  = num(params, "span_mm", 20.0);
    const int    steps = int(num(params, "steps", 81.0));
    const studies::FocusStudy study =
        studies::throughFocus(Simulation::run(cfg), span, steps);
    data["valid"]      = study.valid;
    data["best_focus_mm"] = study.bestZ;
    data["best_rms_mm"]   = study.bestRms;
    data["receiver_z_mm"] = study.receiverZ;
    QJsonArray arr;
    for (const analysis::FocusSample& s : study.samples) {
        QJsonObject o;
        o["z_mm"]          = s.z;
        o["rms_radius_mm"] = s.rmsRadius;
        o["d86_radius_mm"] = s.d86Radius;
        o["peak_density"]  = s.peakDensity;
        arr.append(o);
    }
    data["samples"] = arr;
    return true;
}

bool opConvergence(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    SimConfig cfg; QStringList warnings;
    if (!loadConfig(params, cfg, warnings, errCode, errMsg)) return false;
    const int minR = int(num(params, "min_rays", 1000.0));
    const int maxR = int(num(params, "max_rays", double(cfg.rays)));
    const int pts  = std::clamp(int(num(params, "points", 6.0)), 2, 32);
    const std::vector<studies::ConvergencePoint> sweep =
        studies::convergence(cfg, minR, maxR, pts);
    QJsonArray arr;
    for (const studies::ConvergencePoint& p : sweep) {
        QJsonObject o;
        o["rays"]         = p.rays;
        o["efficiency"]   = p.efficiency;
        o["std_err"]      = p.stdErr;
        o["trace_seconds"] = p.traceSeconds;
        arr.append(o);
    }
    data["points"]     = arr;
    data["converged"]  = studies::hasConverged(sweep);
    return true;
}

bool opSweep(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    SimConfig cfg; QStringList warnings;
    if (!loadConfig(params, cfg, warnings, errCode, errMsg)) return false;
    studies::Metric metric = studies::Metric::Efficiency;
    const QString mname = str(params, "metric", "efficiency");
    if (!parseMetric(mname, metric)) {
        *errCode = "invalid_metric"; *errMsg = "unknown metric " + mname; return false;
    }
    const int slot = int(num(params, "slot", 0.0));
    const auto& pi = GeometryProvider::paramInfo(cfg.scene);
    if (slot < 0 || slot >= int(pi.size())) {
        *errCode = "invalid_slot";
        *errMsg  = QStringLiteral("slot %1 out of range, scene %2 has %3 parameters")
                       .arg(slot).arg(cfg.sceneName()).arg(pi.size());
        return false;
    }
    const int steps = std::clamp(int(num(params, "steps", 8.0)), 2, 512);
    const std::vector<studies::SweepPoint> sweep = studies::parameterSweep(
        cfg, slot, num(params, "from", pi[std::size_t(slot)].min),
                   num(params, "to",   pi[std::size_t(slot)].max),
                   steps, metric, int(num(params, "repeats", 2.0)));
    data["parameter"] = pi[std::size_t(slot)].name;
    data["unit"]      = pi[std::size_t(slot)].unit;
    data["metric"]    = metricId(metric);
    QJsonArray arr;
    for (const studies::SweepPoint& p : sweep) {
        QJsonObject o;
        o["parameter"] = p.parameter;
        o["value"]     = p.value;
        o["std_err"]   = p.stdErr;
        o["efficiency"] = p.efficiency;
        o["seconds"]   = p.seconds;
        arr.append(o);
    }
    data["points"] = arr;
    return true;
}

bool opSweep2d(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    SimConfig cfg; QStringList warnings;
    if (!loadConfig(params, cfg, warnings, errCode, errMsg)) return false;
    studies::Metric metric = studies::Metric::Efficiency;
    const QString mname = str(params, "metric", "efficiency");
    if (!parseMetric(mname, metric)) {
        *errCode = "invalid_metric"; *errMsg = "unknown metric " + mname; return false;
    }
    const int slotA = int(num(params, "slot_a", 0.0));
    const int slotB = int(num(params, "slot_b", 1.0));
    const auto& pi = GeometryProvider::paramInfo(cfg.scene);
    if (slotA < 0 || slotA >= int(pi.size()) || slotB < 0 || slotB >= int(pi.size())) {
        *errCode = "invalid_slot";
        *errMsg  = QStringLiteral("slotIdxs %1/%2 out of range, scene %3 has %4 parameters")
                       .arg(slotA).arg(slotB).arg(cfg.sceneName()).arg(pi.size());
        return false;
    }
    const studies::Sweep2D sweep = studies::parameterSweep2D(
        cfg, slotA, slotB, int(num(params, "steps_a", 6.0)), int(num(params, "steps_b", 6.0)),
        metric);
    data["parameter_a"] = pi[std::size_t(slotA)].name;
    data["parameter_b"] = pi[std::size_t(slotB)].name;
    data["metric"]      = metricId(metric);
    QJsonArray a, b, vals;
    for (double v : sweep.a) a.append(v);
    for (double v : sweep.b) b.append(v);
    for (double v : sweep.values) vals.append(v);
    data["a"] = a; data["b"] = b; data["values"] = vals;
    data["lo"] = sweep.lo; data["hi"] = sweep.hi;
    return true;
}

bool opOptimise(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    SimConfig cfg; QStringList warnings;
    if (!loadConfig(params, cfg, warnings, errCode, errMsg)) return false;
    studies::Metric metric = studies::Metric::Efficiency;
    const QString mname = str(params, "metric", "efficiency");
    if (!parseMetric(mname, metric)) {
        *errCode = "invalid_metric"; *errMsg = "unknown metric " + mname; return false;
    }
    studies::Objective obj;
    obj.metric = metric;
    const QString goal = str(params, "goal", "maximise");
    if (goal.compare("minimise", Qt::CaseInsensitive) == 0)      obj.goal = studies::Objective::Goal::Minimise;
    else if (goal.compare("target", Qt::CaseInsensitive) == 0) { obj.goal = studies::Objective::Goal::Target; obj.target = num(params, "target", 0.0); }
    else if (goal.compare("maximise", Qt::CaseInsensitive) != 0) {
        *errCode = "invalid_goal"; *errMsg = "goal is maximise, minimise or target"; return false;
    }
    QJsonArray slotArr = params.value("slots").toArray();
    std::vector<int> slotIdxs;
    for (const auto& v : slotArr) slotIdxs.push_back(int(v.toDouble()));
    const auto& pi = GeometryProvider::paramInfo(cfg.scene);
    for (int s : slotIdxs) {
        if (s < 0 || s >= int(pi.size())) {
            *errCode = "invalid_slot";
            *errMsg  = QStringLiteral("slot %1 out of range, scene %2 has %3 parameters")
                           .arg(s).arg(cfg.sceneName()).arg(pi.size());
            return false;
        }
    }
    if (slotIdxs.empty()) {
        *errCode = "empty_slots"; *errMsg = "optimise needs a 'slots' array of parameter indices";
        return false;
    }
    const QString m = str(params, "optimiser", "neldermead");
    const studies::Optimiser method = m.compare("cmaes", Qt::CaseInsensitive) == 0
                                          ? studies::Optimiser::Cmaes : studies::Optimiser::NelderMead;
    const studies::OptimisationResult r = studies::optimise(
        cfg, slotIdxs, obj, method, int(num(params, "evaluations", 120.0)));

    data["valid"]       = r.valid;
    data["method"]      = r.method;
    data["evaluations"] = r.evaluations;
    data["seconds"]     = r.seconds;
    data["best_value"]  = r.bestValue;
    data["start_value"] = r.startValue;
    QJsonArray best;
    for (int i = 0; i < int(slotIdxs.size()); ++i) {
        QJsonObject o;
        o["slot"]  = slotIdxs[std::size_t(i)];
        o["name"]  = pi[std::size_t(slotIdxs[std::size_t(i)])].name;
        o["value"] = r.best.v[i];
        best.append(o);
    }
    data["best_params"] = best;
    QJsonArray hist;
    for (const studies::OptimisationStep& s : r.history) {
        QJsonObject o;
        o["evaluation"] = s.evaluation;
        o["value"]      = s.value;
        o["std_err"]    = s.stdErr;
        o["merit"]      = s.merit;
        QJsonArray p;
        for (int i = 0; i < int(slotIdxs.size()); ++i) p.append(s.params.v[i]);
        o["params"] = p;
        hist.append(o);
    }
    data["history"] = hist;
    return true;
}

bool opTolerance(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    SimConfig cfg; QStringList warnings;
    if (!loadConfig(params, cfg, warnings, errCode, errMsg)) return false;
    studies::Metric metric = studies::Metric::Efficiency;
    const QString mname = str(params, "metric", "efficiency");
    if (!parseMetric(mname, metric)) {
        *errCode = "invalid_metric"; *errMsg = "unknown metric " + mname; return false;
    }
    std::vector<studies::Tolerance> tols;
    const QJsonArray tArr = params.value("tolerances").toArray();
    for (const auto& v : tArr) {
        const QJsonObject t = v.toObject();
        studies::Tolerance tol;
        tol.slotIndex = int(num(t, "slot", -1.0));
        tol.amount    = num(t, "amount", 0.0);
        const QString shape = str(t, "shape", "gaussian");
        if      (shape.compare("uniform", Qt::CaseInsensitive) == 0) tol.shape = studies::Tolerance::Shape::Uniform;
        else if (shape.compare("bimodal", Qt::CaseInsensitive) == 0) tol.shape = studies::Tolerance::Shape::Bimodal;
        else                                                         tol.shape = studies::Tolerance::Shape::Gaussian;
        if (!tol.active()) continue;
        tols.push_back(tol);
    }
    if (tols.empty()) {
        *errCode = "empty_tolerances";
        *errMsg  = "tolerance needs a 'tolerances' array of {slot, amount, shape}";
        return false;
    }
    const studies::ToleranceStudy s = studies::tolerance(
        cfg, tols, metric, num(params, "criterion", 0.5),
        params.contains("pass_above") ? params.value("pass_above").toBool(true)
                                      : studies::metricBiggerIsBetter(metric),
        int(num(params, "samples", 200.0)));
    data["valid"]     = s.valid;
    data["samples"]   = s.samples;
    data["seconds"]   = s.seconds;
    data["nominal"]   = s.nominal;
    data["mean"]      = s.mean;
    data["std_dev"]   = s.stdDev;
    data["best"]      = s.best;
    data["worst"]     = s.worst;
    data["median"]    = s.median;
    data["p90"]       = s.p90;
    data["p99"]       = s.p99;
    data["criterion"] = s.criterion;
    data["pass_above"] = s.passIsAbove;
    data["yield"]     = s.yield;
    QJsonArray sens;
    for (const studies::ToleranceStudy::Sensitivity& sn : s.sensitivity) {
        QJsonObject o;
        o["slot"]     = sn.slotIndex;
        o["name"]     = sn.name;
        o["amount"]   = sn.amount;
        o["gradient"] = sn.gradient;
        o["share"]    = sn.share;
        sens.append(o);
    }
    data["sensitivity"] = sens;
    QJsonArray hist;
    for (std::size_t i = 0; i < s.binCentre.size(); ++i) {
        QJsonObject o;
        o["bin"]   = s.binCentre[i];
        o["count"] = s.binCount[i];
        hist.append(o);
    }
    data["histogram"] = hist;
    return true;
}

bool opValidate(const QJsonObject& params, QJsonObject& data, QString*, QString*) {
    const int rays = int(num(params, "rays", 200000.0));
    const std::vector<studies::ValidationCase> cases = studies::validate(rays);
    QJsonArray arr;
    int passed = 0;
    for (const studies::ValidationCase& c : cases) {
        QJsonObject o;
        o["name"]      = c.name;
        o["reference"] = c.reference;
        o["unit"]      = c.unit;
        o["expected"]  = c.expected;
        o["measured"]  = c.measured;
        o["tolerance"] = c.tolerance;
        o["residual"]  = c.residual();
        o["relative"]  = c.relative();
        o["passed"]    = c.passed;
        arr.append(o);
        if (c.passed) ++passed;
    }
    data["cases"] = arr;
    data["passed_count"] = passed;
    data["all_passed"]   = (passed == int(cases.size()));
    return true;
}

bool opCad(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    const QString path = str(params, "path");
    if (path.isEmpty()) { *errCode = "missing_path"; *errMsg = "cad needs a 'path' to a STEP/IGES file"; return false; }
    const double scale = num(params, "scale", 0.0);   // 0 == read it from the file's header

    cadimport::ImportOptions io;
    io.scale = scale;
    io.audit = true;
    const cadimport::ImportResult r = cadimport::read(path, io);
    if (!r.ok) { *errCode = "import_failed"; *errMsg = r.error; return false; }

    QJsonArray shapes;
    for (const cadimport::ImportedShape& sh : r.shapes) {
        QJsonObject o;
        o["label"]    = sh.label;
        o["status"]   = sh.audit.statusName();
        o["faces"]    = sh.faceCount;
        o["size_mm"]  = sh.size();
        o["closed"]   = sh.audit.closed;
        o["free_edges"] = sh.audit.freeEdges;
        shapes.append(o);
    }
    QJsonObject import;
    import["format"]           = r.format;
    import["parts"]            = int(r.shapes.size());
    import["total_faces"]      = r.totalFaces;
    import["applied_scale_mm"] = r.appliedScale;
    import["unit_from_header"] = r.unitFromHeader;
    import["unit_name"]        = r.unitName;
    QJsonArray bmin, bmax;
    for (int i = 0; i < 3; ++i) { bmin.append(r.bboxMin[i]); bmax.append(r.bboxMax[i]); }
    import["bbox_min"] = bmin;
    import["bbox_max"] = bmax;
    import["shapes"]   = shapes;
    data["import"] = import;

    const QJsonArray axisArr = params.value("axis").toArray();
    const gp_Dir axis(axisArr.size() == 3 ? axisArr.at(0).toDouble(0.0) : 0.0,
                      axisArr.size() == 3 ? axisArr.at(1).toDouble(0.0) : 0.0,
                      axisArr.size() == 3 ? axisArr.at(2).toDouble(1.0) : 1.0);
    auto setup = std::make_shared<GeometryProvider::SceneSetup>(cadimport::makeScene(
        r, str(params, "material", "N-BK7"), params.value("reflective").toBool(false),
        int(num(params, "quality", 64.0)), axis));
    if (setup->surfaces.empty()) {
        *errCode = "not_traceable";
        *errMsg  = "nothing in the file could be turned into traceable geometry";
        return false;
    }
    setup->label = path;

    SimConfig cfg;
    cfg.imported     = setup;
    cfg.source       = SourceConfig::Type::Collimated;
    cfg.rays         = int(num(params, "rays", 20000.0));
    cfg.seed         = std::uint64_t(std::max(0.0, num(params, "seed", 12345.0)));
    cfg.threads      = unsigned(std::max(0.0, num(params, "threads", 0.0)));
    // An import must not be traced against a built-in scene's config, which
    // always carries one -- the same rule the --cad flag states.
    cfg.scene        = GeometryProvider::Scene::Reflector;

    const Simulation::SceneRef ref = Simulation::dataFor(cfg);
    QJsonObject geom;
    geom["surfaces"]  = int(ref->surfaces.size());
    geom["triangles"] = qlonglong(ref->scene.triangles().size());
    data["geometry"] = geom;

    const SimulationResult res = Simulation::run(cfg);
    data["scene"]   = cfg.sceneName();
    data["rays"]    = cfg.rays;
    data["metrics"] = runMetrics(res);
    return true;
}

bool opRayfile(const QJsonObject& params, QJsonObject& data, QString* errCode, QString* errMsg) {
    const QString path = str(params, "path");
    if (path.isEmpty()) { *errCode = "missing_path"; *errMsg = "rayfile needs a 'path'"; return false; }
    const std::size_t maxRays = params.contains("max_rays")
        ? std::size_t(std::max(0.0, num(params, "max_rays", 0.0))) : std::size_t(0);
    const rayfile::LoadResult r = rayfile::load(path, maxRays);
    if (!r.ok || !r.data) { *errCode = "load_failed"; *errMsg = r.error; return false; }
    const RayFileData& d = *r.data;
    data["label"]                = d.label;
    data["format"]               = d.format;
    data["declared_rays"]        = qlonglong(d.declared);
    data["loaded_rays"]          = qlonglong(d.size());
    data["ray_set_flux"]         = d.raySetFlux;
    data["source_flux"]          = d.sourceFlux;
    data["flux_unit"]            = fluxUnitName(d.unit);
    data["has_wavelengths"]      = d.hasWavelengths;
    data["header_wavelength_nm"] = d.headerWavelengthNm;
    data["unit_scale_mm"]        = d.unitScale;
    QJsonArray bmin, bmax;
    for (int i = 0; i < 3; ++i) { bmin.append(d.bmin[i]); bmax.append(d.bmax[i]); }
    data["bbox_min"] = bmin;
    data["bbox_max"] = bmax;
    data["summary"]  = d.summary();
    return true;
}

bool dispatch(const QString& op, const QJsonObject& params, QJsonObject& data,
              QString* errCode, QString* errMsg) {
    if (op == "features")   return opFeatures(params, data, errCode, errMsg);
    if (op == "scenes")     return opScenes(params, data, errCode, errMsg);
    if (op == "materials")  return opMaterials(params, data, errCode, errMsg);
    if (op == "run")        return opRun(params, data, errCode, errMsg);
    if (op == "focus")      return opFocus(params, data, errCode, errMsg);
    if (op == "convergence") return opConvergence(params, data, errCode, errMsg);
    if (op == "sweep")      return opSweep(params, data, errCode, errMsg);
    if (op == "sweep2d")    return opSweep2d(params, data, errCode, errMsg);
    if (op == "optimise")   return opOptimise(params, data, errCode, errMsg);
    if (op == "tolerance")  return opTolerance(params, data, errCode, errMsg);
    if (op == "validate")   return opValidate(params, data, errCode, errMsg);
    if (op == "cad")        return opCad(params, data, errCode, errMsg);
    if (op == "rayfile")    return opRayfile(params, data, errCode, errMsg);
    *errCode = "unknown_operation";
    *errMsg  = QStringLiteral("unknown operation '%1', known: %2").arg(op, opNames().join(", "));
    return false;
}

void writeEnvelope(QTextStream& out, const QJsonObject& env) {
    out << QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)) << '\n';
    out.flush();
}

QJsonObject writeErrorEnvelope(const QString& op, const QString& code, const QString& message) {
    QJsonObject env;
    env["v"]    = kProtocolVersion;
    env["type"] = "error";
    if (!op.isEmpty()) env["op"] = op;
    QJsonObject e;
    e["code"]    = code;
    e["message"] = message;
    env["error"] = e;
    return env;
}

bool processLine(const QJsonObject& op, QTextStream& out) {
    static int seq = 0;
    bool fatal = false;
    QString fatalMessage;
    const QJsonObject env = runOne(op, seq, &fatal, &fatalMessage);
    writeEnvelope(out, env);
    ++seq;
    return !fatal;
}

} // namespace

QJsonObject runOne(const QJsonObject& op, int seq, bool* fatalOut, QString* fatalMessageOut) {
    *fatalOut = false;
    fatalMessageOut->clear();

    QElapsedTimer timer;
    timer.start();

    const QString opName = op.value(QStringLiteral("op")).toString();
    const QJsonObject params = op.value(QStringLiteral("params")).toObject();

    QJsonObject data;
    QString errCode = "internal", errMsg;
    const bool ok = dispatch(opName, params, data, &errCode, &errMsg);

    QJsonObject env;
    env["v"]   = kProtocolVersion;
    env["seq"] = seq;
    env["op"]  = opName;
    if (op.contains(QStringLiteral("id"))) env["id"] = op.value(QStringLiteral("id"));
    env["ms"] = round(100.0 * double(timer.nsecsElapsed()) / 1e6) / 100.0;
    if (ok) {
        env["type"] = "result";
        env["data"] = data;
    } else {
        env["type"]  = "error";
        QJsonObject e;
        e["code"]    = errCode;
        e["message"] = errMsg;
        env["error"] = e;
    }
    return env;
}

int runJobFile(const QString& path) {
    QTextStream out(stdout);

    QString text;
    if (path.isEmpty() || path == QStringLiteral("-")) {
        QTextStream in(stdin);
        text = in.readAll();
    } else {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            writeEnvelope(out, writeErrorEnvelope({}, "bad_job",
                QStringLiteral("cannot open job file %1").arg(path)));
            return 1;
        }
        text = QString::fromUtf8(f.readAll());
    }

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        writeEnvelope(out, writeErrorEnvelope({}, "bad_job",
            QStringLiteral("job document is not JSON: %1").arg(pe.errorString())));
        return 1;
    }
    QJsonArray ops;
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        ops = o.contains(QStringLiteral("ops")) ? o.value(QStringLiteral("ops")).toArray()
                                                : QJsonArray{o};
    } else if (doc.isArray()) {
        ops = doc.array();
    }
    if (ops.isEmpty()) {
        writeEnvelope(out, writeErrorEnvelope({}, "bad_job",
            "job document holds no operations: expected {\"ops\":[...]} or one operation object"));
        return 1;
    }

    for (const QJsonValue& v : ops) {
        if (!v.isObject()) {
            writeEnvelope(out, writeErrorEnvelope({}, "bad_operation",
                "each entry of 'ops' must be an operation object with an 'op' key"));
            continue;
        }
        bool fatal = false;
        QString fatalMessage;
        const QJsonObject env = runOne(v.toObject(), 0, &fatal, &fatalMessage);
        writeEnvelope(out, env);
        if (fatal) return 1;
        std::ignore = fatalMessage;
    }
    return 0;
}

int runServe() {
    // A human-readable readiness marker, on stderr: stdout is protocol.
    QTextStream err(stderr);
    err << "luxtrace-serve ready (protocol " << kProtocolVersion << ", "
        << opNames().size() << " operations)\n";
    err.flush();

    QTextStream in(stdin);
    QTextStream out(stdout);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) continue;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            writeEnvelope(out, writeErrorEnvelope({}, "bad_line",
                QStringLiteral("line is not a JSON object: %1").arg(pe.errorString())));
            continue;
        }
        processLine(doc.object(), out);
    }
    return 0;
}

QStringList opNames() {
    return {
        "features", "scenes", "materials", "run", "focus", "convergence",
        "sweep", "sweep2d", "optimise", "tolerance", "validate", "cad", "rayfile",
    };
}

} // namespace jobrunner