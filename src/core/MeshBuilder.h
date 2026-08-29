#pragma once
#include "Mesh.h"
#include "GeometryProvider.h"

// Converts OCCT B-Rep shapes into flattened triangle meshes the ray tracer can
// intersect. Uses BRepMesh_IncrementalMesh + Poly_Triangulation.
//
// The mesh is adaptive per face, not uniform per body: the linear/angular
// deflection a face gets is driven by its local curvature and by whether that
// curvature makes it an optical face at all, so a mounting flange sheds
// triangles while an asphere keeps (and tightens) the accuracy that matters for
// hit position. Vertex normals are still read off the exact B-Rep surface.
// Receiver resolution when a surface does not ask for one. 64 x 64 is what the
// mesher used to hardcode; it is now a default a scene or the UI can override.
constexpr int kDefaultDetectorBins = 64;

class MeshBuilder {
public:
    // Meshes every optical surface. Detector surfaces also get a receiver frame
    // derived from their own plane.
    static MeshList build(const std::vector<OpticalSurface>& surfaces);

private:
    // Appends triangles of one shape into `out`, copying optical behaviour.
    static void meshShape(const OpticalSurface& os, MeshSurface& out);
    // Origin, in-plane axes, normal, extents and grid of a receiver surface.
    static void buildDetectorFrame(MeshSurface& out);
};
