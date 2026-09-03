// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "SurfaceInspector.h"
#include "MultiEdit.h"
#include "PlotWidget.h"

#include "core/Material.h"
#include "core/Optics.h"
#include "core/Polarisation.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

QDoubleSpinBox* dspin(QWidget* p, double lo, double hi, double step, int decimals,
                      const QString& suffix = QString()) {
    auto* s = new multiedit::DoubleSpinBox(p);
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    return s;
}

QGroupBox* group(const QString& title, QWidget* parent) {
    auto* g = new QGroupBox(title, parent);
    g->setFlat(true);
    return g;
}

// The catalogue entry a material came from, so the picker can show what the
// surface is actually carrying. A material travels as resolved coefficients
// rather than as a name, so this is a match rather than a lookup.
int catalogueIndexOf(const OpticalMaterial& m) {
    if (!m.valid()) return -1;
    for (int i = 0; i < materials::count(); ++i) {
        const OpticalMaterial c = materials::at(i);
        if (c.model == m.model && std::fabs(c.nd - m.nd) < 1e-9) return i;
    }
    return -1;
}

} // namespace

SurfaceInspector::SurfaceInspector(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // No scroll area of its own, deliberately. This panel is only ever
    // embedded in the object inspector, which already scrolls -- and a scroll
    // area inside a scroll area gives a short window onto a long form: the two
    // 130px plots below fill it, and everything after the coating section
    // (scattering, volume, appearance) sits off the bottom of an inner
    // viewport most people never realise is there. One scroll, one form.
    auto* body = new QWidget(this);
    outer->addWidget(body);

    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(8);

    m_name = new QLabel(body);
    m_name->setWordWrap(true);
    m_name->setStyleSheet(QStringLiteral("font-weight: 600;"));
    layout->addWidget(m_name);

    m_editedNote = new QLabel(body);
    m_editedNote->setWordWrap(true);
    m_editedNote->setStyleSheet(QStringLiteral("color: #b07018;"));
    layout->addWidget(m_editedNote);

    // ---- what it is ---------------------------------------------------------
    auto* basicBox  = group(QStringLiteral("Surface"), body);
    auto* basicForm = new QFormLayout(basicBox);

    m_reflectivity = dspin(basicBox, 0.0, 1.0, 0.01, 4);
    m_reflectivity->setToolTip(QStringLiteral(
        "The fixed reflected share, used when Fresnel is off and for every "
        "opaque surface. A front-surface aluminium mirror is about 0.92; a "
        "polished dielectric is nearer 0.04 and should be left to Fresnel."));

    m_transmissivity = dspin(basicBox, 0.0, 1.0, 0.01, 4);
    m_transmissivity->setToolTip(QStringLiteral(
        "The fixed transmitted share. Whatever is neither reflected nor "
        "transmitted is absorbed at the surface."));

    m_fresnel = new QCheckBox(QStringLiteral("Angle-dependent (Fresnel)"), basicBox);
    m_fresnel->setToolTip(QStringLiteral(
        "Split by the Fresnel equations rather than by the fixed shares above: "
        "about 4 % at normal incidence for air/glass, rising to 1 at grazing "
        "and exactly 1 past the critical angle. Only meaningful with an index."));

    m_priority = new multiedit::SpinBox(basicBox);
    m_priority->setRange(-100, 100);
    m_priority->setToolTip(QStringLiteral(
        "Where two solids overlap, the higher priority wins: a ray inside both "
        "is treated as travelling in the higher-priority medium. This is how "
        "FRED and TracePro resolve modelling overlap, and it is what lets a "
        "cemented doublet refract glass into cement rather than glass into air."));

    basicForm->addRow(QStringLiteral("Reflectance:"), m_reflectivity);
    basicForm->addRow(QStringLiteral("Transmittance:"), m_transmissivity);
    basicForm->addRow(m_fresnel);
    basicForm->addRow(QStringLiteral("Medium priority:"), m_priority);
    layout->addWidget(basicBox);

    // ---- material -----------------------------------------------------------
    auto* matBox  = group(QStringLiteral("Material"), body);
    auto* matForm = new QFormLayout(matBox);

    m_material = new QComboBox(matBox);
    m_material->addItem(QStringLiteral("(none)"));
    for (int i = 0; i < materials::count(); ++i) {
        m_material->addItem(materials::name(i));
        m_material->setItemData(i + 1, materials::description(i), Qt::ToolTipRole);
    }
    m_material->setToolTip(QStringLiteral(
        "A catalogue glass or metal. It supersedes the index below as the "
        "*shape* of the dispersion curve -- a Sellmeier fit holds across the "
        "whole visible band where a two-term Cauchy diverges outside it -- and "
        "a metal supplies the complex index its reflectance is computed from."));

    m_materialInfo = new QLabel(matBox);
    m_materialInfo->setWordWrap(true);
    m_materialInfo->setStyleSheet(QStringLiteral("color: #7a7a7a;"));

    m_index = dspin(matBox, 0.0, 5.0, 0.01, 4);
    m_index->setToolTip(QStringLiteral(
        "Refractive index at the d line, 587.6 nm. Zero makes the surface "
        "opaque. It stays authoritative for the level even with a material "
        "assigned: the curve is the material's, shifted so n(587.6) is this."));

    m_absorption = dspin(matBox, 0.0, 10.0, 0.0001, 6, QStringLiteral(" /mm"));
    m_absorption->setToolTip(QStringLiteral(
        "Beer-Lambert attenuation of the medium behind this surface. This is "
        "the loss that scales with path length rather than with hit count, so "
        "it is what makes a long light guide cost more than a short one."));

    matForm->addRow(QStringLiteral("Catalogue:"), m_material);
    matForm->addRow(m_materialInfo);
    matForm->addRow(QStringLiteral("Index n_d:"), m_index);
    matForm->addRow(QStringLiteral("Bulk absorption:"), m_absorption);
    layout->addWidget(matBox);

    // ---- coating ------------------------------------------------------------
    m_coatingBox = group(QStringLiteral("Coating"), body);
    auto* coatForm = new QFormLayout(m_coatingBox);

    m_coatingName = new QComboBox(m_coatingBox);
    m_coatingName->addItem(QStringLiteral("(custom)"));
    for (int i = 0; i < coating::count(); ++i) {
        m_coatingName->addItem(coating::name(i));
        m_coatingName->setItemData(i + 1, coating::description(i), Qt::ToolTipRole);
    }
    m_coatingName->setToolTip(QStringLiteral(
        "A coating by name, the way a drawing specifies one. Without any, every "
        "refractive surface pays bare-glass Fresnel and a multi-element system "
        "overstates its loss by roughly 3.5 % per surface against any real lens."));

    m_coatingModel = new QComboBox(m_coatingBox);
    for (int i = 0; i < 4; ++i) {
        m_coatingModel->addItem(coating::modelName(coating::Model(i)));
        m_coatingModel->setItemData(i, coating::modelTip(coating::Model(i)),
                                    Qt::ToolTipRole);
    }

    m_coatingResidual = dspin(m_coatingBox, 0.0, 1.0, 0.001, 5);
    m_coatingResidual->setToolTip(QStringLiteral(
        "Residual reflectance at normal incidence. 0.0025 is an ordinary "
        "broadband anti-reflection coating; 0.995 a dielectric high reflector."));

    m_coatingHigh = new QCheckBox(QStringLiteral("High reflector"), m_coatingBox);
    m_coatingHigh->setToolTip(QStringLiteral(
        "The residual is a floor to reflect rather than a target to suppress."));

    m_coatingInfo = new QLabel(m_coatingBox);
    m_coatingInfo->setWordWrap(true);
    m_coatingInfo->setStyleSheet(QStringLiteral("color: #7a7a7a;"));

    m_coatingPlot = new PlotWidget(m_coatingBox);
    m_coatingPlot->setMinimumHeight(130);
    m_coatingPlot->setAxisLabels(QStringLiteral("angle of incidence [deg]"),
                                 QStringLiteral("R"));
    m_coatingPlot->setPlaceholder(QStringLiteral("No coating"));

    coatForm->addRow(QStringLiteral("Preset:"), m_coatingName);
    coatForm->addRow(QStringLiteral("Model:"), m_coatingModel);
    coatForm->addRow(QStringLiteral("Residual R:"), m_coatingResidual);
    coatForm->addRow(m_coatingHigh);
    coatForm->addRow(m_coatingInfo);
    coatForm->addRow(m_coatingPlot);
    layout->addWidget(m_coatingBox);

    // ---- scatter ------------------------------------------------------------
    auto* scatterBox  = group(QStringLiteral("Scatter"), body);
    auto* scatterForm = new QFormLayout(scatterBox);

    m_bsdfModel = new QComboBox(scatterBox);
    for (int i = 0; i < 5; ++i) {
        m_bsdfModel->addItem(bsdf::modelName(bsdf::Model(i)));
        m_bsdfModel->setItemData(i, bsdf::modelTip(bsdf::Model(i)), Qt::ToolTipRole);
    }
    m_bsdfTip = new QLabel(scatterBox);
    m_bsdfTip->setWordWrap(true);
    m_bsdfTip->setStyleSheet(QStringLiteral("color: #7a7a7a;"));

    m_bsdfAlpha = dspin(scatterBox, 0.0, 1.0, 0.005, 4);
    m_bsdfAlpha->setToolTip(QStringLiteral(
        "GGX microfacet roughness: the RMS slope of the surface. It is a real "
        "alpha with Smith shadowing-masking and correct sampling weights, and "
        "Fresnel is evaluated at the sampled facet -- so a rough dielectric no "
        "longer reflects as though it were polished."));

    m_bsdfFraction = dspin(scatterBox, 0.0, 1.0, 0.05, 4);
    m_bsdfFraction->setToolTip(QStringLiteral(
        "The share of the energy this lobe takes. The rest stays specular, "
        "which is what a real polished-but-imperfect surface does."));

    m_abgA = dspin(scatterBox, 0.0, 100.0, 0.0001, 6);
    m_abgB = dspin(scatterBox, 1e-6, 1.0, 0.001, 6);
    m_abgG = dspin(scatterBox, 0.05, 8.0, 0.1, 3);
    for (QDoubleSpinBox* s : {m_abgA, m_abgB, m_abgG})
        s->setToolTip(QStringLiteral(
            "The A, B, g model TracePro and LightTools expose. B is the width of "
            "the specular core in direction cosines and g the falloff of the "
            "wings; A sets the level. These map directly onto a measured BSDF, "
            "which is what lets a scatter spec sheet be reproduced."));

    m_scatter = dspin(scatterBox, 0.0, 1.0, 0.05, 4);
    m_scatter->setToolTip(QStringLiteral(
        "Shorthand: a Lambertian lobe of this fraction. Superseded wherever a "
        "real BSDF above is set."));

    m_roughness = dspin(scatterBox, 0.0, 1.0, 0.002, 4, QStringLiteral(" rad"));
    m_roughness->setToolTip(QStringLiteral(
        "Shorthand: RMS surface slope error, read as a GGX microfacet of the "
        "same RMS slope. Superseded wherever a scatter fraction or a real BSDF "
        "is set."));

    m_resolved = new QLabel(scatterBox);
    m_resolved->setWordWrap(true);
    m_resolved->setStyleSheet(QStringLiteral("color: #4a7a4a;"));

    m_lobePlot = new PlotWidget(scatterBox);
    m_lobePlot->setMinimumHeight(130);
    m_lobePlot->setAxisLabels(QStringLiteral("offset from specular [direction cosine]"),
                              QStringLiteral("BSDF [1/sr]"));
    m_lobePlot->setLogY(true);
    m_lobePlot->setPlaceholder(QStringLiteral("Polished: no lobe to draw"));

    scatterForm->addRow(QStringLiteral("Model:"), m_bsdfModel);
    scatterForm->addRow(m_bsdfTip);
    scatterForm->addRow(QStringLiteral("Roughness alpha:"), m_bsdfAlpha);
    scatterForm->addRow(QStringLiteral("Lobe fraction:"), m_bsdfFraction);
    scatterForm->addRow(QStringLiteral("ABg  A:"), m_abgA);
    scatterForm->addRow(QStringLiteral("ABg  B:"), m_abgB);
    scatterForm->addRow(QStringLiteral("ABg  g:"), m_abgG);
    scatterForm->addRow(QStringLiteral("Scatter fraction:"), m_scatter);
    scatterForm->addRow(QStringLiteral("Slope error:"), m_roughness);
    scatterForm->addRow(m_resolved);
    scatterForm->addRow(m_lobePlot);
    layout->addWidget(scatterBox);

    // ---- volume -------------------------------------------------------------
    auto* volBox  = group(QStringLiteral("Volume scattering"), body);
    auto* volForm = new QFormLayout(volBox);

    m_volumeCoefficient = dspin(volBox, 0.0, 10.0, 0.001, 5, QStringLiteral(" /mm"));
    m_volumeCoefficient->setToolTip(QStringLiteral(
        "Scattering coefficient inside the medium behind this surface, not at "
        "it. Every white diffusing plastic in every luminaire is a volume "
        "scatterer, and a surface model has no way to say so."));

    m_volumeAnisotropy = dspin(volBox, -0.99, 0.99, 0.05, 3);
    m_volumeAnisotropy->setToolTip(QStringLiteral(
        "Henyey-Greenstein asymmetry g. 0 is isotropic, positive is forward "
        "scattering -- which is what a filled polymer does."));

    volForm->addRow(QStringLiteral("Coefficient:"), m_volumeCoefficient);
    volForm->addRow(QStringLiteral("Anisotropy g:"), m_volumeAnisotropy);
    layout->addWidget(volBox);

    // ---- appearance ---------------------------------------------------------
    //
    // Its own box, below the physics and visibly not part of it. Everything
    // above changes what a run reports; this changes only what the Appearance
    // tab draws, and a panel that mixed the two would be inviting somebody to
    // paint a surface and then wonder why the efficiency did not move.
    auto* lookBox  = group(QStringLiteral("Appearance (preview only)"), body);
    auto* lookForm = new QFormLayout(lookBox);

    m_appearance = new QPushButton(lookBox);
    m_appearance->setToolTip(QStringLiteral(
        "The colour the Appearance preview paints this surface. It moves no "
        "flux: reflectivity stays a single number, a run gives bit-identical "
        "answers with a colour set or unset, and nothing but the render reads "
        "it. It is here because an imported assembly is mostly painted parts, "
        "and a render in which all of them are the same grey is not a render "
        "of the assembly."));

    m_appearanceClear = new QPushButton(QStringLiteral("Use the physics"), lookBox);
    m_appearanceClear->setToolTip(QStringLiteral(
        "Drop the colour and let the render derive one the way it does for "
        "every built-in scene -- an aluminium reflector's colour is its own "
        "complex index, not a swatch somebody picked."));

    auto* lookRow = new QHBoxLayout;
    lookRow->setContentsMargins(0, 0, 0, 0);
    lookRow->addWidget(m_appearance, 1);
    lookRow->addWidget(m_appearanceClear);
    lookForm->addRow(QStringLiteral("Colour:"), lookRow);
    layout->addWidget(lookBox);

    connect(m_appearance, &QPushButton::clicked, this, [this] {
        QColor start = QColor::fromRgbF(0.8, 0.8, 0.8);
        if (m_appearanceRgb[0] >= 0.0)
            start = QColor::fromRgbF(m_appearanceRgb[0], m_appearanceRgb[1],
                                     m_appearanceRgb[2]);
        const QColor picked = QColorDialog::getColor(
            start, this, QStringLiteral("Preview colour for this surface"));
        if (!picked.isValid()) return;
        m_appearanceRgb[0] = picked.redF();
        m_appearanceRgb[1] = picked.greenF();
        m_appearanceRgb[2] = picked.blueF();
        m_appearanceMixed = false;
        refreshAppearanceSwatch();
        onEdit(multiedit::optic::Appearance);
    });
    connect(m_appearanceClear, &QPushButton::clicked, this, [this] {
        m_appearanceRgb[0] = m_appearanceRgb[1] = m_appearanceRgb[2] = -1.0;
        m_appearanceMixed = false;
        refreshAppearanceSwatch();
        onEdit(multiedit::optic::Appearance);
    });

    // ---- receiver -----------------------------------------------------------
    m_detectorBox = group(QStringLiteral("Receiver"), body);
    auto* detForm = new QFormLayout(m_detectorBox);

    m_detNX = new multiedit::SpinBox(m_detectorBox);
    m_detNX->setRange(0, 4096);
    m_detNY = new multiedit::SpinBox(m_detectorBox);
    m_detNY->setRange(0, 4096);
    for (QSpinBox* s : {m_detNX, m_detNY})
        s->setToolTip(QStringLiteral(
            "Bins across this receiver; 0 keeps the mesher's default grid. "
            "Changing it is a geometry rebuild, not just a re-trace, because "
            "the grid is baked into the receiver layout."));

    m_detAcceptance = dspin(m_detectorBox, 0.0, 180.0, 5.0, 2, QStringLiteral(" deg"));
    m_detAcceptance->setToolTip(QStringLiteral(
        "Acceptance half-angle about the receiver normal. A ray arriving "
        "outside the cone is not counted, which is how a real photometer and a "
        "real fibre behave. 180 accepts everything."));

    m_detReject = new QComboBox(m_detectorBox);
    m_detReject->addItem(QStringLiteral("Absorb it (a photometer head)"));
    m_detReject->addItem(QStringLiteral("Let it through (a recording plane)"));
    m_detReject->setToolTip(QStringLiteral(
        "What happens to a ray the acceptance cone refuses. Either way its "
        "energy is booked as refused rather than absorbed: it was not lost to "
        "anything, it was declined by a measurement condition."));

    detForm->addRow(QStringLiteral("Bins across x:"), m_detNX);
    detForm->addRow(QStringLiteral("Bins across y:"), m_detNY);
    detForm->addRow(QStringLiteral("Acceptance:"), m_detAcceptance);
    detForm->addRow(QStringLiteral("Refused rays:"), m_detReject);
    layout->addWidget(m_detectorBox);

    m_reset = new QPushButton(QStringLiteral("Restore the scene's own optics"), body);
    layout->addWidget(m_reset);
    layout->addStretch(1);

    // ---- wiring -------------------------------------------------------------
    //
    // Tagged one by one rather than swept up in a loop, because each control
    // now has to report *which* property it is: a multi-selection writes back
    // only the fields somebody actually answered.
    namespace F = multiedit::optic;

    struct DoubleTag { QDoubleSpinBox* w; quint32 f; };
    for (const DoubleTag& t : {DoubleTag{m_reflectivity,      F::Reflectivity},
                               DoubleTag{m_transmissivity,    F::Transmissivity},
                               DoubleTag{m_index,             F::Index},
                               DoubleTag{m_absorption,        F::Absorption},
                               DoubleTag{m_coatingResidual,   F::CoatingResidual},
                               DoubleTag{m_bsdfAlpha,         F::BsdfAlpha},
                               DoubleTag{m_bsdfFraction,      F::BsdfFraction},
                               DoubleTag{m_abgA,              F::AbgA},
                               DoubleTag{m_abgB,              F::AbgB},
                               DoubleTag{m_abgG,              F::AbgG},
                               DoubleTag{m_scatter,           F::Scatter},
                               DoubleTag{m_roughness,         F::Roughness},
                               DoubleTag{m_volumeCoefficient, F::VolumeCoefficient},
                               DoubleTag{m_volumeAnisotropy,  F::VolumeAnisotropy},
                               DoubleTag{m_detAcceptance,     F::DetAcceptance}}) {
        const quint32 f = t.f;
        connect(t.w, &QDoubleSpinBox::valueChanged, this, [this, f](double) { onEdit(f); });
    }

    struct IntTag { QSpinBox* w; quint32 f; };
    for (const IntTag& t : {IntTag{m_priority, F::MediumPriority},
                            IntTag{m_detNX,    F::DetNX},
                            IntTag{m_detNY,    F::DetNY}}) {
        const quint32 f = t.f;
        connect(t.w, &QSpinBox::valueChanged, this, [this, f](int) { onEdit(f); });
    }

    struct CheckTag { QCheckBox* w; quint32 f; };
    for (const CheckTag& t : {CheckTag{m_fresnel,     F::Fresnel},
                              CheckTag{m_coatingHigh, F::CoatingHigh}}) {
        QCheckBox* w = t.w;
        const quint32 f = t.f;
        connect(w, &QCheckBox::toggled, this, [this, w, f](bool) {
            multiedit::clearMixed(w);
            onEdit(f);
        });
    }

    struct ComboTag { QComboBox* w; quint32 f; };
    for (const ComboTag& t : {ComboTag{m_material,     F::MaterialChoice},
                              ComboTag{m_coatingModel, F::CoatingModel},
                              ComboTag{m_bsdfModel,    F::BsdfModel},
                              ComboTag{m_detReject,    F::DetReject}}) {
        QComboBox* w = t.w;
        const quint32 f = t.f;
        connect(w, &QComboBox::currentIndexChanged, this, [this, w, f](int) {
            multiedit::clearMixed(w);
            onEdit(f);
        });
    }

    // A preset rewrites the fields rather than living beside them, so what is on
    // screen after choosing one is what will actually be traced.
    connect(m_coatingName, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_loading || i <= 0) return;
        const coating::Coating c = coating::at(i - 1);
        m_loading = true;
        multiedit::clearMixed(m_coatingModel);
        multiedit::clearMixed(m_coatingHigh);
        m_coatingModel->setCurrentIndex(std::clamp(int(c.model), 0, 3));
        m_coatingResidual->setValue(c.residual);
        multiedit::setMixed(m_coatingResidual, false);
        m_coatingHigh->setChecked(c.highReflector);
        m_loading = false;
        // One action, three fields: naming a coating answers all three at once,
        // so all three go to every selected surface.
        onEdit(F::CoatingPreset);
    });
    connect(m_material, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_loading || i <= 0) return;
        // Assigning a material sets the index to its published d-line value, so
        // the default is the glass the user just named.
        const OpticalMaterial m = materials::at(i - 1);
        if (m.valid() && m.nd > 0.0 && !m.isMetal()) {
            m_loading = true;
            m_index->setValue(m.nd);
            multiedit::setMixed(m_index, false);
            if (m.alpha > 0.0) {
                m_absorption->setValue(m.alpha);
                multiedit::setMixed(m_absorption, false);
            }
            m_loading = false;
        }
    });

    connect(m_reset, &QPushButton::clicked, this, &SurfaceInspector::resetRequested);

    clearSurface();
}

