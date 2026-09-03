// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "Polarisation.h"

#include <algorithm>
#include <cmath>

namespace polarisation {

Mueller Mueller::fromFresnel(double rs, double rp, double phaseDelta) {
    // Intensity reflectances, and the two cross terms that carry the phase.
    const double Rs = rs * rs;
    const double Rp = rp * rp;
    const double c  = rs * rp * std::cos(phaseDelta);
    const double s  = rs * rp * std::sin(phaseDelta);

    Mueller r;
    r.m[0][0] = 0.5 * (Rs + Rp);
    r.m[0][1] = 0.5 * (Rs - Rp);
    r.m[1][0] = 0.5 * (Rs - Rp);
    r.m[1][1] = 0.5 * (Rs + Rp);
    r.m[2][2] =  c;
    r.m[2][3] =  s;
    r.m[3][2] = -s;
    r.m[3][3] =  c;
    return r;
}

Mueller Mueller::polariser(double angle) {
    const double c = std::cos(2.0 * angle);
    const double s = std::sin(2.0 * angle);
    Mueller r;
    r.m[0][0] = 0.5;     r.m[0][1] = 0.5 * c;     r.m[0][2] = 0.5 * s;
    r.m[1][0] = 0.5 * c; r.m[1][1] = 0.5 * c * c; r.m[1][2] = 0.5 * c * s;
    r.m[2][0] = 0.5 * s; r.m[2][1] = 0.5 * c * s; r.m[2][2] = 0.5 * s * s;
    return r;
}

Mueller Mueller::retarder(double angle, double retardance) {
    const double c2 = std::cos(2.0 * angle), s2 = std::sin(2.0 * angle);
    const double cd = std::cos(retardance),  sd = std::sin(retardance);
    Mueller r;
    r.m[0][0] = 1.0;
    r.m[1][1] = c2 * c2 + s2 * s2 * cd;
    r.m[1][2] = c2 * s2 * (1.0 - cd);
    r.m[1][3] = -s2 * sd;
    r.m[2][1] = c2 * s2 * (1.0 - cd);
    r.m[2][2] = c2 * c2 * cd + s2 * s2;
    r.m[2][3] = c2 * sd;
    r.m[3][1] = s2 * sd;
    r.m[3][2] = -c2 * sd;
    r.m[3][3] = cd;
    return r;
}

Mueller Mueller::rotation(double angle) {
    const double c = std::cos(2.0 * angle);
    const double s = std::sin(2.0 * angle);
    Mueller r;
    r.m[0][0] = 1.0;
    r.m[1][1] =  c; r.m[1][2] = s;
    r.m[2][1] = -s; r.m[2][2] = c;
    r.m[3][3] = 1.0;
    return r;
}

void fresnelAmplitudes(double n1, double n2, double cosI,
                       double& rs, double& rp, double& phaseDelta) {
    cosI = std::clamp(std::fabs(cosI), 0.0, 1.0);
    const double eta   = n1 / n2;
    const double sinT2 = eta * eta * (1.0 - cosI * cosI);

    if (sinT2 >= 1.0) {
        // Total internal reflection. Both amplitudes have unit magnitude, and
        // what is left is the phase between them -- which is the whole reason a
        // Fresnel rhomb turns linear light into circular, and which an
        // unpolarised model has no way to express.
        rs = 1.0;
        rp = 1.0;
        const double sinI = std::sqrt(std::max(0.0, 1.0 - cosI * cosI));
        const double root = std::sqrt(std::max(0.0, sinI * sinI - (n2 / n1) * (n2 / n1)));
        const double ds = -2.0 * std::atan2(root, cosI);
        const double dp = -2.0 * std::atan2((n1 / n2) * (n1 / n2) * root, cosI);
        phaseDelta = dp - ds;
        return;
    }

    const double cosT = std::sqrt(1.0 - sinT2);
    const double as = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    const double ap = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
    rs = std::fabs(as);
    rp = std::fabs(ap);
    // Below the critical angle the coefficients are real, so the only phase is
    // whichever of them changed sign. That sign flip through Brewster's angle is
    // what makes a stack of plates a polariser.
    const double ps = (as < 0.0) ? 3.14159265358979323846 : 0.0;
    const double pp = (ap < 0.0) ? 3.14159265358979323846 : 0.0;
    phaseDelta = pp - ps;
}

double frameRotation(const Vec3& d, const Vec3& nOld, const Vec3& nNew) {
    // The s axis of each frame is perpendicular to that surface's plane of
    // incidence: d x n, normalised. The rotation is the angle between them.
    Vec3 sOld = d.cross(nOld);
    Vec3 sNew = d.cross(nNew);
    if (!sOld.normalize() || !sNew.normalize()) return 0.0;
    const double c = std::clamp(sOld.dot(sNew), -1.0, 1.0);
    const double s = sOld.cross(sNew).dot(d.normalized());
    return std::atan2(s, c);
}

} // namespace polarisation
