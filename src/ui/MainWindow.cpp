#include "MainWindow.h"

#include "ControlsPanel.h"
#include "HeatmapWidget.h"
#include "OcctViewWidget.h"
#include "PlotWidget.h"
#include "PolarPlotWidget.h"
#include "RayDiagramWidget.h"
#include "core/Analysis.h"
#include "core/ConfigIO.h"
#include "core/SimulationWorker.h"
#include "core/StudyWorker.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
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
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace {

// Geometry rebuilds run OCCT tessellation and a BVH build. That is fast, but
// not per-keystroke fast, so a spin box drag is coalesced into one rebuild.
constexpr int kGeometryDebounceMs = 180;

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

    m_geometryTimer = new QTimer(this);
    m_geometryTimer->setSingleShot(true);
    m_geometryTimer->setInterval(kGeometryDebounceMs);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildViewerTab(),     QStringLiteral("3D View"));
    m_tabs->addTab(buildDiagramTab(),    QStringLiteral("Ray Diagram (X-Z)"));
    m_tabs->addTab(buildIrradianceTab(), QStringLiteral("Irradiance"));
    m_tabs->addTab(buildIntensityTab(),  QStringLiteral("Intensity"));
    m_tabs->addTab(buildStudiesTab(),    QStringLiteral("Studies"));

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

    auto* left = new QWidget(this);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(m_controls, 1);
    lv->addWidget(m_result, 0);

    auto* central = new QWidget(this);
    auto* main = new QHBoxLayout(central);
    main->setContentsMargins(6, 6, 6, 6);
    main->addWidget(left, 0);
    main->addWidget(right, 1);
    setCentralWidget(central);

    buildMenus();
    statusBar()->showMessage(QStringLiteral("Ready"));

    connect(m_controls, &ControlsPanel::runRequested,    this, &MainWindow::onRun);
    connect(m_controls, &ControlsPanel::cancelRequested, this, &MainWindow::onCancel);
    connect(m_controls, &ControlsPanel::sceneChanged,    this, &MainWindow::onSceneChanged);
    connect(m_controls, &ControlsPanel::geometryChanged, this, &MainWindow::onGeometryChanged);
    connect(m_geometryTimer, &QTimer::timeout, this, &MainWindow::rebuildGeometryView);

    connect(m_worker, &SimulationWorker::progress,    this, &MainWindow::onProgress);
    connect(m_worker, &SimulationWorker::resultReady, this, &MainWindow::onResult);
    connect(m_study,  &StudyWorker::progress,         this, &MainWindow::onProgress);
    connect(m_study,  &StudyWorker::convergenceReady, this, &MainWindow::onConvergenceReady);

    connect(m_view3d, &OcctViewWidget::surfacePicked, this, &MainWindow::onSurfacePicked);
    connect(m_heatmap, &HeatmapWidget::cutMoved, this, &MainWindow::onCutMoved);

    // Hand the viewport keyboard focus when its tab comes up, so WASD works
    // without having to click into it first.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) m_view3d->setFocus(Qt::OtherFocusReason);
    });

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

    auto* bar1 = new QHBoxLayout;
    bar1->setContentsMargins(4, 2, 4, 0);
    bar1->addWidget(fit);
    bar1->addWidget(reset);
    bar1->addWidget(showRays);
    bar1->addWidget(perspective);
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
                                       "WASD walk · click a surface to inspect it"), this), 1);

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

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
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
    file->addAction(QStringLiteral("Export current &view (PNG)..."),
                    this, &MainWindow::onExportImage);
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* run = menuBar()->addMenu(QStringLiteral("&Run"));
    run->addAction(QStringLiteral("&Trace"), QKeySequence(Qt::Key_F5), this, &MainWindow::onRun);
    run->addAction(QStringLiteral("&Cancel"), QKeySequence(Qt::Key_Escape),
                   this, &MainWindow::onCancel);
    run->addSeparator();
    run->addAction(QStringLiteral("Con&vergence sweep"), this, &MainWindow::onRunConvergence);
    run->addAction(QStringLiteral("Through-&focus sweep"), this, &MainWindow::onFocusSweep);
}

