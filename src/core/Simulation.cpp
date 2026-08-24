#include "Simulation.h"
#include "MeshBuilder.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

namespace {

// How many non-default parameter sets to keep around. Dragging a spin box mints
// a new geometry per step, and each one costs a few MB of triangles and BVH
// nodes, so the tail has to be dropped -- but only after the last SceneRef to it
// is gone, which is what shared ownership buys.
constexpr std::size_t kVariantCacheSize = 12;

// Imported geometry, which is not keyed by anything the registry knows. One
// entry is the common case -- a session imports a file and traces it -- and a
// few more cover a user flipping between two imports.
constexpr std::size_t kImportedCacheSize = 4;

struct CacheKey {
    int         scene = 0;
    SceneParams params;
    // The receiver grid is baked into the mesh, so two resolutions of the same
    // scene are two different builds and must not share a cache slot.
    int         detBins = 0;
    bool operator==(const CacheKey& o) const {
        return scene == o.scene && params == o.params && detBins == o.detBins;
    }
};

struct SceneCache {
    std::mutex mutex;
    // Default-parameter geometry, one slot per scene. Pinned for the lifetime of
    // the process so `sceneFor` can keep handing out plain references.
    std::vector<Simulation::SceneRef> pinned{std::size_t(GeometryProvider::count())};
    // Everything else, most recently used at the front.
    std::deque<std::pair<CacheKey, Simulation::SceneRef>> variants;
    // Imported geometry, keyed on the identity of the setup it was built from.
    // The owning handle is kept alongside so a raw-pointer comparison cannot
    // match an address that a later allocation happened to reuse.
    struct ImportedEntry {
        std::shared_ptr<const GeometryProvider::SceneSetup> setup;
        int                                                 detBins = 0;
        Simulation::SceneRef                                data;
    };
    std::deque<ImportedEntry> imported;
};

SceneCache& cache() {
    static SceneCache c;
    return c;
}

// Tessellation and hierarchy for one setup, whether the registry built it or a
// file did. The two paths differ only in where the surfaces came from, so they
// share this one: an imported part is meshed, binned and hierarchy-built by
// exactly the code every built-in scene goes through.
Simulation::SceneRef buildFromSetup(const GeometryProvider::SceneSetup& setup, int detBins,
                                    double& seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    auto data = std::make_shared<Simulation::SceneData>();
    data->sourceOrigin = setup.sourceOrigin;
    data->sourceAxis   = setup.sourceAxis;
    data->surfaces     = setup.surfaces;
    if (detBins > 0)
        for (auto& s : data->surfaces)
            if (s.isDetector) { s.detNX = detBins; s.detNY = detBins; }
    data->scene.build(MeshBuilder::build(data->surfaces));
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return data;
}

Simulation::SceneRef buildData(GeometryProvider::Scene scene, const SceneParams& params,
                               int detBins, double& seconds) {
    return buildFromSetup(GeometryProvider::build(scene, params), detBins, seconds);
}

// Builds and caches one scene. The caller must hold the cache mutex.
Simulation::SceneRef entryFor(SceneCache& c, GeometryProvider::Scene scene,
                              const SceneParams& params, int detBins,
                              double* buildSecondsOut) {
    double seconds = 0.0;
    const std::size_t idx = std::size_t(int(scene));
    const bool isDefault  = (params == GeometryProvider::defaultParams(scene)) && detBins <= 0;

    Simulation::SceneRef ref;
    if (isDefault) {
        if (!c.pinned[idx]) c.pinned[idx] = buildData(scene, params, detBins, seconds);
        ref = c.pinned[idx];
    } else {
        const CacheKey key{int(scene), params, detBins};
        auto it = std::find_if(c.variants.begin(), c.variants.end(),
                               [&](const auto& e) { return e.first == key; });
        if (it != c.variants.end()) {
            // Move to the front: a slider sweep revisits the same few sets.
            auto entry = std::move(*it);
            c.variants.erase(it);
            c.variants.push_front(std::move(entry));
        } else {
            c.variants.emplace_front(key, buildData(scene, params, detBins, seconds));
            while (c.variants.size() > kVariantCacheSize) c.variants.pop_back();
        }
        ref = c.variants.front().second;
    }

    if (buildSecondsOut) *buildSecondsOut = seconds;
    return ref;
}

} // namespace

Simulation::SceneRef Simulation::dataFor(GeometryProvider::Scene scene,
                                         const SceneParams& params,
                                         double* buildSecondsOut, int detectorBins) {
    SceneCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return entryFor(c, scene, GeometryProvider::sanitise(scene, params), detectorBins,
                    buildSecondsOut);
}

