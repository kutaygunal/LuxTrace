#pragma once
#include <QString>
#include <QStringList>
#include <vector>
#include "GeometryProvider.h"

// Reading a customer's CAD and assigning optics to it.
//
// Illumination tools are bought to be run on the geometry a company already
// has, not on a library of demonstration scenes. Everything downstream of this
// -- meshing, the hierarchy, the trace, the analysis -- already works on a
// std::vector<OpticalSurface> and needs no change; the only thing that was
// missing was a way to produce one from a file.
//
// The built-in scenes keep their value as a validation suite and a tutorial
// library. They stop being the only thing the app can open.
namespace cadimport {

// What the tracer assumes about a solid, checked rather than hoped for.
//
// The import path used to report a face count and a bounding box. It did not
// check shell closedness, free edges, degenerate faces or normal consistency --
// all of which the medium-tracking model depends on. A ray entering a body it
// is never recorded as leaving gets systematically wrong index pairs and
// produces a perfectly clean-looking result, and "it traced, so the model must
// be fine" is the assumption that produces confidently wrong numbers on a
// customer's geometry.
struct GeometryAudit {
    // Green: nothing the tracer relies on is missing.
    // Amber: traceable, but something about it is worth knowing.
    // Red:   the tracer's assumptions do not hold on this part.
    enum class Status : int { Ok = 0, Warning, Bad };

    Status status = Status::Ok;

    bool   brepValid = true;        // BRepCheck_Analyzer
    bool   closed    = true;        // every shell bounds a volume
    int    freeEdges = 0;           // edges with exactly one adjacent face
    int    degenerateFaces = 0;     // faces of no area
    int    shells = 0;
    bool   healed = false;          // ShapeFix ran and changed it

    // What to show the user, one line each.
    QStringList notes;

    bool traceable() const { return status != Status::Bad; }
    const char* statusName() const {
        switch (status) {
        case Status::Ok:      return "ok";
        case Status::Warning: return "warning";
        case Status::Bad:     return "bad";
        }
        return "ok";
    }
};

// What was found in a file, before any optics are assigned.
struct ImportedShape {
    QString label;          // the STEP/IGES product name where the file carries one
    TopoDS_Shape shape;
    int     faceCount = 0;
    double  bboxMin[3] = {0, 0, 0};
    double  bboxMax[3] = {0, 0, 0};
    GeometryAudit audit;
    // Largest extent, so a caller can guess whether the file is in millimetres.
    double size() const;
};

// How to read a file.
struct ImportOptions {
    // Millimetres per file unit. Zero means "read it out of the file's own
    // header", which is what makes the most common import mistake -- a part
    // that is a thousand times too big or too small -- something the file
    // settles rather than something the user has to notice.
    double scale = 0.0;

    // Check every part against what the tracer assumes about a solid. It walks
    // every face and every edge, so a study that re-reads the same file can
    // turn it off.
    bool audit = true;

    // Run ShapeFix over a part that fails its audit. Off by default: healing
    // changes the customer's geometry, and doing that silently is worse than
    // reporting that it needs it.
    bool heal = false;
};

struct ImportResult {
    bool                       ok = false;
    QString                    error;
    QString                    format;        // "STEP" or "IGES"
    std::vector<ImportedShape> shapes;
    // Overall bounds of everything read, after scaling.
    double bboxMin[3] = {0, 0, 0};
    double bboxMax[3] = {0, 0, 0};
    int    totalFaces = 0;

    // The scale that was actually applied, and where it came from. A STEP file
    // states its own length unit, and reading it is the difference between an
    // import that is right by default and one that is right if the user
    // happened to know.
    double  appliedScale   = 1.0;
    QString unitName;              // "millimetre", "inch", ...
    bool    unitFromHeader = false;

    // The worst status any part came back with, so a caller can decide whether
    // to warn without walking the list.
    GeometryAudit::Status worstStatus = GeometryAudit::Status::Ok;
    int    partsHealed = 0;

