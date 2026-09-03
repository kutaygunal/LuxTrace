// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "ControlsPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {

QDoubleSpinBox* dspin(QWidget* parent, double mn, double mx, double step, int decimals,
                      double value, const QString& suffix = QString()) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(mn, mx);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setValue(value);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    s->setKeyboardTracking(false);   // do not rebuild geometry on every keystroke
    return s;
}

QGroupBox* group(const QString& title, QWidget* parent) {
    auto* g = new QGroupBox(title, parent);
    g->setStyleSheet(QStringLiteral("QGroupBox{font-weight:bold;margin-top:8px;}"
                                    "QGroupBox::title{subcontrol-origin:margin;left:6px;}"));
    return g;
}

} // namespace

ControlsPanel::ControlsPanel(QWidget* parent) : QWidget(parent) {
    // How wide this column is, is the window's decision -- it sits in a
    // splitter the user can drag. It used to be nailed to 330 px, which is
    // narrower than the widest row it holds (the flux value and its unit, side
    // by side). The scroll area below scrolls vertically only, so that surplus
    // became a horizontal offset with no scrollbar to undo it, and every run
    // pushed it further: disabling Run moves the keyboard focus, and a scroll
    // area scrolls sideways to keep the focused widget in view. That is how the
    // labels and the checkbox indicators ended up off the left edge.
    //
    // minimumSizeHint() below is what stops it for good -- refusing to be drawn
    // narrower than the content leaves nothing to scroll sideways.

    // The panel outgrew a fixed column once the physics and source blocks
    // arrived, so it scrolls rather than squeezing every row.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(m_scroll);
    m_body = body;
    m_scroll->setWidget(body);
    outer->addWidget(m_scroll, 1);

    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* title = new QLabel(QStringLiteral("LuxTrace"), body);
    title->setStyleSheet(QStringLiteral("font-weight:bold; font-size:15px;"));
    auto* subtitle = new QLabel(QStringLiteral("Optical Design Studio"), body);
    subtitle->setStyleSheet(QStringLiteral("color:#8a8f9c; font-size:11px;"));
    layout->addWidget(title);
    layout->addWidget(subtitle);


    // Names the file when one stands in for the scene above, so the two are
    // never both silently in play.
    m_importNote = new QLabel(body);
    m_importNote->setWordWrap(true);
    m_importNote->setStyleSheet(QStringLiteral("color:#d0a040;"));
    m_importNote->setVisible(false);
    layout->addWidget(m_importNote);

    // ---- geometry parameters ----------------------------------------------
    m_paramBox = group(QStringLiteral("Geometry"), body);
    auto* paramLayout = new QVBoxLayout(m_paramBox);
    m_paramForm = new QFormLayout;
    paramLayout->addLayout(m_paramForm);
    m_resetParams = new QPushButton(QStringLiteral("Reset to scene defaults"), m_paramBox);
    paramLayout->addWidget(m_resetParams);
    layout->addWidget(m_paramBox);


    // ---- physics -----------------------------------------------------------
    auto* physBox = group(QStringLiteral("Physics"), body);
    auto* physLayout = new QVBoxLayout(physBox);

    m_fresnel = new QCheckBox(QStringLiteral("Fresnel reflectance"), physBox);
    m_fresnel->setChecked(true);
    m_fresnel->setToolTip(QStringLiteral(
        "Angle-dependent R/T at refractive surfaces: ~4 % at normal incidence, "
        "rising to 1 at grazing, exactly 1 past the critical angle. Switch it off "
        "to fall back to each surface's fixed split."));

    m_absorption = new QCheckBox(QStringLiteral("Bulk absorption (Beer-Lambert)"), physBox);
    m_absorption->setChecked(true);
    m_absorption->setToolTip(QStringLiteral(
        "exp(-alpha x path length) inside a refractive solid. This is the loss "
        "that makes a long light guide dimmer than a short one."));

    m_coatings = new QCheckBox(QStringLiteral("Coatings"), physBox);
    m_coatings->setChecked(true);
    m_coatings->setToolTip(QStringLiteral(
        "Thin films on refractive surfaces. Without them every surface pays bare "
        "Fresnel, so a multi-element system overstates its loss by roughly 3.5 % "
        "per surface against any real lens -- all of which are coated. Switch it "
        "off to see what the bare glass would have done."));

    m_volume = new QCheckBox(QStringLiteral("Volume scattering"), physBox);
    m_volume->setChecked(true);
    m_volume->setToolTip(QStringLiteral(
        "Scattering inside a medium rather than at its boundary. Every white "
        "diffusing plastic in every luminaire is a volume scatterer, and a "
        "surface model has no way to say so."));

    m_polarised = new QCheckBox(QStringLiteral("Polarisation (Stokes / Mueller)"), physBox);
    m_polarised->setChecked(false);
    m_polarised->setToolTip(QStringLiteral(
        "Carries a Stokes vector on every branch and applies a Mueller matrix at "
        "every interaction. It costs roughly four times the per-ray state and "
        "most illumination work does not need it, so the unpolarised fast path "
        "is the default -- but it is what makes Brewster's angle, a polariser "
        "and the phase of total internal reflection expressible at all."));

    m_scattering = new QCheckBox(QStringLiteral("Diffuse scattering"), physBox);
    m_scattering->setChecked(true);

    m_roughness = new QCheckBox(QStringLiteral("Surface roughness"), physBox);
    m_roughness->setChecked(true);

    m_dispersion = new QCheckBox(QStringLiteral("Dispersion n(λ)"), physBox);
    m_dispersion->setChecked(true);
    m_dispersion->setToolTip(QStringLiteral(
        "Cauchy index. It only changes anything on a spectral run, since a "
        "monochromatic trace sits at the line the indices are quoted at."));

    physLayout->addWidget(m_fresnel);
    physLayout->addWidget(m_absorption);
    physLayout->addWidget(m_scattering);
    physLayout->addWidget(m_roughness);
    physLayout->addWidget(m_dispersion);

    auto* physForm = new QFormLayout;
    m_roughOverride = dspin(physBox, -1.0, 0.30, 0.005, 4, -1.0, QStringLiteral(" rad"));
    m_roughOverride->setSpecialValueText(QStringLiteral("per surface"));
    m_roughOverride->setToolTip(QStringLiteral(
        "Applies one RMS slope error to every optic in the scene. Roughness is a "
        "ray-time property, so this needs no geometry rebuild -- drag it and "
        "re-run to watch a polished mirror turn matte."));
    m_scatterOverride = dspin(physBox, -1.0, 1.0, 0.05, 3, -1.0);
    m_scatterOverride->setSpecialValueText(QStringLiteral("per surface"));
    m_scatterOverride->setToolTip(QStringLiteral(
        "Fraction of every outgoing branch re-emitted cosine-weighted. 1.0 turns "
        "every surface in the scene into a Lambertian diffuser."));
    m_absorptionScale = dspin(physBox, 0.0, 200.0, 0.5, 2, 1.0, QStringLiteral(" ×"));
    m_absorptionScale->setToolTip(QStringLiteral(
        "Multiplies every medium's attenuation coefficient. 1 is ordinary optical "
        "glass; 20 is roughly a cheap plastic light pipe."));

    physForm->addRow(QStringLiteral("Roughness:"), m_roughOverride);
    physForm->addRow(QStringLiteral("Scatter:"), m_scatterOverride);
    physForm->addRow(QStringLiteral("Absorption:"), m_absorptionScale);
    physLayout->addWidget(m_coatings);
    physLayout->addWidget(m_volume);
    physLayout->addWidget(m_polarised);
    physLayout->addLayout(physForm);
    layout->addWidget(physBox);

    // ---- run ---------------------------------------------------------------
    auto* runBox = group(QStringLiteral("Run"), body);
    auto* runForm = new QFormLayout(runBox);

    m_rays = new QSpinBox(runBox);
    m_rays->setRange(100, 10000000);
    m_rays->setSingleStep(10000);
    m_rays->setValue(100000);
    m_rays->setGroupSeparatorShown(true);
    m_rays->setSuffix(QStringLiteral(" rays"));
    m_rays->setKeyboardTracking(false);

    m_seed = new QSpinBox(runBox);
    m_seed->setRange(0, 2000000000);
    m_seed->setValue(12345);
    m_seed->setKeyboardTracking(false);
    m_seed->setToolTip(QStringLiteral(
        "A run is fully determined by this seed and the ray count -- same inputs, "
        "same answer, whatever the thread count."));

    m_threads = new QSpinBox(runBox);
    m_threads->setRange(0, 256);
    m_threads->setValue(0);
    m_threads->setSpecialValueText(QStringLiteral("all cores"));
    m_threads->setKeyboardTracking(false);

    m_detBins = new QComboBox(runBox);
    for (int n : {32, 64, 128, 256, 512, 1024})
        m_detBins->addItem(QStringLiteral("%1 x %1").arg(n), n);
    m_detBins->setCurrentIndex(1);            // 64 x 64, the long-standing default
    m_detBins->setToolTip(QStringLiteral(
        "Receiver grid. A finer grid resolves a tight spot but puts fewer rays in "
        "each bin, so the uniformity figures get noisier -- trace more rays with "
        "it. Changing it rebuilds the scene."));
    m_detBins->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

    // The unit every result is quoted in. Flux itself belongs to each emitter,
    // but what it is measured in is a property of the run: a scene cannot report
    // half its light in watts and half in lumens.
    m_powerUnit = new QComboBox(runBox);
    m_powerUnit->addItem(QStringLiteral("W (radiometric)"));
    m_powerUnit->addItem(QStringLiteral("lm (photometric)"));
    m_powerUnit->setItemData(1, QStringLiteral(
        "Lumens are watts weighted by the CIE V(lambda) curve, so a photometric "
        "run needs a real spectrum: each ray is emitted carrying how much of it "
        "the eye sees."), Qt::ToolTipRole);
    m_powerUnit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

    runForm->addRow(QStringLiteral("Rays:"), m_rays);
    runForm->addRow(QStringLiteral("Flux unit:"), m_powerUnit);
    runForm->addRow(QStringLiteral("Receiver grid:"), m_detBins);
    runForm->addRow(QStringLiteral("Seed:"), m_seed);
    runForm->addRow(QStringLiteral("Threads:"), m_threads);
    layout->addWidget(runBox);

    m_run = new QPushButton(QStringLiteral("Run Simulation"), body);
    m_run->setDefault(true);
    m_cancel = new QPushButton(QStringLiteral("Cancel"), body);
    m_cancel->setEnabled(false);

    m_progress = new QProgressBar(body);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);

    m_status = new QLabel(QStringLiteral("Idle"), body);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color:#888;"));

    layout->addWidget(m_run);
    layout->addWidget(m_cancel);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addStretch();

    // ---- wiring ------------------------------------------------------------
    connect(m_run, &QPushButton::clicked, this, &ControlsPanel::runRequested);
    connect(m_cancel, &QPushButton::clicked, this, &ControlsPanel::cancelRequested);

    connect(m_resetParams, &QPushButton::clicked, this, [this] {
        rebuildParamRows();
        emit geometryChanged();
    });

    auto settings = [this] { if (!m_loading) { syncEnabledState(); emit settingsChanged(); } };
    connect(m_coatings,   &QCheckBox::toggled, this, settings);
    connect(m_volume,     &QCheckBox::toggled, this, settings);
    connect(m_polarised,  &QCheckBox::toggled, this, settings);
    connect(m_powerUnit,  &QComboBox::currentIndexChanged, this, settings);
    connect(m_detBins,    &QComboBox::currentIndexChanged, this, settings);
    for (QDoubleSpinBox* s : {m_roughOverride, m_scatterOverride, m_absorptionScale})
        connect(s, &QDoubleSpinBox::valueChanged, this, settings);
    for (QCheckBox* c : {m_fresnel, m_absorption, m_scattering, m_roughness, m_dispersion})
        connect(c, &QCheckBox::toggled, this, settings);
    for (QSpinBox* s : {m_rays, m_seed, m_threads})
        connect(s, &QSpinBox::valueChanged, this, settings);

    rebuildParamRows();
    syncEnabledState();
}

