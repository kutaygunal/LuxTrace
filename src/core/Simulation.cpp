#include "Simulation.h"
#include "MeshBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <future>
#include <mutex>
#include <vector>

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
    // Guards the maps below and nothing else. It is emphatically *not* held
    // across a build: entryFor() used to call buildData() -- OCCT tessellation
    // plus a full hierarchy build, potentially seconds -- with this locked. A
    // parameter sweep serialised every geometry build behind it, and worse, the
    // UI's geometry worker blocked behind a running study, so the 3D viewport
    // froze on a background job it had nothing to do with. Responsiveness is
    // what the whole worker architecture exists to protect.
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

    // Builds that are running right now, keyed the way the cache is.
    //
    // A second request for geometry already being built waits on the first
    // one's future instead of building it a second time -- which is the
    // ordinary case when a slider is dragged back to a value the previous step
    // is still building.
    struct InFlight {
        CacheKey                                 key;
        std::shared_future<Simulation::SceneRef> future;
    };
    std::vector<InFlight> building;

    struct InFlightImport {
        std::shared_ptr<const GeometryProvider::SceneSetup> setup;
        int                                                 detBins = 0;
        std::shared_future<Simulation::SceneRef>            future;
    };
    std::vector<InFlightImport> buildingImports;
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

bool isDefaultKey(const CacheKey& key) {
    const auto scene = GeometryProvider::Scene(key.scene);
    return key.detBins <= 0 && key.params == GeometryProvider::defaultParams(scene);
}

// The cached entry for a key, or null. The caller must hold the cache mutex.
Simulation::SceneRef lookupLocked(SceneCache& c, const CacheKey& key) {
    if (isDefaultKey(key)) return c.pinned[std::size_t(key.scene)];

    auto it = std::find_if(c.variants.begin(), c.variants.end(),
                           [&](const auto& e) { return e.first == key; });
    if (it == c.variants.end()) return {};
    // Move to the front: a slider sweep revisits the same few sets.
    auto entry = std::move(*it);
    c.variants.erase(it);
    c.variants.push_front(std::move(entry));
    return c.variants.front().second;
}

// Files a finished build. The caller must hold the cache mutex.
void storeLocked(SceneCache& c, const CacheKey& key, const Simulation::SceneRef& ref) {
    if (!ref) return;
    if (isDefaultKey(key)) {
        // Pinned for the lifetime of the process, so sceneFor() can keep handing
        // out plain references.
        c.pinned[std::size_t(key.scene)] = ref;
        return;
    }
    c.variants.emplace_front(key, ref);
    while (c.variants.size() > kVariantCacheSize) c.variants.pop_back();
}

} // namespace

Simulation::SceneRef Simulation::dataFor(GeometryProvider::Scene scene,
                                         const SceneParams& params,
                                         double* buildSecondsOut, int detectorBins) {
    if (buildSecondsOut) *buildSecondsOut = 0.0;

    const SceneParams sane = GeometryProvider::sanitise(scene, params);
    const CacheKey    key{int(scene), sane, detectorBins};
    SceneCache&       c = cache();

    std::promise<SceneRef>   promise;
    std::shared_future<SceneRef> waitOn;
    bool building = false;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (SceneRef hit = lookupLocked(c, key)) return hit;

        auto it = std::find_if(c.building.begin(), c.building.end(),
                               [&](const SceneCache::InFlight& b) { return b.key == key; });
        if (it != c.building.end()) {
            waitOn = it->future;                 // somebody is already on it
        } else {
            waitOn = promise.get_future().share();
            c.building.push_back({key, waitOn});
            building = true;
        }
    }
    // Outside the lock, both ways: waiting for somebody else's build must not
    // block a third request for geometry that is already cached.
    if (!building) return waitOn.get();

    double   seconds = 0.0;
    SceneRef built;
    try {
        built = buildData(scene, sane, detectorBins, seconds);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(c.mutex);
            std::erase_if(c.building, [&](const SceneCache::InFlight& b) {
                return b.key == key;
            });
        }
        // Everyone waiting on this key gets the same failure rather than a
        // future nobody will ever fulfil.
        promise.set_exception(std::current_exception());
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(c.mutex);
        storeLocked(c, key, built);
        std::erase_if(c.building, [&](const SceneCache::InFlight& b) {
            return b.key == key;
        });
    }
    promise.set_value(built);
    if (buildSecondsOut) *buildSecondsOut = seconds;
    return built;
}

