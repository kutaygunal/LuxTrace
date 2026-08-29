#include "SceneDocument.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>

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
#include <GeomAPI_PointsToBSpline.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Plane.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopLoc_Location.hxx>
#include <gp.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

#include "ConfigIO.h"
#include "Coating.h"
#include "Material.h"

namespace scenedoc {
namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

// The catalogue names an assembled scene is built from, matching the ones the
// registry's own scenes use. A part says "N-BK7" the way a drawing does.
const char* const kGlass    = "N-BK7";
const char* const kFlint    = "N-SF11";
const char* const kMetal    = "Aluminium";
const char* const kArCoat   = "Broadband AR";

void applyMaterial(SurfaceOptics& o, const char* name) {
    const OpticalMaterial m = materials::byName(QLatin1String(name));
    if (!m.valid()) return;
    o.material = m;
    if (m.isMetal()) return;      // opaque: the material supplies a complex index
    o.index      = m.nd;
    o.absorption = m.alpha;
}

// ---- shape helpers ---------------------------------------------------------
// Profiles are given in the plane x = 0 as (0, r, z) and revolved about +Z, the
// same convention GeometryProvider builds its optics in.

TopoDS_Shape revolveSpline(const std::vector<gp_Pnt>& profile) {
    TColgp_Array1OfPnt pts(1, int(profile.size()));
    for (int i = 0; i < int(profile.size()); ++i) pts.SetValue(i + 1, profile[std::size_t(i)]);
    Handle(Geom_BSplineCurve) curve = GeomAPI_PointsToBSpline(pts).Curve();
    BRepBuilderAPI_MakeEdge edge(curve);
    return BRepPrimAPI_MakeRevol(edge.Edge(), gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
}

// Profile in the x-z plane, extruded along y and centred on y = 0.
TopoDS_Shape extrudeAlongY(const std::vector<gp_Pnt>& profileXZ, double length) {
    BRepBuilderAPI_MakePolygon poly;
    for (const auto& p : profileXZ) poly.Add(gp_Pnt(p.X(), -0.5 * length, p.Z()));
    poly.Close();
    BRepBuilderAPI_MakeFace face(poly.Wire());
    return BRepPrimAPI_MakePrism(face.Face(), gp_Vec(0, length, 0)).Shape();
}

// Planar square perpendicular to +Z, centred on the local origin.
TopoDS_Shape makePlate(double size) {
    const double h = 0.5 * size;
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(-h, -h, 0.0));
    poly.Add(gp_Pnt(h, -h, 0.0));
    poly.Add(gp_Pnt(h, h, 0.0));
    poly.Add(gp_Pnt(-h, h, 0.0));
    poly.Close();
    Handle(Geom_Plane) plane = new Geom_Plane(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)));
    BRepBuilderAPI_MakeFace face(plane, poly.Wire(), true);
    return face.Shape();
}

// Paraboloid z = r^2 / (4f), from the vertex out to `aperture`. Its focus is at
// z = f on the axis, which is the whole reason the vertex is the anchor.
TopoDS_Shape paraboloid(double f, double aperture, int n = 40) {
    std::vector<gp_Pnt> profile;
    profile.reserve(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        const double r = aperture * double(i) / double(n - 1);
        profile.emplace_back(0.0, r, (r * r) / (4.0 * f));
    }
    return revolveSpline(profile);
}

// Spherical cap of curvature R out to `aperture`, vertex at the local origin
// and centre of curvature at z = R.
TopoDS_Shape sphericalCap(double R, double aperture, int n = 32) {
    const double rMax = std::min(aperture, 0.999 * R);
    std::vector<gp_Pnt> profile;
    profile.reserve(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        const double r = rMax * double(i) / double(n - 1);
        profile.emplace_back(0.0, r, R - std::sqrt(std::max(0.0, R * R - r * r)));
    }
    return revolveSpline(profile);
}

// ---- the type registry -----------------------------------------------------

SceneParamInfo prm(const char* name, const char* unit, double mn, double mx,
                   double def, double step, int dec, const char* tip) {
    SceneParamInfo p;
    p.name     = QString::fromLatin1(name);
    p.unit     = QString::fromLatin1(unit);
    p.min      = mn;
    p.max      = mx;
    p.def      = def;
    p.step     = step;
    p.decimals = dec;
    p.tip      = QString::fromLatin1(tip);
    return p;
}

