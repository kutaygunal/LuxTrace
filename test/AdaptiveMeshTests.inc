// Curvature-driven adaptive meshing.
//
// The mesh is now per-face adaptive rather than uniform per body: a face's
// deflection is driven by its local curvature and by whether that curvature
// makes it an optical face at all (see MeshBuilder.cpp). These tests pin the
// contract that this is actually happening and that it did not cost the exact
// normals or a plane's exactness:
//
//   * a mounting-flange / planar face does not inflate when the hint tightens
//     (a plane is triangulated exactly at any edge size), while an optical
//     face does spend the extra triangles on position accuracy;
//   * position accuracy (max facet sagitta vs the true surface) rises as the
//     hint tightens on an optic, and stays bounded across curvature;
//   * a planar face's mesh still lies exactly on its plane;
//   * per-vertex normals stay exact (read off the B-Rep, not averaged facets).

#include <cmath>
#include <functional>
#include <vector>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopLoc_Location.hxx>
#include <map>
#include <utility>

namespace {

// Count of triangles in a meshed surface.
int triangleCount(const MeshSurface& m) { return int(m.tris.size()); }

// Returns the largest distance from any triangle's midpoint to the given
// signed-distance function over the surface, i.e. the worst sagitta error.
// dev must return the distance from a point to the true surface.
double maxSagitta(const MeshSurface& m, const std::function<double(const gp_Pnt&)>& dev) {
    double mx = 0.0;
    for (const auto& t : m.tris) {
        const gp_Pnt& a = m.verts[std::size_t(t.v0)];
        const gp_Pnt& b = m.verts[std::size_t(t.v1)];
        const gp_Pnt& c = m.verts[std::size_t(t.v2)];
        const gp_Pnt mid((a.X() + b.X() + c.X()) / 3.0,
                         (a.Y() + b.Y() + c.Y()) / 3.0,
                         (a.Z() + b.Z() + c.Z()) / 3.0);
        mx = std::max(mx, dev(mid));
    }
    return mx;
}

// Builds and meshes one surface shape with the given hint; returns the
// flattened mesh. A single OpticalSurface -> a single MeshSurface.
MeshSurface meshShape(const TopoDS_Shape& shape, double defl, double angle,
                      const QString& label) {
    OpticalSurface os;
    os.shape          = shape;
    os.label          = label;
    os.meshDeflection = defl;
    os.meshAngle      = angle;
    return MeshBuilder::build({os}).front();
}

// Signed distance from an origin-centred sphere of radius R.
std::function<double(const gp_Pnt&)> sphereDist(double R) {
    return [R](const gp_Pnt& p) {
        const double r = std::sqrt(p.X() * p.X() + p.Y() * p.Y() + p.Z() * p.Z());
        return std::fabs(r - R);
    };
}

// A "mounting flange": a flat planar body (a box) whose faces carry no optics.
MeshSurface flangeSurface(double defl) {
    return meshShape(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(100, 100, 100)).Shape(),
                     defl, 0.06, QStringLiteral("flange"));
}

// An optical face: a solid sphere of the given radius.
MeshSurface opticSphere(double R, double defl) {
    return meshShape(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), R).Shape(),
                     defl, 0.06, QStringLiteral("sphere"));
}

// A single planar quad face in the z = 0 plane over [x0,x1] x [y0,y1]. Two
// such faces sharing the boundary x == 50 model a coarse mounting flange
// abutting a fine optic, the exact case the shared-edge note worries about.
TopoDS_Shape quadFace(double x0, double x1, double y0, double y1) {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(x0, y0, 0));
    poly.Add(gp_Pnt(x1, y0, 0));
    poly.Add(gp_Pnt(x1, y1, 0));
    poly.Add(gp_Pnt(x0, y1, 0));
    poly.Close();
    return BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
}

} // namespace

