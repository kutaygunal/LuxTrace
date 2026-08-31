// The Appearance tab: the path-traced preview, and exporting a picture of it.
//
// Kept apart from the rest of the window because the boundary matters. Nothing
// here is a measurement -- the preview is OCCT's own RGB path tracer fed a
// translation of our materials -- and every image it exports carries that
// sentence in its caption. Holding the translation, the caption and the export
// in one file is what keeps the three from drifting apart.
#include "MainWindowInternal.h"

// ---- the appearance preview ------------------------------------------------
//
// What the part looks like switched on, path-traced on the GPU through OCCT's
// own renderer.
//
// It is a *preview*, and the tab says so in the strip under it rather than in a
// manual nobody opens. OCCT's path tracer is an RGB graphics renderer with a
// two-layer BSDF of its own: it does not read the scatter model, the coating
// stack, the Sellmeier curve or the polarisation state the tracer reads, its
// output is tone-mapped sRGB rather than cd/m^2, and unlike every other output
// this application produces it is not deterministic. Shipping it as an analysis
// result would put two light-transport models that disagree with each other
// inside one application. Shipping it as a picture, labelled, gives a design
// review the image it wants and costs nothing that is already true.
QWidget* MainWindow::buildAppearanceTab() {
    m_appearance = new AppearanceView(this);

    auto* render  = new QPushButton(QStringLiteral("Restart"), this);
    auto* fit     = new QPushButton(QStringLiteral("Fit (F)"), this);
    auto* save    = new QPushButton(QStringLiteral("Save PNG..."), this);
    render->setToolTip(QStringLiteral(
        "Throws the accumulated image away and starts again. Every camera move "
        "does this by itself -- the picture is an average over frames taken "
        "from one viewpoint, so moving the viewpoint invalidates all of them."));

    auto* trace = new QCheckBox(QStringLiteral("Path trace"), this);
    trace->setChecked(true);
    trace->setToolTip(QStringLiteral(
        "Off gives the rasterized preview, which reads the same materials "
        "through the PBR model: the same scene at lower fidelity, drawn in one "
        "frame instead of accumulated over hundreds."));

    auto* detectors = new QCheckBox(QStringLiteral("Show receivers"), this);
    detectors->setToolTip(QStringLiteral(
        "A receiver is a measurement plane, not a part, and in a render it sits "
        "in front of the optic and blocks the shot. Switch it on to frame "
        "against it."));

    auto* lighting = new QComboBox(this);
    lighting->addItems({QStringLiteral("Scene emitters"), QStringLiteral("Studio"),
                        QStringLiteral("Sky")});
    lighting->setToolTip(QStringLiteral(
        "Scene emitters lights the picture with the optic's own sources and "
        "nothing else -- what a luminaire looks like switched on. Studio is a "
        "three-point rig, for reading the shape of a part whose source is not "
        "the subject. Sky puts a procedural dome behind the part and lights it "
        "with a sun from the same direction, for the outdoor look and for "
        "giving a metal surface something bright to reflect. "
        "A scene with no source of its own falls back to Studio, because a part "
        "lit by nothing renders black."));

    // Which receiver's traced distribution drives the emission, if any. This is
    // the control that turns the preview from a picture of the geometry into a
    // picture of the answer: with a receiver nominated, the exit surface glows
    // with the distribution the run computed rather than uniformly, and
    // changing the source moves the bright region.
    m_appearanceEmission = new QComboBox(this);
    m_appearanceEmission->addItem(QStringLiteral("Sources only"), -1);
    m_appearanceEmission->setToolTip(QStringLiteral(
        "Drive the glow of a receiver plane from the flux the last run put on "
        "it -- a diffuser plate, a light-guide exit face. The distribution is "
        "the trace's; the brightness scale is not photometric, and the caption "
        "under the view says what the total flux actually was."));

    // The four shots a design review asks for. A render is a photograph, and
    // "front elevation" is a thing a reviewer asks for by name rather than by
    // dragging until it looks about right.
    auto* camera = new QComboBox(this);
    camera->addItems({QStringLiteral("Iso"), QStringLiteral("Front"),
                      QStringLiteral("Side"), QStringLiteral("Top")});
    camera->setToolTip(QStringLiteral(
        "Stands the camera at one of the four standard views and frames the "
        "scene. Front looks along -y at the x-z plane the ray diagram draws, "
        "side looks along x, top looks down z."));

    auto* tone = new QComboBox(this);
    tone->addItems({QStringLiteral("Filmic"), QStringLiteral("Linear")});
    tone->setToolTip(QStringLiteral(
        "Filmic rolls the highlights off the way a camera does, which is what "
        "keeps a bright emitter from clipping to a white disc. Linear shows the "
        "estimate as it stands."));

    auto* exposure = new QDoubleSpinBox(this);
    exposure->setRange(-6.0, 6.0);
    exposure->setSingleStep(0.25);
    exposure->setValue(0.0);
    exposure->setPrefix(QStringLiteral("EV "));
    exposure->setToolTip(QStringLiteral("Exposure, in stops."));

    auto* white = new QDoubleSpinBox(this);
    white->setRange(0.1, 8.0);
    white->setSingleStep(0.25);
    white->setValue(1.0);
    white->setPrefix(QStringLiteral("white "));
    white->setToolTip(QStringLiteral(
        "The radiance filmic tone mapping rolls off to white. Raise it when a "
        "bright emitter is clipping the whole frame; it does nothing under "
        "linear tone mapping."));

    auto* depth = new QSpinBox(this);
    depth->setRange(1, 32);
    depth->setValue(8);
    depth->setPrefix(QStringLiteral("depth "));
    depth->setToolTip(QStringLiteral(
        "Maximum path length. OCCT defaults to 3, which is too few for glass: a "
        "ball lens needs a refraction in, a refraction out, and something left "
        "over to hit."));

    auto* budget = new QSpinBox(this);
    budget->setRange(16, 8192);
    budget->setSingleStep(64);
    budget->setValue(512);
    budget->setPrefix(QStringLiteral("samples "));
    budget->setToolTip(QStringLiteral(
        "How many frames to accumulate before stopping. More is quieter; the "
        "renderer goes idle when it gets there, so a finished image costs no "
        "GPU at all."));

    // Depth of field. Path tracing only, and off by default: every other view
    // in the application is a pinhole camera, and a render that quietly blurs
    // the part it was opened to inspect is a render that gets mistrusted.
    auto* aperture = new QDoubleSpinBox(this);
    aperture->setRange(0.0, 500.0);
    aperture->setDecimals(2);
    aperture->setSingleStep(0.25);
    aperture->setValue(0.0);
    aperture->setPrefix(QStringLiteral("aperture "));
    aperture->setSuffix(QStringLiteral(" mm"));
    aperture->setSpecialValueText(QStringLiteral("aperture pinhole"));
    aperture->setToolTip(QStringLiteral(
        "Radius of the lens the camera looks through, in scene units. Zero is a "
        "pinhole and everything is sharp. Opening it costs samples -- the "
        "tracer is sampling a real aperture, not blurring afterwards -- so a "
        "wide aperture needs a bigger budget to settle."));

    auto* focus = new QDoubleSpinBox(this);
    focus->setRange(0.0, 100000.0);
    focus->setDecimals(1);
    focus->setSingleStep(5.0);
    focus->setValue(0.0);
    focus->setPrefix(QStringLiteral("focus "));
    focus->setSuffix(QStringLiteral(" mm"));
    focus->setSpecialValueText(QStringLiteral("focus on the scene"));
    focus->setToolTip(QStringLiteral(
        "How far in front of the camera the sharp plane sits. Left at zero it "
        "tracks the middle of the scene as the camera moves, which is what "
        "keeps the subject sharp through an orbit; set it to put the sharp "
        "plane somewhere else deliberately."));

    m_appearanceState = new QLabel(this);
    m_appearanceState->setWordWrap(true);
    m_appearanceState->setText(QStringLiteral(
        "Appearance preview — not a photometric result. RGB path tracing, no "
        "dispersion, no polarisation, no cd/m². Open the tab to start it."));

    // Two rows, because they answer two different questions: where the camera
    // stands and what is in front of it, then how the picture is developed.
    auto* scene = new QHBoxLayout;
    scene->setContentsMargins(0, 0, 0, 0);
    scene->addWidget(render);
    scene->addWidget(fit);
    scene->addWidget(camera);
    scene->addWidget(trace);
    scene->addWidget(detectors);
    scene->addWidget(new QLabel(QStringLiteral("Light"), this));
    scene->addWidget(lighting);
    scene->addWidget(new QLabel(QStringLiteral("Emit from"), this));
    scene->addWidget(m_appearanceEmission, 1);

    auto* image = new QHBoxLayout;
    image->setContentsMargins(0, 0, 0, 0);
    image->addWidget(new QLabel(QStringLiteral("Tone"), this));
    image->addWidget(tone);
    image->addWidget(exposure);
    image->addWidget(white);
    image->addWidget(depth);
    image->addWidget(budget);
    image->addStretch(1);

    // Depth of field is its own question, and putting it on its own row keeps
    // the tone row from forcing the whole tab (and the window) wider than a
    // 1920 screen. Aperture and focus only matter when path tracing.
    auto* dof = new QHBoxLayout;
    dof->setContentsMargins(0, 0, 0, 0);
    dof->addWidget(new QLabel(QStringLiteral("Depth of field"), this));
    dof->addWidget(aperture);
    dof->addWidget(focus);
    dof->addStretch(1);
    dof->addWidget(save);

    auto* page = new QWidget(this);
    auto* col  = new QVBoxLayout(page);
    col->setContentsMargins(6, 6, 6, 6);
    col->setSpacing(4);
    col->addLayout(scene);
    col->addLayout(image);
    col->addLayout(dof);
    col->addWidget(m_appearance, 1);
    col->addWidget(m_appearanceState, 0);

    connect(render, &QPushButton::clicked, m_appearance, &AppearanceView::restart);
    connect(fit,    &QPushButton::clicked, m_appearance, &AppearanceView::fitAll);
    connect(trace, &QCheckBox::toggled, this, [this](bool on) {
        m_appearance->setPathTracing(on);
    });
    connect(detectors, &QCheckBox::toggled, this, [this](bool on) {
        m_appearance->setShowDetectors(on);
    });
    connect(camera, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        m_appearance->setCamera(i == 1   ? AppearanceView::Camera::Front
                                : i == 2 ? AppearanceView::Camera::Side
                                : i == 3 ? AppearanceView::Camera::Top
                                         : AppearanceView::Camera::Iso);
    });
    connect(aperture, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double r) { m_appearance->setApertureRadius(r); });
    connect(focus, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double d) { m_appearance->setFocalDistance(d); });
    connect(tone, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        m_appearance->setToneMapping(i == 0 ? AppearanceView::ToneMapping::Filmic
                                            : AppearanceView::ToneMapping::Disabled);
    });
    connect(lighting, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        m_appearance->setLighting(i == 1 ? AppearanceView::Lighting::Studio
                                : i == 2 ? AppearanceView::Lighting::Sky
                                         : AppearanceView::Lighting::SceneEmitters);
    });
    connect(m_appearanceEmission, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i) {
                if (i < 0) return;
                m_appearance->setRadianceSource(
                    m_appearanceEmission->itemData(i).toInt());
                refreshAppearanceCaption();
            });
    connect(exposure, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double ev) { m_appearance->setExposure(ev); });
    connect(white, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double w) { m_appearance->setWhitePoint(w); });
    connect(depth, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int d) { m_appearance->setRayDepth(d); });
    connect(budget, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int n) { m_appearance->setSampleBudget(n); });

    // The sample count, where a user can see it. A progressive renderer that
    // does not say how far along it is cannot be told from one that has stopped.
    connect(m_appearance, &AppearanceView::progress, this, [this](int n, int of) {
        m_appearanceSamples = n;
        m_appearanceBudget  = of;
        refreshAppearanceCaption();
    });

    // A machine that cannot path-trace is told so, and told what it has. OCCT
    // falls back to rasterization without a word otherwise, which reads as "the
    // render button does nothing".
    connect(m_appearance, &AppearanceView::capabilityDetermined, this,
            [this, trace, aperture, focus](bool ok, const QString& why) {
                if (ok) return;
                trace->setChecked(false);
                trace->setEnabled(false);
                // The rasterized preview has no aperture to sample across, so
                // the controls for one are disabled rather than left looking
                // like something that is broken.
                aperture->setEnabled(false);
                focus->setEnabled(false);
                if (m_appearanceState) m_appearanceState->setText(why);
            });

    connect(m_appearance, &AppearanceView::lightingFailed, this, [this](const QString& why) {
        statusBar()->showMessage(why, 10000);
    });

    connect(save, &QPushButton::clicked, this, &MainWindow::exportAppearanceImage);

    return page;
}

