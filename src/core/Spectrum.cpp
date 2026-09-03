// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "Spectrum.h"
#include "Optics.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Multi-lobe Gaussian fits to the CIE 1931 colour-matching functions
// (Wyman, Sloan & Shirley, JCGT 2013). Accurate to about 1 % of the peak and
// two lines each, which is the right trade for a function called once per ray.
double gauss(double x, double mu, double s1, double s2) {
    const double t = (x - mu) * (x < mu ? 1.0 / s1 : 1.0 / s2);
    return std::exp(-0.5 * t * t);
}

} // namespace

namespace spectrum {

void cie1931(double l, double& X, double& Y, double& Z) {
    X = 1.056 * gauss(l, 599.8, 37.9, 31.0)
      + 0.362 * gauss(l, 442.0, 16.0, 26.7)
      - 0.065 * gauss(l, 501.1, 20.4, 26.2);
    Y = 0.821 * gauss(l, 568.8, 46.9, 40.5)
      + 0.286 * gauss(l, 530.9, 16.3, 31.1);
    Z = 1.217 * gauss(l, 437.0, 11.8, 36.0)
      + 0.681 * gauss(l, 459.0, 26.0, 13.8);
    X = std::max(0.0, X);
    Y = std::max(0.0, Y);
    Z = std::max(0.0, Z);
}

double photopic(double l) {
    if (l < 360.0 || l > 830.0) return 0.0;
    // V(lambda) is the Y colour-matching function by definition.
    double X, Y, Z;
    cie1931(l, X, Y, Z);
    return Y;
}

void srgbWeights(double l, double w[3]) {
    double X, Y, Z;
    cie1931(l, X, Y, Z);
    // XYZ -> linear sRGB (IEC 61966-2-1).
    double r =  3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    double b =  0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    // A monochromatic stimulus sits outside the sRGB gamut, so one channel comes
    // out negative. Clamping and renormalising is what keeps the three band
    // grids summing to the irradiance grid -- the invariant every band-wise
    // reading of the result depends on.
    r = std::max(0.0, r); g = std::max(0.0, g); b = std::max(0.0, b);
    const double sum = r + g + b;
    if (sum <= 1e-12) {
        // Outside the visible band: paint it into whichever end it fell off.
        w[0] = (l > 600.0) ? 1.0 : 0.0;
        w[1] = 0.0;
        w[2] = (l > 600.0) ? 0.0 : 1.0;
        return;
    }
    w[0] = r / sum; w[1] = g / sum; w[2] = b / sum;
}

double cctFromXy(double x, double y) {
    const double d = y - 0.1858;
    if (std::fabs(x - 0.3320) < 1e-9 && std::fabs(d) < 1e-9) return 0.0;
    const double n = (x - 0.3320) / (0.1858 - y);
    return 449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33;
}

} // namespace spectrum

QString SpectrumConfig::kindName(Kind k) {
    switch (k) {
    case Kind::Monochromatic: return QStringLiteral("Monochromatic");
    case Kind::Rgb:           return QStringLiteral("RGB (3 bands)");
    case Kind::Blackbody:     return QStringLiteral("Blackbody");
    case Kind::LedPhosphor:   return QStringLiteral("White LED");
    case Kind::D65:           return QStringLiteral("CIE D65");
    case Kind::Table:         return QStringLiteral("Measured SPD");
    }
    return QStringLiteral("Monochromatic");
}

SpectrumConfig::Kind SpectrumConfig::kindFromName(const QString& s, bool* ok) {
    for (int i = 0; i <= int(Kind::Table); ++i) {
        if (kindName(Kind(i)).compare(s, Qt::CaseInsensitive) == 0) {
            if (ok) *ok = true;
            return Kind(i);
        }
    }
    if (ok) *ok = false;
    return Kind::Monochromatic;
}

