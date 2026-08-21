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

struct CacheKey {
    int         scene = 0;
    SceneParams params;
    bool operator==(const CacheKey& o) const { return scene == o.scene && params == o.params; }
};

struct SceneCache {
    std::mutex mutex;
    // Default-parameter geometry, one slot per scene. Pinned for the lifetime of
    // the process so `sceneFor` can keep handing out plain references.
    std::vector<Simulation::SceneRef> pinned{std::size_t(GeometryProvider::count())};
    // Everything else, most recently used at the front.
    std::deque<std::pair<CacheKey, Simulation::SceneRef>> variants;
};

SceneCache& cache() {
    static SceneCache c;
    return c;
}

Simulation::SceneRef buildData(GeometryProvider::Scene scene, const SceneParams& params,
                               double& seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    auto data = std::make_shared<Simulation::SceneData>();
    GeometryProvider::SceneSetup setup = GeometryProvider::build(scene, params);
    data->sourceOrigin = setup.sourceOrigin;
    data->sourceAxis   = setup.sourceAxis;
    data->surfaces     = std::move(setup.surfaces);
    data->scene.build(MeshBuilder::build(data->surfaces));
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return data;
}

// Builds and caches one scene. The caller must hold the cache mutex.
Simulation::SceneRef entryFor(SceneCache& c, GeometryProvider::Scene scene,
                              const SceneParams& params, double* buildSecondsOut) {
    double seconds = 0.0;
    const std::size_t idx = std::size_t(int(scene));
    const bool isDefault  = (params == GeometryProvider::defaultParams(scene));

    Simulation::SceneRef ref;
    if (isDefault) {
        if (!c.pinned[idx]) c.pinned[idx] = buildData(scene, params, seconds);
        ref = c.pinned[idx];
    } else {
        const CacheKey key{int(scene), params};
        auto it = std::find_if(c.variants.begin(), c.variants.end(),
                               [&](const auto& e) { return e.first == key; });
        if (it != c.variants.end()) {
            // Move to the front: a slider sweep revisits the same few sets.
            auto entry = std::move(*it);
            c.variants.erase(it);
            c.variants.push_front(std::move(entry));
        } else {
            c.variants.emplace_front(key, buildData(scene, params, seconds));
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
                                         double* buildSecondsOut) {
    SceneCache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return entryFor(c, scene, GeometryProvider::sanitise(scene, params), buildSecondsOut);
}

Simulation::SceneRef Simulation::dataFor(const SimConfig& cfg, double* buildSecondsOut) {
    return dataFor(cfg.scene, cfg.effectiveParams(), buildSecondsOut);
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
    src.wavelengthNm = cfg.wavelengthNm;
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

    SimulationResult res;
    RayTracer::trace(data->scene, src, res, opt, ctl);
    res.buildSeconds = buildSeconds;
    return res;
}
