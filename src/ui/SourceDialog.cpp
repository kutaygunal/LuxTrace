#include "SourceDialog.h"
#include "core/RayFile.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QDoubleSpinBox* dspin(QWidget* p, double lo, double hi, double step, int decimals,
                      double value, const QString& suffix) {
    auto* s = new QDoubleSpinBox(p);
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setValue(value);
    if (!suffix.isEmpty()) s->setSuffix(suffix);
    return s;
}

} // namespace

SourceDialog::SourceDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Source"));
    auto* outer = new QVBoxLayout(this);

    // ---- what it emits -----------------------------------------------------
    auto* emitBox  = new QGroupBox(QStringLiteral("Emission"), this);
    auto* emitForm = new QFormLayout(emitBox);

    m_label = new QLineEdit(emitBox);
    m_label->setPlaceholderText(QStringLiteral("what to call it in the report"));

    m_type = new QComboBox(emitBox);
    m_type->addItem(QStringLiteral("Point (uniform in solid angle)"));
    m_type->addItem(QStringLiteral("Lambertian (cosine weighted)"));
    m_type->addItem(QStringLiteral("Collimated"));

    m_shape = new QComboBox(emitBox);
    m_shape->addItem(QStringLiteral("Point-like"));
    m_shape->addItem(QStringLiteral("Disc"));
    m_shape->addItem(QStringLiteral("Rectangle"));
    m_shape->addItem(QStringLiteral("Sphere"));

    m_halfAngle  = dspin(emitBox, 0.0, 180.0, 5.0, 1, 90.0, QStringLiteral(" deg"));
    m_sizeA      = dspin(emitBox, 0.0, 1000.0, 0.1, 3, 0.0, QStringLiteral(" mm"));
    m_sizeB      = dspin(emitBox, 0.0, 1000.0, 0.1, 3, 0.0, QStringLiteral(" mm"));
    m_beamRadius = dspin(emitBox, 0.0, 1000.0, 1.0, 2, 25.0, QStringLiteral(" mm"));

    m_power = dspin(emitBox, 0.0, 1000000.0, 1.0, 3, 1.0, QString());
    m_power->setToolTip(QStringLiteral(
        "Emitted flux, in whatever unit the run reports in. The ray budget is "
        "shared between sources in proportion to this, so a source carrying a "
        "tenth of the light gets a tenth of the rays and every source ends the "
        "run with a comparable error bar."));

    m_spectrum = new QComboBox(emitBox);
    for (int i = 0; i < SpectrumConfig::kKindCount; ++i)
        m_spectrum->addItem(SpectrumConfig::kindName(SpectrumConfig::Kind(i)));
    m_wavelength = dspin(emitBox, 200.0, 2000.0, 10.0, 1, 587.6, QStringLiteral(" nm"));
    m_cct        = dspin(emitBox, 1000.0, 20000.0, 100.0, 0, 5000.0, QStringLiteral(" K"));

    m_polState = new QComboBox(emitBox);
    m_polState->addItem(QStringLiteral("Unpolarised"));
    m_polState->addItem(QStringLiteral("Linear, s"));
    m_polState->addItem(QStringLiteral("Linear, p"));
    m_polState->addItem(QStringLiteral("Circular"));

    emitForm->addRow(QStringLiteral("Name:"), m_label);
    emitForm->addRow(QStringLiteral("Type:"), m_type);
    emitForm->addRow(QStringLiteral("Cone half-angle:"), m_halfAngle);
    emitForm->addRow(QStringLiteral("Beam radius:"), m_beamRadius);
    emitForm->addRow(QStringLiteral("Emitter:"), m_shape);
    emitForm->addRow(QStringLiteral("Size A:"), m_sizeA);
    emitForm->addRow(QStringLiteral("Size B:"), m_sizeB);
    emitForm->addRow(QStringLiteral("Flux:"), m_power);
    emitForm->addRow(QStringLiteral("Spectrum:"), m_spectrum);
    emitForm->addRow(QStringLiteral("Wavelength:"), m_wavelength);
    emitForm->addRow(QStringLiteral("Colour temperature:"), m_cct);
    emitForm->addRow(QStringLiteral("Polarisation:"), m_polState);
    outer->addWidget(emitBox);

    // ---- where it sits -----------------------------------------------------
    auto* placeBox  = new QGroupBox(QStringLiteral("Placement"), this);
    auto* placeForm = new QFormLayout(placeBox);

    m_absolute = new QCheckBox(QStringLiteral("Absolute world position"), placeBox);
    m_absolute->setToolTip(QStringLiteral(
        "Off, the position below is an offset from wherever the scene puts its "
        "own emitter -- so a second LED beside the first stays beside it when "
        "the optic changes. On, it is a world coordinate."));

    m_ox = dspin(placeBox, -100000.0, 100000.0, 1.0, 3, 0.0, QStringLiteral(" mm"));
    m_oy = dspin(placeBox, -100000.0, 100000.0, 1.0, 3, 0.0, QStringLiteral(" mm"));
    m_oz = dspin(placeBox, -100000.0, 100000.0, 1.0, 3, 0.0, QStringLiteral(" mm"));
    auto* offRow = new QHBoxLayout;
    offRow->setContentsMargins(0, 0, 0, 0);
    offRow->addWidget(m_ox, 1);
    offRow->addWidget(m_oy, 1);
    offRow->addWidget(m_oz, 1);

    m_sceneAxis = new QCheckBox(QStringLiteral("Aim the way the scene aims"), placeBox);
    m_sceneAxis->setChecked(true);
    m_ax = dspin(placeBox, -1.0, 1.0, 0.1, 3, 0.0, QString());
    m_ay = dspin(placeBox, -1.0, 1.0, 0.1, 3, 0.0, QString());
    m_az = dspin(placeBox, -1.0, 1.0, 0.1, 3, 1.0, QString());
    auto* axRow = new QHBoxLayout;
    axRow->setContentsMargins(0, 0, 0, 0);
    axRow->addWidget(m_ax, 1);
    axRow->addWidget(m_ay, 1);
    axRow->addWidget(m_az, 1);

    placeForm->addRow(m_absolute);
    placeForm->addRow(QStringLiteral("Position x, y, z:"), offRow);
    placeForm->addRow(m_sceneAxis);
    placeForm->addRow(QStringLiteral("Axis x, y, z:"), axRow);
    outer->addWidget(placeBox);

    // ---- a measured ray set ------------------------------------------------
    auto* rayBox  = new QGroupBox(QStringLiteral("Measured ray file"), this);
    auto* rayForm = new QFormLayout(rayBox);

    m_rayNote = new QLabel(rayBox);
    m_rayNote->setWordWrap(true);

    auto* pick  = new QPushButton(QStringLiteral("Load ray file..."), rayBox);
    auto* clear = new QPushButton(QStringLiteral("Clear"), rayBox);
    auto* rayRow = new QHBoxLayout;
    rayRow->setContentsMargins(0, 0, 0, 0);
    rayRow->addWidget(pick, 1);
    rayRow->addWidget(clear, 0);

    m_rayScale  = dspin(rayBox, 0.001, 1000.0, 0.1, 4, 1.0, QString());
    m_rayScale->setToolTip(QStringLiteral(
        "Extra scale on the ray positions, on top of whatever the file's own "
        "dimension code already applied. 1 is the ordinary case."));
    m_rayLambda = new QCheckBox(QStringLiteral("Use the file's wavelengths"), rayBox);
    m_rayLambda->setChecked(true);
    m_rayLambda->setToolTip(QStringLiteral(
        "Off pins the whole set to the spectrum above, which is what comparing "
        "a measured set against an idealised one needs."));

    rayForm->addRow(rayRow);
    rayForm->addRow(m_rayNote);
    rayForm->addRow(QStringLiteral("Position scale:"), m_rayScale);
    rayForm->addRow(m_rayLambda);
    outer->addWidget(rayBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    outer->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(pick,  &QPushButton::clicked, this, &SourceDialog::chooseRayFile);
    connect(clear, &QPushButton::clicked, this, &SourceDialog::clearRayFile);
    connect(m_type,      &QComboBox::currentIndexChanged, this, [this] { syncEnabledState(); });
    connect(m_shape,     &QComboBox::currentIndexChanged, this, [this] { syncEnabledState(); });
    connect(m_spectrum,  &QComboBox::currentIndexChanged, this, [this] { syncEnabledState(); });
    connect(m_absolute,  &QCheckBox::toggled,             this, [this] { syncEnabledState(); });
    connect(m_sceneAxis, &QCheckBox::toggled,             this, [this] { syncEnabledState(); });

    refreshRayFileLabel();
    syncEnabledState();
}

