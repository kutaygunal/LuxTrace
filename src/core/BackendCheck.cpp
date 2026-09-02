#include "BackendCheck.h"

#include <algorithm>
#include <cmath>

#include <QTextStream>

namespace backendcheck {
namespace {

// E|X| for a zero-mean normal of unit sigma. The noise floor is a sum of such
// absolute differences, so this is the factor that turns sigmas into the
// distance they actually produce.
constexpr double kMeanAbsNormal = 0.7978845608028654;   // sqrt(2/pi)

std::vector<double> normalised(const std::vector<double>& v) {
    double sum = 0.0;
    for (double x : v) sum += std::max(0.0, x);
    std::vector<double> out(v.size(), 0.0);
    if (sum <= 0.0) return out;
    for (std::size_t i = 0; i < v.size(); ++i) out[i] = std::max(0.0, v[i]) / sum;
    return out;
}

// The standard error of a fraction measured from n samples. An estimate, and a
// generous one on a weighted run: variance reduction makes the effective sample
// size smaller than the ray count, so this understates the true error bar and
// therefore over-reports sigma rather than hiding a difference.
double binomialStdErr(double fraction, std::size_t n) {
    if (n == 0) return 0.0;
    const double f = std::clamp(fraction, 0.0, 1.0);
    return std::sqrt(std::max(0.0, f * (1.0 - f)) / double(n));
}

Comparison scalar(const QString& name, double ref, double prev, double stdErr,
                  const Tolerance& tol) {
    Comparison c;
    c.name      = name;
    c.reference = ref;
    c.preview   = prev;
    c.stdErr    = stdErr;
    const double diff = prev - ref;
    const double scale = std::max(std::fabs(ref), std::fabs(prev));
    c.relative = scale > 0.0 ? diff / scale : 0.0;
    if (stdErr > 0.0) {
        c.hasSigma = true;
        c.sigma    = diff / stdErr;
        c.ok       = std::fabs(c.sigma) <= tol.sigmas;
    } else {
        // No error bar: either the quantity is exact or the run was too small
        // to give it one. A plain relative band is the honest fallback.
        c.ok = std::fabs(c.relative) <= tol.relative;
    }
    return c;
}

MapComparison mapPair(const QString& name, const std::vector<double>& a,
                      const std::vector<double>& b, std::size_t na, std::size_t nb,
                      const Tolerance& tol) {
    MapComparison m;
    m.name = name;
    if (a.empty() || b.empty() || a.size() != b.size()) return m;
    m.comparable = true;
    m.distance   = totalVariation(a, b);
    m.noiseFloor = noiseFloorFor(a, b, na, nb);
    if (m.noiseFloor > 0.0) {
        m.ratio = m.distance / m.noiseFloor;
        m.ok    = m.ratio <= tol.mapRatio;
    } else {
        // Nothing to compare against: identical is the only pass.
        m.ratio = m.distance > 0.0 ? 1e30 : 0.0;
        m.ok    = m.distance <= 0.0;
    }
    return m;
}

// Two adaptive theta partitions are the same partition when every edge agrees
// to a hair. Not exactly equal: the edges come from the run own histogram,
// so two correct runs place them a rounding apart.
bool sameEdges(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;   // both on the legacy uniform grid
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > 1e-6) return false;
    return true;
}

} // namespace

double totalVariation(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    const std::vector<double> pa = normalised(a);
    const std::vector<double> pb = normalised(b);
    double sum = 0.0;
    for (std::size_t i = 0; i < pa.size(); ++i) sum += std::fabs(pa[i] - pb[i]);
    return 0.5 * sum;
}

double noiseFloorFor(const std::vector<double>& a, const std::vector<double>& b,
                     std::size_t raysA, std::size_t raysB) {
    if (a.size() != b.size() || a.empty() || raysA == 0 || raysB == 0) return 0.0;
    const std::vector<double> pa = normalised(a);
    const std::vector<double> pb = normalised(b);

    // Pooled estimate of the underlying distribution: neither run's own
    // histogram is the truth, and the average of the two is the best available
    // guess at what both were drawn from.
    const double inv = std::sqrt(1.0 / double(raysA) + 1.0 / double(raysB));
    double s = 0.0;
    for (std::size_t i = 0; i < pa.size(); ++i) {
        const double p = 0.5 * (pa[i] + pb[i]);
        s += std::sqrt(std::max(0.0, p * (1.0 - p)));
    }
    return 0.5 * kMeanAbsNormal * inv * s;
}

