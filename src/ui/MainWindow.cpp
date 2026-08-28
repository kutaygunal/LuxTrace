#include "MainWindow.h"

#include "ControlsPanel.h"
#include "HeatmapWidget.h"
#include "OcctViewWidget.h"
#include "PlotWidget.h"
#include "PolarPlotWidget.h"
#include "RayDiagramWidget.h"
#include "core/Analysis.h"
#include "core/CadImport.h"
#include "core/Report.h"
#include "EnergyBarWidget.h"
#include "ImportDialog.h"
#include "SurfaceInspector.h"
#include "PythonPanel.h"
#include "core/ConfigIO.h"
#include "core/MaterialFile.h"
#include "core/SimulationWorker.h"
#include "core/StudyWorker.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace {

// Geometry rebuilds run OCCT tessellation and a BVH build. They happen on a
// worker thread now, and that thread coalesces requests on its own, so this
// only has to stop a spin box drag from queueing a build per step -- it no
// longer has to hide a stall, and can be short enough that an edit looks
// immediate.
constexpr int kGeometryDebounceMs = 60;

QLabel* dim(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color:#8a8f9c;"));
    return l;
}

} // namespace

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
    m_tabs->addTab(buildSurfacesTab(),   QStringLiteral("Surfaces"));
    m_python = new PythonPanel(this);
    m_tabs->addTab(m_python,               QStringLiteral("Python"));

    m_metrics = new QTextBrowser(this);
    m_metrics->setOpenExternalLinks(false);
    m_metrics->setMinimumHeight(150);
    m_metrics->setHtml(QStringLiteral("<i>Run a simulation to see the metrics.</i>"));

    auto* right = new QSplitter(Qt::Vertical, this);
    right->addWidget(m_tabs);
    right->addWidget(m_metrics);
    right->setStretchFactor(0, 4);
    right->setStretchFactor(1, 1);

    m_result = new QLabel(QStringLiteral("Ready. Choose a scene and run."), this);
    m_result->setWordWrap(true);
    m_result->setFrameShape(QFrame::StyledPanel);

    // The quantities the parameters imply, live, before anything is traced.
    // This is the difference between a form and an instrument.
    m_derived = new QLabel(this);
    m_derived->setWordWrap(true);
    m_derived->setStyleSheet(QStringLiteral("color:#9aa0b0; font-size:11px;"));
    m_derived->setVisible(false);

    // Where the light went, drawn rather than printed. It sits under the
    // controls because it is the answer to the question the Run button asks.
    m_energyBar = new EnergyBarWidget(this);

    auto* left = new QWidget(this);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(m_controls, 1);
    lv->addWidget(m_derived, 0);
    lv->addWidget(m_energyBar, 0);
    lv->addWidget(m_result, 0);

    // The controls column is draggable against the viewport rather than pinned
    // at whatever width it was built with: a long scene name, a wide parameter
    // label or a set of derived quantities is a reason to give it more room,
    // and the 3D view is where that room comes from.
    auto* columns = new QSplitter(Qt::Horizontal, this);
    columns->addWidget(left);
    columns->addWidget(right);
    columns->setStretchFactor(0, 0);   // the viewport takes the window's growth
    columns->setStretchFactor(1, 1);
    columns->setChildrenCollapsible(false);
    columns->setSizes({left->sizeHint().width(), 1200});

    auto* central = new QWidget(this);
    auto* main = new QHBoxLayout(central);
    main->setContentsMargins(6, 6, 6, 6);
    main->addWidget(columns);
    setCentralWidget(central);

    buildMenus();
    statusBar()->showMessage(QStringLiteral("Ready"));

    connect(m_controls, &ControlsPanel::runRequested,    this, &MainWindow::onRun);
    connect(m_controls, &ControlsPanel::cancelRequested, this, &MainWindow::onCancel);
    connect(m_controls, &ControlsPanel::sceneChanged,    this, &MainWindow::onSceneChanged);
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
    connect(m_heatmap, &HeatmapWidget::cutMoved, this, &MainWindow::onCutMoved);

    // Hand the viewport keyboard focus when its tab comes up, so WASD works
    // without having to click into it first.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) m_view3d->setFocus(Qt::OtherFocusReason);
    });

    refreshSweepAxes();
    refreshDerivedQuantities();
    onSceneChanged();   // show the starting geometry before the first run
}

// ---- tab construction ------------------------------------------------------

