// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "ConfigIO.h"
#include "Material.h"
#include "RayFile.h"
#include "SceneDocument.h"

#include <algorithm>
#include <cmath>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <gp_Vec.hxx>

namespace configio {
namespace {

const char* kSourceType[]  = {"point", "lambertian", "collimated"};
const char* kSourceShape[] = {"point", "disc", "rect", "sphere"};
const char* kSpectrum[]    = {"monochrome", "rgb", "blackbody", "led", "d65", "table"};
const char* kFluxUnit[]    = {"watt", "lumen"};
const char* kPolState[]    = {"unpolarised", "linear-s", "linear-p", "circular"};

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

const char* kBsdfModel[]   = {"specular", "microfacet", "abg", "lambertian", "table"};
const char* kCoatModel[]   = {"none", "ideal", "table", "stack"};
const char* kRejectMode[]  = {"absorb", "pass"};
const char* kPhase[]       = {"henyey-greenstein", "gegenbauer"};

// ---- surface overrides -----------------------------------------------------
//
// These are keyed by surface *label*, not by index.
//
// The index was what a config used to carry, and reopening one after the scene
// registry changed -- or against a different CAD import -- silently landed
// every optical edit on whatever surface now occupied the slot. That is the
// failure mode that produces a plausible-looking wrong answer, which is the
// only kind that actually costs someone money. The scene enum was stored by
// name for exactly this reason; the overrides were not.
//
// The index is still written, as a hint for an override whose surface carries
// no label at all, and the identity hash is written so a stale hint can be
// vetoed rather than trusted.

QJsonObject bsdfToJson(const bsdf::Surface& b) {
    QJsonObject o;
    o[QStringLiteral("model")]    = QLatin1String(kBsdfModel[std::clamp(int(b.model), 0, 4)]);
    o[QStringLiteral("alpha")]    = b.alpha;
    o[QStringLiteral("fraction")] = b.fraction;
    o[QStringLiteral("abgA")]     = b.abgA;
    o[QStringLiteral("abgB")]     = b.abgB;
    o[QStringLiteral("abgG")]     = b.abgG;
    return o;
}

bsdf::Surface bsdfFromJson(const QJsonObject& o, bsdf::Surface b) {
    b.model    = bsdf::Model(indexOf(kBsdfModel, o.value(QStringLiteral("model")).toString(),
                                     int(b.model)));
    b.alpha    = std::max(0.0, num(o, "alpha", b.alpha));
    b.fraction = std::clamp(num(o, "fraction", b.fraction), 0.0, 1.0);
    b.abgA     = std::max(0.0, num(o, "abgA", b.abgA));
    b.abgB     = std::max(1e-9, num(o, "abgB", b.abgB));
    b.abgG     = std::max(0.0, num(o, "abgG", b.abgG));
    return b;
}

QJsonObject coatingToJson(const coating::Coating& c) {
    QJsonObject o;
    o[QStringLiteral("model")]         = QLatin1String(kCoatModel[std::clamp(int(c.model), 0, 3)]);
    o[QStringLiteral("residual")]      = c.residual;
    o[QStringLiteral("highReflector")] = c.highReflector;
    if (c.samples > 0) {
        QJsonArray lam, refl;
        for (int i = 0; i < c.samples && i < coating::Coating::kMaxSamples; ++i) {
            lam.append(c.lambdaNm[i]);
            refl.append(c.reflectance[i]);
        }
        o[QStringLiteral("tableNm")] = lam;
        o[QStringLiteral("tableR")]  = refl;
    }
    if (c.layers > 0) {
        QJsonArray n, t;
        for (int i = 0; i < c.layers && i < coating::Coating::kMaxLayers; ++i) {
            n.append(c.layerIndex[i]);
            t.append(c.layerThicknessNm[i]);
        }
        o[QStringLiteral("layerIndex")]       = n;
        o[QStringLiteral("layerThicknessNm")] = t;
    }
    return o;
}

coating::Coating coatingFromJson(const QJsonObject& o, coating::Coating c) {
    c.model = coating::Model(indexOf(kCoatModel, o.value(QStringLiteral("model")).toString(),
                                     int(c.model)));
    c.residual      = std::clamp(num(o, "residual", c.residual), 0.0, 1.0);
    c.highReflector = flag(o, "highReflector", c.highReflector);

    const QJsonArray lam  = o.value(QStringLiteral("tableNm")).toArray();
    const QJsonArray refl = o.value(QStringLiteral("tableR")).toArray();
    if (!lam.isEmpty()) {
        c.samples = 0;
        for (int i = 0; i < lam.size() && i < refl.size() &&
                        c.samples < coating::Coating::kMaxSamples; ++i) {
            c.lambdaNm[c.samples]    = lam[i].toDouble();
            c.reflectance[c.samples] = std::clamp(refl[i].toDouble(), 0.0, 1.0);
            ++c.samples;
        }
    }
    const QJsonArray ln = o.value(QStringLiteral("layerIndex")).toArray();
    const QJsonArray lt = o.value(QStringLiteral("layerThicknessNm")).toArray();
    if (!ln.isEmpty()) {
        c.layers = 0;
        for (int i = 0; i < ln.size() && i < lt.size() &&
                        c.layers < coating::Coating::kMaxLayers; ++i) {
            c.layerIndex[c.layers]       = std::max(1.0, ln[i].toDouble());
            c.layerThicknessNm[c.layers] = std::max(0.0, lt[i].toDouble());
            ++c.layers;
        }
    }
    return c;
}

// One surface's optical behaviour, flat. Written by a surface override and by
// every object in a scene document, because they are describing the same
// struct: a second encoding of SurfaceOptics would be a second thing to keep
// in step with the engine.
QJsonObject opticsJson(const SurfaceOptics& s) {
    QJsonObject o;
    o[QStringLiteral("reflectivity")]   = s.reflectivity;
    o[QStringLiteral("transmissivity")] = s.transmissivity;
    o[QStringLiteral("index")]          = s.index;
    o[QStringLiteral("dispersionB")]    = s.dispersionB;
    o[QStringLiteral("absorption")]     = s.absorption;
    o[QStringLiteral("scatter")]        = s.scatter;
    // Written only when set, so a config for a scene nobody has painted looks
    // exactly as it always did.
    if (s.hasAppearanceColour()) {
        QJsonArray rgb;
        for (double c : s.appearanceRgb) rgb.append(c);
        o[QStringLiteral("appearanceRgb")] = rgb;
    }
    o[QStringLiteral("roughness")]      = s.roughness;
    o[QStringLiteral("fresnel")]        = s.fresnel;
    o[QStringLiteral("mediumPriority")] = s.mediumPriority;

    // The catalogue name, not the resolved coefficients: a material is a name a
    // user typed, and rewriting it as six Sellmeier terms would make a saved
    // config unreadable and unpatchable by hand.
    if (s.material.valid()) {
        for (int i = 0; i < materials::count(); ++i) {
            const OpticalMaterial m = materials::at(i);
            if (m.model == s.material.model && std::fabs(m.nd - s.material.nd) < 1e-9) {
                o[QStringLiteral("material")] = materials::name(i);
                break;
            }
        }
    }

    o[QStringLiteral("bsdf")]    = bsdfToJson(s.bsdf);
    o[QStringLiteral("coating")] = coatingToJson(s.coating);

    QJsonObject vol;
    vol[QStringLiteral("coefficient")] = s.volume.coefficient;
    vol[QStringLiteral("anisotropy")]  = s.volume.anisotropy;
    vol[QStringLiteral("phase")] = QLatin1String(kPhase[std::clamp(int(s.volume.phase), 0, 1)]);
    vol[QStringLiteral("alpha")] = s.volume.alpha;
    o[QStringLiteral("volume")] = vol;

    if (s.isDetector) {
        QJsonObject det;
        det[QStringLiteral("nx")]            = s.detNX;
        det[QStringLiteral("ny")]            = s.detNY;
        det[QStringLiteral("acceptanceDeg")] = s.detAcceptanceDeg;
        det[QStringLiteral("reject")] =
            QLatin1String(kRejectMode[std::clamp(int(s.detRejectMode), 0, 1)]);
        o[QStringLiteral("detector")] = det;
    }
    return o;
}

QJsonObject overrideToJson(const SurfaceOverride& ov) {
    QJsonObject o = opticsJson(ov.optics);
    o[QStringLiteral("label")] = ov.label;
    // A 64-bit hash does not survive a JSON double, so it is written as hex.
    o[QStringLiteral("identity")] =
        QStringLiteral("%1").arg(ov.identity, 16, 16, QLatin1Char('0'));
    o[QStringLiteral("surfaceHint")] = ov.surface;
    return o;
}

// The inverse. Anything missing keeps whatever `s` already carries, so a type's
// own defaults survive a partial object rather than being zeroed by it.
SurfaceOptics opticsFrom(const QJsonObject& o, SurfaceOptics s) {
    s.reflectivity   = std::clamp(num(o, "reflectivity",   s.reflectivity),   0.0, 1.0);
    s.transmissivity = std::clamp(num(o, "transmissivity", s.transmissivity), 0.0, 1.0);
    s.index          = std::clamp(num(o, "index",          s.index),          0.0, 10.0);
    s.dispersionB    = num(o, "dispersionB", s.dispersionB);
    s.absorption     = std::max(0.0, num(o, "absorption", s.absorption));
    s.scatter        = std::clamp(num(o, "scatter",   s.scatter),   0.0, 1.0);
    if (const QJsonArray rgb = o.value(QStringLiteral("appearanceRgb")).toArray();
        rgb.size() == 3)
        s.setAppearanceColour(std::clamp(rgb[0].toDouble(-1.0), 0.0, 1.0),
                              std::clamp(rgb[1].toDouble(-1.0), 0.0, 1.0),
                              std::clamp(rgb[2].toDouble(-1.0), 0.0, 1.0));
    s.roughness      = std::clamp(num(o, "roughness", s.roughness), 0.0, 1.0);
    s.fresnel        = flag(o, "fresnel", s.fresnel);
    s.mediumPriority = int(num(o, "mediumPriority", 0.0));

    const QString mat = o.value(QStringLiteral("material")).toString();
    // An unknown name degrades to "no material" rather than failing the load,
    // which is what makes a config written against a catalogue file that is not
    // loaded here still open.
    if (!mat.isEmpty()) s.material = materials::byName(mat);

    s.bsdf    = bsdfFromJson(o.value(QStringLiteral("bsdf")).toObject(), s.bsdf);
    s.coating = coatingFromJson(o.value(QStringLiteral("coating")).toObject(), s.coating);

    const QJsonObject vol = o.value(QStringLiteral("volume")).toObject();
    s.volume.coefficient = std::max(0.0, num(vol, "coefficient", s.volume.coefficient));
    s.volume.anisotropy  = std::clamp(num(vol, "anisotropy", s.volume.anisotropy),
                                      -0.999, 0.999);
    // A file written before there was a choice carries no "phase" and loads as
    // Henyey-Greenstein, which is what it was traced with.
    s.volume.phase = bsdf::Phase(indexOf(kPhase, vol.value(QStringLiteral("phase")).toString(),
                                         int(s.volume.phase)));
    s.volume.alpha = std::clamp(num(vol, "alpha", s.volume.alpha), 0.01, 10.0);

    if (o.contains(QStringLiteral("detector"))) {
        const QJsonObject det = o.value(QStringLiteral("detector")).toObject();
        s.isDetector       = true;
        s.detNX            = std::clamp(int(num(det, "nx", 0.0)), 0, 4096);
        s.detNY            = std::clamp(int(num(det, "ny", 0.0)), 0, 4096);
        s.detAcceptanceDeg = std::clamp(num(det, "acceptanceDeg", 180.0), 0.0, 180.0);
        s.detRejectMode    = SurfaceOptics::RejectMode(
            indexOf(kRejectMode, det.value(QStringLiteral("reject")).toString(), 0));
    }
    return s;
}

SurfaceOverride overrideFromJson(const QJsonObject& o) {
    SurfaceOverride ov;
    ov.label   = o.value(QStringLiteral("label")).toString();
    ov.surface = int(num(o, "surfaceHint", -1.0));
    bool okHex = false;
    const std::uint64_t id =
        o.value(QStringLiteral("identity")).toString().toULongLong(&okHex, 16);
    ov.identity = okHex ? id : 0;
    ov.optics   = opticsFrom(o, ov.optics);
    return ov;
}

// ---- sources ---------------------------------------------------------------

QJsonObject rayFileToJson(const std::shared_ptr<const RayFileData>& rf,
                          double scale, bool wavelengths) {
    QJsonObject o;
    // The path, not the rays. A ray set is tens of megabytes and it is somebody
    // else's file; a config that inlined it would be unopenable and would go
    // stale the moment the vendor published a new measurement.
    o[QStringLiteral("path")]        = rf->path;
    o[QStringLiteral("scale")]       = scale;
    o[QStringLiteral("wavelengths")] = wavelengths;
    // Written for the reader of the file, and checked on load so a set that has
    // been replaced underneath the config is reported rather than assumed.
    o[QStringLiteral("rays")]        = double(rf->declared);
    return o;
}

// Reloads the set a config names. A file that has moved or changed leaves the
// source analytic and the failure named, rather than silently tracing a
// different emitter.
std::shared_ptr<const RayFileData> rayFileFromJson(const QJsonObject& o,
                                                   double& scale, bool& wavelengths,
                                                   QStringList* warnings) {
    const QString path = o.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) return {};
    scale       = std::max(1e-9, num(o, "scale", 1.0));
    wavelengths = flag(o, "wavelengths", true);

