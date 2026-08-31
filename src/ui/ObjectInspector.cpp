#include "ObjectInspector.h"
#include "MultiEdit.h"
#include "SurfaceInspector.h"

#include <algorithm>
#include <map>

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
#include <QStyle>
#include <QVBoxLayout>

#include "core/RayFile.h"

using scenedoc::ObjectType;
using scenedoc::SceneObject;

namespace {

QDoubleSpinBox* coord(QWidget* parent, const QString& suffix, double step = 1.0) {
    auto* s = new multiedit::DoubleSpinBox(parent);
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

    // The one thing that must be impossible to miss: that what is being typed
    // into reaches more than the object whose name is at the top.
    m_multiBanner = new QLabel(m_body);
    m_multiBanner->setWordWrap(true);
    m_multiBanner->setStyleSheet(QStringLiteral(
        "background:#3a2f12; color:#e8c46a; border:1px solid #6b5520;"
        "border-radius:3px; padding:5px; font-size:11px;"));
    m_multiBanner->setVisible(false);
    v->addWidget(m_multiBanner);

    auto* nameForm = new QFormLayout;
    m_name = new QLineEdit(m_body);
    nameForm->addRow(QStringLiteral("Name"), m_name);
    v->addLayout(nameForm);
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        if (m_loading || !m_haveObject || isMulti()) return;
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

    m_scale = new multiedit::DoubleSpinBox(m_placeBox);
    m_scale->setRange(scenedoc::SceneDocument::kMinScale,
                      scenedoc::SceneDocument::kMaxScale);
    m_scale->setDecimals(3);
    m_scale->setSingleStep(0.05);
    m_scale->setKeyboardTracking(false);
    m_scale->setToolTip(QStringLiteral(
        "Size, as a multiple of the dimensions above. Uniform, because an optic "
        "stretched along one axis is a different optic rather than the same one "
        "at another size -- to change a lens in one direction, edit its own "
        "dimensions instead. This is the same number the viewport's Scale gizmo "
        "sets."));
    pf->addRow(QStringLiteral("Scale"), m_scale);

    m_placeBox->setToolTip(QStringLiteral(
        "Where the object sits, relative to its group, and how big it is. "
        "Rotations are applied X, then Y, then Z, and the scale is about the "
        "object's own origin. The viewport's Move / Rotate / Scale gizmos write "
        "these same numbers."));
    v->addWidget(m_placeBox);

    struct PlaceTag { QDoubleSpinBox* w; quint32 f; };
    for (const PlaceTag& t : {PlaceTag{m_px,    multiedit::place::PosX},
                              PlaceTag{m_py,    multiedit::place::PosY},
                              PlaceTag{m_pz,    multiedit::place::PosZ},
                              PlaceTag{m_rx,    multiedit::place::RotX},
                              PlaceTag{m_ry,    multiedit::place::RotY},
                              PlaceTag{m_rz,    multiedit::place::RotZ},
                              PlaceTag{m_scale, multiedit::place::Scale}}) {
        const quint32 f = t.f;
        connect(t.w, &QDoubleSpinBox::valueChanged, this, [this, f] {
            if (m_loading || !m_haveObject) return;
            m_object.position       = gp_Pnt(m_px->value(), m_py->value(), m_pz->value());
            m_object.rotationDeg[0] = m_rx->value();
            m_object.rotationDeg[1] = m_ry->value();
            m_object.rotationDeg[2] = m_rz->value();
            m_object.scale          = m_scale->value();
            emitPlacement(f);
        });
    }

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

    m_halfAngle = new multiedit::DoubleSpinBox(m_sourceBox);
    m_halfAngle->setRange(0.0, 180.0);
    m_halfAngle->setSuffix(QStringLiteral(" deg"));
    m_halfAngle->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Cone half-angle"), m_halfAngle);

    m_sizeA = new multiedit::DoubleSpinBox(m_sourceBox);
    m_sizeA->setRange(0.0, 10000.0);
    m_sizeA->setSuffix(QStringLiteral(" mm"));
    m_sizeA->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Size A (radius / width)"), m_sizeA);

    m_sizeB = new multiedit::DoubleSpinBox(m_sourceBox);
    m_sizeB->setRange(0.0, 10000.0);
    m_sizeB->setSuffix(QStringLiteral(" mm"));
    m_sizeB->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Size B (height)"), m_sizeB);

    m_beamRadius = new multiedit::DoubleSpinBox(m_sourceBox);
    m_beamRadius->setRange(0.0, 10000.0);
    m_beamRadius->setSuffix(QStringLiteral(" mm"));
    m_beamRadius->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Beam radius"), m_beamRadius);

    m_spectrum = new QComboBox(m_sourceBox);
    for (int i = 0; i < SpectrumConfig::kKindCount; ++i)
        m_spectrum->addItem(SpectrumConfig::kindName(SpectrumConfig::Kind(i)));
    sf->addRow(QStringLiteral("Spectrum"), m_spectrum);

    m_wavelength = new multiedit::DoubleSpinBox(m_sourceBox);
    m_wavelength->setRange(200.0, 2000.0);
    m_wavelength->setSuffix(QStringLiteral(" nm"));
    m_wavelength->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Wavelength"), m_wavelength);

    m_cct = new multiedit::DoubleSpinBox(m_sourceBox);
    m_cct->setRange(1000.0, 20000.0);
    m_cct->setSingleStep(100.0);
    m_cct->setSuffix(QStringLiteral(" K"));
    m_cct->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Colour temperature"), m_cct);

    m_power = new multiedit::DoubleSpinBox(m_sourceBox);
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

    m_rayScale = new multiedit::DoubleSpinBox(m_sourceBox);
    m_rayScale->setRange(1e-6, 1e6);
    m_rayScale->setDecimals(4);
    m_rayScale->setValue(1.0);
    m_rayScale->setKeyboardTracking(false);
    sf->addRow(QStringLiteral("Ray set scale"), m_rayScale);

    m_rayLambda = new QCheckBox(QStringLiteral("Use the file's wavelengths"), m_sourceBox);
    sf->addRow(m_rayLambda);

    v->addWidget(m_sourceBox);

    struct SrcComboTag { QComboBox* w; quint32 f; };
    for (const SrcComboTag& t : {SrcComboTag{m_srcType,  multiedit::src::Type},
                                 SrcComboTag{m_srcShape, multiedit::src::Shape},
                                 SrcComboTag{m_spectrum, multiedit::src::Spectrum},
                                 SrcComboTag{m_polState, multiedit::src::Polarisation}}) {
        QComboBox*    w = t.w;
        const quint32 f = t.f;
        connect(w, &QComboBox::currentIndexChanged, this, [this, w, f](int) {
            multiedit::clearMixed(w);
            emitSource(f);
        });
    }
    connect(m_rayLambda, &QCheckBox::toggled, this, [this](bool) {
        multiedit::clearMixed(m_rayLambda);
        emitSource(multiedit::src::RayLambda);
    });

    struct SrcSpinTag { QDoubleSpinBox* w; quint32 f; };
    for (const SrcSpinTag& t : {SrcSpinTag{m_halfAngle,  multiedit::src::HalfAngle},
                                SrcSpinTag{m_sizeA,      multiedit::src::SizeA},
                                SrcSpinTag{m_sizeB,      multiedit::src::SizeB},
                                SrcSpinTag{m_beamRadius, multiedit::src::BeamRadius},
                                SrcSpinTag{m_wavelength, multiedit::src::Wavelength},
                                SrcSpinTag{m_cct,        multiedit::src::Cct},
                                SrcSpinTag{m_power,      multiedit::src::Power},
                                SrcSpinTag{m_rayScale,   multiedit::src::RayScale}}) {
        const quint32 f = t.f;
        connect(t.w, &QDoubleSpinBox::valueChanged, this, [this, f](double) {
            emitSource(f);
        });
    }

    connect(m_rayChoose, &QPushButton::clicked, this, &ObjectInspector::chooseRayFile);
    connect(m_rayClear,  &QPushButton::clicked, this, &ObjectInspector::clearRayFile);

    // ---- optics ------------------------------------------------------------
    m_optics = new SurfaceInspector(m_body);
    v->addWidget(m_optics);
    connect(m_optics, &SurfaceInspector::edited, this, [this](quint32 fields) {
        if (m_loading || !m_haveObject) return;
        m_object.optics = m_optics->optics();
        if (isMulti()) emit multiOpticsEdited(fields);
        else           emit opticsEdited(m_object.id);
    });
    connect(m_optics, &SurfaceInspector::resetRequested, this, [this] {
        // Offered on one surface only: each of a selection has its own scene
        // value, so there is no single thing this could restore.
        if (!m_haveObject || isMulti()) return;
        emit opticsResetRequested(m_object.id);
    });

    v->addStretch(1);
    clearObject();
}

QSize ObjectInspector::minimumSizeHint() const {
    QSize base = QWidget::minimumSizeHint();
    if (!m_body || !m_scroll) return base;
    // Answered on demand rather than stored, because the number moves: which
    // sections are shown depends on what is selected, and none of the font
    // metrics behind any of it are final until the widget is polished.
    m_body->ensurePolished();
    base.setWidth(m_body->minimumSizeHint().width() +
                  style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this) +
                  2 * m_scroll->frameWidth());
    return base;
}