QWidget* MainWindow::buildViewerTab() {
    auto* fit         = new QPushButton(QStringLiteral("Fit (F)"), this);
    auto* reset       = new QPushButton(QStringLiteral("Reset (R)"), this);
    auto* showRays    = new QCheckBox(QStringLiteral("Show rays"), this);
    auto* perspective = new QCheckBox(QStringLiteral("Perspective"), this);
    showRays->setChecked(true);
    perspective->setChecked(true);
    perspective->setToolTip(QStringLiteral(
        "Perspective is needed to walk through the optic; switch it off for the "
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

    auto* bar1 = new QHBoxLayout;
    bar1->setContentsMargins(4, 2, 4, 0);
    bar1->addWidget(fit);
    bar1->addWidget(reset);
    bar1->addWidget(showRays);
    bar1->addWidget(perspective);
    bar1->addWidget(viewCube);
    bar1->addSpacing(10);
    bar1->addWidget(dim(QStringLiteral("Colour:"), this));
    bar1->addWidget(m_rayColor);
    bar1->addWidget(m_detectorOnly);
    bar1->addStretch(1);

    auto* bar2 = new QHBoxLayout;
    bar2->setContentsMargins(4, 0, 4, 2);
    bar2->addWidget(m_clipOn);
    bar2->addWidget(m_clipAxis);
    bar2->addWidget(m_clipSlider);
    bar2->addWidget(m_clipFlip);
    bar2->addSpacing(12);
    bar2->addWidget(dim(QStringLiteral("Drag: L orbit · M pan · R look | Wheel zoom | "
                                       "WASD walk · click a surface to inspect it · "
                                       "click the cube to aim the camera"), this), 1);

    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    v->addWidget(m_view3d, 1);
    v->addLayout(bar1, 0);
    v->addLayout(bar2, 0);

    connect(fit,   &QPushButton::clicked, m_view3d, &OcctViewWidget::fitAll);
    connect(reset, &QPushButton::clicked, m_view3d, &OcctViewWidget::resetView);
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

QWidget* MainWindow::buildStudiesTab() {
    m_convergence = new PlotWidget(this);
    m_convergence->setTitle(QStringLiteral("Efficiency vs ray count"));
    m_convergence->setAxisLabels(QStringLiteral("rays"), QStringLiteral("efficiency"));
    m_convergence->setPlaceholder(QStringLiteral("Run a convergence sweep"));

    m_focus = new PlotWidget(this);
    m_focus->setTitle(QStringLiteral("Through focus"));
    m_focus->setAxisLabels(QStringLiteral("detector z [mm]"), QStringLiteral("spot radius [mm]"));
    m_focus->setPlaceholder(QStringLiteral("Run a trace, then sweep the focus"));

    m_convMin = new QSpinBox(this);
    m_convMin->setRange(100, 1000000);
    m_convMin->setValue(500);
    m_convMin->setGroupSeparatorShown(true);
    m_convMax = new QSpinBox(this);
    m_convMax->setRange(1000, 10000000);
    m_convMax->setValue(500000);
    m_convMax->setGroupSeparatorShown(true);
    m_convPoints = new QSpinBox(this);
    m_convPoints->setRange(3, 20);
    m_convPoints->setValue(10);
    auto* runConv = new QPushButton(QStringLiteral("Run convergence sweep"), this);

    m_focusSpan = new QDoubleSpinBox(this);
    m_focusSpan->setRange(1.0, 4000.0);
    m_focusSpan->setValue(200.0);
    m_focusSpan->setSingleStep(20.0);
    m_focusSpan->setSuffix(QStringLiteral(" mm"));
    auto* runFocus = new QPushButton(QStringLiteral("Sweep through focus"), this);
    runFocus->setToolTip(QStringLiteral(
        "Propagates the recorded receiver arrivals to nearby planes. One trace "
        "answers every plane, so this is instant -- but it only holds where "
        "nothing stands between them."));

    auto* convBar = new QHBoxLayout;
    convBar->setContentsMargins(4, 2, 4, 2);
    convBar->addWidget(dim(QStringLiteral("From:"), this));
    convBar->addWidget(m_convMin);
    convBar->addWidget(dim(QStringLiteral("to:"), this));
    convBar->addWidget(m_convMax);
    convBar->addWidget(dim(QStringLiteral("points:"), this));
    convBar->addWidget(m_convPoints);
    convBar->addWidget(runConv);
    convBar->addStretch(1);
    convBar->addWidget(dim(QStringLiteral("Span:"), this));
    convBar->addWidget(m_focusSpan);
    convBar->addWidget(runFocus);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(m_convergence);
    split->addWidget(m_focus);

    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->addLayout(convBar, 0);
    v->addWidget(split, 1);

    connect(runConv,  &QPushButton::clicked, this, &MainWindow::onRunConvergence);
    connect(runFocus, &QPushButton::clicked, this, &MainWindow::onFocusSweep);
    return pane;
}


// ---- design tab -------------------------------------------------------------
// Sweeps, optimisation, the numbers the parameters imply, and the optics of
// whichever surface was last clicked in the viewport. Everything that turns the
// app from something that answers "what does this do" into something that
// answers "what should I build".

QWidget* MainWindow::buildDesignTab() {
    m_sweepPlot = new PlotWidget(this);
    m_sweepPlot->setTitle(QStringLiteral("Parameter sweep"));
    m_sweepPlot->setPlaceholder(QStringLiteral(
        "Pick a dimension and a metric, and sweep it.\n"
        "Every point is traced with independent seeds, so the error bars say "
        "whether a bump in the curve is the design or the sampling."));

    m_optPlot = new PlotWidget(this);
    m_optPlot->setTitle(QStringLiteral("Optimisation"));
    m_optPlot->setAxisLabels(QStringLiteral("evaluation"), QStringLiteral("metric"));
    m_optPlot->setPlaceholder(QStringLiteral(
        "Search the scene's own dimensions for the best design.\n"
        "The tracer is deterministic, so the objective is a genuine function of "
        "the parameters rather than something that jitters between evaluations."));

    m_sweepParam = new QComboBox(this);
    m_sweepParam->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_sweepParam->setMinimumContentsLength(16);

    m_sweepMetric = new QComboBox(this);
    m_optMetric   = new QComboBox(this);
    for (int i = 0; i < int(studies::Metric::Count); ++i) {
        const auto m = studies::Metric(i);
        m_sweepMetric->addItem(studies::metricName(m), i);
        m_sweepMetric->setItemData(i, studies::metricTip(m), Qt::ToolTipRole);
        m_optMetric->addItem(studies::metricName(m), i);
        m_optMetric->setItemData(i, studies::metricTip(m), Qt::ToolTipRole);
    }
    m_sweepMetric->setCurrentIndex(int(studies::Metric::Efficiency));
    m_optMetric->setCurrentIndex(int(studies::Metric::Efficiency));

    m_sweepSteps = new QSpinBox(this);
    m_sweepSteps->setRange(3, 61);
    m_sweepSteps->setValue(13);
    m_sweepSteps->setToolTip(QStringLiteral("Points along the parameter's declared range."));

    m_sweepRepeats = new QSpinBox(this);
    m_sweepRepeats->setRange(1, 8);
    m_sweepRepeats->setValue(2);
    m_sweepRepeats->setToolTip(QStringLiteral(
        "Independent seeds per point. One gives a curve whose wiggles cannot be "
        "told from its noise; two give it an error bar."));

    auto* runSweep = new QPushButton(QStringLiteral("Sweep"), this);

    m_optGoal = new QComboBox(this);
    m_optGoal->addItem(QStringLiteral("maximise"));
    m_optGoal->addItem(QStringLiteral("minimise"));
    m_optGoal->addItem(QStringLiteral("hit target"));

    m_optTarget = new QDoubleSpinBox(this);
    m_optTarget->setRange(-1e6, 1e6);
    m_optTarget->setDecimals(3);
    m_optTarget->setValue(0.0);
    m_optTarget->setEnabled(false);

    m_optMethod = new QComboBox(this);
    m_optMethod->addItem(QStringLiteral("Nelder-Mead"));
    m_optMethod->addItem(QStringLiteral("CMA-ES"));
    m_optMethod->setItemData(0, QStringLiteral(
        "A derivative-free simplex. Short, tolerant of a noisy objective, and the "
        "right first answer for two to four parameters."), Qt::ToolTipRole);
    m_optMethod->setItemData(1, QStringLiteral(
        "An evolution strategy that learns the shape of the landscape. Slower to "
        "start and far better where the best designs lie along a diagonal valley "
        "rather than along the parameter axes."), Qt::ToolTipRole);

    m_optEvals = new QSpinBox(this);
    m_optEvals->setRange(10, 2000);
    m_optEvals->setValue(80);
    m_optEvals->setToolTip(QStringLiteral("Traces the search is allowed to spend."));

    auto* runOpt  = new QPushButton(QStringLiteral("Optimise"), this);
    m_adoptButton = new QPushButton(QStringLiteral("Adopt best"), this);
    m_adoptButton->setEnabled(false);
    m_adoptButton->setToolTip(QStringLiteral(
        "Writes the design the search found back into the parameter boxes."));

    m_optSummary = new QTextBrowser(this);
    m_optSummary->setMaximumHeight(120);
    m_optSummary->setHtml(QStringLiteral("<i>No search has been run yet.</i>"));

    auto* sweepBar = new QHBoxLayout;
    sweepBar->setContentsMargins(4, 2, 4, 2);
    sweepBar->addWidget(dim(QStringLiteral("Sweep:"), this));
    sweepBar->addWidget(m_sweepParam);
    sweepBar->addWidget(dim(QStringLiteral("measuring:"), this));
    sweepBar->addWidget(m_sweepMetric);
    sweepBar->addWidget(dim(QStringLiteral("points:"), this));
    sweepBar->addWidget(m_sweepSteps);
    sweepBar->addWidget(dim(QStringLiteral("seeds:"), this));
    sweepBar->addWidget(m_sweepRepeats);
    sweepBar->addWidget(runSweep);
    sweepBar->addStretch(1);

    auto* optBar = new QHBoxLayout;
    optBar->setContentsMargins(4, 2, 4, 2);
    optBar->addWidget(dim(QStringLiteral("Optimise:"), this));
    optBar->addWidget(m_optGoal);
    optBar->addWidget(m_optMetric);
    optBar->addWidget(dim(QStringLiteral("target:"), this));
    optBar->addWidget(m_optTarget);
    optBar->addWidget(dim(QStringLiteral("by:"), this));
    optBar->addWidget(m_optMethod);
    optBar->addWidget(dim(QStringLiteral("budget:"), this));
    optBar->addWidget(m_optEvals);
    optBar->addWidget(runOpt);
    optBar->addWidget(m_adoptButton);
    optBar->addStretch(1);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(m_sweepPlot);

    auto* optSide = new QWidget(this);
    auto* optLayout = new QVBoxLayout(optSide);
    optLayout->setContentsMargins(0, 0, 0, 0);
    optLayout->addWidget(m_optPlot, 1);
    optLayout->addWidget(m_optSummary, 0);
    split->addWidget(optSide);

    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->addLayout(sweepBar, 0);
    v->addLayout(optBar, 0);
    v->addWidget(split, 1);

    connect(runSweep, &QPushButton::clicked, this, &MainWindow::onRunSweep);
    connect(runOpt,   &QPushButton::clicked, this, &MainWindow::onRunOptimisation);
    connect(m_adoptButton, &QPushButton::clicked, this, &MainWindow::onAdoptOptimum);
    connect(m_optGoal, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_optTarget->setEnabled(i == 2);
    });
    return pane;
}

void MainWindow::refreshSweepAxes() {
    if (!m_sweepParam) return;
    const auto scene = m_controls->scene();
    const auto& infos = GeometryProvider::paramInfo(scene);
    const int keep = m_sweepParam->currentIndex();
    m_sweepParam->clear();
    for (std::size_t i = 0; i < infos.size(); ++i) {
        m_sweepParam->addItem(infos[i].name, int(i));
        m_sweepParam->setItemData(int(i), infos[i].tip, Qt::ToolTipRole);
    }
    if (keep >= 0 && keep < m_sweepParam->count()) m_sweepParam->setCurrentIndex(keep);
}

void MainWindow::refreshDerivedQuantities() {
    if (!m_derived) return;
    // These follow from the selected scene's dimensions. While a file stands in
    // for that scene they describe an optic that is not being traced, which is
    // exactly the kind of leftover that makes an import look like it kept the
    // previous shape's behaviour.
    if (m_showingImported && m_imported) {
        m_derived->clear();
        m_derived->setVisible(false);
        return;
    }
    const auto scene = m_controls->scene();
    const SimConfig cfg = m_controls->config();
    const auto list = GeometryProvider::derived(scene, cfg.effectiveParams());
    if (list.empty()) {
        m_derived->clear();
        m_derived->setVisible(false);
        return;
    }
    // Plain text in a wrapped label rather than a table: it sits under the
    // parameter block and has to stay out of the way until it is looked at.
    QStringList parts;
    QString tip;
    for (const auto& d : list) {
        parts << QStringLiteral("%1 %2%3").arg(d.name, d.value,
                                               d.unit.isEmpty() ? QString()
                                                                : QLatin1Char(' ') + d.unit);
        if (!d.tip.isEmpty())
            tip += QStringLiteral("<b>%1</b> — %2<br>").arg(d.name.toHtmlEscaped(),
                                                            d.tip.toHtmlEscaped());
    }
    m_derived->setText(parts.join(QStringLiteral("   ·   ")));
    m_derived->setToolTip(tip);
    m_derived->setVisible(true);
}

// ---- sweeps and optimisation ------------------------------------------------

void MainWindow::onRunSweep() {
    if (m_worker->isRunning() || m_study->isRunning()) return;
    if (!requireParametricScene(QStringLiteral("A parameter sweep"))) return;
    const auto scene = m_controls->scene();
    const auto& infos = GeometryProvider::paramInfo(scene);
    const int slotIndex = m_sweepParam->currentData().toInt();
    if (slotIndex < 0 || slotIndex >= int(infos.size())) return;

    m_sweepSlot       = slotIndex;
    m_sweepMetricUsed = studies::Metric(m_sweepMetric->currentData().toInt());
    m_controls->setRunning(true);
    m_controls->setStatus(QStringLiteral("Sweeping %1...").arg(infos[std::size_t(slotIndex)].name));
    m_tabs->setCurrentIndex(5);
    m_study->startSweep(currentConfig(), slotIndex,
                        infos[std::size_t(slotIndex)].min, infos[std::size_t(slotIndex)].max,
                        m_sweepSteps->value(), m_sweepMetricUsed, m_sweepRepeats->value());
}

void MainWindow::onSweepReady(const std::vector<studies::SweepPoint>& points) {
    m_controls->setRunning(false);
    m_sweepPoints = points;
    if (points.empty()) {
        m_controls->setStatus(QStringLiteral("Sweep produced nothing"));
        return;
    }

    const auto& infos = GeometryProvider::paramInfo(m_controls->scene());
    const SceneParamInfo& info = infos[std::size_t(std::clamp(m_sweepSlot, 0,
                                                             int(infos.size()) - 1))];
    PlotSeries s;
    s.name  = studies::metricName(m_sweepMetricUsed);
    s.color = QColor(255, 175, 90);
    for (const auto& p : points) {
        s.x.push_back(p.parameter);
        s.y.push_back(p.value);
        s.yErr.push_back(p.stdErr);
    }
    m_sweepPlot->setSeries({s});
    m_sweepPlot->setAxisLabels(
        QStringLiteral("%1 [%2]").arg(info.name, info.unit),
        QStringLiteral("%1 [%2]").arg(studies::metricName(m_sweepMetricUsed),
                                      studies::metricUnit(m_sweepMetricUsed, m_last.unit)));

    // Mark the best design the sweep found, which is the thing the curve was
    // drawn to show.
    std::size_t best = 0;
    const bool up = studies::metricBiggerIsBetter(m_sweepMetricUsed);
    for (std::size_t i = 1; i < points.size(); ++i)
        if (up ? points[i].value > points[best].value : points[i].value < points[best].value)
            best = i;
    m_sweepPlot->setMarkers({{points[best].parameter, QStringLiteral("best"),
                              QColor(140, 210, 140)}});

    // Is the curve saying anything? A sweep whose whole travel sits inside its
    // own error bars is a picture of the sampling, not of the design -- and
    // reading a Monte Carlo wiggle as a trend is the classic misreading this
    // app already has the number to prevent.
    double lo = points[best].value, hi = points[best].value, worstErr = 0.0;
    for (const auto& p : points) {
        lo = std::min(lo, p.value);
        hi = std::max(hi, p.value);
        worstErr = std::max(worstErr, p.stdErr);
    }
    const bool insideNoise = worstErr > 0.0 && (hi - lo) < 2.0 * worstErr;

    m_sweepPlot->setTitle(
        insideNoise
            ? QStringLiteral("%1 against %2 — the whole curve moves less than its own "
                             "error bar (%3 over %4). Trace more rays or widen the range.")
                  .arg(studies::metricName(m_sweepMetricUsed), info.name,
                       QString::number(hi - lo, 'g', 3),
                       QString::number(2.0 * worstErr, 'g', 3))
            : QStringLiteral("%1 against %2 — best %3 at %4 %5")
                  .arg(studies::metricName(m_sweepMetricUsed), info.name,
                       QString::number(points[best].value, 'g', 4),
                       QString::number(points[best].parameter, 'f', info.decimals),
                       info.unit));
    m_controls->setStatus(insideNoise
                              ? QStringLiteral("Sweep done — but it is all noise")
                              : QStringLiteral("Sweep done"));
}

void MainWindow::onRunOptimisation() {
    if (m_worker->isRunning() || m_study->isRunning()) return;
    if (!requireParametricScene(QStringLiteral("The design search"))) return;
    const auto scene = m_controls->scene();
    const auto& infos = GeometryProvider::paramInfo(scene);
    if (infos.empty()) return;

    // Every dimension except the receiver position, which is the last slot and
    // is a measurement choice rather than a design one.
    m_optimisedSlots.clear();
    for (int i = 0; i + 1 < int(infos.size()); ++i) m_optimisedSlots.push_back(i);
    if (m_optimisedSlots.empty()) m_optimisedSlots.push_back(0);

    m_objective.metric = studies::Metric(m_optMetric->currentData().toInt());
    m_objective.goal   = studies::Objective::Goal(m_optGoal->currentIndex());
    m_objective.target = m_optTarget->value();

    m_controls->setRunning(true);
    m_controls->setStatus(QStringLiteral("Searching..."));
    m_tabs->setCurrentIndex(5);
    m_study->startOptimisation(currentConfig(), m_optimisedSlots, m_objective,
                               m_optMethod->currentIndex() == 1 ? studies::Optimiser::Cmaes
                                                                : studies::Optimiser::NelderMead,
                               m_optEvals->value());
}

void MainWindow::onOptimisationReady(const studies::OptimisationResult& result) {
    m_controls->setRunning(false);
    m_optimisation = result;
    m_adoptButton->setEnabled(result.valid);
    if (!result.valid) {
        m_controls->setStatus(QStringLiteral("The search found nothing to evaluate"));
        return;
    }

    PlotSeries tried;
    tried.name  = QStringLiteral("evaluated");
    tried.color = QColor(120, 130, 165);
    PlotSeries best;
    best.name  = QStringLiteral("best so far");
    best.color = QColor(255, 175, 90);
    bool anyError = false;
    for (const auto& step : result.history) {
        tried.x.push_back(step.evaluation);
        tried.y.push_back(step.value);
        tried.yErr.push_back(step.stdErr);
        if (step.stdErr > 0.0) anyError = true;
        best.x.push_back(step.evaluation);
        best.y.push_back(m_objective.goal == studies::Objective::Goal::Maximise ? -step.merit
                                                                                : step.merit);
    }
    // A metric that reports no uncertainty gets no bars rather than bars of
    // zero, which would read as a claim of exactness.
    if (!anyError) tried.yErr.clear();
    m_optPlot->setSeries({tried, best});
    m_optPlot->setAxisLabels(QStringLiteral("evaluation"),
                             QStringLiteral("%1 [%2]")
                                 .arg(studies::metricName(m_objective.metric),
                                      studies::metricUnit(m_objective.metric, m_last.unit)));
    m_optPlot->setTitle(QStringLiteral("%1 — %2 evaluations")
                            .arg(result.method).arg(result.evaluations));

    const auto& infos = GeometryProvider::paramInfo(m_controls->scene());
    QString html = QStringLiteral("<div style='font-family:sans-serif;font-size:11px'>");
    html += QStringLiteral("<b>%1</b> in %2 s%3<br>")
                .arg(result.method, QString::number(result.seconds, 'f', 1),
                     result.cancelled ? QStringLiteral(" (cancelled)") : QString());
    html += QStringLiteral("%1: <b>%2</b> &rarr; <b>%3</b> %4<br>")
                .arg(studies::metricName(m_objective.metric),
                     QString::number(result.startValue, 'g', 5),
                     QString::number(result.bestValue, 'g', 5),
                     studies::metricUnit(m_objective.metric, m_last.unit));
    html += QStringLiteral("<table>");
    for (int slotIndex : m_optimisedSlots)
        if (slotIndex >= 0 && slotIndex < int(infos.size()))
            html += QStringLiteral("<tr><td>%1</td><td><b>%2</b> %3</td></tr>")
                        .arg(infos[std::size_t(slotIndex)].name.toHtmlEscaped(),
                             QString::number(result.best.v[slotIndex], 'f',
                                             infos[std::size_t(slotIndex)].decimals),
                             infos[std::size_t(slotIndex)].unit.toHtmlEscaped());
    html += QStringLiteral("</table></div>");
    m_optSummary->setHtml(html);
    m_controls->setStatus(QStringLiteral("Search done"));
}

void MainWindow::onAdoptOptimum() {
    if (!m_optimisation.valid) return;
    SimConfig cfg = m_controls->config();
    cfg.useSceneDefaults = false;
    cfg.params = m_optimisation.best;
    m_controls->setConfig(cfg);
    statusBar()->showMessage(QStringLiteral("Adopted the design the search found — run to trace it"),
                             5000);
}

// ---- comparing two runs -----------------------------------------------------

void MainWindow::onPinResult() {
    if (!m_hasResult || m_last.partial) {
        statusBar()->showMessage(QStringLiteral("Nothing finished to pin yet"), 3000);
        return;
    }
    m_pinned      = m_last;
    m_hasPinned   = true;
    m_pinnedLabel = QStringLiteral("%1, %2 rays, seed %3")
                        .arg(currentConfig().sceneName())
                        .arg(m_last.raysEmitted)
                        .arg(m_controls->config().seed);
    m_heatmap->setReference(m_pinned);
    refreshDerivedViews();
    m_metrics->setHtml(formatMetrics());
    statusBar()->showMessage(QStringLiteral("Pinned: %1").arg(m_pinnedLabel), 5000);
}

void MainWindow::onClearPin() {
    m_hasPinned = false;
    m_pinnedLabel.clear();
    m_heatmap->clearReference();
    refreshDerivedViews();
    m_metrics->setHtml(formatMetrics());
    statusBar()->showMessage(QStringLiteral("Comparison cleared"), 3000);
}

// ---- editing the optics of the surface that was clicked ---------------------

const std::vector<OpticalSurface>& MainWindow::currentSurfaces() const {
    static const std::vector<OpticalSurface> kEmpty;
    if (m_showingImported && m_imported && !m_imported->surfaces.empty())
        return m_imported->surfaces;
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

QWidget* MainWindow::buildSurfacesTab() {
    auto* page  = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(6, 6, 6, 6);

    auto* split = new QSplitter(Qt::Horizontal, page);

    // ---- the tree -----------------------------------------------------------
    auto* leftSide = new QWidget(split);
    auto* lv       = new QVBoxLayout(leftSide);
    lv->setContentsMargins(0, 0, 0, 0);

    m_surfaceTree = new QTreeWidget(leftSide);
    m_surfaceTree->setColumnCount(3);
    m_surfaceTree->setHeaderLabels({QStringLiteral("Surface"), QStringLiteral("What it is"),
                                    QStringLiteral("Edited")});
    m_surfaceTree->setRootIsDecorated(false);
    m_surfaceTree->setAlternatingRowColors(true);
    m_surfaceTree->setToolTip(QStringLiteral(
        "Every surface in the scene, whether or not it can be clicked in the 3D "
        "view. The checkbox hides one; a surface carrying an edit is badged, so "
        "an edited optic is not indistinguishable from an untouched one."));
    lv->addWidget(m_surfaceTree, 1);

    m_isolate  = new QPushButton(QStringLiteral("Isolate"), leftSide);
    m_showAll  = new QPushButton(QStringLiteral("Show all"), leftSide);
    m_resetAll = new QPushButton(QStringLiteral("Reset all edits"), leftSide);
    m_isolate->setToolTip(QStringLiteral("Hide everything except the selected surface."));
    m_resetAll->setToolTip(QStringLiteral(
        "Put every surface back to the optics its scene declares."));
    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(m_isolate, 1);
    buttons->addWidget(m_showAll, 1);
    buttons->addWidget(m_resetAll, 1);
    lv->addLayout(buttons);

    // ---- the single-ray probe ----------------------------------------------
    auto* probeBox  = new QGroupBox(QStringLiteral("Single ray"), leftSide);
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
    for (QDoubleSpinBox* s : {m_probeX, m_probeY})
        s->setToolTip(QStringLiteral(
            "Where the ray leaves the source plane, across the optical axis. "
            "The direction is the scene's own emission axis."));

    m_inspectRay = new QPushButton(QStringLiteral("Trace one ray"), probeBox);

    probeForm->addRow(QStringLiteral("Offset x:"), m_probeX);
    probeForm->addRow(QStringLiteral("Offset y:"), m_probeY);
    probeForm->addRow(m_inspectRay);
    lv->addWidget(probeBox);

    m_rayLog = new QTextBrowser(leftSide);
    m_rayLog->setMinimumHeight(140);
    m_rayLog->setHtml(QStringLiteral("<i>Trace one ray to see every interaction "
                                     "it had.</i>"));
    lv->addWidget(m_rayLog, 1);

    // ---- the inspector ------------------------------------------------------
    m_inspector = new SurfaceInspector(split);

    split->addWidget(leftSide);
    split->addWidget(m_inspector);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    outer->addWidget(split);

    connect(m_surfaceTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) { onSurfaceRowChanged(); });
    connect(m_surfaceTree, &QTreeWidget::itemChanged, this,
            &MainWindow::onSurfaceVisibilityChanged);
    connect(m_isolate,  &QPushButton::clicked, this, &MainWindow::onIsolateSurface);
    connect(m_showAll,  &QPushButton::clicked, this, &MainWindow::onShowAllSurfaces);
    connect(m_resetAll, &QPushButton::clicked, this, &MainWindow::onResetAllSurfaces);
    connect(m_inspectRay, &QPushButton::clicked, this, &MainWindow::onInspectRay);
    connect(m_inspector, &SurfaceInspector::edited, this, &MainWindow::onSurfaceEdited);
    connect(m_inspector, &SurfaceInspector::resetRequested, this,
            &MainWindow::onResetSurface);

    return page;
}

SurfaceOptics MainWindow::effectiveOptics(int surface) const {
    const auto& surfs = currentSurfaces();
    if (surface < 0 || surface >= int(surfs.size())) return SurfaceOptics{};
    SurfaceOptics o = surfs[std::size_t(surface)];
    for (const SurfaceOverride& ov : m_overrides)
        if (ov.surface == surface) o = ov.optics;
    return o;
}

bool MainWindow::surfaceIsEdited(int surface) const {
    for (const SurfaceOverride& ov : m_overrides)
        if (ov.surface == surface) return true;
    return false;
}

void MainWindow::rebuildSurfaceTree() {
    if (!m_surfaceTree) return;
    const auto& surfs = currentSurfaces();

    m_loadingTree = true;
    m_surfaceTree->clear();
    for (std::size_t i = 0; i < surfs.size(); ++i) {
        const OpticalSurface& s = surfs[i];
        auto* item = new QTreeWidgetItem(m_surfaceTree);
        item->setData(0, Qt::UserRole, int(i));
        item->setText(0, s.label.isEmpty()
                             ? QStringLiteral("Surface %1").arg(i + 1) : s.label);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);

        QString what;
        if (s.isDetector)          what = QStringLiteral("receiver");
        else if (s.index > 0.0)    what = QStringLiteral("refractive, n_d %1")
                                              .arg(s.index, 0, 'f', 3);
        else if (s.material.isMetal()) what = QStringLiteral("metal mirror");
        else                       what = QStringLiteral("mirror, R %1")
                                              .arg(s.reflectivity, 0, 'f', 2);
        item->setText(1, what);
    }
    m_surfaceTree->resizeColumnToContents(0);
    m_loadingTree = false;

    refreshSurfaceTreeBadges();
    if (m_pickedSurface >= 0 && m_pickedSurface < int(surfs.size()))
        m_surfaceTree->setCurrentItem(m_surfaceTree->topLevelItem(m_pickedSurface));
    else
        updateSurfacePanel();
}

void MainWindow::refreshSurfaceTreeBadges() {
    if (!m_surfaceTree) return;
    const bool wasLoading = m_loadingTree;
    m_loadingTree = true;
    for (int i = 0; i < m_surfaceTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_surfaceTree->topLevelItem(i);
        const bool edited = surfaceIsEdited(i);
        item->setText(2, edited ? QStringLiteral("edited") : QString());
        item->setForeground(2, QColor(176, 112, 24));
    }
    m_loadingTree = wasLoading;
    if (m_resetAll) m_resetAll->setEnabled(!m_overrides.empty());
}

void MainWindow::onSurfaceRowChanged() {
    if (m_loadingTree || !m_surfaceTree) return;
    QTreeWidgetItem* item = m_surfaceTree->currentItem();
    m_pickedSurface = item ? item->data(0, Qt::UserRole).toInt() : -1;
    updateSurfacePanel();
    // Keep the viewport's own idea of the selection in step, so clicking a row
    // and clicking the solid mean the same thing.
    m_view3d->setHighlightedSurface(m_pickedSurface);
}

void MainWindow::onSurfaceVisibilityChanged(QTreeWidgetItem* item, int column) {
    if (m_loadingTree || !item || column != 0) return;
    const int index = item->data(0, Qt::UserRole).toInt();
    m_view3d->setSurfaceVisible(index, item->checkState(0) == Qt::Checked);
}

void MainWindow::onIsolateSurface() {
    if (!m_surfaceTree) return;
    const int keep = m_pickedSurface;
    m_loadingTree = true;
    for (int i = 0; i < m_surfaceTree->topLevelItemCount(); ++i) {
        const bool show = (i == keep);
        m_surfaceTree->topLevelItem(i)->setCheckState(0, show ? Qt::Checked : Qt::Unchecked);
        m_view3d->setSurfaceVisible(i, show);
    }
    m_loadingTree = false;
}

void MainWindow::onShowAllSurfaces() {
    if (!m_surfaceTree) return;
    m_loadingTree = true;
    for (int i = 0; i < m_surfaceTree->topLevelItemCount(); ++i) {
        m_surfaceTree->topLevelItem(i)->setCheckState(0, Qt::Checked);
        m_view3d->setSurfaceVisible(i, true);
    }
    m_loadingTree = false;
}

void MainWindow::onResetAllSurfaces() {
    if (m_overrides.empty()) return;
    m_overrides.clear();
    refreshSurfaceTreeBadges();
    updateSurfacePanel();
    statusBar()->showMessage(
        QStringLiteral("Every surface restored to the optics its scene declares"), 4000);
}

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

void MainWindow::updateSurfacePanel() {
    if (!m_inspector) return;
    const auto& surfs = currentSurfaces();
    const bool have = !surfs.empty() && m_pickedSurface >= 0 &&
                      m_pickedSurface < int(surfs.size());
    if (!have) {
        m_inspector->clearSurface();
        return;
    }

    const OpticalSurface& base = surfs[std::size_t(m_pickedSurface)];
    m_loadingSurface = true;
    m_inspector->setSurface(base.label, effectiveOptics(m_pickedSurface), base,
                            surfaceIsEdited(m_pickedSurface));
    m_loadingSurface = false;
}

void MainWindow::onSurfaceEdited() {
    const auto& surfs = currentSurfaces();
    if (m_loadingSurface || surfs.empty() || m_pickedSurface < 0 ||
        m_pickedSurface >= int(surfs.size()))
        return;
    const OpticalSurface& base = surfs[std::size_t(m_pickedSurface)];

    SurfaceOverride ov;
    // Keyed by what the surface *is*, not by where it sat, so a saved config
    // reopened against changed geometry cannot land the edit on whatever now
    // occupies the slot.
    ov.label   = base.label;
    ov.surface = m_pickedSurface;
    if (m_sceneData) ov.identity = m_sceneData->scene.surfaceIdentity(m_pickedSurface);
    ov.optics  = m_inspector->optics();

    auto it = std::find_if(m_overrides.begin(), m_overrides.end(),
                           [&](const SurfaceOverride& o) { return o.surface == m_pickedSurface; });
    if (it != m_overrides.end()) *it = ov;
    else                         m_overrides.push_back(ov);

    refreshSurfaceTreeBadges();

    // Whether this costs a trace or a rebuild depends on what was touched: a
    // receiver's bin grid is baked into the geometry, and everything else is a
    // ray-time property the cached tessellation and hierarchy do not care about.
    const bool binsChanged = base.isDetector &&
                             (ov.optics.detNX != base.detNX || ov.optics.detNY != base.detNY);
    statusBar()->showMessage(
        binsChanged
            ? QStringLiteral("%1 edited — the bin grid is geometry, so this rebuilds")
                  .arg(base.label)
            : QStringLiteral("%1 edited — run to trace it (no rebuild needed)")
                  .arg(base.label),
        4000);
    if (binsChanged) onGeometryChanged();
}

void MainWindow::onResetSurface() {
    m_overrides.erase(std::remove_if(m_overrides.begin(), m_overrides.end(),
                                     [&](const SurfaceOverride& o) {
                                         return o.surface == m_pickedSurface;
                                     }),
                      m_overrides.end());
    refreshSurfaceTreeBadges();
    updateSurfacePanel();
    statusBar()->showMessage(QStringLiteral("Surface restored to the scene's own optics"), 3000);
}

SimConfig MainWindow::currentConfig() const {
    SimConfig cfg = m_controls->config();
    cfg.surfaceOverrides = m_overrides;
    // What is on screen is what gets traced. Without this the run took the
    // scene enum the controls still carried and traced the built-in optic
    // behind it, which is why an imported part appeared in the viewport while
    // the rays went through the shape it had replaced.
    if (m_showingImported && m_imported) cfg.imported = m_imported;
    return cfg;
}

bool MainWindow::requireParametricScene(const QString& what) {
    if (!(m_showingImported && m_imported)) return true;
    QMessageBox::information(
        this, windowTitle(),
        QStringLiteral("%1 varies the scene's own dimensions, and imported geometry "
                       "has none: the file is a fixed solid, not a parametric optic.\n\n"
                       "Trace it, sweep the ray count, or edit a surface's optics — "
                       "those all apply. To sweep a dimension, pick a built-in scene "
                       "from the list, which replaces the imported part.").arg(what));
    return false;
}


// ---- tolerance tab ----------------------------------------------------------
// The question a manufacturer actually asks. Not "how good is the nominal
// design" but "what fraction of production will pass", and "which dimension do
// I tighten to change that".

QWidget* MainWindow::buildToleranceTab() {
    m_tolPlot = new PlotWidget(this);
    m_tolPlot->setTitle(QStringLiteral("Yield"));
    m_tolPlot->setPlaceholder(QStringLiteral(
        "Perturb every dimension by its tolerance and trace the ensemble.\n"
        "The histogram is the production run; the pass line is the specification."));

    m_mtfPlot = new PlotWidget(this);
    m_mtfPlot->setTitle(QStringLiteral("Modulation transfer"));
    m_mtfPlot->setAxisLabels(QStringLiteral("cycles/mm"), QStringLiteral("modulation"));
    m_mtfPlot->setPlaceholder(QStringLiteral(
        "The frequency-domain half of the spot metrics.\n"
        "The spot size says how big the blur is; this says what it does to "
        "contrast, which is what an imaging system is specified by."));

    m_tolMetric = new QComboBox(this);
    for (int i = 0; i < int(studies::Metric::Count); ++i) {
        const auto m = studies::Metric(i);
        m_tolMetric->addItem(studies::metricName(m), i);
        m_tolMetric->setItemData(i, studies::metricTip(m), Qt::ToolTipRole);
    }
    m_tolMetric->setCurrentIndex(int(studies::Metric::Efficiency));

    m_tolPercent = new QDoubleSpinBox(this);
    m_tolPercent->setRange(0.1, 25.0);
    m_tolPercent->setValue(2.0);
    m_tolPercent->setSingleStep(0.5);
    m_tolPercent->setDecimals(2);
    m_tolPercent->setSuffix(QStringLiteral(" %"));
    m_tolPercent->setToolTip(QStringLiteral(
        "The +/- limit on every dimension, as a share of its declared range. "
        "Whatever the distribution, this is the limit on the drawing."));

    m_tolShape = new QComboBox(this);
    m_tolShape->addItem(QStringLiteral("Gaussian (limit at 3 sigma)"));
    m_tolShape->addItem(QStringLiteral("Uniform within the limit"));
    m_tolShape->addItem(QStringLiteral("At the limits (worst case)"));
    m_tolShape->setItemData(0, QStringLiteral(
        "A process under control: most parts near nominal, the limit at three "
        "standard deviations."), Qt::ToolTipRole);
    m_tolShape->setItemData(2, QStringLiteral(
        "Every part at one limit or the other -- the worst a supplier can ship "
        "while still passing inspection."), Qt::ToolTipRole);
    m_tolShape->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

    m_tolCriterion = new QDoubleSpinBox(this);
    m_tolCriterion->setRange(-1e6, 1e6);
    m_tolCriterion->setDecimals(3);
    m_tolCriterion->setValue(50.0);
    m_tolCriterion->setToolTip(QStringLiteral(
        "The specification a part has to meet. Which side counts as passing "
        "follows from the metric: more efficiency is better, less blur is."));

    m_tolSamples = new QSpinBox(this);
    m_tolSamples->setRange(20, 2000);
    m_tolSamples->setValue(120);
    m_tolSamples->setToolTip(QStringLiteral("Parts in the simulated production run."));

    auto* runTol = new QPushButton(QStringLiteral("Run the ensemble"), this);

    m_tolSummary = new QTextBrowser(this);
    m_tolSummary->setMaximumHeight(150);
    m_tolSummary->setHtml(QStringLiteral("<i>No tolerance study has been run yet.</i>"));

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(dim(QStringLiteral("Judge by:"), this));
    bar->addWidget(m_tolMetric);
    bar->addWidget(dim(QStringLiteral("passing at:"), this));
    bar->addWidget(m_tolCriterion);
    bar->addWidget(dim(QStringLiteral("tolerance:"), this));
    bar->addWidget(m_tolPercent);
    bar->addWidget(m_tolShape);
    bar->addWidget(dim(QStringLiteral("parts:"), this));
    bar->addWidget(m_tolSamples);
    bar->addWidget(runTol);
    bar->addStretch(1);

    auto* split = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(this);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(m_tolPlot, 1);
    lv->addWidget(m_tolSummary, 0);
    split->addWidget(left);
    split->addWidget(m_mtfPlot);

    auto* pane = new QWidget(this);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->addLayout(bar, 0);
    v->addWidget(split, 1);

    connect(runTol, &QPushButton::clicked, this, &MainWindow::onRunTolerance);
    return pane;
}

void MainWindow::onRunTolerance() {
    if (m_worker->isRunning() || m_study->isRunning()) return;
    if (!requireParametricScene(QStringLiteral("A tolerance analysis"))) return;
    const auto scene = m_controls->scene();
    const auto& infos = GeometryProvider::paramInfo(scene);
    if (infos.size() < 2) {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("This scene has nothing to tolerance."));
        return;
    }

    // Every dimension except the receiver position: where the measurement plane
    // sits is a choice, not a manufacturing variation.
    std::vector<studies::Tolerance> tols;
    const double share = m_tolPercent->value() / 100.0;
    for (int i = 0; i + 1 < int(infos.size()); ++i) {
        studies::Tolerance t;
        t.slotIndex = i;
        t.amount    = share * (infos[std::size_t(i)].max - infos[std::size_t(i)].min);
        switch (m_tolShape->currentIndex()) {
        case 1:  t.shape = studies::Tolerance::Shape::Uniform;  break;
        case 2:  t.shape = studies::Tolerance::Shape::Bimodal;  break;
        default: t.shape = studies::Tolerance::Shape::Gaussian; break;
        }
        tols.push_back(t);
    }

    const auto metric = studies::Metric(m_tolMetric->currentData().toInt());
    m_controls->setRunning(true);
    m_controls->setStatus(QStringLiteral("Tracing %1 parts...").arg(m_tolSamples->value()));
    m_tabs->setCurrentIndex(6);
    m_study->startTolerance(currentConfig(), tols, metric, m_tolCriterion->value(),
                            studies::metricBiggerIsBetter(metric), m_tolSamples->value());
}

