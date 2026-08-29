// T3-001 -- beam-adaptive theta binning.
//
// The flat `180.0 / nTheta` partition was replaced by an equal-flux,
// beam-adaptive bin. A fine master theta-ring flux histogram is reduced to the
// requested nTheta via IntensityGrid::buildBeamAdaptiveEdges, so a narrow
// (couple-of-degree) beam spans several output bins instead of collapsing
// under one, while dark directions share wide ones. Every bin accessor --
// thetaCenterDeg, thetaLow/HighDeg, thetaWidthDeg, cellSolidAngleRad,
// thetaBinIndex -- reads the single thetaEdges array, so the same grid flows
// to every exporter and the polar readout with no further change.
//
// These tests pin the contract: strictly increasing edges on 0..pi, equal
// flux per bin (CDF inversion), beam concentration, a uniform fallback on a
// silent run, and cross-accessor consistency on an adaptive grid.

#include <cmath>
#include <vector>

namespace {

/// A master flux histogram with a single concentrated band of `widthMaster`
/// master bins sitting on [lo, lo+band), strength `s`, silent elsewhere.
std::vector<double> bandFlux(int masterN, int lo, int widthMaster, double s) {
    std::vector<double> flux(std::size_t(masterN), 0.0);
    for (int b = lo; b < lo + widthMaster && b < masterN; ++b)
        flux[std::size_t(b)] = s;
    return flux;
}

std::vector<double> uniformFlux(int masterN, double s) {
    return std::vector<double>(std::size_t(masterN), s);
}

/// Integral of (ringFlux + baseline) over theta in [0, theta], treating the
/// master histogram as piecewise constant on master bins of width W.
double cumFluxAt(const std::vector<double>& ringFlux, int masterN, double W,
                 double baseline, double theta) {
    double acc = 0.0;
    for (int m = 0; m < masterN; ++m) {
        const double lo = W * m;
        const double hi = W * (m + 1);
        if (theta <= lo) break;
        const double span = std::min(theta, hi) - lo;
        acc += (ringFlux[std::size_t(m)] + baseline) * span;
    }
    return acc;
}

} // namespace

TEST(thetabin, edges_are_strictly_increasing_from_zero_to_pi) {
    const int masterN = 720;
    const int nOut = 12;
    std::vector<double> edges;
    IntensityGrid::buildBeamAdaptiveEdges(bandFlux(masterN, masterN / 2, 20, 100.0),
                                          masterN, nOut, 0.0, edges);
    CHECK(int(edges.size()) == nOut + 1);
    CHECK_NEAR(edges.front(), 0.0, 1e-12);
    CHECK_NEAR(edges.back(), kPi, 1e-12);
    for (int i = 1; i <= nOut; ++i)
        CHECK(edges[std::size_t(i)] > edges[std::size_t(i - 1)]);
}

TEST(thetabin, each_bin_takes_an_equal_share_of_the_flux) {
    // CDF inversion puts every interior output edge at the k/nOut quantile of
    // total (floor-inclusive) flux, so each bin integrates to the same share
    // even in the presence of a per-master-bin floor. This is the conservation
    // guarantee behind the adaptive grid.
    const int masterN  = 720;
    const int nOut     = 20;
    const double W     = kPi / double(masterN);
    const double baseline = 1e-6;   // tiny non-zero floor
    const auto ring = bandFlux(masterN, masterN / 2, 30, 1000.0);

    std::vector<double> edges;
    IntensityGrid::buildBeamAdaptiveEdges(ring, masterN, nOut, baseline, edges);
    const double total = cumFluxAt(ring, masterN, W, baseline, kPi);
    CHECK(total > 0.0);
    for (int k = 1; k < nOut; ++k) {
        const double got  = cumFluxAt(ring, masterN, W, baseline, edges[std::size_t(k)]);
        const double want = total * double(k) / double(nOut);
        CHECK_NEAR(got, want, 1e-9 * total);
    }
}

TEST(thetabin, a_narrow_beam_spans_several_bins_and_dark_directions_share_wide_ones) {
    // A ~5-degree beam concentrated near the axis must no longer sit under one
    // flat bin: with an adaptive grid it is resolved across multiple narrow
    // bins, while empty directions are pooled into wide bins -- the reason the
    // narrow beam became visible at all.
    const int masterN = 900;               // 0.2 deg per master bin
    const int nOut    = 12;
    const double uniformWidth = 180.0 / double(nOut);
    const int lo       = masterN / 2;      // beam centre near 90 deg
    const int width    = 25;               // ~5 deg band
    const double beam   = 1000.0;

    std::vector<double> edges;
    IntensityGrid::buildBeamAdaptiveEdges(bandFlux(masterN, lo, width, beam),
                                          masterN, nOut, 0.0, edges);

    int narrow = 0, wide = 0;
    for (int i = 0; i < nOut; ++i) {
        const double w = (edges[std::size_t(i + 1)] - edges[std::size_t(i)]) * 180.0 / kPi;
        if (w < uniformWidth * 0.5) ++narrow;
        if (w > uniformWidth * 1.5) ++wide;
    }
    CHECK(narrow >= 2);     // the beam is resolved, not collapsed
    CHECK(wide >= 1);       // silent directions are pooled
}

