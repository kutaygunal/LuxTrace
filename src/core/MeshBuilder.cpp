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

namespace {

// How curved a face is, as 1/mm: the largest absolute mean curvature found over
// a coarse grid of UV samples. A plane stays ~0 everywhere; an asphere shows
// its shape at the first interior point, and sampling a handful keeps the
// estimate robust against a singular parameterisation (a sphere's pole, say)
// where curvature is undefined at a single point.
double characteristicCurvature(const TopoDS_Face& face) {
    BRepAdaptor_Surface ad(face);
    BRepLProp_SLProps props(ad, 2, 1e-9);      // 2nd derivatives -> curvature
    const double u0 = ad.FirstUParameter(), u1 = ad.LastUParameter();
    const double v0 = ad.FirstVParameter(), v1 = ad.LastVParameter();
    const double du = u1 - u0, dv = v1 - v0;
    const int n = 5;
    double max = 0.0;
    for (int i = 0; i < n; ++i) {
        const double fu = (n > 1) ? double(i) / double(n - 1) : 0.5;
        for (int j = 0; j < n; ++j) {
            const double fv = (n > 1) ? double(j) / double(n - 1) : 0.5;
            props.SetParameters(u0 + fu * du, v0 + fv * dv);
            if (!props.IsCurvatureDefined()) continue;
            max = std::max(max, std::fabs(props.MeanCurvature()));
        }
    }
    return max;
}

// The linear deflection, in mm, a face gets from its curvature `k` and the
// caller's per-body hint `base`.
//
// A face that is (effectively) flat is a mechanical face -- a mounting seat, a
// planar back, a box side. It carries no optics, so there is no reason to
// resolve it beyond a couple of triangles: coarsening it is free because a
// plane is triangulated exactly at any edge size. Everything else is an optical
// face: keep the caller's hint, then tighten it as the surface sharpens past a
// reference curvature, because the facet chord that holds a given sagitta error
// `e` on curvature `k` goes as 1/sqrt(k) (e = k*s^2/8), so a sharper surface
// needs a finer tessellation to keep the same positional error.
constexpr double kMechanicalCurvature = 1e-3;   // 1/mm; gentler than this -> flange
constexpr double kMechanicalCoarsen    = 8.0;   // flange faces get this many fewer facets
constexpr double kReferenceCurvature   = 0.02;  // at this the caller's hint applies as-is
constexpr double kMinDeflection        = 1e-3;  // never ask the mesher for the impossible

double faceDeflection(double curvature, double base) {
    if (std::fabs(curvature) < kMechanicalCurvature)
        return std::max(base * kMechanicalCoarsen, kMinDeflection);
    const double strength =
        std::clamp(std::fabs(curvature) / kReferenceCurvature, 1.0, 64.0);
    return std::max(base / std::sqrt(strength), kMinDeflection);
}

// The angular deflection for a face. Optical faces keep the caller's value (the
// per-vertex normals are exact, but the angular term still decides how many
// chords ring a surface of revolution). A mechanical face gets a much looser
// angle -- the second lever that keeps a flange down to a handful of triangles.
double faceAngle(double curvature, double baseAngle) {
    if (std::fabs(curvature) < kMechanicalCurvature)
        return std::max(baseAngle * kMechanicalCoarsen, 0.5);
    return baseAngle;
}

} // namespace

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
    // The mesh used to be uniform per body: one deflection for the whole shape,
    // whether a face was an optically active asphere or a mounting flange. T3-005
    // made it adaptive (curvature-driven per face). An early per-face attempt
    // meshed every face on its own, which CREATED a seam defect the Verifier
    // caught: two adjacent faces meshed at different deflections are triangulated
    // twice, and unless they agree on exactly where the nodes sit along their
    // shared curved boundary the two triangulations stop meeting -- a thin
    // T-junction crack a ray slips through, which is exactly how a lens starts
    // leaking fluxEscaped. OCCT's BRepMesh keeps a shared edge watertight only
    // when the whole shape is meshed as one unit.
    //
    // So the deflection is decided per face (curvature-driven, see
    // faceDeflection / faceAngle) but APPLIED to the whole body at the finest
    // face's value, in one BRepMesh_IncrementalMesh call. That keeps shared
    // edges consistent (no crack / T-junction) while adaptivity is still real
    // where it is safe:
    //
    //   * an all-planar mechanical body sheds triangles for no accuracy cost,
    //     because a plane is triangulated exactly at any edge size;
    //   * an optically curved face keeps the caller's tolerance and tightens it
    //     as it sharpens, because holding a sagitta error on a curved surface
    //     needs a finer chord;
    //   * a mixed body (optic + mounting flange) is meshed at the optic's finer
    //     deflection, which spends a few triangles on the flange but keeps the
    //     seam closed -- worth far more than the triangle.
    //
    // The per-vertex exact normals below are unchanged: adaptivity moves where
    // the vertices sit, never the surface they are read back from.
    double bodyDefl  = os.meshDeflection;
    double bodyAngle = os.meshAngle;
    for (TopExp_Explorer ex(os.shape, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face face = TopoDS::Face(ex.Current());
        double curvature = 0.0;
        bool measurable   = true;
        try {
            curvature = characteristicCurvature(face);
        } catch (const Standard_Failure&) {
            measurable = false;
        }
        // A face whose curvature cannot be evaluated falls back to the caller's
        // hint unchanged, rather than being over- or under-refined blindly.
        const double defl =
            measurable ? faceDeflection(curvature, os.meshDeflection) : os.meshDeflection;
        const double angle =
            measurable ? faceAngle(curvature, os.meshAngle) : os.meshAngle;
        bodyDefl  = std::min(bodyDefl, defl);
        bodyAngle = std::min(bodyAngle, angle);
    }
    try {
        BRepMesh_IncrementalMesh(os.shape, bodyDefl, Standard_False, bodyAngle);
    } catch (const Standard_Failure&) {
        // A body that cannot be meshed at all is skipped; a missing mesh is
        // never worth aborting the whole build.
        return;
    }

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