    // A short human summary of the audit, for a dialog or a report.
    QString auditSummary() const;
};

// Which formats this build can read. The OCCT data-exchange toolkits are linked
// in, so this is a compile-time fact rather than a runtime probe.
QStringList supportedFormats();
// A filter string for a file dialog.
QString     fileFilter();

// Reads a file, guessing the format from the extension. `scale` multiplies every
// coordinate: a STEP file that turns out to be in metres becomes millimetres at
// 1000, which is the mismatch that bites first.
//
// This overload keeps the manual scale as the only mechanism, which is what it
// always was. Prefer the ImportOptions one: it reads the unit out of the file
// and leaves the manual factor as an override.
ImportResult read(const QString& path, double scale = 1.0);

ImportResult read(const QString& path, const ImportOptions& options);

// Millimetres per length unit, from a STEP file's own unit declaration.
// Returns 0 when the file does not say, or is not a STEP file.
//
// Read out of the Part 21 text rather than through the reader, deliberately:
// the entity is a handful of characters in a fixed grammar, and a scan of it
// is the same on every OCCT version. Getting this from the file at all is what
// matters -- a part a thousand times too big is the import mistake everyone
// makes exactly once.
double stepUnitScale(const QString& path, QString* unitNameOut = nullptr);

// Runs the audit over one shape.
GeometryAudit auditShape(const TopoDS_Shape& shape);

// ShapeFix over a shape that failed its audit, returning the repaired one and
// re-auditing it. Returns the original where nothing could be improved.
TopoDS_Shape healShape(const TopoDS_Shape& shape, GeometryAudit& audit);

// What an imported part is made of, before anybody edits it.
//
// It used to be one bool -- refractive, or a mirror -- and both of those are
// optical components. Most of what arrives in a STEP file is neither: a
// housing, a bracket, a tyre, a painted brick. Importing those as glass is why
// an assembly came into the Appearance preview transparent, and why the first
// thing anybody did after an import was edit every part.
enum class Finish {
    Refractive,   //!< a solid of the chosen catalogue material: a lens, a prism
    Mirror,       //!< a front-surface reflector
    Opaque        //!< a painted or moulded part: diffuse, no transmission
};

// Turns imported shapes into optical surfaces, all sharing one material.
// The caller then edits individual surfaces -- which is what the pick panel is
// for -- rather than having to describe every face up front.
std::vector<OpticalSurface> toSurfaces(const ImportResult& result,
                                       const QString& materialName,
                                       Finish finish);

// Splits one imported shape into a surface per face, so a lens body and its
// mount can carry different optics without going back to the CAD tool.
std::vector<OpticalSurface> facesOf(const ImportedShape& shape,
                                    const QString& materialName,
                                    Finish finish);

// A planar receiver sized and placed to catch what an imported assembly emits:
// square, `size` across, facing +Z at `z`, centred on (cx, cy). Imported
// geometry has no receiver of its own, and a scene without one reports nothing.
//
// The centre matters: a part that is not modelled about the origin -- which is
// most parts, since a CAD origin is wherever the designer put it -- would
// otherwise be measured against a plane sitting off to one side of it.
OpticalSurface makeReceiver(double size, double z, int bins = 0,
                            double cx = 0.0, double cy = 0.0);

// The same receiver on an arbitrary plane: square, `size` across, centred on
// `center` and facing `normal`. A part is illuminated from whichever side the
// user picks, and a receiver that could only lie in a z = const plane would
// leave five of those six directions unmeasurable.
OpticalSurface makeReceiver(double size, const gp_Pnt& center, const gp_Dir& normal,
                            int bins = 0);

// Everything a *trace* of an imported file needs, as opposed to everything a
// *view* of one needs: the parts with optics assigned, a receiver beyond them,
// and the source placement that aims at them.
//
// A CAD file describes a body, not an experiment. It carries no emitter and no
// measurement plane, so until this existed there was nothing to trace: pressing
// Run after an import traced whichever built-in scene was selected in the
// controls, on that scene's own geometry.
//
// `axis` is the direction the light travels. The source sits `standoff` extents back
// along it from the part's bounding box and the receiver faces it from the same
// distance on the far side, so illuminating a part from the side is a choice at
// import rather than a re-export with the model rotated. It matters more than
// it looks: a prism lit down its extrusion axis is a slab, and the deviation
// the part exists for only appears when the beam meets a slanted face.
GeometryProvider::SceneSetup makeScene(const ImportResult& result,
                                       const QString& materialName,
                                       Finish finish,
                                       int detectorBins = 0,
                                       const gp_Dir& axis = gp_Dir(0, 0, 1),
                                       double standoff = 0.75);

} // namespace cadimport
