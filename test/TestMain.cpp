// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

// Unit tests for the optics core. Deliberately dependency-free: a tiny
// assertion harness keeps the test target buildable with nothing but Qt + OCCT,
// which are already required by the app.
//
// Run everything:      optics_tests
// Run one suite:       optics_tests <suite>
// Name every suite:    optics_tests --list-suites
//
// CTest registers one test per suite, and it gets the list by asking this
// binary rather than from a list kept by hand: see test/CMakeLists.txt and
// test/DiscoverSuites.cmake. A suite is registered by existing.

#include <QCoreApplication>
#include <QDir>
#include <QStringList>
#include <QElapsedTimer>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <STEPControl_Writer.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Vec.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include "core/Analysis.h"
#include "core/BackendCheck.h"
#include "core/EnvironmentMap.h"
#include "core/BackwardTracer.h"
#include "core/Metasurface.h"
#include "core/MetaFile.h"
#include "gpu/GpuTrace.h"
#include "core/CadImport.h"
#include "core/Report.h"
#include "core/ConfigIO.h"
#include "core/GeometryProvider.h"
#include "core/MultiEditFields.h"
#include "core/GeometryWorker.h"
#include "core/MeshBuilder.h"
#include "core/Bsdf.h"
#include "core/Coating.h"
#include "core/Polarisation.h"
#include "core/Material.h"
#include "core/MaterialFile.h"
#include "core/PythonEnv.h"
#include "core/RayFile.h"
#include "core/Spectrum.h"
#include "core/Sampling.h"
#include "core/Optics.h"
#include "core/RayTracer.h"
#include "core/SceneDocument.h"
#include "render/AppearanceMaterials.h"
#include "render/AppearanceScene.h"
#include "render/AppearanceEmitters.h"
#include "render/AppearanceExport.h"
#include "render/RadianceMap.h"
#include "core/Simulation.h"
#include "core/SimulationWorker.h"
#include "core/Studies.h"
#include "core/TraceScene.h"
#include "core/Vec3.h"

// ---------------------------------------------------------------- harness --
namespace {

int g_failures = 0;
int g_checks   = 0;
std::string g_currentTest;

void reportFailure(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::printf("  FAIL %s:%d  [%s]\n       %s\n", file, line, g_currentTest.c_str(),
                what.c_str());
}

void checkTrue(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) reportFailure(file, line, std::string("expected true: ") + expr);
}

void checkNear(double got, double want, double tol, const char* expr,
               const char* file, int line) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: got %.12g, want %.12g (tol %.3g)", expr, got,
                      want, tol);
        reportFailure(file, line, buf);
    }
}

#define CHECK(cond)               checkTrue((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(got, want, tol) checkNear((got), (want), (tol), #got, __FILE__, __LINE__)

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Register {
    Register(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back({suite, name, std::move(fn)});
    }
};

#define TEST(suite, name)                                                      \
    static void suite##_##name();                                              \
    static Register reg_##suite##_##name(#suite, #name, suite##_##name);       \
    static void suite##_##name()

// ------------------------------------------------------------- test helpers --

// A single triangle in the z = 0 plane, wound counter-clockwise seen from +Z.
MeshSurface unitTriangleSurface(double reflectivity = 0.0, double transmissivity = 0.0,
                                double index = 0.0) {
    MeshSurface m;
    m.label = QStringLiteral("tri");
    m.verts = {gp_Pnt(0, 0, 0), gp_Pnt(1, 0, 0), gp_Pnt(0, 1, 0)};
    Triangle t;
    t.v0 = 0; t.v1 = 1; t.v2 = 2;
    t.normal = gp_Dir(0, 0, 1);
    m.tris.push_back(t);
    m.reflectivity   = reflectivity;
    m.transmissivity = transmissivity;
    m.index          = index;
    return m;
}

// An axis-aligned square receiver of the given size at z = D, facing +Z.
MeshSurface detectorSurface(double size, double D, int bins = 8) {
    const double h = 0.5 * size;
    MeshSurface m;
    m.label = QStringLiteral("detector");
    m.verts = {gp_Pnt(-h, -h, D), gp_Pnt(h, -h, D), gp_Pnt(h, h, D), gp_Pnt(-h, h, D)};
    Triangle a; a.v0 = 0; a.v1 = 1; a.v2 = 2; a.normal = gp_Dir(0, 0, 1);
    Triangle b; b.v0 = 0; b.v1 = 2; b.v2 = 3; b.normal = gp_Dir(0, 0, 1);
    m.tris = {a, b};
    m.isDetector = true;
    m.detCenter = gp_Pnt(0, 0, D);
    m.detW = size;
    m.detH = size;
    m.detNX = bins;
    m.detNY = bins;
    return m;
}

// A planar slab of glass: two triangles at z = 0 with a refractive index. Used
// to exercise Snell / TIR at a controlled angle of incidence.
MeshSurface glassPlane(double index, double halfSize = 50.0) {
    const double h = halfSize;
    MeshSurface m;
    m.label = QStringLiteral("glass");
    m.verts = {gp_Pnt(-h, -h, 0), gp_Pnt(h, -h, 0), gp_Pnt(h, h, 0), gp_Pnt(-h, h, 0)};
    Triangle a; a.v0 = 0; a.v1 = 1; a.v2 = 2; a.normal = gp_Dir(0, 0, 1);
    Triangle b; b.v0 = 0; b.v1 = 2; b.v2 = 3; b.normal = gp_Dir(0, 0, 1);
    m.tris = {a, b};
    m.index          = index;
    m.transmissivity = 1.0;
    return m;
}

// A cheap deterministic RNG for the randomised comparison tests.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    double next() {
        s += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return double(z >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
};

} // namespace

// ============================================================ intersectTri ==
// The handoff called out Moller-Trumbore as untested; these pin down every
// branch of it.

