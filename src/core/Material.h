#pragma once
#include <QString>
#include <vector>

// The optical material of a medium or a mirror coating: how its refractive
// index moves with wavelength, how much light it absorbs per millimetre, and --
// for a metal -- its extinction coefficient.
//
// Nobody specifying an optic works in Cauchy coefficients; they work in
// catalogue names. So the catalogue below is the interface, and the struct is
// what a name resolves to.
//
// It is a fixed-size POD on purpose. A material is copied into OpticalSurface,
// then into MeshSurface, then into SceneSurface, and read on the hot path from
// several threads at once; a value that carries no allocation and no shared
// registry cannot go stale behind any of them.
struct OpticalMaterial {
    // How n(lambda) is evaluated.
    //   Constant   n is the same at every wavelength
    //   Cauchy     n = A + B / lambda^2, a visible-band fit
    //   Sellmeier  n^2 - 1 = sum Bi lambda^2 / (lambda^2 - Ci), the form every
    //              glass catalogue publishes
    //   Table      linear interpolation of measured n (and k) samples
    enum class Model : int { Constant = 0, Cauchy, Sellmeier, Table };

    // Enough for a metal across the visible band, and for a polymer fit. A
    // longer table belongs in a file the app imports and resamples onto this.
    static constexpr int kMaxSamples = 8;

    bool    set   = false;   // false means "no material assigned"
    Model   model = Model::Constant;

    double  nd = 0.0;        // index at the d line (587.6 nm); 0 == opaque
    double  cauchyB = 0.0;   // um^2
    double  sell[6] = {};    // B1, B2, B3, C1, C2, C3 -- C in um^2

    int     samples = 0;
    double  lambdaNm[kMaxSamples] = {};
    double  nSample[kMaxSamples]  = {};
    double  kSample[kMaxSamples]  = {};   // extinction; non-zero only for a metal

    // Internal attenuation, 1/mm, at the d line. Catalogue-derived rather than
    // one number the whole scene library shares.
    double  alpha = 0.0;

    bool valid() const { return set; }

    // A metal is the material whose extinction is not zero; its reflectance
    // comes from the complex Fresnel equations rather than from a flat number.
    bool isMetal() const;

    double indexAt(double lambda) const;
    double extinctionAt(double lambda) const;

    // Abbe number V_d = (n_d - 1) / (n_F - n_C), the number a glass is sold by.
    // 0 for a non-dispersive material.
    double abbe() const;
};

// Unpolarised reflectance at a dielectric -> absorbing-medium interface, from
// the incident index `n1` onto a medium of complex index n2 - i k2. Born & Wolf
// section 13.2. Reduces to the ordinary Fresnel result as k2 -> 0, so an
// aluminium mirror at 45 degrees is genuinely not the same as at normal
// incidence, and not the same at 460 nm as at 620.
double metalReflectance(double n1, double n2, double k2, double cosI);

namespace materials {

// The catalogue, in registry order. Index 0 is always vacuum.
int  count();
const QString& name(int i);
OpticalMaterial at(int i);

// Resolves a catalogue name. Returns an unset material when the name is not
// known, so a hand-edited config degrades to "no material" rather than failing.
OpticalMaterial byName(const QString& name);
int             indexOf(const QString& name);

// A short human description, for a tooltip in the material picker.
const QString& description(int i);

} // namespace materials