QSize ObjectInspector::sizeHint() const {
    return QSize(minimumSizeHint().width(), QWidget::sizeHint().height());
}

void ObjectInspector::rebuildParamRows(bool sameObject) {
    const std::vector<SceneParamInfo>& info = scenedoc::typeInfo(m_object.type).params;
    const std::size_t want = std::min(info.size(), std::size_t(SceneObject::kMaxParams));

    // The rows belong to the type, so re-reading an object of the type they
    // were already built for is a matter of writing the numbers in -- not of
    // building the same rows again.
    //
    // This is not only an economy. rebuildParamRows is reachable from inside
    // one of these spin boxes' own valueChanged: the box reports an edit, the
    // window writes it into the document, and the panel is asked to show the
    // document again. Destroying the box at that point frees it while it is
    // still on the stack about to be returned into, which is what took the
    // window down every time a radius was nudged.
    if (m_paramType == int(m_object.type) && m_params.size() == want) {
        for (std::size_t i = 0; i < want; ++i) {
            // Whatever the user is holding stays as they left it -- but only
            // while it is still the same object's number.
            if (sameObject && m_params[i]->hasFocus()) continue;
            const double v = m_object.p[i];
            if (m_params[i]->value() != v) m_params[i]->setValue(v);
        }
        return;
    }

    // A genuinely different type: take the old rows out of the layout and let
    // the event loop delete them, for the same reason -- `delete` here would be
    // a delete of whatever is still unwinding above us.
    for (QDoubleSpinBox* s : m_params) {
        const QFormLayout::TakeRowResult row = m_geomForm->takeRow(s);
        for (QLayoutItem* item : {row.labelItem, row.fieldItem}) {
            if (!item) continue;
            if (QWidget* w = item->widget()) { w->hide(); w->deleteLater(); }
            delete item;
        }
    }
    m_params.clear();
    m_paramType = int(m_object.type);

    for (std::size_t i = 0; i < want; ++i) {
        const SceneParamInfo& p = info[i];
        auto* s = new multiedit::DoubleSpinBox(m_geomBox);
        s->setRange(p.min, p.max);
        s->setDecimals(p.decimals);
        s->setSingleStep(p.step);
        s->setValue(m_object.p[i]);
        s->setKeyboardTracking(false);
        if (!p.unit.isEmpty()) s->setSuffix(QStringLiteral(" ") + p.unit);
        if (!p.tip.isEmpty())  s->setToolTip(p.tip);
        m_geomForm->addRow(p.name, s);
        m_params.push_back(s);

        const int slot = int(i);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, slot] {
            if (m_loading || !m_haveObject) return;
            for (std::size_t k = 0; k < m_params.size(); ++k)
                m_object.p[k] = m_params[k]->value();
            emitParameters(multiedit::param::slot(slot));
        });
    }
}

