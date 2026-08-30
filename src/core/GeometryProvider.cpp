#include "GeometryProvider.h"
#include "Coating.h"
#include "Material.h"

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
// The fixed splits below are the fallbacks used when the Fresnel model is
// switched off; with it on -- the default -- the numbers come from the material.
constexpr double kMirrorR = 0.95;    // fallback for a front-surface mirror
constexpr double kGlassT  = 0.96;
constexpr double kGlassR  = 0.04;

// The catalogue names the scenes are built from. Naming them here rather than
// spelling out coefficients is the whole point of having a catalogue: a scene
// says "N-SF11" the way a drawing does.
const char* const kDefaultGlass  = "N-BK7";
const char* const kFlintGlass    = "N-SF11";
const char* const kPolymer       = "PMMA (acrylic)";
const char* const kDefaultMirror = "Aluminium";

// The coating a catalogue lens is sold with. A guide or a prism that works by
// total internal reflection is left bare, because that is how they are made.
const char* const kDefaultCoating = "Broadband AR";

// Applies a catalogue material to a surface: the reference index, the internal
// attenuation and the dispersion model all come from one name.
void applyMaterial(OpticalSurface& o, const char* name) {
    const OpticalMaterial m = materials::byName(QLatin1String(name));
    if (!m.valid()) return;
    o.material = m;
    if (m.isMetal()) {
        // A metal is opaque: what its material supplies is the complex index the
        // angle- and wavelength-dependent reflectance is computed from.
        return;
    }
    o.index      = m.nd;
    o.absorption = m.alpha;
}

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

// A front-surface mirror. With the Fresnel model on it reflects what the metal
// actually reflects -- angle- and wavelength-dependent, roughly 92 % for
// aluminium at normal incidence and rising toward grazing -- and falls back to
// the flat number when the model is off.
OpticalSurface mirror(TopoDS_Shape s, const QString& label, double refl = kMirrorR,
                      const char* material = kDefaultMirror) {
    OpticalSurface o = surf(s, label, refl, 0.0, 0.0);
    if (material) applyMaterial(o, material);
    return o;
}

// A refractive solid. `guide` surfaces describe a pure light pipe: when the
// Fresnel model is switched off they fall back to R = 0 / T = 1 rather than to
// a 4 % surface reflection. With Fresnel on -- the default -- the split comes
// from the angle of incidence either way, so the flag only sets the fallback.
OpticalSurface glass(TopoDS_Shape s, const QString& label, bool guide = false,
                     const char* material = kDefaultGlass,
                     const char* coating = nullptr) {
    OpticalSurface o = guide ? surf(s, label, 0.0, 1.0, 1.5)
                             : surf(s, label, kGlassR, kGlassT, 1.5);
    o.fresnel = true;
    applyMaterial(o, material);
    // Every real lens is coated, and leaving them bare made a multi-element
    // system overstate its loss by roughly 3.5 % per surface -- which anybody
    // who knows optics notices at once.
    //
    // A light pipe is the exception and not an oversight: its walls and its end
    // faces are one surface, and a coating on the wall is precisely what a part
    // that works by total internal reflection must not have. So `guide` gets no
    // coating unless one is asked for by name.
    const char* film = coating ? coating : (guide ? nullptr : kDefaultCoating);
    if (film) o.coating = coating::byName(QLatin1String(film));
    return o;
}

// A matte white surface: high reflectance, all of it Lambertian. This is the
// paint inside an integrating sphere and the wall of a luminaire cavity.
OpticalSurface diffuseWhite(TopoDS_Shape s, const QString& label, double refl) {
    OpticalSurface o = surf(s, label, std::clamp(refl, 0.0, 1.0), 0.0, 0.0);
    o.scatter = 1.0;
    return o;
}

