// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <cmath>
#include "Bsdf.h"
#include "Coating.h"
#include "Material.h"
#include "Metasurface.h"


// Optical behaviour of one surface, shared verbatim by the three
// representations a surface passes through: OpticalSurface (B-Rep + optics),
// MeshSurface (tessellated) and SceneSurface (flattened for the BVH).
//
// The three used to declare the same four fields each and copy them across by
// hand twice. Inheriting instead means a new optical property is declared once
// and reaches the tracer without touching either conversion loop.
struct SurfaceOptics {
    // Fixed split, used when `fresnel` is off. Whatever is neither reflected
    // nor transmitted is absorbed at the surface.
    double reflectivity   = 0.0;
    double transmissivity = 0.0;

    // Refractive index at the d line (587.6 nm). 0 == opaque.
    double index = 0.0;

    // Cauchy dispersion coefficient B, in um^2: n(lambda) = A + B / lambda^2,
    // with A fixed so that n(587.6 nm) == index. 0 == non-dispersive.
    // BK7-like crown glass is about 0.00420 um^2 (Abbe number ~64).
    // Only consulted when no catalogue material is assigned.
    double dispersionB = 0.0;

    // The catalogue material, when one has been assigned. It supersedes the
    // Cauchy shorthand above: a Sellmeier fit holds across the whole visible
    // band where a two-term Cauchy diverges outside it, and it is the form every
    // glass catalogue actually publishes. A metal material also supplies the
    // complex index its reflectance is computed from.
    OpticalMaterial material;

    // Beer-Lambert bulk attenuation of the medium *behind* this surface, 1/mm.
    // Only consulted while a ray is travelling inside the solid.
    double absorption = 0.0;

    // What this surface *looks* like, and nothing else.
    //
    // The one field in this struct the tracer never reads. Everything else here
    // is a physical quantity that changes an answer; this changes a picture.
    // It exists because reflectivity is a single scalar -- grey -- so a red
    // brick, a yellow hub and a black tyre are the same surface to this model
    // and come out of the Appearance preview as three identical greys. A CAD
    // assembly is *mostly* made of parts like that, and a render in which the
    // whole assembly is one colour is not a render of it.
    //
    // Held apart from the physics rather than folded into it, deliberately.
    // Doing this properly means a spectral reflectance curve per surface, which
    // is a real feature with real consequences -- it would change every number
    // the tracer reports, and colour-over-angle would become a measurable
    // output rather than a limit. This is not that, and must not be mistaken
    // for it: it is sRGB paint on the preview, it moves no flux, and a run
    // gives bit-identical answers with it set or unset.
    //
    // Negative means "unset": the render derives the colour from the physics
    // the way it always has, which is what every built-in scene wants -- an
    // aluminium reflector's colour is its complex index and not a swatch
    // somebody picked.
    double appearanceRgb[3] = {-1.0, -1.0, -1.0};

    bool hasAppearanceColour() const {
        return appearanceRgb[0] >= 0.0 && appearanceRgb[1] >= 0.0 && appearanceRgb[2] >= 0.0;
    }
    void setAppearanceColour(double r, double g, double b) {
        appearanceRgb[0] = r;
        appearanceRgb[1] = g;
        appearanceRgb[2] = b;
    }
    void clearAppearanceColour() {
        appearanceRgb[0] = appearanceRgb[1] = appearanceRgb[2] = -1.0;
    }

    // Fraction of the reflected (and of the transmitted) energy that leaves
    // cosine-weighted about the surface normal instead of specularly.
    // 0 is a polished surface, 1 a perfect Lambertian diffuser.
    //
    // A shorthand now, not a model of its own: effectiveBsdf() reads it as a
    // Lambertian lobe of this fraction.
    double scatter = 0.0;

    // RMS surface slope error, radians -- the simplest possible statement of
    // "not quite polished", and the number a polish tolerance is written in.
    //
    // A shorthand now, not a model of its own: effectiveBsdf() reads it as a
    // GGX microfacet of the same RMS slope. It used to be applied as a Gaussian
    // jitter of the interaction normal with a rejection branch, which is not an
    // energy-conserving BSDF, has no shadowing-masking term, biases toward
    // specular by however often the tilt had to be rejected, and cannot be
    // fitted to measured data.
    double roughness = 0.0;

    // How this surface scatters, where it is described by a real BSDF rather
    // than by one of the two shorthands above.
    bsdf::Surface bsdf;

    // Scattering *inside* the medium behind this surface, rather than at it.
    // Every white diffusing plastic in every luminaire is a volume scatterer,
    // and there was no way to say so at all.
    bsdf::Volume volume;

    // A designed phase gradient across this surface, if it has one.
    //
    // Off by default and behind one null check, like `bsdf` and `coating`
    // beside it: a metasurface is a different physical law -- the
    // generalised Snell law rather than Snell -- and every scene that does
    // not have one should pay a predicted branch for the fact.
    meta::Metasurface metasurface;

