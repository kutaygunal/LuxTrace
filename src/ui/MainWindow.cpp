// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "MainWindowInternal.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("LuxTrace \u2014 Optical Design Studio"));

    m_controls = new ControlsPanel(this);
    m_view3d   = new OcctViewWidget(this);
    m_diagram  = new RayDiagramWidget(this);
    m_heatmap  = new HeatmapWidget(this);
    m_worker   = new SimulationWorker(this);
    m_study    = new StudyWorker(this);
    m_geometry = new GeometryWorker(this);

    m_geometryTimer = new QTimer(this);
    m_geometryTimer->setSingleShot(true);
    m_geometryTimer->setInterval(kGeometryDebounceMs);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildViewerTab(),     QStringLiteral("3D View"));
    m_tabs->addTab(buildDiagramTab(),    QStringLiteral("Ray Diagram (X-Z)"));
    m_tabs->addTab(buildIrradianceTab(), QStringLiteral("Irradiance"));
    m_tabs->addTab(buildIntensityTab(),  QStringLiteral("Intensity"));
    m_tabs->addTab(buildStudiesTab(),    QStringLiteral("Studies"));
    m_tabs->addTab(buildDesignTab(),     QStringLiteral("Design"));
    m_tabs->addTab(buildToleranceTab(),  QStringLiteral("Tolerance"));
    m_tabs->addTab(buildProbeTab(),      QStringLiteral("Ray probe"));
    m_python = new PythonPanel(this);
    m_tabs->addTab(m_python,               QStringLiteral("Python"));
    // Last, and appended rather than slotted in: the tab indices above are
    // referred to by number elsewhere in this file, and a renderer that
    // renumbered the Studies tab would be a renderer that broke navigation.
    m_tabs->addTab(buildAppearanceTab(),   QStringLiteral("Appearance"));

    m_metrics = new QTextBrowser(this);
    m_metrics->setOpenExternalLinks(false);
    m_metrics->setMinimumHeight(150);
    m_metrics->setHtml(QStringLiteral("<i>Run a simulation to see the metrics.</i>"));

    // ---- left: what can be made, and what is in the scene -------------------
    //
    // Two panels, in the order the work goes: pick a thing from the library,
    // drop it, and the tree below is what it became.
    m_library   = new ObjectLibraryPanel(this);
    m_sceneTree = new SceneTreePanel(this);
    m_object    = new ObjectInspector(this);
    m_sceneTree->setDocument(&m_document);

    auto* leftCol = new QSplitter(Qt::Vertical, this);
    leftCol->addWidget(m_library);
    leftCol->addWidget(m_sceneTree);
    leftCol->setStretchFactor(0, 3);
    leftCol->setStretchFactor(1, 4);

    // ---- the views, and what the last run said under them --------------------
    m_result = new QLabel(QStringLiteral("Ready. Load a tutorial from the File menu, "
                                         "or drag an object in."), this);
    m_result->setWordWrap(true);
    m_result->setFrameShape(QFrame::StyledPanel);

    // Where the light went, drawn rather than printed. It sits with the metrics
    // because it is the answer to the question the Run button asks.
    m_energyBar = new EnergyBarWidget(this);

    auto* results = new QWidget(this);
    auto* resv    = new QVBoxLayout(results);
    resv->setContentsMargins(0, 0, 0, 0);
    resv->setSpacing(4);
    resv->addWidget(m_metrics, 1);
    resv->addWidget(m_energyBar, 0);
    resv->addWidget(m_result, 0);

    auto* centre = new QSplitter(Qt::Vertical, this);
    centre->addWidget(m_tabs);
    centre->addWidget(results);
    centre->setStretchFactor(0, 4);
    centre->setStretchFactor(1, 1);

    // ---- right: the selected object ------------------------------------------
    //
    // Whatever is selected -- a lens, a light source, a receiver, a whole group
    // -- is described here, beside the view it was picked in. What the *run*
    // does lives in its own window instead: a ray budget and a lens's radius of
    // curvature are not the same kind of setting, and putting them in one
    // column made the panel read as a form for the application rather than a
    // description of the thing under the cursor.

    // Both side columns are draggable against the viewport rather than pinned
    // at whatever width they were built with, and the 3D view is where the
    // window's growth goes.
    auto* columns = new QSplitter(Qt::Horizontal, this);
    columns->addWidget(leftCol);
    columns->addWidget(centre);
    columns->addWidget(m_object);
    columns->setStretchFactor(0, 0);
    columns->setStretchFactor(1, 1);
    columns->setStretchFactor(2, 0);
    columns->setChildrenCollapsible(false);
    columns->setSizes({300, 1000, m_object->sizeHint().width()});

    auto* central = new QWidget(this);
    auto* main = new QHBoxLayout(central);
    main->setContentsMargins(6, 6, 6, 6);
    main->addWidget(columns);
    setCentralWidget(central);

    buildSimulationWindow();
    buildMenus();
    statusBar()->showMessage(QStringLiteral("Ready"));

    connect(m_controls, &ControlsPanel::runRequested,    this, &MainWindow::onRun);
    connect(m_controls, &ControlsPanel::cancelRequested, this, &MainWindow::onCancel);
    connect(m_controls, &ControlsPanel::geometryChanged, this, &MainWindow::onGeometryChanged);
    // Adding, editing or removing a source changes where the light comes from
    // without changing a triangle, so the markers have to follow it. Nothing
    // was listening to this signal at all, which is why a source added to the
    // list stayed invisible until the next geometry rebuild happened to run.
    connect(m_controls, &ControlsPanel::settingsChanged, this,
            &MainWindow::refreshSourceGlyphs);
    connect(m_geometryTimer, &QTimer::timeout, this, &MainWindow::rebuildGeometryView);
    connect(m_geometry, &GeometryWorker::geometryReady,  this, &MainWindow::onGeometryReady);
    connect(m_geometry, &GeometryWorker::geometryFailed, this, &MainWindow::onGeometryFailed);

    connect(m_worker, &SimulationWorker::progress,    this, &MainWindow::onProgress);
    connect(m_worker, &SimulationWorker::resultReady, this, &MainWindow::onResult);
    connect(m_worker, &SimulationWorker::partialReady, this, &MainWindow::onPartial);
    connect(m_study,  &StudyWorker::progress,         this, &MainWindow::onProgress);
    connect(m_study,  &StudyWorker::convergenceReady, this, &MainWindow::onConvergenceReady);
    connect(m_study,  &StudyWorker::sweepReady,        this, &MainWindow::onSweepReady);
    connect(m_study,  &StudyWorker::optimisationReady, this, &MainWindow::onOptimisationReady);
    connect(m_study,  &StudyWorker::toleranceReady,    this, &MainWindow::onToleranceReady);

    connect(m_view3d, &OcctViewWidget::surfacePicked, this, &MainWindow::onSurfacePicked);
    connect(m_view3d, &OcctViewWidget::objectDropped, this, &MainWindow::onObjectDropped);
    connect(m_view3d, &OcctViewWidget::sourcePicked,  this, &MainWindow::onSourcePicked);
    connect(m_view3d, &OcctViewWidget::deleteRequested, this, &MainWindow::onViewportDelete);

    // ---- the scene document -------------------------------------------------
    connect(m_library, &ObjectLibraryPanel::createRequested, this,
            [this](scenedoc::ObjectType type) { onCreateObject(type, 0); });

    connect(m_sceneTree, &SceneTreePanel::selectionChanged,    this, &MainWindow::onObjectSelected);
    connect(m_sceneTree, &SceneTreePanel::selectionSetChanged, this, &MainWindow::onSelectionSetChanged);
    connect(m_sceneTree, &SceneTreePanel::visibilityChanged,   this, &MainWindow::onObjectVisibility);
    connect(m_sceneTree, &SceneTreePanel::renamed,             this, &MainWindow::onObjectRenamed);
    connect(m_sceneTree, &SceneTreePanel::deleteRequested,     this, &MainWindow::onObjectDelete);
    connect(m_sceneTree, &SceneTreePanel::duplicateRequested,  this, &MainWindow::onObjectDuplicate);
    connect(m_sceneTree, &SceneTreePanel::reparentRequested,   this, &MainWindow::onObjectReparent);
    connect(m_sceneTree, &SceneTreePanel::createRequested,     this, &MainWindow::onCreateObject);
    connect(m_sceneTree, &SceneTreePanel::groupRequested,      this, &MainWindow::onAddGroup);
    connect(m_sceneTree, &SceneTreePanel::isolateRequested,    this, &MainWindow::onIsolateObject);
    connect(m_sceneTree, &SceneTreePanel::showAllRequested,    this, &MainWindow::onShowAllObjects);

    connect(m_object, &ObjectInspector::nameEdited, this, [this](int id) {
        InspectorEditScope editing(m_inspectorEditing);
        onObjectRenamed(id, m_object->object().name);
    });
    connect(m_object, &ObjectInspector::placementEdited,      this, &MainWindow::onObjectPlacementEdited);
    connect(m_object, &ObjectInspector::parametersEdited,     this, &MainWindow::onObjectParametersEdited);
    connect(m_object, &ObjectInspector::sourceEdited,         this, &MainWindow::onObjectSourceEdited);
    connect(m_object, &ObjectInspector::opticsEdited,         this, &MainWindow::onObjectOpticsEdited);
    connect(m_object, &ObjectInspector::opticsResetRequested, this, &MainWindow::onObjectOpticsReset);
    connect(m_object, &ObjectInspector::multiPlacementEdited,  this, &MainWindow::onMultiPlacementEdited);
    connect(m_object, &ObjectInspector::multiParametersEdited, this, &MainWindow::onMultiParametersEdited);
    connect(m_object, &ObjectInspector::multiSourceEdited,     this, &MainWindow::onMultiSourceEdited);
    connect(m_object, &ObjectInspector::multiOpticsEdited,     this, &MainWindow::onMultiOpticsEdited);
    connect(m_heatmap, &HeatmapWidget::cutMoved, this, &MainWindow::onCutMoved);

    // Hand the viewport keyboard focus when its tab comes up, so WASD works
    // without having to click into it first.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) m_view3d->setFocus(Qt::OtherFocusReason);
        // The Appearance view runs on the viewport's own graphic driver rather
        // than starting a second one. The driver is created on the viewport's
        // first paint, so it is handed over here -- by which time the 3D tab
        // has certainly been shown -- and not at construction, when it does not
        // exist yet.
        if (m_appearance && m_tabs->widget(index) &&
            m_tabs->widget(index)->isAncestorOf(m_appearance)) {
            m_appearance->setSharedDriver(m_view3d->graphicDriver());
            refreshAppearanceSurfaces();
            m_appearance->setSources(appearanceSources());
        }
    });

    refreshSweepAxes();
    refreshDerivedQuantities();
    // Something to look at before anything is chosen. The first tutorial in the
    // registry is the parabolic collimator, which is the one that explains what
    // the application is for in one picture.
    loadTutorial(GeometryProvider::Scene::Reflector);


}