namespace {

// Writes `v` into `s` unless the user is in the middle of using it.
//
// Re-reading the document into the panel is right when the selection changed
// and wrong when the value came from this box a moment ago: setValue would
// reformat the text under the caret mid-type, and reset a wheel drag to
// whatever the last committed step was.
void setIfIdle(QDoubleSpinBox* s, double v) {
    if (!s || s->hasFocus() || s->value() == v) return;
    s->setValue(v);
}

} // namespace

void ObjectInspector::setObjects(const std::vector<SceneObject>& objects) {
    if (objects.empty()) { clearObject(); return; }
    if (objects.size() == 1) {
        setObject(objects.front(), objects.front().sceneOptics);
        return;
    }
    showObjects(objects);
}

void ObjectInspector::setObject(const SceneObject& object, const SurfaceOptics& defaultOptics) {
    m_defaultOptics = defaultOptics;
    showObjects({object});
}

void ObjectInspector::showObjects(const std::vector<SceneObject>& objects) {
    const SceneObject& object = objects.front();

    // Whether this is a change of subject or the same selection read back. The
    // second is allowed to leave the control the user is working in alone; the
    // first has to overwrite everything, because none of it is about these
    // objects any more.
    const bool same = m_haveObject && m_object.id == object.id &&
                      m_object.type == object.type &&
                      m_count == int(objects.size());

    m_loading    = true;
    m_object     = object;
    m_count      = int(objects.size());
    m_haveObject = true;

    const scenedoc::TypeInfo& info = scenedoc::typeInfo(object.type);
    showSelectionSummary(objects);
    if (!isMulti() && (!same || !m_name->hasFocus())) m_name->setText(object.name);
    // Two objects cannot share one name and stay two objects the tree can tell
    // apart, so this is the one common property a multi-selection does not
    // offer -- which is what Unreal and Unity do with it as well.
    m_name->setEnabled(!isMulti());
    if (isMulti())
        m_name->setText(QStringLiteral("%1 objects").arg(objects.size()));
    m_name->setToolTip(isMulti()
        ? QStringLiteral("Renaming is per object: select one row to rename it.")
        : QString());

    if (same) {
        setIfIdle(m_px, object.position.X());
        setIfIdle(m_py, object.position.Y());
        setIfIdle(m_pz, object.position.Z());
        setIfIdle(m_rx, object.rotationDeg[0]);
        setIfIdle(m_ry, object.rotationDeg[1]);
        setIfIdle(m_rz, object.rotationDeg[2]);
        setIfIdle(m_scale, object.scale);
    } else {
        m_px->setValue(object.position.X());
        m_py->setValue(object.position.Y());
        m_pz->setValue(object.position.Z());
        m_rx->setValue(object.rotationDeg[0]);
        m_ry->setValue(object.rotationDeg[1]);
        m_rz->setValue(object.rotationDeg[2]);
        m_scale->setValue(object.scale);
    }

    bool sameType = true;
    for (const SceneObject& o : objects) sameType = sameType && o.type == object.type;

    rebuildParamRows(same);
    const bool baked = object.type == ObjectType::TutorialPart;
    m_bakedNote->setVisible(baked && sameType);
    if (baked)
        m_bakedNote->setText(QStringLiteral(
            "Built by the tutorial. Its dimensions come from the scene parameters "
            "on the right; everything else about it is editable here."));
    // Slot 2 is a radius on one type and a wall thickness on another, so there
    // is nothing a mixed-type selection could put in these rows.
    m_geomBox->setVisible(sameType && (!info.params.empty() || baked));

    // ---- source ----
    bool allSources = true;
    for (const SceneObject& o : objects) allSources = allSources && o.isSource();
    const bool isSource = allSources;
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
    bool hasOptics = true;
    for (const SceneObject& o : objects)
        hasOptics = hasOptics && !o.isGroup() && !o.isSource();
    m_optics->setVisible(hasOptics);
    if (hasOptics && isMulti()) {
        std::vector<SurfaceOptics> values;
        values.reserve(objects.size());
        for (const SceneObject& o : objects) values.push_back(o.optics);
        m_optics->setSurfaces(
            QStringLiteral("%1 surfaces selected").arg(objects.size()), values);
    } else if (hasOptics) {
        m_optics->setSurface(object.name, object.optics, m_defaultOptics, false);
    } else {
        m_optics->clearSurface();
    }

    markMixedRows(objects);
    // After the dashes, not before: what to grey out depends on what the fields
    // now read, and a dashed field is not an answer to that question either.
    if (m_sourceBox->isVisible()) syncSourceEnabledState();

    m_empty->setVisible(false);
    m_scroll->setVisible(true);
    m_loading = false;
}