QSize ControlsPanel::minimumSizeHint() const {
    QSize base = QWidget::minimumSizeHint();
    if (!m_body || !m_scroll) return base;
    // The narrowest this column can be drawn with all of it on screen: the
    // widest row it holds, plus the vertical scrollbar beside it.
    //
    // Answered on demand rather than stored, because the number moves: a
    // scene's parameter names set the width of the label column, and none of
    // the font metrics behind any of it are final until the widget is polished.
    m_body->ensurePolished();
    base.setWidth(m_body->minimumSizeHint().width() +
                  style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this) +
                  2 * m_scroll->frameWidth());
    return base;
}

QSize ControlsPanel::sizeHint() const {
    // Exactly as wide as the content needs, and no wider. The splitter opens
    // the window at this and hands the width over to the user from there.
    return QSize(minimumSizeHint().width(), QWidget::sizeHint().height());
}

void ControlsPanel::rebuildParamRows() {
    const bool wasLoading = m_loading;
    m_loading = true;

    while (m_paramForm->rowCount() > 0) m_paramForm->removeRow(0);
    m_params.clear();

    const GeometryProvider::Scene sc = scene();
    const auto& infos = GeometryProvider::paramInfo(sc);
    for (std::size_t i = 0; i < infos.size() && i < SceneParams::kMax; ++i) {
        const SceneParamInfo& info = infos[i];
        QDoubleSpinBox* s = dspin(m_paramBox, info.min, info.max, info.step, info.decimals,
                                  info.def, info.unit.isEmpty() ? QString()
                                                                : QStringLiteral(" ") + info.unit);
        if (!info.tip.isEmpty()) s->setToolTip(info.tip);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            if (!m_loading) emit geometryChanged();
        });
        m_paramForm->addRow(info.name + QStringLiteral(":"), s);
        m_params.push_back(s);
    }
    m_paramBox->setVisible(!m_params.empty());

    m_loading = wasLoading;
    // A scene's parameter names set the width of the label column, and the
    // column may not fit what it did a moment ago.
    updateGeometry();
}

