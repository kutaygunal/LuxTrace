// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QtGlobal>

#include <gp_Pnt.hxx>

#include "Simulation.h"       // SourceSpec
#include "SurfaceOptics.h"

// Which field an edit touched, and how to write only that field.
//
// A property panel showing one object writes the whole form back. A panel
// showing four cannot: writing every box would also write the boxes that were
// only ever a dash, flattening four different positions onto the primary's the
// moment somebody nudged a rotation. So the panel reports *which* box the user
// moved, as a mask, and these functions copy exactly the fields that mask names
// and leave the rest of the destination as it was.
//
// The flags and the copies live in the core rather than beside the widgets that
// raise them. Nothing here needs Qt Widgets, and "an edit to one number stays
// an edit to one number" is a correctness property, not a presentation one --
// it belongs somewhere a headless test can assert it. The widgets that put a
// dash on screen are `multiedit` in src/ui/MultiEdit.h and stay there.
namespace multiedit {

// Placement, shared by every type there is -- which is what makes it the one
// section a mixed-type selection can always edit.
namespace place {
enum : quint32 {
    PosX  = 1u << 0,
    PosY  = 1u << 1,
    PosZ  = 1u << 2,
    RotX  = 1u << 3,
    RotY  = 1u << 4,
    RotZ  = 1u << 5,
    Scale = 1u << 6,
};
}

// Geometry parameters, in the slot order the type declares them. Only ever
// meaningful for a selection that is all one type, because slot 2 is a radius
// on one type and a wall thickness on another.
namespace param {
enum : quint32 { Slot0 = 1u << 0 };
inline quint32 slot(int i) { return 1u << i; }
}

// Emission. Sources only.
namespace src {
enum : quint32 {
    Type       = 1u << 0,
    Shape      = 1u << 1,
    HalfAngle  = 1u << 2,
    SizeA      = 1u << 3,
    SizeB      = 1u << 4,
    BeamRadius = 1u << 5,
    Spectrum   = 1u << 6,
    Wavelength = 1u << 7,
    Cct        = 1u << 8,
    Power      = 1u << 9,
    Polarisation = 1u << 10,
    RayFile    = 1u << 11,
    RayScale   = 1u << 12,
    RayLambda  = 1u << 13,
};
}

// What a surface does to light. The largest common section: a lens, a mirror,
// a light guide and a receiver are four types with four different geometries
// and one shared answer to "how reflective is it".
namespace optic {
enum : quint32 {
    Reflectivity     = 1u << 0,
    Transmissivity   = 1u << 1,
    Fresnel          = 1u << 2,
    MediumPriority   = 1u << 3,
    Material         = 1u << 4,
    Index            = 1u << 5,
    Absorption       = 1u << 6,
    CoatingModel     = 1u << 7,
    CoatingResidual  = 1u << 8,
    CoatingHigh      = 1u << 9,
    BsdfModel        = 1u << 10,
    BsdfAlpha        = 1u << 11,
    BsdfFraction     = 1u << 12,
    AbgA             = 1u << 13,
    AbgB             = 1u << 14,
    AbgG             = 1u << 15,
    Scatter          = 1u << 16,
    Roughness        = 1u << 17,
    Appearance       = 1u << 18,
    VolumeCoefficient = 1u << 19,
    VolumeAnisotropy  = 1u << 20,
    DetNX            = 1u << 21,
    DetNY            = 1u << 22,
    DetAcceptance    = 1u << 23,
    DetReject        = 1u << 24,
};
// Choosing a coating preset rewrites the three fields under it, and choosing a
// catalogue material rewrites the index and the bulk absorption. Both are one
// user action that touches several fields, so both report all of them.
constexpr quint32 CoatingPreset  = CoatingModel | CoatingResidual | CoatingHigh;
constexpr quint32 MaterialChoice = Material | Index | Absorption;
}

// ---- the copies ------------------------------------------------------------
//
// Every one of these has the same contract: a field the mask does not name is
// not touched, and a field it does name is taken verbatim from `ref`.

// Position, XYZ Euler rotation in degrees, and uniform scale.
void applyPlacementFields(gp_Pnt& pos, double (&rotDeg)[3], double& scale,
                          const gp_Pnt& refPos, const double (&refRotDeg)[3],
                          double refScale, quint32 fields);

// Geometry parameter slots, `count` of them. Slot k answers to param::slot(k);
// slots past the 32 a mask can address are left alone.
void applyParamFields(double* dst, const double* ref, int count, quint32 fields);

// Emission fields.
void applySourceFields(SourceSpec& dst, const SourceSpec& ref, quint32 fields);

// Surface optics.
//
// The receiver rows are the one asymmetry: a bin grid on a mirror is not a
// harmless spare number -- isDetector is what decides whether the mesher lays a
// grid out at all -- so those four fields are refused unless the destination is
// a receiver, whatever the mask says.
void applyOpticsFields(SurfaceOptics& dst, const SurfaceOptics& ref, quint32 fields);

} // namespace multiedit
