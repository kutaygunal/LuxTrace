#include "CadImport.h"
#include "Material.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
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

#include <QFile>
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

// ---- the audit -------------------------------------------------------------

// A face of no area is a modelling artefact that the mesher will either drop or
// turn into a degenerate triangle. Either way it is not a surface light can
// meet, and counting them is how a user finds out their exporter produced some.
constexpr double kZeroArea = 1e-12;

int countFreeEdges(const TopoDS_Shape& shape, int* degenerateOut) {
    TopTools_IndexedDataMapOfShapeListOfShape edgeToFace;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeToFace);

    int free = 0, degenerate = 0;
    for (int i = 1; i <= edgeToFace.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(edgeToFace.FindKey(i));
        // A degenerate edge is the seam at a pole -- a sphere's axis, a cone's
        // apex. It has one adjacent face by construction and is not a hole.
        if (BRep_Tool::Degenerated(e)) { ++degenerate; continue; }
        if (edgeToFace.FindFromIndex(i).Extent() == 1) ++free;
    }
    if (degenerateOut) *degenerateOut = degenerate;
    return free;
}

int countZeroAreaFaces(const TopoDS_Shape& shape) {
    int n = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(TopoDS::Face(ex.Current()), props);
        if (props.Mass() <= kZeroArea) ++n;
    }
    return n;
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

GeometryAudit auditShape(const TopoDS_Shape& shape) {
    GeometryAudit a;
    if (shape.IsNull()) {
        a.status = GeometryAudit::Status::Bad;
        a.brepValid = false;
        a.notes << QStringLiteral("the part is empty");
        return a;
    }

    try {
        OCC_CATCH_SIGNALS
        // The whole point of this function is to be handed geometry that is not
        // well formed, so nothing in it may reach the caller as an exception.
        a.brepValid = BRepCheck_Analyzer(shape).IsValid() == Standard_True;

        int degenerate = 0;
        a.freeEdges       = countFreeEdges(shape, &degenerate);
        a.degenerateFaces = countZeroAreaFaces(shape);

        a.closed = true;
        for (TopExp_Explorer ex(shape, TopAbs_SHELL); ex.More(); ex.Next()) {
            ++a.shells;
            if (!BRep_Tool::IsClosed(ex.Current())) a.closed = false;
        }
        // A part with no shell at all is a sheet body: a single face, or a set
        // of them, with no inside. That is a legitimate way to model a mirror
        // and an illegitimate way to model a lens, and the tracer's own sheet
        // idiom handles the first -- so it is worth saying, not worth failing.
        if (a.shells == 0) {
            a.closed = false;
            a.notes << QStringLiteral("no closed shell: this is a sheet body, so it "
                                      "has no inside. Fine for a mirror; a "
                                      "refractive part modelled this way cannot "
                                      "track which medium a ray is in.");
        }
    } catch (const Standard_Failure& e) {
        a.status = GeometryAudit::Status::Bad;
        a.brepValid = false;
        a.notes << QStringLiteral("the checker failed on this part: %1")
                       .arg(QString::fromLatin1(e.GetMessageString()));
        return a;
    } catch (...) {
        a.status = GeometryAudit::Status::Bad;
        a.brepValid = false;
        a.notes << QStringLiteral("the checker failed on this part");
        return a;
    }

    if (!a.brepValid)
        a.notes << QStringLiteral("the B-Rep is not valid: faces, edges or "
                                  "orientations disagree with each other");
    if (a.freeEdges > 0)
        a.notes << QStringLiteral("%1 free edge(s): the surface has holes in it, so "
                                  "a ray can enter the solid without ever being "
                                  "recorded as leaving")
                       .arg(a.freeEdges);
    if (a.degenerateFaces > 0)
        a.notes << QStringLiteral("%1 face(s) of no area").arg(a.degenerateFaces);
    if (a.shells > 0 && !a.closed)
        a.notes << QStringLiteral("a shell does not bound a volume");

    // What the tracer actually depends on is medium tracking, and that depends
    // on closedness. A hole in the surface is the failure that produces a
    // confidently wrong answer; everything else is worth knowing about.
    if (!a.brepValid || a.freeEdges > 0)          a.status = GeometryAudit::Status::Bad;
    else if (!a.closed || a.degenerateFaces > 0)  a.status = GeometryAudit::Status::Warning;
    else                                          a.status = GeometryAudit::Status::Ok;

    if (a.status == GeometryAudit::Status::Ok)
        a.notes << QStringLiteral("closed, valid, %1 shell(s)").arg(a.shells);
    return a;
}

