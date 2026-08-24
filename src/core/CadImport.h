#pragma once
#include <QString>
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

// What was found in a file, before any optics are assigned.
struct ImportedShape {
    QString label;          // the STEP/IGES product name where the file carries one
    TopoDS_Shape shape;
    int     faceCount = 0;
    double  bboxMin[3] = {0, 0, 0};
    double  bboxMax[3] = {0, 0, 0};
    // Largest extent, so a caller can guess whether the file is in millimetres.
    double size() const;
};

struct ImportResult {
    bool                       ok = false;
    QString                    error;
    QString                    format;        // "STEP" or "IGES"
    std::vector<ImportedShape> shapes;
    // Overall bounds of everything read, in the file's own units.
    double bboxMin[3] = {0, 0, 0};
    double bboxMax[3] = {0, 0, 0};
    int    totalFaces = 0;
};

// Which formats this build can read. The OCCT data-exchange toolkits are linked
// in, so this is a compile-time fact rather than a runtime probe.
QStringList supportedFormats();
// A filter string for a file dialog.
QString     fileFilter();

// Reads a file, guessing the format from the extension. `scale` multiplies every
// coordinate: a STEP file that turns out to be in metres becomes millimetres at
// 1000, which is the mismatch that bites first.
ImportResult read(const QString& path, double scale = 1.0);

// Turns imported shapes into optical surfaces, all sharing one material.
// The caller then edits individual surfaces -- which is what the pick panel is
// for -- rather than having to describe every face up front.
std::vector<OpticalSurface> toSurfaces(const ImportResult& result,
                                       const QString& materialName,
                                       bool reflective);

// Splits one imported shape into a surface per face, so a lens body and its
// mount can carry different optics without going back to the CAD tool.
std::vector<OpticalSurface> facesOf(const ImportedShape& shape,
                                    const QString& materialName,
                                    bool reflective);

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
                                       bool reflective,
                                       int detectorBins = 0,
                                       const gp_Dir& axis = gp_Dir(0, 0, 1),
                                       double standoff = 0.75);

} // namespace cadimport