void MainWindow::onToleranceReady(const studies::ToleranceStudy& study) {
    m_controls->setRunning(false);
    m_toleranceStudy = study;
    if (!study.valid) {
        m_controls->setStatus(QStringLiteral("The tolerance study produced nothing"));
        return;
    }

    PlotSeries hist;
    hist.name   = QStringLiteral("parts");
    hist.color  = QColor(255, 175, 90);
    hist.filled = true;
    for (std::size_t i = 0; i < study.binCentre.size(); ++i) {
        hist.x.push_back(study.binCentre[i]);
        hist.y.push_back(double(study.binCount[i]));
        // A histogram bin is a count, so its uncertainty is the square root of
        // it. Without that a hundred-sample ensemble looks like a smooth
        // distribution rather than the handful of parts per bin it is.
        hist.yErr.push_back(std::sqrt(double(std::max(0, study.binCount[i]))));
    }
    m_tolPlot->setSeries({hist});
    m_tolPlot->setAxisLabels(
        QStringLiteral("%1 [%2]").arg(studies::metricName(study.metric),
                                      studies::metricUnit(study.metric, m_last.unit)),
        QStringLiteral("parts"));
    // The specification and the nominal design, so the histogram can be read
    // against both at once.
    m_tolPlot->setMarkers({{study.criterion, QStringLiteral("spec"), QColor(230, 120, 110)},
                           {study.nominal, QStringLiteral("nominal"), QColor(140, 200, 250)}});
    m_tolPlot->setTitle(QStringLiteral("Yield %1 %  (%2 parts)")
                            .arg(100.0 * study.yield, 0, 'f', 1)
                            .arg(study.samples));

    const auto& infos = GeometryProvider::paramInfo(m_controls->scene());
    QString html = QStringLiteral("<div style='font-family:sans-serif;font-size:11px'>");
    html += QStringLiteral("<b>%1 %</b> of parts %2 %3 %4<br>")
                .arg(100.0 * study.yield, 0, 'f', 1)
                .arg(study.passIsAbove ? QStringLiteral("reach") : QStringLiteral("stay under"))
                .arg(study.criterion, 0, 'g', 5)
                .arg(studies::metricUnit(study.metric, m_last.unit));
    html += QStringLiteral("<table>");
    html += QStringLiteral("<tr><td>nominal</td><td>%1</td></tr>")
                .arg(study.nominal, 0, 'g', 5);
    html += QStringLiteral("<tr><td>mean</td><td>%1 &plusmn; %2</td></tr>")
                .arg(study.mean, 0, 'g', 5).arg(study.stdDev, 0, 'g', 3);
    html += QStringLiteral("<tr><td>90 % of parts beat</td><td>%1</td></tr>")
                .arg(study.p90, 0, 'g', 5);
    html += QStringLiteral("<tr><td>worst part</td><td>%1</td></tr>")
                .arg(study.worst, 0, 'g', 5);
    html += QStringLiteral("</table><br><b>What dominates it</b><table>");
    for (const auto& sens : study.sensitivity)
        html += QStringLiteral("<tr><td>%1</td><td><b>%2 %</b> of the spread</td></tr>")
                    .arg(sens.name.toHtmlEscaped())
                    .arg(100.0 * sens.share, 0, 'f', 1);
    html += QStringLiteral("</table>");
    if (!study.sensitivity.empty() && study.sensitivity[0].share > 0.5)
        html += QStringLiteral("<br><span style='color:#9aa0b0'>Tighten <b>%1</b> first: "
                               "it carries most of the variation.</span>")
                    .arg(study.sensitivity[0].name.toHtmlEscaped());
    html += QStringLiteral("</div>");
    m_tolSummary->setHtml(html);
    (void)infos;
    m_controls->setStatus(QStringLiteral("Tolerance study done"));
}