namespace {

// Planck's law, in arbitrary units -- only the shape matters, since the SPD is
// normalised to a probability density.
double planck(double lambdaNm, double T) {
    if (T <= 0.0) return 0.0;
    const double l = lambdaNm * 1e-9;
    const double c1 = 3.741771852e-16;      // 2 h c^2
    const double c2 = 1.438776877e-2;       // h c / k
    const double e  = std::exp(c2 / (l * T)) - 1.0;
    if (e <= 0.0) return 0.0;
    return c1 / (std::pow(l, 5.0) * e);
}

// A phosphor-converted white LED: a narrow InGaN pump near 450 nm plus a broad
// Ce:YAG hump whose centre and weight move with the target colour temperature.
// Not a substitute for a manufacturer's measured SPD, but the right shape --
// and the right shape is what makes colour-over-angle mean anything.
double ledPhosphor(double l, double cct) {
    const double t     = std::clamp((cct - 2700.0) / (6500.0 - 2700.0), 0.0, 1.0);
    const double pumpW = 0.20 + 0.45 * t;                 // cooler == more blue
    const double phosC = 600.0 - 45.0 * t;                // warmer == redder hump
    const double pump  = std::exp(-0.5 * std::pow((l - 452.0) / 14.0, 2.0));
    const double phos  = std::exp(-0.5 * std::pow((l - phosC) / 62.0, 2.0));
    return pumpW * pump + (1.0 - pumpW) * phos;
}

// CIE D65, as its published 20 nm table over 380..780 nm.
double d65(double l) {
    static const double s[] = {
        49.98, 54.65, 82.75, 91.49, 93.43, 86.68, 104.87, 117.01, 117.81, 114.86,
        115.92, 108.81, 109.35, 107.80, 104.79, 107.69, 104.41, 104.05, 100.00,
        96.33, 95.79, 88.69, 90.01, 89.60, 87.70, 83.29, 83.70, 80.03, 80.21,
        82.28, 78.28, 69.72, 71.61, 74.35, 61.60, 69.89, 75.09, 63.59, 46.42,
        66.81, 63.38
    };
    constexpr int n = int(sizeof(s) / sizeof(s[0]));   // 380..780 in 10 nm steps
    if (l <= 380.0) return s[0];
    if (l >= 380.0 + 10.0 * (n - 1)) return s[n - 1];
    const double f = (l - 380.0) / 10.0;
    const int    i = int(f);
    const double a = f - double(i);
    return s[i] * (1.0 - a) + s[i + 1] * a;
}

} // namespace