std::vector<TypeInfo> buildRegistry() {
    const std::size_t    kTypes = static_cast<std::size_t>(ObjectType::Count);
    std::vector<TypeInfo> t(kTypes);
    auto set = [&t](ObjectType type, const char* key, const char* name, Category cat,
                    const char* desc, std::vector<SceneParamInfo> params) {
        TypeInfo& i  = t[std::size_t(type)];
        i.key        = QString::fromLatin1(key);
        i.name       = QString::fromLatin1(name);
        i.category   = cat;
        i.description = QString::fromLatin1(desc);
        i.params     = std::move(params);
    };

    set(ObjectType::Group, "group", "Group", Category::Other,
        "A folder with a transform: moving it moves everything under it.", {});

    set(ObjectType::Source, "source", "Light source", Category::Sources,
        "An emitter. Its angular law, spectrum and flux are set on the object; "
        "where it sits and which way it points are its placement like anything else.",
        {});

    // --- refractive ---
    set(ObjectType::BallLens, "ball-lens", "Ball lens", Category::Lenses,
        "A solid glass sphere. Short focal length, strong spherical aberration -- "
        "the fibre-coupling optic.",
        {prm("Radius", "mm", 1.0, 200.0, 25.0, 1.0, 1, "Sphere radius.")});

    set(ObjectType::PlanoConvexLens, "plano-convex", "Plano-convex lens", Category::Lenses,
        "Flat face on the local z = 0 plane, convex cap bulging toward +Z.",
        {prm("Radius of curvature", "mm", 5.0, 500.0, 60.0, 1.0, 1,
             "Curvature of the convex face. The aperture follows from it.")});

    set(ObjectType::BiconvexLens, "biconvex", "Biconvex lens", Category::Lenses,
        "Two convex faces of equal curvature, centred on the object origin.",
        {prm("Radius of curvature", "mm", 5.0, 500.0, 80.0, 1.0, 1,
             "Curvature of both faces."),
         prm("Centre thickness", "mm", 1.0, 200.0, 24.0, 1.0, 1,
             "Thickness on the axis.")});

    set(ObjectType::PlanoConcaveLens, "plano-concave", "Plano-concave lens", Category::Lenses,
        "A cylindrical blank with a spherical dish cut into its +Z face. Diverging.",
        {prm("Dish radius", "mm", 5.0, 500.0, 60.0, 1.0, 1,
             "Curvature of the concave face."),
         prm("Blank radius", "mm", 2.0, 200.0, 30.0, 1.0, 1, "Outer radius."),
         prm("Thickness", "mm", 2.0, 200.0, 25.0, 1.0, 1, "Edge thickness of the blank.")});

    set(ObjectType::CylindricalLens, "cylindrical-lens", "Cylindrical rod lens", Category::Lenses,
        "A glass rod along the local Y axis: it focuses one axis and leaves the other alone.",
        {prm("Radius", "mm", 1.0, 100.0, 12.0, 0.5, 1, "Rod radius."),
         prm("Length", "mm", 5.0, 1000.0, 120.0, 5.0, 1, "Rod length along Y.")});

    set(ObjectType::Axicon, "axicon", "Axicon", Category::Lenses,
        "A glass cone. Fed a collimated beam it makes a ring rather than a spot.",
        {prm("Base radius", "mm", 1.0, 200.0, 30.0, 1.0, 1, "Radius of the flat base."),
         prm("Height", "mm", 1.0, 200.0, 20.0, 1.0, 1, "Apex height above the base.")});

    set(ObjectType::Prism, "prism", "Dispersing prism", Category::Lenses,
        "A triangular flint-glass prism extruded along Y. Four times the dispersion "
        "of a crown, which is what makes the spread visible.",
        {prm("Half width", "mm", 2.0, 300.0, 40.0, 1.0, 1, "Half the base width."),
         prm("Apex height", "mm", 2.0, 300.0, 60.0, 1.0, 1, "Height of the apex above the base."),
         prm("Length", "mm", 2.0, 500.0, 120.0, 5.0, 1, "Extrusion along Y.")});

    // --- reflective ---
    set(ObjectType::FlatMirror, "flat-mirror", "Flat mirror", Category::Mirrors,
        "A square front-surface aluminium mirror in the local z = 0 plane, facing +Z.",
        {prm("Size", "mm", 2.0, 2000.0, 80.0, 5.0, 1, "Edge length.")});

    set(ObjectType::SphericalMirror, "spherical-mirror", "Spherical mirror", Category::Mirrors,
        "A spherical cap, vertex at the object origin and centre of curvature at +R. "
        "Focuses at R/2, with the spherical aberration that implies.",
        {prm("Radius of curvature", "mm", 5.0, 5000.0, 200.0, 5.0, 1, "Curvature radius."),
         prm("Aperture radius", "mm", 2.0, 1000.0, 50.0, 2.0, 1, "Half the clear aperture.")});

    set(ObjectType::ParabolicMirror, "parabolic-mirror", "Parabolic mirror", Category::Mirrors,
        "A paraboloid with its vertex at the object origin and its focus at +f on the "
        "axis. A source at the focus leaves collimated.",
        {prm("Focal length", "mm", 2.0, 2000.0, 50.0, 2.0, 1, "Distance from vertex to focus."),
         prm("Aperture radius", "mm", 2.0, 1000.0, 60.0, 2.0, 1, "Half the clear aperture.")});

    // --- total internal reflection ---
    set(ObjectType::RodGuide, "rod-guide", "Round light guide", Category::Guides,
        "An uncoated glass rod along +Z. Works by total internal reflection, so it is "
        "deliberately left bare.",
        {prm("Radius", "mm", 0.5, 100.0, 10.0, 0.5, 1, "Rod radius."),
         prm("Length", "mm", 2.0, 2000.0, 150.0, 5.0, 1, "Length along Z.")});

    set(ObjectType::SquareGuide, "square-guide", "Square light guide", Category::Guides,
        "A square-section glass bar along +Z. Mixes an LED's near field into a "
        "uniform patch.",
        {prm("Width", "mm", 0.5, 200.0, 20.0, 1.0, 1, "Section width and height."),
         prm("Length", "mm", 2.0, 2000.0, 150.0, 5.0, 1, "Length along Z.")});

    set(ObjectType::TaperedGuide, "tapered-guide", "Tapered light guide", Category::Guides,
        "A truncated glass cone along +Z. Trades area for angle, both ways.",
        {prm("Entry radius", "mm", 0.5, 200.0, 20.0, 1.0, 1, "Radius at z = 0."),
         prm("Exit radius", "mm", 0.2, 200.0, 8.0, 0.5, 1, "Radius at the far end."),
         prm("Length", "mm", 2.0, 2000.0, 120.0, 5.0, 1, "Length along Z.")});

    // --- plain bodies ---
    set(ObjectType::Box, "box", "Glass block", Category::Bodies,
        "A rectangular glass block centred on the object origin.",
        {prm("X size", "mm", 0.5, 2000.0, 40.0, 1.0, 1, "Width."),
         prm("Y size", "mm", 0.5, 2000.0, 40.0, 1.0, 1, "Depth."),
         prm("Z size", "mm", 0.5, 2000.0, 40.0, 1.0, 1, "Height.")});

    set(ObjectType::Sphere, "sphere", "Glass sphere", Category::Bodies,
        "A solid sphere centred on the object origin.",
        {prm("Radius", "mm", 0.5, 1000.0, 25.0, 1.0, 1, "Sphere radius.")});

    set(ObjectType::Cylinder, "cylinder", "Glass cylinder", Category::Bodies,
        "A solid cylinder along +Z, base at the object origin.",
        {prm("Radius", "mm", 0.5, 1000.0, 20.0, 1.0, 1, "Cylinder radius."),
         prm("Height", "mm", 0.5, 2000.0, 60.0, 2.0, 1, "Length along Z.")});

    set(ObjectType::DiffuserPlate, "diffuser", "Diffuser plate", Category::Bodies,
        "A thin transmissive plate that scatters what passes through it -- the "
        "milky panel over every luminaire.",
        {prm("Size", "mm", 2.0, 2000.0, 80.0, 5.0, 1, "Edge length."),
         prm("Thickness", "mm", 0.2, 100.0, 4.0, 0.5, 1, "Plate thickness along Z.")});

    set(ObjectType::Detector, "detector", "Detector", Category::Detectors,
        "A square receiver in the local z = 0 plane facing +Z. Its binning and its "
        "acceptance cone are set with its other optical properties.",
        {prm("Size", "mm", 1.0, 5000.0, 200.0, 10.0, 1, "Edge length.")});

    set(ObjectType::TutorialPart, "tutorial-part", "Tutorial part", Category::Other,
        "One part of a loaded tutorial scene.", {});

    set(ObjectType::ImportedPart, "imported-part", "Imported part", Category::Other,
        "One body read from a CAD file.", {});

    return t;
}

