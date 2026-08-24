#include <QApplication>
#include <QIcon>
#include <QTextStream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

#include <Standard_ErrorHandler.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include "core/Analysis.h"
#include "core/CadImport.h"
#include "core/GeometryProvider.h"
#include "core/Material.h"
#include "core/MeshBuilder.h"
#include "core/Simulation.h"
#include "core/Studies.h"
#include "core/TraceScene.h"
#include "ui/MainWindow.h"

namespace {

QString sceneName(int s) {
    return GeometryProvider::info(GeometryProvider::Scene(s)).name;
}

int meshCheck() {
    QTextStream out(stdout);
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        try {
            auto surfs  = GeometryProvider::buildScene(GeometryProvider::Scene(s));
            auto meshes = MeshBuilder::build(surfs);
            TraceScene scene;
            scene.build(meshes);

            out << sceneName(s) << " meshes=" << meshes.size()
                << " tris=" << scene.triangles().size()
                << " instanced=" << scene.instancedTriangles()
                << " placements=" << scene.instances().size()
                << " bvhNodes=" << scene.nodeCount()
                << " bvhDepth=" << scene.bvhDepth();
            for (const auto& m : meshes) {
                double x0 = 1e18, y0 = 1e18, z0 = 1e18, x1 = -1e18, y1 = -1e18, z1 = -1e18;
                for (const auto& p : m.verts) {
                    x0 = std::min(x0, p.X()); y0 = std::min(y0, p.Y()); z0 = std::min(z0, p.Z());
                    x1 = std::max(x1, p.X()); y1 = std::max(y1, p.Y()); z1 = std::max(z1, p.Z());
                }
                out << " | " << m.label << " tris=" << m.tris.size()
                    << " bbox=[" << x0 << "," << x1 << "]x[" << y0 << "," << y1
                    << "]x[" << z0 << "," << z1 << "]";
            }
            out << Qt::endl;

            // Fire a few rays and confirm the BVH agrees with brute force.
            const gp_Pnt o(0, 0, -6.0);
            const double dirs[][3] = {{0, 0, 1}, {0.1, 0, 1}, {-0.2, 0, 1}, {0, 0.3, 1}, {0.5, 0, 1}};
            for (const auto& dd : dirs) {
                Vec3 d(dd[0], dd[1], dd[2]);
                d.normalize();
                RayHit fast, slow;
                const bool a = scene.nearestHit(Vec3(o), d, fast);
                const bool b = scene.nearestHitBruteForce(Vec3(o), d, slow);
                out << "   ray d=(" << dd[0] << "," << dd[1] << "," << dd[2] << ") t="
                    << (a ? fast.t : 0.0)
                    << (a ? (" surf=" + QString::number(scene.triangles()[std::size_t(fast.tri)].surf))
                          : QStringLiteral(" NONE"))
                    << ((a == b && (!a || std::fabs(fast.t - slow.t) < 1e-9))
                            ? QStringLiteral("  [bvh==brute]")
                            : QStringLiteral("  [BVH MISMATCH]"))
                    << Qt::endl;
            }
        } catch (const Standard_Failure& e) {
            out << sceneName(s) << " SF: " << e.GetMessageString() << Qt::endl;
        }
    }
    return 0;
}

int smokeTest(int rays, unsigned threads) {
    QTextStream out(stdout);
    out << "threads=" << (threads ? threads : std::thread::hardware_concurrency())
        << " rays=" << rays << Qt::endl;

    double totalTrace = 0.0;
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        try {
            OCC_CATCH_SIGNALS
            SimConfig cfg;
            cfg.scene   = GeometryProvider::Scene(s);
            cfg.source  = SourceConfig::Type::Lambertian;
            cfg.rays    = rays;
            cfg.threads = threads;

            SimulationResult res = Simulation::run(cfg);
            totalTrace += res.traceSeconds;

            const double accounted = res.fluxAccounted();
            const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
            out << sceneName(s)
                << " | emitted=" << res.raysEmitted
                << " | detector=" << res.raysHitDetector
                << " | flux=" << QString::number(res.fluxDetector, 'g', 5)
                << " | eff=" << QString::number(100.0 * res.efficiency, 'f', 2)
                << "+-" << QString::number(100.0 * res.efficiencyStdErr, 'f', 3) << "%"
                << " | rms=" << QString::number(m.rmsRadius, 'f', 1) << "mm"
                << " | unif=" << QString::number(m.meanToPeak, 'f', 3)
                << " | fwhm=" << QString::number(res.intensity.fwhmDeg, 'f', 1) << "deg"
                << " | bulk=" << QString::number(100.0 * res.fluxBulkAbsorbed, 'f', 2) << "%"
                << " | energy=" << QString::number(accounted, 'f', 6)
                << " | build=" << QString::number(res.buildSeconds, 'f', 3) << "s"
                << " | trace=" << QString::number(res.traceSeconds, 'f', 3) << "s"
                << " | " << qRound(res.traceSeconds > 0
                                       ? double(res.raysEmitted) / res.traceSeconds : 0.0)
                << " rays/s"
                << " | segs=" << res.raySegments.size() << Qt::endl;
            out.flush();
        } catch (const Standard_Failure& e) {
            out << "scene " << s << " Standard_Failure: " << e.GetMessageString() << Qt::endl;
        } catch (const std::exception& e) {
            out << "scene " << s << " std::exception: " << e.what() << Qt::endl;
        } catch (...) {
            out << "scene " << s << " unknown exception" << Qt::endl;
        }
        out.flush();
    }
    out << "total trace time: " << QString::number(totalTrace, 'f', 3) << " s" << Qt::endl;
    return 0;
}