TEST(intersect, hits_centre_of_triangle) {
    const Vec3 v0(0, 0, 0), e1(1, 0, 0), e2(0, 1, 0);
    double t = 0, u = 0, v = 0;
    CHECK(intersectTriangle(Vec3(0.25, 0.25, -5), Vec3(0, 0, 1), v0, e1, e2, t, u, v));
    CHECK_NEAR(t, 5.0, 1e-12);
    CHECK_NEAR(u, 0.25, 1e-12);
    CHECK_NEAR(v, 0.25, 1e-12);
}

TEST(intersect, misses_outside_barycentric_bounds) {
    const Vec3 v0(0, 0, 0), e1(1, 0, 0), e2(0, 1, 0);
    double t = 0, u = 0, v = 0;
    // u + v > 1: past the hypotenuse but still inside the bounding box.
    CHECK(!intersectTriangle(Vec3(0.9, 0.9, -5), Vec3(0, 0, 1), v0, e1, e2, t, u, v));
    // u < 0.
    CHECK(!intersectTriangle(Vec3(-0.2, 0.3, -5), Vec3(0, 0, 1), v0, e1, e2, t, u, v));
    // v < 0.
    CHECK(!intersectTriangle(Vec3(0.3, -0.2, -5), Vec3(0, 0, 1), v0, e1, e2, t, u, v));
}

TEST(intersect, ray_parallel_to_plane_misses) {
    const Vec3 v0(0, 0, 0), e1(1, 0, 0), e2(0, 1, 0);
    double t = 0, u = 0, v = 0;
    CHECK(!intersectTriangle(Vec3(0.25, 0.25, 0), Vec3(1, 0, 0), v0, e1, e2, t, u, v));
    // Parallel but offset from the plane misses as well.
    CHECK(!intersectTriangle(Vec3(0.25, 0.25, 3), Vec3(1, 0, 0), v0, e1, e2, t, u, v));
}

TEST(intersect, backface_is_hit_on_purpose) {
    // Two-sided intersection is required: a ray inside a refractive solid
    // approaches every exit wall from behind. Culling backfaces here would
    // silently break total internal reflection.
    const Vec3 v0(0, 0, 0), e1(1, 0, 0), e2(0, 1, 0);
    double t = 0, u = 0, v = 0;
    CHECK(intersectTriangle(Vec3(0.25, 0.25, 5), Vec3(0, 0, -1), v0, e1, e2, t, u, v));
    CHECK_NEAR(t, 5.0, 1e-12);
}

TEST(intersect, triangle_behind_the_ray_is_rejected) {
    const Vec3 v0(0, 0, 0), e1(1, 0, 0), e2(0, 1, 0);
    double t = 0, u = 0, v = 0;
    // Origin past the triangle, travelling away from it -> t would be negative.
    CHECK(!intersectTriangle(Vec3(0.25, 0.25, 5), Vec3(0, 0, 1), v0, e1, e2, t, u, v));
}

TEST(intersect, degenerate_triangle_is_rejected) {
    const Vec3 v0(0, 0, 0), e1(1, 0, 0), e2(2, 0, 0);   // zero area
    double t = 0, u = 0, v = 0;
    CHECK(!intersectTriangle(Vec3(0.25, 0.0, -5), Vec3(0, 0, 1), v0, e1, e2, t, u, v));
}

// =================================================================== BVH ====
// The BVH is an optimisation, so the contract is that it never changes an
// answer. These tests hold it to exactly that.

TEST(bvh, matches_brute_force_on_every_scene) {
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        TraceScene scene;
        scene.build(MeshBuilder::build(GeometryProvider::buildScene(GeometryProvider::Scene(s))));
        CHECK(!scene.empty());
        CHECK(scene.nodeCount() > 0);

        Rng rng(0xC0FFEEu + std::uint64_t(s));
        int compared = 0, hits = 0;
        for (int i = 0; i < 1500; ++i) {
            const Vec3 o(rng.range(-200, 200), rng.range(-200, 200), rng.range(-250, 450));
            Vec3 d(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
            if (!d.normalize()) continue;

            RayHit fast, slow;
            const bool a = scene.nearestHit(o, d, fast);
            const bool b = scene.nearestHitBruteForce(o, d, slow);
            ++compared;
            CHECK(a == b);
            if (a && b) {
                ++hits;
                // Same distance; the triangle index can differ only where two
                // coincident triangles are exactly the same distance away.
                CHECK_NEAR(fast.t, slow.t, 1e-9);
            }
        }
        CHECK(compared > 1200);
        CHECK(hits > 20);    // the rays must actually be exercising the geometry
    }
}

TEST(bvh, empty_scene_reports_no_hit) {
    TraceScene scene;
    scene.build(MeshList{});
    CHECK(scene.empty());
    CHECK(scene.nodeCount() == 0);
    RayHit h;
    CHECK(!scene.nearestHit(Vec3(0, 0, 0), Vec3(0, 0, 1), h));
    CHECK(h.tri == -1);
}

TEST(bvh, axis_aligned_ray_on_a_slab_plane_still_hits) {
    // A ray whose origin sits exactly on a bounding-plane produces 0 * inf in
    // the slab test; the traversal must not lose the hit to the resulting NaN.
    MeshList meshes{unitTriangleSurface()};
    TraceScene scene;
    scene.build(meshes);
    RayHit h;
    CHECK(scene.nearestHit(Vec3(0.0, 0.0, -1.0), Vec3(0, 0, 1), h));
    CHECK_NEAR(h.t, 1.0, 1e-9);
}

// ============================================================ ray tracing ===

TEST(trace, energy_is_conserved_in_every_scene) {
    // Every joule emitted must end up in exactly one bucket: on the detector,
    // absorbed by a surface, escaped from the scene, or dropped at the energy
    // cutoff / depth limit. Run across the whole scene registry, so a newly
    // added scene cannot quietly leak energy.
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        for (int t = 0; t <= 1; ++t) {
            SimConfig cfg;
            cfg.scene  = GeometryProvider::Scene(s);
            cfg.source = SourceConfig::Type(t);
            cfg.rays   = 1500;
            const SimulationResult res = Simulation::run(cfg);

            CHECK(res.raysEmitted == 1500);
            CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
            CHECK(res.efficiency >= 0.0 && res.efficiency <= 1.0);
            // The irradiance grid can never hold more than the detector total.
            double gridSum = 0.0;
            for (double v : res.irradiance) gridSum += v;
            CHECK(gridSum <= res.fluxDetector + 1e-9);
        }
    }
}

