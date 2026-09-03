// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QDialog>
#include <gp_Dir.hxx>

#include "core/CadImport.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QTreeWidget;

// The import dialog: read a customer's CAD, say what unit it is in, say which
// way the light goes through it, and see whether the tracer's assumptions
// actually hold on it before anything is traced.
//
// It used to be three QInputDialogs in a row -- a scale factor, an axis, and an
// information box afterwards -- and the scale factor was the only mechanism
// there was for units even though a STEP file states its own. Nothing checked
// shell closedness, free edges or degenerate faces, all of which the medium
// model depends on, so "it traced, so the model must be fine" was the only
// feedback anybody got.
//
// The dialog re-reads the file whenever the unit or the healing switch changes,
// because both change what the geometry *is*, and showing an audit of the
// previous read next to the new settings would be worse than showing none.
class ImportDialog : public QDialog {
    Q_OBJECT
public:
    ImportDialog(const QString& path, QWidget* parent = nullptr);

    const cadimport::ImportResult& result() const { return m_result; }
    gp_Dir  axis() const;
    QString axisName() const;
    QString materialName() const;
    cadimport::Finish finish() const;

private:
    void reread();
    void refreshTable();
    void refreshSummary();

    QString m_path;
    cadimport::ImportResult m_result;

    QLabel*      m_headline = nullptr;
    QComboBox*   m_unit     = nullptr;
    QDoubleSpinBox* m_customScale = nullptr;
    QComboBox*   m_axis     = nullptr;
    QComboBox*   m_material = nullptr;
    QComboBox*   m_finish = nullptr;
    QCheckBox*   m_heal     = nullptr;
    QTreeWidget* m_parts    = nullptr;
    QLabel*      m_verdict  = nullptr;
    QDialogButtonBox* m_buttons = nullptr;

    // Set while the unit combo is being rewritten, so its currentIndexChanged
    // does not read back as a user edit and re-read the file again.
    bool m_loading = false;
};