// The receivers the last run measured, offered as things the render can glow
// with. Rebuilt whenever a run finishes, because a scene edit can add or remove
// one and an index into a list that has changed is a picture of the wrong plane.
// What the Appearance view should be drawing right now. Cheap when the tab is
// hidden -- the view defers its rebuild until it is on screen -- so callers do
// not have to ask whether anybody is looking.
//
// The *document's* surfaces, not `currentSurfaces()`. Those two differ in
// exactly the case that matters here: `currentSurfaces()` returns the compiled
// trace scene, which is rebuilt only when the geometry changes, and an optics
// edit deliberately does not rebuild it -- a reflectance is a ray-time property
// and re-tessellating for one would be waste. So the traced copy carries the
// optics the surface had when its mesh was last built, and handing that to the
// renderer showed the old material no matter how often it was handed over.
// `m_document.compile()` runs on every optics edit and is the live answer.
void MainWindow::refreshAppearanceSurfaces() {
    if (!m_appearance) return;
    if (m_compiled.setup) m_appearance->setSurfaces(m_compiled.setup->surfaces);
    else                  m_appearance->setSurfaces(currentSurfaces());
}

void MainWindow::refreshAppearanceReceivers() {
    if (!m_appearanceEmission || !m_appearance) return;

    const int wanted = m_appearance->radianceSource();

    QSignalBlocker block(m_appearanceEmission);
    m_appearanceEmission->clear();
    m_appearanceEmission->addItem(QStringLiteral("Sources only"), -1);

    if (m_hasResult) {
        for (std::size_t i = 0; i < m_last.detectors.size(); ++i) {
            const DetectorFrame& d = m_last.detectors[i];
            const QString name = d.label.isEmpty()
                                     ? QStringLiteral("Receiver %1").arg(i + 1)
                                     : d.label;
            m_appearanceEmission->addItem(name, int(i));
        }
    }

    const int row = m_appearanceEmission->findData(wanted);
    m_appearanceEmission->setCurrentIndex(row >= 0 ? row : 0);
    if (row < 0 && wanted >= 0) m_appearance->setRadianceSource(-1);

    // Nothing to nominate until something has been traced.
    m_appearanceEmission->setEnabled(m_appearanceEmission->count() > 1);
    refreshAppearanceCaption();
}