const std::vector<TypeInfo>& registry() {
    static const std::vector<TypeInfo> r = buildRegistry();
    return r;
}

// The optical behaviour a type arrives with. A lens is coated glass, a mirror
// is aluminium, a guide is deliberately bare: these are what the parts are made
// of, not arbitrary defaults, and they are the same choices the registry's own
// scenes make.
SurfaceOptics defaultOptics(ObjectType type) {
    SurfaceOptics o;
    switch (type) {
    case ObjectType::BallLens:
    case ObjectType::PlanoConvexLens:
    case ObjectType::BiconvexLens:
    case ObjectType::PlanoConcaveLens:
    case ObjectType::CylindricalLens:
    case ObjectType::Axicon:
    case ObjectType::Box:
    case ObjectType::Sphere:
    case ObjectType::Cylinder:
        o.reflectivity   = 0.04;
        o.transmissivity = 0.96;
        o.fresnel        = true;
        applyMaterial(o, kGlass);
        o.coating = coating::byName(QLatin1String(kArCoat));
        break;

    case ObjectType::Prism:
        o.reflectivity   = 0.04;
        o.transmissivity = 0.96;
        o.fresnel        = true;
        applyMaterial(o, kFlint);
        o.coating = coating::byName(QLatin1String(kArCoat));
        break;

    // A guide works by total internal reflection, so it is left uncoated: an AR
    // film on the wall is precisely what such a part must not have.
    case ObjectType::RodGuide:
    case ObjectType::SquareGuide:
    case ObjectType::TaperedGuide:
        o.reflectivity   = 0.0;
        o.transmissivity = 1.0;
        o.fresnel        = true;
        applyMaterial(o, kGlass);
        break;

    case ObjectType::FlatMirror:
    case ObjectType::SphericalMirror:
    case ObjectType::ParabolicMirror:
        o.reflectivity = 0.92;
        applyMaterial(o, kMetal);
        break;

    case ObjectType::DiffuserPlate:
        o.reflectivity   = 0.05;
        o.transmissivity = 0.90;
        o.scatter        = 1.0;
        o.fresnel        = true;
        applyMaterial(o, kGlass);
        break;

    case ObjectType::Detector:
        o.isDetector = true;
        break;

    default:
        break;
    }
    return o;
}