// ---- tab construction ------------------------------------------------------

QWidget* MainWindow::buildViewerTab() {
    auto* fit         = new QPushButton(QStringLiteral("Fit (F)"), this);
    auto* reset       = new QPushButton(QStringLiteral("Reset (R)"), this);
    auto* showRays    = new QCheckBox(QStringLiteral("Show rays"), this);
    auto* perspective = new QCheckBox(QStringLiteral("Perspective"), this);
    showRays->setChecked(true);
    // Off to begin with: the orthographic CAD projection is what a part is read
    // in, and it is the projection the viewport starts up in. Perspective is
    // opted into, for walking through the optic.
    perspective->setChecked(false);
    perspective->setToolTip(QStringLiteral(
        "Perspective is needed to walk through the optic; leave it off for the "
        "familiar orthographic CAD projection."));

    m_rayColor = new QComboBox(this);
    m_rayColor->addItems({QStringLiteral("Uniform"), QStringLiteral("Energy"),
                          QStringLiteral("Bounces"), QStringLiteral("Wavelength")});
    m_rayColor->setCurrentIndex(1);
    m_rayColor->setToolTip(QStringLiteral(
        "Energy shows where the light is being lost; bounces show where a path "
        "gets trapped; wavelength colours a spectral run for real."));

    m_detectorOnly = new QCheckBox(QStringLiteral("Only rays reaching the receiver"), this);
    m_detectorOnly->setToolTip(QStringLiteral(
        "Hides every path that ended absorbed or escaped. On a low-efficiency "
        "scene this is the difference between a fog and an answer."));

    m_clipOn     = new QCheckBox(QStringLiteral("Section"), this);
    m_clipAxis   = new QComboBox(this);
    m_clipAxis->addItems({QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")});
    m_clipSlider = new QSlider(Qt::Horizontal, this);
    m_clipSlider->setRange(0, 1000);
    m_clipSlider->setValue(500);
    m_clipSlider->setMaximumWidth(160);
    m_clipFlip   = new QCheckBox(QStringLiteral("Flip"), this);
    m_clipOn->setToolTip(QStringLiteral(
        "Slice the optic open with a clipping plane, so the TIR bounces inside a "
        "guide can be watched from outside it."));

    auto* viewCube = new QCheckBox(QStringLiteral("Nav cube"), this);
    viewCube->setChecked(true);
    viewCube->setToolTip(QStringLiteral(
        "The navigation cube in the lower-left corner, in place of the plain "
        "axis trihedron. Click a face for a standard view, an edge for a "
        "45-degree one, a corner for an isometric. It turns the camera only -- "
        "the zoom and the pan stay where you put them, and F is still what "
        "frames the scene. Switch it off to get the plain axes back."));

    // The transform tools. One at a time and each one un-checkable, so clicking
    // the active tool puts the gizmo away -- which is the same thing Escape
    // does, and is needed because the handles sit over the object they move.
    auto tool = [this](const QString& label, const QString& key, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(label);
        b->setCheckable(true);
        b->setToolTip(QStringLiteral("%1 the selected object  (press %2)  —  %3")
                          .arg(label, key, tip));
        return b;
    };
    m_moveTool = tool(QStringLiteral("Move"), QStringLiteral("2"),
                      QStringLiteral("Drag an arrow to slide the object along that axis. "
                                     "Press 1 to put the gizmo away."));
    m_rotateTool = tool(QStringLiteral("Rotate"), QStringLiteral("3"),
                        QStringLiteral("Drag a ring to turn the object about that axis. "
                                       "Press 1 to put the gizmo away."));
    m_scaleTool = tool(QStringLiteral("Scale"), QStringLiteral("4"),
                       QStringLiteral(
                           "Drag a handle to resize the object about its own origin. "
                           "The scale is uniform: an optic stretched along one axis "
                           "would be a different optic, not the same one at another "
                           "size. To change a lens in one direction only, edit its "
                           "dimensions in the object panel."));

    auto* bar1 = new QHBoxLayout;
    bar1->setContentsMargins(4, 2, 4, 0);
    bar1->addWidget(m_moveTool);
    bar1->addWidget(m_rotateTool);
    bar1->addWidget(m_scaleTool);
    bar1->addSpacing(10);
    bar1->addWidget(fit);
    bar1->addWidget(reset);
    bar1->addWidget(showRays);
    bar1->addWidget(perspective);
    bar1->addWidget(viewCube);
    bar1->addStretch(1);

    auto* bar2 = new QHBoxLayout;
    bar2->setContentsMargins(4, 0, 4, 2);
    bar2->addWidget(m_clipOn);
    bar2->addWidget(m_clipAxis);
    bar2->addWidget(m_clipSlider);
    bar2->addWidget(m_clipFlip);
    bar2->addSpacing(12);
    bar2->addWidget(dim(QStringLiteral("Colour:"), this));
    bar2->addWidget(m_rayColor);
    bar2->addWidget(m_detectorOnly);
    bar2->addSpacing(12);
    auto* help = new QLabel(QStringLiteral("Drag: L orbit · M pan · R look | Wheel zoom | "
                                       "click a surface to inspect it · 2/3/4 move, "
                                       "rotate, scale it · 1 drops the gizmo · "
                                       "WASD walk · F fits · R resets the view"), this);
    help->setStyleSheet(QStringLiteral("color:#8a8f9c;"));
    help->setWordWrap(true);
    bar2->addWidget(help, 1);

    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    v->addWidget(m_view3d, 1);
    v->addLayout(bar1, 0);
    v->addLayout(bar2, 0);

    connect(fit,   &QPushButton::clicked, m_view3d, &OcctViewWidget::fitAll);
    connect(reset, &QPushButton::clicked, m_view3d, &OcctViewWidget::resetView);

    using TM = OcctViewWidget::TransformMode;
    const std::pair<QToolButton*, TM> tools[] = {
        {m_moveTool, TM::Translate}, {m_rotateTool, TM::Rotate}, {m_scaleTool, TM::Scale}};
    for (const auto& [button, mode] : tools)
        connect(button, &QToolButton::clicked, this, [this, mode] {
            // Clicking the tool that is already active turns it off, so the
            // gizmo is never in the way of the object it belongs to.
            m_view3d->setTransformMode(m_view3d->transformMode() == mode ? TM::None : mode);
        });
    connect(m_view3d, &OcctViewWidget::transformModeChanged,
            this, &MainWindow::onTransformModeChanged);
    connect(m_view3d, &OcctViewWidget::objectTransformed,
            this, &MainWindow::onObjectTransformed);
    connect(showRays, &QCheckBox::toggled, m_view3d, &OcctViewWidget::setRaysVisible);
    connect(perspective, &QCheckBox::toggled, m_view3d, &OcctViewWidget::setPerspective);
    connect(viewCube, &QCheckBox::toggled, m_view3d, &OcctViewWidget::setViewCubeVisible);
    connect(m_rayColor, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_view3d->setRayColorMode(OcctViewWidget::RayColor(i));
        m_diagram->setColorMode(RayDiagramWidget::ColorMode(i));
    });
    connect(m_detectorOnly, &QCheckBox::toggled, this, [this](bool on) {
        m_view3d->setDetectorRaysOnly(on);
        m_diagram->setDetectorOnly(on);
    });
    connect(m_clipOn, &QCheckBox::toggled, m_view3d, &OcctViewWidget::setClipEnabled);
    connect(m_clipAxis, &QComboBox::currentIndexChanged, m_view3d, &OcctViewWidget::setClipAxis);
    connect(m_clipFlip, &QCheckBox::toggled, m_view3d, &OcctViewWidget::setClipFlipped);
    connect(m_clipSlider, &QSlider::valueChanged, this, [this](int v) {
        m_view3d->setClipPosition(double(v) / 1000.0);
    });

    m_view3d->setRayColorMode(OcctViewWidget::RayColor::Energy);
    m_diagram->setColorMode(RayDiagramWidget::ColorMode::Energy);
    return pane;
}

QWidget* MainWindow::buildDiagramTab() {
    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_diagram, 1);
    v->addWidget(dim(QStringLiteral("Colour mode and the receiver-path filter are shared "
                                    "with the 3D view."), pane), 0);
    return pane;
}