// The banner, and the two lines above it.
void ObjectInspector::showSelectionSummary(const std::vector<SceneObject>& objects) {
    if (objects.size() == 1) {
        m_title->setText(objects.front().name);
        m_kind->setText(scenedoc::typeInfo(objects.front().type).description);
        m_multiBanner->setVisible(false);
        return;
    }

    // Counted by the label the tree shows, not by the enum: a tutorial's parts
    // are all TutorialPart, and a breakdown reading "4 tutorial part" answers a
    // question nobody asked.
    std::map<QString, int> kinds;
    for (const SceneObject& o : objects) ++kinds[scenedoc::typeLabel(o)];

    QStringList parts;
    for (const auto& [label, n] : kinds)
        parts << QStringLiteral("%1 x %2").arg(n).arg(label);

    m_title->setText(QStringLiteral("%1 objects selected").arg(objects.size()));
    m_kind->setText(parts.join(QStringLiteral("  ·  ")));
    m_multiBanner->setText(
        kinds.size() == 1
            ? QStringLiteral("MULTI-EDIT — %1 objects. Every property below is "
                             "written to all %1 of them.")
                  .arg(objects.size())
            : QStringLiteral("MULTI-EDIT — %1 objects of %2 types. Only the "
                             "properties they share are shown, and each one is "
                             "written to all %1 of them.")
                  .arg(objects.size()).arg(kinds.size()));
    m_multiBanner->setVisible(true);
}