void SurfaceInspector::onEdit(quint32 fields) {
    if (m_loading || !m_haveSurface) return;
    refreshAll();
    emit edited(fields);
}

void SurfaceInspector::clearSurface() {
    m_haveSurface  = false;
    m_surfaceCount = 1;
    m_name->setText(QStringLiteral("No surface selected"));
    m_editedNote->clear();
    m_editedNote->setVisible(false);
    setEnabled(false);
}

void SurfaceInspector::setSurface(const QString& label, const SurfaceOptics& o,
                                  const SurfaceOptics& sceneValue, bool edited) {
    m_loading      = true;
    m_haveSurface  = true;
    m_surfaceCount = 1;
    m_sceneValue   = sceneValue;
    setEnabled(true);

    m_name->setText(label.isEmpty() ? QStringLiteral("(unnamed surface)") : label);
    m_editedNote->setVisible(edited);
    m_editedNote->setText(edited ? QStringLiteral("Edited: this differs from what the "
                                                  "scene declares.")
                                 : QString());

    showValues({o});

    m_reset->setVisible(true);
    m_reset->setEnabled(edited);
    m_loading = false;
    refreshAll();
}

void SurfaceInspector::setSurfaces(const QString& label,
                                   const std::vector<SurfaceOptics>& values) {
    if (values.empty()) { clearSurface(); return; }
    if (values.size() == 1) {
        setSurface(label, values.front(), values.front(), false);
        return;
    }

    m_loading      = true;
    m_haveSurface  = true;
    m_surfaceCount = int(values.size());
    // The primary's optics, which is what optics() starts from -- so a field
    // this form does not expose, and every field left as a dash, keeps a real
    // value rather than a zero.
    m_sceneValue   = values.front();
    setEnabled(true);

    m_name->setText(label);
    m_editedNote->setVisible(true);
    m_editedNote->setText(
        QStringLiteral("Editing %1 surfaces at once. A field they disagree on reads "
                       "%2; answering it writes that answer to all %1.")
            .arg(values.size())
            .arg(multiedit::marker()));

    showValues(values);

    // Each of them has its own scene value, so there is no single thing this
    // button could restore.
    m_reset->setVisible(false);
    m_loading = false;
    refreshAll();
}