QWidget* MainWindow::buildIrradianceTab() {
    m_colormap = new QComboBox(this);
    m_colormap->addItems(palette::names());
    m_scale = new QComboBox(this);
    m_scale->addItems(palette::scaleNames());
    m_scale->setToolTip(QStringLiteral(
        "A log scale reveals the stray light two or three decades below the beam "
        "that a linear map crushes to black."));
    m_rgbMode = new QCheckBox(QStringLiteral("True colour (RGB run)"), this);
    m_rgbMode->setToolTip(QStringLiteral(
        "Available once the source is set to RGB: paints the three traced bands "
        "as red, green and blue instead of a false-colour map."));
    m_rgbMode->setEnabled(false);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(dim(QStringLiteral("Colormap:"), this));
    bar->addWidget(m_colormap);
    bar->addWidget(dim(QStringLiteral("Scale:"), this));
    bar->addWidget(m_scale);
    bar->addWidget(m_rgbMode);
    bar->addStretch(1);
    bar->addWidget(dim(QStringLiteral("Click the map to move the profile cut"), this));

    m_profile = new PlotWidget(this);
    m_profile->setTitle(QStringLiteral("Cross-section through the cut"));
    m_profile->setAxisLabels(QStringLiteral("mm"), QStringLiteral("W/mm²"));
    m_profile->setPlaceholder(QStringLiteral("Profile through the receiver"));

    m_encircled = new PlotWidget(this);
    m_encircled->setTitle(QStringLiteral("Encircled energy"));
    m_encircled->setAxisLabels(QStringLiteral("radius [mm]"), QStringLiteral("fraction"));
    m_encircled->setPlaceholder(QStringLiteral("Encircled energy about the centroid"));

    auto* plots = new QSplitter(Qt::Horizontal, this);
    plots->addWidget(m_profile);
    plots->addWidget(m_encircled);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->addWidget(m_heatmap);
    split->addWidget(plots);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->addLayout(bar, 0);
    v->addWidget(split, 1);

    connect(m_colormap, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_heatmap->setColormap(palette::Map(i));
        m_polar->setColormap(palette::Map(i));
    });
    connect(m_scale, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_heatmap->setScale(palette::Scale(i));
        m_polar->setScale(palette::Scale(i));
    });
    connect(m_rgbMode, &QCheckBox::toggled, m_heatmap, &HeatmapWidget::setRgbMode);
    return pane;
}

QWidget* MainWindow::buildIntensityTab() {
    m_polar = new PolarPlotWidget(this);

    m_polarMode = new QComboBox(this);
    m_polarMode->addItems({QStringLiteral("Polar curve"), QStringLiteral("Angular map")});
    m_polarForward = new QCheckBox(QStringLiteral("Forward hemisphere only"), this);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(dim(QStringLiteral("View:"), this));
    bar->addWidget(m_polarMode);
    bar->addWidget(m_polarForward);
    bar->addStretch(1);
    bar->addWidget(dim(QStringLiteral("Flux per steradian, binned by the direction rays "
                                      "leave the system on"), this));

    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->addLayout(bar, 0);
    v->addWidget(m_polar, 1);

    connect(m_polarMode, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_polar->setMode(i == 1 ? PolarPlotWidget::Mode::Map : PolarPlotWidget::Mode::Polar);
    });
    connect(m_polarForward, &QCheckBox::toggled, m_polar, &PolarPlotWidget::setForwardOnly);
    return pane;
}


// ---- design tab -------------------------------------------------------------
// Sweeps, optimisation, the numbers the parameters imply, and the optics of
// whichever surface was last clicked in the viewport. Everything that turns the
// app from something that answers "what does this do" into something that
// answers "what should I build".


// ---- sweeps and optimisation ------------------------------------------------


// ---- comparing two runs -----------------------------------------------------


// Back to a scene that has not been traced yet.
//
// Every view that shows a result is emptied -- the paths in the viewport, the
// receiver map, the profiles, the far field, the studies and the metrics -- and
// nothing that describes the optic is touched. The objects stay where they are,
// so do their dimensions and materials, and so does the camera: the whole point
// of clearing the rays is usually to look at the geometry underneath them from
// where you were already standing.
void MainWindow::onResetResults() {
    // A run still writing into these would put its rays straight back.
    if (m_worker->isRunning() || m_study->isRunning()) {
        statusBar()->showMessage(
            QStringLiteral("A run is in progress -- cancel it first (Esc)"), 4000);
        return;
    }

    const SimulationResult empty;

    m_last      = empty;
    m_hasResult = false;
    m_hasPinned = false;
    m_pinnedLabel.clear();

    m_view3d->setRays({});
    if (m_appearance) m_appearance->clearResult();
    refreshAppearanceReceivers();
    m_diagram->setResult(empty);
    m_heatmap->setResult(empty);
    m_heatmap->clearReference();
    m_polar->setResult(empty);
    if (m_energyBar) m_energyBar->setResult(empty);

    // refreshDerivedViews() is a no-op without a result, so the plots it fills
    // are cleared here rather than left showing the run that has just gone.
    m_profile->clear();
    m_encircled->clear();
    m_mtfPlot->clear();

    // The studies are traces too, and a stale yield histogram beside a cleared
    // receiver map is the same lie the rays were.
    m_convergencePoints.clear();
    m_focusStudy = studies::FocusStudy{};
    m_sweepPoints.clear();
    m_sweepSlot = -1;
    m_toleranceStudy = studies::ToleranceStudy{};
    m_optimisation   = studies::OptimisationResult{};
    m_optimisedSlots.clear();
    m_convergence->clear();
    m_focus->clear();
    m_sweepPlot->clear();
    m_optPlot->clear();
    m_tolPlot->clear();
    m_optSummary->setHtml(QStringLiteral("<i>No search has been run yet.</i>"));
    m_tolSummary->setHtml(QStringLiteral("<i>No tolerance study has been run yet.</i>"));
    m_adoptButton->setEnabled(false);

    m_rgbMode->setChecked(false);
    m_rgbMode->setEnabled(false);

    m_rayLog->setHtml(QStringLiteral("<i>Trace one ray to see every interaction "
                                     "it had.</i>"));

    m_result->setText(QStringLiteral("Results cleared. The scene is as you left "
                                     "it -- run again when you are ready."));
    m_metrics->setHtml(formatMetrics());

    statusBar()->showMessage(QStringLiteral("Results cleared -- the scene and the "
                                            "view are unchanged"), 4000);
}

// ---- editing the optics of the surface that was clicked ---------------------

const std::vector<OpticalSurface>& MainWindow::currentSurfaces() const {
    static const std::vector<OpticalSurface> kEmpty;
    return m_sceneData ? m_sceneData->surfaces : kEmpty;
}

// ---- the surfaces tab ------------------------------------------------------
//
// Two things that were missing, side by side.
//
// A scene tree, because picking through the 3D view alone cannot reach an
// internal surface at all: the far wall of a light guide, a lens's second face,
// anything behind anything. And because an edited surface used to be
// indistinguishable from an untouched one.
//
// The inspector, because SurfaceOptics carries roughly fifteen properties and
// the editor exposed four -- so nearly every accuracy feature in the engine
// existed and could not be driven from the application.

QWidget* MainWindow::buildProbeTab() {
    auto* page  = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(6, 6, 6, 6);

    // What a surface *is* moved to the object inspector with everything else
    // about an object; what is left here is the one thing that is not a
    // property of anything -- a single ray, and what happened to it.
    auto* probeBox  = new QGroupBox(QStringLiteral("Single ray"), page);
    auto* probeForm = new QFormLayout(probeBox);
    probeBox->setToolTip(QStringLiteral(
        "Fires one ray through the optic with the estimators switched off, so "
        "the whole path tree is visible, and lists every interaction it had: "
        "surface, angle of incidence, index pair, R and T, energy remaining and "
        "cumulative optical path. This is the debugging tool every commercial "
        "tracer has, over machinery that was already finished."));

    m_probeX = new QDoubleSpinBox(probeBox);
    m_probeX->setRange(-10000.0, 10000.0);
    m_probeX->setDecimals(3);
    m_probeX->setSuffix(QStringLiteral(" mm"));
    m_probeY = new QDoubleSpinBox(probeBox);
    m_probeY->setRange(-10000.0, 10000.0);
    m_probeY->setDecimals(3);
    m_probeY->setSuffix(QStringLiteral(" mm"));
    for (QDoubleSpinBox* sp : {m_probeX, m_probeY})
        sp->setToolTip(QStringLiteral(
            "Where the ray leaves the source plane, across the optical axis. "
            "The direction is the scene's own emission axis."));

    m_inspectRay = new QPushButton(QStringLiteral("Trace one ray"), probeBox);

    probeForm->addRow(QStringLiteral("Offset x:"), m_probeX);
    probeForm->addRow(QStringLiteral("Offset y:"), m_probeY);
    probeForm->addRow(m_inspectRay);
    outer->addWidget(probeBox, 0);

    m_rayLog = new QTextBrowser(page);
    m_rayLog->setMinimumHeight(140);
    m_rayLog->setHtml(QStringLiteral("<i>Trace one ray to see every interaction "
                                     "it had.</i>"));
    outer->addWidget(m_rayLog, 1);

    connect(m_inspectRay, &QPushButton::clicked, this, &MainWindow::onInspectRay);
    return page;
}

// ---- the scene document ----------------------------------------------------


// ---- what the panels ask the document for ----------------------------------


// ---- the transform gizmo ----------------------------------------------------


// ---- the single-ray inspector ----------------------------------------------