// Traces every scene x source combination, single-threaded and then with the
// full thread pool, so a change in either can be compared at a glance.
int bench(int rays) {
    QTextStream out(stdout);
    const char* srcNames[] = {"Point     ", "Lambertian"};
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());

    out << "scene / source                    rays      1-thread    " << hw
        << "-thread   speedup   efficiency" << Qt::endl;

    for (int s = 0; s < GeometryProvider::count(); ++s) {
        Simulation::sceneFor(GeometryProvider::Scene(s));   // exclude build cost
        for (int t = 0; t <= 1; ++t) {
            SimConfig cfg;
            cfg.scene  = GeometryProvider::Scene(s);
            cfg.source = SourceConfig::Type(t);
            cfg.rays   = rays;

            cfg.threads = 1;
            const SimulationResult one = Simulation::run(cfg);
            cfg.threads = 0;
            const SimulationResult all = Simulation::run(cfg);

            out << sceneName(s).leftJustified(32) << " " << srcNames[t] << " "
                << QString::number(rays).rightJustified(8) << "  "
                << QString::number(one.traceSeconds, 'f', 3).rightJustified(8) << "s  "
                << QString::number(all.traceSeconds, 'f', 3).rightJustified(8) << "s  "
                << QString::number(all.traceSeconds > 0 ? one.traceSeconds / all.traceSeconds : 0.0,
                                   'f', 1).rightJustified(6) << "x  "
                << QString::number(100.0 * all.efficiency, 'f', 2).rightJustified(7) << "%"
                << (std::fabs(one.efficiency - all.efficiency) < 1e-12
                        ? QStringLiteral("  [deterministic]")
                        : QStringLiteral("  [THREAD-DEPENDENT!]"))
                << Qt::endl;
            out.flush();
        }
    }
    return 0;
}

