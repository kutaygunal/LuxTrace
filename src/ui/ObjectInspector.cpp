#include "ObjectInspector.h"
#include "SurfaceInspector.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "core/RayFile.h"

using scenedoc::ObjectType;
using scenedoc::SceneObject;

namespace {

QDoubleSpinBox* coord(QWidget* parent, const QString& suffix, double step = 1.0) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(-1e6, 1e6);
    s->setDecimals(2);
    s->setSingleStep(step);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    return s;
}

} // namespace

ObjectInspector::ObjectInspector(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    m_empty = new QLabel(QStringLiteral("<i>Select an object to edit it.</i>"), this);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignTop);
    outer->addWidget(m_empty);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(m_scroll, 1);

    m_body    = new QWidget(m_scroll);
    auto* v   = new QVBoxLayout(m_body);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(6);
    m_scroll->setWidget(m_body);

    // ---- identity ----------------------------------------------------------
    m_title = new QLabel(m_body);
    QFont tf = m_title->font();
    tf.setBold(true);
    m_title->setFont(tf);
    m_title->setWordWrap(true);
    v->addWidget(m_title);

    m_kind = new QLabel(m_body);
    m_kind->setWordWrap(true);
    m_kind->setStyleSheet(QStringLiteral("color:#9aa0b0; font-size:11px;"));
    v->addWidget(m_kind);

    auto* nameForm = new QFormLayout;
    m_name = new QLineEdit(m_body);
    nameForm->addRow(QStringLiteral("Name"), m_name);
    v->addLayout(nameForm);
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        if (m_loading || !m_haveObject) return;
        const QString text = m_name->text().trimmed();
        if (text.isEmpty() || text == m_object.name) return;
        m_object.name = text;
        emit nameEdited(m_object.id);
    });

    // ---- placement ---------------------------------------------------------
    m_placeBox   = new QGroupBox(QStringLiteral("Placement"), m_body);
    auto* pf     = new QFormLayout(m_placeBox);
    auto* posRow = new QHBoxLayout;
    m_px = coord(m_placeBox, QStringLiteral(" mm"));
    m_py = coord(m_placeBox, QStringLiteral(" mm"));
    m_pz = coord(m_placeBox, QStringLiteral(" mm"));
    posRow->addWidget(m_px);
    posRow->addWidget(m_py);
    posRow->addWidget(m_pz);
    pf->addRow(QStringLiteral("Position X / Y / Z"), posRow);

    auto* rotRow = new QHBoxLayout;
    m_rx = coord(m_placeBox, QStringLiteral(" deg"), 5.0);
    m_ry = coord(m_placeBox, QStringLiteral(" deg"), 5.0);
    m_rz = coord(m_placeBox, QStringLiteral(" deg"), 5.0);
    for (QDoubleSpinBox* s : {m_rx, m_ry, m_rz}) s->setRange(-360.0, 360.0);
    rotRow->addWidget(m_rx);
    rotRow->addWidget(m_ry);
    rotRow->addWidget(m_rz);
    pf->addRow(QStringLiteral("Rotation X / Y / Z"), rotRow);
    m_placeBox->setToolTip(QStringLiteral(
        "Where the object sits, relative to its group. Rotations are applied "
        "X, then Y, then Z."));
    v->addWidget(m_placeBox);

    for (QDoubleSpinBox* s : {m_px, m_py, m_pz, m_rx, m_ry, m_rz})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            if (m_loading || !m_haveObject) return;
            m_object.position       = gp_Pnt(m_px->value(), m_py->value(), m_pz->value());
            m_object.rotationDeg[0] = m_rx->value();
            m_object.rotationDeg[1] = m_ry->value();
            m_object.rotationDeg[2] = m_rz->value();
            emit placementEdited(m_object.id);
        });

    // ---- geometry ----------------------------------------------------------
    m_geomBox  = new QGroupBox(QStringLiteral("Geometry"), m_body);
    m_geomForm = new QFormLayout(m_geomBox);
    m_bakedNote = new QLabel(m_geomBox);
    m_bakedNote->setWordWrap(true);
    m_bakedNote->setStyleSheet(QStringLiteral("color:#9aa0b0; font-size:11px;"));
    m_bakedNote->setVisible(false);
    m_geomForm->addRow(m_bakedNote);
    v->addWidget(m_geomBox);

    // ---- source ------------------------------------------------------------
    m_sourceBox = new QGroupBox(QStringLiteral("Emission"), m_body);
    auto* sf    = new QFormLayout(m_sourceBox);

    m_srcType = new QComboBox(m_sourceBox);
    m_srcType->addItems({QStringLiteral("Point (uniform in solid angle)"),
                         QStringLiteral("Lambertian (cosine)"),
                         QStringLiteral("Collimated")});
    sf->addRow(QStringLiteral("Angular law"), m_srcType);

    m_srcShape = new QComboBox(m_sourceBox);
    m_srcShape->addItems({QStringLiteral("Point-like"), QStringLiteral("Disc"),
                          QStringLiteral("Rectangle"), QStringLiteral("Sphere")});
    m_srcShape->setToolTip(QStringLiteral(
        "A real emitting area gives the source etendue, which is what stops a "
        "concentrator reporting an impossible gain."));
    sf->addRow(QStringLiteral("Emitter shape"), m_srcShape);

    m_halfAngle = new QDoubleSpinBox(m_sourceBox);
    m_halfAngle->setRange(0.0, 180.0);
    m_halfAngle->setSuffix(QStringLiteral(" deg"));
    m_halfAngle->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Cone half-angle"), m_halfAngle);

    m_sizeA = new QDoubleSpinBox(m_sourceBox);
    m_sizeA->setRange(0.0, 10000.0);
    m_sizeA->setSuffix(QStringLiteral(" mm"));
    m_sizeA->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Size A (radius / width)"), m_sizeA);

    m_sizeB = new QDoubleSpinBox(m_sourceBox);
    m_sizeB->setRange(0.0, 10000.0);
    m_sizeB->setSuffix(QStringLiteral(" mm"));
    m_sizeB->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Size B (height)"), m_sizeB);

    m_beamRadius = new QDoubleSpinBox(m_sourceBox);
    m_beamRadius->setRange(0.0, 10000.0);
    m_beamRadius->setSuffix(QStringLiteral(" mm"));
    m_beamRadius->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Beam radius"), m_beamRadius);

    m_spectrum = new QComboBox(m_sourceBox);
    for (int i = 0; i < SpectrumConfig::kKindCount; ++i)
        m_spectrum->addItem(SpectrumConfig::kindName(SpectrumConfig::Kind(i)));
    sf->addRow(QStringLiteral("Spectrum"), m_spectrum);

    m_wavelength = new QDoubleSpinBox(m_sourceBox);
    m_wavelength->setRange(200.0, 2000.0);
    m_wavelength->setSuffix(QStringLiteral(" nm"));
    m_wavelength->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Wavelength"), m_wavelength);

    m_cct = new QDoubleSpinBox(m_sourceBox);
    m_cct->setRange(1000.0, 20000.0);
    m_cct->setSingleStep(100.0);
    m_cct->setSuffix(QStringLiteral(" K"));
    m_cct->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Colour temperature"), m_cct);

    m_power = new QDoubleSpinBox(m_sourceBox);
    m_power->setRange(0.0, 1e9);
    m_power->setDecimals(3);
    m_power->setKeyboardTracking(false);
    m_power->setToolTip(QStringLiteral(
        "Emitted flux, in the unit the run is quoted in. Ray budget is shared "
        "between sources in proportion to this."));
    sf->addRow(QStringLiteral("Flux"), m_power);

    m_polState = new QComboBox(m_sourceBox);
    m_polState->addItems({QStringLiteral("Unpolarised"), QStringLiteral("Linear s"),
                          QStringLiteral("Linear p"), QStringLiteral("Circular")});
    sf->addRow(QStringLiteral("Polarisation"), m_polState);

    m_rayNote = new QLabel(QStringLiteral("No measured ray set."), m_sourceBox);
    m_rayNote->setWordWrap(true);
    m_rayNote->setStyleSheet(QStringLiteral("color:#9aa0b0; font-size:11px;"));
    sf->addRow(m_rayNote);

    auto* rayRow = new QHBoxLayout;
    m_rayChoose  = new QPushButton(QStringLiteral("Ray set..."), m_sourceBox);
    m_rayClear   = new QPushButton(QStringLiteral("Clear"), m_sourceBox);
    m_rayChoose->setAutoDefault(false);
    m_rayClear->setAutoDefault(false);
    m_rayChoose->setToolTip(QStringLiteral(
        "A vendor's measured ray set -- Zemax .dat, ASAP .dis or TracePro text. "
        "With one loaded the angular law, the emitter shape and the sizes above "
        "are not consulted: the file already carries every ray."));
    rayRow->addWidget(m_rayChoose);
    rayRow->addWidget(m_rayClear);
    sf->addRow(rayRow);

    m_rayScale = new QDoubleSpinBox(m_sourceBox);
    m_rayScale->setRange(1e-6, 1e6);
    m_rayScale->setDecimals(4);
    m_rayScale->setValue(1.0);
    m_rayScale->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Ray set scale"), m_rayScale);

    m_rayLambda = new QCheckBox(QStringLiteral("Use the file's wavelengths"), m_sourceBox);
    sf->addRow(m_rayLambda);

    v->addWidget(m_sourceBox);

    auto emitSource = [this] {
        if (m_loading || !m_haveObject) return;
        readSourceForm();
        syncSourceEnabledState();
        emit sourceEdited(m_object.id);
    };
    connect(m_srcType,   &QComboBox::currentIndexChanged,  this, emitSource);
    connect(m_srcShape,  &QComboBox::currentIndexChanged,  this, emitSource);
    connect(m_spectrum,  &QComboBox::currentIndexChanged,  this, emitSource);
    connect(m_polState,  &QComboBox::currentIndexChanged,  this, emitSource);
    connect(m_rayLambda, &QCheckBox::toggled,              this, emitSource);
    for (QDoubleSpinBox* s : {m_halfAngle, m_sizeA, m_sizeB, m_beamRadius,
                              m_wavelength, m_cct, m_power, m_rayScale})
        connect(s, &QDoubleSpinBox::valueChanged, this, emitSource);

    connect(m_rayChoose, &QPushButton::clicked, this, &ObjectInspector::chooseRayFile);
    connect(m_rayClear,  &QPushButton::clicked, this, &ObjectInspector::clearRayFile);

    // ---- optics ------------------------------------------------------------
    m_optics = new SurfaceInspector(m_body);
    v->addWidget(m_optics);
    connect(m_optics, &SurfaceInspector::edited, this, [this] {
        if (m_loading || !m_haveObject) return;
        m_object.optics = m_optics->optics();
        emit opticsEdited(m_object.id);
    });
    connect(m_optics, &SurfaceInspector::resetRequested, this, [this] {
        if (!m_haveObject) return;
        emit opticsResetRequested(m_object.id);
    });

    v->addStretch(1);
    clearObject();
}

