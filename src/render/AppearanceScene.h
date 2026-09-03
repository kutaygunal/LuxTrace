// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <vector>

#include <AIS_Shape.hxx>
#include <Bnd_Box.hxx>

#include "AppearanceMaterials.h"
#include "core/GeometryProvider.h"

// Turns a compiled scene into the presentations the Appearance view displays.
//
// It reads the scene and nothing reads it back: deleting this file, its header
// and the view that calls them leaves a working application. No existing type
// gains a dependency on the renderer.
//
// Deliberately free of Qt widgets and of V3d, so the whole build -- which
// presentation exists, which material it carries, which surface it came from --
// is assertable in a headless test. Only `AppearanceView` needs a GL context.
namespace appearance {

// One presentation, and what it was built from.
struct Part {
    Handle(AIS_Shape) shape;
    int               surface  = -1;    // index into the surfaces vector
    bool              detector = false;
    Material          material;
};

struct Build {
    std::vector<Part> parts;
    // Detectors that were dropped rather than drawn. Reported so the view can
    // say "2 receivers hidden" instead of silently leaving them out.
    int hiddenDetectors = 0;
    // The bounding box of everything drawn, for framing the camera and for
    // scaling anything the view draws at a fraction of the scene.
    Bnd_Box bounds;
};

// Builds one presentation per visible body. An instanced part -- the twenty-five
// lenslets of an array, the cups of an LED luminaire -- becomes one presentation
// per placement, because a render that drew the first cup and none of the other
// eleven would not be a render of the luminaire.
//
// `showDetectors` draws receivers as faint matte targets for framing. Off by
// default: a measurement plane sits in front of the optic and blocks the shot.
Build buildScene(const std::vector<OpticalSurface>& surfaces, bool showDetectors);

// Puts `m` on `shape`, in both shading models at once. Separate from the build
// so that a rebuild which reuses presentations can restate a material without
// recreating anything.
void applyMaterial(const Handle(AIS_Shape)& shape, const Material& m);

} // namespace appearance
