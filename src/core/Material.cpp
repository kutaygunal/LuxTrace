#include "Material.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kLambdaD = 587.5618;   // helium d
constexpr double kLambdaF = 486.1327;   // hydrogen F
constexpr double kLambdaC = 656.2725;   // hydrogen C

// Linear interpolation of a sample table, clamped at both ends. Extrapolating a
// measured metal beyond its data would invent an optic.
double interpolate(const double* x, const double* y, int n, double at) {
    if (n <= 0) return 0.0;
    if (n == 1 || at <= x[0]) return y[0];
    if (at >= x[n - 1]) return y[n - 1];
    int i = 0;
    while (i + 1 < n && x[i + 1] < at) ++i;
    const double span = x[i + 1] - x[i];
    if (span <= 0.0) return y[i];
    const double f = (at - x[i]) / span;
    return y[i] * (1.0 - f) + y[i + 1] * f;
}

OpticalMaterial constant(double nd, double alpha = 0.0) {
    OpticalMaterial m;
    m.set   = true;
    m.model = OpticalMaterial::Model::Constant;
    m.nd    = nd;
    m.alpha = alpha;
    return m;
}

OpticalMaterial sellmeier(const double b[3], const double c[3], double alpha) {
    OpticalMaterial m;
    m.set   = true;
    m.model = OpticalMaterial::Model::Sellmeier;
    for (int i = 0; i < 3; ++i) { m.sell[i] = b[i]; m.sell[3 + i] = c[i]; }
    m.alpha = alpha;
    m.nd    = 0.0;                 // filled in below from the model itself
    m.nd    = m.indexAt(kLambdaD);
    return m;
}

// A metal: n and k measured across the visible band. The index is the real part
// and is never used for refraction (a metal surface here is opaque); what it
// drives is the angle- and wavelength-dependent reflectance.
OpticalMaterial metal(std::initializer_list<double> lam,
                      std::initializer_list<double> n,
                      std::initializer_list<double> k) {
    OpticalMaterial m;
    m.set   = true;
    m.model = OpticalMaterial::Model::Table;
    m.samples = int(std::min<std::size_t>(OpticalMaterial::kMaxSamples, lam.size()));
    for (int i = 0; i < m.samples; ++i) {
        m.lambdaNm[i] = *(lam.begin() + i);
        m.nSample[i]  = *(n.begin() + i);
        m.kSample[i]  = *(k.begin() + i);
    }
    m.nd = m.indexAt(kLambdaD);
    return m;
}

struct Entry {
    QString         name;
    QString         description;
    OpticalMaterial material;
};

