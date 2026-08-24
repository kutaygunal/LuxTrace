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
#include <QVBoxLayout>

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
    setFixedWidth(330);

    // The panel outgrew a fixed column once the physics and source blocks
    // arrived, so it scrolls rather than squeezing every row.
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

    auto* title = new QLabel(QStringLiteral("LuxTrace"), body);
    title->setStyleSheet(QStringLiteral("font-weight:bold; font-size:15px;"));
    auto* subtitle = new QLabel(QStringLiteral("Optical Design Studio"), body);
    subtitle->setStyleSheet(QStringLiteral("color:#8a8f9c; font-size:11px;"));
    layout->addWidget(title);
    layout->addWidget(subtitle);

    // ---- scene -------------------------------------------------------------
    m_scene = new QComboBox(body);
    // Driven by the scene registry, so a new scene appears here automatically.
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto& si = GeometryProvider::info(GeometryProvider::Scene(i));
        m_scene->addItem(si.name);
        m_scene->setItemData(i, si.description, Qt::ToolTipRole);
    }
    m_scene->setMaxVisibleItems(26);
    // Some scene names are long ("Compound Parabolic Concentrator"). Without
    // this the combo demands the full width of its longest entry and squeezes
    // the form's label column until the labels are elided to "Scen:".
    m_scene->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_scene->setMinimumContentsLength(16);

    auto* sceneForm = new QFormLayout;
    sceneForm->addRow(QStringLiteral("Scene:"), m_scene);
    layout->addLayout(sceneForm);

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

    // ---- source ------------------------------------------------------------
    auto* srcBox = group(QStringLiteral("Source"), body);
    auto* srcForm = new QFormLayout(srcBox);

    m_source = new QComboBox(srcBox);
    m_source->addItem(QStringLiteral("Point (uniform solid angle)"));
    m_source->addItem(QStringLiteral("Lambertian (cosine weighted)"));
    m_source->addItem(QStringLiteral("Collimated beam"));
    m_source->setItemData(0, QStringLiteral(
        "Uniform over solid angle inside the cone. At 180 degrees this is the full "
        "sphere, so light sent away from the optic is simply lost."), Qt::ToolTipRole);
    m_source->setItemData(1, QStringLiteral(
        "Cosine-weighted emission, as an LED die radiates. At 90 degrees it fills "
        "the hemisphere facing the optic."), Qt::ToolTipRole);
    m_source->setItemData(2, QStringLiteral(
        "A disc of parallel rays. This is what an imaging test wants, and it lets "
        "an axicon or a diffuser be fed directly instead of through a collimator."),
        Qt::ToolTipRole);
    m_source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_source->setMinimumContentsLength(14);
    m_source->setCurrentIndex(1);

    m_shape = new QComboBox(srcBox);
    m_shape->addItem(QStringLiteral("Point (zero area)"));
    m_shape->addItem(QStringLiteral("Disc"));
    m_shape->addItem(QStringLiteral("Rectangle"));
    m_shape->addItem(QStringLiteral("Sphere"));
    m_shape->setToolTip(QStringLiteral(
        "An emitter with real area has real etendue. A point source can be "
        "concentrated without limit, which is exactly why a concentrator fed by "
        "one reports a gain no real optic could reach."));
    m_shape->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_shape->setMinimumContentsLength(14);

    m_halfAngle  = dspin(srcBox, 0.5, 180.0, 5.0, 1, 180.0, QStringLiteral(" °"));
    m_halfAngle->setToolTip(QStringLiteral(
        "Emission cone half-angle. Clamped to 90 for a Lambertian source, where "
        "90 is the full hemisphere; 180 is the full sphere for a point source."));
    m_sizeA      = dspin(srcBox, 0.0, 200.0, 1.0, 2, 0.0, QStringLiteral(" mm"));
    m_sizeB      = dspin(srcBox, 0.0, 200.0, 1.0, 2, 0.0, QStringLiteral(" mm"));
    m_beamRadius = dspin(srcBox, 0.1, 300.0, 2.5, 2, 25.0, QStringLiteral(" mm"));

    m_spectrum = new QComboBox(srcBox);
    for (int i = 0; i < SpectrumConfig::kKindCount; ++i)
        m_spectrum->addItem(SpectrumConfig::kindName(SpectrumConfig::Kind(i)));
    m_spectrum->setItemData(int(SpectrumConfig::Kind::Rgb), QStringLiteral(
        "Three fixed lines at 620 / 546 / 460 nm, one ray in three. Fast, and "
        "enough to show a prism splitting a beam."), Qt::ToolTipRole);
    m_spectrum->setItemData(int(SpectrumConfig::Kind::Blackbody), QStringLiteral(
        "Planck's law at the colour temperature below. One wavelength is sampled "
        "per ray from it, so the spectrum resolves to whatever the ray budget "
        "supports -- at the price of a monochromatic trace."), Qt::ToolTipRole);
    m_spectrum->setItemData(int(SpectrumConfig::Kind::LedPhosphor), QStringLiteral(
        "A blue pump near 450 nm plus a phosphor hump, the shape every white LED "
        "has. This is what makes colour-over-angle mean anything."), Qt::ToolTipRole);
    m_spectrum->setItemData(int(SpectrumConfig::Kind::D65), QStringLiteral(
        "CIE standard daylight, the reference illuminant most colour work is "
        "specified against."), Qt::ToolTipRole);
    m_spectrum->setItemData(int(SpectrumConfig::Kind::Table), QStringLiteral(
        "A measured spectral power distribution loaded from a CSV of "
        "wavelength,power rows."), Qt::ToolTipRole);
    m_spectrum->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_spectrum->setMinimumContentsLength(14);

    m_wavelength = dspin(srcBox, 300.0, 1600.0, 10.0, 1, 587.6, QStringLiteral(" nm"));
    m_cct = dspin(srcBox, 1500.0, 12000.0, 250.0, 0, 5000.0, QStringLiteral(" K"));
    m_cct->setToolTip(QStringLiteral(
        "Colour temperature of the blackbody or the white LED. It sets the shape "
        "of the spectrum, and therefore the luminous efficacy the lumen figures "
        "are derived through."));

    // Absolute flux. Without it every number the app reports is a fraction of an
    // unnamed unit, which is not something an engineer can put in a spec.
    m_power = dspin(srcBox, 0.0, 1000000.0, 1.0, 3, 1.0, QString());
    m_power->setToolTip(QStringLiteral(
        "Total emitted flux. Everything downstream is reported in this unit: the "
        "receiver in W/m^2 or lux, the far field in W/sr or candela."));
    m_powerUnit = new QComboBox(srcBox);
    m_powerUnit->addItem(QStringLiteral("W (radiometric)"));
    m_powerUnit->addItem(QStringLiteral("lm (photometric)"));
    m_powerUnit->setItemData(1, QStringLiteral(
        "Lumens are watts weighted by the CIE V(lambda) curve, so a photometric "
        "run needs a real spectrum: each ray is emitted carrying how much of it "
        "the eye sees."), Qt::ToolTipRole);
    m_powerUnit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_powerUnit->setMinimumContentsLength(14);

    auto* powerRow = new QHBoxLayout;
    powerRow->setContentsMargins(0, 0, 0, 0);
    powerRow->addWidget(m_power, 1);
    powerRow->addWidget(m_powerUnit, 1);

    srcForm->addRow(QStringLiteral("Type:"), m_source);
    srcForm->addRow(QStringLiteral("Cone half-angle:"), m_halfAngle);
    srcForm->addRow(QStringLiteral("Beam radius:"), m_beamRadius);
    srcForm->addRow(QStringLiteral("Emitter:"), m_shape);
    srcForm->addRow(QStringLiteral("Size A:"), m_sizeA);
    srcForm->addRow(QStringLiteral("Size B:"), m_sizeB);
    srcForm->addRow(QStringLiteral("Total flux:"), powerRow);
    srcForm->addRow(QStringLiteral("Spectrum:"), m_spectrum);
    srcForm->addRow(QStringLiteral("Wavelength:"), m_wavelength);
    srcForm->addRow(QStringLiteral("Colour temp:"), m_cct);
    layout->addWidget(srcBox);

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
    m_polState = new QComboBox(physBox);
    m_polState->addItem(QStringLiteral("Unpolarised"));
    m_polState->addItem(QStringLiteral("Linear, s"));
    m_polState->addItem(QStringLiteral("Linear, p"));
    m_polState->addItem(QStringLiteral("Circular"));
    m_polState->setEnabled(false);
    m_polState->setToolTip(QStringLiteral(
        "What the source emits. s and p are measured against the plane of "
        "incidence of the first surface the ray meets."));
    m_polState->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

    physForm->addRow(QStringLiteral("Source state:"), m_polState);
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

    runForm->addRow(QStringLiteral("Rays:"), m_rays);
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

    connect(m_scene, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        rebuildParamRows();
        emit sceneChanged();
    });
    connect(m_resetParams, &QPushButton::clicked, this, [this] {
        rebuildParamRows();
        emit geometryChanged();
    });

    auto settings = [this] { if (!m_loading) { syncEnabledState(); emit settingsChanged(); } };
    connect(m_source,   &QComboBox::currentIndexChanged, this, settings);
    connect(m_shape,    &QComboBox::currentIndexChanged, this, settings);
    connect(m_spectrum, &QComboBox::currentIndexChanged, this, settings);
    connect(m_coatings, &QCheckBox::toggled, this, settings);
    connect(m_volume, &QCheckBox::toggled, this, settings);
    connect(m_polarised, &QCheckBox::toggled, this, settings);
    connect(m_polState, &QComboBox::currentIndexChanged, this, settings);
    connect(m_powerUnit, &QComboBox::currentIndexChanged, this, settings);
    connect(m_detBins, &QComboBox::currentIndexChanged, this, settings);
    connect(m_cct, &QDoubleSpinBox::valueChanged, this, settings);
    connect(m_power, &QDoubleSpinBox::valueChanged, this, settings);
    for (QDoubleSpinBox* s : {m_halfAngle, m_sizeA, m_sizeB, m_beamRadius, m_wavelength,
                              m_roughOverride, m_scatterOverride, m_absorptionScale})
        connect(s, &QDoubleSpinBox::valueChanged, this, settings);
    for (QCheckBox* c : {m_fresnel, m_absorption, m_scattering, m_roughness, m_dispersion})
        connect(c, &QCheckBox::toggled, this, settings);
    for (QSpinBox* s : {m_rays, m_seed, m_threads})
        connect(s, &QSpinBox::valueChanged, this, settings);

    rebuildParamRows();
    syncEnabledState();
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
}