Simulation::SceneRef Simulation::dataFor(
    const std::shared_ptr<const GeometryProvider::SceneSetup>& setup, int detectorBins,
    double* buildSecondsOut) {
    if (buildSecondsOut) *buildSecondsOut = 0.0;
    if (!setup) return {};

    SceneCache& c = cache();

    std::promise<SceneRef>       promise;
    std::shared_future<SceneRef> waitOn;
    bool building = false;
    {
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

        auto inFlight = std::find_if(c.buildingImports.begin(), c.buildingImports.end(),
                                     [&](const SceneCache::InFlightImport& b) {
                                         return b.setup == setup && b.detBins == detectorBins;
                                     });
        if (inFlight != c.buildingImports.end()) {
            waitOn = inFlight->future;
        } else {
            waitOn = promise.get_future().share();
            c.buildingImports.push_back({setup, detectorBins, waitOn});
            building = true;
        }
    }
    if (!building) return waitOn.get();

    auto forget = [&] {
        std::lock_guard<std::mutex> lock(c.mutex);
        std::erase_if(c.buildingImports, [&](const SceneCache::InFlightImport& b) {
            return b.setup == setup && b.detBins == detectorBins;
        });
    };

    double   seconds = 0.0;
    SceneRef built;
    try {
        built = buildFromSetup(*setup, detectorBins, seconds);
    } catch (...) {
        forget();
        promise.set_exception(std::current_exception());
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.imported.push_front({setup, detectorBins, built});
        while (c.imported.size() > kImportedCacheSize) c.imported.pop_back();
    }
    forget();
    promise.set_value(built);
    if (buildSecondsOut) *buildSecondsOut = seconds;
    return built;
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
    src.rayFile            = cfg.rayFile;
    src.rayFileScale       = cfg.rayFileScale;
    src.rayFileWavelengths = cfg.rayFileWavelengths;
    src.label = cfg.rayFile && !cfg.rayFile->label.isEmpty()
                    ? cfg.rayFile->label
                    : QStringLiteral("Source 1");
    // Every option is traced exactly as configured -- the scene never silently
    // substitutes one source for another, so the efficiency reported always
    // matches the source selected.
    return src;
}

std::vector<SourceConfig> Simulation::sourcesFor(const SimConfig& cfg,
                                                 const SceneData& data) {
    return sourcesFor(cfg, data.sourceOrigin, data.sourceAxis);
}

std::vector<SourceConfig> Simulation::sourcesFor(const SimConfig& cfg,
                                                 const gp_Pnt& origin,
                                                 const gp_Dir& axis) {
    SceneData placement;
    placement.sourceOrigin = origin;
    placement.sourceAxis   = axis;

    std::vector<SourceConfig> out;
    out.reserve(std::size_t(cfg.sourceCount()));
    out.push_back(sourceFor(cfg, placement));
    if (cfg.extraSources.empty()) return out;

    const gp_Pnt base = origin;
    int n = 2;
    for (const SourceSpec& s : cfg.extraSources) {
        SourceConfig c;
        c.type         = s.type;
        c.shape        = s.shape;
        c.spectrum     = s.spectrum;
        c.halfAngleDeg = s.halfAngleDeg;
        c.sizeA        = s.sizeA;
        c.sizeB        = s.sizeB;
        c.beamRadius   = s.beamRadius;
        c.power        = s.power;
        // The run reports in one unit, and the first source fixes it.
        c.fluxUnit     = cfg.fluxUnit;
        c.polarisationState = s.polarisationState;
        c.origin = s.absolute
                       ? s.offset
                       : gp_Pnt(base.X() + s.offset.X(),
                                base.Y() + s.offset.Y(),
                                base.Z() + s.offset.Z());
        c.axis   = s.useSceneAxis ? axis : s.axis;
        c.rayFile            = s.rayFile;
        c.rayFileScale       = s.rayFileScale;
        c.rayFileWavelengths = s.rayFileWavelengths;
        c.label = !s.label.isEmpty()
                      ? s.label
                      : (s.rayFile && !s.rayFile->label.isEmpty()
                             ? s.rayFile->label
                             : QStringLiteral("Source %1").arg(n));
        ++n;
        out.push_back(c);
    }

    // Share the run budget in proportion to power. That is the variance-optimal
    // split -- a source carrying a tenth of the light gets a tenth of the rays,
    // and every source ends the run with a comparable error bar -- and it keeps
    // the cost of adding a second LED at zero.
    //
    // Every source keeps at least one ray, so a source configured at zero power
    // still appears in the report as the zero it is rather than disappearing.
    double totalPower = 0.0;
    for (const SourceConfig& c : out) totalPower += std::max(0.0, c.power);
    const int budget = std::max(int(out.size()), cfg.rays);

    int handed = 0;
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        const double share = totalPower > 0.0 ? std::max(0.0, out[i].power) / totalPower
                                              : 1.0 / double(out.size());
        const int r = std::max(1, int(std::llround(share * double(budget))));
        out[i].rays = std::min(r, budget - handed - int(out.size() - i - 1));
        handed += out[i].rays;
    }
    // The last source takes the remainder, so the budget is spent exactly and
    // the ray count a user asked for is the ray count the run traces.
    out.back().rays = std::max(1, budget - handed);
    return out;
}

