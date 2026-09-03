// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "core/EnvironmentMap.h"

#include <QFile>
#include <QObject>

#include <algorithm>
#include <cmath>

namespace envmap {

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

// Where a direction lands in the map, as continuous texel coordinates.
//
// Row 0 sits at the +up pole and row h-1 at the -up pole, which is the
// convention every equirectangular environment ships in. The azimuth runs the
// same way round e1 -> e2 for the lookup and for the sampler, and the reason to
// say so is that getting them opposite produces a picture that is merely
// mirrored -- perfectly plausible, and wrong in a way no energy check catches.
void toTexel(const Vec3& d, const Vec3& up, const Vec3& e1, const Vec3& e2,
             int w, int h, double& fx, double& fy, double& sinTheta) {
    const double c = std::clamp(d.dot(up), -1.0, 1.0);
    const double theta = std::acos(c);
    sinTheta = std::sqrt(std::max(0.0, 1.0 - c * c));
    double phi = std::atan2(d.dot(e2), d.dot(e1));
    if (phi < 0.0) phi += kTwoPi;
    fx = phi / kTwoPi * double(w);
    fy = theta / kPi * double(h);
}

Vec3 fromTexel(double fx, double fy, const Vec3& up, const Vec3& e1, const Vec3& e2,
               int w, int h, double& sinTheta) {
    const double phi   = fx / double(w) * kTwoPi;
    const double theta = fy / double(h) * kPi;
    sinTheta = std::sin(theta);
    const double c = std::cos(theta);
    return (up * c + e1 * (sinTheta * std::cos(phi)) + e2 * (sinTheta * std::sin(phi)))
        .normalized();
}

// The first index whose prefix sum is at or above `u`. A linear scan would be
// fine for one lookup and is not for one per bounce per pixel.
int upperIndex(const double* cdf, int n, double u) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (cdf[mid] < u) lo = mid + 1;
        else              hi = mid;
    }
    return lo;
}

} // namespace

void EnvironmentMap::build(int w, int h, std::vector<double> luminance) {
    m_w = m_h = 0;
    m_lum.clear();
    m_margCdf.clear();
    m_condCdf.clear();
    m_rowMass.clear();
    m_total = m_mean = 0.0;
    if (w <= 0 || h <= 0 || luminance.size() != std::size_t(w) * std::size_t(h)) return;

    m_w = w;
    m_h = h;
    m_lum = std::move(luminance);
    for (double& v : m_lum)
        if (!(v > 0.0) || !std::isfinite(v)) v = 0.0;

    m_condCdf.assign(std::size_t(w) * std::size_t(h), 0.0);
    m_margCdf.assign(std::size_t(h), 0.0);
    m_rowMass.assign(std::size_t(h), 0.0);

    // The CDF is built from a *dilated* copy of the map -- each texel weighted
    // by the brightest of itself and its eight neighbours -- and not from the
    // map as stored.
    //
    // The lookup is bilinear, so a texel of zero luminance beside a bright one
    // still reads bright over half its width. Weighting the CDF by the stored
    // value gives that half-texel no probability at all, and it is then
    // reached only by the uniform share above: a thin ring around every hard
    // edge, carrying real light, drawn a hundred times too rarely and weighted
    // a hundred times too heavily. The estimator stays unbiased and the
    // variance is ruinous -- a small bright window read ten per cent high over
    // half a million samples, off four of them. Dilating by one texel makes
    // the support the sampler covers the same support the lookup has.
    const auto dilated = [&](int x, int y) {
        double best = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
            const int yy = std::clamp(y + dy, 0, h - 1);
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = ((x + dx) % w + w) % w;
                best = std::max(best, m_lum[std::size_t(yy) * std::size_t(w) +
                                            std::size_t(xx)]);
            }
        }
        return best;
    };

    // Weighted by sin(theta), because a texel near a pole covers less sky than
    // one at the equator and a sampler that ignored that would crowd the poles.
    double total = 0.0;
    for (int y = 0; y < h; ++y) {
        const double theta = (double(y) + 0.5) / double(h) * kPi;
        const double sy    = std::sin(theta);
        double row = 0.0;
        for (int x = 0; x < w; ++x) {
            row += dilated(x, y) * sy;
            m_condCdf[std::size_t(y) * std::size_t(w) + std::size_t(x)] = row;
        }
        m_rowMass[std::size_t(y)] = row;
        total += row;
        m_margCdf[std::size_t(y)] = total;
    }
    m_total = total;

    if (total > 0.0) {
        for (int y = 0; y < h; ++y) {
            const double row = m_rowMass[std::size_t(y)];
            if (row > 0.0)
                for (int x = 0; x < w; ++x)
                    m_condCdf[std::size_t(y) * std::size_t(w) + std::size_t(x)] /= row;
            else
                // A row with no light still needs a valid CDF: the uniform share
                // can land in it, and a CDF of zeros would put every such draw
                // in the last column.
                for (int x = 0; x < w; ++x)
                    m_condCdf[std::size_t(y) * std::size_t(w) + std::size_t(x)] =
                        double(x + 1) / double(w);
            m_margCdf[std::size_t(y)] /= total;
            m_rowMass[std::size_t(y)] = row / total;
        }
        // The mean over the sphere: sum(L sin) / sum(sin), which is the
        // uniform surround of the same total power. From the map as stored,
        // never from the dilated copy the CDF is built on -- the dilation is a
        // sampling device, and a map must not get brighter for having been
        // sampled.
        double weight = 0.0, lit = 0.0;
        for (int y = 0; y < h; ++y) {
            const double sy = std::sin((double(y) + 0.5) / double(h) * kPi);
            weight += sy * double(w);
            for (int x = 0; x < w; ++x)
                lit += m_lum[std::size_t(y) * std::size_t(w) + std::size_t(x)] * sy;
        }
        m_mean = weight > 0.0 ? lit / weight : 0.0;
    } else {
        for (int y = 0; y < h; ++y) {
            m_margCdf[std::size_t(y)] = double(y + 1) / double(h);
            m_rowMass[std::size_t(y)] = 1.0 / double(h);
            for (int x = 0; x < w; ++x)
                m_condCdf[std::size_t(y) * std::size_t(w) + std::size_t(x)] =
                    double(x + 1) / double(w);
        }
    }
}

