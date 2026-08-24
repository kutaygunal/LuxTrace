#include "CadImport.h"
#include "Material.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Bnd_Box.hxx>
#include <IGESControl_Reader.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_HSequenceOfTransient.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace cadimport {

namespace {

void boundsOf(const TopoDS_Shape& shape, double lo[3], double hi[3]) {
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        for (int i = 0; i < 3; ++i) { lo[i] = 0.0; hi[i] = 0.0; }
        return;
    }
    box.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
}

int countFaces(const TopoDS_Shape& shape) {
    int n = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) ++n;
    return n;
}

TopoDS_Shape scaled(const TopoDS_Shape& shape, double factor) {
    if (std::fabs(factor - 1.0) < 1e-12) return shape;
    gp_Trsf t;
    t.SetScale(gp_Pnt(0, 0, 0), factor);
    return BRepBuilderAPI_Transform(shape, t, Standard_True).Shape();
}

// A compound is a container, not a part. Splitting it means each product in the
// assembly arrives as its own shape, which is what lets a user give the lens and
// its housing different optics.
void collect(const TopoDS_Shape& shape, const QString& label, double scale,
             std::vector<ImportedShape>& out) {
    if (shape.IsNull()) return;
    if (shape.ShapeType() == TopAbs_COMPOUND) {
        int i = 0;
        for (TopoDS_Iterator it(shape); it.More(); it.Next(), ++i)
            collect(it.Value(), QStringLiteral("%1.%2").arg(label).arg(i + 1), scale, out);
        // A compound of compounds has already been split; one that held nothing
        // contributes nothing.
        if (i > 0) return;
    }
    ImportedShape s;
    s.shape = scaled(shape, scale);
    s.label = label;
    s.faceCount = countFaces(s.shape);
    if (s.faceCount == 0) return;
    boundsOf(s.shape, s.bboxMin, s.bboxMax);
    out.push_back(std::move(s));
}

void applyMaterial(OpticalSurface& o, const QString& materialName, bool reflective) {
    const OpticalMaterial m = materials::byName(materialName);
    if (reflective) {
        o.reflectivity = 0.95;
        o.transmissivity = 0.0;
        o.index = 0.0;
        if (m.valid() && m.isMetal()) o.material = m;
        return;
    }
    o.fresnel        = true;
    o.reflectivity   = 0.04;
    o.transmissivity = 0.96;
    o.index          = 1.5;
    if (m.valid() && !m.isMetal()) {
        o.material    = m;
        o.index       = m.nd;
        o.absorption  = m.alpha;
    }
}

} // namespace

double ImportedShape::size() const {
    double s = 0.0;
    for (int i = 0; i < 3; ++i) s = std::max(s, bboxMax[i] - bboxMin[i]);
    return s;
}

QStringList supportedFormats() {
    return {QStringLiteral("STEP"), QStringLiteral("IGES")};
}

QString fileFilter() {
    return QStringLiteral("CAD (*.step *.stp *.iges *.igs);;"
                          "STEP (*.step *.stp);;IGES (*.iges *.igs);;All files (*)");
}

ImportResult read(const QString& path, double scale) {
    ImportResult out;
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool iges = (suffix == QLatin1String("iges") || suffix == QLatin1String("igs"));
    out.format = iges ? QStringLiteral("IGES") : QStringLiteral("STEP");

    if (!QFileInfo::exists(path)) {
        out.error = QStringLiteral("No such file: %1").arg(path);
        return out;
    }
    if (!(scale > 0.0)) {
        out.error = QStringLiteral("The scale factor has to be positive.");
        return out;
    }

    try {
        OCC_CATCH_SIGNALS
        // Both readers throw on malformed input, and a malformed file is an
        // ordinary thing to be handed. Nothing here may reach the caller as an
        // exception: a bad file is a message, not a crash.
        if (iges) {
            IGESControl_Reader reader;
            if (reader.ReadFile(path.toLocal8Bit().constData()) != IFSelect_RetDone) {
                out.error = QStringLiteral("This does not read as an IGES file.");
                return out;
            }
            reader.TransferRoots();
            const int n = reader.NbShapes();
            if (n <= 0) {
                out.error = QStringLiteral("The file parsed, but carries no geometry.");
                return out;
            }
            for (int i = 1; i <= n; ++i)
                collect(reader.Shape(i), QStringLiteral("Part %1").arg(i), scale, out.shapes);
        } else {
            STEPControl_Reader reader;
            if (reader.ReadFile(path.toLocal8Bit().constData()) != IFSelect_RetDone) {
                out.error = QStringLiteral("This does not read as a STEP file.");
                return out;
            }
            reader.TransferRoots();
            const int n = reader.NbShapes();
            if (n <= 0) {
                out.error = QStringLiteral("The file parsed, but carries no geometry.");
                return out;
            }
            for (int i = 1; i <= n; ++i)
                collect(reader.Shape(i), QStringLiteral("Part %1").arg(i), scale, out.shapes);
        }
    } catch (const Standard_Failure& e) {
        out.error = QStringLiteral("The reader failed: %1")
                        .arg(QString::fromLatin1(e.GetMessageString()));
        return out;
    } catch (const std::exception& e) {
        out.error = QStringLiteral("The reader failed: %1").arg(QString::fromLatin1(e.what()));
        return out;
    } catch (...) {
        out.error = QStringLiteral("The reader failed for an unknown reason.");
        return out;
    }

    if (out.shapes.empty()) {
        out.error = QStringLiteral("The file parsed, but nothing in it has a face to trace.");
        return out;
    }

    for (int a = 0; a < 3; ++a) { out.bboxMin[a] = 1e300; out.bboxMax[a] = -1e300; }
    for (const auto& s : out.shapes) {
        out.totalFaces += s.faceCount;
        for (int a = 0; a < 3; ++a) {
            out.bboxMin[a] = std::min(out.bboxMin[a], s.bboxMin[a]);
            out.bboxMax[a] = std::max(out.bboxMax[a], s.bboxMax[a]);
        }
    }
    out.ok = true;
    return out;
}