TEST(trace, every_registered_scene_is_well_formed) {
    // A scene that builds no geometry, forgets its detector, or is aimed so
    // badly that nothing ever reaches the receiver is a broken scene, not an
    // interesting one. With more than twenty of them, this has to be checked
    // mechanically rather than by eye.
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        const auto scene = GeometryProvider::Scene(s);
        const auto& si = GeometryProvider::info(scene);

        CHECK(!si.name.isEmpty());
        CHECK(!si.description.isEmpty());

        const auto surfaces = GeometryProvider::buildScene(scene);
        CHECK(surfaces.size() >= 2);          // at least one optic + a detector

        int detectors = 0, optics = 0;
        for (const auto& os : surfaces) {
            CHECK(!os.label.isEmpty());
            CHECK(!os.shape.IsNull());
            if (os.isDetector) ++detectors;
            else               ++optics;
            // Reflected plus transmitted energy may never exceed what arrived.
            CHECK(os.reflectivity >= 0.0 && os.reflectivity <= 1.0);
            CHECK(os.transmissivity >= 0.0 && os.transmissivity <= 1.0);
            CHECK(os.reflectivity + os.transmissivity <= 1.0 + 1e-12);
            CHECK(os.index == 0.0 || os.index >= 1.0);
        }
        CHECK(detectors == 1);
        CHECK(optics >= 1);

        const TraceScene& traceScene = Simulation::sceneFor(scene);
        CHECK(!traceScene.empty());
        CHECK(traceScene.detector().valid);
        CHECK(traceScene.detector().w > 0.0);
        CHECK(traceScene.detector().h > 0.0);

        // The source must actually illuminate the optic, and some light must
        // find the receiver -- otherwise the scene shows the user nothing.
        SimConfig cfg;
        cfg.scene  = scene;
        cfg.source = SourceConfig::Type::Lambertian;
        cfg.rays   = 4000;
        const SimulationResult res = Simulation::run(cfg);
        CHECK(res.efficiency > 0.005);        // more than half a percent arrives
        CHECK(res.efficiency <= 1.0);
        CHECK(res.raysHitDetector > 0);
        CHECK(!res.raySegments.empty());      // something was actually hit
    }
}

TEST(trace, scene_names_are_unique) {
    for (int a = 0; a < GeometryProvider::count(); ++a)
        for (int b = a + 1; b < GeometryProvider::count(); ++b)
            CHECK(GeometryProvider::info(GeometryProvider::Scene(a)).name !=
                  GeometryProvider::info(GeometryProvider::Scene(b)).name);
}

TEST(trace, reflector_and_guide_stay_in_their_expected_band) {
    // Regression guard on the physics, loose enough to tolerate Monte Carlo
    // noise but tight enough to catch a broken interaction.
    SimConfig cfg;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 20000;

    cfg.scene = GeometryProvider::Scene::Reflector;
    const double reflector = Simulation::run(cfg).efficiency;
    CHECK(reflector > 0.78 && reflector < 0.90);

    // The guide used to sit near 89 %, back when its walls reflected nothing at
    // the ends and its glass absorbed nothing along the way. With the Fresnel
    // split at the entrance and exit faces and Beer-Lambert loss over the
    // zig-zag path it lands in the seventies, which is what a real rod does.
    cfg.scene = GeometryProvider::Scene::LightGuide;
    const double guide = Simulation::run(cfg).efficiency;
    CHECK(guide > 0.68 && guide < 0.82);

    cfg.scene = GeometryProvider::Scene::Lens;
    const double lens = Simulation::run(cfg).efficiency;
    CHECK(lens > 0.05 && lens < 0.25);
}

TEST(trace, point_source_loses_the_backward_hemisphere) {
    // The scene used to silently swap a Point source for a Lambertian one. It
    // no longer does, so an isotropic source must come out near half as
    // efficient as the hemisphere emitter.
    SimConfig cfg;
    cfg.scene = GeometryProvider::Scene::Reflector;
    cfg.rays  = 20000;

    cfg.source = SourceConfig::Type::Lambertian;
    const double lambertian = Simulation::run(cfg).efficiency;
    cfg.source = SourceConfig::Type::Point;
    const double point = Simulation::run(cfg).efficiency;

    CHECK(point < lambertian);
    CHECK(point > 0.2 * lambertian);
}

TEST(trace, result_does_not_depend_on_thread_count) {
    SimConfig cfg;
    cfg.scene  = GeometryProvider::Scene::LightGuide;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 6000;

    cfg.threads = 1;
    const SimulationResult one = Simulation::run(cfg);
    cfg.threads = 4;
    const SimulationResult four = Simulation::run(cfg);
    cfg.threads = 7;              // does not divide the chunk count evenly
    const SimulationResult seven = Simulation::run(cfg);

    // Bit-exact, not just close: rays are seeded from their own index, and the
    // scalar totals are reduced in chunk order rather than thread order, so
    // scheduling cannot leak into the answer.
    CHECK(one.raysEmitted == four.raysEmitted && one.raysEmitted == seven.raysEmitted);
    CHECK(one.raysHitDetector == four.raysHitDetector);
    CHECK(one.raysHitDetector == seven.raysHitDetector);
    CHECK(four.fluxDetector == one.fluxDetector);
    CHECK(seven.fluxDetector == one.fluxDetector);
    CHECK(four.efficiency == one.efficiency);
    CHECK(seven.efficiency == one.efficiency);
    CHECK(four.fluxEscaped == one.fluxEscaped);
    CHECK(four.fluxAbsorbed == one.fluxAbsorbed);
    CHECK(four.fluxTruncated == one.fluxTruncated);

    // The irradiance bins are accumulated per thread rather than per chunk (a
    // per-chunk grid would cost 32 KB a chunk), so they can differ in the last
    // ULPs when the work is split differently.
    CHECK(one.irradiance.size() == four.irradiance.size());
    for (std::size_t i = 0; i < one.irradiance.size(); ++i) {
        CHECK_NEAR(four.irradiance[i], one.irradiance[i], 1e-12);
        CHECK_NEAR(seven.irradiance[i], one.irradiance[i], 1e-12);
    }
}

