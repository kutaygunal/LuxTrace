#pragma once
#include <vector>

#include <AIS_Shape.hxx>
#include <Graphic3d_Vec3.hxx>
#include <QString>

#include "core/RayTracer.h"

// Making the light in the render come from the light in the scene.
//
// Two mechanisms, because a source has two shapes it can be. A source with a
// real emitting area becomes *geometry* with `Graphic3d_BSDF::Le` set on it,
// which is what puts a visible bright face on the die and lets the reflector
// around it be lit by the thing it is built around. An isotropic point source
// has no area to emit from, so it becomes an OCCT positional light instead.
//
// **What the radiance is, and what it is not.** The values below are correct in
// their *ratios*: two sources of different power, different area and different
// spectrum come out in the right proportion and the right colours relative to
// each other, because they are computed from L = flux / (A * pi) and from the
// source's own SPD. They are then divided by a scene-wide normalisation so the
// brightest maps to a fixed display value. That normalisation is the point at
// which the numbers stop being physical: nothing here is cd/m^2, and no part of
// the application may read one off this view.
namespace appearance {

// One source, as the renderer draws it.
struct Emitter {
    // The emitting face, or null for a point source.
    Handle(AIS_Shape) shape;
    // Emitted radiance, already normalised and coloured. Meaningful only with
    // `shape`.
    Graphic3d_Vec3 radiance{0.0f, 0.0f, 0.0f};

    // A point source has no area, so it is an OCCT light rather than geometry.
    bool   pointLike = false;
    gp_Pnt origin{0, 0, 0};
    // Light intensity and the angular size that turns a hard shadow into a soft
    // one. Point sources only.
    double intensity    = 1.0;
    double smoothRadius = 0.0;

    QString label;
};

struct EmitterBuild {
    std::vector<Emitter> emitters;
    // The physical radiance the brightest area source had, before normalisation
    // -- the divisor that was applied. Kept so the scaling can be seen to be a
    // scaling, and so an unchanged source list produces an unchanged image
    // instead of one that breathes between redraws.
    double normalisation = 0.0;
    // How many of the emitters are lights rather than geometry.
    int pointSources = 0;
};

// The display radiance the brightest area source is mapped to.
//
// The plan says "the brightest source maps near 1.0 and Exposure does the
// rest", and 1.0 is the right *structure*: one scene-wide divisor, applied
// once. It is not the right number. A surface emitting a radiance of 1 renders,
// after filmic tone mapping at EV 0, as something around mid grey -- an emitter
// that does not look switched on, which is the one thing this phase exists to
// fix. So the normalised value is multiplied by a single constant, stated here
// rather than buried, and the exposure control moves it from there.
inline constexpr double kBrightestRadiance = 6.0;

// Linear-sRGB colour of a spectrum, scaled so its largest channel is 1.
//
// Integrated over the source's own SPD through the same `SampledSpectrum` the
// tracer draws wavelengths from, so a 2700 K source and a 6500 K one differ in
// the render for the reason they differ in life. Deterministic: a fixed
// quadrature of the inverse CDF, not a random draw.
Graphic3d_Vec3 spectrumColour(const SpectrumConfig& spectrum);

// Lambertian radiance of a source, flux / (area * pi), in the run's flux unit
// per mm^2 per steradian. Zero for a source with no emitting area, which is the
// signal that it has to become a light instead.
double sourceRadiance(const SourceConfig& source);

// Builds the emitting geometry and the point lights for a run's sources.
//
// `sceneSize` is the scene's characteristic dimension, used only to give a
// point source a sensible angular size for its soft shadow -- a shadow edge
// that is a fixed number of millimetres wide looks right on a lens and wrong on
// a luminaire.
EmitterBuild buildEmitters(const std::vector<SourceConfig>& sources, double sceneSize);

} // namespace appearance