// The shape a type builds at the given parameters, in its own local frame.
// An empty shape means "nothing to trace", which is what a Group and a Source
// legitimately are.
TopoDS_Shape buildShape(ObjectType type, const double* p) {
    switch (type) {
    case ObjectType::BallLens:
        return BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), p[0]).Shape();

    case ObjectType::PlanoConvexLens: {
        const double R = p[0];
        // Sphere centred at -R/2 so the cap vertex lands at z = R/2, cut against
        // a slab starting at z = 0: a flat face on the origin plane and a
        // convex cap above it.
        const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, -0.5 * R), R).Shape();
        const TopoDS_Shape slab   = BRepPrimAPI_MakeBox(gp_Pnt(-1.1 * R, -1.1 * R, 0.0),
                                                        2.2 * R, 2.2 * R, 0.6 * R).Shape();
        return BRepAlgoAPI_Common(sphere, slab).Shape();
    }

    case ObjectType::BiconvexLens: {
        const double R = p[0], half = 0.5 * std::max(1.0, p[1]);
        // Two spheres overlapping by the centre thickness: their intersection is
        // the lens, centred on the origin.
        if (half >= R) return BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), R).Shape();
        const TopoDS_Shape a = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, R - half), R).Shape();
        const TopoDS_Shape b = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, half - R), R).Shape();
        return BRepAlgoAPI_Common(a, b).Shape();
    }

    case ObjectType::PlanoConcaveLens: {
        const double R = p[0], blank = p[1], thk = p[2];
        const TopoDS_Shape body = BRepPrimAPI_MakeCylinder(
            gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), blank, thk).Shape();
        // The dish bites into the +Z face by half the thickness at most, so the
        // blank cannot be cut clean through however the numbers are dragged.
        const double bite = std::min(0.5 * thk, 0.5 * R);
        const TopoDS_Shape dish =
            BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, thk - bite + R), R).Shape();
        return BRepAlgoAPI_Cut(body, dish).Shape();
    }

    case ObjectType::CylindricalLens:
        return BRepPrimAPI_MakeCylinder(
                   gp_Ax2(gp_Pnt(0, -0.5 * p[1], 0), gp_Dir(0, 1, 0)), p[0], p[1]).Shape();

    case ObjectType::Axicon:
        return BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                                    p[0], 0.0, p[1]).Shape();

    case ObjectType::Prism:
        return extrudeAlongY({gp_Pnt(-p[0], 0, 0.0), gp_Pnt(p[0], 0, 0.0),
                              gp_Pnt(0.0, 0, p[1])}, p[2]);

    case ObjectType::FlatMirror:
        return makePlate(p[0]);

    case ObjectType::SphericalMirror:
        return sphericalCap(p[0], p[1]);

    case ObjectType::ParabolicMirror:
        return paraboloid(p[0], p[1]);

    case ObjectType::RodGuide:
        return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                                        p[0], p[1]).Shape();

    case ObjectType::SquareGuide:
        return BRepPrimAPI_MakeBox(gp_Pnt(-0.5 * p[0], -0.5 * p[0], 0.0),
                                   p[0], p[0], p[1]).Shape();

    case ObjectType::TaperedGuide: {
        // A cone with equal ends is degenerate to OCCT, so the exit is held
        // clear of the entry rather than refusing the build.
        const double rIn = p[0];
        double       rOut = p[1];
        if (std::fabs(rOut - rIn) < 1e-6) rOut = rIn * 0.9;
        return BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                                    rIn, rOut, p[2]).Shape();
    }

    case ObjectType::Box:
        return BRepPrimAPI_MakeBox(gp_Pnt(-0.5 * p[0], -0.5 * p[1], -0.5 * p[2]),
                                   p[0], p[1], p[2]).Shape();

    case ObjectType::Sphere:
        return BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), p[0]).Shape();

    case ObjectType::Cylinder:
        return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                                        p[0], p[1]).Shape();

    case ObjectType::DiffuserPlate:
        return BRepPrimAPI_MakeBox(gp_Pnt(-0.5 * p[0], -0.5 * p[0], 0.0),
                                   p[0], p[0], p[1]).Shape();

    case ObjectType::Detector:
        return makePlate(p[0]);

    default:
        return {};
    }
}

} // namespace

// ---- the registry, as the rest of the app sees it --------------------------

const TypeInfo& typeInfo(ObjectType t) {
    const std::vector<TypeInfo>& r = registry();
    const int i = int(t);
    return r[std::size_t(i >= 0 && i < int(ObjectType::Count) ? i : 0)];
}

ObjectType typeFromKey(const QString& key, bool* ok) {
    for (int i = 0; i < int(ObjectType::Count); ++i) {
        if (registry()[std::size_t(i)].key == key) {
            if (ok) *ok = true;
            return ObjectType(i);
        }
    }
    if (ok) *ok = false;
    return ObjectType::Group;
}

QString categoryName(Category c) {
    switch (c) {
    case Category::Sources:   return QStringLiteral("Sources");
    case Category::Lenses:    return QStringLiteral("Lenses & prisms");
    case Category::Mirrors:   return QStringLiteral("Mirrors");
    case Category::Guides:    return QStringLiteral("Light guides");
    case Category::Bodies:    return QStringLiteral("Bodies");
    case Category::Detectors: return QStringLiteral("Detectors");
    default:                  return QStringLiteral("Other");
    }
}

const std::vector<ObjectType>& creatableTypes() {
    static const std::vector<ObjectType> t = [] {
        std::vector<ObjectType> v;
        for (int i = 0; i < int(ObjectType::Count); ++i) {
            const ObjectType type = ObjectType(i);
            if (type == ObjectType::Group || type == ObjectType::TutorialPart ||
                type == ObjectType::ImportedPart)
                continue;
            v.push_back(type);
        }
        return v;
    }();
    return t;
}

const char* dragMimeType() { return "application/x-luxtrace-object"; }

// ---- SceneObject -----------------------------------------------------------

gp_Trsf SceneObject::localPlacement() const {
    gp_Trsf rx, ry, rz, tr;
    rx.SetRotation(gp_Ax1(gp::Origin(), gp::DX()), rotationDeg[0] * kDegToRad);
    ry.SetRotation(gp_Ax1(gp::Origin(), gp::DY()), rotationDeg[1] * kDegToRad);
    rz.SetRotation(gp_Ax1(gp::Origin(), gp::DZ()), rotationDeg[2] * kDegToRad);
    tr.SetTranslation(gp_Vec(position.X(), position.Y(), position.Z()));
    return tr * rz * ry * rx;
}

gp_Dir SceneObject::localAxis() const {
    gp_Dir d(0, 0, 1);
    gp_Trsf rot = localPlacement();
    rot.SetTranslationPart(gp_Vec(0, 0, 0));
    return d.Transformed(rot);
}

void SceneObject::setLocalAxis(const gp_Dir& axis) {
    // Rz(phi) * Ry(theta) takes +Z to (sin t cos p, sin t sin p, cos t), which is
    // every direction there is; the X term stays zero because roll about the
    // axis is not something an axis can state.
    const double z = std::clamp(axis.Z(), -1.0, 1.0);
    rotationDeg[0] = 0.0;
    rotationDeg[1] = std::acos(z) * kRadToDeg;
    rotationDeg[2] = (std::fabs(axis.X()) < 1e-12 && std::fabs(axis.Y()) < 1e-12)
                         ? 0.0
                         : std::atan2(axis.Y(), axis.X()) * kRadToDeg;
}

// ---- SceneDocument ---------------------------------------------------------

SceneDocument::SceneDocument() = default;

int SceneDocument::indexOf(int id) const {
    for (int i = 0; i < int(m_objects.size()); ++i)
        if (m_objects[std::size_t(i)].id == id) return i;
    return -1;
}