// A matte body: the same Lambertian mechanism as the white cavity above, at
// whatever reflectance the part is actually finished to. A housing is not white
// and a deck is not paint, but both scatter everything they return, and that is
// what the mechanism says.
OpticalSurface matte(TopoDS_Shape s, const QString& label, double refl) {
    return diffuseWhite(std::move(s), label, refl);
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

// The floor a fixture stands on: a square normal to the axis, behind everything,
// facing the way the optic points.
//
// Behind rather than beside, which is not a composition choice but the only
// place it can go. The optical axis is +z and so is the viewer's up, so a
// luminaire on this axis is an uplighter: it stands on the floor and throws its
// beam at the receiver overhead. A plane below it is therefore *behind* it, and
// a beam that only ever travels +z cannot reach it -- the measurement is the
// measurement it would have been without a floor at all. What the floor is for
// is the picture, where an object standing on something reads as a photograph
// and the same object in a void reads as CAD.
//
// Built on an explicit plane rather than on the winding of its own wire, so the
// normal faces the fixture because it was asked to. A wire wound the other way
// gives a face pointing away into nothing, which the tracer copes with and a
// renderer draws as a black slab: the geometry right and the picture wrong,
// which is the hardest kind of wrong to find by reading.
TopoDS_Shape floorPlate(double halfSide, double z) {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(-halfSide, -halfSide, z));
    poly.Add(gp_Pnt(halfSide, -halfSide, z));
    poly.Add(gp_Pnt(halfSide, halfSide, z));
    poly.Add(gp_Pnt(-halfSide, halfSide, z));
    poly.Close();
    Handle(Geom_Plane) plane =
        new Geom_Plane(gp_Ax2(gp_Pnt(0.0, 0.0, z), gp_Dir(0, 0, 1)));
    return BRepBuilderAPI_MakeFace(plane, poly.Wire(), true).Shape();
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

// The showcase fixture's dimensions, derived once from its three parameters so
// the builder and the derived-quantity strip cannot disagree about where the rim
// is. Every length below is cut from the cup radius, which is what makes the
// fixture scale as one object rather than as four parts that happen to fit.
struct Fixture {
    double cupRadius = 60.0;
    double focal     = 24.0;
    double die       = 5.0;
    double diffusion = 0.55;

    // Rim height of the paraboloid z = r^2 / 4f. With f below half the rim
    // radius this is above the focus, so the die sits down inside the cup.
    double rimZ() const { return cupRadius * cupRadius / (4.0 * focal); }
    // A hole at the vertex, where the die's own mount would pass through.
    double vertexHole() const { return std::max(1.5, 0.75 * die); }

    double bezelRadius() const { return cupRadius + 5.0; }
    double backZ() const { return -8.0; }

    // The cover lens: a shallow spherical cap on a cylindrical edge band,
    // sitting on the rim. Gentle enough to be a cover rather than a second
    // optic -- it is here because acrylic in front of a lit cup is what the
    // part actually looks like.
    double lensSag() const { return 0.30 * cupRadius; }
    double lensRadius() const { return 2.2 * cupRadius; }

    // Wide enough for the beam the die's own size forces open: the spread
    // below is the half-angle at the rim, and the aperture is added to it.
    double receiverSize() const {
        const double spread = 0.5 * die / std::max(1.0, focal);
        return std::max(220.0, 2.2 * (cupRadius + spread * 400.0));
    }

    // The base the fixture stands on -- a short coaxial plinth under the
    // housing, the heat sink of any real fixture -- and the floor under that.
    double baseHeight() const { return 0.22 * cupRadius; }
    double baseRadius() const { return 1.12 * bezelRadius(); }
    double baseZ() const { return backZ() - baseHeight(); }
    double floorHalfSide() const { return 2.6 * cupRadius; }
};

Fixture fixture(const SceneParams& P) {
    Fixture x;
    x.cupRadius = P.v[0];
    x.focal     = P.v[1];
    x.die       = P.v[2];
    x.diffusion = P.v[3];
    return x;
}

// Where the lenslets of an nx x ny array sit, as translations about the origin.
// One lenslet plus these is the whole array: it is tessellated once, its
// hierarchy is built once, and the placements are what put it in nx * ny
// places. Meshing twenty-five copies instead meant twenty-five tessellations
// and a hierarchy over all of their triangles, which is why the array used to
// need a hand-coarsened mesh to build in reasonable time.
std::vector<gp_Trsf> gridPlacements(int nx, int ny, double pitch) {
    std::vector<gp_Trsf> out;
    out.reserve(std::size_t(nx) * std::size_t(ny));
    const double x0 = -0.5 * pitch * (nx - 1);
    const double y0 = -0.5 * pitch * (ny - 1);
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j) {
            gp_Trsf t;
            t.SetTranslation(gp_Vec(x0 + i * pitch, y0 + j * pitch, 0.0));
            out.push_back(t);
        }
    return out;
}

// The cup geometry of the LED array, resolved from the parameters once so the
// builder, the derived readouts and the source lattice cannot disagree about
// where a cup is -- which they would the moment one of them clamped differently
// from another.
struct LedArray {
    int    cups  = 4;
    double pitch = 90.0;
    double radius = 40.0;
    double focal = 32.0;    // cup focal length; the emitter sits here
    double x0    = 0.0;     // centre of the first cup, so the row straddles x = 0

    double span() const { return pitch * double(cups - 1) + 2.0 * radius; }
};

LedArray ledArray(const SceneParams& P) {
    LedArray a;
    a.cups   = std::clamp(int(std::lround(P.v[0])), 2, 8);
    a.pitch  = P.v[1];
    // Neighbours that overlap would intersect each other's rims, and a ray
    // caught between two cups is a geometry bug reported as an optical result.
    a.radius = std::min(P.v[2], 0.48 * a.pitch);
    // A deep cup: the rim stands 0.31 R proud of the vertex, which subtends
    // about 128 degrees from the focus. That is what a real LED cup does -- it
    // has to catch a hemisphere, not a pencil.
    a.focal  = 0.8 * a.radius;
    a.x0     = -0.5 * a.pitch * double(a.cups - 1);
    return a;
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

        // --- multi-source ---
        {QStringLiteral("LED Array Luminaire (multi-source)"),
         QStringLiteral("A row of identical reflector cups, one LED to a cup. The scene "
                        "places a single emitter, at the focus of the leftmost cup, so a "
                        "run out of the box lights one cup and leaves the rest dark -- "
                        "which is the point. Add a source per remaining cup, each offset "
                        "along x by a whole multiple of the pitch, and the receiver fills "
                        "in one beam at a time. Every scene in this library is built "
                        "around a single emitter on an axis; this one cannot be described "
                        "by one at all."),
         gp_Pnt(-135.0, 0, 32.0), gp_Dir(0, 0, -1)},

        // --- showcase ---
        {QStringLiteral("Showcase Luminaire (appearance)"),
         QStringLiteral("A whole fixture rather than a bare optic: an LED die in an "
                        "aluminium reflector cup, behind an acrylic cover lens, in a "
                        "housing, over a matte deck. Every other scene in this library "
                        "is one optic floating in nothing, which is the right way to "
                        "teach one optic and the wrong way to see what a part looks "
                        "like -- a render of an object against no background reads as "
                        "CAD, and a render of the same object standing on something "
                        "reads as a photograph. So this one carries the surfaces a "
                        "picture needs: metal to reflect, glass to refract, a deck for "
                        "the light to land on. It ships with a real emitting area, so "
                        "the die glows as a face in the Appearance tab instead of being "
                        "a point light -- and it is traced exactly as it is drawn."),
         gp_Pnt(0, 0, 24.0), gp_Dir(0, 0, -1)},
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
        // LedArrayLuminaire
        {mk("Cup count", "", 2, 8, 4, 1, 0,
            "How many reflector cups stand in the row. The scene lights the first "
            "one; each of the others needs a source of its own, which is what this "
            "scene exists to exercise."),
         mk("Cup pitch", "mm", 40, 220, 90, 5, 1,
            "Centre-to-centre spacing along x. Every added source sits at a whole "
            "multiple of it, so a four-cup row is one number typed three times -- "
            "and Add fills the next one in for you."),
         mk("Cup radius", "mm", 10, 90, 40, 2.5, 1,
            "Rim radius of one cup. Clamped below half the pitch, because "
            "neighbouring cups that overlap are not a luminaire."),
         detZ(200, 1200, 500)},
        // ShowcaseLuminaire
        {mk("Cup radius", "mm", 25, 120, 60, 2.5, 1,
            "Rim radius of the reflector, and the size of the whole fixture: the "
            "housing, the cover lens and the deck are all cut from it."),
         mk("Cup focal length", "mm", 10, 70, 24, 1, 1,
            "The die sits at the focus, so this moves both. Below half the rim "
            "radius the cup is deeper than it is wide and the die disappears "
            "inside it -- which is what a real fixture does, and what makes the "
            "render show a lit cavity rather than a bare emitter."),
         mk("Die size", "mm", 1, 16, 5, 0.5, 1,
            "Edge of the square emitting area. This is the one number that is "
            "both an optical parameter and a visual one: it sets the etendue "
            "that limits how tightly the cup can collimate, and it is the size "
            "of the glowing face the Appearance tab draws."),
         mk("Cover diffusion", "", 0.0, 1.0, 0.55, 0.05, 2,
            "How opal the cover is. At zero it is clear acrylic: the beam "
            "leaves as the cup formed it and the render shows the reflector "
            "through the glass. Turned up it is an opal cover -- the beam "
            "widens and softens, and the whole aperture glows, which is what a "
            "luminaire looks like switched on and what a bare optic never "
            "does."),
         detZ(150, 1200, 320)},
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
    p.n = int(std::min<std::size_t>(info.size(), std::size_t(SceneParams::kMax)));
    for (std::size_t i = 0; i < info.size() && i < SceneParams::kMax; ++i)
        p.v[i] = info[i].def;
    return p;
}

