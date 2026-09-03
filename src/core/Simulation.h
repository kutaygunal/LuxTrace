// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <cstdint>
#include <memory>
#include "GeometryProvider.h"
#include "Mesh.h"
#include "RayTracer.h"
#include "SimulationResult.h"
#include "TraceScene.h"

// One source beyond the one the scene places, described independently of it.
//
// SimConfig held exactly one source, placed by the scene, so a luminaire with
// more than one LED -- and any system needing a stray-light source alongside
// the signal source -- could not be built at all. A spec is placed *relative*
// to the scene emitter by default, which is what makes a four-LED array four
// copies of one spec at four offsets rather than four hand-placed sources.
struct SourceSpec {
    QString label;

    SourceConfig::Type  type  = SourceConfig::Type::Lambertian;
    SourceConfig::Shape shape = SourceConfig::Shape::PointLike;
    SpectrumConfig      spectrum;

    double halfAngleDeg = 90.0;
    double sizeA        = 0.0;
    double sizeB        = 0.0;
    double beamRadius   = 25.0;

    // In the run unit, which the first source fixes. Ray budgets are shared
    // between sources in proportion to this, which is the variance-optimal
    // split: a source contributing a tenth of the light gets a tenth of the
    // rays and every source ends up with a comparable error bar.
    double power = 1.0;

    int polarisationState = 0;

    // Where it sits. `offset` is millimetres from the scene emitter unless
    // `absolute`, in which case it is a world position.
    bool   absolute = false;
    gp_Pnt offset{0, 0, 0};
    // Emission axis. Empty means "whichever way the scene aims its emitter",
    // so a second LED beside the first faces the same optic without anyone
    // having to restate the geometry.
    bool   useSceneAxis = true;
    gp_Dir axis{0, 0, 1};

    // A measured ray set standing in for the analytic emitter above.
    std::shared_ptr<const RayFileData> rayFile;
    double rayFileScale       = 1.0;
    bool   rayFileWavelengths = true;