SceneObject* SceneDocument::find(int id) {
    const int i = indexOf(id);
    return i < 0 ? nullptr : &m_objects[std::size_t(i)];
}

const SceneObject* SceneDocument::find(int id) const {
    const int i = indexOf(id);
    return i < 0 ? nullptr : &m_objects[std::size_t(i)];
}

std::vector<int> SceneDocument::childrenOf(int parent) const {
    std::vector<int> out;
    for (const SceneObject& o : m_objects)
        if (o.parent == parent) out.push_back(o.id);
    return out;
}

bool SceneDocument::isAncestorOf(int ancestor, int id) const {
    // Bounded by the object count rather than trusting the parent chain to
    // terminate: a hand-edited file can describe a cycle, and a load that hangs
    // is worse than one that refuses a drag.
    int guard = int(m_objects.size()) + 1;
    for (int cur = id; cur != 0 && guard-- > 0;) {
        if (cur == ancestor) return true;
        const SceneObject* o = find(cur);
        if (!o) break;
        cur = o->parent;
    }
    return false;
}

int SceneDocument::sourceCount() const {
    int n = 0;
    for (const SceneObject& o : m_objects)
        if (o.isSource()) ++n;
    return n;
}

gp_Trsf SceneDocument::worldPlacement(int id) const {
    gp_Trsf t;
    int     guard = int(m_objects.size()) + 1;
    for (int cur = id; cur != 0 && guard-- > 0;) {
        const SceneObject* o = find(cur);
        if (!o) break;
        t   = o->localPlacement() * t;
        cur = o->parent;
    }
    return t;
}

void SceneDocument::clear() {
    m_objects.clear();
    m_nextId = 1;
    m_linked = false;
}

int SceneDocument::add(ObjectType type, const gp_Pnt& at, int parent) {
    // Adding to a tutorial is exactly the edit that means the parameter block no
    // longer describes what is on screen.
    detach();

    SceneObject o;
    o.id       = m_nextId++;
    o.parent   = (parent != 0 && indexOf(parent) >= 0) ? parent : 0;
    o.type     = type;
    o.position = at;
    o.optics      = defaultOptics(type);
    o.sceneOptics = o.optics;

    const TypeInfo& info = typeInfo(type);
    for (std::size_t i = 0; i < info.params.size() && i < SceneObject::kMaxParams; ++i)
        o.p[i] = info.params[i].def;

    if (type == ObjectType::Source) {
        o.source              = SourceSpec{};
        o.source.type         = SourceConfig::Type::Point;
        o.source.halfAngleDeg = 180.0;
        o.source.power        = 1.0;
        o.source.absolute     = true;
        o.source.useSceneAxis = false;
    }

    // A name a user can tell apart from the last one they dropped.
    int  n    = 1;
    auto used = [this](const QString& s) {
        for (const SceneObject& e : m_objects)
            if (e.name == s) return true;
        return false;
    };
    QString name = info.name;
    while (used(name)) name = QStringLiteral("%1 %2").arg(info.name).arg(++n);
    o.name = name;

    m_objects.push_back(std::move(o));
    return m_objects.back().id;
}

int SceneDocument::remove(int id) {
    if (indexOf(id) < 0) return 0;
    detach();

    const std::size_t before = m_objects.size();
    m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(),
                                   [this, id](const SceneObject& o) {
                                       return isAncestorOf(id, o.id);
                                   }),
                    m_objects.end());
    return int(before - m_objects.size());
}

int SceneDocument::duplicateInto(int id, int newParent) {
    const SceneObject* src = find(id);
    if (!src) return 0;

    SceneObject copy = *src;
    copy.id     = m_nextId++;
    copy.parent = newParent;

    int     n    = 1;
    auto    used = [this](const QString& s) {
        for (const SceneObject& e : m_objects)
            if (e.name == s) return true;
        return false;
    };
    QString name = src->name;
    while (used(name)) name = QStringLiteral("%1 (%2)").arg(src->name).arg(++n);
    copy.name = name;

    const int newId = copy.id;
    m_objects.push_back(std::move(copy));

    // Children are copied against the *original* child list, taken before the
    // loop: copying into the same vector it is walking would otherwise copy the
    // copies as well, forever.
    for (int child : childrenOf(id)) duplicateInto(child, newId);
    return newId;
}

int SceneDocument::duplicate(int id) {
    const SceneObject* src = find(id);
    if (!src) return 0;
    detach();
    return duplicateInto(id, src->parent);
}

bool SceneDocument::setName(int id, const QString& name) {
    SceneObject* o = find(id);
    if (!o || name.isEmpty() || o->name == name) return false;
    o->name = name;
    return true;
}

bool SceneDocument::setVisible(int id, bool visible) {
    SceneObject* o = find(id);
    if (!o || o->visible == visible) return false;
    o->visible = visible;
    return true;
}

bool SceneDocument::setOptics(int id, const SurfaceOptics& optics) {
    SceneObject* o = find(id);
    if (!o || o->isGroup() || o->isSource()) return false;
    o->optics       = optics;
    o->opticsEdited = true;
    return true;
}

bool SceneDocument::resetOptics(int id) {
    SceneObject* o = find(id);
    if (!o || o->isGroup() || o->isSource() || !o->opticsEdited) return false;
    o->optics       = o->sceneOptics;
    o->opticsEdited = false;
    return true;
}

bool SceneDocument::setSourceSpec(int id, const SourceSpec& spec) {
    SceneObject* o = find(id);
    if (!o || !o->isSource()) return false;
    o->source = spec;
    return true;
}