double EnvironmentMap::along(const Vec3& d, const Vec3& up, const Vec3& e1,
                             const Vec3& e2) const {
    if (empty()) return 0.0;
    double fx = 0.0, fy = 0.0, sinTheta = 0.0;
    toTexel(d, up, e1, e2, m_w, m_h, fx, fy, sinTheta);

    // Bilinear about texel centres, wrapping in azimuth and clamping in
    // elevation -- azimuth is periodic and elevation is not, and treating them
    // the same way puts a seam across the top of every map.
    const double gx = fx - 0.5;
    const double gy = std::clamp(fy - 0.5, 0.0, double(m_h) - 1.0);
    int x0 = int(std::floor(gx));
    const double ax = gx - double(x0);
    int y0 = int(std::floor(gy));
    const double ay = gy - double(y0);
    y0 = std::clamp(y0, 0, m_h - 1);
    const int y1 = std::min(m_h - 1, y0 + 1);
    const auto wrap = [&](int x) { return ((x % m_w) + m_w) % m_w; };
    const int xa = wrap(x0), xb = wrap(x0 + 1);

    const double l00 = m_lum[std::size_t(y0) * std::size_t(m_w) + std::size_t(xa)];
    const double l10 = m_lum[std::size_t(y0) * std::size_t(m_w) + std::size_t(xb)];
    const double l01 = m_lum[std::size_t(y1) * std::size_t(m_w) + std::size_t(xa)];
    const double l11 = m_lum[std::size_t(y1) * std::size_t(m_w) + std::size_t(xb)];
    const double top = l00 * (1.0 - ax) + l10 * ax;
    const double bot = l01 * (1.0 - ax) + l11 * ax;
    return top * (1.0 - ay) + bot * ay;
}

