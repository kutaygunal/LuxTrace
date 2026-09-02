#include "SimulationResult.h"
// Plain data struct; nothing to implement.

// Builds the reported far field from a fine uniform accumulation grid.
//
// Here rather than inside the reference tracer because both backends have to
// produce the *same* far field: the output bins are beam-adaptive, derived from
// the data, so two backends that built their own edges would be compared ring
// against different ring. One function, one partition, two callers.
namespace {
// The same two constants the grid's own accessors use; IntensityGrid declares
// kPi itself, and this file had no need of them until the far-field build moved
// here.
constexpr double kPi    = IntensityGrid::kPi;
constexpr double kTwoPi = 2.0 * IntensityGrid::kPi;
} // namespace

void finishIntensityGrid(IntensityGrid& g, std::vector<double> master, int nTheta, int nPhi) {
    g.nTheta = nTheta;
    g.nPhi   = nPhi;
    g.perSteradian.assign(std::size_t(nTheta) * std::size_t(nPhi), 0.0);
    g.profile.assign(std::size_t(nTheta), 0.0);
    g.totalFlux = 0.0;
    g.thetaEdges.clear();
    if (nTheta <= 0 || nPhi <= 0 || master.empty()) {
        g.bin = std::move(master);
        return;
    }

    // `master` is the fine uniform accumulation grid (its theta resolution is
    // what lets a narrow beam be seen at all). The requested output grid is
    // beam-adaptive: equal-flux theta bins are rebuilt from it, so the bins
    // concentrate where the flux is -- which is the fix that lets a ~1.3 deg
    // beam span several output bins at the default 90 instead of under one.
    const int masterN = int(master.size()) / nPhi;
    g.bin.assign(std::size_t(nTheta) * std::size_t(nPhi), 0.0);

    // Ring flux (sum over phi) of the master grid -- the signal the theta edges
    // are built from.
    std::vector<double> ringFlux(std::size_t(masterN), 0.0);
    double peakRing = 0.0;
    for (int m = 0; m < masterN; ++m) {
        double s = 0.0;
        for (int ip = 0; ip < nPhi; ++ip)
            s += master[std::size_t(m) * std::size_t(nPhi) + std::size_t(ip)];
        ringFlux[m] = s;
        peakRing = std::max(peakRing, s);
    }
    const double baseline = peakRing > 0.0 ? 1e-9 * peakRing : 1e-12;
    IntensityGrid::buildBeamAdaptiveEdges(ringFlux, masterN, nTheta, baseline, g.thetaEdges);

    // Aggregate master cells into the adaptive output; a master bin straddling
    // an output edge is split by the fraction of its theta width each ring owns,
    // so the total flux is conserved exactly.
    const double dPhi = kTwoPi / double(nPhi);
    const double Wm   = kPi / double(masterN);
    for (int k = 0; k < nTheta; ++k) {
        const double a = g.thetaEdges[std::size_t(k)];
        const double b = g.thetaEdges[std::size_t(k) + 1];
        const double omega = dPhi * (std::cos(a) - std::cos(b));   // one cell's solid angle
        const int m0 = std::clamp(int(a / Wm), 0, masterN - 1);
        const int m1 = std::clamp(int(b / Wm), 0, masterN - 1);
        for (int m = m0; m <= m1; ++m) {
            const double lo = std::max(a, m * Wm);
            const double hi = std::min(b, (m + 1) * Wm);
            if (hi <= lo) continue;
            const double frac = (hi - lo) / Wm;
            for (int ip = 0; ip < nPhi; ++ip) {
                const std::size_t mk = std::size_t(m) * std::size_t(nPhi) + std::size_t(ip);
                const std::size_t ok = std::size_t(k) * std::size_t(nPhi) + std::size_t(ip);
                const double v = frac * master[mk];
                g.bin[ok] += v;
                g.totalFlux += v;
            }
        }
        double ringSum = 0.0;
        for (int ip = 0; ip < nPhi; ++ip) {
            const std::size_t ok = std::size_t(k) * std::size_t(nPhi) + std::size_t(ip);
            if (omega > 1e-15) g.perSteradian[ok] = g.bin[ok] / omega;
            ringSum += g.perSteradian[ok];
        }
        g.profile[std::size_t(k)] = ringSum / double(nPhi);
    }

    int peakBin = 0;
    for (int it = 0; it < nTheta; ++it)
        if (g.profile[std::size_t(it)] > g.profile[std::size_t(peakBin)]) peakBin = it;
    g.peak = g.profile[std::size_t(peakBin)];

    // Full width at half maximum, walking out from the peak and interpolating the
    // crossing so the answer is not quantised to the bin width.
    g.fwhmDeg = 0.0;
    if (g.peak > 0.0) {
        const double half = 0.5 * g.peak;
        auto edge = [&](int step) {
            int i = peakBin;
            while (i + step >= 0 && i + step < nTheta &&
                   g.profile[std::size_t(i + step)] >= half) i += step;
            const int j = i + step;
            double a = g.thetaCenterDeg(i);
            if (j >= 0 && j < nTheta) {
                const double vi = g.profile[std::size_t(i)];
                const double vj = g.profile[std::size_t(j)];
                if (vi > vj) a += (g.thetaCenterDeg(j) - a) * (vi - half) / (vi - vj);
            }
            return a;
        };
        g.fwhmDeg = std::fabs(edge(+1) - edge(-1));
    }
}

