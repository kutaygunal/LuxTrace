// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

// Everything that crosses the process boundary: saving and loading a config,
// importing CAD and a material catalogue, and the CSV, image and HTML report
// exports.
//
// Grouped by direction rather than by tab, because that is what these share --
// each one is a file dialog, a core call that can fail, and a message that says
// which. The numbers themselves come from the analysis; nothing here computes
// one.
#include "MainWindowInternal.h"

void MainWindow::onSaveConfig() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save configuration"), QStringLiteral("optics-setup.json"),
        QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;
    QString err;
    // The document travels with the configuration: a composed scene is not
    // derivable from a SimConfig, which carries the compiled geometry with no
    // memory of which object produced which body.
    if (!configio::save(path, currentConfig(), &m_document, &err))
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
    scenedoc::SceneDocument doc;
    if (!configio::load(path, cfg, &doc, &err, &warnings)) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Could not open: %1").arg(err));
        return;
    }

    m_controls->setConfig(cfg);
    m_controls->setScene(cfg.scene);
    m_controls->setParams(cfg.effectiveParams());

    // A file written before there were documents carries none, and the scene it
    // does name is a tutorial: loading that is what the file meant.
    if (doc.empty()) m_document.loadTutorial(cfg.scene, cfg.effectiveParams());
    else             m_document = std::move(doc);

    m_selectedObject = 0;
    m_haveRequested  = false;
    m_view3d->setRays({});
    syncDocument(true);
    refreshSweepAxes();
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

    // Last, and captioned unlike any other figure in the document: every plot
    // above is a measurement and this one is a picture. It goes in because a
    // design review wants to see what the part looks like switched on, and it
    // goes in last with its own disclaimer because a render sitting among
    // irradiance maps will otherwise be read as one of them.
    appearance::ExportRequest shot;
    if (const QImage render = appearanceFigure(shot); !render.isNull()) {
        report::Figure f;
        f.title   = QStringLiteral("Appearance preview");
        f.caption = appearance::exportCaption(
            shot, appearance::distributionNote(
                      m_appearance->radianceMap(), m_last.unit));
        f.image   = render;
        figures.push_back(std::move(f));
    }
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
        cadimport::makeScene(result, dlg.materialName(), dlg.finish(),
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
                             .arg([&] {
                                 switch (dlg.finish()) {
                                 case cadimport::Finish::Mirror:
                                     return QStringLiteral("Every part is a mirror.");
                                 case cadimport::Finish::Opaque:
                                     return QStringLiteral(
                                         "Every part is an opaque diffuse moulding; "
                                         "give them colours in the surface panel.");
                                 default:
                                     return QStringLiteral("Every part is %1 with "
                                                           "Fresnel splitting.")
                                         .arg(dlg.materialName());
                                 }
                             }());

    // The file's parts become objects like any others: selectable in the tree,
    // hideable, re-specifiable, and something a lens can be dropped in front of.
    // That is the whole reason the document exists -- an import used to be a
    // third kind of thing the window had to remember it was showing.
    m_document.loadImport(*setup);
    m_selectedObject = 0;

    m_geometryTimer->stop();
    m_geometryGen   = 0;
    m_haveRequested = false;

    m_view3d->setRays({});   // traced through the previous geometry
    syncDocument(true);
    m_view3d->resetView();

    statusBar()->showMessage(
        QStringLiteral("%1 — %2 part(s) spanning %3 mm, now in the scene tree.")
            .arg(setup->label)
            .arg(result.shapes.size())
            .arg(extent, 0, 'f', 1), 12000);
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
