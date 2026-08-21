#include "Palette.h"

#include <algorithm>
#include <cmath>

namespace palette {
namespace {

struct Stop { double t, r, g, b; };

// Eight-stop approximations. Sampling the real 256-entry tables would be more
// faithful, but at 64 x 64 bins the difference is invisible and this keeps the
// maps as data rather than as a blob.
const Stop kViridis[] = {
    {0.00, 0.267, 0.005, 0.329}, {0.14, 0.283, 0.141, 0.458},
    {0.29, 0.254, 0.265, 0.530}, {0.43, 0.207, 0.372, 0.553},
    {0.57, 0.164, 0.471, 0.558}, {0.71, 0.128, 0.567, 0.551},
    {0.86, 0.267, 0.749, 0.441}, {1.00, 0.993, 0.906, 0.144},
};
const Stop kInferno[] = {
    {0.00, 0.001, 0.000, 0.014}, {0.14, 0.113, 0.045, 0.226},
    {0.29, 0.298, 0.059, 0.388}, {0.43, 0.475, 0.107, 0.383},
    {0.57, 0.657, 0.176, 0.310}, {0.71, 0.823, 0.290, 0.196},
    {0.86, 0.955, 0.508, 0.055}, {1.00, 0.988, 0.998, 0.645},
};
const Stop kJet[] = {
    {0.00, 0.000, 0.000, 0.560}, {0.13, 0.000, 0.000, 1.000},
    {0.38, 0.000, 1.000, 1.000}, {0.50, 0.200, 1.000, 0.800},
    {0.62, 1.000, 1.000, 0.000}, {0.87, 1.000, 0.000, 0.000},
    {1.00, 0.560, 0.000, 0.000}, {1.00, 0.560, 0.000, 0.000},
};
const Stop kGray[] = {
    {0.00, 0.000, 0.000, 0.000}, {1.00, 1.000, 1.000, 1.000},
    {1.00, 1.000, 1.000, 1.000}, {1.00, 1.000, 1.000, 1.000},
    {1.00, 1.000, 1.000, 1.000}, {1.00, 1.000, 1.000, 1.000},
    {1.00, 1.000, 1.000, 1.000}, {1.00, 1.000, 1.000, 1.000},
};
const Stop kTurbo[] = {
    {0.00, 0.190, 0.072, 0.232}, {0.14, 0.246, 0.489, 0.936},
    {0.29, 0.164, 0.774, 0.860}, {0.43, 0.267, 0.947, 0.560},
    {0.57, 0.611, 0.997, 0.245}, {0.71, 0.917, 0.848, 0.194},
    {0.86, 0.996, 0.499, 0.101}, {1.00, 0.680, 0.011, 0.005},
};

const Stop* tableFor(Map m) {
    switch (m) {
    case Map::Inferno:   return kInferno;
    case Map::Jet:       return kJet;
    case Map::Grayscale: return kGray;
    case Map::Turbo:     return kTurbo;
    case Map::Viridis:
    default:             return kViridis;
    }
}

} // namespace

QStringList names() {
    return {QStringLiteral("Viridis"), QStringLiteral("Inferno"), QStringLiteral("Jet"),
            QStringLiteral("Grayscale"), QStringLiteral("Turbo")};
}

QString name(Map m) {
    const QStringList n = names();
    const int i = int(m);
    return (i >= 0 && i < n.size()) ? n[i] : n[0];
}

QColor sample(Map m, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const Stop* s = tableFor(m);
    constexpr int n = 8;
    for (int i = 1; i < n; ++i) {
        if (t <= s[i].t || i == n - 1) {
            const double span = s[i].t - s[i - 1].t;
            const double f = span > 1e-12 ? std::clamp((t - s[i - 1].t) / span, 0.0, 1.0) : 0.0;
            return QColor::fromRgbF(s[i - 1].r + (s[i].r - s[i - 1].r) * f,
                                    s[i - 1].g + (s[i].g - s[i - 1].g) * f,
                                    s[i - 1].b + (s[i].b - s[i - 1].b) * f);
        }
    }
    return QColor::fromRgbF(s[n - 1].r, s[n - 1].g, s[n - 1].b);
}

QStringList scaleNames() {
    return {QStringLiteral("Linear"), QStringLiteral("Log (4 decades)"),
            QStringLiteral("Square root")};
}

QString scaleName(Scale s) {
    const QStringList n = scaleNames();
    const int i = int(s);
    return (i >= 0 && i < n.size()) ? n[i] : n[0];
}

double normalise(Scale s, double value, double peak) {
    if (peak <= 0.0 || value <= 0.0) return 0.0;
    const double r = std::clamp(value / peak, 0.0, 1.0);
    switch (s) {
    case Scale::Log: {
        // log10(r) runs 0 at the peak down to -inf; anything below the decade
        // floor is off the bottom of the map rather than clamped onto its
        // darkest colour, so an empty bin stays visibly empty.
        const double d = std::log10(r) / kLogDecades + 1.0;
        return std::clamp(d, 0.0, 1.0);
    }
    case Scale::Sqrt:
        return std::sqrt(r);
    case Scale::Linear:
    default:
        return r;
    }
}

} // namespace palette