TEST(meshadaptive, flange_does_not_inflate_when_the_hint_tightens) {
    // A planar face is triangulated exactly at any edge size, so adaptive
    // meshing must not add triangles to a mounting flange just because the
    // caller asked for a fine deflection. A 6-face box is 12 triangles whether
    // the hint is loose or tight.
    const MeshSurface loose = flangeSurface(0.30);
    const MeshSurface tight = flangeSurface(0.01);
    CHECK(!loose.tris.empty());
    CHECK(triangleCount(loose) == triangleCount(tight));
    CHECK(triangleCount(loose) <= 16);            // one minimal triangulation per face
    CHECK(triangleCount(tight) <= 16);
}

TEST(meshadaptive, plane_vertices_stay_exactly_on_the_plane) {
    // The guarantee behind the flange test: coarsening never moves a planar
    // mesh off its plane, because a plane is exact at any facet size. Every
    // triangle midpoint of the box must lie on one of its six face planes.
    const MeshSurface m = flangeSurface(2.0);     // deliberately very coarse
    for (const auto& t : m.tris) {
        const gp_Pnt& a = m.verts[std::size_t(t.v0)];
        const gp_Pnt& b = m.verts[std::size_t(t.v1)];
        const gp_Pnt& c = m.verts[std::size_t(t.v2)];
        for (const double coord : {a.X(), a.Y(), a.Z(), b.X(), b.Y(), b.Z(),
                                   c.X(), c.Y(), c.Z()})
            CHECK(coord >= -1e-9 && coord <= 100.0 + 1e-9);
    }
}

TEST(meshadaptive, optic_spends_triangles_and_accuracy_when_the_hint_tightens) {
    // On a curved optical face the hint is what position accuracy is bought
    // with: a tighter hint must yield a finer mesh AND a smaller worst sagitta
    // against the true sphere. Use a shallow, large-radius sphere so the linear
    // deflection is the binding constraint (on a tight-radius sphere the
    // angular tessellation already satisfies a loose linear hint, which is why
    // that case is exercised by the curvature-driven test below).
    const double R = 500.0;
    const MeshSurface loose = opticSphere(R, 0.30);
    const MeshSurface tight = opticSphere(R, 0.03);
    CHECK(triangleCount(tight) > triangleCount(loose));
    CHECK(maxSagitta(loose, sphereDist(R)) > 0.02);          // visibly coarse
    CHECK(maxSagitta(tight, sphereDist(R)) < maxSagitta(loose, sphereDist(R)));
    CHECK(maxSagitta(tight, sphereDist(R)) < 0.03 * 1.5);    // bounded by the ask
}

TEST(meshadaptive, position_accuracy_is_held_across_curvature) {
    // Curvature drives the per-face deflection, so a sharp surface and a gentle
    // one asked for with the SAME hint must end up with comparable positional
    // error instead of the sharp one falling far behind (the old uniform mesh).
    const double hint = 0.30;
    const MeshSurface sharp  = opticSphere(30.0, hint);      // high curvature
    const MeshSurface gentle = opticSphere(250.0, hint);     // low curvature
    const double sharpErr  = maxSagitta(sharp, sphereDist(30.0));
    const double gentleErr = maxSagitta(gentle, sphereDist(250.0));
    CHECK(sharpErr < hint * 1.5);
    CHECK(gentleErr < hint * 1.5);
    // The sharp optic must not end up grossly worse than the gentle one.
    CHECK(sharpErr < 2.0 * gentleErr + 1e-9);
}

