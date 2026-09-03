// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "Coating.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace coating {

namespace {

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

// How a coating's reflectance rises away from normal incidence.
//
// A real single- or multi-layer stack is designed at normal incidence and gets
// worse as the angle opens, roughly as the bare-interface Fresnel curve does
// relative to its own normal-incidence value. Scaling the specified residual by
// that ratio gives the right shape without pretending to know the stack.
double angularRolloff(double bare, double bareNormal) {
    if (bareNormal <= 1e-12) return 1.0;
    return std::max(1.0, bare / bareNormal);
}

double bareAtNormal(double n1, double n2) {
    const double r = (n1 - n2) / (n1 + n2);
    return r * r;
}

// Characteristic-matrix solve for a stack, per polarisation. Born & Wolf 1.6.
// Complex arithmetic because a layer's phase thickness is complex the moment
// the angle passes total internal reflection inside it.
double stackReflectance(const Coating& c, double n1, double n2, double cosI,
                        double lambdaNm, bool sPol) {
    using cplx = std::complex<double>;
    if (c.layers <= 0 || lambdaNm <= 0.0) return -1.0;

    const double sinI = std::sqrt(std::max(0.0, 1.0 - cosI * cosI));
    // Snell's invariant: n sin(theta) is the same in every layer.
    const double inv = n1 * sinI;

    auto tilt = [&](double n) {
        const cplx s = cplx(inv / n, 0.0);
        return std::sqrt(cplx(1.0, 0.0) - s * s);      // cos(theta) in that layer
    };
    auto eta = [&](double n, const cplx& ct) {
        return sPol ? cplx(n, 0.0) * ct : cplx(n, 0.0) / ct;
    };

    const cplx ct0 = cplx(cosI, 0.0);
    const cplx eta0 = eta(n1, ct0);
    const cplx ctS = tilt(n2);
    const cplx etaS = eta(n2, ctS);

    // The stack's characteristic matrix, outermost layer first.
    cplx m11(1.0, 0.0), m12(0.0, 0.0), m21(0.0, 0.0), m22(1.0, 0.0);
    for (int i = 0; i < c.layers; ++i) {
        const double n = c.layerIndex[i];
        if (!(n > 0.0)) continue;
        const cplx ct = tilt(n);
        const cplx e  = eta(n, ct);
        const cplx delta = cplx(2.0 * 3.14159265358979323846 * n * c.layerThicknessNm[i] /
                                    lambdaNm, 0.0) * ct;
        const cplx cd = std::cos(delta);
        const cplx sd = std::sin(delta);
        const cplx a11 = cd;
        const cplx a12 = cplx(0.0, 1.0) * sd / e;
        const cplx a21 = cplx(0.0, 1.0) * e * sd;
        const cplx a22 = cd;

        const cplx n11 = m11 * a11 + m12 * a21;
        const cplx n12 = m11 * a12 + m12 * a22;
        const cplx n21 = m21 * a11 + m22 * a21;
        const cplx n22 = m21 * a12 + m22 * a22;
        m11 = n11; m12 = n12; m21 = n21; m22 = n22;
    }

    const cplx b = m11 + m12 * etaS;
    const cplx d = m21 + m22 * etaS;
    const cplx denom = eta0 * b + d;
    if (std::abs(denom) < 1e-18) return -1.0;
    const cplx r = (eta0 * b - d) / denom;
    return std::clamp(std::norm(r), 0.0, 1.0);
}

struct Entry {
    QString  name;
    QString  description;
    Coating  coating;
};

Coating idealAr(double residual) {
    Coating c;
    c.model = Model::Ideal;
    c.residual = residual;
    return c;
}

Coating idealHr(double reflect) {
    Coating c;
    c.model = Model::Ideal;
    c.residual = reflect;
    c.highReflector = true;
    return c;
}

const std::vector<Entry>& catalogue() {
    static const std::vector<Entry> c = [] {
        std::vector<Entry> v;
        v.push_back({QStringLiteral("None"),
                     QStringLiteral("Bare glass. Roughly 4 % per surface at normal incidence, "
                                    "which over six surfaces is a fifth of the light."),
                     Coating{}});
        v.push_back({QStringLiteral("MgF2 single layer"),
                     QStringLiteral("The commodity coating: about 1.3 % residual at 550 nm, "
                                    "and the cheapest thing that helps."),
                     idealAr(0.013)});
        v.push_back({QStringLiteral("Broadband AR"),
                     QStringLiteral("A multi-layer stack under 0.5 % across the visible. What "
                                    "a catalogue lens is sold with."),
                     idealAr(0.004)});
        v.push_back({QStringLiteral("V-coat (laser line)"),
                     QStringLiteral("Optimised at one wavelength, under 0.25 % there and worse "
                                    "either side of it."),
                     idealAr(0.0025)});
        v.push_back({QStringLiteral("Dielectric HR"),
                     QStringLiteral("A high reflector: over 99 %, and better than any metal, "
                                    "over the band it was designed for."),
                     idealHr(0.995)});
        {
            // A real quarter-wave MgF2 layer on crown glass, solved rather than
            // asserted -- so the stack model can be checked against the ideal
            // one it is meant to supersede.
            Coating q;
            q.model = Model::Stack;
            q.layers = 1;
            q.layerIndex[0] = 1.38;                 // MgF2
            q.layerThicknessNm[0] = 550.0 / (4.0 * 1.38);
            v.push_back({QStringLiteral("Quarter-wave MgF2 (solved)"),
                         QStringLiteral("The same coating as a characteristic-matrix stack "
                                        "rather than a specified residual: a quarter wave of "
                                        "MgF2 at 550 nm, with its wavelength and angle "
                                        "dependence coming out of the solve."),
                         q});
        }
        return v;
    }();
    return c;
}

} // namespace