void SampledSpectrum::build(const SpectrumConfig& cfg) {
    *this = SampledSpectrum{};

    if (cfg.kind == SpectrumConfig::Kind::Monochromatic) {
        m_mono       = true;
        m_monoLambda = cfg.wavelengthNm;
        m_meanLambda = cfg.wavelengthNm;
        m_meanV      = spectrum::photopic(cfg.wavelengthNm);
        m_efficacy   = 683.0 * m_meanV;
        return;
    }
    if (cfg.kind == SpectrumConfig::Kind::Rgb) {
        m_mono = false;
        m_rgb  = true;
        double v = 0.0, l = 0.0;
        for (int i = 0; i < 3; ++i) {
            v += spectrum::photopic(optics::kBandNm[i]);
            l += optics::kBandNm[i];
        }
        m_meanV      = v / 3.0;
        m_meanLambda = l / 3.0;
        m_efficacy   = 683.0 * m_meanV;
        return;
    }

    m_mono = false;
    const double lo = std::max(200.0, std::min(cfg.minNm, cfg.maxNm - 1.0));
    const double hi = std::max(lo + 1.0, cfg.maxNm);

    // Sample the SPD densely, integrate it, then invert the CDF onto a uniform
    // grid. Inverting once means the per-ray cost is one lookup and one lerp.
    constexpr int kDense = 2048;
    std::vector<double> lam(kDense), pdf(kDense);
    for (int i = 0; i < kDense; ++i) {
        const double l = lo + (hi - lo) * double(i) / double(kDense - 1);
        lam[std::size_t(i)] = l;
        double p = 0.0;
        switch (cfg.kind) {
        case SpectrumConfig::Kind::Blackbody:   p = planck(l, cfg.cct); break;
        case SpectrumConfig::Kind::LedPhosphor: p = ledPhosphor(l, cfg.cct); break;
        case SpectrumConfig::Kind::D65:         p = d65(l); break;
        case SpectrumConfig::Kind::Table: {
            const auto& xs = cfg.tableNm;
            const auto& ys = cfg.tablePower;
            if (xs.size() < 2 || xs.size() != ys.size()) { p = 1.0; break; }
            if (l <= xs.front()) { p = ys.front(); break; }
            if (l >= xs.back())  { p = ys.back();  break; }
            const auto it = std::upper_bound(xs.begin(), xs.end(), l);
            const std::size_t j = std::size_t(it - xs.begin());
            const double span = xs[j] - xs[j - 1];
            const double f = span > 0.0 ? (l - xs[j - 1]) / span : 0.0;
            p = ys[j - 1] * (1.0 - f) + ys[j] * f;
            break;
        }
        default: p = 1.0; break;
        }
        pdf[std::size_t(i)] = std::max(0.0, p);
    }

    std::vector<double> cdf(std::size_t(kDense), 0.0);
    double total = 0.0;
    for (int i = 1; i < kDense; ++i) {
        const double dl = lam[std::size_t(i)] - lam[std::size_t(i - 1)];
        total += 0.5 * (pdf[std::size_t(i)] + pdf[std::size_t(i - 1)]) * dl;
        cdf[std::size_t(i)] = total;
    }
    if (total <= 0.0) {
        // A degenerate SPD (all zero, or a table that is not one) falls back to
        // the middle of the band rather than producing no light at all.
        m_mono       = true;
        m_monoLambda = 0.5 * (lo + hi);
        m_meanLambda = m_monoLambda;
        m_meanV      = spectrum::photopic(m_monoLambda);
        m_efficacy   = 683.0 * m_meanV;
        return;
    }
    for (auto& c : cdf) c /= total;

    int j = 0;
    for (int b = 0; b <= kBins; ++b) {
        const double target = double(b) / double(kBins);
        while (j + 1 < kDense - 1 && cdf[std::size_t(j + 1)] < target) ++j;
        const double c0 = cdf[std::size_t(j)], c1 = cdf[std::size_t(j + 1)];
        const double f  = (c1 > c0) ? (target - c0) / (c1 - c0) : 0.0;
        m_invCdf[b] = lam[std::size_t(j)] +
                      (lam[std::size_t(j + 1)] - lam[std::size_t(j)]) * std::clamp(f, 0.0, 1.0);
    }

    // <V> and <lambda> over the distribution, from the same quadrature.
    double sv = 0.0, sl = 0.0;
    for (int i = 1; i < kDense; ++i) {
        const double dl = lam[std::size_t(i)] - lam[std::size_t(i - 1)];
        const double w0 = pdf[std::size_t(i - 1)] * dl * 0.5;
        const double w1 = pdf[std::size_t(i)] * dl * 0.5;
        sv += w0 * spectrum::photopic(lam[std::size_t(i - 1)]) +
              w1 * spectrum::photopic(lam[std::size_t(i)]);
        sl += w0 * lam[std::size_t(i - 1)] + w1 * lam[std::size_t(i)];
    }
    m_meanV      = sv / total;
    m_meanLambda = sl / total;
    m_efficacy   = 683.0 * m_meanV;
}

double SampledSpectrum::sample(std::size_t index, double u) const {
    if (m_mono) return m_monoLambda;
    if (m_rgb)  return optics::kBandNm[index % 3];
    const double f = std::clamp(u, 0.0, 1.0) * double(kBins);
    const int    b = std::min(kBins - 1, int(f));
    const double a = f - double(b);
    return m_invCdf[b] * (1.0 - a) + m_invCdf[b + 1] * a;
}

int SampledSpectrum::rgbBandOf(std::size_t index) const {
    return m_rgb ? int(index % 3) : -1;
}

double SampledSpectrum::photometricWeight(double lambda) const {
    if (m_meanV <= 1e-12) return 1.0;
    return spectrum::photopic(lambda) / m_meanV;
}

void SampledSpectrum::bandWeights(double lambda, double w[3]) const {
    if (m_rgb) {
        // Exactly one band, as the three-line model always did.
        int b = 1;
        double best = 1e18;
        for (int i = 0; i < 3; ++i) {
            const double d = std::fabs(lambda - optics::kBandNm[i]);
            if (d < best) { best = d; b = i; }
        }
        w[0] = w[1] = w[2] = 0.0;
        w[b] = 1.0;
        return;
    }
    spectrum::srgbWeights(lambda, w);
}
