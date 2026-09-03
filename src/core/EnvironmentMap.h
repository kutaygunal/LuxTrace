// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once

// An environment as an emitter: a latitude-longitude map of luminance that
// surrounds the scene and lights it.
//
// The camera already has a uniform surround, and for most of this library that
// is the honest room to put an optic in. What a uniform surround cannot do is
// be *directional*: a window on one side, a sky and a ground, a sun. Those are
// what a real appearance measurement is made under, and they are also the cases
// where a reflective optic stops being a silhouette and starts showing what it
// is shaped like.
//
// Two things are needed to make one usable rather than merely present:
//
//   the lookup, so an escaping ray reads the map along its own direction, and
//
//   the *sampling*, so a scattering vertex can connect to the map analytically
//   in proportion to how bright it is. Without that, a map with a sun in it is
//   almost all noise: the sun is a thousandth of the sphere carrying most of the
//   light, and a cosine-weighted draw finds it once in a few thousand tries.
//
// The map is stored in cd/m^2. A Radiance .hdr file carries relative values, so
// loading one takes a calibration scale rather than pretending the file's own
// numbers are photometric.

#include <QString>
#include <cstdint>
#include <vector>

#include "core/Vec3.h"

namespace envmap {

// The share of the sampling distribution spent uniformly over the sphere rather
// than in proportion to the map.
//
// Not a tuning knob: it is what keeps the estimator unbiased. The map is read
// bilinearly, so a direction inside a dark texel can still pick up a corner of
// a bright neighbour -- and a purely luminance-proportional distribution gives a
// texel of zero luminance a probability of exactly zero. A direction that can
// contribute but can never be drawn is a dark bias, and it is invisible in any
// single image. One per cent of the draws spread uniformly makes every
// direction reachable at a cost of one per cent of the sampling efficiency.
inline constexpr double kUniformShare = 0.01;

class EnvironmentMap {
public:
    // Rebuilds from a luminance image, row 0 at the +up pole, wrapping in
    // azimuth. `w * h` entries, in cd/m^2. Any non-finite or negative entry is
    // clamped to zero rather than being allowed to poison the CDF.
    void build(int w, int h, std::vector<double> luminance);

    bool empty() const { return m_w <= 0 || m_h <= 0 || m_lum.empty(); }
    int  width()  const { return m_w; }
    int  height() const { return m_h; }

    // Mean luminance over the sphere, weighted by solid angle. What a uniform
    // surround of the same total power would read.
    double meanLuminance() const { return m_mean; }

    // The luminance along `d`, bilinear in azimuth and elevation. `up`, `e1`
    // and `e2` are the frame the map is oriented in; the caller holds it so it
    // is built once per run rather than once per lookup.
    double along(const Vec3& d, const Vec3& up, const Vec3& e1, const Vec3& e2) const;

    // Draws a direction in proportion to luminance times solid angle, and
    // returns the pdf per steradian. False when the map carries no light at
    // all, which is the one case where there is nothing to connect to.
    bool sample(double u1, double u2, const Vec3& up, const Vec3& e1, const Vec3& e2,
                Vec3& dir, double& pdf) const;

    // The pdf `sample` would have returned for `d`. Kept so a test can assert
    // the two agree -- a sampler and its own density drifting apart is the
    // failure that shows up as a plausible picture with the wrong brightness.
    double pdfAlong(const Vec3& d, const Vec3& up, const Vec3& e1, const Vec3& e2) const;

private:
    int m_w = 0, m_h = 0;
    std::vector<double> m_lum;        // w * h, cd/m^2
    // Marginal over rows and conditional over columns, both inclusive prefix
    // sums normalised to end at one. `m_condCdf` is h blocks of w.
    std::vector<double> m_margCdf;    // h
    std::vector<double> m_condCdf;    // h * w
    std::vector<double> m_rowMass;    // h, the marginal probability of each row
    double m_total = 0.0;             // sum of luminance * sin(theta)
    double m_mean  = 0.0;
};

// Loads a Radiance RGBE (.hdr / .pic) file and converts it to luminance with
// `scaleToNits`, since the file's own numbers are relative. Returns false with
// `err` filled -- by name, never silently -- on anything it cannot read.
bool loadRadiance(const QString& path, double scaleToNits, EnvironmentMap& out,
                  QString* err);

// The same, from bytes already in memory. What the loader above is written in
// terms of, and what a test can drive without touching a disk.
bool parseRadiance(const std::vector<std::uint8_t>& bytes, double scaleToNits,
                   EnvironmentMap& out, QString* err);

} // namespace envmap