void MainWindow::refreshImageQuality() {
    if (!m_mtfPlot) return;
    if (!m_hasResult) { m_mtfPlot->clear(); return; }

    const analysis::Mtf mtf = analysis::modulationTransfer(m_last);
    if (!mtf.valid) { m_mtfPlot->clear(); return; }

    std::vector<PlotSeries> series;
    PlotSeries t;
    t.name  = QStringLiteral("tangential");
    t.color = QColor(255, 175, 90);
    t.x = mtf.frequency;
    t.y = mtf.tangential;
    series.push_back(std::move(t));

    PlotSeries sag;
    sag.name  = QStringLiteral("sagittal");
    sag.color = QColor(110, 195, 255);
    sag.x = mtf.frequency;
    sag.y = mtf.sagittal;
    series.push_back(std::move(sag));

    if (!mtf.diffractionLimit.empty()) {
        PlotSeries d;
        d.name  = QStringLiteral("diffraction limit");
        d.color = QColor(150, 150, 160);
        d.x = mtf.frequency;
        d.y = mtf.diffractionLimit;
        series.push_back(std::move(d));
    }
    m_mtfPlot->setSeries(std::move(series));
    if (mtf.cutoff50 > 0.0)
        m_mtfPlot->setMarkers({{mtf.cutoff50, QStringLiteral("50 %"), QColor(140, 200, 140)}});
    else
        m_mtfPlot->setMarkers({});
    m_mtfPlot->setTitle(mtf.cutoff50 > 0.0
                            ? QStringLiteral("Modulation transfer — 50 %% at %1 cycles/mm")
                                  .arg(mtf.cutoff50, 0, 'g', 4)
                            : QStringLiteral("Modulation transfer"));
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("&Import CAD (STEP / IGES)..."),
                    this, &MainWindow::onImportCad)
        ->setStatusTip(QStringLiteral(
            "Reads a customer's geometry. The whole pipeline downstream already "
            "works on it; only the reader was missing."));
    file->addSeparator();
    file->addAction(QStringLiteral("&Save configuration..."), QKeySequence::Save,
                    this, &MainWindow::onSaveConfig);
    file->addAction(QStringLiteral("&Open configuration..."), QKeySequence::Open,
                    this, &MainWindow::onLoadConfig);
    file->addSeparator();
    file->addAction(QStringLiteral("Export &irradiance grid (CSV)..."),
                    this, &MainWindow::onExportIrradianceCsv);
    file->addAction(QStringLiteral("Export in&tensity distribution (CSV)..."),
                    this, &MainWindow::onExportIntensityCsv);
    file->addAction(QStringLiteral("Export &metrics (CSV)..."),
                    this, &MainWindow::onExportMetricsCsv);
    file->addAction(QStringLiteral("Export &photometry (IES / EULUMDAT)..."),
                    this, &MainWindow::onExportPhotometry)
        ->setStatusTip(QStringLiteral(
            "Writes the far field as the candela distribution a luminaire is specified by, "
            "ready for DIALux, AGi32 or Relux."));
    file->addAction(QStringLiteral("Export current &view (PNG)..."),
                    this, &MainWindow::onExportImage);
    file->addAction(QStringLiteral("Export &report (HTML)..."),
                    QKeySequence(Qt::CTRL | Qt::Key_R),
                    this, &MainWindow::onExportReport)
        ->setStatusTip(QStringLiteral(
            "One document: the scene, its parameters, the energy budget, every "
            "plot and the metrics, stamped with the seed so it can be reproduced."));
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    file->addSeparator();
    file->addAction(QStringLiteral("Load &material catalogue..."), this,
                    &MainWindow::onLoadMaterialCatalogue)
        ->setStatusTip(QStringLiteral(
            "Read a Zemax .agf glass catalogue -- the format Schott, Ohara, CDGM, "
            "Hoya and Sumita all publish in -- or a refractiveindex.info .yml "
            "entry. Ten built-in materials is a demonstration; a catalogue is a "
            "tool."));

    QMenu* run = menuBar()->addMenu(QStringLiteral("&Run"));
    run->addAction(QStringLiteral("&Trace"), QKeySequence(Qt::Key_F5), this, &MainWindow::onRun);
    run->addAction(QStringLiteral("&Cancel"), QKeySequence(Qt::Key_Escape),
                   this, &MainWindow::onCancel);
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