bool SceneDocument::setPlacement(int id, const gp_Pnt& position, const double rotationDeg[3]) {
    SceneObject* o = find(id);
    if (!o) return false;
    const bool same = o->position.IsEqual(position, 1e-9) &&
                      o->rotationDeg[0] == rotationDeg[0] &&
                      o->rotationDeg[1] == rotationDeg[1] &&
                      o->rotationDeg[2] == rotationDeg[2];
    if (same) return false;
    o->position = position;
    for (int i = 0; i < 3; ++i) o->rotationDeg[i] = rotationDeg[i];
    detach();
    return true;
}

bool SceneDocument::setParams(int id, const double* values, int count) {
    SceneObject* o = find(id);
    if (!o || !values) return false;
    const std::vector<SceneParamInfo>& info = typeInfo(o->type).params;
    bool changed = false;
    for (int i = 0; i < count && i < int(info.size()) && i < SceneObject::kMaxParams; ++i) {
        const double v = std::clamp(values[i], info[std::size_t(i)].min,
                                    info[std::size_t(i)].max);
        if (o->p[i] != v) { o->p[i] = v; changed = true; }
    }
    if (changed) detach();
    return changed;
}

bool SceneDocument::reparent(int id, int newParent) {
    SceneObject* o = find(id);
    if (!o || id == newParent) return false;
    // Dropping a group onto its own child would cut both out of the tree.
    if (newParent != 0 && (indexOf(newParent) < 0 || isAncestorOf(id, newParent)))
        return false;
    if (o->parent == newParent) return false;
    detach();
    o->parent = newParent;
    return true;
}

// ---- the tutorial link -----------------------------------------------------

void SceneDocument::loadTutorial(GeometryProvider::Scene scene, const SceneParams& params) {
    clear();
    m_scene  = scene;
    m_params = GeometryProvider::sanitise(scene, params);
    rebuildTutorialParts();
    m_linked = true;
}

void SceneDocument::loadImport(const GeometryProvider::SceneSetup& setup) {
    clear();
    for (std::size_t i = 0; i < setup.surfaces.size(); ++i) {
        const OpticalSurface& s = setup.surfaces[i];
        SceneObject o;
        o.id             = m_nextId++;
        o.type           = ObjectType::ImportedPart;
        o.name           = s.label.isEmpty() ? QStringLiteral("Part %1").arg(i + 1) : s.label;
        o.baked          = s.shape;
        o.optics         = static_cast<const SurfaceOptics&>(s);
        o.sceneOptics    = o.optics;
        o.instances      = s.placements;
        o.meshDeflection = s.meshDeflection;
        o.meshAngle      = s.meshAngle;
        m_objects.push_back(std::move(o));
    }

    SceneObject src;
    src.id                  = m_nextId++;
    src.type                = ObjectType::Source;
    src.name                = QStringLiteral("Light source");
    src.position            = setup.sourceOrigin;
    src.setLocalAxis(setup.sourceAxis);
    src.source.type         = SourceConfig::Type::Point;
    src.source.power        = 1.0;
    src.source.absolute     = true;
    src.source.useSceneAxis = false;
    m_objects.push_back(std::move(src));

    // A file is a fixed solid, not a parametric optic: there is no parameter
    // set that describes it, so the link is never claimed in the first place.
    m_linked = false;
}

void SceneDocument::setTutorialParams(const SceneParams& params) {
    if (!m_linked) return;
    m_params = GeometryProvider::sanitise(m_scene, params);
    rebuildTutorialParts();
}

void SceneDocument::rebuildTutorialParts() {
    const GeometryProvider::SceneSetup setup = GeometryProvider::build(m_scene, m_params);

    // Ids are kept where the part list keeps its shape, so a dimension change
    // does not throw away whatever the user had selected. A tutorial's part
    // count is fixed by its builder, so this is the ordinary case.
    struct Kept { int id; SurfaceOptics optics; bool edited; };
    std::vector<Kept> keptParts;
    for (const SceneObject& o : m_objects)
        if (o.type == ObjectType::TutorialPart)
            keptParts.push_back({o.id, o.optics, o.opticsEdited});
    const bool reuse = keptParts.size() == setup.surfaces.size();

    int sourceId = 0;
    for (const SceneObject& o : m_objects)
        if (o.isSource()) { sourceId = o.id; break; }

    SceneObject keptSource;
    bool        haveKept = false;
    if (const SceneObject* s = find(sourceId)) { keptSource = *s; haveKept = true; }

    m_objects.clear();

    for (std::size_t i = 0; i < setup.surfaces.size(); ++i) {
        const OpticalSurface& s = setup.surfaces[i];
        SceneObject o;
        o.id            = reuse ? keptParts[i].id : m_nextId++;
        o.type          = ObjectType::TutorialPart;
        o.name          = s.label.isEmpty() ? QStringLiteral("Part %1").arg(i + 1) : s.label;
        o.baked         = s.shape;
        o.tutorialIndex = int(i);
        o.optics        = static_cast<const SurfaceOptics&>(s);
        o.sceneOptics   = o.optics;
        o.instances     = s.placements;
        o.meshDeflection = s.meshDeflection;
        o.meshAngle      = s.meshAngle;
        // A reflectivity somebody typed is theirs; lengthening the optic is not
        // a reason to throw it away.
        if (reuse && keptParts[i].edited) {
            o.optics       = keptParts[i].optics;
            o.opticsEdited = true;
        }
        m_objects.push_back(std::move(o));
    }

    // The emitter the scene places, as an object like any other -- so it can be
    // selected, aimed and given a spectrum without a dialog of its own, and so
    // a second one can simply be dropped beside it.
    SceneObject src = haveKept ? keptSource : SceneObject{};
    if (!haveKept) {
        src.id                = m_nextId++;
        src.type              = ObjectType::Source;
        src.name              = QStringLiteral("Light source");
        src.source.type       = SourceConfig::Type::Point;
        src.source.power      = 1.0;
    }
    src.type     = ObjectType::Source;
    src.parent   = 0;
    src.position = setup.sourceOrigin;
    src.setLocalAxis(setup.sourceAxis);
    src.source.absolute     = true;
    src.source.useSceneAxis = false;
    const SceneObject templateSource = src;
    m_objects.push_back(std::move(src));

    // Any offsets the scene declares for additional emitters -- the four cups of
    // the LED luminaire are four sources, and inventing them by hand from the
    // parameter block was the alternative.
    const std::vector<gp_Pnt> offsets = GeometryProvider::sourceOffsets(m_scene, m_params);
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        SceneObject extra = templateSource;
        extra.id          = m_nextId++;
        extra.name        = QStringLiteral("Light source %1").arg(i + 2);
        extra.position    = gp_Pnt(setup.sourceOrigin.X() + offsets[i].X(),
                                   setup.sourceOrigin.Y() + offsets[i].Y(),
                                   setup.sourceOrigin.Z() + offsets[i].Z());
        m_objects.push_back(std::move(extra));
    }
}