// Which rows the selection disagrees about. Nothing here decides what is shown
// -- only what a shown row reads as.
void ObjectInspector::markMixedRows(const std::vector<SceneObject>& objects) {
    const auto differs = [&objects](auto get) {
        for (std::size_t i = 1; i < objects.size(); ++i)
            if (!(get(objects[i]) == get(objects[0]))) return true;
        return false;
    };

    multiedit::setMixed(m_px, differs([](const SceneObject& o) { return o.position.X(); }));
    multiedit::setMixed(m_py, differs([](const SceneObject& o) { return o.position.Y(); }));
    multiedit::setMixed(m_pz, differs([](const SceneObject& o) { return o.position.Z(); }));
    multiedit::setMixed(m_rx, differs([](const SceneObject& o) { return o.rotationDeg[0]; }));
    multiedit::setMixed(m_ry, differs([](const SceneObject& o) { return o.rotationDeg[1]; }));
    multiedit::setMixed(m_rz, differs([](const SceneObject& o) { return o.rotationDeg[2]; }));
    multiedit::setMixed(m_scale, differs([](const SceneObject& o) { return o.scale; }));

    for (std::size_t i = 0; i < m_params.size(); ++i)
        multiedit::setMixed(m_params[i],
            differs([i](const SceneObject& o) { return o.p[i]; }));

    if (!m_sourceBox->isVisible()) return;
    multiedit::setMixed(m_srcType,  differs([](const SceneObject& o) { return int(o.source.type); }));
    multiedit::setMixed(m_srcShape, differs([](const SceneObject& o) { return int(o.source.shape); }));
    multiedit::setMixed(m_spectrum, differs([](const SceneObject& o) { return int(o.source.spectrum.kind); }));
    multiedit::setMixed(m_polState, differs([](const SceneObject& o) { return o.source.polarisationState; }));
    multiedit::setMixed(m_halfAngle,  differs([](const SceneObject& o) { return o.source.halfAngleDeg; }));
    multiedit::setMixed(m_sizeA,      differs([](const SceneObject& o) { return o.source.sizeA; }));
    multiedit::setMixed(m_sizeB,      differs([](const SceneObject& o) { return o.source.sizeB; }));
    multiedit::setMixed(m_beamRadius, differs([](const SceneObject& o) { return o.source.beamRadius; }));
    multiedit::setMixed(m_wavelength, differs([](const SceneObject& o) { return o.source.spectrum.wavelengthNm; }));
    multiedit::setMixed(m_cct,        differs([](const SceneObject& o) { return o.source.spectrum.cct; }));
    multiedit::setMixed(m_power,      differs([](const SceneObject& o) { return o.source.power; }));
    multiedit::setMixed(m_rayScale,   differs([](const SceneObject& o) { return o.source.rayFileScale; }));
    multiedit::setMixed(m_rayLambda,  differs([](const SceneObject& o) { return o.source.rayFileWavelengths; }));
}

void ObjectInspector::emitPlacement(quint32 field) {
    if (isMulti()) emit multiPlacementEdited(field);
    else           emit placementEdited(m_object.id);
}

void ObjectInspector::emitParameters(quint32 field) {
    if (isMulti()) emit multiParametersEdited(field);
    else           emit parametersEdited(m_object.id);
}

void ObjectInspector::emitSource(quint32 field) {
    if (m_loading || !m_haveObject) return;
    readSourceForm();
    syncSourceEnabledState();
    if (isMulti()) emit multiSourceEdited(field);
    else           emit sourceEdited(m_object.id);
}

void ObjectInspector::clearObject() {
    m_haveObject = false;
    m_count      = 0;
    m_object     = SceneObject{};
    m_multiBanner->setVisible(false);
    m_name->setEnabled(true);
    // Nothing is shown, so the rows describe no type: the next selection has to
    // build its own rather than inherit whichever ones happen to be left.
    m_paramType  = -1;
    m_optics->clearSurface();
    m_empty->setVisible(true);
    m_scroll->setVisible(false);
}

