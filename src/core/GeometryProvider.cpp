#include "GeometryProvider.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Builder.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Plane.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Optical property presets, so a scene reads as intent rather than numbers.
constexpr double kMirrorR = 0.95;    // good front-surface mirror
constexpr double kGlassN  = 1.5;     // BK7-ish
constexpr double kGlassT  = 0.96;    // the fixed split, used only when Fresnel is off
constexpr double kGlassR  = 0.04;
// Cauchy B for a crown glass of Abbe number ~64: n_F - n_C = B * 1.91 um^-2.
constexpr double kGlassB  = 0.00420;
// Internal attenuation, 1/mm. 0.998 transmittance per 10 mm, which is ordinary
// for optical glass -- and enough to matter over the metre of zig-zag a light
// guide actually makes a ray travel.
constexpr double kGlassA  = 2.0e-4;

OpticalSurface surf(TopoDS_Shape s, const QString& label,
                    double refl, double trans, double index, bool det = false) {
    OpticalSurface o;
    o.shape = s;
    o.label = label;
    o.reflectivity = refl;
    o.transmissivity = trans;
    o.index = index;
    o.isDetector = det;
    return o;
}

OpticalSurface mirror(TopoDS_Shape s, const QString& label, double refl = kMirrorR) {
    return surf(s, label, refl, 0.0, 0.0);
}

// A refractive solid. `guide` surfaces describe a pure light pipe: when the
// Fresnel model is switched off they fall back to R = 0 / T = 1 rather than to
// a 4 % surface reflection. With Fresnel on -- the default -- the split comes
// from the angle of incidence either way, so the flag only sets the fallback.
OpticalSurface glass(TopoDS_Shape s, const QString& label, bool guide = false,
                     double index = kGlassN) {
    OpticalSurface o = guide ? surf(s, label, 0.0, 1.0, index)
                             : surf(s, label, kGlassR, kGlassT, index);
    o.fresnel     = true;
    o.dispersionB = kGlassB;
    o.absorption  = kGlassA;
    return o;
}

// A matte white surface: high reflectance, all of it Lambertian. This is the
// paint inside an integrating sphere and the wall of a luminaire cavity.
OpticalSurface diffuseWhite(TopoDS_Shape s, const QString& label, double refl) {
    OpticalSurface o = surf(s, label, std::clamp(refl, 0.0, 1.0), 0.0, 0.0);
    o.scatter = 1.0;
    return o;
}

// ---- profile helpers -------------------------------------------------------
// Profiles are given in the plane x = 0 as (0, r, z) and revolved about +Z.

