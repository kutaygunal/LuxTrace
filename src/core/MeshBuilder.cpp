#include "MeshBuilder.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopLoc_Location.hxx>
#include <Poly_Triangulation.hxx>

#include <algorithm>

#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>

MeshList MeshBuilder::build(const std::vector<OpticalSurface>& surfaces) {
    MeshList out;
    out.reserve(surfaces.size());
    for (const auto& os : surfaces) {
        MeshSurface m;
        static_cast<SurfaceOptics&>(m) = static_cast<const SurfaceOptics&>(os);
        m.label = os.label;
        meshShape(os, m);
        out.push_back(std::move(m));
    }
    return out;
}

void MeshBuilder::meshShape(const OpticalSurface& os, MeshSurface& out) {
    // The angular deflection is the term that matters optically: it bounds how
    // far a facet normal can sit from the true surface normal, and a mirror
    // turns that into twice as much error in the reflected ray. Tightening it
    // from OCCT's usual 0.5 rad is what lets an imaging scene actually image.
    BRepMesh_IncrementalMesh(os.shape, os.meshDeflection, Standard_False, os.meshAngle);

    int base = 0;
    for (TopExp_Explorer ex(os.shape, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;

        base = int(out.verts.size());
        const gp_Trsf tf = loc.Transformation();
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            out.verts.push_back(tri->Node(i).Transformed(tf));
        }

        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a = 0, b = 0, c = 0;
            tri->Triangle(i).Get(a, b, c);  // 1-based local node indices
            const gp_Pnt& A = out.verts[base + a - 1];
            const gp_Pnt& B = out.verts[base + b - 1];
            const gp_Pnt& C = out.verts[base + c - 1];
            gp_Vec n = (gp_Vec(A, B)) ^ (gp_Vec(A, C));
            if (n.Magnitude() < 1e-12)
                continue;
            Triangle t;
            t.v0 = base + a - 1;
            t.v1 = base + b - 1;
            t.v2 = base + c - 1;
            try { t.normal = gp_Dir(n); } catch (...) { continue; }
            out.tris.push_back(t);
        }
    }

    // Detector geometry: the receiver is assumed planar and axis-aligned, so
    // its frame can be read straight off the vertex extents. The centre is the
    // true middle of that rectangle (not the origin), which is what the tracer
    // bins against -- an off-axis receiver therefore bins correctly.
    if (out.isDetector && !out.verts.empty()) {
        double xmin = 1e18, xmax = -1e18, ymin = 1e18, ymax = -1e18;
        double z = 0;
        for (const auto& p : out.verts) {
            xmin = std::min(xmin, p.X()); xmax = std::max(xmax, p.X());
            ymin = std::min(ymin, p.Y()); ymax = std::max(ymax, p.Y());
            z += p.Z();
        }
        z /= double(out.verts.size());
        out.detCenter = gp_Pnt(0.5 * (xmin + xmax), 0.5 * (ymin + ymax), z);
        out.detW = xmax - xmin;
        out.detH = ymax - ymin;
        out.detNX = 64;
        out.detNY = 64;
    }
}
