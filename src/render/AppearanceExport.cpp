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
    // What this sentence has to do changed when the luminance camera
    // arrived, and it changed in a way that makes it more useful rather
    // than less.
    //
    // It used to say what this render is not, full stop: RGB, no cd/m2, not
    // reproducible. All of that is still true of this render and none of it
    // is going to stop being true -- it is an OpenGL path tracer with its own
    // two-layer BSDF, and it is fast because it is not the physics. What was
    // missing was the other half: a reader told only that this is not a
    // measurement has nowhere to go for one.
    //
    // There is somewhere to go now. backward::render traces the same
    // geometry, the same SurfaceOptics, the same coatings and the same
    // spectra the forward tracer does and reports luminance in cd/m2, and
    // the sentence names it -- including the button on this very tab, which
    // measures the view the reader is already looking at. A reader told only
    // that this is not a measurement, and sent to a command line for one, is
    // being told half of something useful.
    return QStringLiteral(
        "A picture, not a measurement: RGB path tracing, no dispersion, no "
        "polarisation, and not reproducible frame for frame. For luminance "
        "in cd/m² over the same scene and the same view, use Measure "
        "luminance... beside this render (or --camera, or the \"camera\" "
        "operation on the JSON API).");
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