// ---- scene / geometry ------------------------------------------------------

void MainWindow::onSceneChanged() {
    rebuildGeometryView();
    // The old scene's rays do not belong to this one.
    m_view3d->setRays({});
}

void MainWindow::onGeometryChanged() {
    m_geometryTimer->start();
}

void MainWindow::rebuildGeometryView() {
    const SimConfig cfg = m_controls->config();
    try {
        m_sceneData = Simulation::dataFor(cfg);
        m_view3d->setScene(cfg.scene, m_sceneData->surfaces);
        statusBar()->showMessage(
            QStringLiteral("%1 — %2 triangles")
                .arg(GeometryProvider::info(cfg.scene).name)
                .arg(m_sceneData->scene.triangles().size()), 4000);
    } catch (const Standard_Failure& e) {
        // A parameter combination the kernel cannot build should report itself
        // rather than take the window down.
        statusBar()->showMessage(
            QStringLiteral("Geometry failed: %1").arg(QString::fromUtf8(e.GetMessageString())));
    }
}

// ---- running ---------------------------------------------------------------

void MainWindow::onRun() {
    if (m_worker->isRunning() || m_study->isRunning()) return;
    m_controls->setRunning(true);
    m_result->setText(QStringLiteral("Running..."));
    m_worker->startRun(m_controls->config());
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
    const double eff = 100.0 * res.efficiency;
    const double raysPerSec = res.traceSeconds > 0.0
                                  ? double(res.raysEmitted) / res.traceSeconds
                                  : 0.0;

    QString text;
    if (res.cancelled) text += QStringLiteral("CANCELLED - partial result\n");
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

    html += QStringLiteral("<b>Energy budget</b><table>");
    html += row(QStringLiteral("On the receiver"), pct(r.fluxDetector));
    html += row(QStringLiteral("Absorbed at surfaces"),
                pct(r.fluxAbsorbed - r.fluxBulkAbsorbed));
    html += row(QStringLiteral("Absorbed in the bulk"), pct(r.fluxBulkAbsorbed),
                QStringLiteral("Beer-Lambert"));
    html += row(QStringLiteral("Escaped the scene"), pct(r.fluxEscaped));
    html += row(QStringLiteral("Truncated"), pct(r.fluxTruncated),
                QStringLiteral("energy cutoff / depth limit"));
    html += row(QStringLiteral("Accounted"), pct(r.fluxAccounted()));
    html += QStringLiteral("</table>");

    if (r.intensity.valid()) {
        html += QStringLiteral("<br><b>Far field</b><table>");
        html += row(QStringLiteral("Peak intensity"),
                    QStringLiteral("%1 W/sr").arg(r.intensity.peak, 0, 'g', 4));
        html += row(QStringLiteral("Beam FWHM"),
                    QStringLiteral("%1°").arg(r.intensity.fwhmDeg, 0, 'f', 1));
        html += QStringLiteral("</table>");
    }

    html += QStringLiteral("</td><td valign='top'>");

    if (m.valid) {
        html += QStringLiteral("<b>Spot on the receiver</b><table>");
        html += row(QStringLiteral("Peak irradiance"),
                    QStringLiteral("%1 W/mm²").arg(m.peak, 0, 'g', 4));
        html += row(QStringLiteral("Mean (lit bins)"),
                    QStringLiteral("%1 W/mm²").arg(m.mean, 0, 'g', 4));
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
        html += QStringLiteral("</table>");
    }

    html += QStringLiteral("</td></tr></table></div>");
    return html;
}

void MainWindow::onSurfacePicked(int index) {
    if (!m_sceneData || index < 0 || index >= int(m_sceneData->surfaces.size())) {
        statusBar()->showMessage(QStringLiteral("No surface under the cursor"), 3000);
        return;
    }
    const OpticalSurface& s = m_sceneData->surfaces[std::size_t(index)];

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
    if (!configio::load(path, cfg, &err)) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Could not open: %1").arg(err));
        return;
    }
    m_controls->setConfig(cfg);
    statusBar()->showMessage(QStringLiteral("Loaded %1").arg(path), 4000);
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
    QMainWindow::closeEvent(event);
}
