#include "Bsdf.h"
#include "Optics.h"

#include <algorithm>
#include <cmath>

namespace bsdf {

using optics::kPi;
using optics::kTwoPi;
using optics::uniform01;
using optics::orthonormalBasis;
using optics::cosineHemisphere;

QString modelName(Model m) {
    switch (m) {
    case Model::Specular:   return QStringLiteral("Specular");
    case Model::Microfacet: return QStringLiteral("Microfacet (GGX)");
    case Model::Abg:        return QStringLiteral("ABg");
    case Model::Lambertian: return QStringLiteral("Lambertian");
    case Model::Table:      return QStringLiteral("Measured BSDF");
    }
    return QStringLiteral("Specular");
}

QString modelTip(Model m) {
    switch (m) {
    case Model::Specular:
        return QStringLiteral("A polished surface: one outgoing direction, no scatter.");
    case Model::Microfacet:
        return QStringLiteral("GGX with Smith shadowing-masking. One roughness parameter, "
                              "energy conserving, and correctly sampled -- unlike a Gaussian "
                              "tilt of the normal, which biases toward specular by however "
                              "often the tilt has to be rejected.");
    case Model::Abg:
        return QStringLiteral("The A, B, g model TracePro and LightTools expose. Its "
                              "parameters map directly onto a measured BSDF and onto total "
                              "integrated scatter, which is what lets a scatter spec sheet "
                              "be reproduced rather than approximated.");
    case Model::Lambertian:
        return QStringLiteral("A perfect diffuser: cosine-weighted about the normal, whatever "
                              "the angle of incidence.");
    case Model::Table:
        return QStringLiteral("Measured BSDF against |beta - beta0|, used as it stands.");
    }
    return QString();
}

// ---- ABg --------------------------------------------------------------------
// BSDF(beta) = A / (B^g + |beta - beta0|^g), with beta the direction cosine of
// the outgoing ray projected into the surface plane. Away from the specular core
// it is a straight line on a log-log plot of slope -g, which is exactly how a
// scatterometer reports a polished surface.

namespace {

double abgValue(const Surface& s, double dbeta) {
    const double b = std::max(1e-9, s.abgB);
    const double g = std::max(0.05, s.abgG);
    return s.abgA / (std::pow(b, g) + std::pow(std::max(0.0, dbeta), g));
}

// Total integrated scatter of an ABg lobe about normal incidence, by quadrature
// over the hemisphere. The model has a closed form only for g = 2, and a
// hundred-step integral of a smooth function is exact enough to normalise with.
double abgTis(const Surface& s) {
    constexpr int kSteps = 400;
    double total = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        // beta is the sine of the scatter angle, so the hemisphere measure is
        // 2 pi beta d(beta) with the cosine already folded into the BSDF's
        // definition as a radiance ratio.
        const double b0 = double(i) / kSteps;
        const double b1 = double(i + 1) / kSteps;
        const double bm = 0.5 * (b0 + b1);
        total += abgValue(s, bm) * kTwoPi * bm * (b1 - b0);
    }
    return total;
}

double sampleTable(const Surface& s, double dbeta) {
    if (s.samples <= 0) return 0.0;
    if (s.samples == 1 || dbeta <= s.tableBeta[0]) return s.tableValue[0];
    if (dbeta >= s.tableBeta[s.samples - 1]) return s.tableValue[s.samples - 1];
    int i = 0;
    while (i + 1 < s.samples && s.tableBeta[i + 1] < dbeta) ++i;
    const double span = s.tableBeta[i + 1] - s.tableBeta[i];
    if (span <= 0.0) return s.tableValue[i];
    const double f = (dbeta - s.tableBeta[i]) / span;
    // Interpolated in the log, because a BSDF spans decades and a linear
    // interpolation between two of them describes neither.
    const double a = std::max(1e-30, s.tableValue[i]);
    const double b = std::max(1e-30, s.tableValue[i + 1]);
    return std::exp(std::log(a) * (1.0 - f) + std::log(b) * f);
}

double tableTis(const Surface& s) {
    if (s.samples <= 0) return 0.0;
    constexpr int kSteps = 400;
    double total = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double b0 = double(i) / kSteps;
        const double b1 = double(i + 1) / kSteps;
        const double bm = 0.5 * (b0 + b1);
        total += sampleTable(s, bm) * kTwoPi * bm * (b1 - b0);
    }
    return total;
}

// GGX normal distribution and the Smith height-correlated masking term. The
// second is what the Gaussian jitter never had: without it, energy that should
// have been shadowed by a neighbouring microfacet is counted twice at grazing.
double ggxD(double cosTheta, double alpha) {
    const double a2 = alpha * alpha;
    const double c2 = cosTheta * cosTheta;
    const double d  = c2 * (a2 - 1.0) + 1.0;
    return a2 / std::max(1e-18, kPi * d * d);
}