TopoDS_Shape healShape(const TopoDS_Shape& shape, GeometryAudit& audit) {
    if (shape.IsNull()) return shape;
    TopoDS_Shape fixed;
    try {
        OCC_CATCH_SIGNALS
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(shape);
        fixer->Perform();
        fixed = fixer->Shape();
    } catch (...) {
        return shape;
    }
    if (fixed.IsNull()) return shape;

    GeometryAudit after = auditShape(fixed);
    // Only keep the repair if it actually made the part better. ShapeFix is
    // allowed to change the customer's geometry; handing back something worse
    // than what they gave us is not a trade anybody agreed to.
    if (int(after.status) > int(audit.status)) return shape;
    if (after.status == audit.status && after.freeEdges >= audit.freeEdges &&
        after.brepValid == audit.brepValid)
        return shape;

    after.healed = true;
    after.notes.prepend(QStringLiteral("healed with ShapeFix"));
    audit = after;
    return fixed;
}

double stepUnitScale(const QString& path, QString* unitNameOut) {
    if (unitNameOut) unitNameOut->clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0.0;

    // The unit entities live in the DATA section, usually within the first few
    // hundred kilobytes. Reading four megabytes covers every real file without
    // pulling a large assembly into memory twice.
    const QByteArray head = f.read(4 * 1024 * 1024);
    const QString    text = QString::fromLatin1(head).toUpper();

    // A conversion-based unit names itself, and is what an inch-based file uses.
    struct Named { const char* name; double mm; const char* label; };
    static const Named kNamed[] = {
        {"'INCH'",       25.4,   "inch"},
        {"'FOOT'",       304.8,  "foot"},
        {"'MILLIMETRE'", 1.0,    "millimetre"},
        {"'MILLIMETER'", 1.0,    "millimetre"},
        {"'CENTIMETRE'", 10.0,   "centimetre"},
        {"'METRE'",      1000.0, "metre"},
        {"'METER'",      1000.0, "metre"},
    };
    for (const Named& n : kNamed) {
        const int at = text.indexOf(QLatin1String(n.name));
        if (at < 0) continue;
        // Only when it is a length unit: a file also names its angle unit.
        const int from = std::max(0, at - 400);
        if (!text.mid(from, at - from + 200).contains(QLatin1String("LENGTH_UNIT")))
            continue;
        if (unitNameOut) *unitNameOut = QLatin1String(n.label);
        return n.mm;
    }

    // Otherwise an SI unit with a prefix. `.METRE.` is what makes it a length,
    // so an angle in `.RADIAN.` cannot be mistaken for one.
    int at = 0;
    while ((at = text.indexOf(QLatin1String(".METRE."), at)) >= 0) {
        const int from = std::max(0, at - 300);
        const QString around = text.mid(from, at - from + 16);
        at += 7;
        if (!around.contains(QLatin1String("LENGTH_UNIT"))) continue;

        // SI_UNIT(prefix, .METRE.) -- the prefix immediately precedes it.
        const int si = around.lastIndexOf(QLatin1String("SI_UNIT("));
        if (si < 0) continue;
        const QString args = around.mid(si + 8);
        struct Prefix { const char* tag; double mm; const char* label; };
        static const Prefix kPrefix[] = {
            {".MILLI.", 1.0,      "millimetre"},
            {".CENTI.", 10.0,     "centimetre"},
            {".DECI.",  100.0,    "decimetre"},
            {".MICRO.", 0.001,    "micrometre"},
            {".KILO.",  1000000.0,"kilometre"},
        };
        for (const Prefix& p : kPrefix)
            if (args.contains(QLatin1String(p.tag))) {
                if (unitNameOut) *unitNameOut = QLatin1String(p.label);
                return p.mm;
            }
        // No prefix at all: bare metres.
        if (unitNameOut) *unitNameOut = QStringLiteral("metre");
        return 1000.0;
    }
    return 0.0;
}

