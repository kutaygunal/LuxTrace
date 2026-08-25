#pragma once
#include <QString>
#include <cmath>
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

// ---- what a design is judged by --------------------------------------------

// One scalar read off a finished run. A sweep needs a number to plot and an
// optimiser needs a number to minimise; both want the same list, because the
// thing you sweep to look at is the thing you then ask to be made best.
enum class Metric {
    Efficiency,           // fraction of the source reaching the receiver
    RmsRadius,            // mm, flux-weighted about the centroid
    D86Radius,            // mm, encircled energy
    PeakIrradiance,       // W/m^2 or lux
    UniformityMinPeak,    // min / peak over the lit bins
    UniformityMeanPeak,   // mean / peak
    BeamFwhmDeg,          // far-field full width at half maximum
    PeakIntensity,        // W/sr or candela
    SpotFwhmX,            // mm, through the centroid
    CentroidOffset,       // mm, distance of the spot centre from the receiver's
    Count
};

QString metricName(Metric m);
QString metricUnit(Metric m, FluxUnit unit);
QString metricTip(Metric m);
double  metricValue(const SimulationResult& res, Metric m);

// One standard error on that metric, where the run reports one.
//
// Efficiency has an honest error bar: replica-based, measured across
// independent Owen scrambles, so it says what the estimator actually achieved.
// The spot metrics do not -- an RMS radius computed from a reservoir of
// arrivals has a sampling error nobody here has derived, and inventing one
// would be worse than drawing none. Zero means "this metric does not report an
// uncertainty", and a plot draws no bar rather than a bar of zero.
double  metricStdErr(const SimulationResult& res, Metric m);
// Whether a bigger number is a better design, for the axis labels and for the
// optimiser's default direction.
bool    metricBiggerIsBetter(Metric m);

// ---- parameter sweeps ------------------------------------------------------

// One point of a sweep: the parameter value, the metric there, and how much of
// the metric's movement is the Monte Carlo rather than the design.
struct SweepPoint {
    double parameter = 0.0;
    double value     = 0.0;
    double stdErr    = 0.0;
    double efficiency = 0.0;
    double seconds    = 0.0;
};

// A metric against one of the scene's own dimensions.
//
// Every scene already declares its parameters with ranges, steps and units, and
// the geometry cache was built for exactly the case of walking through parameter
// values. This is the study that turns those two facts into a design tool
// instead of a viewer.
//
// `repeats` independent seeds per point: without them a sweep is a curve whose
// wiggles cannot be told from its noise, and the error bar is the whole point of
// plotting one.
std::vector<SweepPoint> parameterSweep(const SimConfig& base, int slotIndex,
                                       double from, double to, int steps,
                                       Metric metric, int repeats = 2,
                                       const TraceControl& ctl = TraceControl{});

// The same over a pair of dimensions, as a contour map. `values` is
// stepsB rows of stepsA columns.
struct Sweep2D {
    int slotA = 0, slotB = 0;
    std::vector<double> a, b;        // parameter values along each axis
    std::vector<double> values;      // stepsA * stepsB
    double lo = 0.0, hi = 0.0;
    bool valid() const { return !values.empty() && a.size() * b.size() == values.size(); }
    double at(int ia, int ib) const { return values[std::size_t(ib) * a.size() + std::size_t(ia)]; }
};

Sweep2D parameterSweep2D(const SimConfig& base, int slotA, int slotB,
                         int stepsA, int stepsB, Metric metric,
                         const TraceControl& ctl = TraceControl{});

QString sweepCsv(const std::vector<SweepPoint>& sweep, const SimConfig& cfg,
                 int slotIndex, Metric metric);
QString sweep2dCsv(const Sweep2D& sweep, const SimConfig& cfg, Metric metric);

// ---- optimisation ----------------------------------------------------------

// What the search is trying to do to the metric.
struct Objective {
    enum class Goal { Maximise, Minimise, Target };
    Metric metric = Metric::Efficiency;
    Goal   goal   = Goal::Maximise;
    double target = 0.0;      // Goal::Target only

    // Always-minimised merit, so the search never has to know the direction.
    double merit(double value) const {
        switch (goal) {
        case Goal::Maximise: return -value;
        case Goal::Minimise: return value;
        case Goal::Target:   return std::fabs(value - target);
        }
        return value;
    }
};