SceneParams GeometryProvider::sanitise(Scene scene, const SceneParams& params) {
    SceneParams p;
    const auto& info = paramInfo(scene);
    const int used = int(std::min<std::size_t>(info.size(), std::size_t(SceneParams::kMax)));
    p.n = used;
    for (int i = 0; i < used; ++i) {
        const double v = params.v[i];
        // A NaN out of a malformed config file would propagate into the
        // geometry and then into every hit test, so it is replaced outright.
        p.v[i] = std::isfinite(v) ? std::clamp(v, info[i].min, info[i].max) : info[i].def;
    }
    for (int i = used; i < SceneParams::kMax; ++i) p.v[i] = 0.0;
    return p;
}


namespace {

DerivedQuantity dq(const QString& name, double value, int decimals,
                   const QString& unit, const QString& tip) {
    DerivedQuantity q;
    q.name  = name;
    q.value = QString::number(value, 'f', decimals);
    q.unit  = unit;
    q.tip   = tip;
    return q;
}

DerivedQuantity dqText(const QString& name, const QString& value,
                       const QString& unit, const QString& tip) {
    DerivedQuantity q;
    q.name  = name;
    q.value = value;
    q.unit  = unit;
    q.tip   = tip;
    return q;
}

// f/# and NA for an optic of focal length f and clear semi-aperture r. The two
// are the same fact stated twice, and different people reach for different ones.
void addFNumberAndNa(std::vector<DerivedQuantity>& out, double f, double r) {
    if (f <= 0.0 || r <= 0.0) return;
    out.push_back(dq(QStringLiteral("f-number"), f / (2.0 * r), 2, QStringLiteral("f/"),
                     QStringLiteral("Focal length over clear aperture diameter. Smaller "
                                    "collects more light and aberrates harder.")));
    const double na = std::sin(std::atan2(r, f));
    out.push_back(dq(QStringLiteral("Numerical aperture"), na, 3, QString(),
                     QStringLiteral("sin of the marginal ray angle, n = 1 outside the optic. "
                                    "The same fact as the f-number, in the units a fibre or "
                                    "an objective is specified in.")));
}

// Etendue of a circular aperture radius r filled to half-angle theta: the
// conserved quantity that says what a concentrator cannot beat.
void addEtendue(std::vector<DerivedQuantity>& out, double r, double thetaRad) {
    if (r <= 0.0 || thetaRad <= 0.0) return;
    const double s = std::sin(thetaRad);
    out.push_back(dq(QStringLiteral("Etendue"), kPi * kPi * r * r * s * s, 1,
                     QStringLiteral("mm^2 sr"),
                     QStringLiteral("Area times projected solid angle. No passive optic can "
                                    "reduce it, which is why a concentrator has a limit at all.")));
}

} // namespace