QString ImportResult::auditSummary() const {
    if (!ok) return error;
    int bad = 0, warn = 0;
    for (const ImportedShape& s : shapes) {
        if (s.audit.status == GeometryAudit::Status::Bad)          ++bad;
        else if (s.audit.status == GeometryAudit::Status::Warning) ++warn;
    }
    QString out = QStringLiteral("%1 part(s), %2 face(s)")
                      .arg(shapes.size()).arg(totalFaces);
    out += QStringLiteral("; scale %1 mm per file unit").arg(appliedScale, 0, 'g', 6);
    if (unitFromHeader && !unitName.isEmpty())
        out += QStringLiteral(" (the file says %1)").arg(unitName);
    else
        out += QStringLiteral(" (the file does not say; this is the manual factor)");
    if (partsHealed > 0) out += QStringLiteral("; %1 healed").arg(partsHealed);
    if (bad > 0)  out += QStringLiteral("; %1 part(s) the tracer's assumptions do not "
                                        "hold on").arg(bad);
    if (warn > 0) out += QStringLiteral("; %1 with warnings").arg(warn);
    if (bad == 0 && warn == 0) out += QStringLiteral("; all parts closed and valid");
    return out;
}

ImportResult read(const QString& path, double scale) {
    ImportOptions opt;
    opt.scale = scale;
    opt.audit = true;
    return read(path, opt);
}

ImportResult read(const QString& path, const ImportOptions& options) {
    ImportResult out;
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool iges = (suffix == QLatin1String("iges") || suffix == QLatin1String("igs"));
    out.format = iges ? QStringLiteral("IGES") : QStringLiteral("STEP");

    if (!QFileInfo::exists(path)) {
        out.error = QStringLiteral("No such file: %1").arg(path);
        return out;
    }

    // The unit the file states, with the manual factor as an override rather
    // than as the only mechanism. A part a thousand times too big is the import
    // mistake everybody makes exactly once, and it is one the file can settle.
    double scale = options.scale;
    if (!(scale > 0.0)) {
        QString unit;
        const double fromFile = iges ? 0.0 : stepUnitScale(path, &unit);
        if (fromFile > 0.0) {
            scale              = fromFile;
            out.unitName       = unit;
            out.unitFromHeader = true;
        } else {
            scale = 1.0;
        }
    }
    if (!(scale > 0.0)) {
        out.error = QStringLiteral("The scale factor has to be positive.");
        return out;
    }
    out.appliedScale = scale;

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

    // Check every part against what the tracer assumes about a solid, before
    // anything is traced through it. "It traced, so the model must be fine" is
    // the assumption that produces confidently wrong numbers on a customer's
    // geometry, and imported CAD is the path most likely to be imperfect.
    if (options.audit) {
        for (ImportedShape& s : out.shapes) {
            s.audit = auditShape(s.shape);
            if (options.heal && s.audit.status != GeometryAudit::Status::Ok) {
                const TopoDS_Shape fixed = healShape(s.shape, s.audit);
                if (!fixed.IsSame(s.shape)) {
                    s.shape     = fixed;
                    s.faceCount = countFaces(s.shape);
                    boundsOf(s.shape, s.bboxMin, s.bboxMax);
                    ++out.partsHealed;
                }
            }
            if (int(s.audit.status) > int(out.worstStatus)) out.worstStatus = s.audit.status;
        }
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
