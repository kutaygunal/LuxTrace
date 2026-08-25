#include "ImportDialog.h"
#include "core/Material.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// The units a CAD file is realistically in, and what a millimetre-based tracer
// has to multiply by. "From the file" is first because it is right by default
// wherever the file says anything at all.
struct UnitChoice { const char* name; double mm; };
const UnitChoice kUnits[] = {
    {"From the file",  0.0},
    {"Millimetre",     1.0},
    {"Centimetre",     10.0},
    {"Metre",          1000.0},
    {"Inch",           25.4},
    {"Micrometre",     0.001},
    {"Custom",         -1.0},
};
constexpr int kUnitCount = int(sizeof(kUnits) / sizeof(kUnits[0]));

const char* kAxisNames[] = {
    "+Z (along the optical axis)", "-Z", "+X", "-X", "+Y", "-Y",
};
const gp_Dir kAxes[] = {
    gp_Dir(0, 0, 1),  gp_Dir(0, 0, -1),
    gp_Dir(1, 0, 0),  gp_Dir(-1, 0, 0),
    gp_Dir(0, 1, 0),  gp_Dir(0, -1, 0),
};
constexpr int kAxisCount = int(sizeof(kAxisNames) / sizeof(kAxisNames[0]));

QColor statusColour(cadimport::GeometryAudit::Status s) {
    switch (s) {
    case cadimport::GeometryAudit::Status::Ok:      return QColor(40, 150, 70);
    case cadimport::GeometryAudit::Status::Warning: return QColor(190, 130, 20);
    case cadimport::GeometryAudit::Status::Bad:     return QColor(190, 55, 45);
    }
    return QColor(120, 120, 120);
}

QString statusText(const cadimport::GeometryAudit& a) {
    switch (a.status) {
    case cadimport::GeometryAudit::Status::Ok:      return QStringLiteral("ok");
    case cadimport::GeometryAudit::Status::Warning: return QStringLiteral("warning");
    case cadimport::GeometryAudit::Status::Bad:     return QStringLiteral("not closed");
    }
    return QString();
}

} // namespace