SourceConfig Simulation::sourceFor(GeometryProvider::Scene scene,
                                   SourceConfig::Type type, int rays) {
    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = type;
    cfg.rays   = rays;
    return sourceFor(cfg, *dataFor(cfg));
}

Simulation::SceneEmitter Simulation::emitterFor(GeometryProvider::Scene scene,
                                                const SceneParams& raw) {
    SceneEmitter out;
    if (scene != GeometryProvider::Scene::ShowcaseLuminaire &&
        scene != GeometryProvider::Scene::TirLens)
        return out;

    const SceneParams P = GeometryProvider::sanitise(scene, raw);

    if (scene == GeometryProvider::Scene::TirLens) {
        // The surface source a collimator is designed around: a square emitting
        // area, Lambertian, sitting at the bottom of the well. A point source in
        // a TIR lens collimates perfectly and says nothing -- the question the
        // optic answers is what a die of finite size does to the beam, so the
        // scene declares the die rather than leaving a point in its place.
        out.declared          = true;
        out.spec.label        = QStringLiteral("Emitting surface");
        out.spec.type         = SourceConfig::Type::Lambertian;
        out.spec.shape        = SourceConfig::Shape::Rect;
        out.spec.halfAngleDeg = 90.0;
        out.spec.sizeA        = P.v[3];
        out.spec.sizeB        = P.v[3];
        out.spec.power        = 1.0;
        return out;
    }

    out.declared          = true;
    out.spec.label        = QStringLiteral("LED die");
    // Lambertian over a square, which is what a die is. The size matters twice:
    // it is the etendue that limits how tightly the cup can collimate, and it
    // is the emitting *area* that makes the Appearance preview draw a glowing
    // face rather than fall back to a point light.
    out.spec.type         = SourceConfig::Type::Lambertian;
    out.spec.shape        = SourceConfig::Shape::Rect;
    out.spec.halfAngleDeg = 90.0;
    out.spec.sizeA        = P.v[2];
    out.spec.sizeB        = P.v[2];
    // A phosphor-converted white LED at a neutral 4000 K. The render integrates
    // this through the colour matching functions, so the cup is lit the colour
    // the source actually is rather than a default white.
    SpectrumConfig spectrum(SpectrumConfig::Kind::LedPhosphor);
    spectrum.cct          = 4000.0;
    out.spec.spectrum     = spectrum;
    out.spec.power        = 1.0;
    return out;
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

    const std::vector<SourceConfig> srcs = sourcesFor(cfg, *data);

    TraceOptions opt;
    opt.threads = cfg.threads;
    opt.seed    = cfg.seed;
    opt.physics = cfg.physics;
    opt.nTheta  = cfg.nTheta;
    opt.nPhi    = cfg.nPhi;
    opt.surfaceOverrides = cfg.surfaceOverrides;
    opt.strayPaths       = cfg.strayPaths;
    opt.deterministicGrids = cfg.deterministicGrids;
    opt.noiseMap           = cfg.noiseMap;

    SimulationResult res;
    RayTracer::trace(data->scene, srcs, res, opt, ctl);
    res.buildSeconds = buildSeconds;
    return res;
}
