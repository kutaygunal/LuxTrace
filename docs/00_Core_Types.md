# LuxTrace — Core Types

This file documents the shared core data structures of the project. It is kept
as reference for the engineering work. Header definitions live in
`src/core/`.

## Geometric primitives
We use OpenCASCADE geometric types directly (`gp_Pnt`, `gp_Dir`, `gp_Vec`)
because the whole engine links OCCT. No custom vector class is needed.

## MeshSurface
A flattened triangle mesh extracted from an OCCT `TopoDS_Shape` via
`BRepMesh_IncrementalMesh` + `Poly_Triangulation`. The ray tracer intersects rays
against these triangles (Moller-Trumbore) rather than solving full B-Rep
surface intersections — robust and simple.

```cpp
struct Triangle { int v0, v1, v2; gp_Dir normal; };

struct MeshSurface {
    QString label;
    std::vector<gp_Pnt> verts;   // unique vertices (global frame)
    std::vector<Triangle> tris;
    // optical behaviour
    double reflectivity   = 0.0;
    double transmissivity = 0.0;
    double index          = 0.0;   // refractive index (0 == opaque)
    bool   isDetector     = false;
    // detector receiver frame (axis-aligned, normal +Z)
    gp_Pnt detCenter; double detW = 0, detH = 0;
    int detNX = 0, detNY = 0;
};
```

## OpticalSurface — shape + material (input to meshing)
```cpp
struct OpticalSurface {
    TopoDS_Shape shape;
    QString label;
    double reflectivity, transmissivity, index;
    bool isDetector;
};
```
