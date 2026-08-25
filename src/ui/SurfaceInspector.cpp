#include "SurfaceInspector.h"
#include "PlotWidget.h"

#include "core/Material.h"
#include "core/Optics.h"
#include "core/Polarisation.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QDoubleSpinBox* dspin(QWidget* p, double lo, double hi, double step, int decimals,
                      const QString& suffix = QString()) {
    auto* s = new QDoubleSpinBox(p);
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

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

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

    m_priority = new QSpinBox(basicBox);
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

    m_coatingPlot = new PlotWidget(m_coatingBox);
    m_coatingPlot->setMinimumHeight(130);
    m_coatingPlot->setAxisLabels(QStringLiteral("angle of incidence [deg]"),
                                 QStringLiteral("R"));
    m_coatingPlot->setPlaceholder(QStringLiteral("No coating"));

    coatForm->addRow(QStringLiteral("Preset:"), m_coatingName);
    coatForm->addRow(QStringLiteral("Model:"), m_coatingModel);
    coatForm->addRow(QStringLiteral("Residual R:"), m_coatingResidual);
    coatForm->addRow(m_coatingHigh);
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

    // ---- receiver -----------------------------------------------------------
    m_detectorBox = group(QStringLiteral("Receiver"), body);
    auto* detForm = new QFormLayout(m_detectorBox);

    m_detNX = new QSpinBox(m_detectorBox);
    m_detNX->setRange(0, 4096);
    m_detNY = new QSpinBox(m_detectorBox);
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
    for (QDoubleSpinBox* s : {m_reflectivity, m_transmissivity, m_index, m_absorption,
                              m_coatingResidual, m_bsdfAlpha, m_bsdfFraction,
                              m_abgA, m_abgB, m_abgG, m_scatter, m_roughness,
                              m_volumeCoefficient, m_volumeAnisotropy, m_detAcceptance})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this](double) { onEdit(); });
    for (QSpinBox* s : {m_priority, m_detNX, m_detNY})
        connect(s, &QSpinBox::valueChanged, this, [this](int) { onEdit(); });
    for (QCheckBox* c : {m_fresnel, m_coatingHigh})
        connect(c, &QCheckBox::toggled, this, [this](bool) { onEdit(); });
    for (QComboBox* c : {m_material, m_coatingModel, m_bsdfModel, m_detReject})
        connect(c, &QComboBox::currentIndexChanged, this, [this](int) { onEdit(); });

    // A preset rewrites the fields rather than living beside them, so what is on
    // screen after choosing one is what will actually be traced.
    connect(m_coatingName, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_loading || i <= 0) return;
        const coating::Coating c = coating::at(i - 1);
        m_loading = true;
        m_coatingModel->setCurrentIndex(std::clamp(int(c.model), 0, 3));
        m_coatingResidual->setValue(c.residual);
        m_coatingHigh->setChecked(c.highReflector);
        m_loading = false;
        onEdit();
    });
    connect(m_material, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_loading || i <= 0) return;
        // Assigning a material sets the index to its published d-line value, so
        // the default is the glass the user just named.
        const OpticalMaterial m = materials::at(i - 1);
        if (m.valid() && m.nd > 0.0 && !m.isMetal()) {
            m_loading = true;
            m_index->setValue(m.nd);
            if (m.alpha > 0.0) m_absorption->setValue(m.alpha);
            m_loading = false;
        }
    });

    connect(m_reset, &QPushButton::clicked, this, &SurfaceInspector::resetRequested);

    clearSurface();
}

void SurfaceInspector::onEdit() {
    if (m_loading || !m_haveSurface) return;
    refreshAll();
    emit edited();
}

void SurfaceInspector::clearSurface() {
    m_haveSurface = false;
    m_name->setText(QStringLiteral("No surface selected"));
    m_editedNote->clear();
    m_editedNote->setVisible(false);
    setEnabled(false);
}