void MainWindow::onSceneChanged() {
    // A different optic has different dimensions, and any surface edits belonged
    // to the old one.
    m_overrides.clear();
    m_pickedSurface = -1;
    refreshSurfaceTreeBadges();
    updateSurfacePanel();
    refreshSweepAxes();
    refreshDerivedQuantities();
    rebuildGeometryView();
    // The old scene's rays do not belong to this one.
    m_view3d->setRays({});
}

void MainWindow::onGeometryChanged() {
    refreshDerivedQuantities();
    m_geometryTimer->start();
}

void MainWindow::rebuildGeometryView() {
    const SimConfig cfg = m_controls->config();
    // Any built-in scene change replaces the imported part: the view now shows
    // the scene's own geometry, so picks must index that again -- and so must
    // the trace, which is why the import is dropped rather than merely hidden.
    const bool droppedImport = m_showingImported && m_imported;
    m_showingImported = false;
    m_imported.reset();
    m_controls->setImportedGeometry(QString());

    const SceneParams params = cfg.effectiveParams();
    // A spin box that ends up back where it started, or a rebuild triggered by
    // something that does not touch the geometry, is not a rebuild at all --
    // whether the geometry it would ask for is already drawn or already
    // building.
    if (!droppedImport && m_haveRequested && cfg.scene == m_requestedScene &&
        params == m_requestedParams && cfg.detectorBins == m_requestedDetBins)
        return;

    m_requestedScene   = cfg.scene;
    m_requestedParams  = params;
    m_requestedDetBins = cfg.detectorBins;
    m_haveRequested    = true;
    m_geometryGen      = m_geometry->request(cfg);
    if (droppedImport)
        statusBar()->showMessage(
            QStringLiteral("The imported part was replaced by %1; import it again to "
                           "go back to it.")
                .arg(GeometryProvider::info(cfg.scene).name), 10000);
}

