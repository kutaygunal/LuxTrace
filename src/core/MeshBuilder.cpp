#include "MeshBuilder.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopLoc_Location.hxx>
#include <Poly_Triangulation.hxx>

#include <algorithm>
#include <vector>

#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>
#include <cmath>

MeshList MeshBuilder::build(const std::vector<OpticalSurface>& surfaces) {
    MeshList out;
    out.reserve(surfaces.size());
    for (const auto& os : surfaces) {
        MeshSurface m;
        static_cast<SurfaceOptics&>(m) = static_cast<const SurfaceOptics&>(os);
        m.label = os.label;
        m.placements = os.placements;
        meshShape(os, m);
        out.push_back(std::move(m));
    }
    return out;
}

void MeshBuilder::meshShape(const OpticalSurface& os, MeshSurface& out) {
    // The angular deflection bounds how far a facet sits from the true surface
    // in position. It used to bound the normal error too, which is what capped
    // every imaging scene; per-vertex normals read off the exact surface remove
    // that term, so this is now a positional tolerance only.
    BRepMesh_IncrementalMesh(os.shape, os.meshDeflection, Standard_False, os.meshAngle);

    for (TopExp_Explorer ex(os.shape, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;

        // A face carried REVERSED in its shape has its triangles wound the
        // other way round, so its facet normals point *into* the solid. Nothing
        // used to check, and the tracer papered over it by flipping the normal
        // toward the incident ray on every hit -- which works for one surface
        // and makes medium tracking impossible. Flipping the winding here is
        // what makes "d . n > 0" genuinely mean "leaving".
        const bool reversed = (face.Orientation() == TopAbs_REVERSED);

        const int base   = int(out.verts.size());
        const int nNodes = tri->NbNodes();
        const gp_Trsf tf = loc.Transformation();
        for (int i = 1; i <= nNodes; ++i) {
            out.verts.push_back(tri->Node(i).Transformed(tf));
        }
        out.vnorm.resize(out.verts.size(), gp_Dir(0, 0, 1));

        // Exact surface normals at the tessellation's own UV parameters. This is
        // strictly better than averaging facet normals: it is the normal of the
        // optic, not of the approximation to it.
        std::vector<char> exact(std::size_t(nNodes), 0);
        if (tri->HasUVNodes()) {
            try {
                BRepAdaptor_Surface ad(face);
                BRepLProp_SLProps props(ad, 1, 1e-9);
                for (int i = 1; i <= nNodes; ++i) {
                    const gp_Pnt2d uv = tri->UVNode(i);
                    props.SetParameters(uv.X(), uv.Y());
                    if (!props.IsNormalDefined())
                        continue;
                    gp_Dir n = props.Normal();
                    if (reversed) n.Reverse();
                    out.vnorm[std::size_t(base + i - 1)] = n;
                    exact[std::size_t(i - 1)] = 1;
                }
            } catch (const Standard_Failure&) {
                // A face whose surface cannot be evaluated keeps the facet
                // fallback below; a normal is never worth an aborted build.
            }
        }

        // Facet normals, accumulated for the nodes the exact evaluation could
        // not reach. The cross product is not normalised, so the sum is area
        // weighted, which is the right average across a fan of unequal facets.
        std::vector<gp_Vec> accum(std::size_t(nNodes), gp_Vec(0, 0, 0));

        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a = 0, b = 0, c = 0;
            tri->Triangle(i).Get(a, b, c);  // 1-based local node indices
            if (reversed) std::swap(b, c);
            const gp_Pnt& A = out.verts[std::size_t(base + a - 1)];
            const gp_Pnt& B = out.verts[std::size_t(base + b - 1)];
            const gp_Pnt& C = out.verts[std::size_t(base + c - 1)];
            gp_Vec n = (gp_Vec(A, B)) ^ (gp_Vec(A, C));
            if (n.Magnitude() < 1e-12)
                continue;
            Triangle t;
            t.v0 = base + a - 1;
            t.v1 = base + b - 1;
            t.v2 = base + c - 1;
            try { t.normal = gp_Dir(n); } catch (...) { continue; }
            out.tris.push_back(t);
            accum[std::size_t(a - 1)] += n;
            accum[std::size_t(b - 1)] += n;
            accum[std::size_t(c - 1)] += n;
        }

        for (int i = 0; i < nNodes; ++i) {
            if (exact[std::size_t(i)]) continue;
            const gp_Vec& v = accum[std::size_t(i)];
            if (v.Magnitude() < 1e-12) continue;
            out.vnorm[std::size_t(base + i)] = gp_Dir(v);
        }
    }

    // Detector geometry. The frame is derived from the receiver's own plane
    // rather than assumed to be +Z, so a tilted or off-axis receiver bins
    // against itself. A planar receiver in the z = const plane comes out exactly
    // as it always did: normal +Z, u along +X, v along +Y.
    // A receiver is never an instanced part -- it is one surface with one
    // frame -- so the frame is read straight off the vertices.
    if (out.isDetector && !out.verts.empty())
        buildDetectorFrame(out);
}