QString modelName(Model m) {
    switch (m) {
    case Model::None:  return QStringLiteral("Uncoated");
    case Model::Ideal: return QStringLiteral("Ideal");
    case Model::Table: return QStringLiteral("Measured R(lambda)");
    case Model::Stack: return QStringLiteral("Thin-film stack");
    }
    return QStringLiteral("Uncoated");
}

QString modelTip(Model m) {
    switch (m) {
    case Model::None:
        return QStringLiteral("Bare Fresnel: about 4 % per air-glass surface.");
    case Model::Ideal:
        return QStringLiteral("A specified residual reflectance at normal incidence, with the "
                              "angular roll-off a real stack has. Closes most of the gap "
                              "between a bare surface and a coated one.");
    case Model::Table:
        return QStringLiteral("Measured reflectance against wavelength, used as it stands.");
    case Model::Stack:
        return QStringLiteral("A characteristic-matrix solve over the layers, for designing a "
                              "coating rather than applying one. Its wavelength and angle "
                              "dependence come out of the physics.");
    }
    return QString();
}

double Coating::reflectance_(double n1, double n2, double cosI, double lambda,
                             double bare) const {
    double rs = 0.0, rp = 0.0;
    reflectanceSP(n1, n2, cosI, lambda, bare, bare, rs, rp);
    return 0.5 * (rs + rp);
}

double tableReflectanceAt(const Coating& c, double lambdaNm) {
    if (c.samples <= 0) return c.residual;
    return interpolate(c.lambdaNm, c.reflectance, c.samples, lambdaNm);
}

void Coating::reflectanceSP(double n1, double n2, double cosI, double lambda,
                            double bareS, double bareP, double& rs, double& rp) const {
    rs = bareS;
    rp = bareP;
    if (model == Model::None) return;

    // Past the critical angle the interface reflects everything whatever is on
    // it: a coating cannot make light leave a medium it cannot leave.
    if (bareS >= 1.0 - 1e-12 && bareP >= 1.0 - 1e-12) return;

    if (model == Model::Stack) {
        const double s = stackReflectance(*this, n1, n2, cosI, lambda, /*sPol=*/true);
        const double p = stackReflectance(*this, n1, n2, cosI, lambda, /*sPol=*/false);
        if (s >= 0.0 && p >= 0.0) { rs = s; rp = p; return; }
        // A stack that could not be solved falls through to the ideal model
        // rather than silently reporting the bare surface.
    }

    double target = residual;
    if (model == Model::Table && samples > 0)
        target = interpolate(lambdaNm, reflectance, samples, lambda);

    const double normal = bareAtNormal(n1, n2);
    const double roll   = angularRolloff(0.5 * (bareS + bareP), normal);

    if (highReflector) {
        // A high reflector loses a little at angle rather than gaining.
        const double r = std::clamp(1.0 - (1.0 - target) * roll, 0.0, 1.0);
        rs = std::max(bareS, r);
        rp = std::max(bareP, r);
        return;
    }
    // An anti-reflection coating: the residual, growing with angle the way the
    // bare interface does, and never worse than bare glass.
    rs = std::clamp(std::min(bareS, target * roll), 0.0, 1.0);
    rp = std::clamp(std::min(bareP, target * roll), 0.0, 1.0);
}

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

Coating at(int i) {
    const auto& c = catalogue();
    if (i < 0 || i >= int(c.size())) return Coating{};
    return c[std::size_t(i)].coating;
}

Coating byName(const QString& n) {
    const auto& c = catalogue();
    for (const auto& e : c)
        if (e.name.compare(n, Qt::CaseInsensitive) == 0) return e.coating;
    return Coating{};
}

} // namespace coating
