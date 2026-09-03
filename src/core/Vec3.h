// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <cmath>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

// Minimal double-precision 3-vector used by the hot ray-tracing loops.
// OCCT's gp_Pnt/gp_Vec are fine at the API boundary, but constructing them
// inside the innermost intersection loop costs far more than plain arithmetic.
struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3() = default;
    Vec3(double X, double Y, double Z) : x(X), y(Y), z(Z) {}
    explicit Vec3(const gp_Pnt& p) : x(p.X()), y(p.Y()), z(p.Z()) {}
    explicit Vec3(const gp_Dir& d) : x(d.X()), y(d.Y()), z(d.Z()) {}
    explicit Vec3(const gp_Vec& v) : x(v.X()), y(v.Y()), z(v.Z()) {}

    double  operator[](int i) const { return (&x)[i]; }
    double& operator[](int i)       { return (&x)[i]; }

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s)      const { return {x * s, y * s, z * s}; }
    Vec3 operator-()              const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }

    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double lengthSquared() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(lengthSquared()); }

    // Returns false (leaving the vector untouched) for a degenerate vector.
    bool normalize() {
        const double l2 = lengthSquared();
        if (l2 < 1e-24) return false;
        const double inv = 1.0 / std::sqrt(l2);
        x *= inv; y *= inv; z *= inv;
        return true;
    }
    Vec3 normalized() const { Vec3 v = *this; v.normalize(); return v; }

    gp_Pnt toPnt() const { return gp_Pnt(x, y, z); }
};
