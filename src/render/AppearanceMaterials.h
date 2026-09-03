// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <Graphic3d_BSDF.hxx>
#include <Graphic3d_PBRMaterial.hxx>
#include <Quantity_Color.hxx>

#include "core/SurfaceOptics.h"

// The one place a LuxTrace surface becomes something OCCT's path tracer can
// shade. Pure translation: no viewer types, no Qt widgets, no GL, so the whole
// mapping is testable headless and a machine that cannot path-trace still
// compiles and runs every assertion about it.
//
// What this is *not* is a second physics. OCCT's path tracer has its own
// two-layer RGB BSDF, and it does not consult `bsdf::Surface`'s ABg or
// Henyey-Greenstein lobes, `Coating`'s characteristic-matrix stack, the
// Sellmeier curve, or `Polarisation` at all. Everything below is an appearance
// mapping chosen so that two surfaces that differ optically also differ on
// screen -- and so that the difference points the same way the tracer's does.
namespace appearance {

// The three wavelengths a spectral quantity is sampled at to become RGB.
// Representative sRGB primaries, not a colorimetric integration: three samples
// is what makes gold look gold, and the render is a preview, not a measurement.
inline constexpr double kLambdaR = 610.0;
inline constexpr double kLambdaG = 550.0;
inline constexpr double kLambdaB = 465.0;

// Reference wavelength for anything asked for at "one" wavelength -- the
// coating residual, the glass index. The d line is what a catalogue quotes.
inline constexpr double kLambdaRef = 587.6;

// What one surface looks like, in both of the shading models OCCT has.
//
// Both are filled, always. `bsdf` is what the path tracer consumes and `pbr`
// what the rasterized preview does, so switching the renderer changes the
// noise and the shadows and not the material -- a raster preview that
// disagreed with the render it previews would be worse than no preview.
struct Material {
    Graphic3d_BSDF        bsdf;
    Graphic3d_PBRMaterial pbr;

    // The flat colour, for the raster fallback and for a machine with no path
    // tracing at all.
    Quantity_Color colour{0.78, 0.78, 0.82, Quantity_TOC_RGB};
    // Presentation transparency, 0 opaque .. 1 invisible. Raster only: the path
    // tracer gets transmission from `bsdf.Kt`.
    float transparency = 0.0f;

    // A receiver is a measurement plane, not a part, and in a render it sits in
    // front of the optic and blocks the shot. The scene builder drops these
    // unless the user asks to see them for framing.
    bool hidden = false;
};

// The mapping. See the dispatch comment in the .cpp for what goes where.
Material materialFor(const SurfaceOptics& optics);

// Normal-incidence reflectance of `optics`' coating at `lambdaNm`, or a
// negative value when the surface carries no coating.
//
// Public because it is the number the coating branch turns into `FresnelBase`,
// and a mapping whose one physical input cannot be read back is a mapping that
// can only be checked by looking at pictures.
double coatingResidual(const SurfaceOptics& optics, double lambdaNm = kLambdaRef);

// Bare unpolarised Fresnel reflectance at normal incidence, 1 -> n. What the
// coating residual above is compared against.
double bareReflectance(double n);

} // namespace appearance