    // The thin film on this surface. Without one, every refractive surface pays
    // bare-glass Fresnel and a multi-element system overstates its loss by
    // roughly 3.5 % per surface against any real lens.
    coating::Coating coating;

    // Angle-dependent reflectance from the Fresnel equations instead of the
    // fixed reflectivity/transmissivity split. Only meaningful when index > 0;
    // an opaque mirror keeps its fixed reflectivity either way.
    bool fresnel = false;

    // Where two solids overlap, the higher priority wins: a ray inside both is
    // treated as travelling in the higher-priority medium. This is how FRED and
    // TracePro resolve modelling overlap, and it is what lets an index-matched
    // interface (glass -> cement -> glass) refract against the right pair of
    // indices instead of always assuming glass -> air.
    int mediumPriority = 0;

    bool isDetector = false;

    // Receiver binning, when isDetector. Kept here rather than written into the
    // mesher so a user can ask for the resolution they need.
    int    detNX = 0, detNY = 0;          // 0 == the default grid

    // Acceptance half-angle about the receiver normal, degrees. A ray arriving
    // outside the cone is not counted -- which is how a real photometer, and a
    // real fibre, behave. >= 180 accepts everything.
    double detAcceptanceDeg = 180.0;

    // What a receiver does with a ray its acceptance cone refuses.
    //   Absorb  the branch stops there, as an absorbing photometer head does
    //   Pass    the branch carries on undeflected, as a recording plane does
    // Refused flux is booked to its own channel either way: it was not absorbed
    // by anything, it was refused by a measurement condition, and an
    // absorbed-flux figure that silently includes it is a wrong loss budget.
    enum class RejectMode : int { Absorb = 0, Pass = 1 };
    RejectMode detRejectMode = RejectMode::Absorb;

    // The one scattering model this surface actually has.
    //
    // A surface could once describe its scattering in three unrelated ways at
    // the same time, and all three ran side by side inside a single
    // interaction: a Gaussian tilt of the normal, a Lambertian fraction, and a
    // real BSDF. The first two decided *direction* while the energy stayed
    // whatever specular Fresnel gave it -- so a rough dielectric reflected as
    // though it were polished and then left in a diffuse direction. Three
    // models meant three behaviours that could disagree, and the two that were
    // not energy conserving were the ones every built-in scene used.
    //
    // There is one now, and this is where the older two are read into it:
    //
    //   a real BSDF wins wherever one is set
    //   `scatter`   becomes a Lambertian lobe of that fraction
    //   `roughness` becomes a GGX microfacet of the same RMS slope
    //
    // Both fields stay, because scenes and saved configs were written against
    // them and because one number is the right way to say "slightly matte".
    bsdf::Surface effectiveBsdf() const {
        if (!bsdf.isSpecular()) return bsdf;

        if (scatter > 0.0) {
            bsdf::Surface b;
            b.model    = bsdf::Model::Lambertian;
            b.fraction = scatter < 1.0 ? scatter : 1.0;
            return b;
        }
        if (roughness > 0.0) {
            bsdf::Surface b;
            b.model = bsdf::Model::Microfacet;
            // A Gaussian slope error of sigma per tangent axis is a Beckmann
            // distribution of alpha = sqrt(2) sigma, and GGX is conventionally
            // matched to Beckmann by equating the two alphas. So the number in
            // the box keeps meaning the RMS slope it always meant.
            b.alpha    = 1.4142135623730951 * roughness;
            b.fraction = 1.0;
            return b;
        }
        return bsdf::Surface{};        // polished
    }

    // Refractive index at `lambdaNm`. The catalogue material wins where one is
    // assigned; otherwise the Cauchy shorthand does. `dispersion` off pins every
    // wavelength to the reference index, which is what makes a monochromatic
    // trace and a dispersion-free one identical.
    // `index` stays authoritative for the level and the material supplies the
    // *shape*: the curve is the material's, shifted so that n(587.6 nm) is
    // exactly `index`. Assigning a catalogue material sets `index` to its d-line
    // value, so the default is the published glass -- and overriding `index`
    // afterwards moves the whole curve, which is what "this glass, but denser"
    // has to mean if the two fields are not to contradict each other.
    double indexAt(double lambdaNm, bool dispersion = true) const {
        if (index <= 0.0) return 0.0;
        if (!dispersion) return index;
        if (material.valid())
            return index + (material.indexAt(lambdaNm) - material.nd);
        if (dispersionB == 0.0 || lambdaNm <= 0.0) return index;
        const double lu = lambdaNm * 1e-3, ld = 587.6e-3;    // nm -> um
        return index + dispersionB * (1.0 / (lu * lu) - 1.0 / (ld * ld));
    }
};
