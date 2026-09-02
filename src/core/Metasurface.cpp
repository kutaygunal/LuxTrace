#include "Metasurface.h"

#include "Optics.h"

namespace meta {

using optics::kTwoPi;

const char* profileName(Profile p) {
    switch (p) {
    case Profile::None:     return "None";
    case Profile::Linear:   return "Linear gradient (grating)";
    case Profile::Radial:   return "Radial gradient (axicon)";
    case Profile::Metalens: return "Metalens";
    }
    return "None";
}

const char* faceName(Face f) {
    switch (f) {
    case Face::Entering: return "Entering face";
    case Face::Leaving:  return "Leaving face";
    case Face::Both:     return "Both faces";
    }
    return "Leaving face";
}

void Efficiency::at(int m, double lambda, double& es, double& ep) const {
    es = 0.0;
    ep = 0.0;
    if (m < -kMaxOrder || m > kMaxOrder) return;
    const int slot = m + kMaxOrder;

    if (samples <= 0) {
        es = flat[slot];
        ep = polarising ? flatP[slot] : flat[slot];
        return;
    }
    // Linear in wavelength between samples, flat outside the measured band.
    // Extrapolating a diffraction efficiency past the wavelengths it was
    // computed at would be inventing data an EM solve did not produce, and the
    // flat end is the honest reading of "we do not know beyond here".
    const std::size_t n = std::size_t(samples);
    auto readAt = [&](const std::vector<double>& v, std::size_t k) {
        const std::size_t i = k * std::size_t(kOrderSlots) + std::size_t(slot);
        return i < v.size() ? v[i] : 0.0;
    };
    auto pick = [&](const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        if (lambda <= lambdaNm.front()) return readAt(v, 0);
        if (lambda >= lambdaNm[n - 1])  return readAt(v, n - 1);
        std::size_t k = 0;
        while (k + 1 < n && lambdaNm[k + 1] < lambda) ++k;
        const double span = lambdaNm[k + 1] - lambdaNm[k];
        if (span <= 0.0) return readAt(v, k);
        const double f = (lambda - lambdaNm[k]) / span;
        return readAt(v, k) * (1.0 - f) + readAt(v, k + 1) * f;
    };
    es = pick(value);
    ep = polarising && !valueP.empty() ? pick(valueP) : es;
}

double Efficiency::total(double lambda) const {
    double sum = 0.0;
    for (int m = -kMaxOrder; m <= kMaxOrder; ++m) {
        double es = 0.0, ep = 0.0;
        at(m, lambda, es, ep);
        sum += 0.5 * (es + ep);
    }
    return sum;
}

void Metasurface::setIdealOrder(int m, double eff) {
    efficiency = Efficiency{};
    if (m < -kMaxOrder || m > kMaxOrder) return;
    efficiency.flat[m + kMaxOrder]  = std::clamp(eff, 0.0, 1.0);
    efficiency.flatP[m + kMaxOrder] = std::clamp(eff, 0.0, 1.0);
}

Vec3 Metasurface::gradientAt(const Vec3& p, const Vec3& n) const {
    if (profile == Profile::None) return Vec3();

    if (profile == Profile::Linear) {
        // Projected into the surface, so a gradient direction given roughly can
        // still be used exactly: what the physics needs is the tangential part
        // and nothing else.
        Vec3 g = axis - n * axis.dot(n);
        if (!g.normalize()) return Vec3();
        return g * gradientPerMm;
    }

    Vec3 r = p - centre;
    r = r - n * r.dot(n);                       // in-plane offset from the centre
    const double rho = r.length();
    if (rho < 1e-12) return Vec3();             // on the axis: no gradient at all
    const Vec3 rhat = r * (1.0 / rho);

    if (profile == Profile::Radial) return rhat * gradientPerMm;

    // Metalens. Phi(r) = -(2 pi / lambda_d)(sqrt(r^2 + f^2) - f), so the
    // gradient is -(2 pi / lambda_d) r / sqrt(r^2 + f^2) along r. The design
    // wavelength enters here and only here, which is what makes the focal
    // length scale as lambda_d / lambda everywhere else.
    const double f = focalLengthMm;
    if (std::fabs(f) < 1e-12 || designLambdaNm <= 0.0) return Vec3();
    const double lambdaMm = designLambdaNm * 1e-6;
    const double hyp = std::sqrt(rho * rho + f * f);
    const double dphi = -(kTwoPi / lambdaMm) * rho / hyp * (f >= 0.0 ? 1.0 : -1.0);
    return rhat * dphi;
}

bool deflect(const Vec3& d, const Vec3& n, const Vec3& grad, double lambdaNm,
             double n1, double n2, int m, bool reflective, Vec3& out) {
    if (lambdaNm <= 0.0) return false;
    const double lambdaMm = lambdaNm * 1e-6;

    // The incident direction split about the surface. `n` faces the ray, so
    // cosI is positive and the tangential part is what is left.
    const double cosI = -d.dot(n);
    const Vec3   dTan = d + n * cosI;

    // The tangential wavevector, in units of the free-space one. The whole of
    // the new physics is the second term: crossing the surface adds the
    // designed phase gradient, times the order, times lambda / 2 pi.
    const Vec3 kTan = dTan * n1 + grad * (double(m) * lambdaMm / kTwoPi);

    const double nOut = reflective ? n1 : n2;
    if (nOut <= 0.0) return false;
    const double sinT = kTan.length() / nOut;
    // Past one, the surface is asking for a tangential wavevector the outgoing
    // medium cannot carry. The order is evanescent -- it does not propagate and
    // it does not exist at this angle. The caller books what it was carrying.
    if (!(sinT < 1.0)) return false;

    Vec3 tangential = kTan * (1.0 / nOut);
    const double cosT = std::sqrt(std::max(0.0, 1.0 - sinT * sinT));
    // Transmission continues through the surface, away from the face the ray
    // arrived at; reflection comes back off it.
    Vec3 v = reflective ? tangential + n * cosT : tangential - n * cosT;
    if (!v.normalize()) return false;
    out = v;
    return true;
}

} // namespace meta