    bool tracesRayFile() const { return rayFile && rayFile->valid(); }
};

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

    // Geometry read from a CAD file, standing in for the registry's scene.
    // Null -- the ordinary case -- traces `scene` at `params`; set, it traces
    // exactly these surfaces from exactly this source placement, and `scene`
    // and `params` are not consulted at all.
    //
    // This is what makes an imported part traceable. It used to reach the 3D
    // view and stop there, so a run after an import silently traced whichever
    // built-in scene the controls still had selected -- the right rays through
    // the wrong solid.
    //
    // Shared and const because a config is copied onto a worker thread per run
    // and once more per study evaluation, and the geometry behind it is
    // read-only: copying it would be copying a B-Rep per trace.
    std::shared_ptr<const GeometryProvider::SceneSetup> imported;

    bool tracesImport() const { return imported != nullptr; }

    // What to call the geometry this config traces. The imported file where
    // there is one, the registry's name otherwise.
    QString sceneName() const;
    QString sceneDescription() const;

    // Source. Split out of SourceConfig rather than embedding it because the
    // origin and axis are a property of the scene, not of the user's choice.
    SourceConfig::Type  source = SourceConfig::Type::Point;
    SourceConfig::Shape shape  = SourceConfig::Shape::PointLike;
    // The emission spectrum: a distribution, with the monochromatic line and
    // the colour temperature living on it.
    SpectrumConfig spectrum;
    double halfAngleDeg = 180.0;
    double sizeA        = 0.0;
    double sizeB        = 0.0;
    double beamRadius   = 25.0;

    // Total emitted flux and the unit it is quoted in. This is what turns every
    // output from a dimensionless fraction into a number that can go in a
    // specification: W/m^2 or lux on the receiver, W/sr or candela in the far
    // field.
    double   power    = 1.0;
    FluxUnit fluxUnit = FluxUnit::Watt;

    // The polarisation state the source emits, on a polarised trace.
    // 0 unpolarised, 1 linear s, 2 linear p, 3 circular.
    int polarisationState = 0;

    // A measured ray set for the primary source. With one loaded, the angular
    // law, the emitter shape and the sizes above are not consulted: the file
    // carries every ray already. This is what turns "model an LED" into "use
    // this LED", and it is the interoperation an illumination engineer asks
    // for first.
    std::shared_ptr<const RayFileData> rayFile;
    double rayFileScale       = 1.0;
    bool   rayFileWavelengths = true;

    // Everything beyond the primary source. Empty is the ordinary case, and a
    // run with an empty list is bit-identical to what it was before there was
    // a list at all.
    std::vector<SourceSpec> extraSources;

    // How many sources this configuration traces.
    int sourceCount() const { return 1 + int(extraSources.size()); }

    // Receiver resolution. 0 keeps the mesher's default grid.
    int detectorBins = 0;

    // Per-surface optical edits. Applied at trace time, so changing one costs a
    // trace rather than a rebuild.
    std::vector<SurfaceOverride> surfaceOverrides;

    // Stray-light path analysis. Off by default; a run with it on costs the
    // aggregation and nothing else.
    StrayPathOptions strayPaths;

    // See TraceOptions::noiseMap and TraceOptions::deterministicGrids.
    bool noiseMap = false;
    bool deterministicGrids = true;

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
                            double* buildSecondsOut = nullptr, int detectorBins = 0);
    static SceneRef dataFor(const SimConfig& cfg, double* buildSecondsOut = nullptr);

    // Geometry for a scene that was assembled rather than looked up -- an
    // imported CAD file, with its receiver and its source placement. Cached on
    // the identity of the setup, so a second run on the same import, and a
    // convergence study's dozen of them, tessellate and build the hierarchy
    // once rather than once each.
    static SceneRef dataFor(const std::shared_ptr<const GeometryProvider::SceneSetup>& setup,
                            int detectorBins = 0, double* buildSecondsOut = nullptr);

    // The emission setup a scene uses at the given parameters. Exposed so tests
    // and the diagnostics paths do not have to duplicate the placement.
    static SourceConfig sourceFor(const SimConfig& cfg, const SceneData& data);

    // Every source the configuration traces, placed and with the run ray budget
    // already shared between them in proportion to power. The first entry is
    // exactly what sourceFor returns, so a single-source configuration produces
    // a one-element list carrying the whole budget.
    static std::vector<SourceConfig> sourcesFor(const SimConfig& cfg,
                                                const SceneData& data);
    // The same, against a bare emitter placement rather than a built scene.
    //
    // Placing the extra sources is one rule -- offset from the scene emitter
    // unless absolute, aimed the way it aims unless overridden -- and the 3D
    // view has to draw exactly the placement the trace will use or the picture
    // is a second, differently-wrong answer. So the view asks for the same
    // list, and an imported setup (which carries a placement but no SceneData)
    // can ask for it too.
    static std::vector<SourceConfig> sourcesFor(const SimConfig& cfg,
                                                const gp_Pnt& origin,
                                                const gp_Dir& axis);
    static SourceConfig sourceFor(GeometryProvider::Scene scene,
                                  SourceConfig::Type type, int rays);

    // The emitter a scene is designed around, where it is designed around a
    // particular one rather than around "a source, on the axis".
    //
    // Most of the library is the second kind: a ball lens teaches the same
    // lesson under a point source, a laser or an LED, so the scene declines to
    // choose and the user's own emitter is traced unchanged. A fixture is the
    // first kind -- an LED luminaire with a point source in it is not a
    // luminaire, it is a thought experiment -- so the scene says what belongs
    // at its focus and the document builds that.
    //
    // Advice, not enforcement, and it reaches exactly one place: the emitter
    // object a freshly loaded scene starts with. Nothing downstream consults
    // it, so a source the user has edited, moved or replaced stays theirs, and
    // `sourceFor` still traces precisely what the configuration says -- the
    // scene never substitutes one source for another behind a run.
    struct SceneEmitter {
        bool       declared = false;
        SourceSpec spec;
    };
    static SceneEmitter emitterFor(GeometryProvider::Scene scene,
                                   const SceneParams& params);

    // The scene at its default parameters. These entries are pinned for the
    // lifetime of the process, so the returned reference stays valid.
    static const TraceScene& sceneFor(GeometryProvider::Scene scene,
                                      double* buildSecondsOut = nullptr);
    static const std::vector<OpticalSurface>& surfacesFor(GeometryProvider::Scene scene);

    static void clearCache();
};
