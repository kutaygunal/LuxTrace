#pragma once
#include <QImage>
#include <QString>
#include <vector>
#include "Analysis.h"
#include "Simulation.h"
#include "Studies.h"

// A design review consumes a document, not a folder of CSV files and PNGs
// assembled by hand. Everything it needs already exists -- every view renders to
// a pixmap at any size, the metrics already format to HTML -- so this is a
// writer over machinery that is already there.
//
// The seed and the version are stamped on it, which is what makes a LuxTrace
// report reproducible rather than merely readable: the numbers in it can be
// regenerated exactly.
namespace report {

// One rendered view, with the caption it goes under.
struct Figure {
    QString title;
    QString caption;
    QImage  image;
};

struct Content {
    SimConfig             config;
    SimulationResult      result;
    analysis::SpotMetrics metrics;
    std::vector<Figure>   figures;

    // Optional sections. Each is written only when it carries data, so a report
    // never has an empty heading in it.
    studies::FocusStudy                     focus;
    std::vector<studies::ConvergencePoint>  convergence;
    std::vector<studies::SweepPoint>        sweep;
    int                                     sweepSlot = -1;
    studies::Metric                         sweepMetric = studies::Metric::Efficiency;
    studies::OptimisationResult             optimisation;
    std::vector<int>                        optimisedSlots;
    studies::Objective                      objective;
    studies::ToleranceStudy                 tolerance;

    // The frequency-domain half of the measurements, and the wavefront behind
    // it. Written only where the run carries enough arrivals to mean anything.
    analysis::Mtf                           mtf;
    analysis::Wavefront                     wavefront;

    // A second run to compare against, when one is pinned.
    bool             hasComparison = false;
    SimulationResult comparison;
    QString          comparisonLabel;

    QString title;
    QString notes;
};

// A self-contained HTML document: images are embedded as data URIs, so it is one
// file that can be mailed, and there is no folder of assets to lose.
QString html(const Content& content);

bool write(const QString& path, const Content& content, QString* errorOut = nullptr);

} // namespace report
