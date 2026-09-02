#pragma once
#include <QString>
#include <vector>

#include "SimulationResult.h"

// Comparing a fast backend against the reference one.
//
// A GPU trace is not the same computation as the CPU trace and cannot be. It
// carries a different random stream, accumulates in a different order, and --
// while its coverage is still growing -- may not model everything the reference
// does. So "is the GPU right" is not a question about equal numbers. Two correct
// Monte Carlo runs of the same system disagree, and the amount they disagree by
// is the thing worth measuring: about one standard error, no more.
//
// That is what this is. Given the reference result and the preview's, it reports
// how far apart they are in units of their own error bars, and how far apart the
// distributions are in units of the noise a run of that size carries anyway.
// A preview that passes is not "close enough"; it is indistinguishable from
// another draw of the reference, which is the only claim a sampled result can
// honestly make.
//
// It is deliberately independent of which backend produced either side: it
// takes two results. That keeps it testable without a GPU, and it makes it just
// as useful for "did this refactor change the answer" as for "is the GPU
// lying".
namespace backendcheck {

// One quantity, as the two runs saw it.
struct Comparison {
    QString name;
    double  reference = 0.0;
    double  preview   = 0.0;
    // Combined standard error of the difference, where the quantity has one.
    // Zero means the quantity carries no error bar and `sigma` is not meaningful.
    double  stdErr    = 0.0;
    // (preview - reference) / stdErr. The number to read: a sampled quantity
    // should land within about two of zero, and a systematic difference does
    // not shrink as the run grows.
    double  sigma     = 0.0;
    // For quantities with no error bar, the relative difference instead.
    double  relative  = 0.0;
    bool    hasSigma  = false;
    bool    ok        = true;
    // Reported but not gated. Beam width and peak intensity are read off a
    // noisy profile and move by more than a tolerance worth setting, but they
    // are the two numbers anybody actually looks at -- so they are shown and
    // left out of the verdict rather than being made to carry one.
    bool    gated     = true;
};

// How far apart two binned distributions are.
//
// Bin-by-bin equality is the wrong question -- two independent runs differ in
// every bin -- so this reports the total variation distance: half the sum of
// |a - b| over bins, each normalised to unit total. It is 0 for identical
// distributions and 1 for distributions that share no support, and it has the
// useful property of being a fraction of the light that would have to move to
// turn one map into the other.
//
// `noiseFloor` is what that distance would be for two independent draws of the
// *same* distribution at these ray counts. A distance near the floor means the
// two maps differ by sampling and nothing else; one well above it means they
// are different pictures.
struct MapComparison {
    QString name;
    bool    comparable = false;   // same shape, both non-empty
    // Why not, when it is not. The far field is binned on a beam-adaptive theta
    // partition, so two runs whose beams differ slightly are binned on
    // different rings -- and comparing those bin for bin would report the
    // partition, not the light.
    QString reason;
    double  distance   = 0.0;
    double  noiseFloor = 0.0;
    // distance / noiseFloor. Around 1 is agreement; large is a real difference.
    double  ratio      = 0.0;
    bool    ok         = true;
};

struct Report {
    bool ok = true;
    // Set when the two runs cannot be compared at all -- different receiver
    // geometry, no rays, a cancelled run. A verdict on incomparable results
    // would be worse than none.
    QString blocker;

    std::vector<Comparison>    scalars;
    std::vector<MapComparison> maps;

    // How many standard errors the worst scalar was out, and which.
    double  worstSigma = 0.0;
    QString worstName;

    // Human-readable, one line per row, for --gpucheck and the report.
    QString text() const;
};

// Tolerances, so a caller can be stricter than the default without editing it.
struct Tolerance {
    // How many combined standard errors a scalar may differ by.
    //
    // Four, not the conventional two or three, and the reason is what this gate
    // is for. The error bars are honest -- measured against the spread of 24
    // independent seeds they come out at 0.99 to 1.16 of the real standard
    // deviation -- so a threshold of three would reject two correct runs
    // roughly one time in three hundred. A gate that cries wolf on a correct
    // preview teaches people to ignore it, and there is nothing to gain by
    // being tight: a backend that has actually dropped a term of the physics
    // shows up at tens of sigma, not at three.
    double sigmas = 4.0;
    // How many times the sampling noise floor a map may differ by. Two
    // independent draws of the same scene measure between 0.6 and 1.0 of their
    // own floor, so three is the same kind of margin.
    double mapRatio = 3.0;
    // Quantities with no error bar (a count, a geometric extent) get a plain
    // relative tolerance.
    double relative = 1e-3;
};

// Compares `preview` against `reference`. Neither is modified, and the order
// matters only for the sign of the reported differences.
Report compare(const SimulationResult& reference, const SimulationResult& preview,
               const Tolerance& tol = Tolerance{});

// The total variation distance between two grids, and the noise floor for it.
// Exposed because it is the part worth testing directly: fed two draws of one
// distribution it must return roughly the floor, and fed two different
// distributions it must not.
double totalVariation(const std::vector<double>& a, const std::vector<double>& b);
double noiseFloorFor(const std::vector<double>& a, const std::vector<double>& b,
                     std::size_t raysA, std::size_t raysB);

// The same floor, computed from each run's own measured per-bin variance
// instead of from a count model.
//
// This is the one to use where it is available. A bin's noise is set by the
// spread of the weights deposited in it, not by how many rays reached it: with
// next-event estimation connecting nearly every diffuse bounce to the receiver,
// an integrating-sphere bin holds hundreds of arrivals and is still five times
// noisier than a count model predicts. `varA` and `varB` are the runs'
// `irradianceVar`, which is exactly the sum of squared deposits that estimates
// that spread.
double measuredNoiseFloor(const std::vector<double>& a, const std::vector<double>& varA,
                          const std::vector<double>& b, const std::vector<double>& varB);

} // namespace backendcheck