Simulation::SceneRef Simulation::dataFor(
    const std::shared_ptr<const GeometryProvider::SceneSetup>& setup, int detectorBins,
    double* buildSecondsOut) {
    if (buildSecondsOut) *buildSecondsOut = 0.0;
    if (!setup) return {};

    SceneCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    auto it = std::find_if(c.imported.begin(), c.imported.end(),
                           [&](const SceneCache::ImportedEntry& e) {
                               return e.setup == setup && e.detBins == detectorBins;
                           });
    if (it != c.imported.end()) {
        auto entry = std::move(*it);
        c.imported.erase(it);
        c.imported.push_front(std::move(entry));
        return c.imported.front().data;
    }

    double seconds = 0.0;
    c.imported.push_front({setup, detectorBins, buildFromSetup(*setup, detectorBins, seconds)});
    while (c.imported.size() > kImportedCacheSize) c.imported.pop_back();
    if (buildSecondsOut) *buildSecondsOut = seconds;
    return c.imported.front().data;
}

Simulation::SceneRef Simulation::dataFor(const SimConfig& cfg, double* buildSecondsOut) {
    // Imported geometry is the whole scene, not a variant of one: the registry's
    // enum and parameters describe a different solid entirely and must not be
    // consulted, which is exactly the substitution that made a run after an
    // import trace the previous shape.
    if (cfg.imported) return dataFor(cfg.imported, cfg.detectorBins, buildSecondsOut);
    return dataFor(cfg.scene, cfg.effectiveParams(), buildSecondsOut, cfg.detectorBins);
}

QString SimConfig::sceneName() const {
    if (imported && !imported->label.isEmpty()) return imported->label;
    if (imported) return QStringLiteral("Imported geometry");
    return GeometryProvider::info(scene).name;
}

QString SimConfig::sceneDescription() const {
    if (imported) return imported->description;
    return GeometryProvider::info(scene).description;
}

SourceConfig Simulation::sourceFor(const SimConfig& cfg, const SceneData& data) {
    SourceConfig src;
    src.type         = cfg.source;
    src.shape        = cfg.shape;
    src.spectrum     = cfg.spectrum;
    src.rays         = cfg.rays;
    src.origin       = data.sourceOrigin;
    src.axis         = data.sourceAxis;
    src.halfAngleDeg = cfg.halfAngleDeg;
    src.sizeA        = cfg.sizeA;
    src.sizeB        = cfg.sizeB;
    src.beamRadius   = cfg.beamRadius;
    src.power        = cfg.power;
    src.fluxUnit     = cfg.fluxUnit;
    src.polarisationState = cfg.polarisationState;
    // Every option is traced exactly as configured -- the scene never silently
    // substitutes one source for another, so the efficiency reported always
    // matches the source selected.
    return src;
}

SourceConfig Simulation::sourceFor(GeometryProvider::Scene scene,
                                   SourceConfig::Type type, int rays) {
    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = type;
    cfg.rays   = rays;
    return sourceFor(cfg, *dataFor(cfg));
}

const TraceScene& Simulation::sceneFor(GeometryProvider::Scene scene,
                                       double* buildSecondsOut) {
    // Default-parameter entries are pinned, so this reference outlives the call.
    return dataFor(scene, GeometryProvider::defaultParams(scene), buildSecondsOut)->scene;
}

const std::vector<OpticalSurface>& Simulation::surfacesFor(GeometryProvider::Scene scene) {
    return dataFor(scene, GeometryProvider::defaultParams(scene))->surfaces;
}

void Simulation::clearCache() {
    SceneCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    for (auto& e : c.pinned) e.reset();
    c.variants.clear();
    c.imported.clear();
}

SimulationResult Simulation::run(const SimConfig& cfg, const TraceControl& ctl) {
    double buildSeconds = 0.0;
    // The reference is held for the whole trace, so the cache may evict the
    // entry underneath without the geometry disappearing.
    const SceneRef data = dataFor(cfg, &buildSeconds);

    const SourceConfig src = sourceFor(cfg, *data);

    TraceOptions opt;
    opt.threads = cfg.threads;
    opt.seed    = cfg.seed;
    opt.physics = cfg.physics;
    opt.nTheta  = cfg.nTheta;
    opt.nPhi    = cfg.nPhi;
    opt.surfaceOverrides = cfg.surfaceOverrides;

    SimulationResult res;
    RayTracer::trace(data->scene, src, res, opt, ctl);
    res.buildSeconds = buildSeconds;
    return res;
}