struct OptimisationStep {
    SceneParams params;
    double      value = 0.0;   // the metric at those parameters
    // One standard error on `value`, from the run that produced it. An
    // optimisation history without it is a curve whose wiggles cannot be told
    // from its noise -- and an optimiser that is chasing noise looks exactly
    // like one that is making progress.
    double      stdErr = 0.0;
    double      merit = 0.0;   // best merit seen up to and including this step
    int         evaluation = 0;
};

struct OptimisationResult {
    bool        valid = false;
    SceneParams best;
    double      bestValue = 0.0;
    double      startValue = 0.0;
    int         evaluations = 0;
    double      seconds = 0.0;
    bool        cancelled = false;
    QString     method;
    // The metric at every evaluation, in order, for a convergence plot.
    std::vector<OptimisationStep> history;
};

// Which search to run.
//   NelderMead  derivative-free simplex. Short, robust to a noisy objective,
//               and the right first answer for two to four parameters.
//   Cmaes       an evolution strategy that learns the shape of the landscape.
//               Slower to start and far better on a valley that is not aligned
//               with the parameter axes.
enum class Optimiser { NelderMead, Cmaes };

// Searches `paramSlots` of the scene's parameters for the best design.
//
// The tracer is deterministic, so a fixed seed makes the objective a genuine
// function of the parameters rather than something that jitters between
// evaluations -- which is what lets a derivative-free search converge at all,
// and is a real advantage over a tracer whose answer moves when you ask twice.
OptimisationResult optimise(const SimConfig& base, const std::vector<int>& paramSlots,
                            const Objective& objective, Optimiser method = Optimiser::NelderMead,
                            int maxEvaluations = 120,
                            const TraceControl& ctl = TraceControl{});

QString optimisationCsv(const OptimisationResult& r, const SimConfig& cfg,
                        const std::vector<int>& paramSlots, const Objective& objective);

// ---- tolerancing -----------------------------------------------------------

// What a manufacturer actually asks is not "how good is the nominal design" but
// "what fraction of production will pass". Nothing here answered it.
//
// A tolerance is a distribution on a parameter, not a second value of it.
struct Tolerance {
    // All three mean the same thing by `amount`: the limit on the drawing. A
    // shape that quietly reinterpreted it would make switching between them
    // change the study rather than the assumption behind it.
    enum class Shape {
        Uniform,      // anywhere within the limit, which is what a machining bound means
        Gaussian,     // a process under control, with the limit at three sigma
        Bimodal       // at one limit or the other: the worst a supplier can ship
                      // while still passing inspection
    };

    int    slotIndex = -1;    // which of the scene's parameters
    double amount    = 0.0;   // the +/- limit, in the parameter's own unit
    Shape  shape     = Shape::Gaussian;
    bool   active() const { return slotIndex >= 0 && amount > 0.0; }
};

// The result of perturbing a design many times over.
struct ToleranceStudy {
    bool   valid = false;
    bool   cancelled = false;
    int    samples = 0;
    double seconds = 0.0;

    Metric metric = Metric::Efficiency;
    double nominal = 0.0;         // the metric at the design values
    double mean = 0.0, stdDev = 0.0;
    double best = 0.0, worst = 0.0;
    // The value that 50, 90 and 99 per cent of production beats.
    double median = 0.0, p90 = 0.0, p99 = 0.0;

    // Pass criterion and the fraction that met it. This is the number the study
    // exists to produce.
    double criterion = 0.0;
    bool   passIsAbove = true;
    double yield = 0.0;

    // Histogram of the metric across the ensemble.
    std::vector<double> binCentre;
    std::vector<int>    binCount;

    // Which tolerance dominates, one entry per active tolerance, sorted worst
    // first. A yield figure without this says a design is fragile; with it, it
    // says which dimension to tighten.
    struct Sensitivity {
        int     slotIndex = -1;
        QString name;
        double  amount = 0.0;
        // Change in the metric per unit of the parameter, from the ensemble
        // itself rather than from a separate one-at-a-time sweep.
        double  gradient = 0.0;
        // Share of the ensemble's variance this tolerance accounts for.
        double  share = 0.0;
    };
    std::vector<Sensitivity> sensitivity;