bool EnvironmentMap::sample(double u1, double u2, const Vec3& up, const Vec3& e1,
                            const Vec3& e2, Vec3& dir, double& pdf) const {
    if (empty()) return false;

    double fx = 0.0, fy = 0.0;
    if (m_total > 0.0 && u1 >= kUniformShare) {
        // Rescaled so the luminance-proportional branch still spans [0,1): a
        // draw reused without rescaling would never reach the first texels.
        const double u = (u1 - kUniformShare) / (1.0 - kUniformShare);
        const int y = upperIndex(m_margCdf.data(), m_h, u);
        const double yLo = y > 0 ? m_margCdf[std::size_t(y) - 1] : 0.0;
        const double yHi = m_margCdf[std::size_t(y)];
        const double ay  = yHi > yLo ? (u - yLo) / (yHi - yLo) : 0.5;

        const double* row = m_condCdf.data() + std::size_t(y) * std::size_t(m_w);
        const int x = upperIndex(row, m_w, u2);
        const double xLo = x > 0 ? row[x - 1] : 0.0;
        const double xHi = row[x];
        const double ax  = xHi > xLo ? (u2 - xLo) / (xHi - xLo) : 0.5;

        fx = double(x) + ax;
        fy = double(y) + ay;
    } else {
        // The uniform share, drawn uniformly in solid angle rather than
        // uniformly over the map: an even spread over (u, v) would crowd the
        // poles exactly as the unweighted CDF would.
        const double u = m_total > 0.0 ? u1 / kUniformShare : u1;
        const double cosT = 1.0 - 2.0 * std::clamp(u, 0.0, 1.0);
        fy = std::acos(std::clamp(cosT, -1.0, 1.0)) / kPi * double(m_h);
        fx = std::clamp(u2, 0.0, 1.0) * double(m_w);
    }

    double sinTheta = 0.0;
    dir = fromTexel(fx, fy, up, e1, e2, m_w, m_h, sinTheta);
    pdf = pdfAlong(dir, up, e1, e2);
    return pdf > 0.0;
}

double EnvironmentMap::pdfAlong(const Vec3& d, const Vec3& up, const Vec3& e1,
                                const Vec3& e2) const {
    if (empty()) return 0.0;
    double fx = 0.0, fy = 0.0, sinTheta = 0.0;
    toTexel(d, up, e1, e2, m_w, m_h, fx, fy, sinTheta);
    if (sinTheta <= 1e-9) return 0.0;   // the poles are a set of measure zero

    const int x = std::clamp(int(fx), 0, m_w - 1);
    const int y = std::clamp(int(fy), 0, m_h - 1);

    // The probability of this texel under each branch, then the two mixed.
    // Both are converted to a density per steradian by the same Jacobian:
    // one texel covers (2 pi / w) * (pi / h) * sin(theta) steradians.
    const double texelSolid = kTwoPi / double(m_w) * kPi / double(m_h) * sinTheta;
    if (texelSolid <= 0.0) return 0.0;

    double pMap = 0.0;
    if (m_total > 0.0) {
        const double rowP = m_rowMass[std::size_t(y)];
        const double* row = m_condCdf.data() + std::size_t(y) * std::size_t(m_w);
        const double colP = row[x] - (x > 0 ? row[x - 1] : 0.0);
        pMap = rowP * colP;
    }
    // Uniform in solid angle: this texel's share of the sphere.
    const double pUni = texelSolid / (4.0 * kPi);

    const double mix = m_total > 0.0
                           ? (1.0 - kUniformShare) * pMap + kUniformShare * pUni
                           : pUni;
    return mix / texelSolid;
}

// ------------------------------------------------------------- Radiance RGBE

namespace {

// RGBE unpacks to a shared exponent. Zero exponent is exactly black rather than
// a very small number, which is what lets a file store a night sky without
// denormals.
void rgbeToLinear(const std::uint8_t* p, double& r, double& g, double& b) {
    if (p[3] == 0) { r = g = b = 0.0; return; }
    const double f = std::ldexp(1.0, int(p[3]) - (128 + 8));
    r = (double(p[0]) + 0.5) * f;
    g = (double(p[1]) + 0.5) * f;
    b = (double(p[2]) + 0.5) * f;
}

bool readScanline(const std::vector<std::uint8_t>& in, std::size_t& at, int width,
                  std::vector<std::uint8_t>& out, QString* err) {
    out.assign(std::size_t(width) * 4, 0);
    const auto fail = [&](const char* why) {
        if (err) *err = QObject::tr("the environment file ends mid-scanline (%1)")
                            .arg(QString::fromLatin1(why));
        return false;
    };
    if (at + 4 > in.size()) return fail("no header");

    // The adaptive RLE format: a four-byte marker, then four separately
    // run-length-encoded channel planes.
    if (in[at] == 2 && in[at + 1] == 2 && !(in[at + 2] & 0x80) &&
        ((int(in[at + 2]) << 8) | int(in[at + 3])) == width && width >= 8 &&
        width <= 0x7fff) {
        at += 4;
        for (int ch = 0; ch < 4; ++ch) {
            int x = 0;
            while (x < width) {
                if (at >= in.size()) return fail("truncated run");
                int count = int(in[at++]);
                if (count > 128) {
                    // A run of one repeated value.
                    count -= 128;
                    if (at >= in.size() || x + count > width) return fail("bad run");
                    const std::uint8_t v = in[at++];
                    for (int k = 0; k < count; ++k)
                        out[std::size_t(x + k) * 4 + std::size_t(ch)] = v;
                } else {
                    if (count == 0 || at + std::size_t(count) > in.size() ||
                        x + count > width)
                        return fail("bad literal");
                    for (int k = 0; k < count; ++k)
                        out[std::size_t(x + k) * 4 + std::size_t(ch)] = in[at++];
                }
                x += count;
            }
        }
        return true;
    }

    // The flat format, and the older run-length one that signals a repeat with
    // (1,1,1,n). Both are still written by older tools, so both are read.
    int x = 0;
    int shift = 0;
    while (x < width) {
        if (at + 4 > in.size()) return fail("truncated pixels");
        const std::uint8_t* p = &in[at];
        if (p[0] == 1 && p[1] == 1 && p[2] == 1) {
            if (x == 0) return fail("a repeat with nothing to repeat");
            const int count = int(p[3]) << shift;
            for (int k = 0; k < count && x < width; ++k, ++x)
                for (int c = 0; c < 4; ++c)
                    out[std::size_t(x) * 4 + std::size_t(c)] =
                        out[std::size_t(x - 1) * 4 + std::size_t(c)];
            shift += 8;
            at += 4;
            continue;
        }
        shift = 0;
        for (int c = 0; c < 4; ++c) out[std::size_t(x) * 4 + std::size_t(c)] = p[c];
        at += 4;
        ++x;
    }
    return true;
}

} // namespace