void ControlsPanel::syncEnabledState() {
    // The emitted polarisation state means nothing unless the state is being
    // carried; it is set per source, in the object inspector.
    m_polarised->setToolTip(
        m_polarised->isChecked()
            ? QStringLiteral("Each source's emitted state is set on the source itself.")
            : m_polarised->toolTip());
}

SceneParams ControlsPanel::params() const {
    SceneParams p;
    p.n = int(std::min(m_params.size(), std::size_t(SceneParams::kMax)));
    for (std::size_t i = 0; i < m_params.size() && i < SceneParams::kMax; ++i)
        p.v[i] = m_params[i]->value();
    return p;
}

void ControlsPanel::setParams(const SceneParams& params) {
    const bool was = m_loading;
    m_loading = true;
    const SceneParams sane = GeometryProvider::sanitise(m_scene, params);
    for (std::size_t i = 0; i < m_params.size() && i < SceneParams::kMax; ++i)
        m_params[i]->setValue(sane.v[i]);
    m_loading = was;
}

void ControlsPanel::setScene(GeometryProvider::Scene scene) {
    if (m_scene == scene && !m_params.empty()) return;
    m_scene = scene;
    rebuildParamRows();
}


SimConfig ControlsPanel::config() const {
    SimConfig cfg;
    cfg.scene            = m_scene;
    cfg.params           = params();
    cfg.useSceneDefaults = false;   // the boxes always hold a full parameter set

    // Everything about the emitters is left at its default here. They are
    // objects in the scene document now, and the window writes them in over
    // this: a source's angular law and flux are properties of that source, not
    // of the run, and there is exactly one place they are edited.
    cfg.fluxUnit     = FluxUnit(m_powerUnit->currentIndex());
    cfg.detectorBins = m_detBins->currentData().toInt();

    cfg.physics.fresnel    = m_fresnel->isChecked();
    cfg.physics.absorption = m_absorption->isChecked();
    cfg.physics.scattering = m_scattering->isChecked();
    cfg.physics.roughness  = m_roughness->isChecked();
    cfg.physics.dispersion = m_dispersion->isChecked();
    cfg.physics.coatings        = m_coatings->isChecked();
    cfg.physics.volumeScattering = m_volume->isChecked();
    cfg.physics.polarised       = m_polarised->isChecked();
    // The spin boxes park at their minimum to mean "leave each surface alone",
    // which is what the negative sentinel in PhysicsOptions expects.
    cfg.physics.roughnessOverride = m_roughOverride->value() < 0.0 ? -1.0 : m_roughOverride->value();
    cfg.physics.scatterOverride   = m_scatterOverride->value() < 0.0 ? -1.0 : m_scatterOverride->value();
    cfg.physics.absorptionScale   = m_absorptionScale->value();

    cfg.rays    = m_rays->value();
    cfg.seed    = std::uint64_t(m_seed->value());
    cfg.threads = unsigned(m_threads->value());
    return cfg;
}