    const auto res = rayfile::load(path);
    if (!res.ok) {
        if (warnings)
            *warnings << QStringLiteral("ray file %1 could not be reopened: %2")
                             .arg(path, res.error);
        return {};
    }
    const std::size_t want = std::size_t(num(o, "rays", 0.0));
    if (want > 0 && res.data->declared != want && warnings)
        *warnings << QStringLiteral("ray file %1 now holds %2 rays; the config was "
                                    "saved against %3")
                         .arg(path).arg(res.data->declared).arg(want);
    return res.data;
}

QJsonObject sourceSpecToJson(const SourceSpec& s) {
    QJsonObject o;
    o[QStringLiteral("label")]        = s.label;
    o[QStringLiteral("type")]         = QLatin1String(kSourceType[std::clamp(int(s.type), 0, 2)]);
    o[QStringLiteral("shape")]        = QLatin1String(kSourceShape[std::clamp(int(s.shape), 0, 3)]);
    o[QStringLiteral("spectrum")]     = QLatin1String(kSpectrum[std::clamp(int(s.spectrum.kind), 0, 5)]);
    o[QStringLiteral("wavelengthNm")] = s.spectrum.wavelengthNm;
    o[QStringLiteral("cct")]          = s.spectrum.cct;
    o[QStringLiteral("halfAngleDeg")] = s.halfAngleDeg;
    o[QStringLiteral("sizeA")]        = s.sizeA;
    o[QStringLiteral("sizeB")]        = s.sizeB;
    o[QStringLiteral("beamRadius")]   = s.beamRadius;
    o[QStringLiteral("power")]        = s.power;
    o[QStringLiteral("polarisation")] =
        QLatin1String(kPolState[std::clamp(s.polarisationState, 0, 3)]);
    o[QStringLiteral("absolute")]     = s.absolute;
    o[QStringLiteral("useSceneAxis")] = s.useSceneAxis;

    QJsonArray off;
    off.append(s.offset.X()); off.append(s.offset.Y()); off.append(s.offset.Z());
    o[QStringLiteral("offset")] = off;
    QJsonArray ax;
    ax.append(s.axis.X()); ax.append(s.axis.Y()); ax.append(s.axis.Z());
    o[QStringLiteral("axis")] = ax;

    if (s.tracesRayFile())
        o[QStringLiteral("rayFile")] =
            rayFileToJson(s.rayFile, s.rayFileScale, s.rayFileWavelengths);
    return o;
}

SourceSpec sourceSpecFromJson(const QJsonObject& o, QStringList* warnings) {
    SourceSpec s;
    s.label = o.value(QStringLiteral("label")).toString();
    s.type  = SourceConfig::Type(
        indexOf(kSourceType, o.value(QStringLiteral("type")).toString(), int(s.type)));
    s.shape = SourceConfig::Shape(
        indexOf(kSourceShape, o.value(QStringLiteral("shape")).toString(), int(s.shape)));
    s.spectrum.kind = SpectrumConfig::Kind(
        indexOf(kSpectrum, o.value(QStringLiteral("spectrum")).toString(),
                int(s.spectrum.kind)));
    s.spectrum.wavelengthNm =
        std::clamp(num(o, "wavelengthNm", s.spectrum.wavelengthNm), 200.0, 2000.0);
    s.spectrum.cct  = std::clamp(num(o, "cct", s.spectrum.cct), 1000.0, 20000.0);
    s.halfAngleDeg  = std::clamp(num(o, "halfAngleDeg", s.halfAngleDeg), 0.0, 180.0);
    s.sizeA         = std::max(0.0, num(o, "sizeA", s.sizeA));
    s.sizeB         = std::max(0.0, num(o, "sizeB", s.sizeB));
    s.beamRadius    = std::max(0.0, num(o, "beamRadius", s.beamRadius));
    s.power         = std::max(0.0, num(o, "power", s.power));
    s.polarisationState =
        indexOf(kPolState, o.value(QStringLiteral("polarisation")).toString(), 0);
    s.absolute      = flag(o, "absolute", false);
    s.useSceneAxis  = flag(o, "useSceneAxis", true);

    const QJsonArray off = o.value(QStringLiteral("offset")).toArray();
    if (off.size() >= 3)
        s.offset = gp_Pnt(off[0].toDouble(), off[1].toDouble(), off[2].toDouble());
    const QJsonArray ax = o.value(QStringLiteral("axis")).toArray();
    if (ax.size() >= 3) {
        const gp_Vec v(ax[0].toDouble(), ax[1].toDouble(), ax[2].toDouble());
        if (v.Magnitude() > 1e-12) s.axis = gp_Dir(v);
    }

    if (o.contains(QStringLiteral("rayFile")))
        s.rayFile = rayFileFromJson(o.value(QStringLiteral("rayFile")).toObject(),
                                    s.rayFileScale, s.rayFileWavelengths, warnings);
    return s;
}

} // namespace

