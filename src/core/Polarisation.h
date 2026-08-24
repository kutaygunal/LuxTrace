#pragma once
#include "Vec3.h"
#include <cmath>

// Polarisation, as a Stokes vector carried by each branch and a Mueller matrix
// applied at each interaction.
//
// Unpolarised Fresnel is the average of Rs and Rp, so Brewster's angle appears
// in the reflectance curve but a polariser cannot be modelled, total internal
// reflection carries no phase, and stress birefringence is out of reach.
//
// A polarised trace costs roughly four times the per-ray state, and most
// illumination work does not need it, so it sits behind a switch and the
// unpolarised fast path stays the default.
namespace polarisation {

// (I, Q, U, V) in the frame whose s axis is perpendicular to the plane of
// incidence: I is the total, Q the linear excess along s over p, U the linear
// excess at 45 degrees, V the circular part.
struct Stokes {
    double i = 1.0, q = 0.0, u = 0.0, v = 0.0;

    static Stokes unpolarised(double intensity = 1.0) { return {intensity, 0.0, 0.0, 0.0}; }
    static Stokes linearS(double intensity = 1.0)     { return {intensity, intensity, 0.0, 0.0}; }
    static Stokes linearP(double intensity = 1.0)     { return {intensity, -intensity, 0.0, 0.0}; }
    static Stokes circular(double intensity = 1.0, bool right = true) {
        return {intensity, 0.0, 0.0, right ? intensity : -intensity};
    }

    // Degree of polarisation: 0 for unpolarised light, 1 for fully polarised.
    double degree() const {
        if (i <= 1e-18) return 0.0;
        return std::min(1.0, std::sqrt(q * q + u * u + v * v) / i);
    }
    double degreeLinear() const {
        return i <= 1e-18 ? 0.0 : std::min(1.0, std::sqrt(q * q + u * u) / i);
    }
    // Angle of the linear polarisation, radians from the s axis.
    double angle() const { return 0.5 * std::atan2(u, q); }

    // A physically realisable state never has more polarised than total.
    bool valid() const {
        return i >= -1e-12 && (q * q + u * u + v * v) <= i * i * (1.0 + 1e-9);
    }

    Stokes scaled(double f) const { return {i * f, q * f, u * f, v * f}; }
};

// A 4x4 Mueller matrix.
struct Mueller {
    double m[4][4] = {};

    static Mueller identity() {
        Mueller r;
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0;
        return r;
    }

    // The reflection or transmission of an interface, from its amplitude
    // reflectances and the phase between them. This is the general form: it
    // reduces to a scale by (Rs + Rp)/2 for unpolarised light, which is exactly
    // what the unpolarised path already computed.
    static Mueller fromFresnel(double rs, double rp, double phaseDelta);

    // An ideal linear polariser whose transmission axis is `angle` radians from
    // the s axis.
    static Mueller polariser(double angle);
    // A retarder of `retardance` radians with its fast axis at `angle`.
    static Mueller retarder(double angle, double retardance);
    // Rotates the reference frame by `angle` radians. Needed because the plane
    // of incidence turns from one surface to the next, and a Stokes vector only
    // means anything relative to a frame.
    static Mueller rotation(double angle);

    Stokes apply(const Stokes& s) const {
        Stokes o;
        o.i = m[0][0] * s.i + m[0][1] * s.q + m[0][2] * s.u + m[0][3] * s.v;
        o.q = m[1][0] * s.i + m[1][1] * s.q + m[1][2] * s.u + m[1][3] * s.v;
        o.u = m[2][0] * s.i + m[2][1] * s.q + m[2][2] * s.u + m[2][3] * s.v;
        o.v = m[3][0] * s.i + m[3][1] * s.q + m[3][2] * s.u + m[3][3] * s.v;
        return o;
    }

    Mueller operator*(const Mueller& o) const {
        Mueller r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double sum = 0.0;
                for (int k = 0; k < 4; ++k) sum += m[i][k] * o.m[k][j];
                r.m[i][j] = sum;
            }
        return r;
    }
};

// Amplitude reflection coefficients at a dielectric interface, and the phase
// difference between the two polarisations. Past the critical angle both have
// unit magnitude and the phase between them is what makes total internal
// reflection a retarder -- which is how a Fresnel rhomb works, and which the
// unpolarised model cannot express at all.
void fresnelAmplitudes(double n1, double n2, double cosI,
                       double& rs, double& rp, double& phaseDelta);

// The angle the plane of incidence turns through between two surfaces, so a
// Stokes vector can be rotated into the frame the next interaction needs.
// `d` is the ray direction, `nOld` and `nNew` the two surface normals.
double frameRotation(const Vec3& d, const Vec3& nOld, const Vec3& nNew);

} // namespace polarisation
