// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "EnvironmentMap.h"
#include "Vec3.h"
#include "RayTracer.h"
#include "SimulationResult.h"
#include "TraceScene.h"

// A camera-side integrator, and the one architectural absence the forward
// tracer could not be talked into covering.
//
// Everything else in this application starts at a source and asks where the
// light goes. A camera asks the other question -- *what does an observer see* --
// and the two are not the same measurement. A receiver reports irradiance over
// an area; an eye reports luminance along a direction, and no amount of
// receiver grids adds up to one. Luminance is what a glare limit is written in,
// what a display specification is written in, and what a luminaire is signed
// off against.
//
// So this is not a renderer bolted on for illustration. `AppearanceView` is
// already that, and its own header says why it is not a measurement: RGB, its
// own two-layer BSDF, no SurfaceOptics, no coatings, no dispersion, tone-mapped
// to sRGB. This traces the same geometry, the same SurfaceOptics, the same
// coatings and the same spectra the forward tracer does, and reports cd/m^2.
//
// The estimator is the forward tracer's next-event estimation pointed the other
// way. A camera ray walks until it meets a surface that scatters; at every such
// vertex the emitters are connected to analytically, because they *are*
// analytic here -- a point, a disc, a rectangle, a sphere, a collimated beam --
// and a path that had to find one by chance would never find a point source at
// all. Specular vertices carry no connection, since a mirror has no direction
// to connect along; they pass the path through and the emitter is picked up by
// the direct hit at the end of the chain instead. That split is what keeps the
// two ways of reaching a light from being counted twice.
namespace backward {

// Where the observer is and what they are looking through.
//
// A real focal length and a real sensor, not a field-of-view angle: an
// appearance question is asked about a photograph somebody is going to take,
// and "35 mm on full frame" is how that is specified. The two together give the
// field of view; either one alone does not.
struct CameraConfig {
    Vec3 eye{0.0, -600.0, 200.0};
    Vec3 target{0.0, 0.0, 0.0};
    Vec3 up{0.0, 0.0, 1.0};

    double focalLengthMm = 50.0;
    // The long side of the sensor. The short side follows from the pixel
    // aspect, so a wider image is a wider view rather than a stretched one.
    double sensorWidthMm = 36.0;

    // f-number. 0 or less is a pinhole: everything in focus and no bokeh, which
    // is what a measurement usually wants. A real aperture is here because
    // depth of field changes what a luminance meter with a real lens sees.
    double fNumber = 0.0;
    // Distance to the plane in focus. 0 focuses on `target`.
    double focusDistanceMm = 0.0;

    int width  = 256;
    int height = 192;

    // Camera rays per pixel, stratified within the pixel.
    int samplesPerPixel = 64;
    // Vertices per path before it is cut. A path that runs out is truncated and
    // counted, never quietly dropped.
    int maxDepth = 24;

    // Where a path stops being followed in proportion to what it still carries.
    // Below this depth every path continues; at or past it a path survives with
    // probability equal to its throughput and is scaled up if it does.
    int rouletteDepth = 4;

    // A uniform surround, in cd/m^2. Zero is a black room.
    //
    // Not decoration. Most of this library is specular, and a mirror lit by
    // a point source shows a highlight of measure zero and nothing else --
    // which is the right answer and a black picture. An optic is looked at
    // in a room, and a uniform surround is the simplest room there is and
    // the one an appearance measurement is actually made in.
    //
    // It is reached only by sampling, never by a connection, so it cannot be
    // double counted against the analytic emitters.
    double environmentLuminance = 0.0;
    // A darker floor below the horizon, as a fraction of the surround. 1
    // makes it uniform; a real room is darker underfoot and an optic that
    // reflects a gradient reads as an object rather than as a silhouette.
    double environmentFloor = 0.35;
    // The horizon direction. Whatever is above this is sky.
    Vec3 environmentUp{0.0, 0.0, 1.0};

    // A latitude-longitude map of luminance, in place of the uniform surround
    // above. Null keeps the uniform one.
    //
    // A map is not a brighter version of the same thing: it is *directional*,
    // which is the whole reason to have one, and that is also why it is
    // connected to analytically at every scattering vertex rather than merely
    // being read where a ray escapes. A sun is a thousandth of the sphere
    // carrying most of the light; found only by a cosine-weighted draw it is
    // almost all noise. A uniform surround needs none of that -- a cosine draw
    // is already its perfect sampler -- so it keeps the sampled path it always
    // had, and every run without a map is unchanged to the bit.
    //
    // Shared rather than copied: a map is megabytes and every worker reads the
    // same one.
    std::shared_ptr<const envmap::EnvironmentMap> environmentMap;
    // What the map own numbers are multiplied by. Kept apart from the
    // calibration the file was loaded with, so a run can dim a room without
    // reloading it.
    double environmentMapScale = 1.0;