// Exercises the analysis and the parameter studies headlessly, so every number
// the UI plots can be checked -- and regressed -- without opening a window.
// Reads a CAD file and traces it, through exactly the path the Import CAD
// menu item and the Run button now take. This exists because "the rays are
// going through the wrong shape" is a claim about which geometry reached the
// tracer, and that is answerable from a console without a window: what it
// prints is the imported part's own bounds, its own receiver and its own
// energy budget, with no scene from the registry anywhere in it.
int cad(const QString& path, double scale, int rays, const gp_Dir& axis) {
    QTextStream out(stdout);

    const cadimport::ImportResult r = cadimport::read(path, scale);
    if (!r.ok) {
        out << "import failed: " << r.error << Qt::endl;
        return 1;
    }
    out << "file:  " << path << Qt::endl;
    out << "format " << r.format << ", " << r.shapes.size() << " part(s), "
        << r.totalFaces << " face(s), scale " << scale << Qt::endl;
    for (const auto& sh : r.shapes)
        out << "  " << sh.label.leftJustified(16) << " faces=" << sh.faceCount
            << " size=" << QString::number(sh.size(), 'f', 2) << " mm" << Qt::endl;
    out << "bounds ["
        << QString::number(r.bboxMin[0], 'f', 2) << ", "
        << QString::number(r.bboxMin[1], 'f', 2) << ", "
        << QString::number(r.bboxMin[2], 'f', 2) << "] .. ["
        << QString::number(r.bboxMax[0], 'f', 2) << ", "
        << QString::number(r.bboxMax[1], 'f', 2) << ", "
        << QString::number(r.bboxMax[2], 'f', 2) << "]" << Qt::endl;

    auto setup = std::make_shared<GeometryProvider::SceneSetup>(
        cadimport::makeScene(r, QStringLiteral("N-BK7"), /*reflective=*/false, 64, axis));
    if (setup->surfaces.empty()) {
        out << "nothing in the file could be turned into traceable geometry" << Qt::endl;
        return 1;
    }
    setup->label = path;

    SimConfig cfg;
    cfg.imported     = setup;
    cfg.source       = SourceConfig::Type::Collimated;
    cfg.rays         = rays;
    cfg.detectorBins = 64;
    // Left at its default on purpose: a config always carries one, and an
    // import must not be traced against it.
    cfg.scene        = GeometryProvider::Scene::Reflector;

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    out << Qt::endl << "traced geometry" << Qt::endl;
    out << "  surfaces   " << data->surfaces.size() << Qt::endl;
    out << "  triangles  " << data->scene.triangles().size() << Qt::endl;
    out << "  lit along  (" << QString::number(axis.X(), 'f', 0) << ", "
        << QString::number(axis.Y(), 'f', 0) << ", "
        << QString::number(axis.Z(), 'f', 0) << ")" << Qt::endl;
    out << "  source     (" << QString::number(data->sourceOrigin.X(), 'f', 2) << ", "
        << QString::number(data->sourceOrigin.Y(), 'f', 2) << ", "
        << QString::number(data->sourceOrigin.Z(), 'f', 2) << ") along ("
        << QString::number(data->sourceAxis.X(), 'f', 2) << ", "
        << QString::number(data->sourceAxis.Y(), 'f', 2) << ", "
        << QString::number(data->sourceAxis.Z(), 'f', 2) << ")" << Qt::endl;
    const DetectorInfo& det = data->scene.detector();
    out << "  receiver   (" << QString::number(det.center.x, 'f', 2) << ", "
        << QString::number(det.center.y, 'f', 2) << ", "
        << QString::number(det.center.z, 'f', 2) << "), "
        << QString::number(det.w, 'f', 1) << " x " << QString::number(det.h, 'f', 1)
        << " mm, " << det.nx << " x " << det.ny << " bins" << Qt::endl;

    const SimulationResult res = Simulation::run(cfg);
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
    out << Qt::endl << "trace" << Qt::endl;
    out << "  emitted    " << res.raysEmitted << Qt::endl;
    out << "  arrived    " << res.raysHitDetector << Qt::endl;
    out << "  efficiency " << QString::number(100.0 * res.efficiency, 'f', 2) << " +- "
        << QString::number(100.0 * res.efficiencyStdErr, 'f', 3) << " %" << Qt::endl;
    out << "  absorbed   " << QString::number(100.0 * res.fluxAbsorbed, 'f', 2) << " %"
        << Qt::endl;
    out << "  escaped    " << QString::number(100.0 * res.fluxEscaped, 'f', 2) << " %"
        << Qt::endl;
    out << "  truncated  " << QString::number(100.0 * res.fluxTruncated, 'f', 2) << " %"
        << Qt::endl;
    out << "  energy     " << QString::number(res.fluxAccounted(), 'f', 6) << " of "
        << QString::number(res.sourcePower, 'f', 6) << Qt::endl;
    out << "  rms spot   " << QString::number(m.rmsRadius, 'f', 2) << " mm" << Qt::endl;
    out << "  build      " << QString::number(res.buildSeconds, 'f', 3) << " s, trace "
        << QString::number(res.traceSeconds, 'f', 3) << " s" << Qt::endl;

    // The trace is only meaningful if it conserves energy.
    return std::fabs(res.fluxAccounted() - res.sourcePower) < 1e-6 ? 0 : 2;
}