void ObjectInspector::readSourceForm() {
    // A field showing a dash is not read: it holds the primary's value for its
    // arrows to step from, and the caller's field mask keeps it from being
    // written anywhere. Reading a dashed combo would be worse than useless --
    // its current row sits one past the end of the enum it maps onto.
    SourceSpec& s = m_object.source;
    if (!multiedit::isMixed(m_srcType))
        s.type = SourceConfig::Type(std::clamp(m_srcType->currentIndex(), 0, 2));
    if (!multiedit::isMixed(m_srcShape))
        s.shape = SourceConfig::Shape(std::clamp(m_srcShape->currentIndex(), 0, 3));
    if (!multiedit::isMixed(m_halfAngle))  s.halfAngleDeg = m_halfAngle->value();
    if (!multiedit::isMixed(m_sizeA))      s.sizeA        = m_sizeA->value();
    if (!multiedit::isMixed(m_sizeB))      s.sizeB        = m_sizeB->value();
    if (!multiedit::isMixed(m_beamRadius)) s.beamRadius   = m_beamRadius->value();
    if (!multiedit::isMixed(m_spectrum))
        s.spectrum.kind = SpectrumConfig::Kind(
            std::clamp(m_spectrum->currentIndex(), 0, SpectrumConfig::kKindCount - 1));
    if (!multiedit::isMixed(m_wavelength)) s.spectrum.wavelengthNm = m_wavelength->value();
    if (!multiedit::isMixed(m_cct))        s.spectrum.cct = m_cct->value();
    if (!multiedit::isMixed(m_power))      s.power        = m_power->value();
    if (!multiedit::isMixed(m_polState))
        s.polarisationState = std::clamp(m_polState->currentIndex(), 0, 3);
    if (!multiedit::isMixed(m_rayScale))   s.rayFileScale = m_rayScale->value();
    if (!multiedit::isMixed(m_rayLambda))  s.rayFileWavelengths = m_rayLambda->isChecked();
}

void ObjectInspector::syncSourceEnabledState() {
    // A dashed field is read as the permissive answer: with several sources
    // disagreeing about their angular law, the cone half-angle stays reachable
    // rather than being greyed out on the strength of whichever one is primary.
    const bool mixedType  = multiedit::isMixed(m_srcType);
    const bool mixedShape = multiedit::isMixed(m_srcShape);
    const bool mixedKind  = multiedit::isMixed(m_spectrum);
    const auto type  = SourceConfig::Type(std::clamp(m_srcType->currentIndex(), 0, 2));
    const auto shape = SourceConfig::Shape(std::clamp(m_srcShape->currentIndex(), 0, 3));
    const auto kind  = SpectrumConfig::Kind(
        std::clamp(m_spectrum->currentIndex(), 0, SpectrumConfig::kKindCount - 1));
    // A measured set carries every ray already, so the analytic emitter's own
    // controls are not consulted at all. Naming them inactive is the difference
    // between a form that lies and one that does not.
    const bool measured = m_object.source.tracesRayFile();

    m_srcType->setEnabled(!measured);
    m_srcShape->setEnabled(!measured);
    m_halfAngle->setEnabled(!measured &&
                            (mixedType || type != SourceConfig::Type::Collimated));
    m_sizeA->setEnabled(!measured &&
                        (mixedShape || shape != SourceConfig::Shape::PointLike));
    m_sizeB->setEnabled(!measured &&
                        (mixedShape || shape == SourceConfig::Shape::Rect));
    m_beamRadius->setEnabled(!measured &&
                             (mixedType || type == SourceConfig::Type::Collimated));
    m_wavelength->setEnabled(mixedKind || kind == SpectrumConfig::Kind::Monochromatic);
    m_cct->setEnabled(mixedKind || kind == SpectrumConfig::Kind::Blackbody ||
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
    // Loading one file into every selected source is a real operation, and it
    // goes through the same field mask as everything else.
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
    if (!m_haveObject) return;
    if (isMulti()) emit multiSourceEdited(multiedit::src::RayFile);
    else           emit sourceEdited(m_object.id);
}

void ObjectInspector::clearRayFile() {
    if (!m_object.source.rayFile) return;
    m_object.source.rayFile.reset();
    refreshRayFileLabel();
    syncSourceEnabledState();
    if (!m_haveObject) return;
    if (isMulti()) emit multiSourceEdited(multiedit::src::RayFile);
    else           emit sourceEdited(m_object.id);
}