std::vector<DerivedQuantity> GeometryProvider::derived(Scene scene, const SceneParams& raw) {
    const SceneParams P = sanitise(scene, raw);
    std::vector<DerivedQuantity> out;
    const double detZ = [&] {
        const auto& infos = paramInfo(scene);
        return infos.empty() ? 0.0 : P.v[infos.size() - 1];
    }();

    // n at the d line for the glasses the library is built from, so the paraxial
    // numbers below match what the tracer will actually do.
    constexpr double nCrown  = 1.5168;   // N-BK7
    constexpr double nFlint  = 1.7847;   // N-SF11
    constexpr double nPolymr = 1.4906;   // PMMA

    switch (scene) {
    case Scene::Reflector: {
        const double f = P.v[0], r = P.v[1];
        addFNumberAndNa(out, f, r);
        out.push_back(dq(QStringLiteral("Rim angle"),
                         2.0 * std::atan2(r, f - r * r / (4.0 * f)) / kPi * 180.0, 1,
                         QStringLiteral("deg"),
                         QStringLiteral("Full angle the dish subtends from its focus. Past "
                                        "about 90 degrees the rim starts shadowing itself.")));
        out.push_back(dq(QStringLiteral("Rim depth"), r * r / (4.0 * f), 1, QStringLiteral("mm"),
                         QStringLiteral("How far the rim stands proud of the vertex.")));
        out.push_back(dq(QStringLiteral("Collected solid angle"),
                         2.0 * kPi * (1.0 - std::cos(std::atan2(r, f))) , 2,
                         QStringLiteral("sr"),
                         QStringLiteral("Of the 4 pi a point source radiates into, this is the "
                                        "share the dish sees.")));
        break;
    }
    case Scene::EllipticalReflector: {
        const double a = P.v[0], b = P.v[1];
        if (a > b) {
            const double c = std::sqrt(a * a - b * b);
            out.push_back(dq(QStringLiteral("Foci at z"), c, 1, QStringLiteral("+/- mm"),
                             QStringLiteral("An ellipse images one focus onto the other exactly, "
                                            "so the source belongs at one and the receiver at the other.")));
            out.push_back(dq(QStringLiteral("Focus separation"), 2.0 * c, 1, QStringLiteral("mm"),
                             QStringLiteral("Distance between the two foci.")));
            out.push_back(dq(QStringLiteral("Eccentricity"), c / a, 3, QString(),
                             QStringLiteral("0 is a sphere, 1 is a parabola.")));
            out.push_back(dqText(QStringLiteral("Receiver vs far focus"),
                                 QString::number(detZ - c, 'f', 1), QStringLiteral("mm"),
                                 QStringLiteral("Zero puts the receiver exactly at the image "
                                                "of the source.")));
        }
        break;
    }
    case Scene::SphericalReflector: {
        const double R = P.v[0], half = P.v[1] * kPi / 180.0;
        out.push_back(dq(QStringLiteral("Paraxial focus"), 0.5 * R, 1, QStringLiteral("mm"),
                         QStringLiteral("R/2 from the vertex. Only the paraxial rays meet there, "
                                        "which is exactly what this scene shows.")));
        const double r = R * std::sin(half);
        addFNumberAndNa(out, 0.5 * R, r);
        // Longitudinal spherical aberration of a mirror, marginal against paraxial.
        const double lsa = 0.5 * R * (1.0 / std::cos(half) - 1.0);
        out.push_back(dq(QStringLiteral("Marginal focus shift"), lsa, 2, QStringLiteral("mm"),
                         QStringLiteral("How far short of the paraxial focus the rim rays cross. "
                                        "This is the spherical aberration, in closed form.")));
        break;
    }
    case Scene::OffAxisParabola: {
        const double f = P.v[0], off = P.v[1];
        out.push_back(dq(QStringLiteral("Off-axis angle"),
                         2.0 * std::atan2(off, 2.0 * f) / kPi * 180.0, 1, QStringLiteral("deg"),
                         QStringLiteral("How far the segment's centre sits off the parent axis.")));
        out.push_back(dq(QStringLiteral("Parent focal length"), f, 1, QStringLiteral("mm"),
                         QStringLiteral("Of the full paraboloid the segment was cut from.")));
        break;
    }
    case Scene::ParabolicTrough: {
        const double f = P.v[0], w = P.v[1];
        out.push_back(dq(QStringLiteral("Rim angle"), 2.0 * std::atan2(w, f) / kPi * 180.0, 1,
                         QStringLiteral("deg"), QStringLiteral("In the focusing plane only.")));
        out.push_back(dq(QStringLiteral("Geometric concentration"), w / std::max(1.0, 0.05 * w), 1,
                         QStringLiteral("x"),
                         QStringLiteral("Aperture width over an absorber a twentieth as wide -- "
                                        "the ratio a trough is usually quoted at.")));
        break;
    }
    case Scene::Cpc: {
        const double aOut = P.v[0], th = P.v[1] * kPi / 180.0;
        const double s = std::sin(th);
        if (s > 1e-6) {
            out.push_back(dq(QStringLiteral("Entrance radius"), aOut / s, 1, QStringLiteral("mm"),
                             QStringLiteral("a_in = a_out / sin(theta_max), which is what makes "
                                            "the concentration what it is.")));
            out.push_back(dq(QStringLiteral("Concentration"), 1.0 / (s * s), 1, QStringLiteral("x"),
                             QStringLiteral("1 / sin^2(theta_max): the thermodynamic limit, which "
                                            "is exactly what a CPC reaches and nothing beats.")));
            out.push_back(dq(QStringLiteral("Height"), (aOut / s + aOut) / std::tan(th), 1,
                             QStringLiteral("mm"),
                             QStringLiteral("A tight acceptance angle buys concentration and "
                                            "pays for it in length.")));
            addEtendue(out, aOut / s, th);
        }
        break;
    }
    case Scene::ConicalConcentrator: {
        const double rIn = P.v[0], rOut = P.v[1];
        if (rOut > 0.0) {
            out.push_back(dq(QStringLiteral("Area ratio"), (rIn * rIn) / (rOut * rOut), 2,
                             QStringLiteral("x"),
                             QStringLiteral("What a funnel would concentrate by if none of the "
                                            "light turned back. It always does, which is the "
                                            "point of comparing it with a CPC.")));
            out.push_back(dq(QStringLiteral("Ideal acceptance"),
                             std::asin(std::min(1.0, rOut / rIn)) / kPi * 180.0, 1,
                             QStringLiteral("deg"),
                             QStringLiteral("The half-angle a CPC of the same area ratio would "
                                            "accept.")));
        }
        break;
    }
    case Scene::Cassegrain: {
        const double f1 = P.v[0], d = P.v[1];
        out.push_back(dq(QStringLiteral("Primary focus"), f1, 1, QStringLiteral("mm"), QString()));
        out.push_back(dq(QStringLiteral("Back focal distance"), detZ - d, 1, QStringLiteral("mm"),
                         QStringLiteral("From the secondary's vertex to the receiver.")));
        break;
    }
    case Scene::Lens: {
        const double R = P.v[0], s = P.v[1];
        // A ball lens: f measured from its centre.
        const double f = nCrown * R / (2.0 * (nCrown - 1.0));
        out.push_back(dq(QStringLiteral("Effective focal length"), f, 1, QStringLiteral("mm"),
                         QStringLiteral("nR / 2(n-1) from the centre of the ball.")));
        out.push_back(dq(QStringLiteral("Back focal distance"), f - R, 1, QStringLiteral("mm"),
                         QStringLiteral("From the far surface. On a ball lens this is short, "
                                        "which is why they are used against a fibre face.")));
        addFNumberAndNa(out, f, R);
        if (s > f) out.push_back(dq(QStringLiteral("Paraxial image at"),
                                    1.0 / (1.0 / f - 1.0 / s), 1, QStringLiteral("mm"),
                                    QStringLiteral("From the lens centre, by the thin-lens "
                                                   "equation at the d line.")));
        break;
    }
    case Scene::PlanoConvexLens:
    case Scene::BiconvexLens: {
        const double R = P.v[0], s = P.v[1];
        const double f = (scene == Scene::BiconvexLens) ? R / (2.0 * (nCrown - 1.0))
                                                        : R / (nCrown - 1.0);
        out.push_back(dq(QStringLiteral("Focal length"), f, 1, QStringLiteral("mm"),
                         (scene == Scene::BiconvexLens)
                             ? QStringLiteral("1/f = (n-1)(1/R1 - 1/R2) with both radii equal, "
                                              "so R/2(n-1) at n = 1.5168.")
                             : QStringLiteral("f = R / (n - 1), the lensmaker's equation with "
                                              "one flat surface, at n = 1.5168.")));
        addFNumberAndNa(out, f, 0.35 * R);
        if (s > f)
            out.push_back(dq(QStringLiteral("Paraxial image at"), 1.0 / (1.0 / f - 1.0 / s), 1,
                             QStringLiteral("mm"),
                             QStringLiteral("Where the thin-lens equation puts the image of the "
                                            "source. Compare it with the receiver position.")));
        else
            out.push_back(dqText(QStringLiteral("Paraxial image at"), QStringLiteral("virtual"),
                                 QString(),
                                 QStringLiteral("The source is inside the focal length, so the "
                                                "image is on the same side and the beam diverges.")));
        out.push_back(dq(QStringLiteral("Receiver vs image"),
                         (s > f) ? detZ - 1.0 / (1.0 / f - 1.0 / s) : 0.0, 1,
                         QStringLiteral("mm"),
                         QStringLiteral("Zero means the receiver is at the paraxial focus, where "
                                        "the residual spot is aberration alone.")));
        break;
    }
    case Scene::PlanoConcaveLens: {
        const double R = P.v[0];
        out.push_back(dq(QStringLiteral("Focal length"), -R / (nCrown - 1.0), 1,
                         QStringLiteral("mm"),
                         QStringLiteral("Negative: a diverging lens has a virtual focus on the "
                                        "source side.")));
        break;
    }
    case Scene::HalfBallLens: {
        const double R = P.v[0];
        out.push_back(dq(QStringLiteral("Escape cone half-angle"),
                         std::asin(1.0 / nCrown) / kPi * 180.0, 1, QStringLiteral("deg"),
                         QStringLiteral("Inside the dome, only rays within this of the local "
                                        "normal get out. A hemisphere over the die makes every "
                                        "ray normal to the surface, which is the trick.")));
        out.push_back(dq(QStringLiteral("Dome radius"), R, 1, QStringLiteral("mm"), QString()));
        break;
    }
    case Scene::CylindricalLens: {
        const double R = P.v[0];
        out.push_back(dq(QStringLiteral("Focal length"), nCrown * R / (2.0 * (nCrown - 1.0)), 1,
                         QStringLiteral("mm"),
                         QStringLiteral("nR / 2(n-1) from the rod axis. It focuses one axis and "
                                        "leaves the other alone, which is what makes a line.")));
        break;
    }
    case Scene::FresnelLens: {
        const double r = P.v[0], zones = P.v[1], s = P.v[2];
        out.push_back(dq(QStringLiteral("Zone width"), r / std::max(1.0, zones), 1,
                         QStringLiteral("mm"),
                         QStringLiteral("Narrower zones approximate the curve better and lose "
                                        "more light to the risers between them.")));
        addFNumberAndNa(out, s, r);
        break;
    }
    case Scene::Axicon: {
        const double r = P.v[0], h = P.v[1];
        const double alpha = std::atan2(h, r);            // base angle of the cone
        const double deviation = (nPolymr - 1.0) * alpha; // small-angle deviation
        out.push_back(dq(QStringLiteral("Cone half-angle"), 90.0 - alpha / kPi * 180.0, 1,
                         QStringLiteral("deg"), QStringLiteral("Measured from the axis.")));
        out.push_back(dq(QStringLiteral("Ray deviation"), deviation / kPi * 180.0, 2,
                         QStringLiteral("deg"),
                         QStringLiteral("(n-1) times the base angle: the angle every ray is bent "
                                        "by, which is what turns a beam into a ring.")));
        if (deviation > 1e-9)
            out.push_back(dq(QStringLiteral("Ring radius at receiver"),
                             std::max(0.0, detZ) * std::tan(deviation), 1, QStringLiteral("mm"),
                             QStringLiteral("The deviation times the throw. A ring, not a spot.")));
        break;
    }
    case Scene::MicrolensArray: {
        const double R = P.v[0], pitch = P.v[1];
        out.push_back(dq(QStringLiteral("Lenslet focal length"),
                         nPolymr * R / (2.0 * (nPolymr - 1.0)), 1, QStringLiteral("mm"),
                         QStringLiteral("Each ball is its own lens.")));
        out.push_back(dq(QStringLiteral("Fill factor"),
                         kPi * R * R / std::max(1e-9, pitch * pitch), 3, QString(),
                         QStringLiteral("Lenslet area over cell area. Above about 0.79 the balls "
                                        "have started to overlap.")));
        out.push_back(dq(QStringLiteral("Array width"), 5.0 * pitch, 1, QStringLiteral("mm"),
                         QStringLiteral("Five lenslets across.")));
        break;
    }
    case Scene::Prism: {
        const double w = P.v[0], h = P.v[1];
        const double apex = 2.0 * std::atan2(w, h);
        out.push_back(dq(QStringLiteral("Apex angle"), apex / kPi * 180.0, 1, QStringLiteral("deg"),
                         QString()));
        const double arg = nFlint * std::sin(0.5 * apex);
        if (arg <= 1.0) {
            const double dev = 2.0 * std::asin(arg) - apex;
            out.push_back(dq(QStringLiteral("Minimum deviation"), dev / kPi * 180.0, 2,
                             QStringLiteral("deg"),
                             QStringLiteral("2 asin(n sin(A/2)) - A at the d line, for the "
                                            "symmetric passage. The smallest bend this prism "
                                            "can give.")));
            const double devF = 2.0 * std::asin(std::min(1.0, materials::byName(
                                    QStringLiteral("N-SF11")).indexAt(486.1) *
                                    std::sin(0.5 * apex))) - apex;
            const double devC = 2.0 * std::asin(std::min(1.0, materials::byName(
                                    QStringLiteral("N-SF11")).indexAt(656.3) *
                                    std::sin(0.5 * apex))) - apex;
            out.push_back(dq(QStringLiteral("Angular dispersion"),
                             (devF - devC) / kPi * 180.0, 3, QStringLiteral("deg"),
                             QStringLiteral("Blue minus red, F to C line. This is the spread the "
                                            "prism was built for.")));
        } else {
            out.push_back(dqText(QStringLiteral("Minimum deviation"),
                                 QStringLiteral("total internal reflection"), QString(),
                                 QStringLiteral("The apex is too steep for light to leave the "
                                                "second face at all.")));
        }
        break;
    }
    case Scene::LightGuide:
    case Scene::SquareLightGuide: {
        const double r = P.v[0], len = P.v[1];
        const double crit = std::asin(1.0 / nCrown);
        out.push_back(dq(QStringLiteral("Critical angle"), crit / kPi * 180.0, 1,
                         QStringLiteral("deg"),
                         QStringLiteral("From the wall normal. Steeper than this and the ray "
                                        "leaves instead of bouncing.")));
        out.push_back(dq(QStringLiteral("Acceptance half-angle"),
                         std::asin(std::min(1.0, std::sqrt(nCrown * nCrown - 1.0))) / kPi * 180.0,
                         1, QStringLiteral("deg"),
                         QStringLiteral("In air, at the entrance face: the numerical aperture of "
                                        "the guide, sqrt(n^2 - 1).")));
        out.push_back(dq(QStringLiteral("Aspect ratio"), len / std::max(1e-9, 2.0 * r), 1,
                         QStringLiteral("x"),
                         QStringLiteral("Length over width. It sets how many bounces an off-axis "
                                        "ray makes, and therefore how much the walls cost.")));
        break;
    }
    case Scene::TaperedLightGuide: {
        const double rIn = P.v[0], rOut = P.v[1], len = P.v[2];
        out.push_back(dq(QStringLiteral("Taper half-angle"),
                         std::atan2(rIn - rOut, len) / kPi * 180.0, 2, QStringLiteral("deg"),
                         QStringLiteral("Every bounce steepens a ray by twice this, which is "
                                        "what eventually pushes it past the critical angle.")));
        if (rOut > 0.0)
            out.push_back(dq(QStringLiteral("Area ratio"), (rIn * rIn) / (rOut * rOut), 2,
                             QStringLiteral("x"),
                             QStringLiteral("What it would concentrate by if nothing leaked. "
                                            "Etendue says something always does.")));
        break;
    }
    case Scene::PorroPrism: {
        const double w = P.v[0], h = P.v[1];
        out.push_back(dq(QStringLiteral("Roof angle"), std::atan2(h, w) / kPi * 180.0, 1,
                         QStringLiteral("deg"),
                         QStringLiteral("45 degrees is the retroreflecting case: height equal to "
                                        "half width.")));
        out.push_back(dq(QStringLiteral("Critical angle"),
                         std::asin(1.0 / nCrown) / kPi * 180.0, 1, QStringLiteral("deg"),
                         QStringLiteral("The roof faces work by total internal reflection alone, "
                                        "with no coating at all.")));
        break;
    }
    case Scene::IntegratingSphere: {
        const double R = P.v[0], rho = P.v[1], port = P.v[2];
        const double sphereArea = 4.0 * kPi * R * R;
        const double f = (port * port) / sphereArea;
        out.push_back(dq(QStringLiteral("Port fraction"), f, 4, QString(),
                         QStringLiteral("Port area over sphere area. Everything about a sphere's "
                                        "throughput follows from this and the wall reflectance.")));
        const double denom = 1.0 - rho * (1.0 - f);
        if (denom > 1e-9)
            out.push_back(dq(QStringLiteral("Sphere multiplier"), rho / denom, 1,
                             QStringLiteral("x"),
                             QStringLiteral("M = rho / (1 - rho(1-f)). How many times the light "
                                            "goes round before it finds the port.")));
        out.push_back(dq(QStringLiteral("Expected throughput"),
                         100.0 * (f + (1.0 - f) * f * rho / std::max(1e-9, denom)), 2,
                         QStringLiteral("%"),
                         QStringLiteral("Straight out through the port, plus everything that "
                                        "comes back round. The trace should land on this.")));
        break;
    }
    case Scene::DiffuserPlate: {
        const double size = P.v[0], diffusion = P.v[1];
        out.push_back(dq(QStringLiteral("Diffuse fraction"), diffusion, 2, QString(),
                         QStringLiteral("How much of what leaves the far face is cosine-weighted "
                                        "rather than refracted straight on.")));
        out.push_back(dq(QStringLiteral("Throw ratio"), std::max(0.0, detZ) / std::max(1e-9, size),
                         2, QStringLiteral("x"),
                         QStringLiteral("Receiver distance over plate width. A Lambertian plate "
                                        "spreads over roughly this much.")));
        break;
    }
    case Scene::LedArrayLuminaire: {
        const LedArray a = ledArray(P);
        addFNumberAndNa(out, a.focal, a.radius);
        out.push_back(dq(QStringLiteral("Sources to add"), double(a.cups - 1), 0,
                         QString(),
                         QStringLiteral("The scene places one emitter, over the first cup. "
                                        "This many more light the rest; a run with fewer "
                                        "reports exactly the dark cups it has.")));
        QString offsets;
        for (int i = 1; i < a.cups; ++i) {
            if (!offsets.isEmpty()) offsets += QStringLiteral(", ");
            offsets += QString::number(double(i) * a.pitch, 'f', 0);
        }
        out.push_back(dqText(QStringLiteral("At x offsets"), offsets, QStringLiteral("mm"),
                             QStringLiteral("Offsets from the scene emitter, for the "
                                            "Position box of the source dialog. Add opens "
                                            "with the next one already filled in.")));
        out.push_back(dq(QStringLiteral("Array span"), a.span(), 1, QStringLiteral("mm"),
                         QStringLiteral("Rim to rim across the row. The receiver is sized "
                                        "from it, so widening the pitch widens the plane "
                                        "rather than pushing beams off it.")));
        out.push_back(dq(QStringLiteral("Rim angle"),
                         2.0 * std::atan2(a.radius,
                                          a.focal - a.radius * a.radius / (4.0 * a.focal))
                             / kPi * 180.0,
                         1, QStringLiteral("deg"),
                         QStringLiteral("Full angle one cup subtends from its own focus. "
                                        "The share of a hemisphere it fails to catch is "
                                        "the spill the receiver sees around the beams.")));
        break;
    }
    case Scene::ShowcaseLuminaire: {
        const Fixture x = fixture(P);
        addFNumberAndNa(out, x.focal, x.cupRadius);

        out.push_back(dq(QStringLiteral("Rim angle"),
                         2.0 * std::atan2(x.cupRadius, x.focal - x.rimZ()) / kPi * 180.0,
                         1, QStringLiteral("deg"),
                         QStringLiteral("Full angle the rim subtends from the die. What "
                                        "the cup fails to catch of the die's hemisphere "
                                        "leaves as spill, which is the light that lands "
                                        "on the deck rather than on the receiver.")));

        out.push_back(dq(QStringLiteral("Cup depth"), x.rimZ(), 1, QStringLiteral("mm"),
                         QStringLiteral("Rim height above the vertex. Deeper than the "
                                        "focus means the die sits inside the cup, which "
                                        "is what makes the render show a lit cavity "
                                        "rather than a bare emitter.")));

        // The etendue limit, stated as the divergence it forces. A point source
        // at the focus of a paraboloid collimates exactly; a real die of finite
        // size cannot, and this is the angle it cannot beat.
        out.push_back(dq(QStringLiteral("Source-limited spread"),
                         2.0 * std::atan2(0.5 * x.die, x.focal) / kPi * 180.0, 2,
                         QStringLiteral("deg"),
                         QStringLiteral("Full beam angle the die's own size forces on a "
                                        "perfect cup: it is the angle the die subtends "
                                        "from the vertex, and no reflector shape removes "
                                        "it. Shrink the die or lengthen the focus to "
                                        "beat it; the far-field plot is the measurement.")));

        out.push_back(dq(QStringLiteral("Cover diffusion"), x.diffusion, 2, QString(),
                         QStringLiteral("Fraction of what leaves the cover that is "
                                        "scattered rather than refracted straight on. "
                                        "It buys the glow and the soft edge, and it "
                                        "costs beam intensity -- the far-field plot is "
                                        "where the trade is read.")));

        out.push_back(dq(QStringLiteral("Die etendue"),
                         kPi * x.die * x.die, 1, QStringLiteral("mm^2 sr"),
                         QStringLiteral("Area times projected solid angle for a "
                                        "Lambertian square over a hemisphere, pi A. No "
                                        "passive optic reduces it, which is the same "
                                        "statement as the spread above.")));
        break;
    }

    case Scene::CornerCube:
    case Scene::Count:
        break;
    }

    if (!out.empty() || scene == Scene::CornerCube)
        out.push_back(dq(QStringLiteral("Receiver at z"), detZ, 1, QStringLiteral("mm"),
                         QStringLiteral("Where the measurement plane sits. Moving it is a "
                                        "parameter change, not a special case.")));
    return out;
}