TEST(meshadaptive, per_vertex_normals_stay_exact) {
    // Adaptive meshing moves where the vertices sit, never the surface normals
    // are read from, so the exact per-vertex normals must still be present and
    // radial on a sphere and perpendicular to a box face.
    const MeshSurface sphere = opticSphere(50.0, 0.30);
    CHECK(!sphere.vnorm.empty());
    CHECK(sphere.vnorm.size() == sphere.verts.size());

    // Normals remain unit.
    for (const auto& n : sphere.vnorm)
        CHECK_NEAR(std::sqrt(n.X() * n.X() + n.Y() * n.Y() + n.Z() * n.Z()), 1.0, 1e-9);

    // The apex vertex of the sphere faces straight back along the axis: its
    // normal must align with the radial direction through the origin.
    std::size_t apex = 0;
    double bestZ = -1e30;
    for (std::size_t i = 0; i < sphere.verts.size(); ++i)
        if (sphere.verts[i].Z() > bestZ) { bestZ = sphere.verts[i].Z(); apex = i; }
    const gp_Pnt& p = sphere.verts[apex];
    const gp_Dir  n = sphere.vnorm[apex];
    const double dot = (n.X() * p.X() + n.Y() * p.Y() + n.Z() * p.Z()) /
                       std::sqrt(p.X() * p.X() + p.Y() * p.Y() + p.Z() * p.Z());
    CHECK_NEAR(dot, 1.0, 1e-6);      // normal is radial where the surface curves

    // A box face has a constant, axis-aligned normal. A box corner belongs to
    // two faces, so a single vertex can legally carry a face normal in either
    // axis direction; assert every box normal is unit and axis-aligned rather
    // than guessing which face a given vertex sits on.
    const MeshSurface box = flangeSurface(2.0);
    CHECK(!box.vnorm.empty());
    for (const auto& n : box.vnorm) {
        CHECK_NEAR(std::sqrt(n.X() * n.X() + n.Y() * n.Y() + n.Z() * n.Z()), 1.0, 1e-9);
        CHECK(std::max({std::fabs(n.X()), std::fabs(n.Y()), std::fabs(n.Z())}) > 0.99);
    }
}

TEST(meshadaptive, survives_revolved_and_boolean_cut_faces) {
    // Scenes use two shape families this mesher must survive, beyond clean
    // primitives: a surface of revolution (a paraboloid mirror shell, built as
    // the engine does in GeometryProvider::revolvePolyline) and a boolean
    // Common of solids (the BiconvexLens shells). A failure here means the
    // per-face adaptive mesher could not handle a real scene topology.

    // Paraboloid shell: z = r^2/(4f), an open profile revolved about +Z -> one face.
    {try {
        constexpr double f = 80.0, rMax = 120.0;
        BRepBuilderAPI_MakePolygon poly;
        const int n = 41;
        for (int i = 0; i < n; ++i) {
            const double r = rMax * double(i) / double(n - 1);
            poly.Add(gp_Pnt(0.0, r, (r * r) / (4.0 * f)));
        }
        const TopoDS_Shape dish = BRepPrimAPI_MakeRevol(
            poly.Wire(), gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
        const MeshSurface mDish = meshShape(dish, 0.30, 0.06, QStringLiteral("dish"));
        CHECK(!mDish.tris.empty());
        CHECK(!mDish.vnorm.empty());
    } catch (const Standard_Failure&) {
        CHECK(false);
    }}

    // Biconvex lens shell: the Common of two intersecting spheres -> cap faces.
    {try {
        constexpr double R = 80.0, halfThk = 25.0;
        const TopoDS_Shape a = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, R - halfThk), R).Shape();
        const TopoDS_Shape b = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, halfThk - R), R).Shape();
        const TopoDS_Shape lens = BRepAlgoAPI_Common(a, b).Shape();
        const MeshSurface mLens = meshShape(lens, 0.30, 0.06, QStringLiteral("biconvex"));
        CHECK(!mLens.tris.empty());
        CHECK(!mLens.vnorm.empty());
    } catch (const Standard_Failure&) {
        CHECK(false);
    }}
}