std::vector<OpticalSurface> toSurfaces(const ImportResult& result,
                                       const QString& materialName, bool reflective) {
    std::vector<OpticalSurface> out;
    if (!result.ok) return out;
    out.reserve(result.shapes.size());
    for (const auto& s : result.shapes) {
        OpticalSurface o;
        o.shape = s.shape;
        o.label = s.label;
        applyMaterial(o, materialName, reflective);
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<OpticalSurface> facesOf(const ImportedShape& shape,
                                    const QString& materialName, bool reflective) {
    std::vector<OpticalSurface> out;
    int i = 0;
    for (TopExp_Explorer ex(shape.shape, TopAbs_FACE); ex.More(); ex.Next()) {
        OpticalSurface o;
        o.shape = ex.Current();
        o.label = QStringLiteral("%1 face %2").arg(shape.label).arg(++i);
        applyMaterial(o, materialName, reflective);
        out.push_back(std::move(o));
    }
    return out;
}

OpticalSurface makeReceiver(double size, double z, int bins, double cx, double cy) {
    return makeReceiver(size, gp_Pnt(cx, cy, z), gp_Dir(0, 0, 1), bins);
}

OpticalSurface makeReceiver(double size, const gp_Pnt& center, const gp_Dir& normal,
                            int bins) {
    const double h = 0.5 * std::max(1.0, size);
    // Any pair of in-plane axes will do -- the mesher reads the frame back off
    // the vertices -- so the only requirement is that they are perpendicular to
    // the normal and to each other.
    const gp_Vec n(normal);
    gp_Vec ref = (std::fabs(n.Z()) > 0.9) ? gp_Vec(1, 0, 0) : gp_Vec(0, 0, 1);
    gp_Vec u = ref - n * (ref * n);
    if (u.Magnitude() < 1e-9) u = gp_Vec(0, 1, 0);
    u.Normalize();
    const gp_Vec v = n.Crossed(u);
    const gp_Vec c(center.X(), center.Y(), center.Z());

    const auto corner = [&](double a, double b) {
        const gp_Vec p = c + u * (a * h) + v * (b * h);
        return gp_Pnt(p.X(), p.Y(), p.Z());
    };
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(corner(-1, -1));
    poly.Add(corner(+1, -1));
    poly.Add(corner(+1, +1));
    poly.Add(corner(-1, +1));
    poly.Close();

    OpticalSurface o;
    o.shape      = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    o.label      = QStringLiteral("Detector");
    o.isDetector = true;
    if (bins > 0) { o.detNX = bins; o.detNY = bins; }
    return o;
}

GeometryProvider::SceneSetup makeScene(const ImportResult& result,
                                       const QString& materialName, bool reflective,
                                       int detectorBins, const gp_Dir& axis,
                                       double standoff) {
    GeometryProvider::SceneSetup setup;
    if (!result.ok) return setup;

    setup.surfaces = toSurfaces(result, materialName, reflective);

    double extent = 0.0;
    for (int a = 0; a < 3; ++a)
        extent = std::max(extent, result.bboxMax[a] - result.bboxMin[a]);
    extent = std::max(extent, 1e-3);
    if (!(standoff > 0.0)) standoff = 0.75;

    const gp_Vec d(axis);
    const gp_Vec centre(0.5 * (result.bboxMin[0] + result.bboxMax[0]),
                        0.5 * (result.bboxMin[1] + result.bboxMax[1]),
                        0.5 * (result.bboxMin[2] + result.bboxMax[2]));

    // How far the box reaches along the beam, whichever way the beam points.
    // Clearing that, rather than clearing one named face, is what lets the same
    // placement work from any of the six sides.
    const double reach = 0.5 * (std::fabs(d.X()) * (result.bboxMax[0] - result.bboxMin[0]) +
                                std::fabs(d.Y()) * (result.bboxMax[1] - result.bboxMin[1]) +
                                std::fabs(d.Z()) * (result.bboxMax[2] - result.bboxMin[2]));
    const double back = reach + standoff * extent;

    // Wide enough to catch what misses the part as well as what goes through
    // it: a receiver cropped to the part reports an efficiency that is really a
    // statement about the receiver.
    const gp_Vec beyond = centre + d * back;
    setup.surfaces.push_back(makeReceiver(1.5 * extent,
                                          gp_Pnt(beyond.X(), beyond.Y(), beyond.Z()), axis,
                                          detectorBins));

    const gp_Vec behind = centre - d * back;
    setup.sourceOrigin = gp_Pnt(behind.X(), behind.Y(), behind.Z());
    setup.sourceAxis   = axis;
    return setup;
}

} // namespace cadimport
