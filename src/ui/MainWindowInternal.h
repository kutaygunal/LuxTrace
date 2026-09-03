// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
// What MainWindow's translation units share.
//
// MainWindow is one class across several .cpp files -- the tabs it builds are
// independent areas of the program and were 4000 lines in one file, which is
// too many to hold at once and too many for two people to edit without
// colliding. Splitting the definitions rather than the class keeps the header,
// the signal wiring and every call site exactly as they were: this is a file
// boundary, not an interface change.
//
// What has to be shared is the include block -- each unit builds Qt widgets and
// touches the same core headers -- and the two file-local helpers that used to
// sit in an anonymous namespace. They are `inline`/`constexpr` here rather than
// in an anonymous namespace, so every unit sees the same one instead of a copy
// per file.
#include "MainWindow.h"

#include "ControlsPanel.h"
#include "HeatmapWidget.h"
#include "MultiEdit.h"
#include "ObjectInspector.h"
#include "SurfaceInspector.h"
#include "ObjectLibraryPanel.h"
#include "SceneTreePanel.h"
#include "OcctViewWidget.h"
#include "render/AppearanceView.h"
#include "PlotWidget.h"
#include "PolarPlotWidget.h"
#include "RayDiagramWidget.h"
#include "core/Analysis.h"
#include "core/CadImport.h"
#include "core/Report.h"
#include "render/AppearanceExport.h"
#include "EnergyBarWidget.h"
#include "ImportDialog.h"
#include "PythonPanel.h"
#include "core/ConfigIO.h"
#include "core/MaterialFile.h"
#include "core/SceneDocument.h"
#include "core/SimulationWorker.h"
#include "core/StudyWorker.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QScreen>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include <QToolButton>
#include <gp_Quaternion.hxx>

namespace mainwindow_detail {

// Geometry rebuilds run OCCT tessellation and a BVH build. They happen on a
// worker thread now, and that thread coalesces requests on its own, so this
// only has to stop a spin box drag from queueing a build per step -- it no
// longer has to hide a stall, and can be short enough that an edit looks
// immediate.
constexpr int kGeometryDebounceMs = 60;

inline QLabel* dim(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color:#8a8f9c;"));
    return l;
}

} // namespace mainwindow_detail

// Used unqualified throughout, as it was when it lived in an anonymous
// namespace in MainWindow.cpp.
using mainwindow_detail::dim;
using mainwindow_detail::kGeometryDebounceMs;