void SourceDialog::syncEnabledState() {
    const auto type  = SourceConfig::Type(m_type->currentIndex());
    const auto shape = SourceConfig::Shape(m_shape->currentIndex());
    const bool collimated = (type == SourceConfig::Type::Collimated);
    // A measured set carries every ray's origin, direction and flux already, so
    // the analytic description of the emitter is not consulted at all. Greying
    // it out is the honest statement of that, rather than leaving live boxes
    // that change nothing.
    const bool measured = (m_rayFile != nullptr);

    m_type->setEnabled(!measured);
    m_shape->setEnabled(!measured);
    m_halfAngle->setEnabled(!measured && !collimated);
    m_beamRadius->setEnabled(!measured && collimated);
    m_sizeA->setEnabled(!measured && shape != SourceConfig::Shape::PointLike);
    m_sizeB->setEnabled(!measured && shape == SourceConfig::Shape::Rect);
    if (type == SourceConfig::Type::Lambertian) m_halfAngle->setMaximum(90.0);
    else                                        m_halfAngle->setMaximum(180.0);

    const auto kind = SpectrumConfig::Kind(m_spectrum->currentIndex());
    m_wavelength->setEnabled(kind == SpectrumConfig::Kind::Monochromatic);
    m_cct->setEnabled(kind == SpectrumConfig::Kind::Blackbody ||
                      kind == SpectrumConfig::Kind::LedPhosphor);

    const bool axis = !m_sceneAxis->isChecked();
    m_ax->setEnabled(axis);
    m_ay->setEnabled(axis);
    m_az->setEnabled(axis);

    m_rayScale->setEnabled(measured);
    m_rayLambda->setEnabled(measured);
}