// ---- compiling -------------------------------------------------------------

SceneDocument::Compiled SceneDocument::compile() const {
    Compiled out;
    out.setup = std::make_shared<GeometryProvider::SceneSetup>();

    for (const SceneObject& o : m_objects) {
        if (o.isGroup()) continue;
        const gp_Trsf world = worldPlacement(o.id);

        if (o.isSource()) {
            SourceSpec s = o.source;
            // Placement lives on the object, so the spec is written absolute:
            // an offset relative to "the scene emitter" means nothing once the
            // emitters *are* the objects.
            s.absolute     = true;
            s.useSceneAxis = false;
            s.offset       = gp_Pnt(0, 0, 0).Transformed(world);
            gp_Trsf rot    = world;
            rot.SetTranslationPart(gp_Vec(0, 0, 0));
            s.axis = gp_Dir(0, 0, 1).Transformed(rot);
            if (s.label.isEmpty()) s.label = o.name;

            if (!out.havePrimary) { out.primary = s; out.havePrimary = true; }
            else                    out.extraSources.push_back(s);
            continue;
        }

        const TopoDS_Shape shape =
            (o.type == ObjectType::TutorialPart || o.type == ObjectType::ImportedPart)
                ? o.baked
                : buildShape(o.type, o.p);
        if (shape.IsNull()) {
            out.warnings << QStringLiteral("%1 built no geometry and is not traced.")
                                .arg(o.name);
            continue;
        }

        OpticalSurface surf;
        static_cast<SurfaceOptics&>(surf) = o.optics;
        surf.label          = o.name;
        surf.meshDeflection = o.meshDeflection;
        surf.meshAngle      = o.meshAngle;

        if (o.instances.empty()) {
            // Rigid, so this sets a location rather than rebuilding the B-Rep.
            surf.shape = shape.Moved(TopLoc_Location(world));
        } else {
            // An instanced part is tessellated once about its own origin and
            // placed at each transform, so the object's placement composes into
            // the instances rather than into the shape -- moving the shape as
            // well would apply it twice.
            surf.shape = shape;
            surf.placements.reserve(o.instances.size());
            for (const gp_Trsf& t : o.instances) surf.placements.push_back(world * t);
        }

        out.setup->surfaces.push_back(std::move(surf));
        out.surfaceObject.push_back(o.id);
    }

    if (out.havePrimary) {
        out.setup->sourceOrigin = out.primary.offset;
        out.setup->sourceAxis   = out.primary.axis;
    } else {
        out.warnings << QStringLiteral("The scene has no light source, so a trace has "
                                       "nothing to emit. Drag one in from the library.");
    }
    if (out.setup->surfaces.empty())
        out.warnings << QStringLiteral("The scene has no geometry to trace.");

    out.setup->label = m_linked ? GeometryProvider::info(m_scene).name
                                : QStringLiteral("Assembled scene");
    out.setup->description =
        m_linked ? GeometryProvider::info(m_scene).description
                 : QStringLiteral("%1 object(s) placed in the scene editor.")
                       .arg(m_objects.size());
    return out;
}

// ---- persistence -----------------------------------------------------------

QJsonObject SceneDocument::toJson() const {
    QJsonObject root;
    // The scene is stored by name, not by enum index, for the same reason a
    // configuration stores it that way: the registry is an ordered list that
    // new scenes get inserted into.
    root[QStringLiteral("tutorial")] = GeometryProvider::info(m_scene).name;
    root[QStringLiteral("linked")]   = m_linked;
    root[QStringLiteral("nextId")]   = m_nextId;

    QJsonArray params;
    for (int i = 0; i < m_params.size(); ++i) params.append(m_params.v[i]);
    root[QStringLiteral("params")] = params;

    QJsonArray objects;
    for (const SceneObject& o : m_objects) {
        QJsonObject j;
        j[QStringLiteral("id")]      = o.id;
        j[QStringLiteral("parent")]  = o.parent;
        j[QStringLiteral("type")]    = typeInfo(o.type).key;
        j[QStringLiteral("name")]    = o.name;
        j[QStringLiteral("visible")] = o.visible;

        QJsonArray pos;
        pos.append(o.position.X());
        pos.append(o.position.Y());
        pos.append(o.position.Z());
        j[QStringLiteral("position")] = pos;

        QJsonArray rot;
        for (double d : o.rotationDeg) rot.append(d);
        j[QStringLiteral("rotation")] = rot;

        const std::size_t np = typeInfo(o.type).params.size();
        if (np > 0) {
            QJsonArray ps;
            for (std::size_t i = 0; i < np && i < SceneObject::kMaxParams; ++i)
                ps.append(o.p[i]);
            j[QStringLiteral("params")] = ps;
        }

        if (o.isSource()) j[QStringLiteral("source")] = configio::sourceToJson(o.source);
        else if (!o.isGroup()) {
            j[QStringLiteral("optics")]       = configio::opticsToJson(o.optics);
            j[QStringLiteral("opticsEdited")] = o.opticsEdited;
        }

        // The B-Rep is not written. A tutorial part is rebuilt from the scene
        // and its parameters on load and matched back by this index, which is
        // both smaller and truer than a serialised solid.
        if (o.tutorialIndex >= 0) j[QStringLiteral("tutorialIndex")] = o.tutorialIndex;

        objects.append(j);
    }
    root[QStringLiteral("objects")] = objects;
    return root;
}