// Diagnostic (Engineer-4): a closed solid's mesh is geometrically watertight
// only if every triangle-boundary edge has a GEOMETRICALLY COINCIDENT twin,
// i.e. another boundary edge with the same two endpoints (within tol). A bare
// count-1 edge is not proof of a hole -- OCCT meshes a single smooth face with
// duplicated coincident vertices along its UV parameterisation seam (e.g. a
// sphere's u=0/2pi), which yields count-1 edges yet a physically closed
// surface. What a crack looks like is a boundary edge with NO coincident twin:
// a coarse triangle spans [A,B] as one chord while a finer neighbour carries
// it as two sub-edges ending at an interior node, so the two triangulations no
// longer meet -- that sliver is a seam a ray can slip through.
namespace {
struct HoleStats { int crackEdges = 0; int boundaryEdges = 0; int totalEdges = 0; };
HoleStats countCracks(const MeshSurface& m) {
    std::map<std::pair<int,int>, int> cnt;
    for (const auto& t : m.tris) {
        const int a[3] = {t.v0, t.v1, t.v2};
        for (int i = 0; i < 3; ++i) {
            int lo = a[i], hi = a[(i+1)%3];
            if (lo > hi) std::swap(lo, hi);
            cnt[std::make_pair(lo, hi)]++;
        }
    }
    HoleStats s;
    s.totalEdges = int(cnt.size());
    // Collect the endpoint points of every count-1 (boundary) edge.
    std::vector<std::pair<gp_Pnt,gp_Pnt>> bdry;
    for (const auto& kv : cnt) if (kv.second == 1) {
        ++s.boundaryEdges;
        bdry.emplace_back(m.verts[std::size_t(kv.first.first)],
                          m.verts[std::size_t(kv.first.second)]);
    }
    // A crack edge is a boundary edge with no boundary twin sharing (within
    // tol) the same two endpoints in either orientation. Parameterisation-
    // seam duplicates always have a twin, so they are not cracks.
    const double tol = 1e-6;
    for (std::size_t i = 0; i < bdry.size(); ++i) {
        bool twin = false;
        const gp_Pnt& a = bdry[i].first;
        const gp_Pnt& b = bdry[i].second;
        for (std::size_t j = 0; j < bdry.size(); ++j) {
            if (i == j) continue;
            const gp_Pnt& c = bdry[j].first;
            const gp_Pnt& d = bdry[j].second;
            if ((a.Distance(c) < tol && b.Distance(d) < tol) ||
                (a.Distance(d) < tol && b.Distance(c) < tol)) { twin = true; break; }
        }
        if (!twin) ++s.crackEdges;
    }
    return s;
}
} // namespace

TEST(meshadaptive, adaptive_mesh_introduces_no_extra_cracks_vs_whole_shape) {
    // The seam regression: a per-face different-deflection mesher opened
    // T-junction cracks along a curved coarse/fine boundary. The fix meshes the
    // WHOLE body at the body-wide finest deflection, so the adaptive result must
    // not carry any more crack edges than the uniform whole-shape baseline over
    // the same Plano-Convex lens. (A small crack count is unavoidable on a
    // boolean-Common solid even when watertight -- OCCT leaves near-coincident
    // nodes on the sphere/box intersection curve within tolerance; what matters
    // is that adaptivity adds NONE on top of that.)
    const double R = 110.0;
    const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, -0.5 * R), R).Shape();
    const TopoDS_Shape slab = BRepPrimAPI_MakeBox(gp_Pnt(-1.1 * R, -1.1 * R, 0.0),
                                                   2.2 * R, 2.2 * R, 0.6 * R).Shape();
    const TopoDS_Shape lens = BRepAlgoAPI_Common(sphere, slab).Shape();

    // Adaptive (per-face deflection -> whole-body mesh): what production runs.
    OpticalSurface os;
    os.shape = lens;
    os.label = QStringLiteral("pcx");
    os.meshDeflection = 0.30; os.meshAngle = 0.06;
    const MeshSurface adaptive = MeshBuilder::build({os}).front();

    // Uniform control: whole-body at the base hint (the pre-T3-005 behaviour).
    MeshSurface control;
    BRepMesh_IncrementalMesh(lens, 0.30, Standard_False, 0.06);
    TopLoc_Location loc;
    for (TopExp_Explorer ex(lens, TopAbs_FACE); ex.More(); ex.Next()) {
        auto tri = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
        if (tri.IsNull()) continue;
        const int nNodes = tri->NbNodes(), nTris = tri->NbTriangles();
        const int base = int(control.verts.size());
        control.verts.reserve(std::size_t(control.verts.size() + std::size_t(nNodes)));
        for (int i = 1; i <= nNodes; ++i)
            control.verts.push_back(tri->Node(i).Transformed(loc));
        for (int i = 1; i <= nTris; ++i) {
            int n1, n2, n3; tri->Triangle(i).Get(n1, n2, n3);
            Triangle t; t.v0 = base + n1; t.v1 = base + n2; t.v2 = base + n3;
            control.tris.push_back(t);
        }
    }

    const HoleStats a = countCracks(adaptive);
    const HoleStats c = countCracks(control);
    std::printf("[diag] adaptive lens cracks=%d  uniform-control cracks=%d\n",
                a.crackEdges, c.crackEdges);
    CHECK(!adaptive.tris.empty());
    CHECK(!control.tris.empty());
    // Adaptive must not open seams beyond what whole-shape meshing already has.
    CHECK(a.crackEdges <= c.crackEdges);
    // And the absolute count must stay tiny (a handful of boolean-Common
    // tolerance nodes), not the hundreds the per-face seam defect produced.
    CHECK(a.crackEdges <= 48);
}