std::vector<gp_Pnt> GeometryProvider::sourceOffsets(Scene scene, const SceneParams& raw) {
    const SceneParams P = sanitise(scene, raw);
    std::vector<gp_Pnt> out;
    if (scene != Scene::LedArrayLuminaire) return out;

    // One offset per cup after the first, along the row. Relative, not
    // absolute: the row is centred on x = 0, so its cups move when the pitch or
    // the count does, and an offset from the scene emitter follows them while a
    // world coordinate would not.
    const LedArray a = ledArray(P);
    out.reserve(std::size_t(a.cups - 1));
    for (int i = 1; i < a.cups; ++i) out.emplace_back(double(i) * a.pitch, 0.0, 0.0);
    return out;
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
        // One moulded acrylic lenslet, placed twenty-five times. Because only
        // one is ever tessellated, it can carry the same fine mesh every other
        // optic in the library does rather than the coarsened one twenty-five
        // copies used to need.
        OpticalSurface array = glass(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), R).Shape(),
                                     QStringLiteral("Microlens Array"), false, kPolymer);
        array.placements = gridPlacements(5, 5, pitch);
        s.push_back(array);
        s.push_back(detector(std::max(200.0, 8.0 * pitch), dz));
        break;
    }

    case Scene::Prism: {
        const double halfW = P.v[0], apex = P.v[1];
        // A dense flint: four times the dispersion of a crown, which is what a
        // dispersing prism is actually made of and what makes the spread visible.
        s.push_back(glass(extrudeAlongY({gp_Pnt(-halfW, 0, 0.0), gp_Pnt(halfW, 0, 0.0),
                                         gp_Pnt(0.0, 0, apex)}, 120.0, true),
                          QStringLiteral("Glass Prism"), false, kFlintGlass));
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
            QStringLiteral("Diffuser Plate"), false, kPolymer, nullptr);
        plate.scatter = std::clamp(diffusion, 0.0, 1.0);
        // A filled polymer diffuses in its bulk, not at its faces: the particles
        // are through the whole thickness. The surface fraction above stays as
        // the etched-face part of the same plate.
        plate.volume.coefficient = 0.02 * std::clamp(diffusion, 0.0, 1.0);
        plate.volume.anisotropy  = 0.6;      // forward scattering, as a filler is
        s.push_back(plate);
        s.push_back(detector(std::max(300.0, 3.2 * size), dz));
        break;
    }

    // -------------------------------------------------------- multi-source --
    case Scene::LedArrayLuminaire: {
        const LedArray a = ledArray(P);

        // One cup, tessellated once and placed `cups` times. An eight-cup row
        // costs the mesh and the hierarchy of a single cup, which is what makes
        // widening the array a free parameter rather than a rebuild to wait on.
        OpticalSurface cups = mirror(paraboloid(a.focal, 0.0, a.radius),
                                     QStringLiteral("Reflector Cups"));
        cups.placements.reserve(std::size_t(a.cups));
        for (int i = 0; i < a.cups; ++i) {
            gp_Trsf t;
            t.SetTranslation(gp_Vec(a.x0 + double(i) * a.pitch, 0.0, 0.0));
            cups.placements.push_back(t);
        }
        s.push_back(cups);
        // Wide enough for every beam plus the spill around them, so a cup that
        // is lit always lands on the plane and an unlit one reads as the dark
        // patch it is rather than as light that fell off the edge.
        s.push_back(detector(std::max(300.0, 1.8 * a.span()), dz));

        // The scene's own emitter belongs to the first cup. Every other cup is
        // a source the user adds -- see sourceOffsets, which says where.
        out.sourceOrigin = gp_Pnt(a.x0, 0.0, a.focal);
        out.sourceAxis   = gp_Dir(0, 0, -1);
        break;
    }

    // ------------------------------------------------------------ showcase --
    case Scene::ShowcaseLuminaire: {
        const Fixture x = fixture(P);

        // Aluminium, which is the part of this scene that pays for the
        // per-channel complex index: a mirror with nothing around it to reflect
        // shows none of it, and a mirror inside a housing on a floor shows all
        // of it.
        //
        // Lightly peened rather than optically polished, which is how a
        // luminaire reflector is actually finished: a mirror finish images the
        // die onto the wall, and the texture is there to stop it. A few percent
        // of scatter, so the beam is softened and not spoiled.
        OpticalSurface cup = mirror(paraboloid(x.focal, x.vertexHole(), x.cupRadius),
                                    QStringLiteral("Reflector Cup"));
        cup.scatter = 0.06;
        s.push_back(cup);

        // The body: a back plate and a wall, in one shell of revolution. Dark
        // anodised rather than black, because a housing that returns nothing at
        // all is a silhouette and not a part.
        s.push_back(matte(revolvePolyline({gp_Pnt(0, 0, x.backZ()),
                                           gp_Pnt(0, x.bezelRadius(), x.backZ()),
                                           gp_Pnt(0, x.bezelRadius(), x.rimZ() + 2.0)}),
                          QStringLiteral("Housing"), 0.14));

        // The cover, as a sphere cut to a disc: a shallow cap with a short
        // cylindrical edge, which is how a moulded cover actually leaves its
        // tool. Uncoated acrylic, because a luminaire cover is not a coated
        // lens and pretending otherwise would flatter the efficiency.
        //
        // This is the part that makes the fixture read as switched on. A clear
        // cover over a specular cup sends the whole beam up the axis and shows
        // a camera off to one side nothing at all -- correct, and a black
        // photograph. An opal cover scatters some of that beam in every
        // direction, which is what makes an aperture glow, and it is also what
        // the diffusion knob is for optically: the beam widens because the
        // cover is doing something, not because the picture wanted it to.
        const double zLens = x.rimZ();
        const TopoDS_Shape dome =
            BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, zLens + x.lensSag() - x.lensRadius()),
                                   x.lensRadius()).Shape();
        const TopoDS_Shape band =
            BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, zLens), gp_Dir(0, 0, 1)),
                                     x.cupRadius, x.lensSag() + 2.0).Shape();
        OpticalSurface cover = glass(BRepAlgoAPI_Common(dome, band).Shape(),
                                     QStringLiteral("Opal Cover"), false, kPolymer,
                                     nullptr);
        cover.scatter = std::clamp(x.diffusion, 0.0, 1.0);
        // A filled acrylic diffuses through its thickness as well as at its
        // faces, exactly as the diffuser plate scene models it.
        cover.volume.coefficient = 0.015 * std::clamp(x.diffusion, 0.0, 1.0);
        cover.volume.anisotropy  = 0.6;
        s.push_back(cover);

        // The base: a plinth the housing sits on, which is where a real fixture
        // puts its heat sink and its cable gland.
        s.push_back(matte(BRepPrimAPI_MakeCylinder(
                              gp_Ax2(gp_Pnt(0, 0, x.baseZ()), gp_Dir(0, 0, 1)),
                              x.baseRadius(), x.baseHeight()).Shape(),
                          QStringLiteral("Base"), 0.18));

        // The floor -- see floorPlate for why it is behind the optic and why it
        // is not in the measurement. Light grey rather than white: a bench top,
        // and something for the cup's spill to land on.
        s.push_back(matte(floorPlate(x.floorHalfSide(), x.baseZ()),
                          QStringLiteral("Floor"), 0.45));

        s.push_back(detector(x.receiverSize(), dz));

        // The die, at the focus, facing into the cup.
        out.sourceOrigin = gp_Pnt(0, 0, x.focal);
        out.sourceAxis   = gp_Dir(0, 0, -1);
        break;
    }

    case Scene::Count:
        break;
    }

    return out;
}