// Writes the primary's numbers into every widget, then marks the ones the rest
// of the selection disagrees about. Two passes rather than one, because a field
// showing a dash still has to carry a value for its arrows to step from.
void SurfaceInspector::showValues(const std::vector<SurfaceOptics>& values) {
    const SurfaceOptics& o = values.front();

    m_reflectivity->setValue(o.reflectivity);
    m_transmissivity->setValue(o.transmissivity);
    m_fresnel->setChecked(o.fresnel);
    m_priority->setValue(o.mediumPriority);

    multiedit::setMixed(m_material, false);
    m_material->setCurrentIndex(catalogueIndexOf(o.material) + 1);
    m_index->setValue(o.index);
    m_absorption->setValue(o.absorption);

    m_coatingName->setCurrentIndex(0);
    multiedit::setMixed(m_coatingModel, false);
    m_coatingModel->setCurrentIndex(std::clamp(int(o.coating.model), 0, 3));
    m_coatingResidual->setValue(o.coating.residual);
    m_coatingHigh->setChecked(o.coating.highReflector);

    multiedit::setMixed(m_bsdfModel, false);
    m_bsdfModel->setCurrentIndex(std::clamp(int(o.bsdf.model), 0, 4));
    m_bsdfAlpha->setValue(o.bsdf.alpha);
    m_bsdfFraction->setValue(o.bsdf.fraction);
    m_abgA->setValue(o.bsdf.abgA);
    m_abgB->setValue(o.bsdf.abgB);
    m_abgG->setValue(o.bsdf.abgG);
    m_scatter->setValue(o.scatter);
    m_roughness->setValue(o.roughness);
    for (int i = 0; i < 3; ++i) m_appearanceRgb[i] = o.appearanceRgb[i];

    m_volumeCoefficient->setValue(o.volume.coefficient);
    m_volumeAnisotropy->setValue(o.volume.anisotropy);

    // Shown when any of them is a receiver: a selection of four receivers and
    // one mirror still has a bin grid worth setting on the four.
    bool anyDetector = false;
    for (const SurfaceOptics& v : values) anyDetector = anyDetector || v.isDetector;
    m_detectorBox->setVisible(anyDetector);
    m_detNX->setValue(o.detNX);
    m_detNY->setValue(o.detNY);
    m_detAcceptance->setValue(o.detAcceptanceDeg);
    multiedit::setMixed(m_detReject, false);
    m_detReject->setCurrentIndex(int(o.detRejectMode));

    // ---- and now what they disagree about ----
    const auto differs = [&values](auto get) {
        for (std::size_t i = 1; i < values.size(); ++i)
            if (!(get(values[i]) == get(values[0]))) return true;
        return false;
    };

    multiedit::setMixed(m_reflectivity,
        differs([](const SurfaceOptics& v) { return v.reflectivity; }));
    multiedit::setMixed(m_transmissivity,
        differs([](const SurfaceOptics& v) { return v.transmissivity; }));
    multiedit::setMixed(m_fresnel,
        differs([](const SurfaceOptics& v) { return v.fresnel; }));
    multiedit::setMixed(m_priority,
        differs([](const SurfaceOptics& v) { return v.mediumPriority; }));
    multiedit::setMixed(m_material,
        differs([](const SurfaceOptics& v) { return catalogueIndexOf(v.material); }));
    multiedit::setMixed(m_index,
        differs([](const SurfaceOptics& v) { return v.index; }));
    multiedit::setMixed(m_absorption,
        differs([](const SurfaceOptics& v) { return v.absorption; }));
    multiedit::setMixed(m_coatingModel,
        differs([](const SurfaceOptics& v) { return int(v.coating.model); }));
    multiedit::setMixed(m_coatingResidual,
        differs([](const SurfaceOptics& v) { return v.coating.residual; }));
    multiedit::setMixed(m_coatingHigh,
        differs([](const SurfaceOptics& v) { return v.coating.highReflector; }));
    multiedit::setMixed(m_bsdfModel,
        differs([](const SurfaceOptics& v) { return int(v.bsdf.model); }));
    multiedit::setMixed(m_bsdfAlpha,
        differs([](const SurfaceOptics& v) { return v.bsdf.alpha; }));
    multiedit::setMixed(m_bsdfFraction,
        differs([](const SurfaceOptics& v) { return v.bsdf.fraction; }));
    multiedit::setMixed(m_abgA,
        differs([](const SurfaceOptics& v) { return v.bsdf.abgA; }));
    multiedit::setMixed(m_abgB,
        differs([](const SurfaceOptics& v) { return v.bsdf.abgB; }));
    multiedit::setMixed(m_abgG,
        differs([](const SurfaceOptics& v) { return v.bsdf.abgG; }));
    multiedit::setMixed(m_scatter,
        differs([](const SurfaceOptics& v) { return v.scatter; }));
    multiedit::setMixed(m_roughness,
        differs([](const SurfaceOptics& v) { return v.roughness; }));
    multiedit::setMixed(m_volumeCoefficient,
        differs([](const SurfaceOptics& v) { return v.volume.coefficient; }));
    multiedit::setMixed(m_volumeAnisotropy,
        differs([](const SurfaceOptics& v) { return v.volume.anisotropy; }));
    multiedit::setMixed(m_detNX,
        differs([](const SurfaceOptics& v) { return v.detNX; }));
    multiedit::setMixed(m_detNY,
        differs([](const SurfaceOptics& v) { return v.detNY; }));
    multiedit::setMixed(m_detAcceptance,
        differs([](const SurfaceOptics& v) { return v.detAcceptanceDeg; }));
    multiedit::setMixed(m_detReject,
        differs([](const SurfaceOptics& v) { return int(v.detRejectMode); }));

    m_appearanceMixed = differs([](const SurfaceOptics& v) {
        return std::array<double, 3>{v.appearanceRgb[0], v.appearanceRgb[1],
                                     v.appearanceRgb[2]};
    });
    refreshAppearanceSwatch();
}