int study(int sceneIndex, int rays) {
    QTextStream out(stdout);
    const auto scene = GeometryProvider::Scene(
        std::clamp(sceneIndex, 0, GeometryProvider::count() - 1));

    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = rays;

    out << "scene: " << sceneName(int(scene)) << Qt::endl;
    out << "  " << GeometryProvider::info(scene).description << Qt::endl << Qt::endl;

    out << "parameters" << Qt::endl;
    const SceneParams params = cfg.effectiveParams();
    const auto& infos = GeometryProvider::paramInfo(scene);
    for (std::size_t i = 0; i < infos.size(); ++i)
        out << "  " << infos[i].name.leftJustified(22) << " "
            << QString::number(params.v[i], 'f', infos[i].decimals) << " " << infos[i].unit
            << "   [" << infos[i].min << " .. " << infos[i].max << "]" << Qt::endl;

    const SimulationResult res = Simulation::run(cfg);
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);

    auto pct = [&](double v) {
        return QString::number(100.0 * v / (res.sourcePower > 0 ? res.sourcePower : 1.0),
                               'f', 3) + "%";
    };
    out << Qt::endl << "source" << Qt::endl;
    out << "  emitted flux     " << QString::number(res.sourcePower, 'g', 5) << " "
        << fluxUnitName(res.unit) << Qt::endl;
    out << "  mean wavelength  " << QString::number(res.meanWavelengthNm, 'f', 1)
        << " nm" << Qt::endl;
    out << "  luminous efficacy" << QString::number(res.luminousEfficacy, 'f', 1)
        << " lm/W" << Qt::endl;

    out << Qt::endl << "energy budget (fraction of emitted power)" << Qt::endl;
    out << "  receiver         " << pct(res.fluxDetector) << Qt::endl;
    out << "  surface absorbed " << pct(res.fluxAbsorbed - res.fluxBulkAbsorbed) << Qt::endl;
    out << "  bulk absorbed    " << pct(res.fluxBulkAbsorbed) << Qt::endl;
    out << "  escaped          " << pct(res.fluxEscaped) << Qt::endl;
    out << "  truncated        " << pct(res.fluxTruncated) << Qt::endl;
    out << "  roulette residual" << pct(res.fluxRoulette) << Qt::endl;
    out << "  accounted        " << pct(res.fluxAccounted()) << Qt::endl;
    out << "  efficiency       " << pct(res.efficiency) << " +- "
        << pct(res.efficiencyStdErr) << Qt::endl;

    if (m.valid) {
        out << Qt::endl << "spot on the receiver" << Qt::endl;
        out << "  peak irradiance  " << QString::number(m.peak * 1e6, 'g', 4) << " "
            << irradianceUnitName(res.unit) << Qt::endl;
        out << "  mean irradiance  " << QString::number(m.mean * 1e6, 'g', 4) << " "
            << irradianceUnitName(res.unit) << Qt::endl;
        out << "  uniformity       " << QString::number(m.uniformity, 'f', 4)
            << " min/peak, " << QString::number(m.meanToPeak, 'f', 4) << " mean/peak" << Qt::endl;
        out << "  centroid         (" << QString::number(m.centroidX, 'f', 2) << ", "
            << QString::number(m.centroidY, 'f', 2) << ") mm" << Qt::endl;
        out << "  RMS radius       " << QString::number(m.rmsRadius, 'f', 3) << " mm" << Qt::endl;
        out << "  D50 / D86 radius " << QString::number(m.d50Radius, 'f', 3) << " / "
            << QString::number(m.d86Radius, 'f', 3) << " mm" << Qt::endl;
        out << "  FWHM x / y       " << QString::number(m.fwhmX, 'f', 2) << " / "
            << QString::number(m.fwhmY, 'f', 2) << " mm" << Qt::endl;
    }

    if (res.intensity.valid()) {
        out << Qt::endl << "far field" << Qt::endl;
        out << "  peak intensity   " << QString::number(res.intensity.peak, 'g', 4)
            << " " << intensityUnitName(res.unit) << Qt::endl;
        out << "  beam FWHM        " << QString::number(res.intensity.fwhmDeg, 'f', 2)
            << " deg" << Qt::endl;
        int peakBin = 0;
        for (int i = 1; i < res.intensity.nTheta; ++i)
            if (res.intensity.profile[std::size_t(i)] >
                res.intensity.profile[std::size_t(peakBin)]) peakBin = i;
        out << "  peak at theta    " << QString::number(res.intensity.thetaCenterDeg(peakBin),
                                                        'f', 1) << " deg" << Qt::endl;
        // Grouped maxima rather than point samples: a beam narrower than the
        // print stride would otherwise fall between two reads and show as zero.
        out << "  I(theta)/peak    ";
        const int step = std::max(1, res.intensity.nTheta / 12);
        for (int i = 0; i < res.intensity.nTheta; i += step) {
            double hi = 0.0;
            for (int j = i; j < std::min(i + step, res.intensity.nTheta); ++j)
                hi = std::max(hi, res.intensity.profile[std::size_t(j)]);
            out << QString::number(res.intensity.thetaCenterDeg(i), 'f', 0) << "-"
                << QString::number(res.intensity.thetaCenterDeg(
                       std::min(i + step, res.intensity.nTheta) - 1), 'f', 0) << ":"
                << QString::number(hi / res.intensity.peak, 'f', 2) << "  ";
        }
        out << Qt::endl;
    }

    const studies::FocusStudy focus =
        studies::throughFocus(res, std::max(40.0, 0.6 * std::fabs(res.detZ)));
    if (focus.valid) {
        out << Qt::endl << "through focus" << Qt::endl;
        out << "  receiver at      " << QString::number(focus.receiverZ, 'f', 1) << " mm" << Qt::endl;
        out << "  best focus at    " << QString::number(focus.bestZ, 'f', 1) << " mm ("
            << QString::number(focus.bestZ - focus.receiverZ, 'f', 1) << " mm away)" << Qt::endl;
        out << "  smallest RMS     " << QString::number(focus.bestRms, 'f', 3) << " mm" << Qt::endl;
    }

    const std::vector<studies::ConvergencePoint> sweep =
        studies::convergence(cfg, 500, std::max(2000, rays), 8);
    out << Qt::endl << "convergence" << Qt::endl;
    for (const auto& p : sweep)
        out << "  " << QString::number(p.rays).rightJustified(9) << " rays -> "
            << QString::number(100.0 * p.efficiency, 'f', 3) << "% +- "
            << QString::number(100.0 * p.stdErr, 'f', 4) << "%  ("
            << QString::number(p.traceSeconds, 'f', 3) << " s)" << Qt::endl;
    out << "  " << (studies::hasConverged(sweep) ? "stable within 2 sigma"
                                                 : "still moving -- trace more rays") << Qt::endl;
    return 0;
}

