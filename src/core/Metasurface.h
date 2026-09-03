// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

#include "Vec3.h"

// A surface that does not obey Snell's law.
//
// Every other optical surface in this application deflects light by refraction
// or reflection, and both follow from one boundary condition: the tangential
// component of the wavevector is continuous. A metasurface breaks that
// deliberately. It is a dense array of sub-wavelength scatterers whose
// individual phase responses are designed, so crossing it adds a *position
// dependent* phase Phi(x) to the wavefront, and the tangential wavevector picks
// up the gradient of that phase:
//
//     n_t sin(theta_t) - n_i sin(theta_i) = (lambda / 2 pi) dPhi/dx
//
// which is the generalised Snell law of Yu and Capasso. The ordinary law is the
// case dPhi/dx == 0, and that is not a limit or an approximation -- it is the
// same expression with the new term switched off, which is why the first thing
// the tests assert is that a metasurface with no gradient traces bit-identically
// to the glass it replaced.
//
// Two consequences are worth stating because they are what a metasurface is
// *for*, and both fall out of the equation rather than being modelled on top of
// it:
//
//   The deflection is proportional to wavelength. A refractive lens bends blue
//   light more than red because n falls with wavelength; a phase-gradient lens
//   bends *red* more than blue, because lambda is in the numerator. Its chromatic
//   dispersion is therefore reversed and about an order of magnitude larger,
//   which is the whole difficulty of designing an achromatic one and the first
//   thing a simulation has to get the sign of.
//
//   The gradient is periodic in phase, so it supports diffraction orders. Order
//   m sees m times the gradient. A real device puts most of its light in one
//   order and the rest into the others and into nothing at all, and the
//   efficiency table is where that is stated rather than assumed.
//
// This is a surface property, not a new tracer: the geometry, the media
// bookkeeping and the energy budget are the ones already there. It sits behind
// `active()` so a scene without one pays a null check per hit.
namespace meta {

// How the designed phase varies across the surface.
enum class Profile : int {
    None = 0,
    // A constant gradient along `axis`: a blazed grating, and the case the
    // generalised Snell law is usually written for.
    Linear,
    // A constant gradient in the radial direction from `centre`: an axicon.
    Radial,
    // The phase of a lens of focal length f at the design wavelength,
    //   Phi(r) = -(2 pi / lambda_d) (sqrt(r^2 + f^2) - f),
    // whose gradient is -(2 pi / lambda_d) r / sqrt(r^2 + f^2). At the design
    // wavelength that aims every ray at the focus exactly; away from it the
    // focus moves as lambda_d / lambda, which is the reversed dispersion above.
    Metalens,
};

const char* profileName(Profile p);

// The largest diffraction order carried, either way. Five orders each side is
// past anything a designed metasurface puts measurable light into, and the
// table is small enough to sit in the surface by value rather than behind a
// pointer -- which is what keeps a hit from touching a second cache line.
inline constexpr int kMaxOrder = 5;
inline constexpr int kOrderSlots = 2 * kMaxOrder + 1;

// Diffraction efficiency against wavelength, one curve per order and
// polarisation.
//
// Meta-optic data comes out of an EM solver as complex transmission
// coefficients per order, and what a ray tracer needs from them is the modulus
// squared -- so this is what an import reduces to. A flat efficiency (one
// number per order, no wavelength dependence) is the common vendor summary and
// is the `samples == 0` case; anything richer is the table.
struct Efficiency {
    // Order m is at index m + kMaxOrder.
    double flat[kOrderSlots] = {};
    // s and p separately, so a polarising metasurface is representable. Equal
    // by default, which is the unpolarised device.
    double flatP[kOrderSlots] = {};
    bool   polarising = false;

    // Wavelength samples, ascending, and the efficiency of each order at each.
    // `value[k * kOrderSlots + slot]` is order `slot - kMaxOrder` at
    // `lambdaNm[k]`.
    std::vector<double> lambdaNm;
    std::vector<double> value;
    std::vector<double> valueP;
    int samples = 0;