TEST(trace, repeating_a_run_reproduces_it_exactly) {
    SimConfig cfg;
    cfg.scene  = GeometryProvider::Scene::Lens;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 5000;
    const SimulationResult a = Simulation::run(cfg);
    const SimulationResult b = Simulation::run(cfg);
    CHECK(a.fluxDetector == b.fluxDetector);
    CHECK(a.raysHitDetector == b.raysHitDetector);
    CHECK(a.raySegments.size() == b.raySegments.size());
}

TEST(trace, cancellation_stops_early_and_keeps_partial_work) {
    std::atomic<bool> cancel{false};
    TraceControl ctl;
    ctl.cancel = &cancel;
    // Trip the flag as soon as the run reports any progress at all.
    ctl.progress = [&cancel](std::size_t done, std::size_t) {
        if (done > 0) cancel.store(true, std::memory_order_relaxed);
    };

    SimConfig cfg;
    cfg.scene   = GeometryProvider::Scene::LightGuide;
    cfg.source  = SourceConfig::Type::Lambertian;
    cfg.rays    = 400000;
    cfg.threads = 2;

    Simulation::sceneFor(cfg.scene);      // keep the build out of the measurement
    const SimulationResult res = Simulation::run(cfg, ctl);

    CHECK(res.cancelled);
    CHECK(res.raysEmitted > 0);
    CHECK(res.raysEmitted < std::size_t(cfg.rays));
    // A partial run is still normalised, so its statistics remain meaningful.
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
    CHECK(res.efficiency > 0.0 && res.efficiency <= 1.0);
}

// ============================================================== edge cases ==

TEST(edge, empty_mesh_list_emits_and_escapes) {
    TraceScene scene;
    scene.build(MeshList{});
    SourceConfig src;
    src.rays = 100;
    SimulationResult res;
    RayTracer::trace(scene, src, res);
    CHECK(res.raysEmitted == 100);
    CHECK(res.raysHitDetector == 0);
    CHECK_NEAR(res.efficiency, 0.0, 1e-12);
    CHECK_NEAR(res.fluxEscaped, src.power, 1e-12);
}

TEST(edge, zero_rays_is_not_a_division_by_zero) {
    SimConfig cfg;
    cfg.scene = GeometryProvider::Scene::Reflector;
    cfg.rays  = 0;
    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.raysEmitted == 0);
    CHECK_NEAR(res.efficiency, 0.0, 1e-12);
    CHECK(std::isfinite(res.fluxDetector));
}

TEST(edge, scene_without_a_detector_reports_zero_efficiency) {
    MeshList meshes{unitTriangleSurface(0.9)};
    TraceScene scene;
    scene.build(meshes);
    CHECK(!scene.detector().valid);

    SourceConfig src;
    src.origin = gp_Pnt(0.25, 0.25, -5);
    src.axis   = gp_Dir(0, 0, 1);
    src.type   = SourceConfig::Type::Lambertian;
    src.rays   = 500;

    SimulationResult res;
    RayTracer::trace(scene, src, res);
    CHECK(res.raysHitDetector == 0);
    CHECK_NEAR(res.efficiency, 0.0, 1e-12);
    CHECK_NEAR(res.fluxAccounted(), src.power, 1e-9);
}

TEST(edge, source_aimed_away_from_the_optic_detects_nothing) {
    MeshList meshes{detectorSurface(20.0, 10.0)};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.origin = gp_Pnt(0, 0, 0);
    src.axis   = gp_Dir(0, 0, -1);        // hemisphere points away from z = +10
    src.type   = SourceConfig::Type::Lambertian;
    src.rays   = 2000;

    SimulationResult res;
    RayTracer::trace(scene, src, res);
    CHECK(res.raysHitDetector == 0);
    CHECK_NEAR(res.fluxEscaped, src.power, 1e-9);
}

TEST(edge, rays_outside_the_receiver_grid_are_not_binned) {
    // The detector plane is larger than the frame the tracer bins against, so
    // a hit off the edge of the grid must be counted in the flux total without
    // corrupting a bin (or writing out of bounds).
    MeshList meshes{detectorSurface(100.0, 10.0, 4)};
    meshes[0].detW = 10.0;    // shrink the receiver frame, keep the big plane
    meshes[0].detH = 10.0;
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.origin = gp_Pnt(0, 0, 0);
    src.axis   = gp_Dir(0, 0, 1);
    src.type   = SourceConfig::Type::Lambertian;
    src.rays   = 4000;

    SimulationResult res;
    RayTracer::trace(scene, src, res);
    CHECK(res.raysHitDetector > 0);

    double gridSum = 0.0;
    for (double v : res.irradiance) gridSum += v;
    CHECK(res.irradiance.size() == 16);
    CHECK(gridSum > 0.0);
    // Strictly less: the wide-angle rays land outside the 10 x 10 frame.
    CHECK(gridSum < res.fluxDetector - 1e-9);
}

TEST(edge, grazing_incidence_conserves_energy) {
    // Near-tangential hits are where the epsilon offsets are most likely to
    // leak a ray back into the surface it just left.
    MeshList meshes{glassPlane(1.5), detectorSurface(400.0, 60.0)};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.origin = gp_Pnt(0, 0, -0.001);    // a micron above the glass
    src.axis   = gp_Dir(0, 0, 1);
    src.type   = SourceConfig::Type::Lambertian;
    src.rays   = 4000;

    SimulationResult res;
    RayTracer::trace(scene, src, res);
    CHECK_NEAR(res.fluxAccounted(), src.power, 1e-9);
    CHECK(std::isfinite(res.fluxDetector));
}