// Sweeps one of a scene's own dimensions and prints the metric against it, with
// the error bars that say whether a bump in the curve is the design or the
// sampling.
int sweep(int sceneIndex, int slotIndex, int metricIndex, int rays, int steps) {
    QTextStream out(stdout);
    const auto scene = GeometryProvider::Scene(
        std::clamp(sceneIndex, 0, GeometryProvider::count() - 1));
    const auto& infos = GeometryProvider::paramInfo(scene);
    if (infos.empty()) { out << "scene has no parameters" << Qt::endl; return 1; }
    slotIndex = std::clamp(slotIndex, 0, int(infos.size()) - 1);
    const auto metric = studies::Metric(
        std::clamp(metricIndex, 0, int(studies::Metric::Count) - 1));

    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = rays;

    const SceneParamInfo& info = infos[std::size_t(slotIndex)];
    out << "scene: " << sceneName(int(scene)) << Qt::endl;
    out << "sweeping " << info.name << " over [" << info.min << ", " << info.max << "] "
        << info.unit << Qt::endl;
    out << "metric:  " << studies::metricName(metric) << " ["
        << studies::metricUnit(metric, cfg.fluxUnit) << "]" << Qt::endl << Qt::endl;

    const auto points = studies::parameterSweep(cfg, slotIndex, info.min, info.max,
                                                steps, metric);
    out << info.name.leftJustified(20) << studies::metricName(metric).leftJustified(22)
        << "+- 1 sigma" << Qt::endl;
    double best = 0.0, bestAt = 0.0;
    bool first = true;
    for (const auto& pt : points) {
        out << QString::number(pt.parameter, 'f', info.decimals).leftJustified(20)
            << QString::number(pt.value, 'g', 6).leftJustified(22)
            << QString::number(pt.stdErr, 'g', 3) << Qt::endl;
        const bool better = first ||
            (studies::metricBiggerIsBetter(metric) ? pt.value > best : pt.value < best);
        if (better) { best = pt.value; bestAt = pt.parameter; first = false; }
    }
    out << Qt::endl << "best " << studies::metricName(metric) << " = "
        << QString::number(best, 'g', 6) << " at " << info.name << " = "
        << QString::number(bestAt, 'f', info.decimals) << " " << info.unit << Qt::endl;
    return 0;
}

// Searches a scene's dimensions for the design that makes a metric best.
int optimise(int sceneIndex, int metricIndex, int rays, int evaluations, bool cmaes) {
    QTextStream out(stdout);
    const auto scene = GeometryProvider::Scene(
        std::clamp(sceneIndex, 0, GeometryProvider::count() - 1));
    const auto& infos = GeometryProvider::paramInfo(scene);
    if (infos.empty()) { out << "scene has no parameters" << Qt::endl; return 1; }

    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = rays;

    // Every dimension the scene exposes except the receiver position, which is
    // the last slot and is a measurement choice rather than a design one.
    std::vector<int> paramSlots;
    for (int i = 0; i + 1 < int(infos.size()); ++i) paramSlots.push_back(i);
    if (paramSlots.empty()) paramSlots.push_back(0);

    studies::Objective obj;
    obj.metric = studies::Metric(std::clamp(metricIndex, 0, int(studies::Metric::Count) - 1));
    obj.goal   = studies::metricBiggerIsBetter(obj.metric) ? studies::Objective::Goal::Maximise
                                                           : studies::Objective::Goal::Minimise;

    out << "scene:     " << sceneName(int(scene)) << Qt::endl;
    out << "objective: " << (obj.goal == studies::Objective::Goal::Maximise ? "maximise "
                                                                            : "minimise ")
        << studies::metricName(obj.metric) << Qt::endl;
    out << "free:      ";
    for (int i : paramSlots) out << infos[std::size_t(i)].name << "  ";
    out << Qt::endl << Qt::endl;

    const auto r = studies::optimise(cfg, paramSlots, obj,
                                     cmaes ? studies::Optimiser::Cmaes
                                           : studies::Optimiser::NelderMead,
                                     evaluations);
    if (!r.valid) { out << "no evaluation succeeded" << Qt::endl; return 1; }

    out << r.method << ": " << r.evaluations << " evaluations in "
        << QString::number(r.seconds, 'f', 2) << " s" << Qt::endl;
    out << "start  " << QString::number(r.startValue, 'g', 6) << " "
        << studies::metricUnit(obj.metric, cfg.fluxUnit) << Qt::endl;
    out << "best   " << QString::number(r.bestValue, 'g', 6) << " "
        << studies::metricUnit(obj.metric, cfg.fluxUnit) << Qt::endl;
    for (int i : paramSlots)
        out << "  " << infos[std::size_t(i)].name.leftJustified(22)
            << QString::number(r.best.v[i], 'f', infos[std::size_t(i)].decimals) << " "
            << infos[std::size_t(i)].unit << Qt::endl;
    return 0;
}

