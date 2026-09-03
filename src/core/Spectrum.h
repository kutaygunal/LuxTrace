// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QString>
#include <cstdint>
#include <vector>

// The spectral power distribution of a source, and everything downstream of it:
// which wavelength a ray carries, how much a lumen-metre cares about that
// wavelength, and what colour it renders as.
//
// Three fixed RGB lines could not weight a source's actual emission, could not
// produce a spectrum out of a prism, and could not answer "how many lumens".
// One wavelength sampled per ray from a real SPD costs exactly what a
// monochromatic trace costs, and resolves the spectrum to whatever the ray
// budget supports.
namespace spectrum {

// CIE 1931 photopic luminous efficiency V(lambda). Zero outside 380..780 nm.
double photopic(double lambdaNm);

// Linear sRGB weights for a monochromatic stimulus, normalised to sum to one
// and clamped at zero. This is what turns the three band grids from a
// three-sample approximation into a colour rendering of the real spectrum.
void srgbWeights(double lambdaNm, double w[3]);

// CIE 1931 XYZ colour-matching functions, for the chromaticity of a spot.
void cie1931(double lambdaNm, double& x, double& y, double& z);

// CIE 1931 (x, y) -> correlated colour temperature, McCamy's cubic.
double cctFromXy(double x, double y);

} // namespace spectrum

// How a source's wavelengths are chosen.
struct SpectrumConfig {
    enum class Kind : int {
        Monochromatic = 0,   // one line: a laser, or an index quoted at the d line
        Monochrome    = 0,   // the older spelling, kept so saved configs load
        Rgb           = 1,   // three fixed lines, stratified one in three
        Blackbody     = 2,   // Planck at `cct`, an incandescent lamp or the sun
        LedPhosphor   = 3,   // blue pump plus a broad phosphor hump, at `cct`
        D65           = 4,   // CIE standard daylight
        Table         = 5    // a measured SPD, from a CSV
    };
    static constexpr int kKindCount = 6;

    SpectrumConfig() = default;
    // Implicit, so `cfg.spectrum = Kind::Rgb` still reads as an assignment of a
    // spectrum rather than of a configuration object.
    SpectrumConfig(Kind k) : kind(k) {}

    bool operator==(Kind k) const { return kind == k; }
    bool operator!=(Kind k) const { return kind != k; }

    Kind   kind         = Kind::Monochromatic;
    double wavelengthNm = 587.6;    // Monochromatic
    double cct          = 5000.0;   // Blackbody, LedPhosphor
    // Band the continuous kinds are sampled over.
    double minNm = 380.0, maxNm = 780.0;

    // Measured SPD, in ascending wavelength. Kind::Table only.
    std::vector<double> tableNm;
    std::vector<double> tablePower;

    static QString kindName(Kind k);
    static Kind    kindFromName(const QString& s, bool* ok = nullptr);
};

// A SpectrumConfig resolved into something a ray can sample in a few flops.
//
// The SPD is turned into a piecewise-linear inverse CDF once per run, so every
// ray draws one uniform, reads one wavelength, and carries weight 1: the
// spectrum is importance sampled rather than stratified into bands, which is
// what keeps a spectral trace the same price as a monochromatic one.
class SampledSpectrum {
public:
    static constexpr int kBins = 256;

    void build(const SpectrumConfig& cfg);

    // Wavelength for a ray. `index` is the global ray index, used only by the
    // stratified RGB kind; `u` is the uniform draw the other kinds consume.
    double sample(std::size_t index, double u) const;

    // Luminous efficacy of the distribution, lm/W: 683 * <V(lambda)>. This is
    // the number that turns watts into lumens, and it is a property of the
    // source's spectrum alone.
    double efficacy() const { return m_efficacy; }

    // Photometric weight of one ray at `lambda`, normalised so the mean weight
    // over the distribution is exactly one. A run in lumens gives every ray this
    // weight at emission, which makes every downstream quantity photometric
    // without a second grid anywhere.
    double photometricWeight(double lambda) const;

    // Which of the three colour bands a ray at `lambda` paints into, as weights
    // that sum to one. For the RGB kind this is a hard one-of-three, so the
    // legacy behaviour is reproduced exactly.
    void bandWeights(double lambda, double w[3]) const;

    bool monochromatic() const { return m_mono; }
    bool rgbBands()      const { return m_rgb; }
    // The band the RGB kind assigns ray `index` to, or -1.
    int  rgbBandOf(std::size_t index) const;

    double meanWavelength() const { return m_meanLambda; }

private:
    double m_invCdf[kBins + 1] = {};
    double m_efficacy    = 0.0;
    double m_meanV       = 0.0;
    double m_meanLambda  = 587.6;
    bool   m_mono        = true;
    bool   m_rgb         = false;
    double m_monoLambda  = 587.6;
};