TEST(thetabin, uniform_flux_yields_a_near_uniform_partition) {
    // With a perfectly isotropic far field the equal-flux and uniform
    // partitions coincide, so the adaptive builder must fall back to the flat
    // grid -- this is what keeps an isotropic luminaire readable.
    const int masterN = 720;
    const int nOut    = 12;
    std::vector<double> edges;
    IntensityGrid::buildBeamAdaptiveEdges(uniformFlux(masterN, 1.0),
                                          masterN, nOut, 0.0, edges);
    const double W = kPi / double(masterN);
    for (int i = 1; i <= nOut; ++i)
        CHECK_NEAR(edges[std::size_t(i)], W * double(masterN) * double(i) / double(nOut),
                   1e-6);
}

TEST(thetabin, a_silent_run_falls_back_to_uniform) {
    // An all-silent far field (no flux anywhere) must not produce a degenerate
    // partition: it falls back to the uniform grid so downstream exporters
    // still get a valid, strictly-increasing 0..pi set of edges.
    const int masterN = 720;
    const int nOut    = 12;
    std::vector<double> edges;
    IntensityGrid::buildBeamAdaptiveEdges(std::vector<double>(std::size_t(masterN), 0.0),
                                          masterN, nOut, 0.0, edges);
    CHECK(int(edges.size()) == nOut + 1);
    CHECK_NEAR(edges.front(), 0.0, 1e-12);
    CHECK_NEAR(edges.back(), kPi, 1e-12);
    for (int i = 1; i <= nOut; ++i) {
        const double want = kPi * double(i) / double(nOut);
        CHECK_NEAR(edges[std::size_t(i)], want, 1e-9);
    }
}

TEST(thetabin, accessors_agree_across_the_adaptive_grid) {
    // Every bin read (center, low/high, width, solid angle, direction->bin)
    // must be mutually consistent on a hand-built adaptive grid: widths sum to
    // pi, centers lie inside their spans, solid angles are positive and sum to
    // the full sphere, and thetaBinIndex inverts the edges. This is the grid
    // RayTracer::finishIntensity fills and every exporter reads.
    const int nTheta = 10, nPhi = 8;
    IntensityGrid g;
    g.nTheta = nTheta;
    g.nPhi   = nPhi;
    // An adaptive, beam-concentrated edge set.
    IntensityGrid::buildBeamAdaptiveEdges(bandFlux(400, 200, 30, 100.0),
                                          400, nTheta, 0.0, g.thetaEdges);
    g.bin.assign(std::size_t(nTheta * nPhi), 0.0);

    CHECK(g.nTheta == nTheta);
    CHECK(g.adaptive());
    CHECK(g.thetaEdges.size() == std::size_t(nTheta) + 1);

    double widthsSum = 0.0, solidSum = 0.0;
    for (int i = 0; i < nTheta; ++i) {
        const double lo = g.thetaLowRad(i), hi = g.thetaHighRad(i);
        CHECK(lo < hi);
        const double c = g.thetaCenterRad(i);
        CHECK(c >= lo && c <= hi);
        CHECK_NEAR(g.thetaCenterDeg(i), c * 180.0 / kPi, 1e-9);
        CHECK(g.thetaWidthDeg(i) > 0.0);
        widthsSum += (hi - lo);
        solidSum += g.cellSolidAngleRad(i);
        // The accessor must report the same bin the direction-to-bin lookup does.
        const double midCos = std::cos(c);
        CHECK(g.thetaBinIndex(midCos) == i);
    }
    CHECK_NEAR(widthsSum, kPi, 1e-9);
    // cellSolidAngle is per phi-cell, so summing over the nTheta rings alone
    // covers every phi-column once: total = nPhi * 4pi/nPhi = 4pi, the whole
    // sphere. The theta-sum below is the 4pi/nPhi per-column share.
    CHECK_NEAR(solidSum, 4.0 * kPi / double(nPhi), 1e-9);
    // thetaStepDeg is documented as the whole-span mean.
    CHECK_NEAR(g.thetaStepDeg(), 180.0 / double(nTheta), 1e-12);
}