void SourceDialog::chooseRayFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open a measured source ray file"), QString(),
        rayfile::fileFilter());
    if (path.isEmpty()) return;

    const auto res = rayfile::load(path);
    if (!res.ok) {
        QMessageBox::warning(this, QStringLiteral("Ray file"), res.error);
        return;
    }
    m_rayFile = res.data;
    if (m_label->text().trimmed().isEmpty()) m_label->setText(m_rayFile->label);
    refreshRayFileLabel();
    syncEnabledState();
}

void SourceDialog::clearRayFile() {
    m_rayFile.reset();
    refreshRayFileLabel();
    syncEnabledState();
}

void SourceDialog::refreshRayFileLabel() {
    if (!m_rayFile) {
        m_rayNote->setText(QStringLiteral(
            "No ray file. The emitter above is used.\n"
            "A vendor ray set replaces it entirely: it carries every ray's "
            "position on the die, its direction and its flux, which is the "
            "difference between modelling an LED and using one."));
        return;
    }
    m_rayNote->setText(QStringLiteral("%1\n%2\n%3")
                           .arg(m_rayFile->label, m_rayFile->format,
                                m_rayFile->summary()));
}

void SourceDialog::setSpec(const SourceSpec& s) {
    m_label->setText(s.label);
    m_type->setCurrentIndex(std::clamp(int(s.type), 0, 2));
    m_shape->setCurrentIndex(std::clamp(int(s.shape), 0, 3));
    m_spectrum->setCurrentIndex(std::clamp(int(s.spectrum.kind), 0,
                                           SpectrumConfig::kKindCount - 1));
    // The maximum before the value, or a 180-degree cone on a Lambertian source
    // would be clipped to the old limit rather than to 90.
    syncEnabledState();
    m_halfAngle->setValue(s.halfAngleDeg);
    m_sizeA->setValue(s.sizeA);
    m_sizeB->setValue(s.sizeB);
    m_beamRadius->setValue(s.beamRadius);
    m_power->setValue(s.power);
    m_wavelength->setValue(s.spectrum.wavelengthNm);
    m_cct->setValue(s.spectrum.cct);
    m_polState->setCurrentIndex(std::clamp(s.polarisationState, 0, 3));

    m_absolute->setChecked(s.absolute);
    m_ox->setValue(s.offset.X());
    m_oy->setValue(s.offset.Y());
    m_oz->setValue(s.offset.Z());
    m_sceneAxis->setChecked(s.useSceneAxis);
    m_ax->setValue(s.axis.X());
    m_ay->setValue(s.axis.Y());
    m_az->setValue(s.axis.Z());

    m_rayFile = s.rayFile;
    m_rayScale->setValue(s.rayFileScale);
    m_rayLambda->setChecked(s.rayFileWavelengths);
    refreshRayFileLabel();
    syncEnabledState();
}

SourceSpec SourceDialog::spec() const {
    SourceSpec s;
    s.label        = m_label->text().trimmed();
    s.type         = SourceConfig::Type(m_type->currentIndex());
    s.shape        = SourceConfig::Shape(m_shape->currentIndex());
    s.spectrum.kind         = SpectrumConfig::Kind(m_spectrum->currentIndex());
    s.spectrum.wavelengthNm = m_wavelength->value();
    s.spectrum.cct          = m_cct->value();
    s.halfAngleDeg = m_halfAngle->value();
    s.sizeA        = m_sizeA->value();
    s.sizeB        = m_sizeB->value();
    s.beamRadius   = m_beamRadius->value();
    s.power        = m_power->value();
    s.polarisationState = m_polState->currentIndex();

    s.absolute = m_absolute->isChecked();
    s.offset   = gp_Pnt(m_ox->value(), m_oy->value(), m_oz->value());
    s.useSceneAxis = m_sceneAxis->isChecked();
    // A degenerate axis would leave the emission basis undefined, so a typed-in
    // zero vector falls back to +Z rather than producing NaN directions.
    const gp_Vec v(m_ax->value(), m_ay->value(), m_az->value());
    s.axis = (v.Magnitude() > 1e-9) ? gp_Dir(v) : gp_Dir(0, 0, 1);

    s.rayFile            = m_rayFile;
    s.rayFileScale       = m_rayScale->value();
    s.rayFileWavelengths = m_rayLambda->isChecked();
    return s;
}
