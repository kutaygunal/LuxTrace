#include "AppearanceMaterials.h"

#include <algorithm>
#include <cmath>

namespace appearance {
namespace {

float clamp01(double v) {
    if (!(v > 0.0)) return 0.0f;                 // also catches NaN
    return float(v < 1.0 ? v : 1.0);
}

Graphic3d_Vec3 grey(double v) {
    const float g = clamp01(v);
    return Graphic3d_Vec3(g, g, g);
}

Quantity_Color toColour(const Graphic3d_Vec3& v) {
    return Quantity_Color(std::max(0.0f, v.r()), std::max(0.0f, v.g()),
                          std::max(0.0f, v.b()), Quantity_TOC_RGB);
}

// The GGX alpha this surface actually scatters with, clamped to the [0,1]
// roughness OCCT's glossy lobe takes. `effectiveBsdf` is the single place the
// three ways of saying "not quite polished" are reconciled, so this asks it
// rather than reading `roughness` and `scatter` a second time.
float glossRoughness(const SurfaceOptics& o) {
    const bsdf::Surface b = o.effectiveBsdf();
    if (b.model == bsdf::Model::Microfacet) return clamp01(b.alpha);
    // A Lambertian or tabulated lobe has no single alpha. Its diffusing part is
    // carried by `Kd` instead; whatever is left specular is polished.
    return 0.0f;
}

// How much of this surface's reflection leaves diffusely, 0..1. A Lambertian
// shorthand says so directly; a microfacet lobe does not diffuse at all.
double diffuseFraction(const SurfaceOptics& o) {
    const bsdf::Surface b = o.effectiveBsdf();
    if (b.model == bsdf::Model::Lambertian) return std::clamp(b.fraction, 0.0, 1.0);
    return 0.0;
}

// The metal's own colour: complex Fresnel at normal incidence, per channel,
// from the same `metalReflectance` the tracer calls. An aluminium reflector and
// a gold one differ here for the same reason they differ in a trace.
Graphic3d_Vec3 conductorReflectance(const Graphic3d_Vec3& n, const Graphic3d_Vec3& k) {
    return Graphic3d_Vec3(
        float(metalReflectance(1.0, n.r(), k.r(), 1.0)),
        float(metalReflectance(1.0, n.g(), k.g(), 1.0)),
        float(metalReflectance(1.0, n.b(), k.b(), 1.0)));
}

// A slight sheen on a painted, non-diffusing surface: a 4 % Schlick coat, which
// is what a dielectric overcoat reflects at normal incidence. Not applied to a
// matte cavity -- an integrating sphere wall is matte white and must read that
// way.
void addDielectricCoat(Graphic3d_BSDF& b, float roughness) {
    b.Kc          = Graphic3d_Vec4(1.0f, 1.0f, 1.0f, roughness);
    b.FresnelCoat = Graphic3d_Fresnel::CreateSchlick(Graphic3d_Vec3(0.04f));
}

} // namespace

double bareReflectance(double n) {
    if (!(n > 1.0)) return 0.0;
    const double r = (n - 1.0) / (n + 1.0);
    return r * r;
}

double coatingResidual(const SurfaceOptics& o, double lambdaNm) {
    if (!o.coating.active()) return -1.0;
    // A coating on an opaque surface has no dielectric interface to modulate,
    // so a nominal glass index stands in and only the coating's own model is
    // read; the mirror's `reflectivity` already says what the substrate does.
    const double n    = o.index > 0.0 ? o.indexAt(lambdaNm) : 1.5;
    const double bare = bareReflectance(n);
    const double r    = o.coating.reflectance_(1.0, n, 1.0, lambdaNm, bare);
    return std::clamp(r, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// The dispatch.
//
//   isDetector             hidden; a measurement plane is not a part
//   index == 0  opaque     a metal, a mirror, or a painted surface
//        material is a metal   -> CreateMetallic + per-channel CreateConductor
//        specular and bright   -> CreateMetallic + CreateConstant(1), weight R
//        otherwise             -> CreateDiffuse(R), with a 4 % coat if not matte
//   index >  0  dielectric -> CreateGlass(n, absorption), coat roughened if rough
//
// The middle opaque case is the one the render lives or dies on. Every
// reflector in the built-in library is an ideal mirror with no catalogue
// material assigned, so a plain metal-or-diffuse split would map every
// parabolic reflector in the application to a lump of grey putty. A specular
// surface that reflects most of what reaches it is a mirror, and is drawn as
// one.
// ---------------------------------------------------------------------------
Material materialFor(const SurfaceOptics& o) {
    Material out;

    if (o.isDetector) {
        // Built anyway, so that "show detectors" has something to show: a faint
        // matte target for framing, not a part of the optic.
        out.hidden       = true;
        out.bsdf         = Graphic3d_BSDF::CreateDiffuse(Graphic3d_Vec3(0.25f, 0.65f, 0.40f));
        out.colour       = Quantity_Color(0.25, 0.85, 0.45, Quantity_TOC_RGB);
        out.transparency = 0.55f;
        out.pbr.SetColor(out.colour);
        out.pbr.SetMetallic(0.0f);
        out.pbr.SetRoughness(0.9f);
        out.pbr.SetAlpha(1.0f - out.transparency);
        return out;
    }

    const float  rough   = glossRoughness(o);
    const double diffuse = diffuseFraction(o);

    if (o.index <= 0.0) {
        // ---- opaque ---------------------------------------------------------
        const double R = std::clamp(o.reflectivity, 0.0, 1.0);

        if (o.material.valid() && o.material.isMetal()) {
            const Graphic3d_Vec3 n(float(o.material.indexAt(kLambdaR)),
                                   float(o.material.indexAt(kLambdaG)),
                                   float(o.material.indexAt(kLambdaB)));
            const Graphic3d_Vec3 k(float(o.material.extinctionAt(kLambdaR)),
                                   float(o.material.extinctionAt(kLambdaG)),
                                   float(o.material.extinctionAt(kLambdaB)));

            // Weight 1: the conductor Fresnel already carries the level, and
            // multiplying the catalogue reflectance by the surface's own
            // `reflectivity` would book the same loss twice.
            // The per-channel overload, which is what makes gold gold: OCCT
            // reduces (n, k) per channel to the Schlick specular colour they
            // imply, so the three samples reach the shader as a colour rather
            // than as one scalar index standing in for a metal's whole curve.
            out.bsdf   = Graphic3d_BSDF::CreateMetallic(
                Graphic3d_Vec3(1.0f), Graphic3d_Fresnel::CreateConductor(n, k), rough);
            out.colour = toColour(conductorReflectance(n, k));
            out.pbr.SetMetallic(1.0f);
            out.pbr.SetIOR(std::clamp(n.g(), 1.0f, 3.0f));
        } else if (diffuse < 0.5 && R >= 0.5) {
            // An ideal mirror: reflects R of everything, at every angle.
            out.bsdf   = Graphic3d_BSDF::CreateMetallic(
                grey(R), Graphic3d_Fresnel::CreateConstant(1.0f), rough);
            out.colour = toColour(grey(R));
            out.pbr.SetMetallic(1.0f);
        } else {
            // Paint, or a matte cavity. `reflectivity` is the albedo, and a
            // surface that never said carries the mid grey the viewport uses.
            const double albedo = R > 0.0 ? R : 0.65;
            out.bsdf   = Graphic3d_BSDF::CreateDiffuse(grey(albedo));
            if (diffuse < 0.5) addDielectricCoat(out.bsdf, std::max(rough, 0.15f));
            out.colour = toColour(grey(albedo));
            out.pbr.SetMetallic(0.0f);
        }

        out.pbr.SetColor(out.colour);
        out.pbr.SetRoughness(std::max(rough, diffuse >= 0.5 ? 0.9f : 0.08f));
        out.pbr.SetAlpha(1.0f);
        out.transparency = 0.0f;
    } else {
        // ---- dielectric -----------------------------------------------------
        //
        // Dispersion is deliberately not passed on: OCCT traces RGB, not
        // wavelengths, so a prism will not split light here however the index
        // is sampled. The Ray Diagram and the Irradiance tab are where
        // dispersion is shown, and they already show it.
        const float n = float(std::clamp(o.indexAt(kLambdaRef, false), 1.0, 4.0));

        // Beer-Lambert, 1/mm, and scene units are millimetres. The absorption
        // colour is neutral because `OpticalMaterial::alpha` is a single
        // wavelength-independent number: tinting from a spectrum we do not have
        // would be inventing a colour rather than deriving one.
        const float absorb = float(std::max(0.0, o.absorption));

        // Note where this lands: `CreateGlass` builds a transmissive *base*
        // under a specular *coat*, and puts the dielectric Fresnel on the coat
        // (Kc / FresnelCoat), leaving Ks and FresnelBase unused. So a ground or
        // moulded surface is roughened through the coat's own roughness slot --
        // writing Ks.w() here would set the roughness of a lobe whose weight is
        // zero, which is a line of code that reads correct and does nothing.
        out.bsdf = Graphic3d_BSDF::CreateGlass(Graphic3d_Vec3(1.0f), Graphic3d_Vec3(1.0f),
                                               absorb, n);
        if (rough > 0.0f) out.bsdf.Kc.a() = rough;

        out.colour       = Quantity_Color(0.62, 0.78, 0.92, Quantity_TOC_RGB);
        out.transparency = 0.65f;
        out.pbr.SetColor(out.colour);
        out.pbr.SetMetallic(0.0f);
        out.pbr.SetRoughness(std::max(rough, 0.03f));
        out.pbr.SetIOR(std::clamp(n, 1.0f, 3.0f));
        out.pbr.SetAlpha(1.0f - out.transparency);
    }

    // ---- the coating ---------------------------------------------------------
    //
    // Not modelled as OCCT's coat layer. That layer *adds* a reflection, which
    // is the opposite of what an AR coating does; what a coating changes is the
    // reflectance of the base interface, so it replaces the base Fresnel with a
    // constant at its own residual. An HR coating is the same statement with a
    // high residual and no transmission left.
    //
    // An approximation, and the right one for appearance: a coated lens stops
    // reading as a bare one, which is a visible several per cent at grazing
    // angles and the whole reason a coating is specified in the first place.
    if (o.coating.active()) {
        const double residual = coatingResidual(o, kLambdaRef);
        if (o.coating.highReflector) {
            // Nothing left to transmit and nothing diffuse: a dielectric high
            // reflector is a mirror, and is built the way the mirror branch
            // above builds one -- a base glossy lobe of weight R behind a
            // Fresnel that always reflects.
            out.bsdf.Kt          = Graphic3d_Vec3(0.0f);
            out.bsdf.Kd          = Graphic3d_Vec3(0.0f);
            out.bsdf.Kc          = Graphic3d_Vec4(0.0f);
            out.bsdf.Ks          = Graphic3d_Vec4(grey(residual), rough);
            out.bsdf.FresnelBase = Graphic3d_Fresnel::CreateConstant(1.0f);
            out.colour           = toColour(grey(residual));
            out.transparency     = 0.0f;
            out.pbr.SetColor(out.colour);
            out.pbr.SetMetallic(1.0f);
            out.pbr.SetAlpha(1.0f);
        } else if (o.index > 0.0) {
            // An AR coating suppresses the reflection of the glass interface,
            // and for a dielectric that interface is OCCT's *coat* layer. This
            // is the line that makes a coated lens stop reading as a bare one.
            out.bsdf.FresnelCoat = Graphic3d_Fresnel::CreateConstant(clamp01(residual));
        } else {
            out.bsdf.FresnelBase = Graphic3d_Fresnel::CreateConstant(clamp01(residual));
        }
    }

    return out;
}

} // namespace appearance