const std::vector<Entry>& catalogue() {
    static const std::vector<Entry> c = [] {
        std::vector<Entry> v;

        v.push_back({QStringLiteral("Vacuum"),
                     QStringLiteral("n = 1 at every wavelength; the absence of a medium."),
                     constant(1.0)});

        // --- glasses, Sellmeier coefficients as published -------------------
        {
            const double b[3] = {1.03961212, 0.231792344, 1.01046945};
            const double c[3] = {0.00600069867, 0.0200179144, 103.560653};
            // 0.998 internal transmittance per 10 mm is ordinary for a crown.
            v.push_back({QStringLiteral("N-BK7"),
                         QStringLiteral("Borosilicate crown. n_d 1.5168, Abbe 64.2. "
                                        "The default glass of almost every catalogue lens."),
                         sellmeier(b, c, 2.0e-4)});
        }
        {
            const double b[3] = {1.73759695, 0.313747346, 1.89878101};
            const double c[3] = {0.013188707, 0.0623068142, 155.23629};
            v.push_back({QStringLiteral("N-SF11"),
                         QStringLiteral("Dense flint. n_d 1.7847, Abbe 25.7. Four times the "
                                        "dispersion of a crown, which is what makes an achromat work."),
                         sellmeier(b, c, 6.0e-4)});
        }
        {
            const double b[3] = {0.6961663, 0.4079426, 0.8974794};
            const double c[3] = {0.0046791, 0.0135121, 97.9340};
            v.push_back({QStringLiteral("Fused Silica"),
                         QStringLiteral("Synthetic SiO2. n_d 1.4585, Abbe 67.8. "
                                        "Transmits far into the UV and takes heat."),
                         sellmeier(b, c, 5.0e-5)});
        }
        {
            const double b[3] = {0.99654, 0.18964, 0.00411};
            const double c[3] = {0.00787, 0.02191, 3.85727};
            // Acrylic loses roughly 8 % over 10 mm at the edge of the visible;
            // 2e-4 /mm is the mid-band figure a moulded part is specified at.
            v.push_back({QStringLiteral("PMMA (acrylic)"),
                         QStringLiteral("Cast acrylic. n_d 1.4906, Abbe 58.0. "
                                        "The commodity moulded optic."),
                         sellmeier(b, c, 3.0e-4)});
        }
        {
            const double b[3] = {1.4182, 0.0, 0.0};
            const double c[3] = {0.021304, 1.0, 1.0};
            v.push_back({QStringLiteral("Polycarbonate"),
                         QStringLiteral("n_d 1.5848, Abbe 27.9. Tough and dispersive: "
                                        "a lens that survives being dropped and shows colour."),
                         sellmeier(b, c, 1.2e-3)});
        }
        {
            const double b[3] = {0.5675888, 0.1719923, 0.02058984};
            const double c[3] = {0.00505203, 0.0184887, 0.0111886};
            v.push_back({QStringLiteral("Water"),
                         QStringLiteral("n_d 1.3338. The immersion fluid a microscope "
                                        "objective is designed against."),
                         sellmeier(b, c, 2.0e-5)});
        }
        v.push_back({QStringLiteral("Optical Cement"),
                     QStringLiteral("n_d 1.56, non-dispersive. The layer between the two "
                                    "elements of a cemented doublet."),
                     constant(1.56, 1.0e-4)});

        // --- metals, n and k at five points across the visible ---------------
        v.push_back({QStringLiteral("Aluminium"),
                     QStringLiteral("Front-surface Al, Johnson & Christy n and k. 92 % at normal "
                                    "incidence, dipping to 87 % near 80 degrees and climbing to 1 at grazing."),
                     metal({400.0, 500.0, 600.0, 700.0, 800.0},
                           {0.490, 0.769, 1.200, 1.830, 2.750},
                           {4.860, 6.080, 7.260, 8.310, 8.310})});
        v.push_back({QStringLiteral("Silver"),
                     QStringLiteral("The best visible reflector there is: 96 % at 550 nm on "
                                    "Johnson & Christy data, and the one that tarnishes."),
                     metal({400.0, 500.0, 600.0, 700.0, 800.0},
                           {0.173, 0.129, 0.124, 0.140, 0.144},
                           {1.950, 2.920, 3.730, 4.520, 5.290})});
        v.push_back({QStringLiteral("Gold"),
                     QStringLiteral("72 % at 550 nm and over 95 % past 700 nm. "
                                    "The infrared mirror, and the reason gold looks gold."),
                     metal({400.0, 500.0, 600.0, 700.0, 800.0},
                           {1.658, 0.916, 0.247, 0.131, 0.156},
                           {1.956, 1.840, 2.980, 3.840, 4.740})});

        return v;
    }();
    return c;
}

} // namespace

bool OpticalMaterial::isMetal() const {
    if (!set || model != Model::Table) return false;
    for (int i = 0; i < samples; ++i)
        if (kSample[i] > 0.0) return true;
    return false;
}

double OpticalMaterial::indexAt(double lambda) const {
    if (!set) return nd;
    switch (model) {
    case Model::Constant:
        return nd;
    case Model::Cauchy: {
        if (cauchyB == 0.0 || lambda <= 0.0) return nd;
        const double lu = lambda * 1e-3, ld = kLambdaD * 1e-3;
        return nd + cauchyB * (1.0 / (lu * lu) - 1.0 / (ld * ld));
    }
    case Model::Sellmeier: {
        if (lambda <= 0.0) return nd;
        const double l2 = (lambda * 1e-3) * (lambda * 1e-3);   // um^2
        double n2 = 1.0;
        for (int i = 0; i < 3; ++i) {
            const double denom = l2 - sell[3 + i];
            if (sell[i] == 0.0 || std::fabs(denom) < 1e-12) continue;
            n2 += sell[i] * l2 / denom;
        }
        return n2 > 0.0 ? std::sqrt(n2) : nd;
    }
    case Model::Table:
        return samples > 0 ? interpolate(lambdaNm, nSample, samples, lambda) : nd;
    }
    return nd;
}

