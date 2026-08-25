#include "Report.h"
#include "Material.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTextStream>

#include <cmath>

namespace report {

namespace {

QString esc(const QString& s) { return s.toHtmlEscaped(); }

QString num(double v, int decimals) { return QString::number(v, 'f', decimals); }

QString pct(double v, double of) {
    if (of <= 0.0) return QStringLiteral("-");
    return QString::number(100.0 * v / of, 'f', 3) + QStringLiteral(" %");
}

// A PNG inlined as a data URI. One file, no folder of assets beside it.
QString embed(const QImage& img) {
    if (img.isNull()) return QString();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!img.save(&buffer, "PNG")) return QString();
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

void row(QTextStream& ts, const QString& name, const QString& value,
         const QString& note = QString()) {
    ts << "<tr><th>" << esc(name) << "</th><td>" << esc(value) << "</td><td class='note'>"
       << esc(note) << "</td></tr>\n";
}

const char* kStyle = R"(
body { font-family: "Segoe UI", "Helvetica Neue", sans-serif; font-size: 13px;
       color: #1c1c1c; background: #fff; margin: 32px auto; max-width: 980px;
       line-height: 1.55; }
h1 { font-size: 24px; margin: 0 0 4px 0; font-weight: 600; }
h2 { font-size: 16px; margin: 32px 0 10px 0; font-weight: 600;
     border-bottom: 1px solid #e2e2e2; padding-bottom: 6px; }
h3 { font-size: 13px; margin: 20px 0 6px 0; font-weight: 600; color: #444; }
.sub { color: #6b6b6b; margin: 0 0 8px 0; }
.warn { color: #8a4b00; background: #fff6e5; border-left: 3px solid #d08a2c;
        padding: 8px 10px; margin: 8px 0; }
table { border-collapse: collapse; margin: 8px 0 4px 0; }
th, td { text-align: left; padding: 3px 18px 3px 0; vertical-align: top; }
th { font-weight: 500; color: #4a4a4a; white-space: nowrap; }
td { font-variant-numeric: tabular-nums; }
.note { color: #8a8a8a; font-size: 12px; }
.grid { display: flex; flex-wrap: wrap; gap: 20px; }
.grid > div { flex: 1 1 420px; }
figure { margin: 12px 0 20px 0; }
figure img { max-width: 100%; border: 1px solid #e2e2e2; border-radius: 3px; display: block; }
figcaption { color: #6b6b6b; font-size: 12px; margin-top: 5px; }
.stamp { color: #8a8a8a; font-size: 12px; margin-top: 40px;
         border-top: 1px solid #e2e2e2; padding-top: 10px; }
.better { color: #1a7f37; }
.worse  { color: #b3261e; }
)";

void writeConfig(QTextStream& ts, const Content& c) {
    const SimConfig& cfg = c.config;
    ts << "<h2>Optic</h2>\n<div class='grid'><div>\n";
    ts << "<table>\n";
    row(ts, QStringLiteral("Scene"), cfg.sceneName());
    // Imported geometry has no parameters and no closed-form quantities of its
    // own: the registry's numbers describe a different solid, and printing them
    // under an imported part would be describing the wrong optic.
    static const std::vector<SceneParamInfo> kNoParams;
    const SceneParams p = cfg.effectiveParams();
    const auto& infos = cfg.tracesImport() ? kNoParams : GeometryProvider::paramInfo(cfg.scene);
    for (std::size_t i = 0; i < infos.size(); ++i)
        row(ts, infos[i].name,
            num(p.v[i], infos[i].decimals) + QLatin1Char(' ') + infos[i].unit,
            infos[i].tip);
    ts << "</table>\n</div><div>\n";

    // The numbers the parameters imply, which is what the design is actually
    // being judged on.
    const auto derived = cfg.tracesImport() ? std::vector<DerivedQuantity>{}
                                            : GeometryProvider::derived(cfg.scene, p);
    if (!derived.empty()) {
        ts << "<h3>Derived</h3>\n<table>\n";
        for (const auto& d : derived)
            row(ts, d.name, d.value + QLatin1Char(' ') + d.unit, d.tip);
        ts << "</table>\n";
    }
    ts << "</div></div>\n";

    ts << "<h2>Source and run</h2>\n<div class='grid'><div>\n<table>\n";
    static const char* kTypes[]  = {"Point (uniform in solid angle)", "Lambertian (cosine)",
                                    "Collimated"};
    static const char* kShapes[] = {"Point", "Disc", "Rectangle", "Sphere"};
    row(ts, QStringLiteral("Angular law"), QLatin1String(kTypes[int(cfg.source)]));
    row(ts, QStringLiteral("Emitter"), QLatin1String(kShapes[int(cfg.shape)]));
    if (cfg.source != SourceConfig::Type::Collimated)
        row(ts, QStringLiteral("Cone half-angle"), num(cfg.halfAngleDeg, 1) + QStringLiteral(" deg"));
    else
        row(ts, QStringLiteral("Beam radius"), num(cfg.beamRadius, 2) + QStringLiteral(" mm"));
    row(ts, QStringLiteral("Total flux"),
        QString::number(cfg.power, 'g', 6) + QLatin1Char(' ') +
            QLatin1String(fluxUnitName(cfg.fluxUnit)));
    row(ts, QStringLiteral("Spectrum"), SpectrumConfig::kindName(cfg.spectrum.kind));
    if (cfg.spectrum.kind == SpectrumConfig::Kind::Monochromatic)
        row(ts, QStringLiteral("Wavelength"), num(cfg.spectrum.wavelengthNm, 1) + QStringLiteral(" nm"));
    if (cfg.spectrum.kind == SpectrumConfig::Kind::Blackbody ||
        cfg.spectrum.kind == SpectrumConfig::Kind::LedPhosphor)
        row(ts, QStringLiteral("Colour temperature"), num(cfg.spectrum.cct, 0) + QStringLiteral(" K"));
    if (c.result.luminousEfficacy > 0.0)
        row(ts, QStringLiteral("Luminous efficacy"),
            num(c.result.luminousEfficacy, 1) + QStringLiteral(" lm/W"),
            QStringLiteral("683 x V(lambda) over the spectrum"));
    ts << "</table>\n</div><div>\n<table>\n";
    row(ts, QStringLiteral("Rays"), QString::number(c.result.raysEmitted));
    row(ts, QStringLiteral("Seed"), QString::number(cfg.seed),
        QStringLiteral("The run is fully determined by this and the ray count."));
    row(ts, QStringLiteral("Receiver grid"),
        QStringLiteral("%1 x %2").arg(c.result.nx).arg(c.result.ny));
    row(ts, QStringLiteral("Far field"),
        cfg.nTheta > 0 ? QStringLiteral("%1 x %2 bins").arg(cfg.nTheta).arg(cfg.nPhi)
                       : QStringLiteral("off"));
    row(ts, QStringLiteral("Trace time"), num(c.result.traceSeconds, 3) + QStringLiteral(" s"));

    QStringList physics;
    if (cfg.physics.fresnel)    physics << QStringLiteral("Fresnel");
    if (cfg.physics.absorption) physics << QStringLiteral("bulk absorption");
    if (cfg.physics.scattering) physics << QStringLiteral("scattering");
    if (cfg.physics.roughness)  physics << QStringLiteral("roughness");
    if (cfg.physics.dispersion) physics << QStringLiteral("dispersion");
    if (cfg.physics.coatings)   physics << QStringLiteral("coatings");
    if (cfg.physics.volumeScattering) physics << QStringLiteral("volume scattering");
    if (cfg.physics.polarised)  physics << QStringLiteral("polarisation");
    row(ts, QStringLiteral("Physics"),
        physics.isEmpty() ? QStringLiteral("none") : physics.join(QStringLiteral(", ")));
    if (!cfg.surfaceOverrides.empty())
        row(ts, QStringLiteral("Surface edits"),
            QString::number(cfg.surfaceOverrides.size()),
            QStringLiteral("Optical properties changed from the scene's own."));
    ts << "</table>\n</div></div>\n";
}

void writeBudget(QTextStream& ts, const Content& c) {
    const SimulationResult& r = c.result;
    const QString fu = QLatin1String(fluxUnitName(r.unit));
    ts << "<h2>Energy budget</h2>\n";
    ts << "<p class='sub'>Every emitted unit is accounted for. The estimator "
          "residual is the price of Russian roulette, emission aiming and "
          "next-event estimation: zero in expectation, and booked so the four "
          "physical buckets still close exactly.</p>\n";
    ts << "<table>\n";
    ts << "<tr><th></th><th>share</th><th>" << esc(fu) << "</th></tr>\n";
    auto line = [&](const QString& name, double v, const QString& note = QString()) {
        ts << "<tr><th>" << esc(name) << "</th><td>" << esc(pct(v, r.sourcePower))
           << "</td><td>" << esc(QString::number(v, 'g', 5)) << "</td><td class='note'>"
           << esc(note) << "</td></tr>\n";
    };
    line(QStringLiteral("On the receiver"), r.fluxDetector);
    line(QStringLiteral("Absorbed at surfaces"), r.fluxAbsorbed - r.fluxBulkAbsorbed);
    line(QStringLiteral("Absorbed in the bulk"), r.fluxBulkAbsorbed, QStringLiteral("Beer-Lambert"));
    line(QStringLiteral("Escaped the scene"), r.fluxEscaped);
    line(QStringLiteral("Refused by a receiver"), r.fluxRejected,
         QStringLiteral("outside an acceptance cone: a measurement condition, "
                        "not a loss"));
    line(QStringLiteral("Truncated"), r.fluxTruncated,
         QStringLiteral("see the breakdown below"));
    line(QStringLiteral("Estimator residual"), r.fluxRoulette, QStringLiteral("zero in expectation"));
    line(QStringLiteral("Accounted"), r.fluxAccounted());
    ts << "</table>\n";
    ts << "<p><b>Efficiency " << esc(num(100.0 * r.efficiency, 3)) << " &plusmn; "
       << esc(num(100.0 * r.efficiencyStdErr, 3)) << " %</b> "
       << "<span class='note'>one standard error, measured across independent "
          "scrambles of the sampling sequence</span></p>\n";

    // The estimator residual, one channel per estimator. Booked to a single
    // accumulator, its expectation is zero even when one contributor is
    // systematically wrong and another cancels it -- which is the one respect
    // in which a closed energy balance can close while being wrong. Named, each
    // channel has to answer for itself.
    ts << "<h3>Estimator residual by channel</h3>\n";
    ts << "<p class='sub'>Each of these has an expectation of exactly zero. They "
          "are not loss channels: they are the estimator's noise, made visible "
          "rather than hidden inside a single number.</p>\n";
    ts << "<table>\n<tr><th></th><th>share</th><th>" << esc(fu) << "</th></tr>\n";
    line(QStringLiteral("Russian roulette"), r.residual.roulette);
    line(QStringLiteral("Emission aiming"), r.residual.aiming);
    line(QStringLiteral("Next-event estimate"), r.residual.nextEvent,
         QStringLiteral("the analytic direct term"));
    line(QStringLiteral("Next-event suppression"), r.residual.neeSuppressed,
         QStringLiteral("the sampled hit it replaced"));
    line(QStringLiteral("BSDF sampling weights"), r.residual.bsdfWeight);
    ts << "</table>\n";

    if (r.fluxTruncated > 0.0) {
        const TruncationBreakdown& t = r.truncation;
        ts << "<h3>Truncated flux, by reason</h3>\n";
        ts << "<p class='sub'>Raising the depth limit only helps where the depth "
              "limit is what took the light.</p>\n";
        ts << "<table>\n<tr><th></th><th>share</th><th>" << esc(fu)
           << "</th><th>branches</th></tr>\n";
        auto tline = [&](const QString& name, double v, std::size_t n) {
            ts << "<tr><th>" << esc(name) << "</th><td>"
               << esc(pct(v, r.sourcePower)) << "</td><td>"
               << esc(QString::number(v, 'g', 5)) << "</td><td>" << n << "</td></tr>\n";
        };
        tline(QStringLiteral("Depth limit"), t.depthLimit, t.depthLimitCount);
        tline(QStringLiteral("Branch stack full"), t.stackOverflow, t.stackOverflowCount);
        tline(QStringLiteral("Degenerate direction"), t.degenerate, t.degenerateCount);
        tline(QStringLiteral("Refraction refused"), t.refractFailed, t.refractFailedCount);
        tline(QStringLiteral("Energy cutoff"), t.energyCutoff, t.energyCutoffCount);
        ts << "</table>\n";
        if (r.truncationSignificant())
            ts << "<p class='warn'><b>More than "
               << esc(num(100.0 * SimulationResult::kTruncationWarn, 2))
               << " % of the source was truncated.</b> The line above says which "
                  "mechanism took it.</p>\n";
    }

    if (r.anomalies.any()) {
        ts << "<h3>Medium-tracking anomalies</h3>\n";
        ts << "<p class='sub'>Recovered from, not errors. A count that scales with "
              "the ray budget on imported geometry means the mesh is not closed, "
              "and the index pairs it produced were guesses.</p>\n";
        ts << "<table>\n";
        row(ts, QStringLiteral("Unmatched exits"),
            QString::number(r.anomalies.unmatchedExit),
            QStringLiteral("left a solid the branch was never recorded entering"));
        row(ts, QStringLiteral("Medium-stack overflows"),
            QString::number(r.anomalies.stackOverflow),
            QStringLiteral("more nested media at once than the stack holds"));
        row(ts, QStringLiteral("Guessed incident indices"),
            QString::number(r.anomalies.guessedIndex),
            QStringLiteral("crossed a face outward while recorded as in vacuum"));
        ts << "</table>\n";
    }

    if (!r.unmatchedOverrides.empty()) {
        ts << "<h3>Surface edits that matched no surface</h3>\n";
        ts << "<p class='sub'>These were reported rather than applied. An edit "
              "keyed to a surface this geometry does not have must not land on "
              "whatever now occupies its slot.</p>\n<ul>\n";
        for (const QString& s : r.unmatchedOverrides)
            ts << "<li>" << esc(s) << "</li>\n";
        ts << "</ul>\n";
    }

    if (r.sources.size() > 1) {
        ts << "<h3>Per source</h3>\n";
        ts << "<table>\n<tr><th></th><th>emitted</th><th>delivered</th>"
              "<th>efficiency</th><th>rays</th></tr>\n";
        for (const SourceSummary& s : r.sources) {
            ts << "<tr><th>" << esc(s.label) << "</th><td>"
               << esc(QString::number(s.power, 'g', 5)) << " " << esc(fu) << "</td><td>"
               << esc(QString::number(s.flux, 'g', 5)) << " " << esc(fu) << "</td><td>"
               << esc(num(100.0 * s.efficiency(), 3)) << " %</td><td>" << s.rays
               << "</td><td class='note'>"
               << (s.rayFile ? "measured ray file" : "") << "</td></tr>\n";
        }
        ts << "</table>\n";
    }
}

void writeMetrics(QTextStream& ts, const Content& c) {
    const analysis::SpotMetrics& m = c.metrics;
    const SimulationResult& r = c.result;
    if (!m.valid && !r.intensity.valid()) return;
    ts << "<h2>Measurements</h2>\n<div class='grid'>\n";
    if (m.valid) {
        ts << "<div><h3>Spot on the receiver</h3>\n<table>\n";
        row(ts, QStringLiteral("Peak irradiance"),
            QString::number(m.peak * 1e6, 'g', 4) + QLatin1Char(' ') +
                QLatin1String(irradianceUnitName(r.unit)));
        row(ts, QStringLiteral("Mean over lit bins"),
            QString::number(m.mean * 1e6, 'g', 4) + QLatin1Char(' ') +
                QLatin1String(irradianceUnitName(r.unit)));
        row(ts, QStringLiteral("Uniformity min/peak"), num(m.uniformity, 4));
        row(ts, QStringLiteral("Uniformity mean/peak"), num(m.meanToPeak, 4));
        row(ts, QStringLiteral("Centroid"),
            QStringLiteral("(%1, %2) mm").arg(num(m.centroidX, 2), num(m.centroidY, 2)));
        row(ts, QStringLiteral("RMS radius"), num(m.rmsRadius, 3) + QStringLiteral(" mm"));
        row(ts, QStringLiteral("D50 / D86 radius"),
            num(m.d50Radius, 3) + QStringLiteral(" / ") + num(m.d86Radius, 3) + QStringLiteral(" mm"));
        row(ts, QStringLiteral("FWHM x / y"),
            num(m.fwhmX, 2) + QStringLiteral(" / ") + num(m.fwhmY, 2) + QStringLiteral(" mm"));
        row(ts, QStringLiteral("Lit bins"),
            QStringLiteral("%1 of %2").arg(m.litBins).arg(m.totalBins));
        if (m.cieY > 0.0 && r.spectral) {
            row(ts, QStringLiteral("CIE x, y"), num(m.cieX, 4) + QStringLiteral(", ") + num(m.cieY, 4));
            if (m.cct > 1000.0 && m.cct < 25000.0)
                row(ts, QStringLiteral("Colour temperature"), num(m.cct, 0) + QStringLiteral(" K"));
        }
        ts << "</table></div>\n";
    }
    if (c.mtf.valid || c.wavefront.valid) {
        ts << "<div><h3>Image quality</h3>\n<table>\n";
        if (c.mtf.valid && c.mtf.cutoff50 > 0.0)
            row(ts, QStringLiteral("MTF 50 %"),
                QString::number(c.mtf.cutoff50, 'g', 4) + QStringLiteral(" cycles/mm"),
                QStringLiteral("Where contrast falls to half. The spot size says how big "
                               "the blur is; this says what it does to detail."));
        if (c.mtf.valid && c.mtf.diffractionCutoff > 0.0)
            row(ts, QStringLiteral("Diffraction cutoff"),
                QString::number(c.mtf.diffractionCutoff, 'g', 4) + QStringLiteral(" cycles/mm"),
                QStringLiteral("No design passes contrast beyond this, however well corrected."));
        if (c.wavefront.valid) {
            row(ts, QStringLiteral("Wavefront RMS"),
                num(c.wavefront.rmsWaves, 3) + QStringLiteral(" waves"),
                QStringLiteral("From the optical path each arrival travelled."));
            row(ts, QStringLiteral("Peak to valley"),
                num(c.wavefront.ptvWaves, 3) + QStringLiteral(" waves"));
            if (c.wavefront.strehlMeaningful)
                row(ts, QStringLiteral("Strehl"), num(c.wavefront.strehl, 4),
                    c.wavefront.diffractionLimited()
                        ? QStringLiteral("At or above 0.8: diffraction limited.")
                        : QStringLiteral("Below 0.8: not diffraction limited."));
            else
                row(ts, QStringLiteral("Strehl"), QStringLiteral("not meaningful"),
                    QStringLiteral("Marechal's approximation holds to about a quarter wave; "
                                   "past that the wavefront error is the number to read."));
        }
        ts << "</table></div>\n";
    }
    if (r.intensity.valid()) {
        ts << "<div><h3>Far field</h3>\n<table>\n";
        row(ts, QStringLiteral("Peak intensity"),
            QString::number(r.intensity.peak, 'g', 4) + QLatin1Char(' ') +
                QLatin1String(intensityUnitName(r.unit)));
        row(ts, QStringLiteral("Beam FWHM"), num(r.intensity.fwhmDeg, 2) + QStringLiteral(" deg"));
        row(ts, QStringLiteral("Binning"),
            QStringLiteral("%1 x %2").arg(r.intensity.nTheta).arg(r.intensity.nPhi));
        ts << "</table></div>\n";
    }
    if (r.polarised) {
        ts << "<div><h3>Polarisation</h3>\n<table>\n";
        row(ts, QStringLiteral("Degree of polarisation"), num(r.degreeOfPolarisation, 4),
            QStringLiteral("0 is unpolarised, 1 is fully polarised."));
        row(ts, QStringLiteral("Linear part"), num(r.degreeLinear, 4));
        row(ts, QStringLiteral("Angle"), num(r.polarisationAngleDeg, 1) + QStringLiteral(" deg"),
            QStringLiteral("Of the linear part, from the s axis."));
        ts << "</table></div>\n";
    }
    ts << "</div>\n";
}

void writeComparison(QTextStream& ts, const Content& c) {
    if (!c.hasComparison) return;
    const SimulationResult& a = c.result;
    const SimulationResult& b = c.comparison;
    const analysis::SpotMetrics ma = c.metrics;
    const analysis::SpotMetrics mb = analysis::computeSpotMetrics(b);

    ts << "<h2>Against " << esc(c.comparisonLabel.isEmpty() ? QStringLiteral("the pinned run")
                                                            : c.comparisonLabel) << "</h2>\n";
    ts << "<p class='sub'>Whether a difference is real is a question about the "
          "error bars, so they are here too.</p>\n";
    ts << "<table>\n<tr><th></th><th>this run</th><th>pinned</th><th>change</th></tr>\n";

    auto cmp = [&](const QString& name, double x, double y, int decimals,
                   bool biggerIsBetter, const QString& unit) {
        const double d = x - y;
        const QString cls = (std::fabs(d) < 1e-12) ? QString()
                            : ((d > 0) == biggerIsBetter ? QStringLiteral(" class='better'")
                                                         : QStringLiteral(" class='worse'"));
        ts << "<tr><th>" << esc(name) << "</th><td>" << esc(num(x, decimals)) << esc(unit)
           << "</td><td>" << esc(num(y, decimals)) << esc(unit)
           << "</td><td" << cls << ">" << (d >= 0 ? "+" : "") << esc(num(d, decimals))
           << esc(unit) << "</td></tr>\n";
    };

    cmp(QStringLiteral("Efficiency"), 100.0 * a.efficiency, 100.0 * b.efficiency, 3, true,
        QStringLiteral(" %"));
    ts << "<tr><th>Uncertainty</th><td>&plusmn; " << esc(num(100.0 * a.efficiencyStdErr, 3))
       << " %</td><td>&plusmn; " << esc(num(100.0 * b.efficiencyStdErr, 3)) << " %</td><td>";
    const double gap  = std::fabs(a.efficiency - b.efficiency);
    const double band = a.efficiencyStdErr + b.efficiencyStdErr;
    ts << esc(band > 0.0 && gap > 2.0 * band
                  ? QStringLiteral("real: %1 sigma").arg(num(gap / band, 1))
                  : QStringLiteral("inside the noise"))
       << "</td></tr>\n";
    if (ma.valid && mb.valid) {
        cmp(QStringLiteral("RMS radius"), ma.rmsRadius, mb.rmsRadius, 3, false, QStringLiteral(" mm"));
        cmp(QStringLiteral("D86 radius"), ma.d86Radius, mb.d86Radius, 3, false, QStringLiteral(" mm"));
        cmp(QStringLiteral("Peak irradiance"), ma.peak * 1e6, mb.peak * 1e6, 1, true,
            QStringLiteral(" ") + QLatin1String(irradianceUnitName(a.unit)));
        cmp(QStringLiteral("Uniformity mean/peak"), ma.meanToPeak, mb.meanToPeak, 4, true, QString());
    }
    if (a.intensity.valid() && b.intensity.valid())
        cmp(QStringLiteral("Beam FWHM"), a.intensity.fwhmDeg, b.intensity.fwhmDeg, 2, false,
            QStringLiteral(" deg"));
    ts << "</table>\n";
}

void writeStudies(QTextStream& ts, const Content& c) {
    if (c.focus.valid) {
        ts << "<h2>Through focus</h2>\n<table>\n";
        row(ts, QStringLiteral("Receiver at"), num(c.focus.receiverZ, 1) + QStringLiteral(" mm"));
        row(ts, QStringLiteral("Best focus at"), num(c.focus.bestZ, 1) + QStringLiteral(" mm"),
            QStringLiteral("%1 mm from the receiver").arg(num(c.focus.bestZ - c.focus.receiverZ, 1)));
        row(ts, QStringLiteral("Smallest RMS"), num(c.focus.bestRms, 3) + QStringLiteral(" mm"));
        ts << "</table>\n<p class='sub'>Propagated analytically from the recorded "
              "arrivals, which is exact only where nothing stands between the planes.</p>\n";
    }

    if (!c.convergence.empty()) {
        ts << "<h2>Convergence</h2>\n<table>\n";
        ts << "<tr><th>rays</th><th>efficiency</th><th>&plusmn; 1 sigma</th><th>trace</th></tr>\n";
        for (const auto& p : c.convergence)
            ts << "<tr><td>" << p.rays << "</td><td>" << esc(num(100.0 * p.efficiency, 3))
               << " %</td><td>" << esc(num(100.0 * p.stdErr, 4)) << " %</td><td>"
               << esc(num(p.traceSeconds, 3)) << " s</td></tr>\n";
        ts << "</table>\n";
        ts << "<p class='sub'>"
           << esc(studies::hasConverged(c.convergence)
                      ? QStringLiteral("Stable within two sigma: enough rays.")
                      : QStringLiteral("Still moving -- trace more rays before quoting this."))
           << "</p>\n";
    }

    if (!c.sweep.empty() && c.sweepSlot >= 0) {
        const auto& infos = GeometryProvider::paramInfo(c.config.scene);
        const QString pname = c.sweepSlot < int(infos.size())
                                  ? infos[std::size_t(c.sweepSlot)].name : QStringLiteral("parameter");
        ts << "<h2>Parameter sweep</h2>\n";
        ts << "<p class='sub'>" << esc(studies::metricName(c.sweepMetric)) << " against "
           << esc(pname) << ".</p>\n<table>\n";
        ts << "<tr><th>" << esc(pname) << "</th><th>"
           << esc(studies::metricName(c.sweepMetric)) << "</th><th>&plusmn; 1 sigma</th></tr>\n";
        for (const auto& p : c.sweep)
            ts << "<tr><td>" << esc(num(p.parameter, 2)) << "</td><td>"
               << esc(QString::number(p.value, 'g', 6)) << "</td><td>"
               << esc(QString::number(p.stdErr, 'g', 3)) << "</td></tr>\n";
        ts << "</table>\n";
    }

    if (c.tolerance.valid) {
        const auto& t = c.tolerance;
        ts << "<h2>Tolerance analysis</h2>\n";
        ts << "<p class='sub'>Not how good the nominal design is, but what fraction of "
              "production will pass. The seed is on this report, so the same ensemble "
              "can be traced again and checked.</p>\n";
        ts << "<table>\n";
        row(ts, QStringLiteral("Judged by"), studies::metricName(t.metric));
        row(ts, QStringLiteral("Specification"),
            QStringLiteral("%1 %2").arg(t.passIsAbove ? QStringLiteral("at or above")
                                                      : QStringLiteral("at or below"))
                                   .arg(t.criterion, 0, 'g', 5));
        row(ts, QStringLiteral("Parts traced"), QString::number(t.samples),
            QStringLiteral("in %1 s").arg(num(t.seconds, 1)));
        row(ts, QStringLiteral("Yield"), num(100.0 * t.yield, 1) + QStringLiteral(" %"));
        row(ts, QStringLiteral("Nominal"), QString::number(t.nominal, 'g', 5));
        row(ts, QStringLiteral("Mean"),
            QStringLiteral("%1 +/- %2").arg(t.mean, 0, 'g', 5).arg(t.stdDev, 0, 'g', 3));
        row(ts, QStringLiteral("90 % of parts beat"), QString::number(t.p90, 'g', 5));
        row(ts, QStringLiteral("Worst part"), QString::number(t.worst, 'g', 5));
        ts << "</table>\n";

        if (!t.sensitivity.empty()) {
            ts << "<h3>What dominates it</h3>\n";
            ts << "<p class='sub'>A yield figure says a design is fragile; this says which "
                  "dimension to tighten.</p>\n<table>\n";
            ts << "<tr><th>dimension</th><th>share of the spread</th><th>tolerance</th></tr>\n";
            for (const auto& sens : t.sensitivity)
                ts << "<tr><td>" << esc(sens.name) << "</td><td>"
                   << esc(num(100.0 * sens.share, 1)) << " %</td><td>&plusmn; "
                   << esc(QString::number(sens.amount, 'g', 4)) << "</td></tr>\n";
            ts << "</table>\n";
        }
    }

    if (c.optimisation.valid) {
        const auto& infos = GeometryProvider::paramInfo(c.config.scene);
        ts << "<h2>Optimisation</h2>\n<table>\n";
        row(ts, QStringLiteral("Method"), c.optimisation.method);
        row(ts, QStringLiteral("Objective"),
            QStringLiteral("%1 %2")
                .arg(c.objective.goal == studies::Objective::Goal::Maximise
                         ? QStringLiteral("maximise")
                         : (c.objective.goal == studies::Objective::Goal::Minimise
                                ? QStringLiteral("minimise")
                                : QStringLiteral("hit a target for")),
                     studies::metricName(c.objective.metric)));
        row(ts, QStringLiteral("Evaluations"), QString::number(c.optimisation.evaluations),
            QStringLiteral("in %1 s").arg(num(c.optimisation.seconds, 1)));
        row(ts, QStringLiteral("Start"), QString::number(c.optimisation.startValue, 'g', 6) +
                                             QLatin1Char(' ') +
                                             studies::metricUnit(c.objective.metric, c.result.unit));
        row(ts, QStringLiteral("Best"), QString::number(c.optimisation.bestValue, 'g', 6) +
                                            QLatin1Char(' ') +
                                            studies::metricUnit(c.objective.metric, c.result.unit));
        for (int slotIndex : c.optimisedSlots)
            if (slotIndex >= 0 && slotIndex < int(infos.size()))
                row(ts, infos[std::size_t(slotIndex)].name,
                    num(c.optimisation.best.v[slotIndex], infos[std::size_t(slotIndex)].decimals) +
                        QLatin1Char(' ') + infos[std::size_t(slotIndex)].unit);
        ts << "</table>\n";
    }
}

} // namespace

QString html(const Content& c) {
    QString out;
    QTextStream ts(&out);
    const QString title = c.title.isEmpty()
                              ? QStringLiteral("%1 - LuxTrace").arg(c.config.sceneName())
                              : c.title;

    ts << "<!doctype html>\n<html><head><meta charset='utf-8'>\n";
    ts << "<title>" << esc(title) << "</title>\n<style>" << kStyle << "</style>\n</head><body>\n";
    ts << "<h1>" << esc(title) << "</h1>\n";
    ts << "<p class='sub'>" << esc(c.config.sceneDescription()) << "</p>\n";
    if (!c.notes.isEmpty()) ts << "<p>" << esc(c.notes) << "</p>\n";

    writeConfig(ts, c);
    writeBudget(ts, c);
    writeMetrics(ts, c);
    writeComparison(ts, c);

    if (!c.figures.empty()) {
        ts << "<h2>Views</h2>\n";
        for (const auto& f : c.figures) {
            const QString src = embed(f.image);
            if (src.isEmpty()) continue;
            ts << "<figure><img alt='" << esc(f.title) << "' src='" << src << "'>\n";
            ts << "<figcaption><b>" << esc(f.title) << "</b>";
            if (!f.caption.isEmpty()) ts << " &mdash; " << esc(f.caption);
            ts << "</figcaption></figure>\n";
        }
    }

    writeStudies(ts, c);

    ts << "<p class='stamp'>LuxTrace "
       << esc(QCoreApplication::applicationVersion().isEmpty()
                  ? QStringLiteral("1.0.0")
                  : QCoreApplication::applicationVersion())
       << " &middot; seed " << c.config.seed << " &middot; " << c.result.raysEmitted
       << " rays &middot; generated "
       << esc(QDateTime::currentDateTime().toString(Qt::ISODate)) << "<br>"
       << "The seed and the ray count fix this run completely: tracing it again on any "
          "machine, at any thread count, reproduces every number above bit for bit.</p>\n";
    ts << "</body></html>\n";
    return out;
}

QString validationHtml(const std::vector<studies::ValidationCase>& cases, int rays) {
    QString out;
    QTextStream ts(&out);

    int failed = 0;
    for (const auto& c : cases) if (!c.passed) ++failed;

    ts << "<!doctype html>\n<html lang='en'><head><meta charset='utf-8'>\n";
    ts << "<title>LuxTrace validation</title>\n<style>" << kStyle << "</style>\n";
    ts << "</head><body>\n";
    ts << "<h1>LuxTrace validation</h1>\n";
    ts << "<p class='sub'>Every case below is checked against optics derived "
          "outside the tracer. Pinning a formula against the same formula "
          "recomputed in a test proves only that the implementation matches "
          "itself; what this table shows is agreement with a closed form nobody "
          "had to trust the tracer to write.</p>\n";

    ts << "<table>\n";
    row(ts, QStringLiteral("Generated"),
        QDateTime::currentDateTime().toString(Qt::ISODate));
    row(ts, QStringLiteral("Build"), QCoreApplication::applicationVersion());
    row(ts, QStringLiteral("Monte Carlo budget"), QString::number(rays) + " rays",
        QStringLiteral("the purely geometric and analytic cases ignore it"));
    row(ts, QStringLiteral("Result"),
        failed == 0 ? QStringLiteral("%1 of %1 passed").arg(cases.size())
                    : QStringLiteral("%1 of %2 FAILED").arg(failed).arg(cases.size()));
    ts << "</table>\n";

    if (failed > 0)
        ts << "<p class='warn'><b>" << failed << " case(s) did not agree with the "
              "closed form.</b> The residual column says by how much, and the "
              "reference column says against what.</p>\n";

    ts << "<h2>Cases</h2>\n";
    ts << "<table>\n<tr><th>case</th><th>expected</th><th>measured</th>"
          "<th>residual</th><th>relative</th><th>tolerance</th><th></th></tr>\n";
    for (const auto& c : cases) {
        ts << "<tr><th>" << esc(c.name) << "</th>"
           << "<td>" << esc(QString::number(c.expected, 'g', 6)) << "</td>"
           << "<td>" << esc(QString::number(c.measured, 'g', 6)) << "</td>"
           << "<td>" << esc(QString::number(c.residual(), 'g', 3)) << "</td>"
           << "<td>" << esc(num(100.0 * c.relative(), 2)) << " %</td>"
           << "<td>" << esc(QString::number(c.tolerance, 'g', 3)) << " "
           << esc(c.unit) << "</td>"
           << "<td>" << (c.passed ? "pass" : "<b>FAIL</b>") << "</td></tr>\n";
        ts << "<tr><td colspan='7' class='note'>" << esc(c.reference) << "</td></tr>\n";
    }
    ts << "</table>\n";

    ts << "<h2>What this does and does not prove</h2>\n";
    ts << "<p class='sub'>These cases check the physics against its own closed "
          "forms. They do not check that the geometry in front of the tracer is "
          "the geometry the customer drew, and they say nothing about a scene "
          "whose mesh is not closed -- the run report's medium-anomaly counters "
          "are what answer that. Nor do they replace the reference corpus, which "
          "asserts that every scene still produces the number it produced "
          "before, to the last few digits.</p>\n";
    ts << "</body></html>\n";
    return out;
}

bool writeValidation(const QString& path,
                     const std::vector<studies::ValidationCase>& cases,
                     int rays, QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    const QByteArray bytes = validationHtml(cases, rays).toUtf8();
    if (f.write(bytes) != bytes.size()) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return true;
}

bool write(const QString& path, const Content& content, QString* errorOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    const QByteArray bytes = html(content).toUtf8();
    if (f.write(bytes) != bytes.size()) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return true;
}

} // namespace report