SurfaceOptics SurfaceInspector::optics() const {
    // Start from what the scene declared, so anything this form does not expose
    // -- a measured coating table, a measured BSDF -- survives an edit to
    // something that it does.
    //
    // It is also where a field showing a dash gets its value from. A dash is
    // the absence of an answer, so nothing is read out of one: the field keeps
    // the primary selection's number here, and the caller's field mask makes
    // sure it is never written anywhere. Reading a mixed combo would be worse
    // than wrong -- its current row is the dash entry, past the end of the
    // enum it maps onto.
    SurfaceOptics o = m_sceneValue;

    if (!multiedit::isMixed(m_reflectivity))   o.reflectivity   = m_reflectivity->value();
    if (!multiedit::isMixed(m_transmissivity)) o.transmissivity = m_transmissivity->value();
    if (!multiedit::isMixed(m_fresnel))        o.fresnel        = m_fresnel->isChecked();
    if (!multiedit::isMixed(m_priority))       o.mediumPriority = m_priority->value();

    if (!multiedit::isMixed(m_material)) {
        const int mat = m_material->currentIndex() - 1;
        o.material = (mat >= 0 && mat < materials::count()) ? materials::at(mat)
                                                           : OpticalMaterial{};
    }
    if (!multiedit::isMixed(m_index))      o.index      = m_index->value();
    if (!multiedit::isMixed(m_absorption)) o.absorption = m_absorption->value();

    if (!multiedit::isMixed(m_coatingModel))
        o.coating.model = coating::Model(std::clamp(m_coatingModel->currentIndex(), 0, 3));
    if (!multiedit::isMixed(m_coatingResidual))
        o.coating.residual = m_coatingResidual->value();
    if (!multiedit::isMixed(m_coatingHigh))
        o.coating.highReflector = m_coatingHigh->isChecked();

    if (!multiedit::isMixed(m_bsdfModel))
        o.bsdf.model = bsdf::Model(std::clamp(m_bsdfModel->currentIndex(), 0, 4));
    if (!multiedit::isMixed(m_bsdfAlpha))    o.bsdf.alpha    = m_bsdfAlpha->value();
    if (!multiedit::isMixed(m_bsdfFraction)) o.bsdf.fraction = m_bsdfFraction->value();
    if (!multiedit::isMixed(m_abgA))         o.bsdf.abgA     = m_abgA->value();
    if (!multiedit::isMixed(m_abgB))         o.bsdf.abgB     = m_abgB->value();
    if (!multiedit::isMixed(m_abgG))         o.bsdf.abgG     = m_abgG->value();
    if (!multiedit::isMixed(m_scatter))      o.scatter       = m_scatter->value();
    if (!multiedit::isMixed(m_roughness))    o.roughness     = m_roughness->value();

    if (!m_appearanceMixed) {
        if (m_appearanceRgb[0] >= 0.0)
            o.setAppearanceColour(m_appearanceRgb[0], m_appearanceRgb[1],
                                  m_appearanceRgb[2]);
        else
            o.clearAppearanceColour();
    }

    if (!multiedit::isMixed(m_volumeCoefficient))
        o.volume.coefficient = m_volumeCoefficient->value();
    if (!multiedit::isMixed(m_volumeAnisotropy))
        o.volume.anisotropy = m_volumeAnisotropy->value();

    // The receiver rows are shown whenever any of the selection is one, so they
    // are read whenever they are shown -- a mask of receiver fields written
    // onto a mirror is refused by applyOpticsFields, not here.
    if (m_detectorBox->isVisible()) {
        if (!multiedit::isMixed(m_detNX))         o.detNX            = m_detNX->value();
        if (!multiedit::isMixed(m_detNY))         o.detNY            = m_detNY->value();
        if (!multiedit::isMixed(m_detAcceptance)) o.detAcceptanceDeg = m_detAcceptance->value();
        if (!multiedit::isMixed(m_detReject))
            o.detRejectMode =
                SurfaceOptics::RejectMode(std::clamp(m_detReject->currentIndex(), 0, 1));
    }
    return o;
}