void ObjectInspector::rebuildParamRows() {
    // The rows belong to the type, so they are torn down and rebuilt whenever
    // the selection changes to a different one.
    for (QDoubleSpinBox* s : m_params) {
        m_geomForm->removeRow(s);   // takes the label with it
    }
    m_params.clear();

    const std::vector<SceneParamInfo>& info = scenedoc::typeInfo(m_object.type).params;
    for (std::size_t i = 0; i < info.size() && i < SceneObject::kMaxParams; ++i) {
        const SceneParamInfo& p = info[i];
        auto* s = new QDoubleSpinBox(m_geomBox);
        s->setRange(p.min, p.max);
        s->setDecimals(p.decimals);
        s->setSingleStep(p.step);
        s->setValue(m_object.p[i]);
        s->setKeyboardTracking(false);
        if (!p.unit.isEmpty()) s->setSuffix(QStringLiteral(" ") + p.unit);
        if (!p.tip.isEmpty())  s->setToolTip(p.tip);
        m_geomForm->addRow(p.name, s);
        m_params.push_back(s);

        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            if (m_loading || !m_haveObject) return;
            for (std::size_t k = 0; k < m_params.size(); ++k)
                m_object.p[k] = m_params[k]->value();
            emit parametersEdited(m_object.id);
        });
    }
}

