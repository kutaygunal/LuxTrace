// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <vector>
#include <QString>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Trsf.hxx>
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

    // Per-vertex normals, parallel to `verts`. Read off the exact B-Rep surface
    // at each node's UV parameters where the tessellation carries them, and
    // area-weighted from the adjacent facets where it does not.
    //
    // Interpolating these across a facet is what removes the tessellation from
    // the accuracy budget: a flat facet normal sits up to half the angular
    // deflection away from the true surface normal, and a mirror doubles that
    // into the reflected ray. Empty means "no shading normals" -- the tracer
    // then falls back to the facet normal, which is what a hand-built test mesh
    // wants.
    std::vector<gp_Dir> vnorm;

    // Where this mesh is placed. Empty or one entry is the ordinary case: the
    // triangles are already in world space. More than one makes it an instanced
    // part -- tessellated once, placed many times -- and the vertices above are
    // then in the part's own frame.
    std::vector<gp_Trsf> placements;

    // Detector receiver frame. detCenter is the true centre of the receiver
    // rectangle, `detU`/`detV` span it (their lengths are the half-extents) and
    // detNormal faces the side rays are counted from. detNX x detNY is the
    // binning grid the tracer accumulates into.
    // Meaningless unless isDetector is true.
    gp_Pnt detCenter;
    gp_Dir detU{1, 0, 0};
    gp_Dir detV{0, 1, 0};
    gp_Dir detNormal{0, 0, 1};
    double detW = 0.0, detH = 0.0;
};

using MeshList = std::vector<MeshSurface>;