// The button *is* the swatch: a colour chip that says what it is beats a chip
// beside a label repeating it.
void SurfaceInspector::refreshAppearanceSwatch() {
    if (!m_appearance) return;

    if (m_appearanceMixed) {
        m_appearance->setStyleSheet(QString());
        m_appearance->setText(multiedit::marker() + QStringLiteral("  (they differ)"));
        if (m_appearanceClear) m_appearanceClear->setEnabled(true);
        return;
    }

    if (m_appearanceRgb[0] < 0.0) {
        m_appearance->setStyleSheet(QString());
        m_appearance->setText(QStringLiteral("From the physics"));
        if (m_appearanceClear) m_appearanceClear->setEnabled(false);
        return;
    }

    const QColor c = QColor::fromRgbF(m_appearanceRgb[0], m_appearanceRgb[1],
                                      m_appearanceRgb[2]);
    // Legible text on whatever was picked, by the same luminance rule a print
    // designer would use rather than by guessing.
    const double luma = 0.2126 * m_appearanceRgb[0] + 0.7152 * m_appearanceRgb[1] +
                        0.0722 * m_appearanceRgb[2];
    m_appearance->setStyleSheet(
        QStringLiteral("background:%1; color:%2; padding:4px;")
            .arg(c.name(), luma > 0.5 ? QStringLiteral("#000") : QStringLiteral("#fff")));
    m_appearance->setText(c.name().toUpper());
    if (m_appearanceClear) m_appearanceClear->setEnabled(true);
}