void ObjectInspector::setObject(const SceneObject& object, const SurfaceOptics& defaultOptics) {
    m_loading    = true;
    m_object     = object;
    m_haveObject = true;

    const scenedoc::TypeInfo& info = scenedoc::typeInfo(object.type);
    m_title->setText(object.name);
    m_kind->setText(info.description);
    m_name->setText(object.name);

    m_px->setValue(object.position.X());
    m_py->setValue(object.position.Y());
    m_pz->setValue(object.position.Z());
    m_rx->setValue(object.rotationDeg[0]);
    m_ry->setValue(object.rotationDeg[1]);
    m_rz->setValue(object.rotationDeg[2]);

    rebuildParamRows();
    const bool baked = object.type == ObjectType::TutorialPart;
    m_bakedNote->setVisible(baked);
    if (baked)
        m_bakedNote->setText(QStringLiteral(
            "Built by the tutorial. Its dimensions come from the scene parameters "
            "on the right; everything else about it is editable here."));
    m_geomBox->setVisible(!info.params.empty() || baked);

    // ---- source ----
    const bool isSource = object.isSource();
    m_sourceBox->setVisible(isSource);
    if (isSource) {
        const SourceSpec& s = object.source;
        m_srcType->setCurrentIndex(int(s.type));
        m_srcShape->setCurrentIndex(int(s.shape));
        m_halfAngle->setValue(s.halfAngleDeg);
        m_sizeA->setValue(s.sizeA);
        m_sizeB->setValue(s.sizeB);
        m_beamRadius->setValue(s.beamRadius);
        m_spectrum->setCurrentIndex(int(s.spectrum.kind));
        m_wavelength->setValue(s.spectrum.wavelengthNm);
        m_cct->setValue(s.spectrum.cct);
        m_power->setValue(s.power);
        m_polState->setCurrentIndex(std::clamp(s.polarisationState, 0, 3));
        m_rayScale->setValue(s.rayFileScale);
        m_rayLambda->setChecked(s.rayFileWavelengths);
        refreshRayFileLabel();
        syncSourceEnabledState();
    }

    // ---- optics ----
    const bool hasOptics = !object.isGroup() && !isSource;
    m_optics->setVisible(hasOptics);
    if (hasOptics) m_optics->setSurface(object.name, object.optics, defaultOptics, false);
    else           m_optics->clearSurface();

    m_empty->setVisible(false);
    m_scroll->setVisible(true);
    m_loading = false;
}