SceneDocument SceneDocument::fromJson(const QJsonObject& root, QStringList* warnings) {
    SceneDocument doc;

    const QString wanted = root.value(QStringLiteral("tutorial")).toString();
    for (int i = 0; i < GeometryProvider::count(); ++i)
        if (GeometryProvider::info(GeometryProvider::Scene(i)).name == wanted) {
            doc.m_scene = GeometryProvider::Scene(i);
            break;
        }

    const QJsonArray params = root.value(QStringLiteral("params")).toArray();
    SceneParams       sp;
    sp.n = std::min(int(params.size()), SceneParams::kMax);
    for (int i = 0; i < sp.n; ++i) sp.v[i] = params[i].toDouble();
    doc.m_params = GeometryProvider::sanitise(doc.m_scene, sp);

    // What the tutorial builds right now, so its parts can be handed their
    // shapes back. Only built when the file actually names some.
    std::vector<OpticalSurface> parts;
    bool                        havePartsBuilt = false;

    int maxId = 0;
    for (const QJsonValue& v : root.value(QStringLiteral("objects")).toArray()) {
        if (!v.isObject()) continue;
        const QJsonObject j = v.toObject();

        bool             okType = false;
        const ObjectType type =
            typeFromKey(j.value(QStringLiteral("type")).toString(), &okType);
        if (!okType) {
            if (warnings)
                *warnings << QStringLiteral("scene object of unknown type \"%1\" dropped")
                                 .arg(j.value(QStringLiteral("type")).toString());
            continue;
        }

        SceneObject o;
        o.type    = type;
        o.id      = j.value(QStringLiteral("id")).toInt();
        o.parent  = j.value(QStringLiteral("parent")).toInt();
        o.name    = j.value(QStringLiteral("name")).toString();
        o.visible = j.value(QStringLiteral("visible")).toBool(true);
        if (o.id <= 0) continue;
        maxId = std::max(maxId, o.id);

        const QJsonArray pos = j.value(QStringLiteral("position")).toArray();
        if (pos.size() >= 3)
            o.position = gp_Pnt(pos[0].toDouble(), pos[1].toDouble(), pos[2].toDouble());
        const QJsonArray rot = j.value(QStringLiteral("rotation")).toArray();
        for (int i = 0; i < 3 && i < rot.size(); ++i) o.rotationDeg[i] = rot[i].toDouble();

        const std::vector<SceneParamInfo>& info = typeInfo(type).params;
        const QJsonArray ps = j.value(QStringLiteral("params")).toArray();
        for (std::size_t i = 0; i < info.size() && i < SceneObject::kMaxParams; ++i) {
            const double raw =
                (qsizetype(i) < ps.size()) ? ps[qsizetype(i)].toDouble() : info[i].def;
            o.p[i] = std::clamp(raw, info[i].min, info[i].max);
        }

        if (type == ObjectType::Source) {
            o.source = configio::sourceFromJson(j.value(QStringLiteral("source")).toObject(),
                                                warnings);
        } else if (type != ObjectType::Group) {
            o.sceneOptics  = defaultOptics(type);
            o.optics       = configio::opticsFromJson(
                j.value(QStringLiteral("optics")).toObject(), o.sceneOptics);
            o.opticsEdited = j.value(QStringLiteral("opticsEdited")).toBool(false);
        }

        if (type == ObjectType::ImportedPart) {
            // Nothing here can rebuild a STEP solid, and an imported part with
            // no shape would be a row in the tree that traces nothing.
            if (warnings)
                *warnings << QStringLiteral("imported part %1 cannot be restored from a "
                                            "saved scene; import the CAD file again")
                                 .arg(o.name);
            continue;
        }

        if (type == ObjectType::TutorialPart) {
            if (!havePartsBuilt) {
                parts          = GeometryProvider::build(doc.m_scene, doc.m_params).surfaces;
                havePartsBuilt = true;
            }
            o.tutorialIndex = j.value(QStringLiteral("tutorialIndex")).toInt(-1);
            if (o.tutorialIndex >= 0 && o.tutorialIndex < int(parts.size())) {
                const OpticalSurface& s = parts[std::size_t(o.tutorialIndex)];
                o.baked          = s.shape;
                o.instances      = s.placements;
                o.meshDeflection = s.meshDeflection;
                o.meshAngle      = s.meshAngle;
                // What the builder declared, so "reset" still means something
                // after a reload.
                o.sceneOptics    = static_cast<const SurfaceOptics&>(s);
                if (!o.opticsEdited) o.optics = o.sceneOptics;
            } else {
                if (warnings)
                    *warnings << QStringLiteral("tutorial part \"%1\" no longer exists in "
                                                "%2 and was dropped")
                                     .arg(o.name, GeometryProvider::info(doc.m_scene).name);
                continue;
            }
        }

        doc.m_objects.push_back(std::move(o));
    }

    doc.m_nextId = std::max(maxId + 1, root.value(QStringLiteral("nextId")).toInt(1));
    doc.m_linked = root.value(QStringLiteral("linked")).toBool(false);

    // A parent that did not survive the load would orphan its children into
    // nothing; they come back to the top level instead.
    for (SceneObject& o : doc.m_objects)
        if (o.parent != 0 && doc.indexOf(o.parent) < 0) o.parent = 0;

    return doc;
}

} // namespace scenedoc
