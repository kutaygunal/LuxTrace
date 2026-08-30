#include "RadianceMap.h"

#include "AppearanceEmitters.h"
#include "AppearanceScene.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>

namespace appearance {
namespace {

constexpr double kPi = 3.14159265358979323846;

// The most facets to build along one axis. 32 x 32 is 1 024 faces, which the
// plan calls ample and which costs nothing in a static render; past that the
// presentation count starts to matter and the extra detail is below what the
// bin noise supports anyway.
constexpr int kMaxResolution = 64;

gp_Pnt point(const Vec3& v) { return v.toPnt(); }

} // namespace

RadianceMap buildRadianceMap(const DetectorFrame& frame,
                             const std::vector<double>& bandFlux,
                             int resolution) {
    RadianceMap out;
    out.label = frame.label;

    const std::size_t cells = std::size_t(frame.nx) * std::size_t(frame.ny);
    if (frame.nx <= 0 || frame.ny <= 0 || frame.grid.size() < cells ||
        !(frame.w > 0.0) || !(frame.h > 0.0))
        return out;                                  // nothing binned, nothing to emit

    // How many bins each facet swallows. Never fewer than one: subdividing
    // finer than the receiver was binned would invent detail the trace did not
    // compute, and a render that shows structure the run did not produce is
    // worse than one that shows less.
    resolution = std::clamp(resolution, 1, kMaxResolution);
    const int stepX = std::max(1, (frame.nx + resolution - 1) / resolution);
    const int stepY = std::max(1, (frame.ny + resolution - 1) / resolution);
    out.nx = (frame.nx + stepX - 1) / stepX;
    out.ny = (frame.ny + stepY - 1) / stepY;

    const bool haveBands = bandFlux.size() >= 3 * cells;

    // Two passes: gather the flux and the colour per facet, then scale. The
    // divisor is a property of the whole map, as it is for the emitters, so a
    // dim corner of a bright plate stays dim instead of being normalised up to
    // meet it.
    struct Cell {
        double flux = 0.0;
        double band[3] = {0.0, 0.0, 0.0};
        int    i0 = 0, i1 = 0, j0 = 0, j1 = 0;
    };
    std::vector<Cell> cellsOut(std::size_t(out.nx) * std::size_t(out.ny));

    for (int fj = 0; fj < out.ny; ++fj) {
        for (int fi = 0; fi < out.nx; ++fi) {
            Cell& c = cellsOut[std::size_t(fj) * std::size_t(out.nx) + std::size_t(fi)];
            c.i0 = fi * stepX;
            c.i1 = std::min(frame.nx, c.i0 + stepX);
            c.j0 = fj * stepY;
            c.j1 = std::min(frame.ny, c.j0 + stepY);

            for (int j = c.j0; j < c.j1; ++j) {
                for (int i = c.i0; i < c.i1; ++i) {
                    const std::size_t k = std::size_t(j) * std::size_t(frame.nx) +
                                          std::size_t(i);
                    c.flux += frame.grid[k];
                    if (haveBands) {
                        c.band[0] += bandFlux[k];
                        c.band[1] += bandFlux[cells + k];
                        c.band[2] += bandFlux[2 * cells + k];
                    }
                }
            }
            out.totalFlux += c.flux;
        }
    }

    // The brightest facet's physical radiance. Computed from radiance rather
    // than from flux, because the facets at the edge of a grid that does not
    // divide evenly cover fewer bins and so have a smaller area: the brightest
    // *facet* and the brightest *patch of surface* are not the same thing.
    const double binW = frame.w / double(frame.nx);
    const double binH = frame.h / double(frame.ny);

    for (const Cell& c : cellsOut) {
        const double area = double(c.i1 - c.i0) * binW * double(c.j1 - c.j0) * binH;
        if (!(area > 0.0)) continue;
        out.peakRadiance = std::max(out.peakRadiance, c.flux / (area * kPi));
    }
    if (!(out.peakRadiance > 0.0)) return out;       // a run that reached nothing

    // The facet rectangles, in the receiver's own frame. Nudged a hair along
    // its normal so that showing the receiver as well does not produce a plane
    // fighting itself for the same pixels.
    const double span   = std::max(frame.w, frame.h);
    const Vec3   offset = frame.normal * (1e-4 * span);
    const Vec3   corner = frame.center + frame.u * (-0.5 * frame.w) +
                          frame.v * (-0.5 * frame.h) + offset;

    for (int fj = 0; fj < out.ny; ++fj) {
        for (int fi = 0; fi < out.nx; ++fi) {
            const Cell& c = cellsOut[std::size_t(fj) * std::size_t(out.nx) +
                                     std::size_t(fi)];

            const double u0 = double(c.i0) * binW, u1 = double(c.i1) * binW;
            const double v0 = double(c.j0) * binH, v1 = double(c.j1) * binH;
            const double area = (u1 - u0) * (v1 - v0);
            if (!(area > 0.0)) continue;

            BRepBuilderAPI_MakePolygon poly(
                point(corner + frame.u * u0 + frame.v * v0),
                point(corner + frame.u * u1 + frame.v * v0),
                point(corner + frame.u * u1 + frame.v * v1),
                point(corner + frame.u * u0 + frame.v * v1),
                Standard_True);
            if (!poly.IsDone()) continue;
            const TopoDS_Shape face = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
            if (face.IsNull()) continue;

            // Lambertian exit, the same relation the emitters use: what leaves
            // this patch per unit area per steradian, from the flux the trace
            // put on it.
            const double radiance = c.flux / (area * kPi);

            Graphic3d_Vec3 colour(1.0f, 1.0f, 1.0f);
            if (haveBands) {
                const double peak = std::max({c.band[0], c.band[1], c.band[2]});
                if (peak > 0.0)
                    colour = Graphic3d_Vec3(float(c.band[0] / peak), float(c.band[1] / peak),
                                            float(c.band[2] / peak));
            }

            RadianceFacet facet;
            facet.flux     = c.flux;
            facet.radiance = colour * float(kBrightestRadiance * radiance / out.peakRadiance);

            Material m;
            m.bsdf    = Graphic3d_BSDF::CreateDiffuse(Graphic3d_Vec3(0.0f));
            m.bsdf.Le = facet.radiance;
            m.colour  = Quantity_Color(std::min(1.0, double(colour.r())),
                                       std::min(1.0, double(colour.g())),
                                       std::min(1.0, double(colour.b())),
                                       Quantity_TOC_RGB);
            m.pbr.SetColor(m.colour);
            m.pbr.SetMetallic(0.0f);
            m.pbr.SetRoughness(1.0f);
            m.pbr.SetAlpha(1.0f);
            // The rasterized preview has no path-traced emission, so the same
            // distribution goes into the PBR emission term -- scaled by how
            // bright this facet is relative to the peak, or every facet would
            // read as equally lit in the fallback renderer.
            const float rel = float(radiance / out.peakRadiance);
            m.pbr.SetEmission(colour * rel);

            facet.shape = new AIS_Shape(face);
            applyMaterial(facet.shape, m);
            out.facets.push_back(facet);
        }
    }

    out.valid = !out.facets.empty();
    return out;
}

} // namespace appearance