void SurfaceInspector::refreshAll() {
    syncEnabledState();
    refreshMaterialReadout();
    refreshCoatingPlot();
    refreshLobePlot();
}

void SurfaceInspector::syncEnabledState() {
    // A dashed field says nothing about what to grey out, so it is read as the
    // permissive answer: with several surfaces disagreeing about their index,
    // the Fresnel box stays reachable rather than being disabled on the
    // strength of whichever one happened to be primary.
    const bool refracts = multiedit::isMixed(m_index) || m_index->value() > 0.0;
    const auto model    = bsdf::Model(
        multiedit::isMixed(m_bsdfModel) ? 0 : std::clamp(m_bsdfModel->currentIndex(), 0, 4));

    const bool fresnelOn = !multiedit::isMixed(m_fresnel) && m_fresnel->isChecked();
    m_fresnel->setEnabled(refracts);
    m_transmissivity->setEnabled(!fresnelOn || !refracts);
    m_reflectivity->setEnabled(!fresnelOn || !refracts);
    m_absorption->setEnabled(refracts);
    // A coating modulates a refractive interface; on an opaque surface there is
    // no transmitted branch for it to act on.
    m_coatingBox->setEnabled(refracts);

    m_bsdfAlpha->setEnabled(model == bsdf::Model::Microfacet);
    m_bsdfFraction->setEnabled(model != bsdf::Model::Specular);
    const bool abg = (model == bsdf::Model::Abg);
    m_abgA->setEnabled(abg);
    m_abgB->setEnabled(abg);
    m_abgG->setEnabled(abg);
    // The two shorthands only mean anything while no real BSDF is set: they are
    // read into one, and the BSDF wins.
    const bool shorthand = (model == bsdf::Model::Specular);
    m_scatter->setEnabled(shorthand);
    m_roughness->setEnabled(shorthand &&
                            (multiedit::isMixed(m_scatter) || m_scatter->value() <= 0.0));

    m_bsdfTip->setText(bsdf::modelTip(model));

    const bool coated = multiedit::isMixed(m_coatingModel) ||
                        coating::Model(std::clamp(m_coatingModel->currentIndex(), 0, 3)) !=
                            coating::Model::None;
    m_coatingResidual->setEnabled(coated);
    m_coatingHigh->setEnabled(coated);
}