// Perturbs a design many times over and reports what fraction of production
// would pass, and which dimension to tighten.
int tolerance(int sceneIndex, int metricIndex, double criterion, int rays, int samples) {
    QTextStream out(stdout);
    const auto scene = GeometryProvider::Scene(
        std::clamp(sceneIndex, 0, GeometryProvider::count() - 1));
    const auto& infos = GeometryProvider::paramInfo(scene);
    if (infos.size() < 2) { out << "scene has nothing to tolerance" << Qt::endl; return 1; }

    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = rays;

    const auto metric = studies::Metric(
        std::clamp(metricIndex, 0, int(studies::Metric::Count) - 1));
    const bool passIsAbove = studies::metricBiggerIsBetter(metric);

    // A per cent of each dimension's declared range, which is the sort of number
    // a drawing carries, on everything except the receiver position.
    std::vector<studies::Tolerance> tols;
    for (int i = 0; i + 1 < int(infos.size()); ++i) {
        studies::Tolerance t;
        t.slotIndex = i;
        t.amount    = 0.02 * (infos[std::size_t(i)].max - infos[std::size_t(i)].min);
        t.shape     = studies::Tolerance::Shape::Gaussian;
        tols.push_back(t);
    }

    out << "scene:     " << sceneName(int(scene)) << Qt::endl;
    out << "metric:    " << studies::metricName(metric) << " ["
        << studies::metricUnit(metric, cfg.fluxUnit) << "]" << Qt::endl;
    out << "criterion: " << (passIsAbove ? "at or above " : "at or below ")
        << criterion << Qt::endl;
    out << "tolerances:" << Qt::endl;
    for (const auto& t : tols)
        out << "  " << infos[std::size_t(t.slotIndex)].name.leftJustified(22) << "+/- "
            << QString::number(t.amount, 'f', infos[std::size_t(t.slotIndex)].decimals) << " "
            << infos[std::size_t(t.slotIndex)].unit << " (3 sigma)" << Qt::endl;
    out << Qt::endl;

    const auto study = studies::tolerance(cfg, tols, metric, criterion, passIsAbove, samples);
    if (!study.valid) { out << "the study produced nothing" << Qt::endl; return 1; }

    out << study.samples << " samples in " << QString::number(study.seconds, 'f', 1)
        << " s" << Qt::endl;
    out << "nominal        " << QString::number(study.nominal, 'g', 6) << Qt::endl;
    out << "mean           " << QString::number(study.mean, 'g', 6) << " +- "
        << QString::number(study.stdDev, 'g', 4) << Qt::endl;
    out << "best / worst   " << QString::number(study.best, 'g', 6) << " / "
        << QString::number(study.worst, 'g', 6) << Qt::endl;
    out << "50/90/99 % of parts beat  " << QString::number(study.median, 'g', 5) << " / "
        << QString::number(study.p90, 'g', 5) << " / "
        << QString::number(study.p99, 'g', 5) << Qt::endl;
    out << "YIELD          " << QString::number(100.0 * study.yield, 'f', 1) << " %"
        << Qt::endl << Qt::endl;

    out << "what dominates it" << Qt::endl;
    for (const auto& sens : study.sensitivity)
        out << "  " << sens.name.leftJustified(22)
            << QString::number(100.0 * sens.share, 'f', 1).rightJustified(6) << " % of the spread"
            << "   (" << QString::number(sens.gradient, 'g', 3) << " per unit)" << Qt::endl;
    return study.yield >= 0.5 ? 0 : 2;
}