double smithG1(double cosV, double alpha) {
    if (cosV <= 0.0) return 0.0;
    const double a2 = alpha * alpha;
    const double t  = (1.0 - cosV * cosV) / std::max(1e-18, cosV * cosV);
    return 2.0 / (1.0 + std::sqrt(1.0 + a2 * t));
}

// A GGX half-vector about `n`.
Vec3 sampleGgxHalf(const Vec3& n, double alpha, double u1, double u2) {
    const double theta = std::atan(alpha * std::sqrt(u1) / std::sqrt(std::max(1e-12, 1.0 - u1)));
    const double phi   = kTwoPi * u2;
    Vec3 t, b;
    orthonormalBasis(n, t, b);
    const double st = std::sin(theta), ct = std::cos(theta);
    Vec3 h = t * (st * std::cos(phi)) + b * (st * std::sin(phi)) + n * ct;
    h.normalize();
    return h;
}

} // namespace

double Surface::value(double dbeta) const {
    switch (model) {
    case Model::Abg:        return abgValue(*this, dbeta);
    case Model::Table:      return sampleTable(*this, dbeta);
    case Model::Lambertian: return fraction / kPi;
    case Model::Microfacet: {
        // The GGX lobe about the specular direction, as a function of the same
        // offset the other models are written in.
        const double th = std::asin(std::clamp(dbeta, 0.0, 1.0));
        return ggxD(std::cos(0.5 * th), std::max(1e-6, alpha));
    }
    case Model::Specular:   break;
    }
    return 0.0;
}

double Surface::totalIntegratedScatter() const {
    switch (model) {
    case Model::Specular:   return 0.0;
    case Model::Lambertian: return fraction;
    case Model::Microfacet: return fraction;
    case Model::Abg:        return std::clamp(fraction * abgTis(*this), 0.0, 1.0);
    case Model::Table:      return std::clamp(fraction * tableTis(*this), 0.0, 1.0);
    }
    return 0.0;
}

bool Surface::sample(const Vec3& wi, const Vec3& n, const Vec3& spec,
                     std::uint64_t& rng, Vec3& out, double& weight) const {
    weight = 1.0;
    if (isSpecular()) return false;

    switch (model) {
    case Model::Lambertian: {
        out = cosineHemisphere(n, uniform01(rng), uniform01(rng));
        return out.dot(n) > 0.0;
    }
    case Model::Microfacet: {
        Vec3 h;
        return sampleMicrofacet(wi, n, rng, h, out, weight);
    }
    case Model::Abg:
    case Model::Table: {
        // Sample the scatter angle about the specular direction, with a density
        // that follows the lobe. The ABg core has width B, so drawing
        // |beta - beta0| from a distribution of that width and correcting by the
        // ratio keeps the weight near one where the lobe carries its energy.
        const double b   = std::max(1e-9, abgB);
        const double u1  = uniform01(rng);
        // A heavy-tailed draw: b * (u^(-1/g') - 1) with g' = g - 1 covers the
        // wings without spending every sample in them.
        const double gp  = std::max(0.5, (model == Model::Abg ? abgG : 2.0) - 1.0);
        const double db  = b * (std::pow(std::max(1e-12, u1), -1.0 / gp) - 1.0);
        if (!(db >= 0.0) || db > 2.0) return false;

        const double phi = kTwoPi * uniform01(rng);
        Vec3 t, bt;
        orthonormalBasis(spec, t, bt);
        // db is a direction-cosine offset, so the polar angle it stands for is
        // its arcsine.
        const double theta = std::asin(std::clamp(db, 0.0, 1.0));
        Vec3 o = t * (std::sin(theta) * std::cos(phi)) + bt * (std::sin(theta) * std::sin(phi)) +
                 spec * std::cos(theta);
        if (!o.normalize()) return false;
        if (o.dot(n) <= 1e-9) return false;   // below the surface: keep the specular ray

        // pdf of the draw above, per unit beta, times the 2 pi beta measure.
        const double pdf = (gp / b) * std::pow(1.0 + db / b, -gp - 1.0) /
                           std::max(1e-12, kTwoPi * std::max(1e-9, db));
        const double f = (model == Model::Abg) ? abgValue(*this, db) : sampleTable(*this, db);
        weight = std::clamp(f / std::max(1e-30, pdf), 0.0, 4.0);
        out = o;
        return true;
    }
    case Model::Specular:
        break;
    }
    return false;
}