void MainWindow::onInspectRay() {
    if (!m_rayLog) return;
    const SimConfig cfg = currentConfig();
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    if (!data) {
        m_rayLog->setHtml(QStringLiteral("<i>No geometry to trace through.</i>"));
        return;
    }

    // From the scene's own emitter, offset across the axis, along the axis it
    // aims. One ray, no estimators, the whole path tree.
    const gp_Dir axis = data->sourceAxis;
    gp_Vec up(0, 0, 1);
    if (std::fabs(axis.Dot(gp_Dir(0, 0, 1))) > 0.99) up = gp_Vec(1, 0, 0);
    gp_Vec ex = gp_Vec(axis).Crossed(up);
    if (ex.Magnitude() < 1e-9) ex = gp_Vec(1, 0, 0);
    ex.Normalize();
    gp_Vec ey = gp_Vec(axis).Crossed(ex);
    if (ey.Magnitude() > 1e-9) ey.Normalize();

    const gp_Pnt o0 = data->sourceOrigin;
    const Vec3 origin(o0.X() + ex.X() * m_probeX->value() + ey.X() * m_probeY->value(),
                      o0.Y() + ex.Y() * m_probeX->value() + ey.Y() * m_probeY->value(),
                      o0.Z() + ex.Z() * m_probeX->value() + ey.Z() * m_probeY->value());

    SimulationResult single;
    RayTracer::traceSingleRay(data->scene, origin, Vec3(axis), single, 1.0,
                              cfg.physics, cfg.spectrum.wavelengthNm);

    QString html;
    html += QStringLiteral(
        "<p style='color:#7a7a7a'>One ray from (%1, %2, %3) along (%4, %5, %6), "
        "%7 nm. Russian roulette and branch collapsing are off, so this is the "
        "whole path tree rather than one draw from it.</p>")
                .arg(origin.x, 0, 'f', 2).arg(origin.y, 0, 'f', 2).arg(origin.z, 0, 'f', 2)
                .arg(axis.X(), 0, 'f', 3).arg(axis.Y(), 0, 'f', 3).arg(axis.Z(), 0, 'f', 3)
                .arg(cfg.spectrum.wavelengthNm, 0, 'f', 1);

    if (single.interactions.empty()) {
        html += QStringLiteral("<p><b>The ray hit nothing.</b> It left the scene "
                               "without meeting a surface.</p>");
        m_rayLog->setHtml(html);
        return;
    }

    html += QStringLiteral(
        "<table cellspacing='0' cellpadding='3' style='font-size:11px'>"
        "<tr style='color:#8a8a8a'><th align='left'>#</th><th align='left'>surface</th>"
        "<th align='right'>angle</th><th align='right'>n1&rarr;n2</th>"
        "<th align='right'>R</th><th align='right'>T</th>"
        "<th align='right'>energy in</th><th align='right'>OPL</th>"
        "<th align='left'>note</th></tr>");

    int row = 0;
    for (const RayInteraction& in : single.interactions) {
        QStringList notes;
        if (in.detector)  notes << QStringLiteral("<b>receiver</b>");
        if (in.tir)       notes << QStringLiteral("TIR");
        if (in.scattered) notes << QStringLiteral("scattered");
        if (in.mediumDepth > 0)
            notes << QStringLiteral("inside %1 medium(s)").arg(in.mediumDepth);

        html += QStringLiteral("<tr>"
                               "<td>%1</td><td>%2</td>"
                               "<td align='right'>%3&deg;</td>"
                               "<td align='right'>%4 &rarr; %5</td>"
                               "<td align='right'>%6</td><td align='right'>%7</td>"
                               "<td align='right'>%8</td><td align='right'>%9 mm</td>"
                               "<td>%10</td></tr>")
                    .arg(++row)
                    .arg(in.label.isEmpty() ? QStringLiteral("surface %1").arg(in.surface)
                                            : in.label.toHtmlEscaped())
                    .arg(in.angleDeg, 0, 'f', 2)
                    .arg(in.n1, 0, 'f', 4).arg(in.n2, 0, 'f', 4)
                    .arg(in.detector ? QStringLiteral("-")
                                     : QString::number(in.reflectance, 'f', 4))
                    .arg(in.detector ? QStringLiteral("-")
                                     : QString::number(in.transmittance, 'f', 4))
                    .arg(in.energyIn, 0, 'g', 4)
                    .arg(in.opl, 0, 'f', 2)
                    .arg(notes.join(QStringLiteral(", ")));
    }
    html += QStringLiteral("</table>");
    html += QStringLiteral(
        "<p style='color:#7a7a7a'>Delivered %1 of the ray to the receiver; "
        "%2 absorbed, %3 escaped, %4 truncated.</p>")
                .arg(single.fluxDetector, 0, 'f', 5)
                .arg(single.fluxAbsorbed, 0, 'f', 5)
                .arg(single.fluxEscaped, 0, 'f', 5)
                .arg(single.fluxTruncated, 0, 'f', 5);

    m_rayLog->setHtml(html);
    // The path this produced is worth looking at as well as reading.
    m_view3d->setRays(single.raySegments);
}


SimConfig MainWindow::currentConfig() const {
    SimConfig cfg = m_controls->config();

    // What is on screen is what gets traced, and there are two ways for that to
    // be true.
    //
    // While the document is still exactly what a tutorial builds, the geometry
    // comes from the registry at the dimensions in the panel -- which is what
    // leaves the sweeps, the optimiser and the tolerance study with numbers to
    // vary -- and any optical edit reaches the trace as an override.
    //
    // Once it has been assembled or imported, there is no parameter set that
    // describes it, so the compiled scene is handed over whole through the same
    // field a CAD import has always used.
    if (m_document.linkedToTutorial()) {
        cfg.surfaceOverrides = overridesFromDocument();
    } else {
        cfg.imported = m_compiled.setup;
    }

    // The emitters are objects, wherever the geometry came from.
    if (m_compiled.havePrimary) {
        const SourceSpec& s      = m_compiled.primary;
        cfg.source               = s.type;
        cfg.shape                = s.shape;
        cfg.spectrum             = s.spectrum;
        cfg.halfAngleDeg         = s.halfAngleDeg;
        cfg.sizeA                = s.sizeA;
        cfg.sizeB                = s.sizeB;
        cfg.beamRadius           = s.beamRadius;
        cfg.power                = s.power;
        cfg.polarisationState    = s.polarisationState;
        cfg.rayFile              = s.rayFile;
        cfg.rayFileScale         = s.rayFileScale;
        cfg.rayFileWavelengths   = s.rayFileWavelengths;
    }
    cfg.extraSources = m_compiled.extraSources;
    return cfg;
}

bool MainWindow::requireLightSource() {
    if (m_compiled.havePrimary) return true;
    QMessageBox::information(
        this, windowTitle(),
        m_document.sourceCount() > 0
            ? QStringLiteral("Every light source in the scene is switched off, so there "
                             "is nothing to trace.\n\nTick one in the scene tree, or add "
                             "another from the object library.")
            : QStringLiteral("The scene has no light source, so there is nothing to "
                             "trace.\n\nDrag one in from the object library."));
    return false;
}

bool MainWindow::requireParametricScene(const QString& what) {
    if (m_document.linkedToTutorial()) return true;
    QMessageBox::information(
        this, windowTitle(),
        QStringLiteral("%1 varies the scene's own dimensions, and an assembled scene "
                       "has none: what is on screen is a set of placed objects, not a "
                       "parametric optic.\n\n"
                       "Trace it, sweep the ray count, or edit any object — those all "
                       "apply. To sweep a dimension, load a tutorial from File > "
                       "Tutorials, which replaces the assembled scene.").arg(what));
    return false;
}


// ---- tolerance tab ----------------------------------------------------------
// The question a manufacturer actually asks. Not "how good is the nominal
// design" but "what fraction of production will pass", and "which dimension do
// I tighten to change that".


// Everything about the run rather than about a part, in a window of its own.
//
// It used to be the right-hand column, sharing a strip with nothing else, and
// that put "how many rays" beside "what is this lens made of" as though they
// were the same kind of question. They are not: one describes the experiment
// and the other describes a thing in it. So the column became the property
// sheet for whatever is selected, and the experiment moved here -- a window the
// user can leave open beside the viewport, park on a second screen, or close
// entirely and drive from the Run menu.
void MainWindow::buildSimulationWindow() {
    m_simWindow = new QDialog(this);
    m_simWindow->setWindowTitle(QStringLiteral("Simulation — LuxTrace"));
    // A window, not a dialog box: it is not asking anything, and it has to be
    // possible to leave it open, move it aside and keep working in the viewport.
    m_simWindow->setWindowFlag(Qt::Window, true);
    m_simWindow->setModal(false);
    m_simWindow->setSizeGripEnabled(true);

    // What the dimensions above imply, live, before anything is traced. This is
    // the difference between a form and an instrument, and it belongs with the
    // dimensions it is derived from.
    m_derived = new QLabel(m_simWindow);
    m_derived->setWordWrap(true);
    m_derived->setStyleSheet(QStringLiteral("color:#9aa0b0; font-size:11px;"));
    m_derived->setVisible(false);

    auto* v = new QVBoxLayout(m_simWindow);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(6);
    v->addWidget(m_controls, 1);
    v->addWidget(m_derived, 0);

    // Tall enough that the Run button is on screen without scrolling to it --
    // clamped to the desktop when it opens, which is where the screen is known.
    m_simWindow->resize(m_controls->sizeHint().width() + 32,
                        m_controls->sizeHint().height() + 120);
}