// The frequency-domain half of the spot metrics, and the wavefront behind it.
int imageQuality(int sceneIndex, int rays) {
    QTextStream out(stdout);
    const auto scene = GeometryProvider::Scene(
        std::clamp(sceneIndex, 0, GeometryProvider::count() - 1));
    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = rays;

    const SimulationResult res = Simulation::run(cfg);
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
    const analysis::Mtf mtf = analysis::modulationTransfer(res);
    const analysis::Wavefront w = analysis::wavefrontError(res);

    out << "scene: " << sceneName(int(scene)) << Qt::endl << Qt::endl;
    if (m.valid)
        out << "RMS spot radius  " << QString::number(m.rmsRadius, 'f', 3) << " mm" << Qt::endl;

    if (mtf.valid) {
        out << "MTF 50 % at       " << QString::number(mtf.cutoff50, 'g', 4)
            << " cycles/mm" << Qt::endl;
        out << "MTF" << Qt::endl;
        const int step = std::max(1, int(mtf.frequency.size()) / 10);
        for (std::size_t i = 0; i < mtf.frequency.size(); i += std::size_t(step))
            out << "  " << QString::number(mtf.frequency[i], 'g', 4).rightJustified(10)
                << " cyc/mm   T " << QString::number(mtf.tangential[i], 'f', 3)
                << "   S " << QString::number(mtf.sagittal[i], 'f', 3) << Qt::endl;
    }

    if (w.valid) {
        out << Qt::endl << "wavefront" << Qt::endl;
        out << "  RMS            " << QString::number(w.rmsWaves, 'f', 3) << " waves" << Qt::endl;
        out << "  measured over  " << QString::number(100.0 * w.coreFraction(), 'f', 1)
            << " % of the arrivals (the imaging core)" << Qt::endl;
        if (w.coreFraction() < 0.6 || w.rmsWaves > 100.0)
            out << "  note           this scene has no aperture stop, so much of what "
                   "reaches the receiver never went through the optic. A wavefront "
                   "error is only meaningful for a bundle that images." << Qt::endl;
        out << "  peak to valley " << QString::number(w.ptvWaves, 'f', 3) << " waves" << Qt::endl;
        if (w.strehlMeaningful)
            out << "  Strehl         " << QString::number(w.strehl, 'f', 4)
                << (w.diffractionLimited() ? "  (diffraction limited)" : "") << Qt::endl;
        else
            out << "  Strehl         not meaningful past a quarter wave of error"
                << Qt::endl;
    }
    return 0;
}

// Checks the tracer against optics derived outside it, and prints the residuals.
// A feature list is a claim; this table is evidence.
int validate(int rays) {
    QTextStream out(stdout);
    out << "LuxTrace validation against closed-form optics" << Qt::endl << Qt::endl;
    const auto cases = studies::validate(rays);
    out << studies::validationTable(cases);
    out.flush();
    for (const auto& c : cases)
        if (!c.passed) return 1;
    return 0;
}