double measuredNoiseFloor(const std::vector<double>& a, const std::vector<double>& varA,
                          const std::vector<double>& b, const std::vector<double>& varB) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    if (varA.size() != a.size() || varB.size() != b.size()) return 0.0;

    double ta = 0.0, tb = 0.0;
    for (double x : a) ta += std::max(0.0, x);
    for (double x : b) tb += std::max(0.0, x);
    if (ta <= 0.0 || tb <= 0.0) return 0.0;

    // Each bin's share carries the bin's own error over the run's total. The
    // total is far better determined than any single bin, so treating it as
    // exact is the right approximation and errs towards a smaller floor.
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double va = std::max(0.0, varA[i]) / (ta * ta);
        const double vb = std::max(0.0, varB[i]) / (tb * tb);
        s += std::sqrt(va + vb);
    }
    return 0.5 * kMeanAbsNormal * s;
}

Report compare(const SimulationResult& reference, const SimulationResult& preview,
               const Tolerance& tol) {
    Report r;

    if (reference.raysEmitted == 0 || preview.raysEmitted == 0) {
        r.blocker = QStringLiteral("one of the runs emitted no rays");
        r.ok = false;
        return r;
    }
    if (reference.unit != preview.unit) {
        r.blocker = QStringLiteral("the two runs report in different units");
        r.ok = false;
        return r;
    }
    // Comparing distributions across different receiver geometry would compare
    // the receivers, not the backends.
    if (reference.nx != preview.nx || reference.ny != preview.ny ||
        std::fabs(reference.detW - preview.detW) > 1e-9 ||
        std::fabs(reference.detH - preview.detH) > 1e-9) {
        r.blocker = QStringLiteral("the two runs used different receiver grids");
        r.ok = false;
        return r;
    }

    const std::size_t nA = reference.raysEmitted;
    const std::size_t nB = preview.raysEmitted;

    // Efficiency is the one quantity both runs measure their own error on, so
    // it is the only comparison here that is fully self-supporting.
    const double effSe = std::sqrt(reference.efficiencyStdErr * reference.efficiencyStdErr +
                                   preview.efficiencyStdErr * preview.efficiencyStdErr);
    r.scalars.push_back(scalar(QStringLiteral("efficiency"),
                               reference.efficiency, preview.efficiency, effSe, tol));

    auto budget = [&](const QString& name, double refFlux, double prevFlux) {
        const double fr = reference.sourcePower > 0.0 ? refFlux / reference.sourcePower : 0.0;
        const double fp = preview.sourcePower > 0.0 ? prevFlux / preview.sourcePower : 0.0;
        const double se = std::sqrt(binomialStdErr(fr, nA) * binomialStdErr(fr, nA) +
                                    binomialStdErr(fp, nB) * binomialStdErr(fp, nB));
        r.scalars.push_back(scalar(name, fr, fp, se, tol));
    };
    budget(QStringLiteral("absorbed"),  reference.fluxAbsorbed,  preview.fluxAbsorbed);
    budget(QStringLiteral("escaped"),   reference.fluxEscaped,   preview.fluxEscaped);
    budget(QStringLiteral("truncated"), reference.fluxTruncated, preview.fluxTruncated);
    budget(QStringLiteral("rejected"),  reference.fluxRejected,  preview.fluxRejected);

    // Geometric quantities carry no error bar of their own and barely move with
    // the sample, so they get the relative band.
    auto geometric = [&](const QString& name, double ref, double prev) {
        r.scalars.push_back(scalar(name, ref, prev, 0.0, tol));
    };
    geometric(QStringLiteral("source power"), reference.sourcePower, preview.sourcePower);

    // The two far-field numbers a person reads. Shown, not gated.
    auto ungated = [&](const QString& name, double ref, double prev) {
        Comparison c = scalar(name, ref, prev, 0.0, tol);
        c.gated = false;
        c.ok    = true;
        r.scalars.push_back(c);
    };
    if (!reference.intensity.bin.empty() && !preview.intensity.bin.empty()) {
        ungated(QStringLiteral("peak intensity"), reference.intensity.peak, preview.intensity.peak);
        ungated(QStringLiteral("beam FWHM deg"), reference.intensity.fwhmDeg, preview.intensity.fwhmDeg);
    }

    for (const Comparison& c : r.scalars) {
        if (!c.gated) continue;
        if (!c.ok) r.ok = false;
        if (c.hasSigma && std::fabs(c.sigma) > std::fabs(r.worstSigma)) {
            r.worstSigma = c.sigma;
            r.worstName  = c.name;
        }
    }

    // The distributions. The sample size for a receiver map is the number of
    // arrivals, not the number of rays emitted: a ray that missed tells the map
    // nothing.
    MapComparison irr = mapPair(QStringLiteral("irradiance"),
                                reference.irradiance, preview.irradiance,
                                std::max<std::size_t>(1, reference.raysHitDetector),
                                std::max<std::size_t>(1, preview.raysHitDetector), tol);
    // Prefer the floor the runs measured over the one a count model guesses.
    // Where both are available the measured one is used even when it is larger,
    // because it is the one that knows about weights.
    if (irr.comparable) {
        const double measured = measuredNoiseFloor(reference.irradiance, reference.irradianceVar,
                                                   preview.irradiance, preview.irradianceVar);
        if (measured > 0.0) {
            irr.noiseFloor = measured;
            irr.ratio      = irr.distance / measured;
            irr.ok         = irr.ratio <= tol.mapRatio;
            irr.reason     = QStringLiteral("floor from the runs' own per-bin variance");
        } else {
            irr.reason = QStringLiteral("floor from a count model; no noise map in the runs");
        }
    }
    r.maps.push_back(irr);
    // The far field only if both runs binned it the same way. The theta
    // partition adapts to where the light went, so two runs that agree about
    // the beam still get slightly different rings -- and a bin-for-bin
    // distance across different rings measures the partition, not the light.
    // The far field on the grid it was accumulated on, not the one it is
    // reported on. The reported rings are derived from each run's own histogram,
    // so two correct runs land on slightly different rings and a cell-for-cell
    // distance would measure the partition rather than the light. The master
    // grid is uniform and the same shape whatever the beam did.
    MapComparison far;
    far.name = QStringLiteral("far field");
    if (reference.intensityMaster.empty() || preview.intensityMaster.empty()) {
        far.reason = QStringLiteral("no far-field noise map; run with noiseMap on");
    } else if (reference.intensityMasterTheta != preview.intensityMasterTheta ||
               reference.intensityMasterPhi   != preview.intensityMasterPhi) {
        far.reason = QStringLiteral("different far-field resolution");
    } else {
        far = mapPair(far.name, reference.intensityMaster, preview.intensityMaster,
                      nA, nB, tol);
        const double measured = measuredNoiseFloor(
            reference.intensityMaster, reference.intensityMasterVar,
            preview.intensityMaster,   preview.intensityMasterVar);
        if (far.comparable && measured > 0.0) {
            far.noiseFloor = measured;
            far.ratio      = far.distance / measured;
            far.ok         = far.ratio <= tol.mapRatio;
            far.reason     = QStringLiteral("floor from the runs' own per-bin variance");
        }
    }
    r.maps.push_back(far);

    for (const MapComparison& m : r.maps)
        if (m.comparable && !m.ok) r.ok = false;

    return r;
}