void SurfaceInspector::refreshMaterialReadout() {
    if (multiedit::isMixed(m_material)) {
        m_materialInfo->setText(QStringLiteral(
            "The selected surfaces are not all the same material. Choosing one "
            "here assigns it to every one of them."));
        return;
    }
    const int mat = m_material->currentIndex() - 1;
    if (mat < 0 || mat >= materials::count()) {
        m_materialInfo->setText(QStringLiteral(
            "No catalogue material: the index above is used flat, or with the "
            "Cauchy shorthand the scene set."));
        return;
    }
    const OpticalMaterial m = materials::at(mat);
    if (m.isMetal()) {
        const double n = m.indexAt(550.0), k = m.extinctionAt(550.0);
        m_materialInfo->setText(
            QStringLiteral("Metal. R at 550 nm: %1 %% at normal, %2 %% at 45 deg, "
                           "%3 %% at 70 deg.")
                .arg(100.0 * metalReflectance(1.0, n, k, 1.0), 0, 'f', 1)
                .arg(100.0 * metalReflectance(1.0, n, k, std::cos(optics::kDegRad * 45.0)),
                     0, 'f', 1)
                .arg(100.0 * metalReflectance(1.0, n, k, std::cos(optics::kDegRad * 70.0)),
                     0, 'f', 1));
        return;
    }
    // Make the fidelity of a measured table legible right where its n is read:
    // "N points, full fidelity" when the file fit the hot-path arrays exactly,
    // and "reduced to M at load" when a file exceeded the cap (its exact source
    // is still kept), so a curve that was thinned is never mistaken for intact.
    QString fidelity;
    if (m.model == OpticalMaterial::Model::Table && m.originCount > 0) {
        fidelity = m.reduced
            ? QStringLiteral(" | %1 measured points reduced to %2 on the trace path "
                             "(exact source kept)").arg(m.originCount).arg(m.samples)
            : QStringLiteral(" | %1 measured points, full fidelity").arg(m.originCount);
    }
    m_materialInfo->setText(QStringLiteral("n_d %1, Abbe %2, alpha %3 /mm%4")
                                .arg(m.indexAt(587.6), 0, 'f', 4)
                                .arg(m.abbe(), 0, 'f', 1)
                                .arg(m.alpha, 0, 'g', 3)
                                .arg(fidelity));
}

