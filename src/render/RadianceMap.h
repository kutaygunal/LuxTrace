#pragma once
#include <vector>

#include <AIS_Shape.hxx>
#include <Graphic3d_Vec3.hxx>
#include <QString>

#include "core/SimulationResult.h"

// The trace, driving the render.
//
// Phases 1 to 3 produce a picture of the geometry lit by its own sources. This
// is the piece that makes the picture LuxTrace's rather than any CAD viewer's:
// a nominated exit surface -- a diffuser plate, a light-guide exit face, any
// receiver the scene already measures -- stops glowing uniformly and starts
// glowing with **the distribution the trace computed**. Change the source and
// the bright region moves, because the bright region is the answer.
//
// The constraint that shapes all of this: `Graphic3d_BSDF::Le` is one `Vec3`
// per material, and OCCT 7.8.1 has no texture-driven emission map on the
// path-traced BSDF. So the distribution has to become geometry. The exit face
// is subdivided into a grid of facets, each with its own `Le` taken from the
// bins it covers. The alternative -- patching OCCT's GLSL -- would fork the
// dependency for one feature, and is not worth it.
//
// No new physics anywhere below. Every number comes out of the
// `SimulationResult` grid the run already produced; this file rebins it and
// turns it into faces.
namespace appearance {

// One facet of the subdivided exit surface.
struct RadianceFacet {
    Handle(AIS_Shape) shape;
    // Emitted radiance, normalised and coloured, as it reaches OCCT.
    Graphic3d_Vec3 radiance{0.0f, 0.0f, 0.0f};
    // The flux this facet stands for, in the run's own unit. Physical, unlike
    // `radiance`: this is what the energy check adds up.
    double flux = 0.0;
};

struct RadianceMap {
    std::vector<RadianceFacet> facets;
    // The facet grid. Never finer than the receiver's own binning -- subdividing
    // past the data would invent detail the trace did not compute.
    int nx = 0, ny = 0;

    // Summed facet flux. Equal to the receiver grid's total by construction:
    // the facets partition the bins, each bin belonging to exactly one facet, so
    // this is a rebinning and not a resampling.
    double totalFlux = 0.0;
    // The physical radiance of the brightest facet, before normalisation -- the
    // divisor that was applied. In the run's flux unit per mm^2 per steradian.
    double peakRadiance = 0.0;

    // What the map was built from, for the caption that has to say so.
    QString label;
    bool    valid = false;
};

// Builds an emissive facet grid over `frame`, driven by the flux the run
// already binned onto it.
//
// `bandFlux` is the optional per-band grid from a spectral run -- band-major,
// 3 * nx * ny, red first, exactly as `SimulationResult::bandIrradiance` carries
// it -- which gives each facet its own colour, so a diffuser that is warm in
// the middle and cold at the edge renders that way. Empty means neutral.
//
// `resolution` is the target facet count per axis. The receiver's own bin count
// caps it, and each facet swallows a whole number of bins, so no bin is ever
// split between two facets -- which is what makes the energy check below an
// equality rather than an approximation.
RadianceMap buildRadianceMap(const DetectorFrame& frame,
                             const std::vector<double>& bandFlux = {},
                             int resolution = 32);

} // namespace appearance
