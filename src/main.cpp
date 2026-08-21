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
#include "core/GeometryProvider.h"
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

    auto pct = [](double v) { return QString::number(100.0 * v, 'f', 3) + "%"; };
    out << Qt::endl << "energy budget (fraction of emitted power)" << Qt::endl;
    out << "  receiver         " << pct(res.fluxDetector) << Qt::endl;
    out << "  surface absorbed " << pct(res.fluxAbsorbed - res.fluxBulkAbsorbed) << Qt::endl;
    out << "  bulk absorbed    " << pct(res.fluxBulkAbsorbed) << Qt::endl;
    out << "  escaped          " << pct(res.fluxEscaped) << Qt::endl;
    out << "  truncated        " << pct(res.fluxTruncated) << Qt::endl;
    out << "  accounted        " << pct(res.fluxAccounted()) << Qt::endl;
    out << "  efficiency       " << pct(res.efficiency) << " +- "
        << pct(res.efficiencyStdErr) << Qt::endl;

    if (m.valid) {
        out << Qt::endl << "spot on the receiver" << Qt::endl;
        out << "  peak irradiance  " << QString::number(m.peak, 'g', 4) << " W/mm^2" << Qt::endl;
        out << "  mean irradiance  " << QString::number(m.mean, 'g', 4) << " W/mm^2" << Qt::endl;
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
            << " W/sr" << Qt::endl;
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