// Exposed so a scene document writes its objects with the same encoding a
// surface override and a source list already use.
QJsonObject   opticsToJson(const SurfaceOptics& optics)  { return opticsJson(optics); }
SurfaceOptics opticsFromJson(const QJsonObject& o, SurfaceOptics fallback) {
    return opticsFrom(o, fallback);
}
QJsonObject sourceToJson(const SourceSpec& spec) { return sourceSpecToJson(spec); }
SourceSpec  sourceFromJson(const QJsonObject& o, QStringList* warnings) {
    return sourceSpecFromJson(o, warnings);
}

QString toJson(const SimConfig& cfg, const scenedoc::SceneDocument* doc) {
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
    src[QStringLiteral("spectrum")]     = QLatin1String(kSpectrum[int(cfg.spectrum.kind)]);
    src[QStringLiteral("halfAngleDeg")] = cfg.halfAngleDeg;
    src[QStringLiteral("sizeA")]        = cfg.sizeA;
    src[QStringLiteral("sizeB")]        = cfg.sizeB;
    src[QStringLiteral("beamRadius")]   = cfg.beamRadius;
    src[QStringLiteral("wavelengthNm")] = cfg.spectrum.wavelengthNm;
    src[QStringLiteral("cct")]          = cfg.spectrum.cct;
    src[QStringLiteral("power")]        = cfg.power;
    src[QStringLiteral("powerUnit")]    = QLatin1String(kFluxUnit[int(cfg.fluxUnit)]);
    src[QStringLiteral("polarisation")] = QLatin1String(kPolState[
        std::clamp(cfg.polarisationState, 0, 3)]);
    if (cfg.rayFile && cfg.rayFile->valid())
        src[QStringLiteral("rayFile")] =
            rayFileToJson(cfg.rayFile, cfg.rayFileScale, cfg.rayFileWavelengths);
    root[QStringLiteral("source")] = src;

    // Everything beyond the primary source. Absent for the ordinary
    // single-source configuration, so a file written by one build and read by
    // another does not gain an empty array it has to ignore.
    if (!cfg.extraSources.empty()) {
        QJsonArray extras;
        for (const SourceSpec& s : cfg.extraSources) extras.append(sourceSpecToJson(s));
        root[QStringLiteral("extraSources")] = extras;
    }

    // Per-surface optical edits, keyed by label. These were not written at all,
    // so every edit a user made to a surface was lost the moment the
    // configuration was saved -- and the index they would have been keyed by is
    // exactly what makes a reopened file land an edit on the wrong surface.
    if (!cfg.surfaceOverrides.empty()) {
        QJsonArray ovs;
        for (const SurfaceOverride& ov : cfg.surfaceOverrides)
            ovs.append(overrideToJson(ov));
        root[QStringLiteral("surfaceOverrides")] = ovs;
    }

    QJsonObject phys;
    phys[QStringLiteral("fresnel")]           = cfg.physics.fresnel;
    phys[QStringLiteral("absorption")]        = cfg.physics.absorption;
    phys[QStringLiteral("scattering")]        = cfg.physics.scattering;
    phys[QStringLiteral("roughness")]         = cfg.physics.roughness;
    phys[QStringLiteral("dispersion")]        = cfg.physics.dispersion;
    phys[QStringLiteral("coatings")]         = cfg.physics.coatings;
    phys[QStringLiteral("volumeScattering")] = cfg.physics.volumeScattering;
    phys[QStringLiteral("polarised")]        = cfg.physics.polarised;
    phys[QStringLiteral("roughnessOverride")] = cfg.physics.roughnessOverride;
    phys[QStringLiteral("scatterOverride")]   = cfg.physics.scatterOverride;
    phys[QStringLiteral("absorptionScale")]   = cfg.physics.absorptionScale;
    root[QStringLiteral("physics")] = phys;

    // Stray-light path analysis. Written only when it is on, so a configuration
    // that never asked for it is byte-for-byte what it was.
    if (cfg.strayPaths.enabled) {
        QJsonObject sp;
        sp[QStringLiteral("enabled")]  = true;
        sp[QStringLiteral("maxPaths")] = double(cfg.strayPaths.maxPaths);
        if (!cfg.strayPaths.surfaceSet.empty()) {
            QJsonArray sets;
            for (int v : cfg.strayPaths.surfaceSet) sets.append(v);
            sp[QStringLiteral("surfaceSet")] = sets;
        }
        if (!cfg.strayPaths.setNames.empty()) {
            QJsonArray names;
            for (const QString& n : cfg.strayPaths.setNames) names.append(n);
            sp[QStringLiteral("setNames")] = names;
        }
        root[QStringLiteral("strayPaths")] = sp;
    }

    QJsonObject run;
    run[QStringLiteral("rays")]    = cfg.rays;
    run[QStringLiteral("threads")] = int(cfg.threads);
    run[QStringLiteral("seed")]    = double(cfg.seed);
    run[QStringLiteral("nTheta")]  = cfg.nTheta;
    run[QStringLiteral("nPhi")]    = cfg.nPhi;
    run[QStringLiteral("detectorBins")] = cfg.detectorBins;
    run[QStringLiteral("deterministicGrids")] = cfg.deterministicGrids;
    root[QStringLiteral("run")] = run;

    // An empty document writes nothing at all, so a configuration saved from a
    // scene nobody has composed is byte-for-byte what it was before documents
    // existed.
    if (doc && !doc->empty()) root[QStringLiteral("document")] = doc->toJson();

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString toJson(const SimConfig& cfg) { return toJson(cfg, nullptr); }

bool fromJson(const QString& json, SimConfig& out, scenedoc::SceneDocument* doc,
              QString* errorOut, QStringList* warnings) {
    QJsonParseError err{};
    const QJsonDocument parsed = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (parsed.isNull() || !parsed.isObject()) {
        if (errorOut) *errorOut = err.errorString();
        return false;
    }
    const QJsonObject root = parsed.object();

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
    cfg.spectrum.kind = SpectrumConfig::Kind(
        indexOf(kSpectrum, src.value(QStringLiteral("spectrum")).toString(),
                int(cfg.spectrum.kind)));
    cfg.halfAngleDeg = std::clamp(num(src, "halfAngleDeg", cfg.halfAngleDeg), 0.0, 180.0);
    cfg.sizeA        = std::max(0.0, num(src, "sizeA", cfg.sizeA));
    cfg.sizeB        = std::max(0.0, num(src, "sizeB", cfg.sizeB));
    cfg.beamRadius   = std::max(0.0, num(src, "beamRadius", cfg.beamRadius));
    cfg.spectrum.wavelengthNm =
        std::clamp(num(src, "wavelengthNm", cfg.spectrum.wavelengthNm), 200.0, 2000.0);
    cfg.spectrum.cct = std::clamp(num(src, "cct", cfg.spectrum.cct), 1000.0, 20000.0);
    cfg.power        = std::max(0.0, num(src, "power", cfg.power));
    cfg.fluxUnit     = FluxUnit(
        indexOf(kFluxUnit, src.value(QStringLiteral("powerUnit")).toString(),
                int(cfg.fluxUnit)));
    cfg.polarisationState = indexOf(kPolState,
                                    src.value(QStringLiteral("polarisation")).toString(),
                                    cfg.polarisationState);
    if (src.contains(QStringLiteral("rayFile")))
        cfg.rayFile = rayFileFromJson(src.value(QStringLiteral("rayFile")).toObject(),
                                      cfg.rayFileScale, cfg.rayFileWavelengths, warnings);

    for (const QJsonValue& v : root.value(QStringLiteral("extraSources")).toArray())
        if (v.isObject()) cfg.extraSources.push_back(sourceSpecFromJson(v.toObject(),
                                                                       warnings));

    for (const QJsonValue& v : root.value(QStringLiteral("surfaceOverrides")).toArray())
        if (v.isObject()) cfg.surfaceOverrides.push_back(overrideFromJson(v.toObject()));

    const QJsonObject phys = root.value(QStringLiteral("physics")).toObject();
    cfg.physics.fresnel    = flag(phys, "fresnel",    cfg.physics.fresnel);
    cfg.physics.absorption = flag(phys, "absorption", cfg.physics.absorption);
    cfg.physics.scattering = flag(phys, "scattering", cfg.physics.scattering);
    cfg.physics.roughness  = flag(phys, "roughness",  cfg.physics.roughness);
    cfg.physics.dispersion = flag(phys, "dispersion", cfg.physics.dispersion);
    cfg.physics.coatings         = flag(phys, "coatings", cfg.physics.coatings);
    cfg.physics.volumeScattering = flag(phys, "volumeScattering", cfg.physics.volumeScattering);
    cfg.physics.polarised        = flag(phys, "polarised", cfg.physics.polarised);
    cfg.physics.roughnessOverride =
        std::min(num(phys, "roughnessOverride", cfg.physics.roughnessOverride), 1.0);
    cfg.physics.scatterOverride =
        std::min(num(phys, "scatterOverride", cfg.physics.scatterOverride), 1.0);
    cfg.physics.absorptionScale =
        std::clamp(num(phys, "absorptionScale", cfg.physics.absorptionScale), 0.0, 1000.0);

    if (root.contains(QStringLiteral("strayPaths"))) {
        const QJsonObject sp = root.value(QStringLiteral("strayPaths")).toObject();
        cfg.strayPaths.enabled  = flag(sp, "enabled", true);
        cfg.strayPaths.maxPaths = std::size_t(std::clamp(num(sp, "maxPaths", 4096.0),
                                                        1.0, 1000000.0));
        cfg.strayPaths.surfaceSet.clear();
        for (const QJsonValue& v : sp.value(QStringLiteral("surfaceSet")).toArray())
            cfg.strayPaths.surfaceSet.push_back(int(v.toDouble(-1.0)));
        cfg.strayPaths.setNames.clear();
        for (const QJsonValue& v : sp.value(QStringLiteral("setNames")).toArray())
            cfg.strayPaths.setNames.push_back(v.toString());
    }

    const QJsonObject run = root.value(QStringLiteral("run")).toObject();
    cfg.rays    = std::clamp(int(num(run, "rays", cfg.rays)), 1, 100000000);
    cfg.threads = unsigned(std::clamp(int(num(run, "threads", int(cfg.threads))), 0, 1024));
    cfg.seed    = std::uint64_t(std::max(0.0, num(run, "seed", double(cfg.seed))));
    cfg.nTheta  = std::clamp(int(num(run, "nTheta", cfg.nTheta)), 0, 720);
    cfg.nPhi    = std::clamp(int(num(run, "nPhi", cfg.nPhi)), 1, 720);
    cfg.detectorBins = std::clamp(int(num(run, "detectorBins", cfg.detectorBins)), 0, 4096);
    cfg.deterministicGrids = flag(run, "deterministicGrids", cfg.deterministicGrids);

    out = cfg;

    // A file with no "document" leaves the caller's document alone rather than
    // clearing it: the old format is a bare configuration, and a scene the user
    // has composed is not something opening one should silently discard. The
    // window decides what to do about that; here it is simply not overwritten.
    if (doc && root.contains(QStringLiteral("document")))
        *doc = scenedoc::SceneDocument::fromJson(
            root.value(QStringLiteral("document")).toObject(), warnings);

    return true;
}

bool fromJson(const QString& json, SimConfig& out, QString* errorOut,
              QStringList* warnings) {
    return fromJson(json, out, nullptr, errorOut, warnings);
}

bool save(const QString& path, const SimConfig& cfg, const scenedoc::SceneDocument* doc,
          QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    const QByteArray bytes = toJson(cfg, doc).toUtf8();
    if (f.write(bytes) != bytes.size()) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return true;
}

bool save(const QString& path, const SimConfig& cfg, QString* errorOut) {
    return save(path, cfg, nullptr, errorOut);
}

bool load(const QString& path, SimConfig& out, scenedoc::SceneDocument* doc,
          QString* errorOut, QStringList* warnings) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return fromJson(QString::fromUtf8(f.readAll()), out, doc, errorOut, warnings);
}

bool load(const QString& path, SimConfig& out, QString* errorOut,
          QStringList* warnings) {
    return load(path, out, nullptr, errorOut, warnings);
}

} // namespace configio