void MeshBuilder::buildDetectorFrame(MeshSurface& out) {
    // Area-weighted mean of the facet normals: robust to one degenerate sliver
    // in a way that picking the first triangle's normal is not.
    gp_Vec acc(0, 0, 0);
    for (const auto& t : out.tris) {
        const gp_Pnt& A = out.verts[std::size_t(t.v0)];
        const gp_Pnt& B = out.verts[std::size_t(t.v1)];
        const gp_Pnt& C = out.verts[std::size_t(t.v2)];
        acc += (gp_Vec(A, B)) ^ (gp_Vec(A, C));
    }
    gp_Dir n(0, 0, 1);
    if (acc.Magnitude() > 1e-12) {
        try { n = gp_Dir(acc); } catch (...) { n = gp_Dir(0, 0, 1); }
    }
    // A receiver facing -Z is the same plane as one facing +Z; keep the sense
    // that makes the frame right-handed with a +Z-ish normal, so the reported
    // grid orientation does not flip on a winding detail.
    if (n.Z() < -1e-12 || (std::fabs(n.Z()) < 1e-12 && n.X() + n.Y() < 0.0)) n.Reverse();

    // In-plane axes. Aligning u with +X where the plane allows it keeps the
    // familiar case familiar: an axis-aligned receiver reports x and y.
    gp_Vec ref = (std::fabs(n.Z()) > 0.9) ? gp_Vec(1, 0, 0) : gp_Vec(0, 0, 1);
    gp_Vec uv  = ref - gp_Vec(n) * (ref * gp_Vec(n));
    if (uv.Magnitude() < 1e-9) uv = gp_Vec(0, 1, 0);
    gp_Dir u(uv);
    gp_Dir v(gp_Vec(n) ^ gp_Vec(u));

    // Extents and centre in that frame.
    double umin = 1e18, umax = -1e18, vmin = 1e18, vmax = -1e18, dsum = 0.0;
    for (const auto& p : out.verts) {
        const gp_Vec r(p.X(), p.Y(), p.Z());
        const double a = r * gp_Vec(u), b = r * gp_Vec(v);
        umin = std::min(umin, a); umax = std::max(umax, a);
        vmin = std::min(vmin, b); vmax = std::max(vmax, b);
        dsum += r * gp_Vec(n);
    }
    const double d = dsum / double(out.verts.size());
    const gp_Vec c = gp_Vec(u) * (0.5 * (umin + umax)) +
                     gp_Vec(v) * (0.5 * (vmin + vmax)) +
                     gp_Vec(n) * d;

    out.detCenter = gp_Pnt(c.X(), c.Y(), c.Z());
    out.detU      = u;
    out.detV      = v;
    out.detNormal = n;
    out.detW      = umax - umin;
    out.detH      = vmax - vmin;
    if (out.detNX <= 0) out.detNX = kDefaultDetectorBins;
    if (out.detNY <= 0) out.detNY = kDefaultDetectorBins;
}