void SurfaceInspector::refreshCoatingPlot() {
    const SurfaceOptics o = optics();
    if (!m_coatingPlot) return;

    if (o.index <= 0.0) {
        m_coatingPlot->clear();
        m_coatingPlot->setPlaceholder(QStringLiteral("Opaque: no interface to coat"));
        return;
    }

    // Three wavelengths across the visible, because a coating is a surface in
    // (angle, wavelength) and one curve of it is half the story: a V-coat rises
    // steeply off its design angle and a broadband AR has a shallow minimum
    // that walks with both.
    struct Band { double nm; QColor colour; const char* name; };
    const Band bands[] = {
        {460.0, QColor(120, 160, 255), "460 nm"},
        {550.0, QColor(140, 220, 140), "550 nm"},
        {620.0, QColor(255, 150, 120), "620 nm"},
    };

    std::vector<PlotSeries> series;
    for (const Band& b : bands) {
        PlotSeries s;
        s.name  = QLatin1String(b.name);
        s.color = b.colour;
        for (int i = 0; i <= 89; ++i) {
            const double th   = double(i);
            const double cosI = std::cos(th * optics::kDegRad);
            const double n2   = o.indexAt(b.nm);
            const double bare = optics::fresnelReflectance(1.0, n2, cosI);
            double r = bare;
            if (o.coating.active()) {
                double rs = 0.0, rp = 0.0, phase = 0.0;
                polarisation::fresnelAmplitudes(1.0, n2, cosI, rs, rp, phase);
                double outS = 0.0, outP = 0.0;
                o.coating.reflectanceSP(1.0, n2, cosI, b.nm, rs * rs, rp * rp, outS, outP);
                r = 0.5 * (outS + outP);
            }
            s.x.push_back(th);
            s.y.push_back(r);
        }
        series.push_back(std::move(s));
    }
    m_coatingPlot->setTitle(o.coating.active()
                                ? QStringLiteral("Coated reflectance, air onto n_d %1")
                                      .arg(o.index, 0, 'f', 3)
                                : QStringLiteral("Bare Fresnel, air onto n_d %1")
                                      .arg(o.index, 0, 'f', 3));
    m_coatingPlot->setSeries(std::move(series));

    // Make the fidelity of a measured R(lambda) coating visible alongside its
    // curve: how many points it carries, and whether a file that exceeded the
    // cap was thinned (its exact source is still kept off-path).
    if (o.coating.model == coating::Model::Table) {
        if (o.coating.originCount > 0) {
            m_coatingInfo->setText(
                o.coating.reduced
                    ? QStringLiteral("Measured R(lambda): %1 points, resampled onto %2 on "
                                     "the trace path (exact source kept).")
                          .arg(o.coating.originCount).arg(o.coating.samples)
                    : QStringLiteral("Measured R(lambda): %1 points, full fidelity.")
                          .arg(o.coating.originCount));
        } else if (o.coating.samples > 0) {
            m_coatingInfo->setText(QStringLiteral("Measured R(lambda): %1 points.")
                                       .arg(o.coating.samples));
        } else {
            m_coatingInfo->clear();
        }
    } else {
        m_coatingInfo->clear();
    }
}

void SurfaceInspector::refreshLobePlot() {
    if (!m_lobePlot) return;
    const SurfaceOptics o = optics();
    const bsdf::Surface b = o.effectiveBsdf();

    m_resolved->setText(
        QStringLiteral("In force: %1%2")
            .arg(b.isSpecular() ? QStringLiteral("polished (specular only)")
                                : bsdf::modelName(b.model))
            .arg(b.isSpecular()
                     ? QString()
                     : QStringLiteral(", total integrated scatter %1 %%")
                           .arg(100.0 * b.totalIntegratedScatter(), 0, 'f', 2)));

    if (b.isSpecular()) {
        m_lobePlot->clear();
        return;
    }

    PlotSeries s;
    s.name  = bsdf::modelName(b.model);
    s.color = QColor(255, 185, 110);
    // Log-spaced in the offset from specular, because a BSDF spans decades and
    // the interesting part is the wings.
    for (int i = 0; i <= 120; ++i) {
        const double dbeta = std::pow(10.0, -4.0 + 4.0 * double(i) / 120.0);
        if (dbeta > 1.0) break;
        const double v = b.value(dbeta);
        if (!(v > 0.0)) continue;
        s.x.push_back(dbeta);
        s.y.push_back(v);
    }
    m_lobePlot->setTitle(QStringLiteral("Scatter lobe about the specular direction"));
    m_lobePlot->setSeries({std::move(s)});
}
