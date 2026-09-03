// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QWidget>
#include <vector>
#include "core/Simulation.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QListWidget;
class QScrollArea;
class QSpinBox;

// Scene-wide controls: the dimensions of the loaded tutorial, the physics
// model, the ray budget, run/cancel and progress.
//
// What is *in* the scene, and what each object in it does, is not here any
// more: objects are chosen from the library, arranged in the scene tree and
// edited in the object inspector, which is where a per-object property belongs.
// What stays is everything that is a property of the run rather than of a part
// -- the physics switches, the ray count, the receiver grid, the flux unit.
//
// The parameter rows are built from GeometryProvider::paramInfo rather than
// hardcoded, so a tutorial that declares a new number gets an editor for it
// with no edit here.
class ControlsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ControlsPanel(QWidget* parent = nullptr);

    // As wide as the content, no wider. The column lives in a splitter, so this
    // is only the width the window opens at; the user sets it after that.
    QSize sizeHint() const override;
    // And never narrower than the content. The scroll area inside scrolls
    // vertically only, so a column narrower than its widest row slides sideways
    // -- to keep a newly focused widget in view -- with no horizontal scrollbar
    // to bring it back. Refusing the width is what stops the drift.
    QSize minimumSizeHint() const override;

    // Everything the panel itself sets. The source fields are left at their
    // defaults: emitters are objects now, and the window fills those in from
    // the scene document after asking for this.
    SimConfig config() const;
    void      setConfig(const SimConfig& cfg);

    // Which tutorial the dimension rows describe. Set by the window when a
    // tutorial is loaded; there is no scene picker here any more, because
    // choosing one is a File menu action rather than a property of the run.
    GeometryProvider::Scene scene() const { return m_scene; }
    void                    setScene(GeometryProvider::Scene scene);

    SceneParams params() const;
    void        setParams(const SceneParams& params);

    // Says that the dimensions below no longer describe what will be traced --
    // because a file was imported, or because the scene has been edited away
    // from what the tutorial builds. An empty reason hands them back.
    //
    // Leaving those spin boxes live was half of why an imported part looked
    // like it was being traced with the previous shape's dimensions, so they
    // are named as inactive rather than silently ignored.
    void setGeometryDetached(const QString& reason);

    // Freezes the inputs for the duration of a run and swaps Run for Cancel.
    // The window itself stays responsive -- the trace runs on a worker thread.
    void setRunning(bool running);
    void setProgress(int percent);
    void setStatus(const QString& text);

signals:
    void runRequested();
    void cancelRequested();
    // Geometry parameters changed; the 3D view needs rebuilding. Coalesced by
    // the window, because dragging a spin box fires this per step.
    void geometryChanged();
    // Anything that only affects the next trace (source, physics, ray count).
    void settingsChanged();

private:
    void rebuildParamRows();
    void syncEnabledState();

    QScrollArea*  m_scroll   = nullptr;
    QWidget*      m_body     = nullptr;
    GeometryProvider::Scene m_scene = GeometryProvider::Scene::Reflector;
    QLabel*       m_importNote = nullptr;
    // Set while something other than the tutorial decides the geometry, so a
    // run finishing does not quietly re-enable parameters it does not drive.
    bool          m_detached = false;
    QGroupBox*    m_paramBox = nullptr;
    QFormLayout*  m_paramForm = nullptr;
    std::vector<QDoubleSpinBox*> m_params;
    QPushButton*  m_resetParams = nullptr;

    QComboBox*      m_powerUnit  = nullptr;
    QComboBox*      m_detBins    = nullptr;
    QCheckBox*      m_coatings   = nullptr;
    QCheckBox*      m_volume     = nullptr;
    QCheckBox*      m_polarised  = nullptr;

    QCheckBox*      m_fresnel    = nullptr;
    QCheckBox*      m_absorption = nullptr;
    QCheckBox*      m_scattering = nullptr;
    QCheckBox*      m_roughness  = nullptr;
    QCheckBox*      m_dispersion = nullptr;
    QDoubleSpinBox* m_roughOverride = nullptr;
    QDoubleSpinBox* m_scatterOverride = nullptr;
    QDoubleSpinBox* m_absorptionScale = nullptr;

    QSpinBox*     m_rays     = nullptr;
    QSpinBox*     m_seed     = nullptr;
    QSpinBox*     m_threads  = nullptr;
    QPushButton*  m_run      = nullptr;
    QPushButton*  m_cancel   = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel*       m_status   = nullptr;

    // Set while setConfig / rebuildParamRows rewrite the widgets, so their
    // valueChanged signals do not read back as user edits.
    bool m_loading = false;
};