bool Surface::sampleMicrofacet(const Vec3& wi, const Vec3& n, std::uint64_t& rng,
                               Vec3& micronormal, Vec3& reflected,
                               double& weight) const {
    weight = 1.0;
    if (model != Model::Microfacet || alpha <= 1e-6) return false;

    const Vec3 h = sampleGgxHalf(n, alpha, uniform01(rng), uniform01(rng));
    // A facet turned past the incoming ray would put the interaction on its
    // back, which is not a facet this ray can meet.
    if (h.dot(wi) >= 0.0) return false;

    // Reflect the incoming direction about the sampled microfacet.
    Vec3 o = wi - h * (2.0 * wi.dot(h));
    if (!o.normalize()) return false;
    const double cosO = o.dot(n);
    const double cosI = -wi.dot(n);
    if (cosO <= 1e-9 || cosI <= 1e-9) return false;

    // Sampling the half-vector from D(h) cos(h.n) leaves the weight as the
    // Smith masking term over its own D-cancelled parts -- which is what makes
    // this energy conserving rather than merely plausible.
    const double cosH = h.dot(n);
    if (cosH <= 1e-9) return false;
    const double g = smithG1(cosI, alpha) * smithG1(cosO, alpha);
    const double denom = std::max(1e-12, cosI * cosH);

    weight      = std::clamp(g * std::fabs(wi.dot(h)) / denom, 0.0, 4.0);
    micronormal = h;
    reflected   = o;
    return true;
}

// ---- volume scattering -------------------------------------------------------

double henyeyGreenstein(double cosTheta, double g) {
    g = std::clamp(g, -0.99, 0.99);
    const double d = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / std::max(1e-12, 4.0 * kPi * d * std::sqrt(d));
}

// The Gegenbauer kernel, normalised over the sphere:
//
//   p(mu) = alpha g (1-g^2)^(2a) / ( pi [ (1+g)^(2a) - (1-g)^(2a) ] )
//           / (1 + g^2 - 2 g mu)^(a+1)
//
// Reynolds and McCormick's two-parameter family. At a = 1/2 the prefactor
// collapses to (1-g^2)/(4 pi) and the exponent to 3/2, which is Henyey-
// Greenstein term for term.
double gegenbauer(double cosTheta, double g, double alpha) {
    g     = std::clamp(g, -0.99, 0.99);
    alpha = std::clamp(alpha, 0.01, 10.0);
    // g -> 0 makes the normalisation 0/0; the limit is the isotropic sphere,
    // which is also what an unset asymmetry means.
    if (std::fabs(g) < 1e-6) return 1.0 / (4.0 * kPi);

    const double a2   = 2.0 * alpha;
    const double span = std::pow(1.0 + g, a2) - std::pow(1.0 - g, a2);
    if (std::fabs(span) < 1e-300) return 1.0 / (4.0 * kPi);

    const double k = alpha * g * std::pow(1.0 - g * g, a2) / (kPi * span);
    const double d = std::max(1e-12, 1.0 + g * g - 2.0 * g * cosTheta);
    return k / std::pow(d, alpha + 1.0);
}

double phaseValue(const Volume& v, double cosTheta) {
    return v.phase == Phase::Gegenbauer ? gegenbauer(cosTheta, v.anisotropy, v.alpha)
                                        : henyeyGreenstein(cosTheta, v.anisotropy);
}

double Volume::sampleDistance(std::uint64_t& rng) const {
    if (coefficient <= 0.0) return 1e30;
    const double u = std::max(1e-12, uniform01(rng));
    return -std::log(u) / coefficient;
}

Vec3 Volume::scatter(const Vec3& d, std::uint64_t& rng) const {
    const double g = std::clamp(anisotropy, -0.99, 0.99);
    const double u = uniform01(rng);
    double cosTheta;
    if (std::fabs(g) < 1e-4) {
        cosTheta = 1.0 - 2.0 * u;
    } else if (phase == Phase::Gegenbauer) {
        // The exact inverse of the Gegenbauer kernel's cumulative distribution.
        // Integrating p(mu) over the sphere from -1 gives
        //   F(mu) = D [ (1+g^2-2 g mu)^-a - (1+g)^-2a ],  D = (1-g^2)^2a / span
        // which inverts in closed form -- so this costs two pow() calls and no
        // rejection loop, exactly like the HG case beside it.
        const double a    = std::clamp(alpha, 0.01, 10.0);
        const double a2   = 2.0 * a;
        const double span = std::pow(1.0 + g, a2) - std::pow(1.0 - g, a2);
        const double D    = std::pow(1.0 - g * g, a2) / span;
        const double base = u / D + std::pow(1.0 + g, -a2);
        cosTheta = (1.0 + g * g - std::pow(base, -1.0 / a)) / (2.0 * g);
    } else {
        // The exact inverse of the Henyey-Greenstein cumulative distribution.
        const double s = (1.0 - g * g) / (1.0 - g + 2.0 * g * u);
        cosTheta = (1.0 + g * g - s * s) / (2.0 * g);
    }
    cosTheta = std::clamp(cosTheta, -1.0, 1.0);
    const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
    const double phi = kTwoPi * uniform01(rng);

    Vec3 t, b;
    orthonormalBasis(d, t, b);
    Vec3 out = t * (sinTheta * std::cos(phi)) + b * (sinTheta * std::sin(phi)) + d * cosTheta;
    out.normalize();
    return out;
}

} // namespace bsdf
