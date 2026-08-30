#include "AppearanceScene.h"

#include <BRepBndLib.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Graphic3d_NameOfMaterial.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <TopLoc_Location.hxx>

namespace appearance {

void applyMaterial(const Handle(AIS_Shape)& shape, const Material& m) {
    if (shape.IsNull()) return;

    // Both models, from one mapping. `SetBSDF` is what the path tracer reads
    // and `SetPBRMaterial` what the rasterized preview does; setting only one
    // means the preview and the render disagree about the same surface, which
    // is exactly the confusion a preview exists to prevent.
    Graphic3d_MaterialAspect aspect(Graphic3d_NameOfMaterial_UserDefined);
    aspect.SetColor(m.colour);
    aspect.SetBSDF(m.bsdf);
    aspect.SetPBRMaterial(m.pbr);

    shape->SetMaterial(aspect);
    shape->SetTransparency(m.transparency);
}

Build buildScene(const std::vector<OpticalSurface>& surfaces, bool showDetectors) {
    Build out;
    out.parts.reserve(surfaces.size());

    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        const OpticalSurface& os = surfaces[i];
        if (os.shape.IsNull()) continue;

        const Material material = materialFor(os);
        if (material.hidden && !showDetectors) {
            ++out.hiddenDetectors;
            continue;
        }

        // An instanced part is tessellated once about its own origin and put at
        // each placement. One presentation per placement, sharing the B-Rep:
        // AIS_Shape carries a location, so the geometry is not duplicated.
        const std::size_t copies = os.placements.empty() ? 1 : os.placements.size();
        for (std::size_t p = 0; p < copies; ++p) {
            Handle(AIS_Shape) shape = new AIS_Shape(os.shape);
            if (!os.placements.empty())
                shape->SetLocalTransformation(os.placements[p]);

            applyMaterial(shape, material);

            Part part;
            part.shape    = shape;
            part.surface  = int(i);
            part.detector = os.isDetector;
            part.material = material;
            out.parts.push_back(part);

            // The box the camera is framed against. Taken from the placed
            // geometry rather than the raw B-Rep, or an array of lenslets would
            // frame as one lenslet at the origin.
            TopoDS_Shape placed = os.shape;
            if (!os.placements.empty()) placed.Move(TopLoc_Location(os.placements[p]));
            BRepBndLib::Add(placed, out.bounds);
        }
    }

    return out;
}

} // namespace appearance