QString Report::text() const {
    QString out;
    QTextStream ts(&out);

    if (!blocker.isEmpty()) {
        ts << "cannot compare: " << blocker << "\n";
        return out;
    }

    ts << "quantity            reference       preview        difference   sigma   verdict\n";
    ts << "-----------------------------------------------------------------------------\n";
    for (const Comparison& c : scalars) {
        ts << QString(c.name).leftJustified(18)
           << QString::number(c.reference, 'g', 8).rightJustified(14)
           << QString::number(c.preview,   'g', 8).rightJustified(14)
           << QString::number(c.preview - c.reference, 'g', 4).rightJustified(14);
        if (c.hasSigma) ts << QString::number(c.sigma, 'f', 2).rightJustified(8);
        else            ts << QStringLiteral("     n/a");
        ts << (!c.gated ? QStringLiteral("   shown")
                        : c.ok ? QStringLiteral("   ok") : QStringLiteral("   DIFFERS")) << "\n";
    }

    ts << "\ndistribution        distance    noise floor   ratio   verdict\n";
    ts << "-----------------------------------------------------------------------------\n";
    for (const MapComparison& m : maps) {
        ts << QString(m.name).leftJustified(18);
        if (!m.comparable) {
            ts << "  not comparable"
               << (m.reason.isEmpty() ? QString()
                                      : QStringLiteral(" -- ") + m.reason) << "\n";
            continue;
        }
        ts << QString::number(m.distance,   'g', 4).rightJustified(10)
           << QString::number(m.noiseFloor, 'g', 4).rightJustified(14)
           << QString::number(m.ratio,      'f', 2).rightJustified(8)
           << (m.ok ? QStringLiteral("   ok") : QStringLiteral("   DIFFERS")) << "\n";
    }

    ts << "\n";
    if (ok) {
        ts << "The preview is indistinguishable from another draw of the reference.\n";
        if (!worstName.isEmpty())
            ts << "Worst scalar: " << worstName << " at "
               << QString::number(worstSigma, 'f', 2) << " sigma.\n";
    } else {
        ts << "The preview DIFFERS from the reference by more than sampling explains.\n";
        if (!worstName.isEmpty())
            ts << "Worst scalar: " << worstName << " at "
               << QString::number(worstSigma, 'f', 2) << " sigma.\n";
    }
    return out;
}

} // namespace backendcheck