void ControlsPanel::setConfig(const SimConfig& cfg) {
    m_loading = true;

    m_scene = cfg.scene;
    rebuildParamRows();
    const SceneParams p = cfg.effectiveParams();
    for (std::size_t i = 0; i < m_params.size(); ++i) m_params[i]->setValue(p.v[i]);

    m_powerUnit->setCurrentIndex(int(cfg.fluxUnit));
    {
        const int idx = m_detBins->findData(cfg.detectorBins > 0 ? cfg.detectorBins : 64);
        m_detBins->setCurrentIndex(idx >= 0 ? idx : 1);
    }
    syncEnabledState();

    m_fresnel->setChecked(cfg.physics.fresnel);
    m_absorption->setChecked(cfg.physics.absorption);
    m_scattering->setChecked(cfg.physics.scattering);
    m_roughness->setChecked(cfg.physics.roughness);
    m_dispersion->setChecked(cfg.physics.dispersion);
    m_coatings->setChecked(cfg.physics.coatings);
    m_volume->setChecked(cfg.physics.volumeScattering);
    m_polarised->setChecked(cfg.physics.polarised);
    m_roughOverride->setValue(cfg.physics.roughnessOverride < 0.0
                                  ? m_roughOverride->minimum() : cfg.physics.roughnessOverride);
    m_scatterOverride->setValue(cfg.physics.scatterOverride < 0.0
                                    ? m_scatterOverride->minimum() : cfg.physics.scatterOverride);
    m_absorptionScale->setValue(cfg.physics.absorptionScale);

    m_rays->setValue(cfg.rays);
    m_seed->setValue(int(cfg.seed & 0x7FFFFFFFull));
    m_threads->setValue(int(cfg.threads));

    m_loading = false;
    syncEnabledState();
}