    // Efficiency of order `m` at `lambda`, for s and p.
    void at(int m, double lambda, double& es, double& ep) const;

    // Everything the orders take between them at this wavelength. What is left
    // is absorbed by the surface, and saying so is the difference between a
    // budget that closes and one that is quietly topped up.
    double total(double lambda) const;
};

// Which face of a solid carries the pattern.
//
// A metasurface is a film on one face of a wafer, not a property of the
// glass behind it. A solid has two faces and a surface property belongs to
// all of them, so without this a metalens deflects on the way in and again
// on the way out and focuses at half the length it was designed for -- which
// is exactly what the first version of this did, and exactly what a focus
// sweep found.
//
// `Leaving` is the default because that is how a transmissive metalens is
// built and used: light crosses the substrate and meets the pattern on the
// far side. `Both` is a real device too -- a metasurface doublet is patterned
// on both faces of one wafer -- and is available rather than assumed.
enum class Face : int { Entering = 0, Leaving, Both };

const char* faceName(Face f);

// The metasurface itself.
struct Metasurface {
    Profile profile = Profile::None;
    Face    face    = Face::Leaving;

    // The wavelength the phase profile was designed at, nanometres. Every
    // deflection scales as lambda / designLambdaNm.
    double designLambdaNm = 550.0;

    // Linear and Radial: the phase gradient, radians per millimetre, at the
    // design wavelength. Positive deflects towards +axis.
    double gradientPerMm = 0.0;
    // The in-plane direction a Linear gradient runs along. Projected into the
    // surface at the hit, so it does not have to be exactly tangent.
    Vec3 axis{1.0, 0.0, 0.0};

    // Metalens: the design focal length in millimetres, positive converging.
    double focalLengthMm = 0.0;
    // Centre of a Radial or Metalens profile, in world millimetres.
    Vec3 centre{0.0, 0.0, 0.0};

    // Whether the gradient acts on the transmitted branch or the reflected one.
    // A metalens is transmissive; a reflectarray is not.
    bool reflective = false;

    Efficiency efficiency;

    bool active() const { return profile != Profile::None; }
    // Whether this crossing meets the pattern. `leaving` is the tracer's own
    // determination of which side of the interface the ray is on.
    bool patternsThisCrossing(bool leaving) const {
        if (face == Face::Both) return true;
        return leaving == (face == Face::Leaving);
    }

    // The in-plane phase gradient at `p`, in radians per millimetre, as a vector
    // lying in the surface whose normal is `n`.
    //
    // Returned as a vector rather than a scalar because the generalised Snell
    // law is a statement about the tangential wavevector, and a radial profile
    // has a different tangential direction at every point on the surface.
    Vec3 gradientAt(const Vec3& p, const Vec3& n) const;

    // Sets an ideal blazed device: everything in one order, nothing anywhere
    // else. The starting point a design is written against, and the one whose
    // energy budget is trivially checkable.
    void setIdealOrder(int m, double eff = 1.0);
};

// The outgoing direction for order `m`, by the generalised Snell law.
//
//   n_t sin(theta_t) = n_i sin(theta_i) + m (lambda / 2 pi) |dPhi/dx|
//
// applied to the tangential component of the incident direction, with the
// normal component recovered from the unit length of the result. `n` faces the
// incident ray, `d` is the incident direction, `grad` is the in-plane gradient
// in radians per millimetre and `lambdaNm` the wavelength.
//
// Returns false when the order is evanescent -- the tangential wavevector the
// grating asks for is larger than the outgoing medium can carry, so there is no
// propagating direction and the order simply does not exist at this angle. That
// is not an error and not a loss to hide: the caller books what the missing
// orders were carrying.
bool deflect(const Vec3& d, const Vec3& n, const Vec3& grad, double lambdaNm,
             double n1, double n2, int m, bool reflective, Vec3& out);

} // namespace meta