void MainWindow::onShowSimulationWindow() {
    if (!m_simWindow) return;

    // Placed the first time it is opened, not before: where it belongs depends
    // on where the main window ended up, which is not settled while the window
    // is still being built.
    if (!m_simWindowPlaced) {
        m_simWindowPlaced = true;
        QRect where(frameGeometry().right() + 8, frameGeometry().top(),
                    m_simWindow->width(), m_simWindow->height());
        if (QScreen* onScreen = screen()) {
            const QRect avail = onScreen->availableGeometry();
            // Nowhere to put it beside the window -- a maximised main window
            // leaves no "beside" -- so it goes over the viewport rather than
            // over the property column it was just moved out of.
            if (where.right() > avail.right()) {
                constexpr int kPropertyColumn = 420;
                where.moveRight(std::max(avail.left() + where.width(),
                                         frameGeometry().right() - kPropertyColumn));
            }
            // setGeometry positions the *client* area, so a window placed flush
            // with the top of a maximised main window puts its own title bar
            // above the desktop -- where it cannot be grabbed and the window
            // cannot be moved.
            constexpr int kTitleBar = 36;
            where.moveTop(std::max(where.top(), avail.top() + kTitleBar));
            // As tall as the desktop allows, rather than as tall as the panel
            // asks for: the panel wraps a scroll area, whose size hint is a
            // default and not the height of what is inside it, so sizing to it
            // opened a window with the Run button below the fold.
            where.setHeight(std::max(420, avail.bottom() - where.top() - 8));
        }
        m_simWindow->setGeometry(where);
    }

    m_simWindow->show();
    m_simWindow->raise();
    m_simWindow->activateWindow();
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));

    // The twenty-eight built-in scenes, as tutorials.
    //
    // They were a combo box at the top of the controls column, which put
    // "which optic am I learning about" in the same place as "how many rays",
    // and made the scene look like a setting of the run rather than the thing
    // being designed. Under File they are what they are: worked examples to
    // open, each of which becomes an ordinary editable scene the moment it
    // lands.
    //
    // The groups follow the registry's own order. Naming the first scene of
    // each group rather than an index means inserting a scene into a group
    // needs no edit here.
    struct TutorialGroup { const char* name; GeometryProvider::Scene first; };
    static const TutorialGroup kGroups[] = {
        {"&Reflective",                 GeometryProvider::Scene::Reflector},
        {"Re&fractive",                 GeometryProvider::Scene::Lens},
        {"&Total internal reflection",  GeometryProvider::Scene::LightGuide},
        {"&Scattering",                 GeometryProvider::Scene::IntegratingSphere},
        {"&Multi-source",               GeometryProvider::Scene::LedArrayLuminaire},
        {"S&howcase",                   GeometryProvider::Scene::ShowcaseLuminaire},
    };

    file->addAction(QStringLiteral("&Save configuration..."), QKeySequence::Save,
                    this, &MainWindow::onSaveConfig);
    file->addAction(QStringLiteral("&Open configuration..."), QKeySequence::Open,
                    this, &MainWindow::onLoadConfig);
    file->addSeparator();

    // Everything that crosses the application boundary, in one segment and two
    // submenus. Six export entries and two import ones spelled out at the top
    // level made the menu a wall of near-identical sentences, and left "read a
    // glass catalogue" three separators away from "read a STEP file".
    QMenu* importMenu = file->addMenu(QStringLiteral("&Import"));
    importMenu->setStatusTip(QStringLiteral(
        "Read geometry or material data produced somewhere else."));
    importMenu->addAction(QStringLiteral("Import &CAD (STEP / IGES)..."),
                          this, &MainWindow::onImportCad)
        ->setStatusTip(QStringLiteral(
            "Reads a customer's geometry. The whole pipeline downstream already "
            "works on it; only the reader was missing."));
    importMenu->addAction(QStringLiteral("Load &material catalogue..."), this,
                          &MainWindow::onLoadMaterialCatalogue)
        ->setStatusTip(QStringLiteral(
            "Read a Zemax .agf glass catalogue -- the format Schott, Ohara, CDGM, "
            "Hoya and Sumita all publish in -- or a refractiveindex.info .yml "
            "entry. Ten built-in materials is a demonstration; a catalogue is a "
            "tool."));

    QMenu* exportMenu = file->addMenu(QStringLiteral("&Export"));
    exportMenu->setStatusTip(QStringLiteral(
        "Write the run out in the format whoever asked for it reads."));
    exportMenu->addAction(QStringLiteral("Export &irradiance grid (CSV)..."),
                          this, &MainWindow::onExportIrradianceCsv);
    exportMenu->addAction(QStringLiteral("Export in&tensity distribution (CSV)..."),
                          this, &MainWindow::onExportIntensityCsv);
    exportMenu->addAction(QStringLiteral("Export &metrics (CSV)..."),
                          this, &MainWindow::onExportMetricsCsv);
    exportMenu->addAction(QStringLiteral("Export &photometry (IES / EULUMDAT)..."),
                          this, &MainWindow::onExportPhotometry)
        ->setStatusTip(QStringLiteral(
            "Writes the far field as the candela distribution a luminaire is specified by, "
            "ready for DIALux, AGi32 or Relux."));
    exportMenu->addAction(QStringLiteral("Export current &view (PNG)..."),
                          this, &MainWindow::onExportImage);
    exportMenu->addAction(QStringLiteral("Export &report (HTML)..."),
                          QKeySequence(Qt::CTRL | Qt::Key_R),
                          this, &MainWindow::onExportReport)
        ->setStatusTip(QStringLiteral(
            "One document: the scene, its parameters, the energy budget, every "
            "plot and the metrics, stamped with the seed so it can be reproduced."));
    file->addSeparator();

    // The support story. A report is what the design review reads; this is what
    // the support engineer reads when the design review is not enough. One zip
    // with the log, the configuration, the build fingerprint and the GPU report
    // turns "it does not work" into a reproduction.
    file->addAction(QStringLiteral("Save &diagnostics bundle..."), this,
                    &MainWindow::onSaveDiagnostics)
        ->setStatusTip(QStringLiteral(
            "One zip: the log, the configuration, the build fingerprint and the "
            "GPU/driver report. Everything a support engineer needs to reproduce "
            "a problem, in one file."));
    file->addSeparator();

    file->addAction(QStringLiteral("&Reset the results"), this,
                    &MainWindow::onResetResults)
        ->setStatusTip(QStringLiteral(
            "Clears everything the last run produced -- the ray paths, the maps, "
            "the plots and the metrics -- and leaves the scene, and the camera "
            "looking at it, exactly as they are."));
    file->addSeparator();

    // The tutorials sit at the bottom, one segment above Exit: they are a way
    // in rather than something reached mid-session, and at the top they pushed
    // saving and exporting past twenty-eight scene names.
    QMenu* tutorials = file->addMenu(QStringLiteral("&Tutorials"));
    tutorials->setStatusTip(QStringLiteral(
        "A worked optic, loaded as a scene you can then edit: every part of it "
        "appears in the scene tree, and its dimensions stay live until you "
        "change something the tutorial does not describe."));

    QMenu* group = nullptr;
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto scene = GeometryProvider::Scene(i);
        for (const TutorialGroup& g : kGroups)
            if (g.first == scene) { group = tutorials->addMenu(QLatin1String(g.name)); break; }
        if (!group) group = tutorials->addMenu(QStringLiteral("Other"));

        const auto& info = GeometryProvider::info(scene);
        group->addAction(info.name, this, [this, scene] { loadTutorial(scene); })
            ->setStatusTip(info.description);
    }
    file->addSeparator();

    file->addAction(QStringLiteral("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* run = menuBar()->addMenu(QStringLiteral("&Run"));
    run->addAction(QStringLiteral("&Run Simulation"), QKeySequence(Qt::Key_F5),
                   this, &MainWindow::onRun);
    run->addAction(QStringLiteral("&Cancel"), QKeySequence(Qt::Key_Escape),
                   this, &MainWindow::onCancel);
    run->addSeparator();
    run->addAction(QStringLiteral("&Simulation window..."), QKeySequence(Qt::Key_F4),
                   this, &MainWindow::onShowSimulationWindow)
        ->setStatusTip(QStringLiteral(
            "The dimensions, the physics model and the ray budget the next run "
            "uses, in a window you can leave open beside the viewport."));
    run->addSeparator();
    run->addAction(QStringLiteral("Con&vergence sweep"), this, &MainWindow::onRunConvergence);
    run->addAction(QStringLiteral("Through-&focus sweep"), this, &MainWindow::onFocusSweep);
    run->addSeparator();
    run->addAction(QStringLiteral("&Parameter sweep"), this, &MainWindow::onRunSweep);
    run->addAction(QStringLiteral("&Optimise the design"), this, &MainWindow::onRunOptimisation);
    run->addAction(QStringLiteral("&Tolerance analysis"), this, &MainWindow::onRunTolerance)
        ->setStatusTip(QStringLiteral(
            "Perturbs every dimension by its tolerance and traces the ensemble: what "
            "fraction of production passes, and which dimension to tighten."));

    // Design is iterative, and the app used to forget the previous iteration the
    // moment a parameter moved.
    QMenu* compare = menuBar()->addMenu(QStringLiteral("&Compare"));
    compare->addAction(QStringLiteral("&Pin this run"), QKeySequence(Qt::CTRL | Qt::Key_P),
                       this, &MainWindow::onPinResult)
        ->setStatusTip(QStringLiteral(
            "Keeps this result to measure the next one against: the plots overlay it, "
            "the map shows the difference, and the metrics carry the deltas."));
    compare->addAction(QStringLiteral("&Clear the pinned run"), this, &MainWindow::onClearPin);
}

// ---- scene / geometry ------------------------------------------------------

void MainWindow::onGeometryChanged() {
    refreshDerivedQuantities();
    m_geometryTimer->start();
}

void MainWindow::rebuildGeometryView() {
    // The dimensions live in the panel while the document is a tutorial, so a
    // parameter edit has to reach the document before the config is read back
    // out of it -- otherwise the geometry would move and the objects listing it
    // would not.
    if (m_document.linkedToTutorial() && m_controls->params() != m_document.tutorialParams()) {
        m_document.setTutorialParams(m_controls->params());
        m_compiled = m_document.compile();
        m_sceneTree->rebuild();
        m_sceneTree->setSelectedId(m_selectedObject);
        refreshInspector();
    }

    const SimConfig cfg       = currentConfig();
    const bool      assembled = cfg.tracesImport();

    // A spin box that ends up back where it started, or a rebuild triggered by
    // something that does not touch the geometry, is not a rebuild at all --
    // whether the geometry it would ask for is already drawn or already
    // building. An assembled scene is compiled afresh on every edit, so its
    // setup is a new pointer each time and there is nothing to compare.
    const SceneParams params = cfg.effectiveParams();
    if (!assembled && m_haveRequested && !m_requestedAssembled &&
        cfg.scene == m_requestedScene && params == m_requestedParams &&
        cfg.detectorBins == m_requestedDetBins)
        return;

    m_requestedScene     = cfg.scene;
    m_requestedParams    = params;
    m_requestedDetBins   = cfg.detectorBins;
    m_requestedAssembled = assembled;
    m_haveRequested      = true;
    m_geometryGen        = m_geometry->request(cfg);
}

void MainWindow::onGeometryReady(Simulation::SceneRef data, quint64 generation) {
    // Overtaken by a later edit, or by an import that has since taken the
    // viewport over: this geometry is no longer what is being asked for.
    if (generation != m_geometryGen || !data) return;
    m_geometryGen = 0;

    m_sceneData = std::move(data);
    // An assembled scene is not one of the registry's, so it is drawn under a
    // key no scene owns: the camera then stays where the user put it across
    // every edit, instead of reframing each time an object moves.
    // ...and dropping the first object into a tutorial is an edit too, even
    // though it changes that key: the camera is left exactly where the user had
    // it rather than snapping back to the default view.
    m_view3d->setScene(m_requestedAssembled ? GeometryProvider::Scene::Count
                                            : m_requestedScene,
                       m_sceneData->surfaces,
                       m_requestedAssembled);
    applySurfaceVisibility();
    refreshSourceGlyphs();
    // The renderer reads the scene, and nothing reads the renderer. It defers
    // the rebuild until its tab is actually on screen, so this costs nothing
    // for a user who never opens it.
    refreshAppearanceSurfaces();
    // The highlight indexes the surface list, and this is a new one. Only the
    // highlight: this arrives a debounce interval after the edit that asked for
    // it, by which time the user may well be part-way through the next one, and
    // re-reading the document into the panel would overwrite it.
    refreshSelectionHighlight();

    statusBar()->showMessage(QStringLiteral("%1 — %2 triangles")
                                 .arg(m_requestedAssembled
                                          ? QStringLiteral("Assembled scene")
                                          : GeometryProvider::info(m_requestedScene).name)
                                 .arg(m_sceneData->scene.triangles().size()),
                             4000);
}

// Every emitter the current configuration traces, drawn where the trace will
// put it.
//
// The view asked the config for one source and drew one marker, so a luminaire
// with four LEDs showed three of them nowhere -- and the only way to check that
// an offset had landed where it was meant to was to run and read the pattern
// back. The placement rule lives in Simulation::sourcesFor and is asked for
// here rather than repeated, because a viewer that placed sources its own way
// would be a second answer to the same question.
void MainWindow::refreshSourceGlyphs() {
    // The document places the emitters, so the markers are drawn from the same
    // compile the trace is handed -- not from whatever the geometry cache last
    // happened to build.
    if (!m_compiled.havePrimary || !m_compiled.setup) {
        m_view3d->clearSourceGlyph();
        return;
    }
    const gp_Pnt* origin = &m_compiled.setup->sourceOrigin;
    const gp_Dir* axis   = &m_compiled.setup->sourceAxis;

    const SimConfig cfg = currentConfig();
    std::vector<OcctViewWidget::SourceGlyph> glyphs;
    for (const SourceConfig& src : Simulation::sourcesFor(cfg, *origin, *axis)) {
        OcctViewWidget::SourceGlyph g;
        g.origin       = src.origin;
        g.axis         = src.axis;
        g.halfAngleDeg = src.halfAngleDeg;
        g.collimated   = (src.type == SourceConfig::Type::Collimated);
        g.beamRadius   = src.beamRadius;
        // Only worth labelling when there is more than one to tell apart.
        if (cfg.sourceCount() > 1) g.label = src.label;
        glyphs.push_back(g);
    }
    m_view3d->setSourceGlyphs(glyphs);
    // The render lights itself from the same emitters, placed by the same call.
    // Deriving them a second way here would be a second answer to "where is the
    // light?", and the whole value of the preview as a geometry check is that
    // it cannot give one.
    if (m_appearance) m_appearance->setSources(appearanceSources());
}


void MainWindow::onGeometryFailed(const QString& message, quint64 generation) {
    if (generation != m_geometryGen) return;
    m_geometryGen = 0;
    // A parameter combination the kernel cannot build should report itself
    // rather than take the window down.
    statusBar()->showMessage(QStringLiteral("Geometry failed: %1").arg(message));
}

// ---- running ---------------------------------------------------------------

void MainWindow::onRun() {
    if (m_worker->isRunning() || m_study->isRunning()) return;
    if (!requireLightSource()) return;
    m_controls->setRunning(true);
    m_result->setText(QStringLiteral("Running..."));
    m_worker->startRun(currentConfig());
}

void MainWindow::onCancel() {
    m_worker->cancel();
    m_study->cancel();
}

void MainWindow::onProgress(int percent) {
    m_controls->setProgress(percent);
}

void MainWindow::onResult(const SimulationResult& res) {
    m_controls->setRunning(false);
    m_last = res;
    m_hasResult = true;

    // A cancelled run still carries the rays that did finish, so showing it is
    // more useful than discarding the work.
    m_diagram->setResult(res);
    m_heatmap->setResult(res);
    m_polar->setResult(res);
    m_view3d->setRays(res.raySegments);
    // The render can be driven by the distribution this run computed, so it is
    // handed the whole result and told which receivers are now on offer.
    if (m_appearance) m_appearance->setResult(res);
    refreshAppearanceReceivers();

    m_rgbMode->setEnabled(res.spectral);
    if (res.spectral && !m_rgbMode->isChecked()) m_rgbMode->setChecked(true);
    if (!res.spectral) m_rgbMode->setChecked(false);

    refreshDerivedViews();
    refreshImageQuality();
    updateSummary();
    m_metrics->setHtml(formatMetrics());
}

void MainWindow::onPartial(const SimulationResult& partial) {
    // The image forming, rather than a progress bar and then an answer.
    //
    // The path overlay is deliberately left alone: ray segments are recorded
    // only in the leading chunks, so redrawing them mid-run would flicker
    // between subsets of the same few thousand paths for no information gained.
    m_last = partial;
    m_hasResult = true;

    m_heatmap->setResult(partial);
    m_polar->setResult(partial);
    refreshDerivedViews();
    updateSummary();
    m_metrics->setHtml(formatMetrics());
}

void MainWindow::onSourcePicked(int glyphIndex) {
    const int id = objectForSource(glyphIndex);
    if (id == 0) return;
    m_selectedObject = id;
    m_sceneTree->setSelectedId(id);
    refreshInspector();
    if (const scenedoc::SceneObject* o = m_document.find(id))
        statusBar()->showMessage(
            QStringLiteral("%1 -- %2, %3 %4")
                .arg(o->name)
                .arg(SpectrumConfig::kindName(o->source.spectrum.kind))
                .arg(o->source.power, 0, 'g', 4)
                .arg(m_controls->config().fluxUnit == FluxUnit::Lumen
                         ? QStringLiteral("lm")
                         : QStringLiteral("W")),
            6000);
}

void MainWindow::onCutMoved(double, double) {
    refreshDerivedViews();
}

void MainWindow::refreshDerivedViews() {
    if (!m_hasResult) return;

    const analysis::Profile px = analysis::crossSection(m_last, true,  m_heatmap->cutY());
    const analysis::Profile py = analysis::crossSection(m_last, false, m_heatmap->cutX());

    std::vector<PlotSeries> profile;
    if (!px.coord.empty()) {
        PlotSeries s;
        s.name = QStringLiteral("along x");
        s.color = QColor(255, 165, 90);
        s.x = px.coord;
        s.y = px.value;
        s.filled = true;
        profile.push_back(std::move(s));
    }
    if (!py.coord.empty()) {
        PlotSeries s;
        s.name = QStringLiteral("along y");
        s.color = QColor(110, 195, 255);
        s.x = py.coord;
        s.y = py.value;
        profile.push_back(std::move(s));
    }
    if (m_hasPinned && m_pinned.nx > 0) {
        const analysis::Profile qx = analysis::crossSection(m_pinned, true, m_heatmap->cutY());
        if (!qx.coord.empty()) {
            PlotSeries s;
            s.name  = QStringLiteral("pinned, along x");
            s.color = QColor(140, 140, 150);
            s.x = qx.coord;
            s.y = qx.value;
            profile.push_back(std::move(s));
        }
    }
    m_profile->setSeries(std::move(profile));
    m_profile->setTitle(QStringLiteral("Cross-section at (%1, %2) mm")
                            .arg(m_heatmap->cutX(), 0, 'f', 1)
                            .arg(m_heatmap->cutY(), 0, 'f', 1));

    const analysis::EncircledEnergy ee = analysis::encircledEnergy(m_last);
    std::vector<PlotSeries> enc;
    if (!ee.radius.empty()) {
        PlotSeries s;
        s.name = QStringLiteral("encircled");
        s.color = QColor(150, 220, 140);
        s.x = ee.radius;
        s.y = ee.fraction;
        enc.push_back(std::move(s));
    }
    if (m_hasPinned && m_pinned.nx > 0) {
        const analysis::EncircledEnergy pe = analysis::encircledEnergy(m_pinned);
        if (!pe.radius.empty()) {
            PlotSeries s;
            s.name  = QStringLiteral("pinned");
            s.color = QColor(140, 140, 150);
            s.x = pe.radius;
            s.y = pe.fraction;
            enc.push_back(std::move(s));
        }
    }
    m_encircled->setSeries(std::move(enc));

    const analysis::SpotMetrics m = analysis::computeSpotMetrics(m_last);
    std::vector<PlotMarker> marks;
    if (m.valid && m.d86Radius > 0.0)
        marks.push_back({m.d86Radius, QStringLiteral("D86"), QColor(255, 190, 90)});
    if (m.valid && m.rmsRadius > 0.0)
        marks.push_back({m.rmsRadius, QStringLiteral("RMS"), QColor(160, 170, 255)});
    m_encircled->setMarkers(std::move(marks));
}

void MainWindow::updateSummary() {
    const SimulationResult& res = m_last;
    // Watching the bands settle while the error bar shrinks is the most
    // persuasive thing a progressive tracer can show, so this follows the
    // partial results too rather than only the finished one.
    if (m_energyBar) m_energyBar->setResult(res);
    const double eff = 100.0 * res.efficiency;
    const double raysPerSec = res.traceSeconds > 0.0
                                  ? double(res.raysEmitted) / res.traceSeconds
                                  : 0.0;

    QString text;
    if (res.cancelled) text += QStringLiteral("CANCELLED - partial result\n");
    else if (res.partial)
        text += QStringLiteral("RUNNING - %1 %% of the rays so far\n")
                    .arg(res.raysRequested ? 100.0 * double(res.raysEmitted) /
                                                 double(res.raysRequested)
                                           : 0.0, 0, 'f', 0);
    text += QStringLiteral("Rays emitted: %1\nDetector arrivals: %2\n"
                           "Detector flux: %3\nEfficiency: %4 ± %5 %\n"
                           "Trace: %6 s (%7 rays/s)")
                .arg(res.raysEmitted)
                .arg(res.raysHitDetector)
                .arg(res.fluxDetector, 0, 'g', 5)
                .arg(eff, 0, 'f', 2)
                .arg(100.0 * res.efficiencyStdErr, 0, 'f', 3)
                .arg(res.traceSeconds, 0, 'f', 3)
                .arg(qRound(raysPerSec));
    if (res.buildSeconds > 0.0)
        text += QStringLiteral("\nGeometry build: %1 s (cached from now on)")
                    .arg(res.buildSeconds, 0, 'f', 3);

    const double p = res.sourcePower > 0.0 ? res.sourcePower : 1.0;

    // What each source delivered. One source is the ordinary case and needs no
    // breakdown; more than one, and separating the signal from the stray light
    // is the whole reason the second source was added.
    if (res.sources.size() > 1) {
        text += QStringLiteral("\n");
        for (const SourceSummary& s : res.sources)
            text += QStringLiteral("\n%1: %2 %3 delivered (%4 %)")
                        .arg(s.label.isEmpty() ? QStringLiteral("source") : s.label)
                        .arg(s.flux, 0, 'g', 4)
                        .arg(QLatin1String(fluxUnitName(res.unit)))
                        .arg(100.0 * s.efficiency(), 0, 'f', 2);
    }

    // Light a receiver refused is not light anything absorbed, and saying so is
    // the difference between a loss budget and a wrong one.
    if (res.fluxRejected > 0.0)
        text += QStringLiteral("\nRefused by an acceptance cone: %1 % "
                               "(a measurement condition, not a loss)")
                    .arg(100.0 * res.fluxRejected / p, 0, 'f', 3);

    // "Where did the missing 4 % go?" is the question the energy balance exists
    // to answer, and it used to answer "somewhere".
    if (res.truncationSignificant()) {
        const TruncationBreakdown& t = res.truncation;
        struct Reason { const char* name; double flux; };
        const Reason reasons[] = {
            {"the depth limit",      t.depthLimit},
            {"a full branch stack",  t.stackOverflow},
            {"degenerate directions", t.degenerate},
            {"refused refractions",  t.refractFailed},
            {"the energy cutoff",    t.energyCutoff},
        };
        const Reason* worst = &reasons[0];
        for (const Reason& r : reasons) if (r.flux > worst->flux) worst = &r;
        text += QStringLiteral("\nWARNING  %1 % of the source was truncated, "
                               "mostly by %2")
                    .arg(100.0 * res.fluxTruncated / p, 0, 'f', 2)
                    .arg(QLatin1String(worst->name));
    }

    // Imported CAD is the path most likely to be geometrically imperfect, and a
    // mesh that is not closed produces systematically wrong index pairs and a
    // perfectly clean-looking result.
    if (res.anomalies.any())
        text += QStringLiteral("\nWARNING  %1 medium-tracking anomalies "
                               "(%2 unmatched exits, %3 stack overflows, "
                               "%4 guessed indices). The geometry may not be closed.")
                    .arg(res.anomalies.total())
                    .arg(res.anomalies.unmatchedExit)
                    .arg(res.anomalies.stackOverflow)
                    .arg(res.anomalies.guessedIndex);

    // An edit that resolves to nothing is reported rather than dropped -- or,
    // worse, applied to whatever surface now occupies the slot it was saved
    // against.
    if (!res.unmatchedOverrides.empty()) {
        QStringList names;
        for (const QString& s : res.unmatchedOverrides) names << s;
        text += QStringLiteral("\nWARNING  %1 surface edit(s) matched no surface "
                               "and were not applied: %2")
                    .arg(names.size()).arg(names.join(QStringLiteral(", ")));
    }

    m_result->setText(text);
}

QString MainWindow::formatMetrics() const {
    if (!m_hasResult) return QStringLiteral("<i>Run a simulation to see the metrics.</i>");

    const SimulationResult& r = m_last;
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(r);

    auto row = [](const QString& a, const QString& b, const QString& c = QString()) {
        return QStringLiteral("<tr><td style='padding-right:14px;color:#9aa0ae'>%1</td>"
                              "<td style='padding-right:22px'><b>%2</b></td>"
                              "<td style='color:#9aa0ae'>%3</td></tr>").arg(a, b, c);
    };
    auto pct = [&](double v) {
        return QStringLiteral("%1 %").arg(100.0 * v / (r.sourcePower > 0 ? r.sourcePower : 1.0),
                                          0, 'f', 2);
    };

    QString html = QStringLiteral("<div style='font-family:sans-serif;font-size:11px'>");
    html += QStringLiteral("<table><tr><td valign='top'>");

    const QString fu = QLatin1String(fluxUnitName(r.unit));
    const QString iu = QLatin1String(irradianceUnitName(r.unit));
    const QString au = QLatin1String(intensityUnitName(r.unit));

    html += QStringLiteral("<b>Source</b><table>");
    html += row(QStringLiteral("Emitted flux"),
                QStringLiteral("%1 %2").arg(r.sourcePower, 0, 'g', 4).arg(fu));
    if (r.meanWavelengthNm > 0.0)
        html += row(QStringLiteral("Mean wavelength"),
                    QStringLiteral("%1 nm").arg(r.meanWavelengthNm, 0, 'f', 1));
    if (r.luminousEfficacy > 0.0)
        html += row(QStringLiteral("Luminous efficacy"),
                    QStringLiteral("%1 lm/W").arg(r.luminousEfficacy, 0, 'f', 1),
                    QStringLiteral("683 x V(lambda) over the spectrum"));
    html += QStringLiteral("</table><br>");

    html += QStringLiteral("<b>Energy budget</b><table>");
    // Every number that has an uncertainty is shown with it. A figure quoted to
    // four digits when its error bar sits in the second is a claim the run
    // cannot support, and the classic Monte Carlo misreading is exactly that.
    html += row(QStringLiteral("On the receiver"),
                QStringLiteral("%1 %2 %3 %")
                    .arg(100.0 * r.efficiency, 0, 'f', 2)
                    .arg(QChar(0x00B1))
                    .arg(100.0 * r.efficiencyStdErr, 0, 'f', 3),
                QStringLiteral("%1 %2, one standard error across independent "
                               "scrambles").arg(r.fluxDetector, 0, 'g', 4).arg(fu));
    html += row(QStringLiteral("Absorbed at surfaces"),
                pct(r.fluxAbsorbed - r.fluxBulkAbsorbed));
    html += row(QStringLiteral("Absorbed in the bulk"), pct(r.fluxBulkAbsorbed),
                QStringLiteral("Beer-Lambert"));
    html += row(QStringLiteral("Escaped the scene"), pct(r.fluxEscaped));
    if (r.fluxRejected > 0.0)
        html += row(QStringLiteral("Refused by a receiver"), pct(r.fluxRejected),
                    QStringLiteral("outside an acceptance cone: a measurement "
                                   "condition, not a loss"));
    html += row(QStringLiteral("Truncated"), pct(r.fluxTruncated),
                r.truncationSignificant()
                    ? QStringLiteral("<span style='color:#c88a30'>over %1 % of the "
                                     "source</span>")
                          .arg(100.0 * SimulationResult::kTruncationWarn, 0, 'g', 2)
                    : QStringLiteral("depth limit / degenerate branch"));
    html += row(QStringLiteral("Estimator residual"), pct(r.fluxRoulette),
                QStringLiteral("roulette %1, aiming %2, next-event %3, BSDF %4")
                    .arg(pct(r.residual.roulette), pct(r.residual.aiming),
                         pct(r.residual.nextEvent + r.residual.neeSuppressed),
                         pct(r.residual.bsdfWeight)));
    html += row(QStringLiteral("Accounted"), pct(r.fluxAccounted()));
    html += QStringLiteral("</table>");

    if (r.sources.size() > 1) {
        html += QStringLiteral("<br><b>Per source</b><table>");
        for (const SourceSummary& s : r.sources)
            html += row(s.label.isEmpty() ? QStringLiteral("source") : s.label,
                        QStringLiteral("%1 %2").arg(s.flux, 0, 'g', 4).arg(fu),
                        QStringLiteral("%1 %2 emitted, %3 %% delivered, %4 rays")
                            .arg(s.power, 0, 'g', 4).arg(fu)
                            .arg(100.0 * s.efficiency(), 0, 'f', 2)
                            .arg(s.rays));
        html += QStringLiteral("</table>");
    }

    if (r.intensity.valid()) {
        html += QStringLiteral("<br><b>Far field</b><table>");
        html += row(QStringLiteral("Peak intensity"),
                    QStringLiteral("%1 %2").arg(r.intensity.peak, 0, 'g', 4).arg(au));
        html += row(QStringLiteral("Beam FWHM"),
                    QStringLiteral("%1°").arg(r.intensity.fwhmDeg, 0, 'f', 1));
        html += QStringLiteral("</table>");
    }

    html += QStringLiteral("</td><td valign='top'>");

    if (m.valid) {
        html += QStringLiteral("<b>Spot on the receiver</b><table>");
        // Per square metre, which is what W/m^2 and lux both are; per square
        // millimetre made every irradiance read as a tiny number.
        html += row(QStringLiteral("Peak irradiance"),
                    QStringLiteral("%1 %2").arg(m.peak * 1e6, 0, 'g', 4).arg(iu));
        html += row(QStringLiteral("Mean (lit bins)"),
                    QStringLiteral("%1 %2").arg(m.mean * 1e6, 0, 'g', 4).arg(iu));
        html += row(QStringLiteral("Uniformity min/peak"),
                    QStringLiteral("%1").arg(m.uniformity, 0, 'f', 4));
        html += row(QStringLiteral("Uniformity mean/peak"),
                    QStringLiteral("%1").arg(m.meanToPeak, 0, 'f', 4));
        html += row(QStringLiteral("Centroid"),
                    QStringLiteral("(%1, %2) mm").arg(m.centroidX, 0, 'f', 2)
                                                 .arg(m.centroidY, 0, 'f', 2));
        html += row(QStringLiteral("RMS radius"),
                    QStringLiteral("%1 mm").arg(m.rmsRadius, 0, 'f', 3));
        html += row(QStringLiteral("D50 / D86 radius"),
                    QStringLiteral("%1 / %2 mm").arg(m.d50Radius, 0, 'f', 3)
                                                .arg(m.d86Radius, 0, 'f', 3));
        html += row(QStringLiteral("FWHM x / y"),
                    QStringLiteral("%1 / %2 mm").arg(m.fwhmX, 0, 'f', 2)
                                                .arg(m.fwhmY, 0, 'f', 2));
        html += row(QStringLiteral("Lit bins"),
                    QStringLiteral("%1 of %2").arg(m.litBins).arg(m.totalBins));
        {
            const analysis::Wavefront w = analysis::wavefrontError(r);
            if (w.valid) {
                html += row(QStringLiteral("Wavefront RMS"),
                            QStringLiteral("%1 waves").arg(w.rmsWaves, 0, 'f', 3),
                            QStringLiteral("from the optical path each arrival carries"));
                if (w.strehlMeaningful)
                    html += row(QStringLiteral("Strehl"),
                                QStringLiteral("%1").arg(w.strehl, 0, 'f', 4),
                                w.diffractionLimited()
                                    ? QStringLiteral("diffraction limited")
                                    : QStringLiteral("below 0.8"));
            }
        }
        if (m.cieY > 0.0 && r.spectral) {
            html += row(QStringLiteral("CIE x, y"),
                        QStringLiteral("%1, %2").arg(m.cieX, 0, 'f', 4).arg(m.cieY, 0, 'f', 4));
            if (m.cct > 1000.0 && m.cct < 25000.0)
                html += row(QStringLiteral("Colour temperature"),
                            QStringLiteral("%1 K").arg(m.cct, 0, 'f', 0),
                            QStringLiteral("McCamy from the spot's chromaticity"));
        }
        html += QStringLiteral("</table>");
    }

    if (m_hasPinned) {
        const analysis::SpotMetrics pm = analysis::computeSpotMetrics(m_pinned);
        html += QStringLiteral("</td></tr><tr><td colspan='2'>");
        html += QStringLiteral("<br><b>Against the pinned run</b> "
                               "<span style='color:#888'>%1</span><table>")
                    .arg(m_pinnedLabel.toHtmlEscaped());
        auto delta = [&](const QString& name, double now, double was, int decimals,
                         bool biggerIsBetter, const QString& unit) {
            const double d = now - was;
            const QString colour = (std::fabs(d) < 1e-12)
                                       ? QStringLiteral("#888")
                                       : ((d > 0) == biggerIsBetter ? QStringLiteral("#7fc98a")
                                                                    : QStringLiteral("#e08a80"));
            html += QStringLiteral("<tr><td>%1</td><td>%2%3</td>"
                                   "<td style='color:#888'>was %4%3</td>"
                                   "<td style='color:%5'>%6%7%3</td></tr>")
                        .arg(name.toHtmlEscaped(), QString::number(now, 'f', decimals), unit,
                             QString::number(was, 'f', decimals), colour,
                             d >= 0 ? QStringLiteral("+") : QString(),
                             QString::number(d, 'f', decimals));
        };
        delta(QStringLiteral("Efficiency"), 100.0 * r.efficiency, 100.0 * m_pinned.efficiency,
              3, true, QStringLiteral(" %"));
        // Whether that difference is real is a question about the error bars,
        // which the app already computes for both runs.
        const double gap  = std::fabs(r.efficiency - m_pinned.efficiency);
        const double band = r.efficiencyStdErr + m_pinned.efficiencyStdErr;
        html += QStringLiteral("<tr><td>Significance</td><td colspan='3' style='color:#888'>%1</td></tr>")
                    .arg(band > 0.0 && gap > 2.0 * band
                             ? QStringLiteral("real: %1 sigma apart").arg(gap / band, 0, 'f', 1)
                             : QStringLiteral("inside the noise of the two runs"));
        if (m.valid && pm.valid) {
            delta(QStringLiteral("RMS radius"), m.rmsRadius, pm.rmsRadius, 3, false,
                  QStringLiteral(" mm"));
            delta(QStringLiteral("D86 radius"), m.d86Radius, pm.d86Radius, 3, false,
                  QStringLiteral(" mm"));
            delta(QStringLiteral("Peak irradiance"), m.peak * 1e6, pm.peak * 1e6, 1, true,
                  QStringLiteral(" ") + QLatin1String(irradianceUnitName(r.unit)));
            delta(QStringLiteral("Uniformity mean/peak"), m.meanToPeak, pm.meanToPeak, 4, true,
                  QString());
        }
        if (r.intensity.valid() && m_pinned.intensity.valid())
            delta(QStringLiteral("Beam FWHM"), r.intensity.fwhmDeg, m_pinned.intensity.fwhmDeg,
                  2, false, QStringLiteral(" deg"));
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("</td></tr></table></div>");
    return html;
}

void MainWindow::onSurfacePicked(int index) {
    const auto& surfs = currentSurfaces();
    if (surfs.empty() || index < 0 || index >= int(surfs.size())) {
        statusBar()->showMessage(QStringLiteral("No object under the cursor"), 3000);
        m_selectedObject = 0;
        m_sceneTree->setSelectedId(0);
        refreshInspector();
        return;
    }

    // Clicking a body and clicking its row in the tree are the same act, and
    // both mean "this object": a surface is what an object compiled into, not a
    // thing with properties of its own.
    m_selectedObject = objectForSurface(index);
    m_sceneTree->setSelectedId(m_selectedObject);
    refreshInspector();
    const OpticalSurface& s = surfs[std::size_t(index)];

    QStringList parts;
    parts << s.label;
    if (s.isDetector) {
        parts << QStringLiteral("receiver");
    } else if (s.index > 0.0) {
        parts << QStringLiteral("n=%1").arg(s.index, 0, 'f', 3);
        if (s.fresnel)       parts << QStringLiteral("Fresnel R/T");
        else                 parts << QStringLiteral("R=%1 T=%2").arg(s.reflectivity, 0, 'f', 3)
                                                                 .arg(s.transmissivity, 0, 'f', 3);
        if (s.dispersionB > 0.0) parts << QStringLiteral("Cauchy B=%1 µm²").arg(s.dispersionB, 0, 'g', 3);
        if (s.absorption > 0.0)  parts << QStringLiteral("α=%1 /mm").arg(s.absorption, 0, 'g', 3);
    } else {
        parts << QStringLiteral("R=%1").arg(s.reflectivity, 0, 'f', 3);
    }
    if (s.scatter > 0.0)   parts << QStringLiteral("scatter %1").arg(s.scatter, 0, 'f', 2);
    if (s.roughness > 0.0) parts << QStringLiteral("roughness %1 rad").arg(s.roughness, 0, 'g', 3);

    statusBar()->showMessage(parts.join(QStringLiteral("  ·  ")));
}

// ---- studies ---------------------------------------------------------------


// ---- file ------------------------------------------------------------------


// ---- the report -------------------------------------------------------------


// ---- importing a customer's geometry ----------------------------------------


void MainWindow::closeEvent(QCloseEvent* event) {
    // Stop the trace before the widgets it reports into are torn down.
    m_worker->cancel();
    m_worker->wait();
    m_study->cancel();
    m_study->wait();
    // The geometry build reports into the status bar and the viewport. Its own
    // destructor joins the thread; this is what stops a build that finishes in
    // between from being drawn into widgets that are on their way out.
    m_geometryTimer->stop();
    m_geometryGen = 0;
    QMainWindow::closeEvent(event);
}
