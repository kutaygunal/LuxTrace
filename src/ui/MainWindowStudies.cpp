// The parameter-study side of the window: the Studies, Design and Tolerance
// tabs, and every handler that fills them.
//
// One area, one file. A sweep, an optimisation, a tolerance run and a
// convergence check are all the same shape -- configure, hand to StudyWorker,
// receive a result on the UI thread, plot it -- and reading them beside each
// other is how the shape stays consistent. None of it is on the path a plain
// trace takes.
#include "MainWindowInternal.h"

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
    // These follow from the tutorial's dimensions. Once the scene has been
    // assembled or imported they describe an optic that is not being traced,
    // which is exactly the kind of leftover that makes an edited scene look
    // like it kept the previous shape's behaviour.
    if (!m_document.linkedToTutorial()) {
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