TopoDS_Shape revolveSpline(const std::vector<gp_Pnt>& profile) {
    TColgp_Array1OfPnt pts(1, int(profile.size()));
    for (int i = 0; i < int(profile.size()); ++i) pts.SetValue(i + 1, profile[std::size_t(i)]);
    Handle(Geom_BSplineCurve) curve = GeomAPI_PointsToBSpline(pts).Curve();
    BRepBuilderAPI_MakeEdge edge(curve);
    return BRepPrimAPI_MakeRevol(edge.Edge(), gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
}

// Open polyline -> shell of revolution (a mirror surface).
TopoDS_Shape revolvePolyline(const std::vector<gp_Pnt>& profile) {
    BRepBuilderAPI_MakePolygon poly;
    for (const auto& p : profile) poly.Add(p);
    return BRepPrimAPI_MakeRevol(poly.Wire(), gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
}

// Closed polyline -> solid of revolution (a refractive body).
TopoDS_Shape revolveSolid(const std::vector<gp_Pnt>& profile) {
    BRepBuilderAPI_MakePolygon poly;
    for (const auto& p : profile) poly.Add(p);
    poly.Close();
    BRepBuilderAPI_MakeFace face(poly.Wire());
    return BRepPrimAPI_MakeRevol(face.Face(), gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
}

// Profile given in the x-z plane, extruded along y and centred on y = 0.
// `solid` closes the profile into a face first; otherwise the result is a shell.
TopoDS_Shape extrudeAlongY(const std::vector<gp_Pnt>& profileXZ, double length, bool solid) {
    BRepBuilderAPI_MakePolygon poly;
    for (const auto& p : profileXZ) poly.Add(gp_Pnt(p.X(), -0.5 * length, p.Z()));
    if (!solid) return BRepPrimAPI_MakePrism(poly.Wire(), gp_Vec(0, length, 0)).Shape();
    poly.Close();
    BRepBuilderAPI_MakeFace face(poly.Wire());
    return BRepPrimAPI_MakePrism(face.Face(), gp_Vec(0, length, 0)).Shape();
}

// Planar square receiver perpendicular to +Z, centred on (cx, cy, cz).
TopoDS_Shape makeDetectorAt(double size, double cx, double cy, double cz) {
    const double h = size * 0.5;
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(cx - h, cy - h, cz));
    poly.Add(gp_Pnt(cx + h, cy - h, cz));
    poly.Add(gp_Pnt(cx + h, cy + h, cz));
    poly.Add(gp_Pnt(cx - h, cy + h, cz));
    poly.Close();
    Handle(Geom_Plane) plane = new Geom_Plane(gp_Ax2(gp_Pnt(cx, cy, cz), gp_Dir(0, 0, 1)));
    BRepBuilderAPI_MakeFace face(plane, poly.Wire(), true);
    return face.Shape();
}

OpticalSurface detector(double size, double z, double cx = 0.0, double cy = 0.0) {
    return surf(makeDetectorAt(size, cx, cy, z), QStringLiteral("Detector"),
                0.0, 0.0, 0.0, true);
}

// ---- individual optics -----------------------------------------------------

// Paraboloid of revolution z = r^2 / (4p), sampled between two radii.
TopoDS_Shape paraboloid(double p, double rInner, double rOuter, int n = 40) {
    std::vector<gp_Pnt> profile;
    profile.reserve(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        const double r = rOuter + (rInner - rOuter) * double(i) / double(n - 1);
        profile.emplace_back(0.0, r, (r * r) / (4.0 * p));
    }
    return revolveSpline(profile);
}

// Prolate spheroid cap: r(z) = b sqrt(1 - z^2/a^2), foci at z = +/- sqrt(a^2-b^2).
TopoDS_Shape ellipsoidCap(double a, double b, double zFrom, double zTo, int n = 40) {
    std::vector<gp_Pnt> profile;
    profile.reserve(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        const double z = zFrom + (zTo - zFrom) * double(i) / double(n - 1);
        const double r = b * std::sqrt(std::max(0.0, 1.0 - (z * z) / (a * a)));
        profile.emplace_back(0.0, r, z);
    }
    return revolveSpline(profile);
}

// Spherical cap of radius R with its vertex at the origin, opening toward +Z.
// `sign` = +1 makes it concave toward +Z (a dish), -1 convex.
TopoDS_Shape sphericalCap(double R, double halfAngleDeg, double zVertex, double sign,
                          int n = 32) {
    std::vector<gp_Pnt> profile;
    profile.reserve(std::size_t(n));
    const double maxT = halfAngleDeg * kPi / 180.0;
    for (int i = 0; i < n; ++i) {
        const double t = maxT * double(i) / double(n - 1);
        profile.emplace_back(0.0, R * std::sin(t), zVertex + sign * R * (1.0 - std::cos(t)));
    }
    return revolveSpline(profile);
}

// Rotationally symmetric compound parabolic concentrator.
// Exit radius `aExit` at z = 0, acceptance half-angle `thetaDeg`; the profile is
// a parabola tilted by the acceptance angle with its focus on the exit rim.
std::vector<gp_Pnt> cpcPoints(double aExit, double thetaDeg, int n = 44) {
    const double th = thetaDeg * kPi / 180.0;
    const double f  = aExit * (1.0 + std::sin(th));
    std::vector<gp_Pnt> profile;
    profile.reserve(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        const double phi = 2.0 * th + (0.5 * kPi + th - 2.0 * th) * double(i) / double(n - 1);
        const double rho = 2.0 * f / (1.0 - std::cos(phi));
        profile.emplace_back(0.0, rho * std::sin(phi - th) - aExit, rho * std::cos(phi - th));
    }
    return profile;
}

// A stepped Fresnel plate: `zones` annular facets over a flat base.
TopoDS_Shape fresnelPlate(double radius, double baseThickness, double facetHeight,
                          int zones) {
    std::vector<gp_Pnt> profile;
    profile.emplace_back(0.0, 0.0, 0.0);                       // on the axis, back face
    profile.emplace_back(0.0, 0.0, baseThickness + facetHeight);
    for (int k = 0; k < zones; ++k) {
        const double rOut = radius * double(k + 1) / double(zones);
        profile.emplace_back(0.0, rOut, baseThickness);        // slope down to the step
        if (k + 1 < zones)
            profile.emplace_back(0.0, rOut, baseThickness + facetHeight);  // vertical riser
    }
    profile.emplace_back(0.0, radius, 0.0);                    // outer edge, back face
    return revolveSolid(profile);
}

// Three mutually perpendicular square mirrors meeting at the origin.
TopoDS_Shape cornerCube(double side) {
    const double s = side;
    auto plate = [](gp_Pnt a, gp_Pnt b, gp_Pnt c, gp_Pnt d) {
        BRepBuilderAPI_MakePolygon poly;
        poly.Add(a); poly.Add(b); poly.Add(c); poly.Add(d);
        poly.Close();
        return BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    };
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, plate(gp_Pnt(0, 0, 0), gp_Pnt(s, 0, 0), gp_Pnt(s, s, 0), gp_Pnt(0, s, 0)));
    builder.Add(compound, plate(gp_Pnt(0, 0, 0), gp_Pnt(0, s, 0), gp_Pnt(0, s, s), gp_Pnt(0, 0, s)));
    builder.Add(compound, plate(gp_Pnt(0, 0, 0), gp_Pnt(s, 0, 0), gp_Pnt(s, 0, s), gp_Pnt(0, 0, s)));
    return compound;
}

TopoDS_Shape microlensArray(int nx, int ny, double radius, double pitch) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const double x0 = -0.5 * pitch * (nx - 1);
    const double y0 = -0.5 * pitch * (ny - 1);
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j)
            builder.Add(compound, BRepPrimAPI_MakeSphere(
                gp_Pnt(x0 + i * pitch, y0 + j * pitch, 0.0), radius).Shape());
    return compound;
}

