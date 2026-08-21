#include "ConfigIO.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace configio {
namespace {

const char* kSourceType[]  = {"point", "lambertian", "collimated"};
const char* kSourceShape[] = {"point", "disc", "rect", "sphere"};
const char* kSpectrum[]    = {"monochrome", "rgb"};

template <std::size_t N>
int indexOf(const char* const (&names)[N], const QString& s, int fallback) {
    for (std::size_t i = 0; i < N; ++i)
        if (s.compare(QLatin1String(names[i]), Qt::CaseInsensitive) == 0) return int(i);
    return fallback;
}

int sceneIndexOf(const QString& name, int fallback) {
    for (int i = 0; i < GeometryProvider::count(); ++i)
        if (GeometryProvider::info(GeometryProvider::Scene(i)).name == name) return i;
    return fallback;
}

double num(const QJsonObject& o, const char* key, double fallback) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

bool flag(const QJsonObject& o, const char* key, bool fallback) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : fallback;
}

} // namespace

QString toJson(const SimConfig& cfg) {
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("occt-optics-studio/1");

    QJsonObject scene;
    scene[QStringLiteral("name")] = GeometryProvider::info(cfg.scene).name;
    scene[QStringLiteral("useDefaults")] = cfg.useSceneDefaults;
    {
        // Parameters are written with their names alongside, so a saved file
        // stays readable and a scene whose slots change can still be matched up
        // by hand.
        const SceneParams p = cfg.effectiveParams();
        const auto& infos = GeometryProvider::paramInfo(cfg.scene);
        QJsonArray values, names;
        for (std::size_t i = 0; i < infos.size() && i < SceneParams::kMax; ++i) {
            values.append(p.v[i]);
            names.append(infos[i].name);
        }
        scene[QStringLiteral("params")]     = values;
        scene[QStringLiteral("paramNames")] = names;
    }
    root[QStringLiteral("scene")] = scene;

    QJsonObject src;
    src[QStringLiteral("type")]         = QLatin1String(kSourceType[int(cfg.source)]);
    src[QStringLiteral("shape")]        = QLatin1String(kSourceShape[int(cfg.shape)]);
    src[QStringLiteral("spectrum")]     = QLatin1String(kSpectrum[int(cfg.spectrum)]);
    src[QStringLiteral("halfAngleDeg")] = cfg.halfAngleDeg;
    src[QStringLiteral("sizeA")]        = cfg.sizeA;
    src[QStringLiteral("sizeB")]        = cfg.sizeB;
    src[QStringLiteral("beamRadius")]   = cfg.beamRadius;
    src[QStringLiteral("wavelengthNm")] = cfg.wavelengthNm;
    root[QStringLiteral("source")] = src;

    QJsonObject phys;
    phys[QStringLiteral("fresnel")]           = cfg.physics.fresnel;
    phys[QStringLiteral("absorption")]        = cfg.physics.absorption;
    phys[QStringLiteral("scattering")]        = cfg.physics.scattering;
    phys[QStringLiteral("roughness")]         = cfg.physics.roughness;
    phys[QStringLiteral("dispersion")]        = cfg.physics.dispersion;
    phys[QStringLiteral("roughnessOverride")] = cfg.physics.roughnessOverride;
    phys[QStringLiteral("scatterOverride")]   = cfg.physics.scatterOverride;
    phys[QStringLiteral("absorptionScale")]   = cfg.physics.absorptionScale;
    root[QStringLiteral("physics")] = phys;

    QJsonObject run;
    run[QStringLiteral("rays")]    = cfg.rays;
    run[QStringLiteral("threads")] = int(cfg.threads);
    run[QStringLiteral("seed")]    = double(cfg.seed);
    run[QStringLiteral("nTheta")]  = cfg.nTheta;
    run[QStringLiteral("nPhi")]    = cfg.nPhi;
    root[QStringLiteral("run")] = run;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool fromJson(const QString& json, SimConfig& out, QString* errorOut) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (doc.isNull() || !doc.isObject()) {
        if (errorOut) *errorOut = err.errorString();
        return false;
    }
    const QJsonObject root = doc.object();

    SimConfig cfg;   // every field starts at its default and is only overwritten

    const QJsonObject scene = root.value(QStringLiteral("scene")).toObject();
    cfg.scene = GeometryProvider::Scene(
        sceneIndexOf(scene.value(QStringLiteral("name")).toString(), int(cfg.scene)));
    cfg.useSceneDefaults = flag(scene, "useDefaults", true);
    {
        const QJsonArray values = scene.value(QStringLiteral("params")).toArray();
        SceneParams p = GeometryProvider::defaultParams(cfg.scene);
        for (int i = 0; i < values.size() && i < SceneParams::kMax; ++i)
            if (values[i].isDouble()) p.v[i] = values[i].toDouble();
        cfg.params = GeometryProvider::sanitise(cfg.scene, p);
    }

    const QJsonObject src = root.value(QStringLiteral("source")).toObject();
    cfg.source = SourceConfig::Type(
        indexOf(kSourceType, src.value(QStringLiteral("type")).toString(), int(cfg.source)));
    cfg.shape = SourceConfig::Shape(
        indexOf(kSourceShape, src.value(QStringLiteral("shape")).toString(), int(cfg.shape)));
    cfg.spectrum = SourceConfig::Spectrum(
        indexOf(kSpectrum, src.value(QStringLiteral("spectrum")).toString(), int(cfg.spectrum)));
    cfg.halfAngleDeg = std::clamp(num(src, "halfAngleDeg", cfg.halfAngleDeg), 0.0, 180.0);
    cfg.sizeA        = std::max(0.0, num(src, "sizeA", cfg.sizeA));
    cfg.sizeB        = std::max(0.0, num(src, "sizeB", cfg.sizeB));
    cfg.beamRadius   = std::max(0.0, num(src, "beamRadius", cfg.beamRadius));
    cfg.wavelengthNm = std::clamp(num(src, "wavelengthNm", cfg.wavelengthNm), 200.0, 2000.0);

    const QJsonObject phys = root.value(QStringLiteral("physics")).toObject();
    cfg.physics.fresnel    = flag(phys, "fresnel",    cfg.physics.fresnel);
    cfg.physics.absorption = flag(phys, "absorption", cfg.physics.absorption);
    cfg.physics.scattering = flag(phys, "scattering", cfg.physics.scattering);
    cfg.physics.roughness  = flag(phys, "roughness",  cfg.physics.roughness);
    cfg.physics.dispersion = flag(phys, "dispersion", cfg.physics.dispersion);
    cfg.physics.roughnessOverride =
        std::min(num(phys, "roughnessOverride", cfg.physics.roughnessOverride), 1.0);
    cfg.physics.scatterOverride =
        std::min(num(phys, "scatterOverride", cfg.physics.scatterOverride), 1.0);
    cfg.physics.absorptionScale =
        std::clamp(num(phys, "absorptionScale", cfg.physics.absorptionScale), 0.0, 1000.0);

    const QJsonObject run = root.value(QStringLiteral("run")).toObject();
    cfg.rays    = std::clamp(int(num(run, "rays", cfg.rays)), 1, 100000000);
    cfg.threads = unsigned(std::clamp(int(num(run, "threads", int(cfg.threads))), 0, 1024));
    cfg.seed    = std::uint64_t(std::max(0.0, num(run, "seed", double(cfg.seed))));
    cfg.nTheta  = std::clamp(int(num(run, "nTheta", cfg.nTheta)), 0, 720);
    cfg.nPhi    = std::clamp(int(num(run, "nPhi", cfg.nPhi)), 1, 720);

    out = cfg;
    return true;
}

bool save(const QString& path, const SimConfig& cfg, QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    const QByteArray bytes = toJson(cfg).toUtf8();
    if (f.write(bytes) != bytes.size()) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return true;
}

bool load(const QString& path, SimConfig& out, QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return fromJson(QString::fromUtf8(f.readAll()), out, errorOut);
}

} // namespace configio