void ObjectInspector::clearObject() {
    m_haveObject = false;
    m_object     = SceneObject{};
    m_optics->clearSurface();
    m_empty->setVisible(true);
    m_scroll->setVisible(false);
}

void ObjectInspector::readSourceForm() {
    SourceSpec& s = m_object.source;
    s.type                 = SourceConfig::Type(m_srcType->currentIndex());
    s.shape                = SourceConfig::Shape(m_srcShape->currentIndex());
    s.halfAngleDeg         = m_halfAngle->value();
    s.sizeA                = m_sizeA->value();
    s.sizeB                = m_sizeB->value();
    s.beamRadius           = m_beamRadius->value();
    s.spectrum.kind        = SpectrumConfig::Kind(m_spectrum->currentIndex());
    s.spectrum.wavelengthNm = m_wavelength->value();
    s.spectrum.cct         = m_cct->value();
    s.power                = m_power->value();
    s.polarisationState    = m_polState->currentIndex();
    s.rayFileScale         = m_rayScale->value();
    s.rayFileWavelengths   = m_rayLambda->isChecked();
}

void ObjectInspector::syncSourceEnabledState() {
    const auto type    = SourceConfig::Type(m_srcType->currentIndex());
    const auto shape   = SourceConfig::Shape(m_srcShape->currentIndex());
    const auto kind    = SpectrumConfig::Kind(m_spectrum->currentIndex());
    // A measured set carries every ray already, so the analytic emitter's own
    // controls are not consulted at all. Naming them inactive is the difference
    // between a form that lies and one that does not.
    const bool measured = m_object.source.tracesRayFile();

    m_srcType->setEnabled(!measured);
    m_srcShape->setEnabled(!measured);
    m_halfAngle->setEnabled(!measured && type != SourceConfig::Type::Collimated);
    m_sizeA->setEnabled(!measured && shape != SourceConfig::Shape::PointLike);
    m_sizeB->setEnabled(!measured && shape == SourceConfig::Shape::Rect);
    m_beamRadius->setEnabled(!measured && type == SourceConfig::Type::Collimated);
    m_wavelength->setEnabled(kind == SpectrumConfig::Kind::Monochromatic);
    m_cct->setEnabled(kind == SpectrumConfig::Kind::Blackbody ||
                      kind == SpectrumConfig::Kind::LedPhosphor);
    m_rayClear->setEnabled(measured);
    m_rayScale->setEnabled(measured);
    m_rayLambda->setEnabled(measured);
}

void ObjectInspector::refreshRayFileLabel() {
    const SourceSpec& s = m_object.source;
    if (!s.tracesRayFile()) {
        m_rayNote->setText(QStringLiteral("No measured ray set."));
        return;
    }
    m_rayNote->setText(QStringLiteral("%1 — %2 rays. The angular law and emitter "
                                      "size above are not consulted.")
                           .arg(QFileInfo(s.rayFile->path).fileName())
                           .arg(s.rayFile->declared));
}

void ObjectInspector::chooseRayFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open measured ray set"), QString(), rayfile::fileFilter());
    if (path.isEmpty()) return;

    const rayfile::LoadResult res = rayfile::load(path);
    if (!res.ok) {
        QMessageBox::warning(this, QStringLiteral("Ray set"), res.error);
        return;
    }
    m_object.source.rayFile = res.data;
    refreshRayFileLabel();
    syncSourceEnabledState();
    if (m_haveObject) emit sourceEdited(m_object.id);
}

void ObjectInspector::clearRayFile() {
    if (!m_object.source.rayFile) return;
    m_object.source.rayFile.reset();
    refreshRayFileLabel();
    syncSourceEnabledState();
    if (m_haveObject) emit sourceEdited(m_object.id);
}
