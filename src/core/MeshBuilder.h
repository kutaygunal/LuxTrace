#pragma once
#include "Mesh.h"
#include "GeometryProvider.h"

// Converts OCCT B-Rep shapes into flattened triangle meshes the ray tracer can
// intersect. Uses BRepMesh_IncrementalMesh + Poly_Triangulation.
class MeshBuilder {
public:
    // Meshes every optical surface. Detector surfaces also store receiver-grid
    // geometry (they are axis-aligned, facing +Z).
    static MeshList build(const std::vector<OpticalSurface>& surfaces);

private:
    // Appends triangles of one shape into `out`, copying optical behaviour.
    static void meshShape(const OpticalSurface& os, MeshSurface& out);
};