// The line under the render. It says three things at once, and has to: how far
// the estimate has got, what the picture is being lit by, and what the picture
// is not. The last of those is the reason it is a permanent strip rather than a
// tooltip -- a render that looks like a measurement, beside six tabs that are
// measurements, is a mistake waiting to be made by somebody who joined the
// project after this was written.
void MainWindow::refreshAppearanceCaption() {
    if (!m_appearanceState || !m_appearance) return;
    // The wording is in AppearanceExport rather than here, because the same
    // three sentences have to reach an exported PNG and a report figure that
    // have no strip under them, and a disclaimer that exists as three separate
    // string literals is one that survives two of the next three edits.
    m_appearanceState->setText(appearance::liveCaption(
        m_appearanceSamples, m_appearanceBudget,
        appearance::distributionNote(m_appearance->radianceMap(), m_last.unit)));
}

// The render, offscreen, at a resolution the window never has to be.
//
// This is the whole of what phase 5 adds to the picture itself: the on-screen
// estimate is tied to the size of a widget, and a design review wants the image
// at the size of a page. The offscreen path runs its own accumulation loop
// against its own buffer, so what comes back is converged at the size asked for
// rather than a single noisy sample of it.
void MainWindow::exportAppearanceImage() {
    if (!m_appearance) return;

    // Whatever was asked for last time, held to what can actually be rendered
    // and framed like the view unless the user says otherwise.
    appearance::ExportRequest request = appearance::normaliseExport(
        m_appearanceExport.width, 0,
        m_appearanceExport.samples > 0 ? m_appearanceExport.samples
                                       : m_appearance->sampleBudget(),
        m_appearance->viewAspect());

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Export the appearance preview"));

    auto* width = new QSpinBox(&dialog);
    width->setRange(appearance::kMinExportDimension, appearance::kMaxExportDimension);
    width->setValue(request.width);
    width->setSuffix(QStringLiteral(" px"));

    auto* height = new QSpinBox(&dialog);
    height->setRange(appearance::kMinExportDimension, appearance::kMaxExportDimension);
    height->setValue(request.height);
    height->setSuffix(QStringLiteral(" px"));

    auto* samples = new QSpinBox(&dialog);
    samples->setRange(appearance::kMinExportSamples, appearance::kMaxExportSamples);
    samples->setSingleStep(64);
    samples->setValue(request.samples);
    samples->setToolTip(QStringLiteral(
        "Frames to accumulate before the image is read back. The offscreen "
        "buffer starts its estimate from nothing, so this is independent of "
        "whatever the window has reached -- and it is the whole cost of the "
        "export."));

    auto* note = new QLabel(
        QStringLiteral(
            "Rendered offscreen, so the resolution is not tied to the window. "
            "The caption goes into the file's description field with it: the "
            "image is an appearance preview and not a photometric result, and "
            "a PNG that leaves here without saying so will be read as one."),
        &dialog);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#8a8f9c; font-size:11px;"));
    note->setMaximumWidth(360);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Width"), width);
    form->addRow(QStringLiteral("Height"), height);
    form->addRow(QStringLiteral("Samples"), samples);

    auto* ok     = new QPushButton(QStringLiteral("Render and save..."), &dialog);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), &dialog);
    ok->setDefault(true);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);

    auto* column = new QVBoxLayout(&dialog);
    column->addLayout(form);
    column->addWidget(note);
    column->addLayout(buttons);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    request = appearance::normaliseExport(width->value(), height->value(),
                                          samples->value(), m_appearance->viewAspect());
    m_appearanceExport = request;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save the appearance preview"),
        QStringLiteral("%1 appearance.png").arg(currentConfig().sceneName()),
        QStringLiteral("PNG image (*.png);;JPEG image (*.jpg)"));
    if (path.isEmpty()) return;

    // A path-traced frame at export resolution is not instant, and a window
    // that stops answering for a minute is indistinguishable from one that has
    // crashed. The dialog also gives the loop somewhere to be cancelled from,
    // which hands back the estimate as it stands rather than nothing.
    QProgressDialog progress(QStringLiteral("Rendering %1 x %2...")
                                 .arg(request.width).arg(request.height),
                             QStringLiteral("Stop"), 0, request.samples, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);

    QImage image;
    int    rendered = 0;
    const bool wrote = m_appearance->renderToImage(
        image, request, [&](int done, int total) {
            rendered = done;
            progress.setMaximum(total);
            progress.setValue(done);
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents |
                                            QEventLoop::AllEvents);
            return !progress.wasCanceled();
        });
    progress.reset();

    if (!wrote || image.isNull()) {
        QMessageBox::warning(
            this, windowTitle(),
            QStringLiteral("The appearance preview could not be rendered at %1 x %2. "
                           "The renderer could not allocate an offscreen buffer that "
                           "size; a smaller one will work.")
                .arg(request.width).arg(request.height));
        return;
    }

    // What the image actually got, not what was asked for -- a cancelled render
    // is a real picture at a sample count that has to be stated rather than
    // rounded up to the one on the dialog.
    appearance::ExportRequest actual = request;
    actual.width   = image.width();
    actual.height  = image.height();
    actual.samples = std::max(1, rendered);

    const QString caption = appearance::exportCaption(
        actual, appearance::distributionNote(m_appearance->radianceMap(), m_last.unit));

    // The caption travels inside the file. A PNG description field is read by
    // every image viewer worth the name, and it is the only place a caption can
    // be attached to an image that is about to be pasted into a slide.
    image.setText(QStringLiteral("Description"), caption);
    image.setText(QStringLiteral("Software"),
                  QStringLiteral("LuxTrace %1").arg(
                      QCoreApplication::applicationVersion().isEmpty()
                          ? QStringLiteral("1.0.0")
                          : QCoreApplication::applicationVersion()));

    if (image.save(path))
        statusBar()->showMessage(
            QStringLiteral("Wrote %1 — %2").arg(QFileInfo(path).fileName(), caption), 10000);
    else
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(path), 6000);
}

