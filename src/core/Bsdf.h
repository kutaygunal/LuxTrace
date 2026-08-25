#pragma once
#include <QString>
#include <cstdint>
#include "Vec3.h"

// How a surface scatters, and how a medium scatters inside itself.
//
// Roughness used to be a Gaussian tilt of the normal with a rejection branch
// that silently kept the smooth normal whenever the tilt flipped past the
// incident ray. That rejection is a bias toward specular which grows with sigma,
// and the model has no shadowing-masking term and no normalisation -- so it is
// not an energy-conserving BSDF and it cannot be fitted to measured data.
//
// Everything here is an energy-conserving distribution with a sampling routine
// and the weight that goes with it, so a surface can be given a scatter
// specification and reproduce it.
namespace bsdf {

// The surface scattering model.
//   Specular    a polished surface: one outgoing direction
//   Microfacet  GGX with Smith shadowing-masking, one roughness parameter.
//               The drop-in replacement for the old Gaussian jitter.
//   Abg         the A, B, g model TracePro and LightTools expose, whose
//               parameters map directly onto a measured BSDF and onto total
//               integrated scatter -- which is what lets a scatter spec sheet be
//               reproduced rather than approximated
//   Lambertian  a perfect diffuser, kept as its own case so an integrating
//               sphere and a matte reflector are exactly what they were
//   Table       a measured BSDF, resampled onto a log-spaced grid in |beta-beta0|
enum class Model : int { Specular = 0, Microfacet, Abg, Lambertian, Table };

QString modelName(Model m);
QString modelTip(Model m);

// A surface's scatter description. Fixed size and trivially copyable, so it
// travels with SurfaceOptics through every representation without allocating.
struct Surface {
    static constexpr int kMaxSamples = 16;

    Model  model = Model::Specular;

    // Microfacet: RMS microfacet slope. Roughly the old sigma, but this one is a
    // real GGX alpha with shadowing-masking and correct sampling weights.
    double alpha = 0.0;

    // ABg. B is in the same units as |beta - beta0| (direction cosines), so it
    // is the width of the specular core; g is the falloff exponent of the wings.
    // A sets the level: TIS = the integral of the model over the hemisphere.
    double abgA = 0.0, abgB = 0.01, abgG = 2.0;

    // Fraction of the reflected energy that scatters at all. The rest stays
    // specular, which is what a real polished-but-imperfect surface does.
    double fraction = 1.0;

    // Table: BSDF against |beta - beta0|, log-spaced, resampled on load.
    int    samples = 0;
    double tableBeta[kMaxSamples] = {};
    double tableValue[kMaxSamples] = {};

    bool isSpecular() const { return model == Model::Specular || fraction <= 0.0; }

    // The BSDF itself, per steradian, at a scatter angle whose direction-cosine
    // offset from the specular direction is `dbeta`. Exposed because a scatter
    // model is only fittable to a measurement if the value it produces can be
    // read off and compared against one -- and because a lobe is worth plotting.
    double value(double dbeta) const;

    // Total integrated scatter: the share of the incident energy this model puts
    // anywhere other than the specular direction. The number a scatter spec is
    // written in, and the one an energy budget has to agree with.
    double totalIntegratedScatter() const;

    // Samples an outgoing direction about the surface normal `n`, given the
    // incoming direction `wi` (pointing at the surface) and the specular
    // direction `spec`. Returns false when the sample would go below the
    // surface, in which case the caller keeps the specular direction.
    //
    // `weight` comes back as the ratio of the BSDF to the sampling density: 1
    // for an exactly importance-sampled lobe, and the correction otherwise.
    bool sample(const Vec3& wi, const Vec3& n, const Vec3& spec,
                std::uint64_t& rng, Vec3& out, double& weight) const;

    // The microfacet this interaction happens at, the specular direction off
    // it, and the visibility weight that makes the lobe energy conserving.
    //
    // Exposed separately from sample() because the facet is *where the
    // interface is*, and both Fresnel and Snell have to be evaluated there: a
    // rough interface is rough for the transmitted branch too. Evaluating the
    // split at the smooth normal and then deflecting the outgoing ray -- which
    // is what the Gaussian tilt this replaced did -- reflects a rough
    // dielectric as though it were polished and then sends it somewhere else.
    //
    // The returned normal is guaranteed to face `wi`, so the caller can use it
    // as the interaction normal without checking. Returns false when the draw
    // would put the outgoing ray below the surface, in which case the smooth
    // normal stands.
    bool sampleMicrofacet(const Vec3& wi, const Vec3& n, std::uint64_t& rng,
                          Vec3& micronormal, Vec3& reflected, double& weight) const;
};

// Scattering inside a medium rather than at its boundary.
//
// Absent entirely before, and among the most-used features in luminaire design:
// every white diffusing plastic in every fixture is a volume scatterer, not a
// surface one. A ray travelling through one is deflected by a Henyey-Greenstein
// phase function every mean free path.
struct Volume {
    // Scattering coefficient, 1/mm. 0 is a clear medium.
    double coefficient = 0.0;
    // Henyey-Greenstein asymmetry. 0 is isotropic, positive is forward
    // scattering (which is what a filled polymer does), negative is backward.
    double anisotropy = 0.0;

    bool active() const { return coefficient > 0.0; }
    // Distance to the next scattering event, exponentially distributed.
    double sampleDistance(std::uint64_t& rng) const;
    // A new direction about `d`, from the phase function.
    Vec3 scatter(const Vec3& d, std::uint64_t& rng) const;
};

// The Henyey-Greenstein phase function, per steradian, for the angle whose
// cosine is `cosTheta`. Exposed so a test can integrate it.
double henyeyGreenstein(double cosTheta, double g);

} // namespace bsdf