bool parseRadiance(const std::vector<std::uint8_t>& bytes, double scaleToNits,
                   EnvironmentMap& out, QString* err) {
    const auto fail = [&](const QString& why) {
        if (err) *err = why;
        return false;
    };
    if (bytes.size() < 16) return fail(QObject::tr("the environment file is empty"));

    std::size_t at = 0;
    const auto line = [&]() {
        std::string s;
        while (at < bytes.size() && bytes[at] != '\n') s.push_back(char(bytes[at++]));
        if (at < bytes.size()) ++at;
        return s;
    };

    const std::string magic = line();
    if (magic.rfind("#?", 0) != 0)
        return fail(QObject::tr("not a Radiance file: it does not begin with #?"));

    bool rgbe = false;
    for (;;) {
        if (at >= bytes.size())
            return fail(QObject::tr("the environment file has no resolution line"));
        const std::string h = line();
        if (h.empty()) break;
        if (h.rfind("FORMAT=", 0) == 0) {
            if (h.find("32-bit_rle_rgbe") != std::string::npos ||
                h.find("32-bit_rle_xyze") != std::string::npos)
                rgbe = true;
            else
                return fail(QObject::tr("unsupported Radiance format: %1")
                                .arg(QString::fromStdString(h.substr(7))));
        }
    }
    if (!rgbe)
        return fail(QObject::tr("the environment file declares no 32-bit_rle_rgbe format"));

    const std::string res = line();
    int w = 0, h = 0;
    // Only the standard orientation is read. A flipped one would load and be
    // upside down, and an environment that is silently upside down is worse
    // than one that is refused.
    if (std::sscanf(res.c_str(), "-Y %d +X %d", &h, &w) != 2 || w <= 0 || h <= 0)
        return fail(QObject::tr("unsupported Radiance scanline order: %1")
                        .arg(QString::fromStdString(res)));

    std::vector<double> lum(std::size_t(w) * std::size_t(h), 0.0);
    std::vector<std::uint8_t> row;
    for (int y = 0; y < h; ++y) {
        if (!readScanline(bytes, at, w, row, err)) return false;
        for (int x = 0; x < w; ++x) {
            double r = 0.0, g = 0.0, b = 0.0;
            rgbeToLinear(&row[std::size_t(x) * 4], r, g, b);
            // Rec. 709 luminance, the same coefficients every other sRGB
            // conversion in this codebase uses.
            lum[std::size_t(y) * std::size_t(w) + std::size_t(x)] =
                std::max(0.0, scaleToNits * (0.2126 * r + 0.7152 * g + 0.0722 * b));
        }
    }
    out.build(w, h, std::move(lum));
    return true;
}

bool loadRadiance(const QString& path, double scaleToNits, EnvironmentMap& out,
                  QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QObject::tr("cannot open %1: %2").arg(path, f.errorString());
        return false;
    }
    const QByteArray raw = f.readAll();
    const std::vector<std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(raw.constData()),
        reinterpret_cast<const std::uint8_t*>(raw.constData()) + raw.size());
    return parseRadiance(bytes, scaleToNits, out, err);
}

} // namespace envmap