void MainWindow::onGeometryReady(Simulation::SceneRef data, quint64 generation) {
    // Overtaken by a later edit, or by an import that has since taken the
    // viewport over: this geometry is no longer what is being asked for.
    if (generation != m_geometryGen || !data) return;
    m_geometryGen = 0;

    m_sceneData = std::move(data);
    m_view3d->setScene(m_requestedScene, m_sceneData->surfaces);
    // The surface list belongs to this geometry; a tree describing the previous
    // one would let a click land on a surface that is no longer there.
    rebuildSurfaceTree();
    refreshSourceGlyphs();
    // A pick indexes the surface list, and this is a new one.
    updateSurfacePanel();

    statusBar()->showMessage(QStringLiteral("%1 — %2 triangles")
                                 .arg(GeometryProvider::info(m_requestedScene).name)
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
    const gp_Pnt* origin = nullptr;
    const gp_Dir* axis   = nullptr;
    if (m_showingImported && m_imported) {
        origin = &m_imported->sourceOrigin;
        axis   = &m_imported->sourceAxis;
    } else if (m_sceneData) {
        origin = &m_sceneData->sourceOrigin;
        axis   = &m_sceneData->sourceAxis;
    }
    if (!origin || !axis) {
        m_view3d->clearSourceGlyph();
        return;
    }

    const SimConfig cfg = m_controls->config();
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
    // A pick and a row selection are the same act; the tree is the one place
    // the selection lives.
    if (m_surfaceTree && index >= 0 && index < m_surfaceTree->topLevelItemCount()) {
        m_loadingTree = true;
        m_surfaceTree->setCurrentItem(m_surfaceTree->topLevelItem(index));
        m_loadingTree = false;
    }
    const auto& surfs = currentSurfaces();
    if (surfs.empty() || index < 0 || index >= int(surfs.size())) {
        statusBar()->showMessage(QStringLiteral("No surface under the cursor"), 3000);
        m_pickedSurface = -1;
        updateSurfacePanel();
        return;
    }
    // Reporting what a surface is and stopping there was half the job. None of
    // these properties are geometry, so editing one costs a trace and not a
    // rebuild -- the tessellation and the hierarchy stay exactly as they are.
    m_pickedSurface = index;
    updateSurfacePanel();
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

void MainWindow::onRunConvergence() {
    if (m_worker->isRunning() || m_study->isRunning()) return;
    if (m_convMax->value() <= m_convMin->value()) {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("The upper ray count must exceed the lower one."));
        return;
    }
    m_controls->setRunning(true);
    m_controls->setStatus(QStringLiteral("Convergence sweep..."));
    m_tabs->setCurrentIndex(4);
    m_study->startConvergence(m_controls->config(), m_convMin->value(), m_convMax->value(),
                              m_convPoints->value());
}

void MainWindow::onConvergenceReady(const std::vector<studies::ConvergencePoint>& points) {
    m_controls->setRunning(false);
    m_convergencePoints = points;
    if (points.empty()) {
        m_convergence->clear();
        return;
    }

    PlotSeries s;
    s.name = QStringLiteral("efficiency");
    s.color = QColor(255, 175, 95);
    s.markers = true;
    for (const auto& p : points) {
        s.x.push_back(double(p.rays));
        s.y.push_back(p.efficiency);
        s.yErr.push_back(p.stdErr);
    }

    // The converged value, drawn flat, so the early points can be read against
    // where they were heading.
    PlotSeries ref;
    ref.name = QStringLiteral("final value");
    ref.color = QColor(120, 200, 255, 180);
    ref.x = {s.x.front(), s.x.back()};
    ref.y = {points.back().efficiency, points.back().efficiency};

    m_convergence->setSeries({std::move(ref), std::move(s)});
    m_convergence->setTitle(
        studies::hasConverged(points)
            ? QStringLiteral("Efficiency vs rays — converged at %1 rays").arg(points.back().rays)
            : QStringLiteral("Efficiency vs rays — not yet converged"));

    const auto& last = points.back();
    statusBar()->showMessage(
        QStringLiteral("Convergence: %1 % ± %2 at %3 rays  (%4)")
            .arg(100.0 * last.efficiency, 0, 'f', 3)
            .arg(100.0 * last.stdErr, 0, 'f', 4)
            .arg(last.rays)
            .arg(studies::hasConverged(points) ? QStringLiteral("stable within 2σ")
                                               : QStringLiteral("still moving — trace more rays")));
}

void MainWindow::onFocusSweep() {
    if (!m_hasResult || m_last.arrivals.empty()) {
        QMessageBox::information(this, windowTitle(),
            QStringLiteral("Run a trace first — the sweep works from the receiver "
                           "arrivals that trace recorded."));
        return;
    }
    m_tabs->setCurrentIndex(4);
    m_focusStudy = studies::throughFocus(m_last, m_focusSpan->value());
    if (!m_focusStudy.valid) {
        m_focus->clear();
        return;
    }

    PlotSeries rms, d86;
    rms.name = QStringLiteral("RMS radius");
    rms.color = QColor(255, 175, 95);
    d86.name = QStringLiteral("D86 radius");
    d86.color = QColor(120, 200, 255);
    for (const auto& f : m_focusStudy.samples) {
        rms.x.push_back(f.z);
        rms.y.push_back(f.rmsRadius);
        d86.x.push_back(f.z);
        d86.y.push_back(f.d86Radius);
    }
    m_focus->setSeries({std::move(rms), std::move(d86)});
    m_focus->setMarkers({{m_focusStudy.receiverZ, QStringLiteral("receiver"), QColor(150, 220, 140)},
                         {m_focusStudy.bestZ, QStringLiteral("best focus"), QColor(255, 120, 120)}});
    m_focus->setTitle(QStringLiteral("Through focus — best at z = %1 mm (%2 mm from the receiver)")
                          .arg(m_focusStudy.bestZ, 0, 'f', 1)
                          .arg(m_focusStudy.bestZ - m_focusStudy.receiverZ, 0, 'f', 1));

    statusBar()->showMessage(
        QStringLiteral("Best focus at z = %1 mm, smallest RMS radius %2 mm "
                       "(receiver sits at %3 mm)")
            .arg(m_focusStudy.bestZ, 0, 'f', 2)
            .arg(m_focusStudy.bestRms, 0, 'f', 3)
            .arg(m_focusStudy.receiverZ, 0, 'f', 1));
}

// ---- file ------------------------------------------------------------------

void MainWindow::onSaveConfig() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save configuration"), QStringLiteral("optics-setup.json"),
        QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;
    QString err;
    if (!configio::save(path, m_controls->config(), &err))
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Could not save: %1").arg(err));
    else
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 4000);
}