void ControlsPanel::syncEnabledState() {
    const auto type  = SourceConfig::Type(m_source->currentIndex());
    const auto shape = SourceConfig::Shape(m_shape->currentIndex());
    const bool collimated = (type == SourceConfig::Type::Collimated);

    m_halfAngle->setEnabled(!collimated);
    m_beamRadius->setEnabled(collimated);
    m_sizeA->setEnabled(shape != SourceConfig::Shape::PointLike);
    m_sizeB->setEnabled(shape == SourceConfig::Shape::Rect);
    const auto kind = SpectrumConfig::Kind(m_spectrum->currentIndex());
    m_wavelength->setEnabled(kind == SpectrumConfig::Kind::Monochromatic);
    m_cct->setEnabled(kind == SpectrumConfig::Kind::Blackbody ||
                      kind == SpectrumConfig::Kind::LedPhosphor);
    // Lumens are watts through V(lambda); with a single line that is still a
    // well-defined conversion, so the unit is never disabled -- but a line
    // outside the visible band converts to nothing, and the readouts say so.
    m_power->setSuffix(m_powerUnit->currentIndex() == 1 ? QStringLiteral(" lm")
                                                        : QStringLiteral(" W"));
    // The emitted state means nothing unless the state is being carried.
    m_polState->setEnabled(m_polarised->isChecked());

    // The cone means different things to the two angular laws, and 180 on a
    // Lambertian source is silently the same as 90 -- showing that in the box
    // is less confusing than letting the user set a number that does nothing.
    if (type == SourceConfig::Type::Lambertian) m_halfAngle->setMaximum(90.0);
    else                                        m_halfAngle->setMaximum(180.0);
}

