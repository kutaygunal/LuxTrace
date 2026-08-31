#include "MultiEdit.h"

#include <QLineEdit>
#include <QSignalBlocker>

namespace multiedit {

QString marker() { return QStringLiteral("—"); }   // em dash

namespace {

constexpr char kMixedItem[] = "luxtrace_mixed_item";

// The text a spin box is actually showing, with the prefix and suffix taken
// off, so the dash can be recognised whatever unit the field carries.
QString core(const QString& prefix, const QString& suffix, QString text) {
    if (!prefix.isEmpty() && text.startsWith(prefix)) text.remove(0, prefix.size());
    if (!suffix.isEmpty() && text.endsWith(suffix))   text.chop(suffix.size());
    return text.trimmed();
}

} // namespace

// ---- DoubleSpinBox ---------------------------------------------------------

DoubleSpinBox::DoubleSpinBox(QWidget* parent) : QDoubleSpinBox(parent) {
    // Typing is an answer, so the dash goes as soon as a key lands. The flag is
    // dropped without redrawing: re-rendering here would replace the digits the
    // user has just typed with the value they are replacing.
    if (QLineEdit* e = lineEdit())
        connect(e, &QLineEdit::textEdited, this, [this](const QString&) { m_mixed = false; });
}

// Puts the dash on screen without going through the value machinery.
//
// setValue() cannot do it -- the value has not changed, that is the whole point
// -- and the one call that rewrites the editor from textFromValue() is private.
// Writing the line edit is the same thing that call would have done, and
// lineEdit() is protected, which is why this lives in the class.
void DoubleSpinBox::setMixed(bool on) {
    if (m_mixed == on) return;
    m_mixed = on;
    if (QLineEdit* e = lineEdit())
        e->setText(prefix() + textFromValue(value()) + suffix());
}

void DoubleSpinBox::stepBy(int steps) {
    // A step is an answer too, and it steps from the primary selection's value
    // -- which is why the box keeps that value behind the dash.
    m_mixed = false;
    QDoubleSpinBox::stepBy(steps);
}

QString DoubleSpinBox::textFromValue(double v) const {
    return m_mixed ? marker() : QDoubleSpinBox::textFromValue(v);
}

double DoubleSpinBox::valueFromText(const QString& text) const {
    if (core(prefix(), suffix(), text) == marker()) return value();
    return QDoubleSpinBox::valueFromText(text);
}

QValidator::State DoubleSpinBox::validate(QString& input, int& pos) const {
    // The dash has to validate, or Qt throws it away the moment the field loses
    // focus and puts the primary's number there instead -- which is the one
    // thing a mixed field must never claim.
    if (core(prefix(), suffix(), input) == marker()) return QValidator::Acceptable;
    return QDoubleSpinBox::validate(input, pos);
}

// ---- SpinBox ---------------------------------------------------------------

SpinBox::SpinBox(QWidget* parent) : QSpinBox(parent) {
    if (QLineEdit* e = lineEdit())
        connect(e, &QLineEdit::textEdited, this, [this](const QString&) { m_mixed = false; });
}

void SpinBox::setMixed(bool on) {
    if (m_mixed == on) return;
    m_mixed = on;
    if (QLineEdit* e = lineEdit())
        e->setText(prefix() + textFromValue(value()) + suffix());
}

void SpinBox::stepBy(int steps) {
    m_mixed = false;
    QSpinBox::stepBy(steps);
}

QString SpinBox::textFromValue(int v) const {
    return m_mixed ? marker() : QSpinBox::textFromValue(v);
}

int SpinBox::valueFromText(const QString& text) const {
    if (core(prefix(), suffix(), text) == marker()) return value();
    return QSpinBox::valueFromText(text);
}

QValidator::State SpinBox::validate(QString& input, int& pos) const {
    if (core(prefix(), suffix(), input) == marker()) return QValidator::Acceptable;
    return QSpinBox::validate(input, pos);
}

// ---- the state, on whichever editor a field uses ---------------------------

void setMixed(QDoubleSpinBox* s, bool on) {
    if (auto* m = qobject_cast<DoubleSpinBox*>(s)) m->setMixed(on);
}

void setMixed(QSpinBox* s, bool on) {
    if (auto* m = qobject_cast<SpinBox*>(s)) m->setMixed(on);
}

void setMixed(QComboBox* c, bool on) {
    if (!c) return;
    const int last = c->count() - 1;
    const bool have = last >= 0 &&
                      c->itemData(last, Qt::UserRole + 7).toString() ==
                          QLatin1String(kMixedItem);
    if (on) {
        if (!have) {
            c->addItem(marker());
            c->setItemData(c->count() - 1, QLatin1String(kMixedItem), Qt::UserRole + 7);
        }
        // Appended, never inserted: every real choice keeps the index the code
        // that reads it back expects.
        c->setCurrentIndex(c->count() - 1);
    } else if (have) {
        // Silenced while the row goes, because the panel's own handler for this
        // combo calls clearMixed() -- which would take the same row out from
        // under us, leaving the removeItem below pointed at a real entry.
        const QSignalBlocker block(c);
        if (c->currentIndex() == last) c->setCurrentIndex(-1);
        c->removeItem(last);
    }
}

void setMixed(QCheckBox* c, bool on) {
    if (!c) return;
    if (on) {
        c->setTristate(true);
        c->setCheckState(Qt::PartiallyChecked);
    } else {
        // Cleared as well as disallowed. A box left partially checked with
        // tristate off is a state nothing can get out of by clicking.
        if (c->checkState() == Qt::PartiallyChecked) c->setCheckState(Qt::Unchecked);
        c->setTristate(false);
    }
}

bool isMixed(const QDoubleSpinBox* s) {
    const auto* m = qobject_cast<const DoubleSpinBox*>(s);
    return m && m->isMixed();
}

bool isMixed(const QSpinBox* s) {
    const auto* m = qobject_cast<const SpinBox*>(s);
    return m && m->isMixed();
}

bool isMixed(const QComboBox* c) {
    if (!c) return false;
    const int i = c->currentIndex();
    return i >= 0 &&
           c->itemData(i, Qt::UserRole + 7).toString() == QLatin1String(kMixedItem);
}

bool isMixed(const QCheckBox* c) {
    return c && c->checkState() == Qt::PartiallyChecked;
}

void clearMixed(QComboBox* c) {
    if (!c) return;
    const int last = c->count() - 1;
    if (last < 0) return;
    if (c->itemData(last, Qt::UserRole + 7).toString() != QLatin1String(kMixedItem)) return;
    // Only once a real entry is current. Removing the row that is current would
    // move the selection somewhere nobody asked for.
    if (c->currentIndex() == last) return;
    c->removeItem(last);
}

void clearMixed(QCheckBox* c) {
    if (c) c->setTristate(false);
}

} // namespace multiedit