void MainWindow::onLoadConfig() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open configuration"), QString(), QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;
    SimConfig cfg;
    QString err;
    QStringList warnings;
    if (!configio::load(path, cfg, &err, &warnings)) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Could not open: %1").arg(err));
        return;
    }
    m_controls->setConfig(cfg);
    statusBar()->showMessage(QStringLiteral("Loaded %1").arg(path), 4000);
    // A ray file the config names and cannot reopen is the one failure a load
    // must never be quiet about: the source falls back to the analytic emitter,
    // and the run would otherwise trace an LED nobody chose.
    if (!warnings.isEmpty())
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("The configuration loaded, with:\n\n%1")
                                 .arg(warnings.join(QStringLiteral("\n"))));
}

void MainWindow::onLoadMaterialCatalogue() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load an optical material catalogue"), QString(),
        materialfile::fileFilter());
    if (path.isEmpty()) return;

    const auto res = materialfile::load(path);
    if (!res.ok) {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("Could not read %1:\n%2").arg(path, res.error));
        return;
    }
    QString msg = QStringLiteral("Loaded %1 material(s).").arg(res.added);
    if (res.skipped > 0)
        msg += QStringLiteral("\n%1 record(s) used a dispersion formula this reader "
                              "cannot represent and were left out rather than "
                              "approximated.").arg(res.skipped);
    // Named, because a glass catalogue is a list of names and the point of
    // loading one is being able to type the name you wanted.
    if (!res.names.isEmpty())
        msg += QStringLiteral("\n\n%1%2")
                   .arg(res.names.mid(0, 12).join(QStringLiteral(", ")),
                        res.names.size() > 12
                            ? QStringLiteral(", and %1 more").arg(res.names.size() - 12)
                            : QString());
    QMessageBox::information(this, windowTitle(), msg);
    statusBar()->showMessage(
        QStringLiteral("%1 materials from %2").arg(res.added).arg(path), 5000);
}

void MainWindow::onExportIrradianceCsv() {
    if (!m_hasResult) { statusBar()->showMessage(QStringLiteral("Nothing to export yet"), 3000); return; }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export irradiance grid"), QStringLiteral("irradiance.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QString err;
    if (!analysis::writeTextFile(path, analysis::irradianceCsv(m_last), &err))
        QMessageBox::warning(this, windowTitle(), err);
    else
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 4000);
}

void MainWindow::onExportIntensityCsv() {
    if (!m_hasResult) { statusBar()->showMessage(QStringLiteral("Nothing to export yet"), 3000); return; }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export intensity distribution"), QStringLiteral("intensity.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QString err;
    if (!analysis::writeTextFile(path, analysis::intensityCsv(m_last), &err))
        QMessageBox::warning(this, windowTitle(), err);
    else
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 4000);
}

void MainWindow::onExportMetricsCsv() {
    if (!m_hasResult) { statusBar()->showMessage(QStringLiteral("Nothing to export yet"), 3000); return; }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export metrics"), QStringLiteral("metrics.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;

    QString text = analysis::metricsCsv(m_last, analysis::computeSpotMetrics(m_last));
    // A study that has been run belongs in the same file: it is part of the same
    // measurement, and splitting it across three exports invites mismatches.
    if (!m_convergencePoints.empty())
        text += QStringLiteral("\n# convergence sweep\n") + studies::convergenceCsv(m_convergencePoints);
    if (m_focusStudy.valid)
        text += QStringLiteral("\n# through-focus sweep\n") + studies::focusCsv(m_focusStudy);

    QString err;
    if (!analysis::writeTextFile(path, text, &err))
        QMessageBox::warning(this, windowTitle(), err);
    else
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 4000);
}

void MainWindow::onExportPhotometry() {
    if (!m_hasResult) { statusBar()->showMessage(QStringLiteral("Nothing to export yet"), 3000); return; }
    QString why;
    if (!analysis::canExportPhotometry(m_last, &why)) {
        QMessageBox::information(this, QStringLiteral("Cannot export photometry"), why);
        return;
    }
    QString selected;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export photometry"), QStringLiteral("luminaire.ies"),
        QStringLiteral("IES LM-63 (*.ies);;EULUMDAT (*.ldt)"), &selected);
    if (path.isEmpty()) return;

    const bool ldt = path.endsWith(QStringLiteral(".ldt"), Qt::CaseInsensitive) ||
                     selected.startsWith(QStringLiteral("EULUMDAT"));
    const QString name = currentConfig().sceneName();
    const QString text = ldt ? analysis::eulumdat(m_last, name)
                             : analysis::iesLm63(m_last, name);
    QString err;
    if (!analysis::writeTextFile(path, text, &err))
        QMessageBox::warning(this, QStringLiteral("Export failed"), err);
    else
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 4000);
}


// ---- the report -------------------------------------------------------------