// ============================================================ TIR / Snell ===

namespace {

constexpr double kPi = 3.14159265358979323846;

// A large planar surface through `point` with the given unit normal, built as
// two triangles. Used to set up an exact angle of incidence.
MeshSurface tiltedPlane(const Vec3& point, const Vec3& normal, double halfSize,
                        double index, double transmissivity, bool detector = false) {
    Vec3 n = normal;
    n.normalize();
    Vec3 u = (std::fabs(n.z) < 0.9) ? Vec3(0, 0, 1).cross(n) : Vec3(1, 0, 0).cross(n);
    u.normalize();
    Vec3 v = n.cross(u);
    v.normalize();

    auto corner = [&](double a, double b) {
        const Vec3 c = point + u * (a * halfSize) + v * (b * halfSize);
        return gp_Pnt(c.x, c.y, c.z);
    };

    MeshSurface m;
    m.label = detector ? QStringLiteral("tilted detector") : QStringLiteral("tilted glass");
    m.verts = {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1)};
    Triangle t0; t0.v0 = 0; t0.v1 = 1; t0.v2 = 2; t0.normal = gp_Dir(n.x, n.y, n.z);
    Triangle t1; t1.v0 = 0; t1.v1 = 2; t1.v2 = 3; t1.normal = gp_Dir(n.x, n.y, n.z);
    m.tris = {t0, t1};
    m.index          = index;
    m.transmissivity = transmissivity;
    m.isDetector     = detector;
    if (detector) {
        // Total flux only; the receiver is not axis-aligned, so leave the
        // binning frame empty rather than binning against the wrong plane.
        m.detCenter = gp_Pnt(point.x, point.y, point.z);
        m.detW = m.detH = 0.0;
    }
    return m;
}

// Sends one ray into a glass half-space at normal incidence (so it enters
// without bending), then onto a second glass wall tilted so the ray meets it at
// exactly `angleDeg`. A detector sits immediately past that wall, on the far
// side, so it can only be reached by energy that refracted through: reflected
// energy stays on the near side by construction.
//
// Returns the fraction of the ray that got through the tilted wall.
double transmittedThroughTiltedWall(double index, double angleDeg) {
    const double a = angleDeg * kPi / 180.0;
    const Vec3 wallPoint(0, 0, 50);
    const Vec3 wallNormal(std::sin(a), 0.0, std::cos(a));   // ray travels +Z

    MeshList meshes;
    meshes.push_back(tiltedPlane(Vec3(0, 0, 0), Vec3(0, 0, 1), 800.0, index, 1.0));
    meshes.push_back(tiltedPlane(wallPoint, wallNormal, 800.0, index, 1.0));
    meshes.push_back(tiltedPlane(wallPoint + wallNormal * 2.0, wallNormal, 4000.0,
                                 0.0, 0.0, /*detector=*/true));

    TraceScene scene;
    scene.build(meshes);

    SimulationResult res;
    RayTracer::traceSingleRay(scene, Vec3(0, 0, -10), Vec3(0, 0, 1), res);
    return res.fluxDetector;
}

} // namespace

TEST(tir, critical_angle_separates_refraction_from_total_reflection) {
    const double index = 1.5;
    const double criticalDeg = std::asin(1.0 / index) * 180.0 / kPi;
    CHECK_NEAR(criticalDeg, 41.8103, 1e-3);

    // Below the critical angle the ray refracts out and reaches the receiver.
    CHECK_NEAR(transmittedThroughTiltedWall(index, criticalDeg - 12.0), 1.0, 1e-9);
    CHECK_NEAR(transmittedThroughTiltedWall(index, criticalDeg - 2.0), 1.0, 1e-9);
    // Above it the ray is totally internally reflected and nothing gets through.
    CHECK_NEAR(transmittedThroughTiltedWall(index, criticalDeg + 2.0), 0.0, 1e-9);
    CHECK_NEAR(transmittedThroughTiltedWall(index, criticalDeg + 20.0), 0.0, 1e-9);
}

TEST(tir, critical_angle_moves_with_the_refractive_index) {
    // n = 2.0 has a much tighter critical angle (30 degrees) than n = 1.5, so an
    // angle between the two must switch behaviour when the index changes.
    const double between = 35.0;
    CHECK_NEAR(transmittedThroughTiltedWall(1.5, between), 1.0, 1e-9);   // below 41.8
    CHECK_NEAR(transmittedThroughTiltedWall(2.0, between), 0.0, 1e-9);   // above 30.0
}

TEST(tir, normal_incidence_never_totally_reflects) {
    // sin(theta) = 0 can never exceed 1/n, whatever the index.
    for (double index : {1.1, 1.5, 2.0, 3.0})
        CHECK_NEAR(transmittedThroughTiltedWall(index, 0.0), 1.0, 1e-9);
}

TEST(tir, light_guide_beats_the_same_geometry_without_tir) {
    // The guide only works because rays beyond the critical angle bounce along
    // it. With a much lower index the critical angle opens up and rays leak out
    // through the walls, so efficiency must drop sharply.
    SimConfig cfg;
    cfg.scene  = GeometryProvider::Scene::LightGuide;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 8000;
    const double guided = Simulation::run(cfg).efficiency;

    auto surfaces = GeometryProvider::buildScene(GeometryProvider::Scene::LightGuide);
    for (auto& s : surfaces)
        if (s.index > 0.0) s.index = 1.02;      // critical angle ~ 78 degrees
    TraceScene leaky;
    leaky.build(MeshBuilder::build(surfaces));

    SimulationResult res;
    RayTracer::trace(leaky,
                     Simulation::sourceFor(GeometryProvider::Scene::LightGuide,
                                           SourceConfig::Type::Lambertian, 8000),
                     res);
    CHECK(res.efficiency < guided);
}

// ============================================================== sampling ====

