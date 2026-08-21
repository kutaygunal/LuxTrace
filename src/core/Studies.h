#pragma once
#include <QString>
#include <vector>
#include "Analysis.h"
#include "Simulation.h"

// Parameter studies: repeated runs that answer a question a single trace
// cannot. Each one lives here rather than in the UI so it can be scripted from
// the command line and asserted by a test.
namespace studies {

// One point of a convergence sweep.
struct ConvergencePoint {
    int    rays        = 0;
    double efficiency  = 0.0;
    double stdErr      = 0.0;   // one Monte Carlo standard error
    double traceSeconds = 0.0;
};

// Efficiency against ray count, with error bars. Ray counts are spaced
// geometrically between the bounds, because the error falls as 1/sqrt(N) and
// linear spacing would spend every point on the flat end of the curve.
//
// Each run uses a different seed, so the points are independent samples rather
// than nested prefixes of one stream -- otherwise the curve would look far
// smoother than the underlying uncertainty actually is.
std::vector<ConvergencePoint> convergence(const SimConfig& base,
                                          int minRays, int maxRays, int points,
                                          const TraceControl& ctl = TraceControl{});

// Whether a converged answer is bracketed: true when the last point's error bar
// overlaps the previous one's, which is the practical form of "enough rays".
bool hasConverged(const std::vector<ConvergencePoint>& sweep, double sigmas = 2.0);

// ---- through focus ---------------------------------------------------------

struct FocusStudy {
    std::vector<analysis::FocusSample> samples;
    double bestZ        = 0.0;
    double bestRms      = 0.0;
    double receiverZ    = 0.0;   // where the receiver actually sits
    bool   valid        = false;
};

// Sweeps the detector plane analytically from the recorded arrivals: one trace,
// many planes. `spanMm` is the total travel centred on the receiver.
FocusStudy throughFocus(const SimulationResult& res, double spanMm, int steps = 81);

QString convergenceCsv(const std::vector<ConvergencePoint>& sweep);
QString focusCsv(const FocusStudy& study);

} // namespace studies