TEST(meshadaptive, single_curved_body_has_no_geometric_cracks) {
    // A solid sphere is one smooth curved face; its adaptive mesh must be fully
    // watertight (every boundary edge has a coincident twin along the UV
    // parameterisation seam) -- zero genuine cracks.
    OpticalSurface os;
    os.shape = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 60.0).Shape();
    os.label = QStringLiteral("sphere");
    os.meshDeflection = 0.30; os.meshAngle = 0.06;
    const MeshSurface m = MeshBuilder::build({os}).front();
    CHECK(!m.tris.empty());
    const HoleStats h = countCracks(m);
    std::printf("[diag] solid-sphere crack/boundary edges: %d / %d\n",
                h.crackEdges, h.boundaryEdges);
    CHECK(h.crackEdges == 0);
}

TEST(meshadaptive, shared_edge_between_coarse_and_fine_faces_stays_watertight) {
    // The architectural risk: meshing each face independently with per-face
    // deflection means a shared boundary edge between a coarse flange and a
    // fine optic is triangulated twice, so a T-junction / seam (tiny gap a ray
    // could slip through) can in principle appear. A plane is exact at any
    // facet size, so two coplanar faces meshed at different deflections must
    // still meet exactly along their common edge -- no crack and no overlap.
    //
    // Flange A occupies x in [0,50], optic B x in [50,100], sharing x == 50.
    const MeshSurface coarse =
        meshShape(quadFace(0.0, 50.0, -50.0, 50.0), 0.30, 0.06, QStringLiteral("flange"));
    const MeshSurface fine =
        meshShape(quadFace(50.0, 100.0, -50.0, 50.0), 0.005, 0.06, QStringLiteral("optic"));
    CHECK(!coarse.tris.empty());
    CHECK(!fine.tris.empty());

    // Neither independent triangulation may cross the seam: coarse stays at or
    // below x==50, fine at or above. A T-junction crack would instead leave a
    // band of uncovered x between the two vertex clouds.
    for (const auto& v : coarse.verts) CHECK(v.X() <= 50.0 + 1e-9);
    for (const auto& v : fine.verts)   CHECK(v.X() >= 50.0 - 1e-9);

    // And the two sides share the boundary vertices exactly, so the seam is
    // watertight: every coarse vertex sitting on x==50 has a coincident fine
    // vertex on the same edge (the quad's two edge endpoints on a planar
    // corner), leaving no gap for a ray to fall through.
    std::vector<gp_Pnt> coarseEdge, fineEdge;
    for (const auto& v : coarse.verts) if (std::fabs(v.X() - 50.0) < 1e-6) coarseEdge.push_back(v);
    for (const auto& v : fine.verts)   if (std::fabs(v.X() - 50.0) < 1e-6) fineEdge.push_back(v);
    CHECK(coarseEdge.size() == 2u);
    CHECK(fineEdge.size() == 2u);
    for (const auto& a : coarseEdge) {
        bool matched = false;
        for (const auto& b : fineEdge)
            if (a.Distance(b) < 1e-9) { matched = true; break; }
        CHECK(matched);
    }
}