// The render as a report figure.
//
// Rendered fresh at figure size rather than scraped off the screen, for the
// same reason every other figure is: a report is printed and zoomed, and the
// window is whatever size it happened to be. Empty where the tab has never been
// opened -- there is no GL context to render with until it has been shown once,
// and a report is not the place to go and make one.
QImage MainWindow::appearanceFigure(appearance::ExportRequest& shot) const {
    if (!m_appearance) return QImage();

    // The size the other figures are rendered at, and a sample count that keeps
    // a report export to a few seconds rather than a few minutes. Stated in the
    // caption either way, so a reader can see the picture is a preview at a
    // modest budget rather than a converged one.
    constexpr int kReportSamples = 192;
    const appearance::ExportRequest request =
        appearance::normaliseExport(1520, 920, kReportSamples, m_appearance->viewAspect());

    QProgressDialog progress(QStringLiteral("Rendering the appearance preview..."),
                             QStringLiteral("Skip"), 0, request.samples,
                             const_cast<MainWindow*>(this));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);

    QImage image;
    int    rendered = 0;
    if (!m_appearance->renderToImage(image, request, [&](int done, int total) {
            rendered = done;
            progress.setMaximum(total);
            progress.setValue(done);
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents |
                                            QEventLoop::AllEvents);
            return !progress.wasCanceled();
        }))
        return QImage();

    shot.width   = image.width();
    shot.height  = image.height();
    shot.samples = std::max(1, rendered);
    return image;
}

// The emitters the current configuration traces, or an empty list where the
// document has no primary source yet.
std::vector<SourceConfig> MainWindow::appearanceSources() const {
    if (!m_compiled.havePrimary || !m_compiled.setup) return {};
    return Simulation::sourcesFor(currentConfig(), m_compiled.setup->sourceOrigin,
                                  m_compiled.setup->sourceAxis);
}
