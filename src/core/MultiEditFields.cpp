// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "MultiEditFields.h"

namespace multiedit {

void applyPlacementFields(gp_Pnt& pos, double (&rotDeg)[3], double& scale,
                          const gp_Pnt& refPos, const double (&refRotDeg)[3],
                          double refScale, quint32 fields) {
    namespace F = place;
    if (fields & F::PosX)  pos.SetX(refPos.X());
    if (fields & F::PosY)  pos.SetY(refPos.Y());
    if (fields & F::PosZ)  pos.SetZ(refPos.Z());
    if (fields & F::RotX)  rotDeg[0] = refRotDeg[0];
    if (fields & F::RotY)  rotDeg[1] = refRotDeg[1];
    if (fields & F::RotZ)  rotDeg[2] = refRotDeg[2];
    if (fields & F::Scale) scale     = refScale;
}

void applyParamFields(double* dst, const double* ref, int count, quint32 fields) {
    if (!dst || !ref) return;
    // A mask is 32 bits wide, so a slot past 31 has no flag that could name it
    // and is left as it was rather than shifted into undefined behaviour.
    const int n = count < 32 ? count : 32;
    for (int k = 0; k < n; ++k)
        if (fields & param::slot(k)) dst[k] = ref[k];
}

void applySourceFields(SourceSpec& dst, const SourceSpec& ref, quint32 fields) {
    namespace F = src;
    if (fields & F::Type)         dst.type         = ref.type;
    if (fields & F::Shape)        dst.shape        = ref.shape;
    if (fields & F::HalfAngle)    dst.halfAngleDeg = ref.halfAngleDeg;
    if (fields & F::SizeA)        dst.sizeA        = ref.sizeA;
    if (fields & F::SizeB)        dst.sizeB        = ref.sizeB;
    if (fields & F::BeamRadius)   dst.beamRadius   = ref.beamRadius;
    if (fields & F::Spectrum)     dst.spectrum.kind = ref.spectrum.kind;
    if (fields & F::Wavelength)   dst.spectrum.wavelengthNm = ref.spectrum.wavelengthNm;
    if (fields & F::Cct)          dst.spectrum.cct = ref.spectrum.cct;
    if (fields & F::Power)        dst.power        = ref.power;
    if (fields & F::Polarisation) dst.polarisationState = ref.polarisationState;
    if (fields & F::RayFile)      dst.rayFile      = ref.rayFile;
    if (fields & F::RayScale)     dst.rayFileScale = ref.rayFileScale;
    if (fields & F::RayLambda)    dst.rayFileWavelengths = ref.rayFileWavelengths;
}

void applyOpticsFields(SurfaceOptics& dst, const SurfaceOptics& ref, quint32 fields) {
    namespace F = optic;
    if (fields & F::Reflectivity)      dst.reflectivity   = ref.reflectivity;
    if (fields & F::Transmissivity)    dst.transmissivity = ref.transmissivity;
    if (fields & F::Fresnel)           dst.fresnel        = ref.fresnel;
    if (fields & F::MediumPriority)    dst.mediumPriority = ref.mediumPriority;
    if (fields & F::Material)          dst.material       = ref.material;
    if (fields & F::Index)             dst.index          = ref.index;
    if (fields & F::Absorption)        dst.absorption     = ref.absorption;
    if (fields & F::CoatingModel)      dst.coating.model         = ref.coating.model;
    if (fields & F::CoatingResidual)   dst.coating.residual      = ref.coating.residual;
    if (fields & F::CoatingHigh)       dst.coating.highReflector = ref.coating.highReflector;
    if (fields & F::BsdfModel)         dst.bsdf.model    = ref.bsdf.model;
    if (fields & F::BsdfAlpha)         dst.bsdf.alpha    = ref.bsdf.alpha;
    if (fields & F::BsdfFraction)      dst.bsdf.fraction = ref.bsdf.fraction;
    if (fields & F::AbgA)              dst.bsdf.abgA     = ref.bsdf.abgA;
    if (fields & F::AbgB)              dst.bsdf.abgB     = ref.bsdf.abgB;
    if (fields & F::AbgG)              dst.bsdf.abgG     = ref.bsdf.abgG;
    if (fields & F::Scatter)           dst.scatter       = ref.scatter;
    if (fields & F::Roughness)         dst.roughness     = ref.roughness;
    if (fields & F::Appearance)
        for (int i = 0; i < 3; ++i) dst.appearanceRgb[i] = ref.appearanceRgb[i];
    if (fields & F::VolumeCoefficient) dst.volume.coefficient = ref.volume.coefficient;
    if (fields & F::VolumeAnisotropy)  dst.volume.anisotropy  = ref.volume.anisotropy;

    // Only where the receiver rows mean anything. A bin grid on a mirror is not
    // a harmless spare number -- isDetector is what decides whether the mesher
    // lays a grid out at all, and the fields below would sit in the document
    // contradicting it.
    if (dst.isDetector) {
        if (fields & F::DetNX)         dst.detNX            = ref.detNX;
        if (fields & F::DetNY)         dst.detNY            = ref.detNY;
        if (fields & F::DetAcceptance) dst.detAcceptanceDeg = ref.detAcceptanceDeg;
        if (fields & F::DetReject)     dst.detRejectMode    = ref.detRejectMode;
    }
}

} // namespace multiedit
