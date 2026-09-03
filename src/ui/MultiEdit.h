// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QString>
#include <QtGlobal>

// The field flags an edit reports, and the copies that write only those
// fields. They live in the core so a headless test can assert them; this
// header is the widget half -- the editors that can draw a dash.
#include "core/MultiEditFields.h"

// Editing several objects through one form.
//
// A property panel showing one object can put a number in every box. A panel
// showing four cannot, because the four may disagree, and every question the
// panel then has to answer -- what does the box read, what happens if the user
// types in it, what happens to the three boxes they did not touch -- has the
// same answer everywhere this has been solved before. Unity, Unreal, Maya and
// Blender all draw a field whose selected objects disagree as a dash rather
// than as one of the values, all leave a field the user never touched alone,
// and all write a field the user *did* touch to every selected object. That is
// what this implements.
//
// Two pieces:
//
//   * editors that can say "these differ" -- a spin box that reads as a dash, a
//     combo that carries a dash entry, a checkbox in its partially-checked
//     state. Nothing here invents a value; a mixed field holds the primary
//     selection's number so that stepping it has somewhere to step *from*, and
//     shows a dash so that nobody reads that number as the answer for all of
//     them.
//
//   * field flags. The panel reports *which* box the user moved, not just that
//     something changed, because writing the whole form to every object would
//     also write the fields that were only ever a dash -- flattening four
//     different positions onto the primary's the moment somebody nudged a
//     rotation. The flags are what keeps an edit to one number an edit to one
//     number.
namespace multiedit {

// What a field whose objects disagree reads as. An em dash, as every DCC and
// engine editor draws it.
QString marker();

// A spin box that can be told its objects disagree.
//
// The dash is a display state, not a value: the box keeps whatever number it
// was given, so the arrows and the wheel step from the primary selection's
// value instead of from zero, and the first step -- or the first keystroke --
// drops the dash because from then on there *is* one answer.
class DoubleSpinBox : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit DoubleSpinBox(QWidget* parent = nullptr);

    void setMixed(bool on);
    bool isMixed() const { return m_mixed; }

    void stepBy(int steps) override;

protected:
    QString           textFromValue(double v) const override;
    double            valueFromText(const QString& text) const override;
    QValidator::State validate(QString& input, int& pos) const override;

private:
    bool m_mixed = false;
};

// The same, for the integer fields -- medium priority and the receiver's bins.
class SpinBox : public QSpinBox {
    Q_OBJECT
public:
    explicit SpinBox(QWidget* parent = nullptr);

    void setMixed(bool on);
    bool isMixed() const { return m_mixed; }

    void stepBy(int steps) override;

protected:
    QString           textFromValue(int v) const override;
    int               valueFromText(const QString& text) const override;
    QValidator::State validate(QString& input, int& pos) const override;

private:
    bool m_mixed = false;
};

// Setting the state on whatever kind of editor a field happens to use, so the
// two inspectors can drive a row without knowing which one it is. A plain
// QDoubleSpinBox that is not one of ours is left alone rather than refused:
// nothing breaks, the field simply shows the primary's value.
void setMixed(QDoubleSpinBox* s, bool on);
void setMixed(QSpinBox* s, bool on);
// The combo grows a dash entry at the end while it is mixed, so no real choice
// changes index and the code that maps an index onto an enum keeps working.
void setMixed(QComboBox* c, bool on);
// Qt already has the state a checkbox needs; this is the two calls that get it
// into and out of it.
void setMixed(QCheckBox* c, bool on);

bool isMixed(const QDoubleSpinBox* s);
bool isMixed(const QSpinBox* s);
bool isMixed(const QComboBox* c);
bool isMixed(const QCheckBox* c);

// The user has answered for this field: take the dash away. Called from the
// panel's own edit handler, which is the moment we know a real choice was made.
void clearMixed(QComboBox* c);
void clearMixed(QCheckBox* c);

} // namespace multiedit