void ControlsPanel::setGeometryDetached(const QString& reason) {
    m_detached = !reason.isEmpty();
    m_importNote->setVisible(m_detached);
    if (m_detached) {
        m_importNote->setText(reason);
        m_importNote->setToolTip(
            QStringLiteral("A run traces what the scene tree lists, not the tutorial "
                           "these dimensions describe. The physics, the ray budget and "
                           "every object's own properties all still apply."));
    }
    // The dimensions describe the tutorial, and the tutorial is not what will
    // be traced.
    m_paramBox->setEnabled(!m_detached);
}

void ControlsPanel::setRunning(bool running) {
    m_paramBox->setEnabled(!running && !m_detached);
    m_run->setEnabled(!running);
    m_cancel->setEnabled(running);
    for (QWidget* w : std::initializer_list<QWidget*>{m_coatings, m_volume, m_polarised,
                                                      m_powerUnit, m_detBins,
                                                      m_rays, m_seed, m_threads})
        w->setEnabled(!running);

    if (running) {
        // Busy (marquee) until the first progress tick arrives, which covers
        // the one-off geometry + mesh + BVH build of a scene.
        m_progress->setRange(0, 0);
        m_status->setText(QStringLiteral("Preparing geometry..."));
    } else {
        m_progress->setRange(0, 100);
        syncEnabledState();
    }
}

void ControlsPanel::setProgress(int percent) {
    if (m_progress->maximum() == 0) m_progress->setRange(0, 100);
    m_progress->setValue(percent);
    m_status->setText(QStringLiteral("Tracing... %1%").arg(percent));
}

void ControlsPanel::setStatus(const QString& text) { m_status->setText(text); }