double OpticalMaterial::extinctionAt(double lambda) const {
    if (!set || samples <= 0) return 0.0;
    return interpolate(lambdaNm, kSample, samples, lambda);
}

double OpticalMaterial::abbe() const {
    const double nD = indexAt(kLambdaD);
    const double nF = indexAt(kLambdaF);
    const double nC = indexAt(kLambdaC);
    const double d  = nF - nC;
    return std::fabs(d) < 1e-12 ? 0.0 : (nD - 1.0) / d;
}

double metalReflectance(double n1, double n2, double k2, double cosI) {
    if (n1 <= 0.0) n1 = 1.0;
    cosI = std::clamp(std::fabs(cosI), 0.0, 1.0);
    if (k2 <= 0.0) {
        // No absorption: the ordinary dielectric result, so a "metal" with zero
        // extinction is not a special case that behaves differently.
        const double eta   = n1 / n2;
        const double sinT2 = eta * eta * (1.0 - cosI * cosI);
        if (sinT2 >= 1.0) return 1.0;
        const double cosT = std::sqrt(1.0 - sinT2);
        const double rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
        const double rp = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
        return 0.5 * (rs * rs + rp * rp);
    }

    // Relative complex index, and the auxiliary p, q of the absorbing-medium
    // form: they are the real and imaginary parts of n2t * cos(theta_t).
    const double n = n2 / n1;
    const double k = k2 / n1;
    const double s2 = 1.0 - cosI * cosI;                 // sin^2 theta
    const double a  = n * n - k * k - s2;
    const double r  = std::sqrt(a * a + 4.0 * n * n * k * k);
    const double p  = std::sqrt(std::max(0.0, 0.5 * (r + a)));
    const double q  = std::sqrt(std::max(0.0, 0.5 * (r - a)));

    const double den_s = (cosI + p) * (cosI + p) + q * q;
    if (den_s <= 0.0) return 1.0;
    const double Rs = ((cosI - p) * (cosI - p) + q * q) / den_s;

    // At exactly normal incidence sin/cos is zero and Rp == Rs, which the
    // general expression reproduces only in the limit.
    if (cosI >= 1.0 - 1e-15) return Rs;
    const double sTan = s2 / cosI;                       // sin theta * tan theta
    const double den_p = (p + sTan) * (p + sTan) + q * q;
    if (den_p <= 0.0) return 1.0;
    const double Rp = Rs * (((p - sTan) * (p - sTan) + q * q) / den_p);

    return std::clamp(0.5 * (Rs + Rp), 0.0, 1.0);
}

namespace materials {

int count() { return int(catalogue().size()); }

const QString& name(int i) {
    static const QString empty;
    const auto& c = catalogue();
    return (i >= 0 && i < int(c.size())) ? c[std::size_t(i)].name : empty;
}

const QString& description(int i) {
    static const QString empty;
    const auto& c = catalogue();
    return (i >= 0 && i < int(c.size())) ? c[std::size_t(i)].description : empty;
}

OpticalMaterial at(int i) {
    const auto& c = catalogue();
    if (i < 0 || i >= int(c.size())) return OpticalMaterial{};
    return c[std::size_t(i)].material;
}

int indexOf(const QString& n) {
    const auto& c = catalogue();
    for (std::size_t i = 0; i < c.size(); ++i)
        if (c[i].name.compare(n, Qt::CaseInsensitive) == 0) return int(i);
    return -1;
}

OpticalMaterial byName(const QString& n) {
    const int i = indexOf(n);
    return i < 0 ? OpticalMaterial{} : at(i);
}

} // namespace materials