TEST(sampling, lambertian_stays_in_the_forward_hemisphere) {
    SourceConfig src;
    src.type = SourceConfig::Type::Lambertian;
    src.axis = gp_Dir(0, 0, 1);

    double meanCos = 0.0;
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
        const Vec3 d = RayTracer::sampleDirection(src, std::size_t(i), 12345u);
        CHECK_NEAR(d.length(), 1.0, 1e-12);
        CHECK(d.z >= -1e-12);
        meanCos += d.z;
    }
    meanCos /= n;
    // Cosine-weighted hemisphere: E[cos] = 2/3.
    CHECK_NEAR(meanCos, 2.0 / 3.0, 0.02);
}

TEST(sampling, point_source_covers_the_whole_sphere) {
    SourceConfig src;
    src.type = SourceConfig::Type::Point;
    src.axis = gp_Dir(0, 0, 1);

    int forward = 0, backward = 0;
    double meanCos = 0.0;
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
        const Vec3 d = RayTracer::sampleDirection(src, std::size_t(i), 12345u);
        CHECK_NEAR(d.length(), 1.0, 1e-12);
        (d.z >= 0 ? forward : backward)++;
        meanCos += d.z;
    }
    meanCos /= n;
    CHECK_NEAR(meanCos, 0.0, 0.02);              // isotropic
    CHECK(forward > int(0.45 * n) && forward < int(0.55 * n));
    CHECK(backward > int(0.45 * n));
}

TEST(sampling, azimuth_of_one_ray_is_independent_of_the_next_rays_polar_angle) {
    // Regression guard. An earlier version strided the per-ray seed by exactly
    // the constant splitmix64 adds internally, which made ray i's SECOND draw
    // bit-identical to ray i+1's FIRST draw -- so the azimuth of every ray was
    // a linear function of the polar angle of the ray after it. Every aggregate
    // statistic (mean direction, hemisphere split) still looked correct; only
    // this pairing exposes it.
    SourceConfig src;
    src.type = SourceConfig::Type::Point;   // cos(theta) = 2*u1 - 1, phi = 2*pi*u2
    src.axis = gp_Dir(0, 0, 1);

    const int n = 40000;
    double sumA = 0, sumB = 0, sumAA = 0, sumBB = 0, sumAB = 0;
    for (int i = 0; i < n; ++i) {
        const Vec3 a = RayTracer::sampleDirection(src, std::size_t(i), 12345u);
        const Vec3 b = RayTracer::sampleDirection(src, std::size_t(i) + 1, 12345u);
        const double x = std::atan2(a.y, a.x);   // driven by ray i's second draw
        const double y = b.z;                    // driven by ray i+1's first draw
        sumA += x; sumB += y; sumAA += x * x; sumBB += y * y; sumAB += x * y;
    }
    const double covar = sumAB / n - (sumA / n) * (sumB / n);
    const double sdA = std::sqrt(std::max(1e-30, sumAA / n - (sumA / n) * (sumA / n)));
    const double sdB = std::sqrt(std::max(1e-30, sumBB / n - (sumB / n) * (sumB / n)));
    const double correlation = covar / (sdA * sdB);
    // 1 sigma is about 0.005 at this sample size; the old seeding scored ~1.0.
    CHECK_NEAR(correlation, 0.0, 0.03);
}

TEST(sampling, is_reproducible_for_a_given_index) {
    SourceConfig src;
    src.type = SourceConfig::Type::Lambertian;
    for (std::size_t i = 0; i < 50; ++i) {
        const Vec3 a = RayTracer::sampleDirection(src, i, 999u);
        const Vec3 b = RayTracer::sampleDirection(src, i, 999u);
        CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
    }
}

// =============================================================== detector ===

TEST(detector, frame_is_centred_on_the_receiver_rectangle) {
    auto surfaces = GeometryProvider::buildScene(GeometryProvider::Scene::Reflector);
    const MeshList meshes = MeshBuilder::build(surfaces);
    bool found = false;
    for (const auto& m : meshes) {
        if (!m.isDetector) continue;
        found = true;
        CHECK(m.detW > 0.0 && m.detH > 0.0);
        CHECK(m.detNX == 64 && m.detNY == 64);
        CHECK_NEAR(m.detCenter.X(), 0.0, 1e-9);
        CHECK_NEAR(m.detCenter.Y(), 0.0, 1e-9);
        CHECK_NEAR(m.detCenter.Z(), 320.0, 1e-6);
    }
    CHECK(found);
}

TEST(detector, offset_receiver_bins_around_its_own_centre) {
    // Shift the receiver off the optical axis; the binning must follow it
    // rather than assuming the grid is centred on the origin.
    MeshSurface det = detectorSurface(20.0, 10.0, 2);
    for (auto& p : det.verts) p = gp_Pnt(p.X() + 100.0, p.Y(), p.Z());
    det.detCenter = gp_Pnt(100.0, 0.0, 10.0);

    TraceScene scene;
    scene.build(MeshList{det});

    SourceConfig src;
    src.origin = gp_Pnt(100.0, 0.0, 0.0);    // straight at the shifted centre
    src.axis   = gp_Dir(0, 0, 1);
    src.type   = SourceConfig::Type::Lambertian;
    src.rays   = 2000;

    SimulationResult res;
    RayTracer::trace(scene, src, res);
    CHECK(res.raysHitDetector > 0);
    double gridSum = 0.0;
    for (double v : res.irradiance) gridSum += v;
    CHECK(gridSum > 0.0);
    CHECK_NEAR(gridSum, res.fluxDetector, 1e-9);   // nothing fell outside the grid
}

// ================================================================= worker ====
// The window drives the tracer through SimulationWorker, so the async plumbing
// it depends on -- queued result delivery and cancellation from another thread
// -- is covered here rather than only by hand.