    std::uint64_t seed = 12345u;
    // 0 == every hardware thread.
    unsigned threads = 0;

    bool valid() const {
        return width > 0 && height > 0 && samplesPerPixel > 0 &&
               focalLengthMm > 0.0 && sensorWidthMm > 0.0;
    }
    double sensorHeightMm() const {
        return width > 0 ? sensorWidthMm * double(height) / double(width) : sensorWidthMm;
    }
};

// What the camera measured.
//
// One number per pixel, in candela per square metre, and the error bar that
// goes with it. Not an image with a gamma curve baked in: a tone mapping is a
// display decision and belongs where the picture is shown, not where the
// measurement is stored.
struct LuminanceImage {
    int width = 0, height = 0;

    // cd/m^2, row-major from the top-left of the image.
    std::vector<double> luminance;
    // Standard error of the mean, per pixel, from the spread of that pixel's
    // own samples. It is what makes a luminance figure quotable and what
    // backendcheck::compare sizes a disagreement in.
    std::vector<double> stdErr;
    // Camera rays that actually landed in each pixel.
    std::vector<std::uint32_t> samples;

    // Flux the path integrator dropped at the depth limit, as a fraction of what
    // it carried. Reported rather than hidden: a scene that truncates a percent
    // of its light is a scene whose answer is a percent low.
    double truncatedFraction = 0.0;
    double seconds = 0.0;

    // Peak, mean and the average of the log -- the three numbers a luminance
    // measurement is quoted with. The log mean is what a glare index is built
    // on, and it is not recoverable from the other two.
    double peak     = 0.0;
    double mean     = 0.0;
    double logMean  = 0.0;

    bool empty() const { return luminance.empty(); }
    double at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0.0;
        return luminance[std::size_t(y) * std::size_t(width) + std::size_t(x)];
    }
};

// Progress, partial results and cancellation, in the same shape TraceControl
// has them: the image forms while it runs rather than appearing at the end.
struct RenderControl {
    std::atomic<bool>* cancel = nullptr;
    std::function<void(std::size_t done, std::size_t total)> progress;
    std::function<void(const LuminanceImage& partial)> partial;
    int partialIntervalMs = 250;
};

// Renders `scene` from `cam` and fills `out`.
//
// `surfs` and `srcs` are what the forward tracer would have been given, so the
// two measure the same scene by construction -- which is the whole point, since
// the acceptance test for this is that a receiver and a camera pointed at the
// same diffuse box agree inside their error bars.
//
// `unit` says what the sources' powers are in. A run in lumens is photometric
// already and the answer is cd/m^2 directly; a run in watts is weighted by
// V(lambda) and scaled by 683 lm/W on the way out, so the output is cd/m^2
// either way and never an RGB triple pretending to be one.
void render(const TraceScene& scene, const std::vector<SceneSurface>& surfs,
            const std::vector<SourceConfig>& srcs, const CameraConfig& cam,
            const PhysicsOptions& physics, FluxUnit unit,
            LuminanceImage& out, const RenderControl& ctl = RenderControl{});

// A camera that frames the whole scene, three-quarter on and slightly above.
//
// A default view has to exist somewhere, and it belongs next to the camera
// rather than in whichever caller asked first: a headless render and the
// desktop one showing the same scene from the same place is the only way the
// two can be compared at all.
//
// The receivers are included in the framing on purpose. A luminaire pointed at
// a receiver two metres away is mostly empty space, and a view tight on the
// fixture would leave the thing it illuminates out of the picture.
CameraConfig defaultView(const TraceScene& scene, int width = 256, int height = 192);

// The luminance a Lambertian emitter of exitance M shows head-on: M / pi.
//
// Exposed because it is the closed form the whole chain is checked against, and
// a constant that only exists inside the integrator is a constant nobody can
// check it with.
inline double lambertianLuminance(double exitance) {
    return exitance / 3.14159265358979323846;
}

} // namespace backward
