#include "render/AppearanceExport.h"

#include "render/RadianceMap.h"

#include <algorithm>
#include <cmath>

namespace appearance {

namespace {

int clampDimension(int v) {
    return std::clamp(v, kMinExportDimension, kMaxExportDimension);
}

} // namespace

ExportRequest normaliseExport(int width, int height, int samples, double viewAspect) {
    // An aspect that is not a positive finite number is no aspect at all. 16:9
    // rather than 1:1, because the fallback is reached when the view has never
    // been shown and the picture is still going to be looked at in a document.
    if (!std::isfinite(viewAspect) || viewAspect <= 0.0) viewAspect = 16.0 / 9.0;

    ExportRequest r;
    // Either dimension left at zero is derived from the other through the
    // aspect of the view, so the export frames what the window frames instead
    // of stretching it. Both zero falls back to the default size.
    if (width <= 0 && height <= 0) {
        width  = 1920;
        height = int(std::lround(1920.0 / viewAspect));
    } else if (width <= 0) {
        width = int(std::lround(double(clampDimension(height)) * viewAspect));
    } else if (height <= 0) {
        height = int(std::lround(double(clampDimension(width)) / viewAspect));
    }

    r.width   = clampDimension(width);
    r.height  = clampDimension(height);
    r.samples = std::clamp(samples, kMinExportSamples, kMaxExportSamples);
    return r;
}

QString notPhotometricNote() {
    return QStringLiteral(
        "Not a photometric result: RGB path tracing, no dispersion, no "
        "polarisation, no cd/m², and not reproducible frame for frame.");
}

QString distributionNote(const RadianceMap& map, FluxUnit unit) {
    if (!map.valid) return QString();
    return QStringLiteral("Emission driven by the traced distribution on %1, "
                          "%2 x %3 facets carrying %4 %5 — the pattern is the "
                          "run's, the brightness is not.")
        .arg(map.label.isEmpty() ? QStringLiteral("the receiver") : map.label)
        .arg(map.nx)
        .arg(map.ny)
        .arg(map.totalFlux, 0, 'g', 4)
        .arg(QLatin1String(fluxUnitName(unit)));
}

QString exportCaption(const ExportRequest& request, const QString& driven) {
    QString text = QStringLiteral("Appearance preview at %1 x %2, %3 samples.")
                       .arg(request.width)
                       .arg(request.height)
                       .arg(request.samples);
    if (!driven.isEmpty()) text += QLatin1Char(' ') + driven;
    return text + QLatin1Char(' ') + notPhotometricNote();
}

QString liveCaption(int samples, int budget, const QString& driven) {
    const QString state = samples >= budget
                              ? QStringLiteral("converged at %1 samples").arg(budget)
                              : QStringLiteral("%1 / %2 samples").arg(samples).arg(budget);
    QString text = QStringLiteral("Appearance preview — %1.").arg(state);
    if (!driven.isEmpty()) text += QLatin1Char(' ') + driven;
    return text + QLatin1Char(' ') + notPhotometricNote();
}

} // namespace appearance