    // Every sample, for a scatter plot or an export.
    std::vector<double> values;
};

// Perturbs the design `samples` times and reports the spread.
//
// The determinism story is what makes the ensemble reproducible, which
// tolerancing tools rarely are: the same seed gives the same production run, so
// a yield figure can be checked rather than merely quoted.
ToleranceStudy tolerance(const SimConfig& base, const std::vector<Tolerance>& tolerances,
                         Metric metric, double criterion, bool passIsAbove,
                         int samples = 200, const TraceControl& ctl = TraceControl{});

QString toleranceCsv(const ToleranceStudy& study, const SimConfig& cfg);

// ---- validation ------------------------------------------------------------

// One check of the tracer against optics derived outside it.
//
// Pinning a formula against the same formula recomputed in a test proves the
// implementation matches itself. What convinces an engineer evaluating the tool
// is agreement with a closed form nobody had to trust the tracer to write: the
// lensmaker's equation, the integrating-sphere multiplier, the inverse-square
// cosine law, the concentration limit, minimum deviation, Lambert's cosine law.
struct ValidationCase {
    QString name;
    QString reference;    // where the expected number comes from
    QString unit;
    double  expected  = 0.0;
    double  measured  = 0.0;
    double  tolerance = 0.0;   // absolute, in `unit`
    bool    passed    = false;

    double residual() const { return measured - expected; }
    // Residual as a fraction of the expected value, for the printed table.
    double relative() const {
        return std::fabs(expected) > 1e-12 ? residual() / expected : 0.0;
    }
};

// Runs every closed-form check. `rays` scales the Monte Carlo cases; the purely
// geometric ones ignore it.
std::vector<ValidationCase> validate(int rays = 200000);

QString validationTable(const std::vector<ValidationCase>& cases);

// ---- regression corpus -----------------------------------------------------

// One scene's reference result, at a fixed ray budget and a fixed seed.
//
// The tests assert bands -- "reflector efficiency between 0.78 and 0.90" --
// which catches a scene that stops working and nothing else. Because a run is
// bit-reproducible, a reference value can be asserted to the last few digits
// instead, and any behavioural drift at all becomes visible. That is the payoff
// for the determinism work, and almost no commercial tracer can offer it.
struct ReferencePoint {
    QString       scene;
    int           rays = 0;
    std::uint64_t seed = 0;

    double efficiency    = 0.0;
    double fluxDetector  = 0.0;
    double fluxAbsorbed  = 0.0;
    double fluxEscaped   = 0.0;
    double fluxTruncated = 0.0;
    double fluxRejected  = 0.0;
    double residual      = 0.0;
};

// One quantity that moved.
struct RegressionDrift {
    QString scene;
    QString quantity;
    double  reference = 0.0;
    double  measured  = 0.0;
    double  relative  = 0.0;    // signed, relative to the reference
};

// Traces every scene in the registry at its default parameters and returns the
// reference points. Deterministic: the same build gives the same numbers, to
// the bit, at any thread count.
std::vector<ReferencePoint> referenceCorpus(int rays = 20000,
                                            std::uint64_t seed = 12345u,
                                            const TraceControl& ctl = TraceControl{});

// The corpus as a text file, one scene per line. Deliberately not JSON: a
// reference file is read by a human deciding whether a change was intended, and
// a diff of it should be legible.
bool writeCorpus(const QString& path, const std::vector<ReferencePoint>& pts,
                 QString* errorOut = nullptr);
bool readCorpus(const QString& path, std::vector<ReferencePoint>& out,
                QString* errorOut = nullptr);

// Every quantity whose relative movement exceeds `tolerance`, plus an entry for
// any scene present in one list and missing from the other. An empty result is
// the pass.
//
// The tolerance is relative and deliberately tight. It is not zero because a
// different compiler, a different instruction set or a different OCCT build can
// move the last bits of a tessellation; it is small enough that any change in
// what the tracer does shows up.
constexpr double kRegressionTolerance = 1e-9;

std::vector<RegressionDrift> compareCorpus(const std::vector<ReferencePoint>& reference,
                                           const std::vector<ReferencePoint>& measured,
                                           double tolerance = kRegressionTolerance);

QString regressionTable(const std::vector<RegressionDrift>& drift,
                        std::size_t scenesChecked);


} // namespace studies
