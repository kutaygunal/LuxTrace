#pragma once
#include <vector>
#include <QString>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include "SurfaceOptics.h"

// A triangle in the flattened mesh with a face normal.
struct Triangle {
    int v0 = 0, v1 = 0, v2 = 0;
    gp_Dir normal;
};

// A surface extracted from an OCCT B-Rep shape: vertices + triangles + optics.
struct MeshSurface : SurfaceOptics {
    QString label;
    std::vector<gp_Pnt> verts;
    std::vector<Triangle> tris;

    // Detector receiver frame. The receiver is assumed planar and axis-aligned
    // (facing +Z); detCenter is the true centre of the detW x detH rectangle,
    // and detNX x detNY is the binning grid the tracer accumulates into.
    // Meaningless unless isDetector is true.
    gp_Pnt detCenter;
    double detW = 0.0, detH = 0.0;
    int    detNX = 0, detNY = 0;
};

using MeshList = std::vector<MeshSurface>;