SceneParams ControlsPanel::currentParams() const {
    SceneParams p;
    for (std::size_t i = 0; i < m_params.size() && i < SceneParams::kMax; ++i)
        p.v[i] = m_params[i]->value();
    return p;
}

GeometryProvider::Scene ControlsPanel::scene() const {
    return GeometryProvider::Scene(m_scene->currentIndex());
}

SimConfig ControlsPanel::config() const {
    SimConfig cfg;
    cfg.scene            = scene();
    cfg.params           = currentParams();
    cfg.useSceneDefaults = false;   // the boxes always hold a full parameter set

    cfg.source       = SourceConfig::Type(m_source->currentIndex());
    cfg.shape        = SourceConfig::Shape(m_shape->currentIndex());
    cfg.spectrum.kind         = SpectrumConfig::Kind(m_spectrum->currentIndex());
    cfg.spectrum.wavelengthNm = m_wavelength->value();
    cfg.spectrum.cct          = m_cct->value();
    cfg.halfAngleDeg = m_halfAngle->value();
    cfg.sizeA        = m_sizeA->value();
    cfg.sizeB        = m_sizeB->value();
    cfg.beamRadius   = m_beamRadius->value();
    cfg.power        = m_power->value();
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
    cfg.polarisationState       = m_polState->currentIndex();
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

    m_scene->setCurrentIndex(int(cfg.scene));
    rebuildParamRows();
    const SceneParams p = cfg.effectiveParams();
    for (std::size_t i = 0; i < m_params.size(); ++i) m_params[i]->setValue(p.v[i]);

    m_source->setCurrentIndex(int(cfg.source));
    m_shape->setCurrentIndex(int(cfg.shape));
    m_spectrum->setCurrentIndex(int(cfg.spectrum.kind));
    m_powerUnit->setCurrentIndex(int(cfg.fluxUnit));
    {
        const int idx = m_detBins->findData(cfg.detectorBins > 0 ? cfg.detectorBins : 64);
        m_detBins->setCurrentIndex(idx >= 0 ? idx : 1);
    }
    // Set the maximum before the value, or a 180-degree cone loaded onto a
    // Lambertian source would be clipped to the old limit instead of to 90.
    syncEnabledState();
    m_halfAngle->setValue(cfg.halfAngleDeg);
    m_sizeA->setValue(cfg.sizeA);
    m_sizeB->setValue(cfg.sizeB);
    m_beamRadius->setValue(cfg.beamRadius);
    m_wavelength->setValue(cfg.spectrum.wavelengthNm);
    m_cct->setValue(cfg.spectrum.cct);
    m_power->setValue(cfg.power);

    m_fresnel->setChecked(cfg.physics.fresnel);
    m_absorption->setChecked(cfg.physics.absorption);
    m_scattering->setChecked(cfg.physics.scattering);
    m_roughness->setChecked(cfg.physics.roughness);
    m_dispersion->setChecked(cfg.physics.dispersion);
    m_coatings->setChecked(cfg.physics.coatings);
    m_volume->setChecked(cfg.physics.volumeScattering);
    m_polarised->setChecked(cfg.physics.polarised);
    m_polState->setCurrentIndex(std::clamp(cfg.polarisationState, 0, 3));
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
    emit sceneChanged();
}

void ControlsPanel::setImportedGeometry(const QString& label) {
    m_importedGeometry = !label.isEmpty();
    m_importNote->setVisible(m_importedGeometry);
    if (m_importedGeometry) {
        m_importNote->setText(
            QStringLiteral("Tracing imported geometry: %1.\n"
                           "The scene above and the dimensions below are not in use — "
                           "picking a scene replaces the imported part.").arg(label));
        m_importNote->setToolTip(
            QStringLiteral("A run traces the file, not the selected scene. Surface "
                           "optics, the source, the physics and the ray budget all "
                           "still apply to it."));
    }
    // The parameters describe the scene, and the scene is not what will be
    // traced.
    m_paramBox->setEnabled(!m_importedGeometry);
}

void ControlsPanel::setRunning(bool running) {
    m_scene->setEnabled(!running);
    m_paramBox->setEnabled(!running && !m_importedGeometry);
    m_run->setEnabled(!running);
    m_cancel->setEnabled(running);
    for (QWidget* w : std::initializer_list<QWidget*>{m_coatings, m_volume, m_polarised,
                                                     m_polState,
                                                     m_cct, m_power, m_powerUnit, m_detBins,
                                                     m_source, m_shape, m_spectrum,
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