ImportDialog::ImportDialog(const QString& path, QWidget* parent)
    : QDialog(parent), m_path(path) {
    setWindowTitle(QStringLiteral("Import CAD"));
    resize(720, 560);

    auto* outer = new QVBoxLayout(this);

    m_headline = new QLabel(this);
    m_headline->setWordWrap(true);
    outer->addWidget(m_headline);

    auto* form = new QFormLayout;

    m_unit = new QComboBox(this);
    for (const UnitChoice& u : kUnits) m_unit->addItem(QLatin1String(u.name));
    m_unit->setToolTip(QStringLiteral(
        "A STEP file states its own length unit, and reading it is what makes an "
        "import right by default rather than right if you happened to know. The "
        "rest of this list is the override, for a file that does not say or says "
        "something wrong."));

    m_customScale = new QDoubleSpinBox(this);
    m_customScale->setRange(1e-6, 1e6);
    m_customScale->setDecimals(6);
    m_customScale->setValue(1.0);
    m_customScale->setSuffix(QStringLiteral(" mm per unit"));
    m_customScale->setEnabled(false);

    auto* unitRow = new QHBoxLayout;
    unitRow->setContentsMargins(0, 0, 0, 0);
    unitRow->addWidget(m_unit, 1);
    unitRow->addWidget(m_customScale, 1);

    m_axis = new QComboBox(this);
    for (const char* a : kAxisNames) m_axis->addItem(QLatin1String(a));
    m_axis->setToolTip(QStringLiteral(
        "Which way the light travels through the part. A CAD file has no "
        "preferred direction: a prism lit down its extrusion axis is a slab, and "
        "the deviation it exists for only appears when the beam meets a slanted "
        "face."));

    m_material = new QComboBox(this);
    for (int i = 0; i < materials::count(); ++i) {
        m_material->addItem(materials::name(i));
        m_material->setItemData(i, materials::description(i), Qt::ToolTipRole);
    }
    {
        const int bk7 = materials::indexOf(QStringLiteral("N-BK7"));
        if (bk7 >= 0) m_material->setCurrentIndex(bk7);
    }

    m_reflective = new QCheckBox(QStringLiteral("Treat every part as a mirror"), this);
    m_reflective->setToolTip(QStringLiteral(
        "Off, the parts are refractive solids of the material above. On, they are "
        "front-surface reflectors. Either way, click a surface afterwards to give "
        "it something else."));

    m_heal = new QCheckBox(QStringLiteral("Heal parts that fail the check"), this);
    m_heal->setToolTip(QStringLiteral(
        "Runs ShapeFix over any part whose geometry the tracer's assumptions do "
        "not hold on. Off by default: healing changes the geometry you were "
        "given, and doing that silently is worse than saying it needs it. A "
        "repair that makes a part worse is discarded rather than kept."));

    form->addRow(QStringLiteral("Units:"), unitRow);
    form->addRow(QStringLiteral("Illuminate along:"), m_axis);
    form->addRow(QStringLiteral("Material:"), m_material);
    form->addRow(m_reflective);
    form->addRow(m_heal);
    outer->addLayout(form);

    m_parts = new QTreeWidget(this);
    m_parts->setColumnCount(5);
    m_parts->setHeaderLabels({QStringLiteral("Part"), QStringLiteral("Faces"),
                              QStringLiteral("Size (mm)"), QStringLiteral("Geometry"),
                              QStringLiteral("What was found")});
    m_parts->setRootIsDecorated(false);
    m_parts->setAlternatingRowColors(true);
    m_parts->header()->setStretchLastSection(true);
    outer->addWidget(m_parts, 1);

    m_verdict = new QLabel(this);
    m_verdict->setWordWrap(true);
    outer->addWidget(m_verdict);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    outer->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_unit, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_customScale->setEnabled(i == kUnitCount - 1);
        if (!m_loading) reread();
    });
    connect(m_customScale, &QDoubleSpinBox::valueChanged, this, [this](double) {
        if (!m_loading && m_unit->currentIndex() == kUnitCount - 1) reread();
    });
    connect(m_heal, &QCheckBox::toggled, this, [this](bool) {
        if (!m_loading) reread();
    });

    reread();
}

void ImportDialog::reread() {
    cadimport::ImportOptions opt;
    const int u = m_unit->currentIndex();
    if (u == kUnitCount - 1)      opt.scale = m_customScale->value();
    else if (kUnits[u].mm > 0.0)  opt.scale = kUnits[u].mm;
    else                          opt.scale = 0.0;      // ask the file
    opt.audit = true;
    opt.heal  = m_heal->isChecked();

    // A large assembly takes a moment to read and every face is walked twice.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_result = cadimport::read(m_path, opt);
    QApplication::restoreOverrideCursor();

    // Show what the file said, once the read has been done: the first pass is
    // where the unit is discovered.
    if (m_result.unitFromHeader && !m_result.unitName.isEmpty()) {
        m_loading = true;
        m_unit->setItemText(0, QStringLiteral("From the file (%1)").arg(m_result.unitName));
        m_loading = false;
    }

    refreshTable();
    refreshSummary();
}

void ImportDialog::refreshTable() {
    m_parts->clear();
    if (!m_result.ok) return;

    for (const cadimport::ImportedShape& s : m_result.shapes) {
        auto* item = new QTreeWidgetItem(m_parts);
        item->setText(0, s.label);
        item->setText(1, QString::number(s.faceCount));
        item->setText(2, QStringLiteral("%1 x %2 x %3")
                             .arg(s.bboxMax[0] - s.bboxMin[0], 0, 'f', 2)
                             .arg(s.bboxMax[1] - s.bboxMin[1], 0, 'f', 2)
                             .arg(s.bboxMax[2] - s.bboxMin[2], 0, 'f', 2));
        item->setText(3, statusText(s.audit));
        item->setForeground(3, statusColour(s.audit.status));
        item->setText(4, s.audit.notes.join(QStringLiteral("; ")));
        item->setToolTip(4, s.audit.notes.join(QStringLiteral("\n")));
    }
    m_parts->resizeColumnToContents(0);
    m_parts->resizeColumnToContents(1);
    m_parts->resizeColumnToContents(2);
    m_parts->resizeColumnToContents(3);
}