// ---- the scene registry ----------------------------------------------------

using Scene = GeometryProvider::Scene;
using SceneInfo = GeometryProvider::SceneInfo;

const std::array<SceneInfo, std::size_t(Scene::Count)>& registry() {
    static const std::array<SceneInfo, std::size_t(Scene::Count)> table = {{
        // --- reflective ---
        {QStringLiteral("Parabolic Reflector"),
         QStringLiteral("Source at the focus of a paraboloid; the mirror collimates it into a parallel beam."),
         gp_Pnt(0, 0, 100.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Elliptical Reflector"),
         QStringLiteral("Ellipsoid mirror: light from one focus is imaged onto the other."),
         gp_Pnt(0, 0, -160.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Spherical Reflector"),
         QStringLiteral("Spherical mirror with the source at the paraxial focus; shows spherical aberration against the parabola."),
         gp_Pnt(0, 0, 90.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Off-Axis Parabola"),
         QStringLiteral("An off-axis segment of a paraboloid, with the receiver offset to match. Exercises off-axis detector binning."),
         gp_Pnt(0, 0, 100.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Parabolic Trough"),
         QStringLiteral("Extruded parabola: collimates in x only, so the receiver sees a line rather than a spot."),
         gp_Pnt(0, 0, 60.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Compound Parabolic Concentrator"),
         QStringLiteral("Nonimaging concentrator: rays inside the acceptance angle reach the exit, the rest are rejected."),
         gp_Pnt(0, 0, 170.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Conical Concentrator"),
         QStringLiteral("Reflective funnel. Simpler than a CPC and measurably worse at the same job."),
         gp_Pnt(0, 0, 170.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Cassegrain (two-mirror)"),
         QStringLiteral("Folded system: light passes the central hole, reflects off a convex secondary onto the annular primary, then out. The secondary obstructs the axis, so the beam is annular."),
         gp_Pnt(0, 0, -10.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Corner-Cube Retroreflector"),
         QStringLiteral("Three orthogonal mirrors send light back the way it came, whatever the angle of arrival."),
         gp_Pnt(50.0, 50.0, 150.0), gp_Dir(0, 0, -1)},

        // --- refractive ---
        {QStringLiteral("Ball Lens (refraction)"),
         QStringLiteral("Solid glass sphere. Strong focusing, heavy aberration, and a large share of the light lost."),
         gp_Pnt(0, 0, -180.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Plano-Convex Lens"),
         QStringLiteral("Flat back, spherical front, with the source at the front focus so the lens collimates."),
         gp_Pnt(0, 0, -240.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Biconvex Lens"),
         QStringLiteral("Two spherical surfaces; roughly twice the power of the plano-convex lens at the same radius."),
         gp_Pnt(0, 0, -180.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Plano-Concave Lens (diverging)"),
         QStringLiteral("Negative power: the beam spreads instead of focusing, so the receiver sees a broad wash."),
         gp_Pnt(0, 0, -200.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Half-Ball Lens (LED dome)"),
         QStringLiteral("Hemisphere sitting on the emitter. Rays meet the dome near normal incidence, so most escape instead of being trapped by total internal reflection."),
         gp_Pnt(0, 0, -5.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Cylindrical Rod Lens"),
         QStringLiteral("Glass rod along y: it has power in x only, so a point source images to a line rather than a spot."),
         gp_Pnt(0, 0, -240.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Fresnel Lens (stepped)"),
         QStringLiteral("Annular facets on a thin plate: most of the deflection of a thick lens with little of the glass."),
         gp_Pnt(0, 0, -150.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Axicon (ring former)"),
         QStringLiteral("A paraboloid collimates the source, then a glass cone deflects every ray radially outward: the receiver sees a ring with a dark centre."),
         gp_Pnt(0, 0, 40.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Microlens Array (5 x 5)"),
         QStringLiteral("A grid of small ball lenses; light between the lenslets passes straight through."),
         gp_Pnt(0, 0, -120.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Glass Prism"),
         QStringLiteral("A prism bends light in one plane only: the pattern is squeezed along x while spreading freely along y. Trace it in RGB to see the dispersion split the beam."),
         gp_Pnt(0, 0, -50.0), gp_Dir(0, 0, 1)},

        // --- total internal reflection ---
        {QStringLiteral("Light Guide (TIR)"),
         QStringLiteral("Round glass rod. Rays beyond the critical angle bounce along it to the far end, paying Fresnel loss at the ends and Beer-Lambert loss along the way."),
         gp_Pnt(0, 0, -6.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Square Light Guide (TIR)"),
         QStringLiteral("Square cross-section rod: the flat walls mix the beam differently from a round one."),
         gp_Pnt(0, 0, -6.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Tapered Light Guide"),
         QStringLiteral("A taper trades area for angle. Each bounce steepens the ray, so some break the TIR condition and leak out."),
         gp_Pnt(0, 0, -6.0), gp_Dir(0, 0, 1)},
        {QStringLiteral("Porro Prism (TIR retro)"),
         QStringLiteral("Two 45-degree faces above the critical angle: the beam is folded twice and sent straight back."),
         gp_Pnt(0, 0, -80.0), gp_Dir(0, 0, 1)},

        // --- scattering ---
        {QStringLiteral("Integrating Sphere"),
         QStringLiteral("A cavity painted matte white: every bounce is Lambertian, so the wall loses its memory of where the light came from and the exit port sees a uniform field."),
         gp_Pnt(0, 0, 0.0), gp_Dir(0, 0, -1)},
        {QStringLiteral("Diffuser Plate"),
         QStringLiteral("A transmissive diffuser: light refracts in, then leaves the far face cosine-weighted instead of straight on, washing a bright spot into an even glow."),
         gp_Pnt(0, 0, -80.0), gp_Dir(0, 0, 1)},
    }};
    return table;
}

// ---- parameter tables ------------------------------------------------------

SceneParamInfo mk(const char* name, const char* unit, double mn, double mx,
                  double def, double step, int dec, const char* tip) {
    SceneParamInfo p;
    p.name     = QString::fromUtf8(name);
    p.unit     = QString::fromUtf8(unit);
    p.min      = mn;
    p.max      = mx;
    p.def      = def;
    p.step     = step;
    p.decimals = dec;
    p.tip      = QString::fromUtf8(tip);
    return p;
}

// The receiver position, appended to every scene as its last slot.
SceneParamInfo detZ(double mn, double mx, double def) {
    return mk("Detector Z", "mm", mn, mx, def, 10.0, 1,
              "Where the receiver plane sits on the optical axis. Step it through "
              "focus to find the plane with the smallest spot.");
}

const std::array<std::vector<SceneParamInfo>, std::size_t(Scene::Count)>& paramTable() {
    static const std::array<std::vector<SceneParamInfo>, std::size_t(Scene::Count)> table = {{
        // Reflector
        {mk("Focal length", "mm", 20, 240, 100, 5, 1,
            "z = r^2 / 4f. The source sits at the focus, so this moves both."),
         mk("Aperture radius", "mm", 40, 220, 140, 5, 1, "Outer rim radius of the dish."),
         detZ(160, 700, 320)},
        // EllipticalReflector
        {mk("Semi-major axis", "mm", 120, 320, 200, 5, 1,
            "Along z. With the semi-minor axis it fixes both foci at +/- sqrt(a^2 - b^2)."),
         mk("Semi-minor axis", "mm", 40, 190, 120, 5, 1, "The rim radius of the ellipsoid."),
         detZ(20, 340, 160)},
        // SphericalReflector
        {mk("Mirror radius", "mm", 80, 320, 180, 5, 1,
            "The source sits at the paraxial focus, R/2, which is where the aberration shows."),
         mk("Half angle", "deg", 10, 75, 50, 2.5, 1, "How much of the sphere is kept."),
         detZ(150, 700, 320)},
        // OffAxisParabola
        {mk("Focal length", "mm", 40, 200, 100, 5, 1, "Of the parent paraboloid."),
         mk("Off-axis offset", "mm", 10, 100, 30, 5, 1,
            "Inner x of the kept segment. The receiver follows the segment's centre."),
         detZ(180, 700, 320)},
        // ParabolicTrough
        {mk("Focal length", "mm", 20, 160, 60, 5, 1, "Line focus height; the source sits on it."),
         mk("Half width", "mm", 40, 200, 120, 5, 1, "Trough half-aperture in x."),
         detZ(150, 700, 300)},
        // Cpc
        {mk("Exit radius", "mm", 10, 70, 30, 2.5, 1, "Radius of the small end."),
         mk("Acceptance angle", "deg", 10, 60, 30, 2.5, 1,
            "Rays arriving inside this half-angle reach the exit; the rest are turned back."),
         detZ(-120, 60, -10)},
        // ConicalConcentrator
        {mk("Entrance radius", "mm", 30, 120, 60, 5, 1, "Wide end, at the top."),
         mk("Exit radius", "mm", 8, 60, 25, 2.5, 1, "Narrow end, at z = 0."),
         detZ(-120, 60, -10)},
        // Cassegrain
        {mk("Primary focal length", "mm", 50, 200, 100, 5, 1, "Of the annular paraboloid."),
         mk("Secondary distance", "mm", 80, 300, 160, 5, 1, "Vertex height of the convex secondary."),
         detZ(200, 800, 420)},
        // CornerCube
        {mk("Mirror side", "mm", 40, 200, 100, 5, 1, "Edge length of each of the three plates."),
         detZ(80, 500, 200)},
        // Lens (ball)
        {mk("Ball radius", "mm", 15, 90, 50, 2.5, 1, "Radius of the glass sphere."),
         mk("Source distance", "mm", 60, 400, 180, 10, 1, "How far in front of the lens the emitter sits."),
         detZ(80, 600, 200)},
        // PlanoConvexLens
        {mk("Radius of curvature", "mm", 60, 240, 120, 5, 1,
            "f = R / (n - 1), so at n = 1.5 the focal length is twice this."),
         mk("Source distance", "mm", 80, 600, 240, 10, 1, "Set it to f for a collimated output."),
         detZ(150, 900, 480)},
        // BiconvexLens
        {mk("Radius of curvature", "mm", 60, 240, 120, 5, 1, "Both surfaces share it."),
         mk("Source distance", "mm", 80, 500, 180, 10, 1, ""),
         detZ(120, 800, 360)},
        // PlanoConcaveLens
        {mk("Dish radius", "mm", 70, 260, 120, 5, 1, "Radius of the concave face; larger is weaker."),
         mk("Blank radius", "mm", 25, 110, 60, 2.5, 1, "Outer radius of the lens body."),
         detZ(80, 700, 300)},
        // HalfBallLens
        {mk("Dome radius", "mm", 20, 120, 60, 5, 1, "Hemisphere radius over the emitter."),
         detZ(100, 600, 250)},
        // CylindricalLens
        {mk("Rod radius", "mm", 25, 140, 80, 5, 1,
            "f = nR / (2(n-1)) from the axis, so the stripe forms at that distance."),
         mk("Source distance", "mm", 80, 500, 240, 10, 1, ""),
         detZ(100, 700, 240)},
        // FresnelLens
        {mk("Plate radius", "mm", 40, 180, 100, 5, 1, "Outer radius of the stepped plate."),
         mk("Zones", "", 2, 16, 6, 1, 0, "Number of annular facets."),
         mk("Source distance", "mm", 60, 400, 150, 10, 1, ""),
         detZ(80, 700, 250)},
        // Axicon
        {mk("Cone radius", "mm", 25, 110, 60, 5, 1, "Base radius of the glass cone."),
         mk("Cone height", "mm", 10, 120, 40, 5, 1, "Taller means a steeper deflection and a wider ring."),
         detZ(180, 900, 400)},
        // MicrolensArray
        {mk("Lenslet radius", "mm", 5, 20, 12, 1, 1, "Radius of each ball in the grid."),
         mk("Pitch", "mm", 12, 60, 25, 2.5, 1, "Centre-to-centre spacing; below 2R the balls merge."),
         detZ(60, 500, 150)},
        // Prism
        {mk("Half width", "mm", 20, 100, 50, 5, 1, "Half the base of the triangle."),
         mk("Apex height", "mm", 10, 140, 30, 5, 1, "Taller is a sharper apex and a bigger deviation."),
         detZ(120, 700, 300)},
        // LightGuide
        {mk("Guide radius", "mm", 15, 100, 60, 5, 1, "Rod radius."),
         mk("Guide length", "mm", 40, 600, 200, 20, 1,
            "Bulk absorption scales with this, so a long guide is measurably dimmer."),
         detZ(60, 800, 230)},
        // SquareLightGuide
        {mk("Half side", "mm", 12, 80, 40, 2.5, 1, "Half the square cross-section."),
         mk("Guide length", "mm", 40, 600, 220, 20, 1, ""),
         detZ(60, 800, 250)},
        // TaperedLightGuide
        {mk("Entrance radius", "mm", 20, 110, 70, 5, 1, "Wide end, at the source."),
         mk("Exit radius", "mm", 8, 90, 35, 2.5, 1, "Narrow end. A steeper taper leaks more."),
         mk("Guide length", "mm", 60, 500, 200, 20, 1, ""),
         detZ(80, 700, 230)},
        // PorroPrism
        {mk("Half width", "mm", 25, 120, 60, 5, 1, "Half the base of the prism."),
         mk("Prism height", "mm", 25, 120, 60, 5, 1, "At half the width the roof faces sit at 45 degrees."),
         detZ(-500, -40, -150)},
        // IntegratingSphere
        {mk("Sphere radius", "mm", 40, 160, 80, 5, 1, "Cavity radius."),
         mk("Wall reflectance", "", 0.80, 0.995, 0.96, 0.01, 3,
            "Matte white paint. The port efficiency is roughly port / (port + (1-R) x sphere)."),
         mk("Port size", "mm", 10, 80, 40, 5, 1, "Edge of the square exit port."),
         detZ(-160, 160, -78)},
        // DiffuserPlate
        {mk("Plate size", "mm", 60, 300, 160, 10, 1, "Edge of the square diffuser."),
         mk("Diffusion", "", 0.0, 1.0, 0.90, 0.05, 2,
            "Fraction of the light leaving the far face cosine-weighted rather than refracted straight on."),
         detZ(60, 600, 150)},
    }};
    return table;
}

} // namespace

const GeometryProvider::SceneInfo& GeometryProvider::info(Scene scene) {
    const auto& table = registry();
    const std::size_t idx = std::size_t(scene);
    return table[idx < table.size() ? idx : 0];
}

const std::vector<SceneParamInfo>& GeometryProvider::paramInfo(Scene scene) {
    const auto& table = paramTable();
    const std::size_t idx = std::size_t(scene);
    return table[idx < table.size() ? idx : 0];
}

SceneParams GeometryProvider::defaultParams(Scene scene) {
    SceneParams p;
    const auto& info = paramInfo(scene);
    for (std::size_t i = 0; i < info.size() && i < SceneParams::kMax; ++i)
        p.v[i] = info[i].def;
    return p;
}

SceneParams GeometryProvider::sanitise(Scene scene, const SceneParams& params) {
    SceneParams p;
    const auto& info = paramInfo(scene);
    for (std::size_t i = 0; i < SceneParams::kMax; ++i) {
        if (i >= info.size()) { p.v[i] = 0.0; continue; }
        const double v = params.v[i];
        // A NaN out of a malformed config file would propagate into the
        // geometry and then into every hit test, so it is replaced outright.
        p.v[i] = std::isfinite(v) ? std::clamp(v, info[i].min, info[i].max) : info[i].def;
    }
    return p;
}

std::vector<OpticalSurface> GeometryProvider::buildScene(Scene scene) {
    return build(scene, defaultParams(scene)).surfaces;
}

GeometryProvider::SceneSetup GeometryProvider::build(Scene scene, const SceneParams& raw) {
    const SceneParams P = sanitise(scene, raw);
    const auto& infos   = paramInfo(scene);
    // The receiver always occupies the last live slot.
    const double dz = infos.empty() ? 0.0 : P.v[infos.size() - 1];

    SceneSetup out;
    out.sourceOrigin = info(scene).sourceOrigin;
    out.sourceAxis   = info(scene).sourceAxis;
    auto& s = out.surfaces;

    switch (scene) {
    // ---------------------------------------------------------- reflective --
    case Scene::Reflector: {
        const double f = P.v[0], rOuter = P.v[1];
        s.push_back(mirror(paraboloid(f, 0.0, rOuter), QStringLiteral("Parabolic Mirror")));
        s.push_back(detector(std::max(200.0, 2.2 * rOuter), dz));
        // The whole point of the scene is that the emitter sits at the focus.
        out.sourceOrigin = gp_Pnt(0, 0, f);
        break;
    }

    case Scene::EllipticalReflector: {
        const double a = P.v[0];
        const double b = std::min(P.v[1], a - 5.0);
        const double c = std::sqrt(std::max(1.0, a * a - b * b));   // focal distance
        s.push_back(mirror(ellipsoidCap(a, b, -0.975 * a, 0.2 * a),
                           QStringLiteral("Ellipsoid Mirror")));
        s.push_back(detector(std::max(60.0, 0.75 * b), dz));
        out.sourceOrigin = gp_Pnt(0, 0, -c);
        break;
    }

    case Scene::SphericalReflector: {
        const double R = P.v[0], half = P.v[1];
        s.push_back(mirror(sphericalCap(R, half, 0.0, +1.0),
                           QStringLiteral("Spherical Mirror")));
        s.push_back(detector(std::max(200.0, 1.8 * R), dz));
        out.sourceOrigin = gp_Pnt(0, 0, 0.5 * R);   // paraxial focus
        break;
    }

    case Scene::OffAxisParabola: {
        const double f = P.v[0], xInner = P.v[1];
        const double rOuter = 150.0;
        // Keep only the patch with x in [xInner, rOuter]; a paraboloid collimates
        // to +Z from anywhere on its surface, so the beam leaves off-axis too.
        const TopoDS_Shape full   = paraboloid(f, 0.0, rOuter);
        const TopoDS_Shape window = BRepPrimAPI_MakeBox(gp_Pnt(xInner, -70.0, -20.0),
                                                        rOuter - xInner + 10.0, 140.0,
                                                        rOuter * rOuter / (4.0 * f) + 30.0).Shape();
        s.push_back(mirror(BRepAlgoAPI_Common(full, window).Shape(),
                           QStringLiteral("Off-Axis Parabolic Segment")));
        s.push_back(detector(220.0, dz, 0.5 * (xInner + rOuter), 0.0));
        out.sourceOrigin = gp_Pnt(0, 0, f);
        break;
    }

    case Scene::ParabolicTrough: {
        const double f = P.v[0], halfW = P.v[1];
        std::vector<gp_Pnt> profile;
        for (int i = 0; i < 41; ++i) {
            const double x = -halfW + 2.0 * halfW * double(i) / 40.0;
            profile.emplace_back(x, 0.0, (x * x) / (4.0 * f));
        }
        s.push_back(mirror(extrudeAlongY(profile, 300.0, false),
                           QStringLiteral("Parabolic Trough")));
        s.push_back(detector(std::max(240.0, 2.8 * halfW), dz));
        out.sourceOrigin = gp_Pnt(0, 0, f);
        break;
    }

    case Scene::Cpc: {
        const double aExit = P.v[0], theta = P.v[1];
        const std::vector<gp_Pnt> profile = cpcPoints(aExit, theta);
        double topZ = 0.0;
        for (const auto& p : profile) topZ = std::max(topZ, p.Z());
        s.push_back(mirror(revolveSpline(profile), QStringLiteral("CPC Wall")));
        s.push_back(detector(std::max(30.0, 2.3 * aExit), dz));
        out.sourceOrigin = gp_Pnt(0, 0, topZ + 0.1 * topZ + 10.0);
        break;
    }

    case Scene::ConicalConcentrator: {
        const double rIn = P.v[0];
        const double rOut = std::min(P.v[1], rIn - 2.0);
        const double h = 160.0;
        s.push_back(mirror(revolvePolyline({gp_Pnt(0, rIn, h), gp_Pnt(0, rOut, 0.0)}),
                           QStringLiteral("Conical Funnel")));
        s.push_back(detector(std::max(30.0, 2.8 * rOut), dz));
        out.sourceOrigin = gp_Pnt(0, 0, h + 10.0);
        break;
    }

    case Scene::Cassegrain: {
        const double f = P.v[0], zSec = P.v[1];
        s.push_back(mirror(paraboloid(f, 0.4 * f, 1.5 * f),
                           QStringLiteral("Primary (annular paraboloid)")));
        s.push_back(mirror(sphericalCap(2.2 * f, 13.0, zSec, -1.0),
                           QStringLiteral("Secondary (convex)")));
        s.push_back(detector(std::max(240.0, 3.6 * f), dz));
        break;
    }

    case Scene::CornerCube: {
        const double side = P.v[0];
        s.push_back(mirror(cornerCube(side), QStringLiteral("Corner Cube")));
        s.push_back(detector(std::max(200.0, 4.2 * side), dz));
        out.sourceOrigin = gp_Pnt(0.5 * side, 0.5 * side, 1.5 * side);
        break;
    }

    // ---------------------------------------------------------- refractive --
    case Scene::Lens: {
        const double R = P.v[0], srcD = P.v[1];
        s.push_back(glass(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), R).Shape(),
                          QStringLiteral("Glass Ball Lens")));
        s.push_back(detector(std::max(180.0, 5.6 * R), dz));
        out.sourceOrigin = gp_Pnt(0, 0, -srcD);
        break;
    }

    case Scene::PlanoConvexLens: {
        const double R = P.v[0], srcD = P.v[1];
        // Sphere centred at -R/2 so the cap vertex lands at z = R/2.
        const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, -0.5 * R), R).Shape();
        const TopoDS_Shape slab = BRepPrimAPI_MakeBox(gp_Pnt(-1.1 * R, -1.1 * R, 0.0),
                                                       2.2 * R, 2.2 * R, 0.6 * R).Shape();
        s.push_back(glass(BRepAlgoAPI_Common(sphere, slab).Shape(),
                          QStringLiteral("Plano-Convex Lens")));
        s.push_back(detector(std::max(200.0, 2.5 * R), dz));
        out.sourceOrigin = gp_Pnt(0, 0, -srcD);
        break;
    }

    case Scene::BiconvexLens: {
        const double R = P.v[0], srcD = P.v[1];
        const double halfThk = 30.0;
        const TopoDS_Shape a = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, R - halfThk), R).Shape();
        const TopoDS_Shape b = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, halfThk - R), R).Shape();
        s.push_back(glass(BRepAlgoAPI_Common(a, b).Shape(), QStringLiteral("Biconvex Lens")));
        s.push_back(detector(std::max(180.0, 2.0 * R), dz));
        out.sourceOrigin = gp_Pnt(0, 0, -srcD);
        break;
    }

    case Scene::PlanoConcaveLens: {
        const double R = P.v[0], blank = P.v[1];
        const TopoDS_Shape body = BRepPrimAPI_MakeCylinder(
            gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), blank, 25.0).Shape();
        // Centre the dish so it bites 12 mm into the top face.
        const TopoDS_Shape dish = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, R + 13.0), R).Shape();
        s.push_back(glass(BRepAlgoAPI_Cut(body, dish).Shape(),
                          QStringLiteral("Plano-Concave Lens")));
        s.push_back(detector(400.0, dz));
        break;
    }

    case Scene::HalfBallLens: {
        const double R = P.v[0];
        const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), R).Shape();
        const TopoDS_Shape upper = BRepPrimAPI_MakeBox(gp_Pnt(-1.2 * R, -1.2 * R, 0.0),
                                                        2.4 * R, 2.4 * R, 1.2 * R).Shape();
        s.push_back(glass(BRepAlgoAPI_Common(sphere, upper).Shape(),
                          QStringLiteral("Half-Ball Lens")));
        s.push_back(detector(std::max(150.0, 5.0 * R), dz));
        out.sourceOrigin = gp_Pnt(0, 0, -0.08 * R);
        break;
    }

    case Scene::CylindricalLens: {
        const double R = P.v[0], srcD = P.v[1];
        // f = nR / (2(n-1)) from the rod axis, so a source at 2f images at 2f --
        // that is where the receiver has to sit for the stripe to be a stripe.
        // The receiver is also kept narrow enough that rays which bypassed the
        // rod land outside it instead of washing it out.
        s.push_back(glass(BRepPrimAPI_MakeCylinder(
                              gp_Ax2(gp_Pnt(0, -150.0, 0), gp_Dir(0, 1, 0)), R, 300.0).Shape(),
                          QStringLiteral("Cylindrical Rod Lens")));
        s.push_back(detector(std::max(120.0, 2.5 * R), dz));
        out.sourceOrigin = gp_Pnt(0, 0, -srcD);
        break;
    }

    case Scene::FresnelLens: {
        const double radius = P.v[0];
        const int    zones  = std::max(2, int(std::lround(P.v[1])));
        const double srcD   = P.v[2];
        s.push_back(glass(fresnelPlate(radius, 4.0, 8.0, zones),
                          QStringLiteral("Fresnel Lens")));
        s.push_back(detector(std::max(200.0, 3.2 * radius), dz));
        out.sourceOrigin = gp_Pnt(0, 0, -srcD);
        break;
    }

    case Scene::Axicon: {
        const double coneR = P.v[0], coneH = P.v[1];
        // A cone turns a collimated beam into a ring; fed a diverging point
        // source directly it just makes a blurred spot, so the paraboloid in
        // front of it is doing essential work rather than decoration.
        s.push_back(mirror(paraboloid(40.0, 0.0, coneR), QStringLiteral("Collimating Paraboloid")));
        s.push_back(glass(BRepPrimAPI_MakeCone(
                              gp_Ax2(gp_Pnt(0, 0, 120.0), gp_Dir(0, 0, 1)), coneR, 0.0, coneH).Shape(),
                          QStringLiteral("Axicon Cone")));
        s.push_back(detector(std::max(240.0, 5.6 * coneR), dz));
        out.sourceOrigin = gp_Pnt(0, 0, 40.0);
        break;
    }

    case Scene::MicrolensArray: {
        const double R = P.v[0], pitch = P.v[1];
        OpticalSurface array = glass(microlensArray(5, 5, R, pitch),
                                     QStringLiteral("Microlens Array"));
        // Twenty-five bodies at the default angular deflection is half a million
        // triangles and a three-second build. An array is a beam homogeniser
        // rather than an imaging optic, so the facet error costs it little.
        array.meshAngle = 0.16;
        s.push_back(array);
        s.push_back(detector(std::max(200.0, 8.0 * pitch), dz));
        break;
    }

    case Scene::Prism: {
        const double halfW = P.v[0], apex = P.v[1];
        s.push_back(glass(extrudeAlongY({gp_Pnt(-halfW, 0, 0.0), gp_Pnt(halfW, 0, 0.0),
                                         gp_Pnt(0.0, 0, apex)}, 120.0, true),
                          QStringLiteral("Glass Prism")));
        s.push_back(detector(600.0, dz));
        break;
    }

    // ----------------------------------------------------------------- TIR --
    case Scene::LightGuide: {
        const double R = P.v[0], len = P.v[1];
        s.push_back(glass(BRepPrimAPI_MakeCylinder(
                              gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), R, len).Shape(),
                          QStringLiteral("Light Guide (n=1.5)"), /*guide=*/true));
        s.push_back(detector(std::max(120.0, 3.3 * R), std::max(dz, len + 5.0)));
        break;
    }

    case Scene::SquareLightGuide: {
        const double h = P.v[0], len = P.v[1];
        s.push_back(glass(BRepPrimAPI_MakeBox(gp_Pnt(-h, -h, 0.0), 2 * h, 2 * h, len).Shape(),
                          QStringLiteral("Square Light Guide"), /*guide=*/true));
        s.push_back(detector(std::max(120.0, 5.5 * h), std::max(dz, len + 5.0)));
        break;
    }

    case Scene::TaperedLightGuide: {
        const double rIn = P.v[0];
        const double rOut = std::min(P.v[1], rIn - 2.0);
        const double len = P.v[2];
        s.push_back(glass(BRepPrimAPI_MakeCone(
                              gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), rIn, rOut, len).Shape(),
                          QStringLiteral("Tapered Light Guide"), /*guide=*/true));
        s.push_back(detector(std::max(120.0, 3.0 * rIn), std::max(dz, len + 5.0)));
        break;
    }

    case Scene::PorroPrism: {
        const double halfW = P.v[0], h = P.v[1];
        // Flat entrance at z = 0, two roof faces above it. At 45 degrees inside
        // n = 1.5 glass the critical angle (41.8) is exceeded, so both faces
        // reflect totally and the beam leaves back down through the base.
        s.push_back(glass(extrudeAlongY({gp_Pnt(-halfW, 0, 0.0), gp_Pnt(halfW, 0, 0.0),
                                         gp_Pnt(0.0, 0, h)}, 140.0, true),
                          QStringLiteral("Porro Prism"), /*guide=*/true));
        s.push_back(detector(std::max(300.0, 7.0 * halfW), std::min(dz, -40.0)));
        break;
    }

    // ---------------------------------------------------------- scattering --
    case Scene::IntegratingSphere: {
        const double R = P.v[0], refl = P.v[1], port = P.v[2];
        s.push_back(diffuseWhite(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), R).Shape(),
                                 QStringLiteral("Matte White Cavity"), refl));
        // The port has to stay inside the cavity or nothing can ever reach it.
        const double z = std::clamp(dz, -(R - 2.0), R - 2.0);
        s.push_back(detector(std::min(port, 1.4 * R), z));
        break;
    }

    case Scene::DiffuserPlate: {
        const double size = P.v[0], diffusion = P.v[1];
        OpticalSurface plate = glass(
            BRepPrimAPI_MakeBox(gp_Pnt(-0.5 * size, -0.5 * size, 0.0), size, size, 6.0).Shape(),
            QStringLiteral("Diffuser Plate"));
        plate.scatter = std::clamp(diffusion, 0.0, 1.0);
        s.push_back(plate);
        s.push_back(detector(std::max(300.0, 3.2 * size), dz));
        break;
    }

    case Scene::Count:
        break;
    }

    return out;
}