// The material catalogue, so the numbers behind a name can be read without
// opening the app.
int listMaterials() {
    QTextStream out(stdout);
    out << "name                  n_d      Abbe    alpha 1/mm   n(460)   n(546)   n(620)"
        << Qt::endl;
    for (int i = 0; i < materials::count(); ++i) {
        const OpticalMaterial m = materials::at(i);
        out << materials::name(i).leftJustified(20).left(20) << "  "
            << QString::number(m.indexAt(587.6), 'f', 4).rightJustified(7) << "  "
            << (m.isMetal() ? QStringLiteral("-").rightJustified(6)
                            : QString::number(m.abbe(), 'f', 1).rightJustified(6))
            << "  "
            << QString::number(m.alpha, 'g', 3).rightJustified(10) << "  "
            << QString::number(m.indexAt(460.0), 'f', 4).rightJustified(7) << "  "
            << QString::number(m.indexAt(546.1), 'f', 4).rightJustified(7) << "  "
            << QString::number(m.indexAt(620.0), 'f', 4).rightJustified(7);
        if (m.isMetal()) {
            out << "   metal, R at 0/45/70 deg (550 nm): "
                << QString::number(100.0 * metalReflectance(1.0, m.indexAt(550.0),
                                                            m.extinctionAt(550.0), 1.0), 'f', 1)
                << " / "
                << QString::number(100.0 * metalReflectance(1.0, m.indexAt(550.0),
                                                            m.extinctionAt(550.0),
                                                            std::cos(45.0 * 3.14159265358979 / 180.0)),
                                   'f', 1)
                << " / "
                << QString::number(100.0 * metalReflectance(1.0, m.indexAt(550.0),
                                                            m.extinctionAt(550.0),
                                                            std::cos(70.0 * 3.14159265358979 / 180.0)),
                                   'f', 1)
                << " %";
        }
        out << Qt::endl;
        out << "    " << materials::description(i) << Qt::endl;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("LuxTrace"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QApplication::setOrganizationName(QStringLiteral("LuxTrace"));

    // Several sizes rather than one master: QIcon picks the nearest, and the
    // taskbar, the switcher and the title bar all ask for different ones.
    QIcon icon;
    for (int size : {32, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/luxtrace_%1.png").arg(size));
    QApplication::setWindowIcon(icon);

    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--smoke")) {
        int rays = argc >= 3 ? std::atoi(argv[2]) : 0;
        if (rays <= 0) rays = 20000;
        const unsigned threads = argc >= 4 ? unsigned(std::atoi(argv[3])) : 0u;
        return smokeTest(rays, threads);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--bench")) {
        int rays = argc >= 3 ? std::atoi(argv[2]) : 0;
        if (rays <= 0) rays = 100000;
        return bench(rays);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--meshcheck")) {
        return meshCheck();
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--validate")) {
        int rays = argc >= 3 ? std::atoi(argv[2]) : 0;
        if (rays <= 0) rays = 400000;
        return validate(rays);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--sweep")) {
        const int scene  = argc >= 3 ? std::atoi(argv[2]) : 0;
        const int slotIx = argc >= 4 ? std::atoi(argv[3]) : 0;
        const int metric = argc >= 5 ? std::atoi(argv[4]) : 0;
        int rays  = argc >= 6 ? std::atoi(argv[5]) : 0;
        int steps = argc >= 7 ? std::atoi(argv[6]) : 0;
        if (rays  <= 0) rays  = 20000;
        if (steps <= 0) steps = 13;
        return sweep(scene, slotIx, metric, rays, steps);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--optimise")) {
        const int scene  = argc >= 3 ? std::atoi(argv[2]) : 0;
        const int metric = argc >= 4 ? std::atoi(argv[3]) : 0;
        int rays  = argc >= 5 ? std::atoi(argv[4]) : 0;
        int evals = argc >= 6 ? std::atoi(argv[5]) : 0;
        const bool cmaes = argc >= 7 && QLatin1String(argv[6]) == QLatin1String("cmaes");
        if (rays  <= 0) rays  = 20000;
        if (evals <= 0) evals = 80;
        return optimise(scene, metric, rays, evals, cmaes);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--tolerance")) {
        const int scene  = argc >= 3 ? std::atoi(argv[2]) : 0;
        const int metric = argc >= 4 ? std::atoi(argv[3]) : 0;
        const double crit = argc >= 5 ? std::atof(argv[4]) : 50.0;
        int rays    = argc >= 6 ? std::atoi(argv[5]) : 0;
        int samples = argc >= 7 ? std::atoi(argv[6]) : 0;
        if (rays <= 0) rays = 8000;
        if (samples <= 0) samples = 100;
        return tolerance(scene, metric, crit, rays, samples);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--imagequality")) {
        const int scene = argc >= 3 ? std::atoi(argv[2]) : 0;
        int rays = argc >= 4 ? std::atoi(argv[3]) : 0;
        if (rays <= 0) rays = 200000;
        return imageQuality(scene, rays);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--cad")) {
        if (argc < 3) {
            QTextStream(stdout)
                << "usage: LuxTrace --cad <file> [scale] [rays] [+x|-x|+y|-y|+z|-z]"
                << Qt::endl;
            return 1;
        }
        const double scale = argc >= 4 ? std::atof(argv[3]) : 1.0;
        int rays = argc >= 5 ? std::atoi(argv[4]) : 0;
        if (rays <= 0) rays = 50000;
        gp_Dir axis(0, 0, 1);
        if (argc >= 6) {
            const QString a = QString::fromLatin1(argv[5]).toLower();
            if      (a == QLatin1String("+x")) axis = gp_Dir( 1,  0,  0);
            else if (a == QLatin1String("-x")) axis = gp_Dir(-1,  0,  0);
            else if (a == QLatin1String("+y")) axis = gp_Dir( 0,  1,  0);
            else if (a == QLatin1String("-y")) axis = gp_Dir( 0, -1,  0);
            else if (a == QLatin1String("-z")) axis = gp_Dir( 0,  0, -1);
        }
        return cad(QString::fromLocal8Bit(argv[2]), scale > 0.0 ? scale : 1.0, rays, axis);
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--materials")) {
        return listMaterials();
    }
    if (argc >= 2 && QLatin1String(argv[1]) == QLatin1String("--study")) {
        const int scene = argc >= 3 ? std::atoi(argv[2]) : 0;
        int rays = argc >= 4 ? std::atoi(argv[3]) : 0;
        if (rays <= 0) rays = 200000;
        return study(scene, rays);
    }

    MainWindow win;
    win.resize(1150, 780);
    win.show();
    return app.exec();
}
