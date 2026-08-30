#include "AppearanceEmitters.h"

#include "AppearanceScene.h"
#include "core/Spectrum.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>

namespace appearance {
namespace {

constexpr double kPi = 3.14159265358979323846;

// How finely the SPD is walked to get a colour. A fixed quadrature of the
// inverse CDF rather than a random draw: the render restarts its estimate often
// enough without the emitter changing colour underneath it.
constexpr int kColourSamples = 256;

// The emitting area of a source, in mm^2. Zero means "no area", which is the
// one thing that decides between geometry and a light.
double emittingArea(const SourceConfig& s) {
    if (s.tracesRayFile() && s.rayFile) {
        // A measured set records the extent of the surface it was measured
        // over, which is exactly the plate to emit from.
        const Vec3 e = s.rayFile->extent();
        const double a = std::abs(e.x) * s.rayFileScale;
        const double b = std::abs(e.y) * s.rayFileScale;
        return (a > 1e-9 && b > 1e-9) ? a * b : 0.0;
    }

    if (s.type == SourceConfig::Type::Collimated)
        return s.beamRadius > 1e-9 ? kPi * s.beamRadius * s.beamRadius : 0.0;

    switch (s.shape) {
        case SourceConfig::Shape::Rect:
            return (s.sizeA > 1e-9 && s.sizeB > 1e-9) ? s.sizeA * s.sizeB : 0.0;
        case SourceConfig::Shape::Disc:
            return s.sizeA > 1e-9 ? kPi * s.sizeA * s.sizeA : 0.0;
        case SourceConfig::Shape::Sphere:
            return s.sizeA > 1e-9 ? 4.0 * kPi * s.sizeA * s.sizeA : 0.0;
        case SourceConfig::Shape::PointLike:
        default:
            return 0.0;
    }
}

// The radius of the emitting face, for the soft shadow the light it becomes
// should cast. A real penumbra is set by the size of the real emitter, so this
// is read off the source rather than guessed from the scene -- which is what a
// point source, having no size at all, has to do instead.
double emittingRadius(const SourceConfig& s) {
    if (s.tracesRayFile() && s.rayFile) {
        const Vec3 e = s.rayFile->extent();
        return 0.5 * std::hypot(std::abs(e.x) * s.rayFileScale,
                                std::abs(e.y) * s.rayFileScale);
    }
    if (s.type == SourceConfig::Type::Collimated) return std::max(0.0, s.beamRadius);
    switch (s.shape) {
        case SourceConfig::Shape::Rect:   return 0.5 * std::hypot(s.sizeA, s.sizeB);
        case SourceConfig::Shape::Disc:
        case SourceConfig::Shape::Sphere: return std::max(0.0, s.sizeA);
        case SourceConfig::Shape::PointLike:
        default:                          return 0.0;
    }
}

// The face light leaves from, in the source's own placement. Null when the
// source has no area, which the caller has already established.
TopoDS_Shape emittingFace(const SourceConfig& s) {
    const gp_Ax2 frame(s.origin, s.axis);

    auto disc = [&frame](double r) -> TopoDS_Shape {
        const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(gp_Circ(frame, r));
        const TopoDS_Wire wire = BRepBuilderAPI_MakeWire(edge);
        return BRepBuilderAPI_MakeFace(gp_Pln(gp_Ax3(frame)), wire);
    };
    auto rect = [&frame](double a, double b) -> TopoDS_Shape {
        return BRepBuilderAPI_MakeFace(gp_Pln(gp_Ax3(frame)),
                                       -0.5 * a, 0.5 * a, -0.5 * b, 0.5 * b);
    };

    if (s.tracesRayFile() && s.rayFile) {
        const Vec3 e = s.rayFile->extent();
        return rect(std::abs(e.x) * s.rayFileScale, std::abs(e.y) * s.rayFileScale);
    }
    if (s.type == SourceConfig::Type::Collimated) return disc(s.beamRadius);

    switch (s.shape) {
        case SourceConfig::Shape::Rect:   return rect(s.sizeA, s.sizeB);
        case SourceConfig::Shape::Disc:   return disc(s.sizeA);
        case SourceConfig::Shape::Sphere: return BRepPrimAPI_MakeSphere(frame, s.sizeA).Shape();
        default:                          return TopoDS_Shape();
    }
}

} // namespace

Graphic3d_Vec3 spectrumColour(const SpectrumConfig& spec) {
    SampledSpectrum sampled;
    sampled.build(spec);

    double acc[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < kColourSamples; ++i) {
        const double u      = (double(i) + 0.5) / double(kColourSamples);
        const double lambda = sampled.sample(std::size_t(i), u);
        double w[3];
        spectrum::srgbWeights(lambda, w);
        acc[0] += w[0];
        acc[1] += w[1];
        acc[2] += w[2];
    }

    const double peak = std::max({acc[0], acc[1], acc[2]});
    if (!(peak > 0.0))
        return Graphic3d_Vec3(1.0f, 1.0f, 1.0f);   // outside the visible band

    return Graphic3d_Vec3(float(std::max(0.0, acc[0] / peak)),
                          float(std::max(0.0, acc[1] / peak)),
                          float(std::max(0.0, acc[2] / peak)));
}

double sourceRadiance(const SourceConfig& s) {
    const double area = emittingArea(s);
    if (!(area > 0.0)) return 0.0;
    const double flux = std::max(0.0, s.power);
    return flux / (area * kPi);
}

EmitterBuild buildEmitters(const std::vector<SourceConfig>& sources, double sceneSize) {
    EmitterBuild out;
    if (sources.empty()) return out;

    // What a source of the full run power becomes as an OCCT light intensity.
    // See kEmitterIrradiance: the square is what cancels the 1/d^2 falloff, so
    // the answer does not depend on how big the scene happens to be.
    const double reach = 0.5 * (sceneSize > 0.0 ? sceneSize : 100.0);
    const double fullPowerIntensity = kEmitterIrradiance * reach * reach;

    // One pass to find the scale, a second to apply it. The divisor is a
    // property of the whole source list rather than of any one source, which is
    // what makes two emitters of different power come out in proportion instead
    // of both being drawn at full brightness.
    double brightest  = 0.0;
    double mostPowerful = 0.0;
    for (const SourceConfig& s : sources) {
        brightest    = std::max(brightest, sourceRadiance(s));
        mostPowerful = std::max(mostPowerful, std::max(0.0, s.power));
    }
    out.normalisation = brightest;

    for (const SourceConfig& s : sources) {
        Emitter e;
        e.label = s.label;
        e.origin = s.origin;

        const double radiance = sourceRadiance(s);
        if (!(radiance > 0.0)) {
            // No area to emit from. An isotropic point source is a real OCCT
            // light instead -- which is also the only way it can cast a shadow,
            // there being no geometry to cast one from.
            e.pointLike = true;
            e.intensity = fullPowerIntensity *
                          (mostPowerful > 0.0 ? std::max(0.0, s.power) / mostPowerful : 1.0);
            // A non-zero angular size is what makes the shadow edge soft. As a
            // fraction of the scene, because a penumbra fixed in millimetres is
            // right on a lens and invisible on a luminaire.
            e.smoothRadius = std::max(1e-3, 0.01 * (sceneSize > 0.0 ? sceneSize : 100.0));
            ++out.pointSources;
            out.emitters.push_back(e);
            continue;
        }

        const TopoDS_Shape face = emittingFace(s);
        if (face.IsNull()) continue;

        // The light half of an area source -- see Emitter::intensity for why
        // the face alone is not enough. Same power law as a point source, so
        // two emitters of different power still light the scene in proportion,
        // and a soft radius taken from the emitter's own size, because that is
        // what sets a real penumbra.
        e.intensity = fullPowerIntensity *
                      (mostPowerful > 0.0 ? std::max(0.0, s.power) / mostPowerful : 1.0);
        e.smoothRadius = std::max(1e-3, emittingRadius(s));

        const Graphic3d_Vec3 colour = spectrumColour(s.spectrum);
        const float scale = float(kBrightestRadiance *
                                  (brightest > 0.0 ? radiance / brightest : 1.0));

        e.radiance = colour * scale;

        // The emitting face is a light, not a part: nothing diffuse, nothing
        // specular, only `Le`. A die given an albedo as well would pick up the
        // reflector around it and read as a lit white disc rather than as the
        // thing doing the lighting.
        Material m;
        m.bsdf              = Graphic3d_BSDF::CreateDiffuse(Graphic3d_Vec3(0.0f));
        m.bsdf.Le           = e.radiance;
        m.colour            = Quantity_Color(std::min(1.0, double(colour.r())),
                                             std::min(1.0, double(colour.g())),
                                             std::min(1.0, double(colour.b())),
                                             Quantity_TOC_RGB);
        m.transparency      = 0.0f;
        m.pbr.SetColor(m.colour);
        m.pbr.SetMetallic(0.0f);
        m.pbr.SetRoughness(1.0f);
        m.pbr.SetAlpha(1.0f);
        // The rasterized preview has no path-traced emission, so the emitter's
        // own colour is put in the PBR emission term instead. Same surface,
        // same colour, in whichever renderer is running.
        m.pbr.SetEmission(colour);

        e.shape = new AIS_Shape(face);
        applyMaterial(e.shape, m);
        out.emitters.push_back(e);
    }

    return out;
}

} // namespace appearance