void SurfaceInspector::setSurface(const QString& label, const SurfaceOptics& o,
                                  const SurfaceOptics& sceneValue, bool edited) {
    m_loading     = true;
    m_haveSurface = true;
    m_sceneValue  = sceneValue;
    setEnabled(true);

    m_name->setText(label.isEmpty() ? QStringLiteral("(unnamed surface)") : label);
    m_editedNote->setVisible(edited);
    m_editedNote->setText(edited ? QStringLiteral("Edited: this differs from what the "
                                                  "scene declares.")
                                 : QString());

    m_reflectivity->setValue(o.reflectivity);
    m_transmissivity->setValue(o.transmissivity);
    m_fresnel->setChecked(o.fresnel);
    m_priority->setValue(o.mediumPriority);

    m_material->setCurrentIndex(catalogueIndexOf(o.material) + 1);
    m_index->setValue(o.index);
    m_absorption->setValue(o.absorption);

    m_coatingName->setCurrentIndex(0);
    m_coatingModel->setCurrentIndex(std::clamp(int(o.coating.model), 0, 3));
    m_coatingResidual->setValue(o.coating.residual);
    m_coatingHigh->setChecked(o.coating.highReflector);

    m_bsdfModel->setCurrentIndex(std::clamp(int(o.bsdf.model), 0, 4));
    m_bsdfAlpha->setValue(o.bsdf.alpha);
    m_bsdfFraction->setValue(o.bsdf.fraction);
    m_abgA->setValue(o.bsdf.abgA);
    m_abgB->setValue(o.bsdf.abgB);
    m_abgG->setValue(o.bsdf.abgG);
    m_scatter->setValue(o.scatter);
    m_roughness->setValue(o.roughness);

    m_volumeCoefficient->setValue(o.volume.coefficient);
    m_volumeAnisotropy->setValue(o.volume.anisotropy);

    m_detectorBox->setVisible(o.isDetector);
    m_detNX->setValue(o.detNX);
    m_detNY->setValue(o.detNY);
    m_detAcceptance->setValue(o.detAcceptanceDeg);
    m_detReject->setCurrentIndex(int(o.detRejectMode));

    m_reset->setEnabled(edited);
    m_loading = false;
    refreshAll();
}

SurfaceOptics SurfaceInspector::optics() const {
    // Start from what the scene declared, so anything this form does not expose
    // -- a measured coating table, a measured BSDF -- survives an edit to
    // something that it does.
    SurfaceOptics o = m_sceneValue;

    o.reflectivity   = m_reflectivity->value();
    o.transmissivity = m_transmissivity->value();
    o.fresnel        = m_fresnel->isChecked();
    o.mediumPriority = m_priority->value();

    const int mat = m_material->currentIndex() - 1;
    o.material   = (mat >= 0) ? materials::at(mat) : OpticalMaterial{};
    o.index      = m_index->value();
    o.absorption = m_absorption->value();

    o.coating.model         = coating::Model(m_coatingModel->currentIndex());
    o.coating.residual      = m_coatingResidual->value();
    o.coating.highReflector = m_coatingHigh->isChecked();

    o.bsdf.model    = bsdf::Model(m_bsdfModel->currentIndex());
    o.bsdf.alpha    = m_bsdfAlpha->value();
    o.bsdf.fraction = m_bsdfFraction->value();
    o.bsdf.abgA     = m_abgA->value();
    o.bsdf.abgB     = m_abgB->value();
    o.bsdf.abgG     = m_abgG->value();
    o.scatter       = m_scatter->value();
    o.roughness     = m_roughness->value();

    o.volume.coefficient = m_volumeCoefficient->value();
    o.volume.anisotropy  = m_volumeAnisotropy->value();

    if (o.isDetector) {
        o.detNX            = m_detNX->value();
        o.detNY            = m_detNY->value();
        o.detAcceptanceDeg = m_detAcceptance->value();
        o.detRejectMode    = SurfaceOptics::RejectMode(m_detReject->currentIndex());
    }
    return o;
}

void SurfaceInspector::refreshAll() {
    syncEnabledState();
    refreshMaterialReadout();
    refreshCoatingPlot();
    refreshLobePlot();
}

void SurfaceInspector::syncEnabledState() {
    const bool refracts = m_index->value() > 0.0;
    const auto model    = bsdf::Model(m_bsdfModel->currentIndex());

    m_fresnel->setEnabled(refracts);
    m_transmissivity->setEnabled(!m_fresnel->isChecked() || !refracts);
    m_reflectivity->setEnabled(!m_fresnel->isChecked() || !refracts);
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
    m_roughness->setEnabled(shorthand && m_scatter->value() <= 0.0);

    m_bsdfTip->setText(bsdf::modelTip(model));

    const bool coated = coating::Model(m_coatingModel->currentIndex()) != coating::Model::None;
    m_coatingResidual->setEnabled(coated);
    m_coatingHigh->setEnabled(coated);
}

void SurfaceInspector::refreshMaterialReadout() {
    const int mat = m_material->currentIndex() - 1;
    if (mat < 0) {
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
    m_materialInfo->setText(QStringLiteral("n_d %1, Abbe %2, alpha %3 /mm")
                                .arg(m.indexAt(587.6), 0, 'f', 4)
                                .arg(m.abbe(), 0, 'f', 1)
                                .arg(m.alpha, 0, 'g', 3));
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