std::vector<report::Figure> MainWindow::figuresForReport() const {
    // Two device pixels per logical one, so the figures stay sharp when the
    // document is printed or zoomed.
    constexpr double kScale = 2.0;
    const QSize wide(760, 460);
    std::vector<report::Figure> figures;

    auto add = [&](const QString& title, const QString& caption, const QPixmap& pm) {
        if (pm.isNull()) return;
        report::Figure f;
        f.title   = title;
        f.caption = caption;
        f.image   = pm.toImage();
        figures.push_back(std::move(f));
    };

    add(QStringLiteral("Receiver irradiance"),
        m_heatmap->hasReference()
            ? QStringLiteral("Difference against the pinned run: warm where this design "
                             "gained flux, cool where it lost it.")
            : QStringLiteral("Flux per bin on the receiver, as a density."),
        m_heatmap->renderToPixmap(wide, kScale));
    add(QStringLiteral("Cross-section"),
        QStringLiteral("Through the cut line on the map above."),
        m_profile->renderToPixmap(wide, kScale));
    add(QStringLiteral("Encircled energy"),
        QStringLiteral("Fraction of the receiver flux inside a radius of the centroid, "
                       "with the RMS and D86 radii marked."),
        m_encircled->renderToPixmap(wide, kScale));
    add(QStringLiteral("Far-field intensity"),
        QStringLiteral("The candela distribution the optic radiates into."),
        m_polar->renderToPixmap(wide, kScale));
    add(QStringLiteral("Ray paths"),
        QStringLiteral("A subsample of the traced paths, projected to x-z."),
        m_diagram->renderToPixmap(wide, kScale));
    if (!m_convergencePoints.empty())
        add(QStringLiteral("Convergence"),
            QStringLiteral("Efficiency against ray count, with one standard error on each "
                           "point. Where the bars start overlapping is where more rays "
                           "stop buying anything."),
            m_convergence->renderToPixmap(wide, kScale));
    if (m_focusStudy.valid)
        add(QStringLiteral("Through focus"),
            QStringLiteral("Spot size against receiver position, propagated analytically "
                           "from one trace."),
            m_focus->renderToPixmap(wide, kScale));
    if (!m_sweepPoints.empty())
        add(QStringLiteral("Parameter sweep"),
            QStringLiteral("The metric against one of the optic's own dimensions, with the "
                           "error bars that say whether a bump is the design or the noise."),
            m_sweepPlot->renderToPixmap(wide, kScale));
    if (m_toleranceStudy.valid)
        add(QStringLiteral("Yield"),
            QStringLiteral("The simulated production run, against the specification and "
                           "the nominal design."),
            m_tolPlot->renderToPixmap(wide, kScale));
    if (!m_mtfPlot->isEmpty())
        add(QStringLiteral("Modulation transfer"),
            QStringLiteral("Contrast against spatial frequency, with the diffraction limit "
                           "where an aperture was given."),
            m_mtfPlot->renderToPixmap(wide, kScale));
    if (m_optimisation.valid)
        add(QStringLiteral("Optimisation"),
            QStringLiteral("Every design the search evaluated, and the best it had found "
                           "at each step."),
            m_optPlot->renderToPixmap(wide, kScale));
    return figures;
}

void MainWindow::onExportReport() {
    if (!m_hasResult) {
        statusBar()->showMessage(QStringLiteral("Run a simulation first"), 3000);
        return;
    }
    const QString suggested =
        QStringLiteral("%1 report.html").arg(currentConfig().sceneName());
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export report"), suggested, QStringLiteral("HTML (*.html *.htm)"));
    if (path.isEmpty()) return;

    bool ok = false;
    const QString notes = QInputDialog::getText(
        this, QStringLiteral("Report"),
        QStringLiteral("A note for the top of the report (optional):"),
        QLineEdit::Normal, QString(), &ok);

    report::Content content;
    content.config      = currentConfig();
    content.result      = m_last;
    content.metrics     = analysis::computeSpotMetrics(m_last);
    content.figures     = figuresForReport();
    content.focus       = m_focusStudy;
    content.convergence = m_convergencePoints;
    content.sweep       = m_sweepPoints;
    content.sweepSlot   = m_sweepSlot;
    content.sweepMetric = m_sweepMetricUsed;
    content.optimisation   = m_optimisation;
    content.optimisedSlots = m_optimisedSlots;
    content.objective      = m_objective;
    content.tolerance      = m_toleranceStudy;
    content.mtf            = analysis::modulationTransfer(m_last);
    content.wavefront      = analysis::wavefrontError(m_last);
    content.hasComparison  = m_hasPinned;
    content.comparison     = m_pinned;
    content.comparisonLabel = m_pinnedLabel;
    if (ok) content.notes = notes;

    QString err;
    if (!report::write(path, content, &err))
        QMessageBox::warning(this, QStringLiteral("Export failed"), err);
    else
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 5000);
}

// ---- importing a customer's geometry ----------------------------------------

void MainWindow::onImportCad() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import CAD"), QString(), cadimport::fileFilter());
    if (path.isEmpty()) return;

    // One dialog rather than three questions in a row, and it shows the answer
    // to the one that used to be unaskable: does the tracer's model of a solid
    // actually hold on this geometry? Units, illumination axis, material and a
    // per-part audit all move together, because changing the unit changes what
    // the geometry is and an audit of the previous read would be worse than none.
    ImportDialog dlg(path, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const cadimport::ImportResult& result = dlg.result();
    if (!result.ok) {
        QMessageBox::warning(this, QStringLiteral("Import failed"), result.error);
        return;
    }
    const QString axisName = dlg.axisName();

    double extent = 0.0;
    for (int a = 0; a < 3; ++a)
        extent = std::max(extent, result.bboxMax[a] - result.bboxMin[a]);

    // The parts, a receiver beyond them and the source placement that aims at
    // them: a file describes a body, not an experiment, and a trace needs all
    // three. Everything downstream of here -- meshing, the hierarchy, the
    // trace, the analysis -- is the same code every built-in scene goes
    // through, on this geometry rather than on the registry's.
    auto setup = std::make_shared<GeometryProvider::SceneSetup>(
        cadimport::makeScene(result, dlg.materialName(), dlg.reflective(),
                             m_controls->config().detectorBins, dlg.axis()));
    if (setup->surfaces.empty()) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QStringLiteral("Nothing in the file could be turned into "
                                            "traceable geometry."));
        return;
    }
    setup->label       = QFileInfo(path).completeBaseName();
    setup->description = QStringLiteral("Imported %1: %2, lit along %3. %4")
                             .arg(result.format)
                             .arg(result.auditSummary())
                             .arg(axisName.left(2))
                             .arg(dlg.reflective()
                                      ? QStringLiteral("Every part is a mirror.")
                                      : QStringLiteral("Every part is %1 with Fresnel "
                                                       "splitting.").arg(dlg.materialName()));

    // Optical edits belonged to the surfaces of whatever was on screen before,
    // and an override is an index into that list: carried over, they would land
    // on unrelated faces of the imported part.
    m_overrides.clear();
    refreshSurfaceTreeBadges();

    m_imported        = std::move(setup);
    m_showingImported = true;
    m_pickedSurface   = -1;
    // The viewport belongs to the import now. A geometry build that was queued
    // or already in flight for the scene list would otherwise land on top of it
    // and put the previous shape back on screen.
    m_geometryTimer->stop();
    m_geometryGen   = 0;
    m_haveRequested = false;
    updateSurfacePanel();
    m_controls->setImportedGeometry(m_imported->label);
    refreshDerivedQuantities();
    m_view3d->setScene(GeometryProvider::Scene::Count, m_imported->surfaces);
    rebuildSurfaceTree();
    refreshSourceGlyphs();
    m_view3d->resetView();
    // The rays on screen were traced through the previous geometry.
    m_view3d->setRays({});
    statusBar()->showMessage(
        QStringLiteral("%1 — %2 part(s) spanning %3 mm. Run traces this, not %4.")
            .arg(m_imported->label)
            .arg(result.shapes.size())
            .arg(extent, 0, 'f', 1)
            .arg(GeometryProvider::info(m_controls->scene()).name), 12000);
}

void MainWindow::onExportImage() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export view"), QStringLiteral("view.png"),
        QStringLiteral("PNG (*.png)"));
    if (path.isEmpty()) return;

    // Rendered at twice the on-screen size: these end up in reports, and a
    // screen-resolution plot looks ragged printed.
    constexpr double kScale = 2.0;
    QPixmap pm;
    switch (m_tabs->currentIndex()) {
    case 0:
        // The OCCT viewport draws straight into a native window, so it dumps its
        // own framebuffer rather than going through a QPixmap.
        if (m_view3d->saveImage(path))
            statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 4000);
        else
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("The 3D viewport could not be captured."));
        return;
    case 1: pm = m_diagram->renderToPixmap(m_diagram->size(), kScale); break;
    case 2: pm = m_heatmap->renderToPixmap(m_heatmap->size(), kScale); break;
    case 3: pm = m_polar->renderToPixmap(m_polar->size(), kScale); break;
    case 4: {
        // Both studies side by side: they are two halves of one answer about
        // whether the run can be trusted and where the focus actually is.
        const QPixmap left  = m_convergence->renderToPixmap(m_convergence->size(), kScale);
        const QPixmap right = m_focus->renderToPixmap(m_focus->size(), kScale);
        pm = QPixmap(left.width() + right.width(), std::max(left.height(), right.height()));
        pm.setDevicePixelRatio(kScale);
        pm.fill(QColor(18, 18, 22));
        QPainter p(&pm);
        p.drawPixmap(0, 0, left);
        p.drawPixmap(left.width(), 0, right);
        break;
    }
    default: break;
    }
    if (pm.isNull()) { statusBar()->showMessage(QStringLiteral("Nothing to export"), 3000); return; }
    if (!pm.save(path, "PNG"))
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Could not write %1").arg(path));
    else
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 4000);
}

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