namespace {

// Spins the event loop until `done` or the timeout expires. Returns true if the
// condition was met.
bool waitFor(const std::function<bool()>& done, int timeoutMs = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

} // namespace

TEST(worker, delivers_a_result_without_blocking_the_caller) {
    SimulationWorker worker;
    SimulationResult got;
    bool finished = false;
    int  lastPercent = -1;
    int  progressCalls = 0;

    QObject::connect(&worker, &SimulationWorker::resultReady,
                     [&](const SimulationResult& r) { got = r; finished = true; });
    QObject::connect(&worker, &SimulationWorker::progress, [&](int pct) {
        CHECK(pct >= lastPercent);      // progress never goes backwards
        CHECK(pct >= 0 && pct <= 100);
        lastPercent = pct;
        ++progressCalls;
    });

    SimConfig cfg;
    cfg.scene  = GeometryProvider::Scene::Reflector;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 60000;

    worker.startRun(cfg);
    // The call returns immediately; the caller keeps running its event loop.
    CHECK(waitFor([&] { return finished; }));

    CHECK(got.raysEmitted == 60000);
    CHECK(!got.cancelled);
    CHECK(got.efficiency > 0.0);
    CHECK(progressCalls > 0);
    CHECK(lastPercent == 100);
    worker.wait();
}

TEST(worker, cancel_stops_a_long_run_and_reports_it) {
    SimulationWorker worker;
    SimulationResult got;
    bool finished = false;

    QObject::connect(&worker, &SimulationWorker::resultReady,
                     [&](const SimulationResult& r) { got = r; finished = true; });
    // Cancel as soon as the run reports any progress at all.
    QObject::connect(&worker, &SimulationWorker::progress,
                     [&](int) { worker.cancel(); });

    SimConfig cfg;
    cfg.scene  = GeometryProvider::Scene::Lens;   // the slowest scene
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 5000000;

    Simulation::sceneFor(cfg.scene);              // exclude the one-off build
    worker.startRun(cfg);
    CHECK(waitFor([&] { return finished; }));

    CHECK(worker.cancelRequested());
    CHECK(got.cancelled);
    CHECK(got.raysEmitted < std::size_t(cfg.rays));
    CHECK_NEAR(got.fluxAccounted(), got.sourcePower, 1e-9);
    worker.wait();
}

TEST(worker, a_second_start_while_running_is_ignored) {
    SimulationWorker worker;
    int results = 0;
    QObject::connect(&worker, &SimulationWorker::resultReady,
                     [&](const SimulationResult&) { ++results; });

    SimConfig cfg;
    cfg.scene  = GeometryProvider::Scene::LightGuide;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 200000;

    worker.startRun(cfg);
    worker.startRun(cfg);        // must not launch a second trace
    CHECK(waitFor([&] { return results > 0; }));
    worker.wait();
    QCoreApplication::processEvents();
    CHECK(results == 1);
}

// =============================================================== patterns ===
// The scene descriptions make claims about what the receiver shows -- a line, a
// ring, a dark centre. Those are the interesting part of each scene, so they are
// asserted rather than left to whoever looks at the heatmap.

namespace {

struct PatternStats {
    double sdx = 0.0, sdy = 0.0;   // spread of the irradiance, in bins
    double peakRadius = 0.0;       // distance from the centroid to the brightest bin
    double centreOverPeak = 0.0;   // 0 means a completely dark centre
};

PatternStats patternOf(GeometryProvider::Scene scene, int rays = 40000) {
    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = rays;
    const SimulationResult r = Simulation::run(cfg);

    PatternStats st;
    double total = 0.0, peak = 0.0, cx = 0.0, cy = 0.0;
    int px = 0, py = 0;
    for (int y = 0; y < r.ny; ++y) {
        for (int x = 0; x < r.nx; ++x) {
            const double v = r.irradiance[std::size_t(y) * std::size_t(r.nx) + std::size_t(x)];
            total += v;
            cx += v * x;
            cy += v * y;
            if (v > peak) { peak = v; px = x; py = y; }
        }
    }
    if (total <= 0.0 || peak <= 0.0) return st;
    cx /= total;
    cy /= total;

    double vx = 0.0, vy = 0.0;
    for (int y = 0; y < r.ny; ++y) {
        for (int x = 0; x < r.nx; ++x) {
            const double v = r.irradiance[std::size_t(y) * std::size_t(r.nx) + std::size_t(x)];
            vx += v * (x - cx) * (x - cx);
            vy += v * (y - cy) * (y - cy);
        }
    }
    st.sdx = std::sqrt(vx / total);
    st.sdy = std::sqrt(vy / total);
    st.peakRadius = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    st.centreOverPeak =
        r.irradiance[std::size_t(r.ny / 2) * std::size_t(r.nx) + std::size_t(r.nx / 2)] / peak;
    return st;
}

} // namespace

TEST(patterns, rotationally_symmetric_scenes_produce_round_spots) {
    for (auto scene : {GeometryProvider::Scene::Reflector,
                       GeometryProvider::Scene::Lens,
                       GeometryProvider::Scene::LightGuide}) {
        const PatternStats st = patternOf(scene);
        CHECK(st.sdx > 0.0 && st.sdy > 0.0);
        // Everything about these scenes is symmetric about z, so x and y must
        // come out the same to within Monte Carlo noise.
        CHECK_NEAR(st.sdx / st.sdy, 1.0, 0.08);
    }
}

TEST(patterns, one_axis_optics_produce_a_line_not_a_spot) {
    // A trough, a rod lens and a prism all have power (or deviation) in x only,
    // so the receiver has to be measurably narrower in x than in y.
    for (auto scene : {GeometryProvider::Scene::ParabolicTrough,
                       GeometryProvider::Scene::CylindricalLens,
                       GeometryProvider::Scene::Prism,
                       GeometryProvider::Scene::PorroPrism}) {
        const PatternStats st = patternOf(scene);
        CHECK(st.sdx > 0.0 && st.sdy > 0.0);
        CHECK(st.sdy > 1.3 * st.sdx);
    }
}

TEST(patterns, axicon_forms_a_ring_with_a_dark_centre) {
    // Fed a diverging point source directly a cone just blurs; the ring only
    // appears because the scene collimates first. If the collimator were
    // dropped the centre would light up and this would fail.
    const PatternStats st = patternOf(GeometryProvider::Scene::Axicon);
    CHECK(st.centreOverPeak < 0.45);   // measured about 0.16
    CHECK(st.peakRadius > 6.0);        // measured about 14 bins off centre
    CHECK_NEAR(st.sdx / st.sdy, 1.0, 0.08);   // the ring is still round
}

TEST(patterns, cassegrain_secondary_obstructs_the_axis) {
    const PatternStats st = patternOf(GeometryProvider::Scene::Cassegrain);
    CHECK(st.centreOverPeak < 0.2);    // measured 0.0 -- fully shadowed
    CHECK(st.peakRadius > 6.0);
}

TEST(patterns, concentrators_beat_the_naive_cone) {
    // The whole point of a compound parabolic concentrator is that it collects
    // more than a plain reflective funnel doing the same job.
    SimConfig cfg;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 20000;
    cfg.scene  = GeometryProvider::Scene::Cpc;
    const double cpc = Simulation::run(cfg).efficiency;
    cfg.scene  = GeometryProvider::Scene::ConicalConcentrator;
    const double cone = Simulation::run(cfg).efficiency;
    CHECK(cpc > cone);
}

TEST(patterns, tapering_a_light_guide_costs_efficiency) {
    // A taper trades area for angle, pushing rays past the critical angle.
    SimConfig cfg;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = 20000;
    cfg.scene  = GeometryProvider::Scene::LightGuide;
    const double straight = Simulation::run(cfg).efficiency;
    cfg.scene  = GeometryProvider::Scene::TaperedLightGuide;
    const double tapered = Simulation::run(cfg).efficiency;
    CHECK(tapered < straight);
}

TEST(patterns, retroreflectors_only_score_by_sending_light_back) {
    // Both scenes put the receiver where no directly emitted ray can reach it:
    // the corner cube's receiver is behind the source, the Porro prism's is on
    // the far side. Any flux at all is therefore returned light.
    for (auto scene : {GeometryProvider::Scene::CornerCube,
                       GeometryProvider::Scene::PorroPrism}) {
        SimConfig cfg;
        cfg.scene  = scene;
        cfg.source = SourceConfig::Type::Lambertian;
        cfg.rays   = 20000;
        const SimulationResult res = Simulation::run(cfg);
        CHECK(res.efficiency > 0.02);

        // Confirm the geometry really does block the direct path: every emitted
        // ray leaves the source in the hemisphere facing away from the receiver.
        const SourceConfig src =
            Simulation::sourceFor(scene, SourceConfig::Type::Lambertian, 1);
        const double receiverZ = Simulation::sceneFor(scene).detector().center.z;
        const double towardReceiver = (receiverZ > src.origin.Z()) ? 1.0 : -1.0;
        for (std::size_t i = 0; i < 400; ++i) {
            const Vec3 d = RayTracer::sampleDirection(src, i, 12345u);
            CHECK(d.z * towardReceiver <= 1e-12);
        }
    }
}

#include "NewTests.inc"
#include "ThetaBinTests.inc"
#include "MeasuredDataTests.inc"
#include "SceneParamsTests.inc"
#include "AtomicGridTests.inc"
#include "MultiEditTests.inc"
#include "AdaptiveMeshTests.inc"
#include "AuditTests.inc"
#include "BackendCheckTests.inc"
#include "GpuBackendTests.inc"
#include "BackwardTests.inc"
#include "EnvironmentTests.inc"
#include "MetasurfaceTests.inc"

// Tests for the assembled-scene document: the object model the scene tree, the
// object library and the property editor are all views onto.
#include "SceneDocTests.inc"

// Tests for the Appearance preview's scene side -- the SurfaceOptics ->
// Graphic3d_BSDF mapping and the presentations built from it. Everything up to
// the GPU; the image itself is reviewed by eye, because a path-traced frame is
// not bit-stable and asserting on pixels would produce a permanently red build.
#include "AppearanceTests.inc"

// =================================================================== main ===

namespace {

// Every distinct suite name in the registry, in the order the suites were first
// registered. This is what CTest is driven from, so it is deliberately the same
// registry the runner iterates -- there is no second list that could disagree.
std::vector<std::string> suiteNames() {
    std::vector<std::string> names;
    for (const auto& tc : registry())
        if (std::find(names.begin(), names.end(), tc.suite) == names.end())
            names.push_back(tc.suite);
    return names;
}

} // namespace

int main(int argc, char** argv) {
    // Answered before QCoreApplication is constructed: the build-time discovery
    // step runs this on a machine that has only just linked the binary, and a
    // suite list should not depend on Qt finding a platform plugin.
    if (argc > 1 && std::strcmp(argv[1], "--list-suites") == 0) {
        for (const std::string& n : suiteNames()) std::printf("%s\n", n.c_str());
        return 0;
    }

    // Queued signal delivery needs an event loop to dispatch into.
    QCoreApplication app(argc, argv);
    const std::string filter = (argc > 1) ? argv[1] : std::string();

    int run = 0;
    for (const auto& tc : registry()) {
        if (!filter.empty() && tc.suite != filter) continue;
        ++run;
        g_currentTest = tc.suite + "." + tc.name;
        const int before = g_failures;
        std::printf("[ RUN ] %s\n", g_currentTest.c_str());
        try {
            tc.fn();
        } catch (const std::exception& e) {
            reportFailure(__FILE__, __LINE__, std::string("threw: ") + e.what());
        } catch (...) {
            reportFailure(__FILE__, __LINE__, "threw an unknown exception");
        }
        std::printf("[ %s ] %s\n", (g_failures == before) ? "OK  " : "FAIL",
                    g_currentTest.c_str());
    }

    if (run == 0) {
        std::printf("no tests matched filter '%s'\n", filter.c_str());
        return 2;
    }
    std::printf("\n%d test(s), %d check(s), %d failure(s)\n", run, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
