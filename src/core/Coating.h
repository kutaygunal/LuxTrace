#pragma once
#include <QString>
#include <cstdint>
#include <vector>

// Thin-film coatings on a refractive surface.
//
// There were none. Every refractive surface paid bare-glass Fresnel, so a
// multi-element system overstated its loss by roughly 3.5 % per surface against
// any real lens -- all of which are coated. The efficiency figures were
// therefore pessimistic in a way that anybody who knows optics notices at once.
//
// Three tiers, in the order they are worth having:
//   Ideal   a specified residual reflectance at normal incidence with the
//           correct angular roll-off. An afternoon's work that closes most of
//           the gap.
//   Table   measured R(lambda, theta), used as it stands.
//   Stack   a characteristic-matrix solver over a handful of layers, for
//           designing a coating rather than applying one.
namespace coating {

enum class Model : int { None = 0, Ideal, Table, Stack };

QString modelName(Model m);
QString modelTip(Model m);

// A coating, fixed-size so it travels with the surface it is on. The measured
// R(lambda) table is sized generously (256 samples) so a real vendor table --
// which rarely exceeds a few hundred points -- is carried on the fixed arrays
// on the hot path exactly as measured, rather than truncated to eight.
struct Coating {
    static constexpr int kMaxLayers  = 8;
    static constexpr int kMaxSamples = 256;

    Model  model = Model::None;

    // Ideal: residual reflectance at normal incidence. 0.0025 is an ordinary
    // broadband AR; 0.995 is a dielectric high reflector.
    double residual = 0.0;
    // Whether the residual is a floor to reflect (a mirror coating) rather than
    // a target to suppress (an anti-reflection one).
    bool   highReflector = false;

    // Table: R against wavelength at normal incidence. The angular dependence
    // comes from the same roll-off the ideal model uses. These are the hot-path
    // arrays; whatever a file carried is kept exactly here when it fits, and a
    // larger file keeps its full source in the off-path sidecar below instead
    // of losing it.
    int    samples = 0;
    double lambdaNm[kMaxSamples] = {};
    double reflectance[kMaxSamples] = {};

    // Full-fidelity source table, off the hot path. A measured R(lambda) file
    // is preserved exactly as read so nothing a vendor published is lost to the
    // fixed arrays above: `reduced` is false whenever every source point fit
    // (originCount == samples), and true only when a file exceeded the cap and
    // was resampled onto the hot-path arrays while its exact source still lives
    // here. The trace loop reads the fixed arrays; these members exist so the
    // reduction is visible where the number is read and the full data round-
    // trips.
    int                 originCount = 0;
    std::vector<double> originLambdaNm;
    std::vector<double> originReflectance;
    bool                reduced = false;

    // Stack: quarter-wave-ish layers, outermost first, described by index and
    // physical thickness in nanometres.
    int    layers = 0;
    double layerIndex[kMaxLayers] = {};
    double layerThicknessNm[kMaxLayers] = {};

    bool active() const { return model != Model::None; }

    // Unpolarised reflectance of the coated interface, n1 -> n2, at the given
    // angle of incidence and wavelength. `bare` is what the uncoated interface
    // would have reflected, which the ideal and table models modulate.
    double reflectance_(double n1, double n2, double cosI, double lambdaNm, double bare) const;

    // Per-polarisation reflectance, for a polarised trace. Both collapse to the
    // unpolarised average when the model has no polarisation of its own.
    void reflectanceSP(double n1, double n2, double cosI, double lambdaNm,
                       double bareS, double bareP, double& rs, double& rp) const;
};

// A few coatings worth having by name, so a surface can be given one the way a
// drawing specifies it.
int             count();
const QString&  name(int i);
const QString&  description(int i);
Coating         at(int i);
Coating         byName(const QString& name);

// The reflectance a measured table holds at one wavelength, before the angular
// roll-off is applied to it.
//
// Exposed because a monochromatic backend can resolve the table once per run
// rather than carrying it: reflectanceSP reads a table only to pick the target
// it then rolls off exactly as the ideal model does, so one number is the whole
// of what a fixed wavelength needs. Sharing the lookup rather than copying it
// is what keeps the two from drifting apart about what the table says.
double tableReflectanceAt(const Coating& c, double lambdaNm);

} // namespace coating
