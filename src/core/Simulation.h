#pragma once
#include <cstdint>
#include <memory>
#include "GeometryProvider.h"
#include "Mesh.h"
#include "RayTracer.h"
#include "SimulationResult.h"
#include "TraceScene.h"

// High-level simulation configuration: everything the UI can set, in one
// serialisable struct.
struct SimConfig {
    GeometryProvider::Scene scene = GeometryProvider::Scene::Reflector;

    // Geometry parameters. `useSceneDefaults` keeps a config valid across a
    // scene change -- the parameters of one scene mean nothing to another, so
    // the flag says "whatever this scene's defaults are" rather than carrying
    // stale numbers over.
    SceneParams params;
    bool        useSceneDefaults = true;

    // Source. Split out of SourceConfig rather than embedding it because the
    // origin and axis are a property of the scene, not of the user's choice.
    SourceConfig::Type     source   = SourceConfig::Type::Point;
    SourceConfig::Shape    shape    = SourceConfig::Shape::PointLike;
    SourceConfig::Spectrum spectrum = SourceConfig::Spectrum::Monochrome;
    double halfAngleDeg = 180.0;
    double sizeA        = 0.0;
    double sizeB        = 0.0;
    double beamRadius   = 25.0;
    double wavelengthNm = 587.6;

    int            rays    = 10000;
    unsigned       threads = 0;      // 0 == every hardware thread
    std::uint64_t  seed    = 12345u;
    PhysicsOptions physics;

    // Far-field intensity binning; 0 theta bins switches it off.
    int nTheta = 90;
    int nPhi   = 72;

    // Effective geometry parameters for this config.
    SceneParams effectiveParams() const {
        return useSceneDefaults ? GeometryProvider::defaultParams(scene)
                                : GeometryProvider::sanitise(scene, params);
    }
};

// Facade: geometry -> meshes -> BVH -> Monte Carlo trace -> result.
class Simulation {
public:
    // Ray-trace-ready geometry for one (scene, parameters) pair, built once and
    // then reused. Building it runs OCCT tessellation plus a BVH build, which
    // dwarfs a small trace.
    struct SceneData {
        std::vector<OpticalSurface> surfaces;      // B-Rep + optics, for the 3D viewer
        TraceScene                  scene;         // triangles + BVH, for the tracer
        gp_Pnt                      sourceOrigin;  // where the parameters put the emitter
        gp_Dir                      sourceAxis;
    };
    // Shared ownership rather than a bare reference: dragging a parameter spin
    // box mints a new scene per step, so the cache has to be able to drop old
    // ones while a run still holds the geometry it is tracing.
    using SceneRef = std::shared_ptr<const SceneData>;

    static SimulationResult run(const SimConfig& cfg,
                                const TraceControl& ctl = TraceControl{});

    // Geometry for an arbitrary parameter set. Thread-safe.
    // `buildSecondsOut`, when given, receives the time this call spent building
    // the scene -- zero when it was already cached.
    static SceneRef dataFor(GeometryProvider::Scene scene, const SceneParams& params,
                            double* buildSecondsOut = nullptr);
    static SceneRef dataFor(const SimConfig& cfg, double* buildSecondsOut = nullptr);

    // The emission setup a scene uses at the given parameters. Exposed so tests
    // and the diagnostics paths do not have to duplicate the placement.
    static SourceConfig sourceFor(const SimConfig& cfg, const SceneData& data);
    static SourceConfig sourceFor(GeometryProvider::Scene scene,
                                  SourceConfig::Type type, int rays);

    // The scene at its default parameters. These entries are pinned for the
    // lifetime of the process, so the returned reference stays valid.
    static const TraceScene& sceneFor(GeometryProvider::Scene scene,
                                      double* buildSecondsOut = nullptr);
    static const std::vector<OpticalSurface>& surfacesFor(GeometryProvider::Scene scene);

    static void clearCache();
};