void ImportDialog::refreshSummary() {
    if (!m_result.ok) {
        m_headline->setText(QStringLiteral("<b>%1</b>").arg(QFileInfo(m_path).fileName()));
        m_verdict->setText(QStringLiteral(
            "<span style='color:#be372d'>Could not read this file: %1</span>")
                               .arg(m_result.error.toHtmlEscaped()));
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);

    double extent = 0.0;
    for (int a = 0; a < 3; ++a)
        extent = std::max(extent, m_result.bboxMax[a] - m_result.bboxMin[a]);

    m_headline->setText(
        QStringLiteral("<b>%1</b> &mdash; %2, %3 part(s), %4 face(s), spanning %5 mm")
            .arg(QFileInfo(m_path).fileName().toHtmlEscaped())
            .arg(m_result.format)
            .arg(m_result.shapes.size())
            .arg(m_result.totalFaces)
            .arg(extent, 0, 'f', 1));

    int bad = 0, warn = 0;
    for (const cadimport::ImportedShape& s : m_result.shapes) {
        if (s.audit.status == cadimport::GeometryAudit::Status::Bad)          ++bad;
        else if (s.audit.status == cadimport::GeometryAudit::Status::Warning) ++warn;
    }

    QString unitLine;
    if (m_result.unitFromHeader)
        unitLine = QStringLiteral("The file says it is in %1, so every coordinate was "
                                  "multiplied by %2.")
                       .arg(m_result.unitName.toHtmlEscaped())
                       .arg(m_result.appliedScale, 0, 'g', 6);
    else
        unitLine = QStringLiteral("The file does not state a unit, so the factor above "
                                  "(%1 mm per unit) was used. Check the size column: "
                                  "a part a thousand times too big is the import "
                                  "mistake everybody makes exactly once.")
                       .arg(m_result.appliedScale, 0, 'g', 6);

    QString verdict = unitLine;
    if (m_result.partsHealed > 0)
        verdict += QStringLiteral("<br>%1 part(s) were repaired with ShapeFix.")
                       .arg(m_result.partsHealed);

    if (bad > 0)
        verdict += QStringLiteral(
            "<br><span style='color:#be372d'><b>%1 part(s) are not closed.</b></span> "
            "A ray can enter a solid like that without ever being recorded as "
            "leaving it, so the medium it is travelling in -- and therefore every "
            "index pair it refracts against -- becomes a guess. It will still "
            "trace, and the answer will still look clean. Try healing them, or go "
            "back to the CAD tool.").arg(bad);
    else if (warn > 0)
        verdict += QStringLiteral(
            "<br><span style='color:#8a5a10'>%1 part(s) came back with warnings.</span> "
            "They are traceable; the notes say what is unusual about them.").arg(warn);
    else
        verdict += QStringLiteral(
            "<br><span style='color:#2a8a46'>Every part is closed and valid.</span> "
            "The run report counts medium-tracking anomalies too, so anything this "
            "check missed still shows up there.");

    m_verdict->setText(verdict);
}

gp_Dir ImportDialog::axis() const {
    return kAxes[std::clamp(m_axis->currentIndex(), 0, kAxisCount - 1)];
}

QString ImportDialog::axisName() const {
    return QLatin1String(kAxisNames[std::clamp(m_axis->currentIndex(), 0, kAxisCount - 1)]);
}

QString ImportDialog::materialName() const { return m_material->currentText(); }

bool ImportDialog::reflective() const { return m_reflective->isChecked(); }
