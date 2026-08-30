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

    // A point source has no area, so it has no face and can *only* be a light.
    // One with an area gets both -- see below.
    bool   pointLike = false;
    gp_Pnt origin{0, 0, 0};
    // The light this source also becomes, and the radius that turns a hard
    // shadow edge into a soft one.
    //
    // Every emitter gets one, not just the point sources, and the reason is a
    // property of OCCT's path tracer rather than a preference: it finds
    // emissive geometry only by a path happening to land on it. There is no
    // next-event estimation to an emissive triangle. A diffuser panel that
    // fills a third of the frame is hit constantly and lights the scene; a 5 mm
    // LED die in a 250 mm luminaire is hit almost never, and the picture comes
    // back black at every exposure -- the die visible as a bright speck,
    // everything around it unlit. Measured, not assumed: the same scene with a
    // 50 mm emitter lights up and with a 5 mm one does not.
    //
    // So the face carries `Le` and is what the camera sees glowing, and the
    // light co-located with it is what actually illuminates. The two overlap by
    // however much the face is hit directly, which is the small quantity that
    // made the face useless as a light in the first place. This is the standard
    // arrangement in any renderer whose area lights are both sampled and
    // visible, and it is why "the emitters are drawn in every mode" is true of
    // the lighting as well as of the geometry.
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

// The irradiance the full-power source is aimed to land on the middle of the
// scene, and the reason emitter intensity is not simply the source's power.
//
// OCCT's positional light falls off as 1/d^2 -- physically, and in whatever
// units its `Intensity` happens to be in. So an intensity fixed at 3 lights a
// 6 mm lens brilliantly and a 600 mm luminaire not at all: at 100 mm the same
// number arrives as 3e-4, which renders black at every exposure the toolbar
// offers. Scaling it by the square of half the scene's own size cancels the
// falloff, so a source lights its scene the same way whatever size the scene
// is, and the exposure control starts from somewhere usable rather than from
// six stops under.
inline constexpr double kEmitterIrradiance = 3.0;

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
