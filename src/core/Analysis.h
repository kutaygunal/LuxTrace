#pragma once
#include <QString>
#include <vector>
#include "SimulationResult.h"

// Post-processing over a finished trace. Nothing here re-traces anything: the
// irradiance grid and the recorded receiver arrivals carry enough information
// to answer every question below, which is what makes a focus sweep cost
// milliseconds instead of one trace per plane.
namespace analysis {

// Numbers that turn "that looks focused" into a measurement.
struct SpotMetrics {
    bool   valid = false;

    double total       = 0.0;   // flux on the receiver (== SimulationResult::fluxDetector)
    double peak        = 0.0;   // brightest bin, flux per mm^2
    double mean        = 0.0;   // over the illuminated bins, flux per mm^2
    double minLit      = 0.0;   // dimmest illuminated bin, flux per mm^2
    // Uniformity, two ways. min/max is the strict specification an illumination
    // designer is held to; mean/peak is the gentler one a beam is judged by.
    double uniformity  = 0.0;   // minLit / peak
    double meanToPeak  = 0.0;

    double centroidX   = 0.0;   // mm, world coordinates on the receiver
    double centroidY   = 0.0;
    double rmsRadius   = 0.0;   // mm, flux-weighted RMS distance from the centroid
    double d50Radius   = 0.0;   // mm, encircled-energy radii about the centroid
    double d86Radius   = 0.0;
    double fwhmX       = 0.0;   // mm, of the profile through the centroid
    double fwhmY       = 0.0;

    int    litBins     = 0;
    int    totalBins   = 0;

    // Chromaticity of the spot, from the recorded arrivals' wavelengths. Only
    // meaningful on a spectral run; cieY <= 0 means "no colour information".
    double cieX = 0.0, cieY = 0.0;   // CIE 1931 chromaticity coordinates
    double cct  = 0.0;               // correlated colour temperature, K
};

// From the binned grid. Radii are measured about the flux centroid, so an
// off-axis spot is described by its own size rather than by its offset.
SpotMetrics computeSpotMetrics(const SimulationResult& res);

// A cut through the irradiance grid. `alongX` takes the row nearest `atMm` in y
// (and vice versa); passing the centroid is what makes it a spot profile.
// Returns flux per mm^2 per bin, with `coords` holding the bin centres in mm.
struct Profile {
    std::vector<double> coord;   // mm
    std::vector<double> value;   // flux / mm^2
    double peak = 0.0;
    QString axisLabel;
};
Profile crossSection(const SimulationResult& res, bool alongX, double atMm);

// Encircled energy: the fraction of the receiver flux inside radius r of the
// centroid, sampled at `steps` radii out to the grid's corner.
struct EncircledEnergy {
    std::vector<double> radius;    // mm
    std::vector<double> fraction;  // 0..1
};
EncircledEnergy encircledEnergy(const SimulationResult& res, int steps = 96);

// ---- through focus ---------------------------------------------------------

// The recorded arrivals carry both a hit point and a direction, so they can be
// propagated analytically to any nearby plane. That only holds where nothing
// stands between the planes, which near the receiver is the interesting region
// anyway -- and it turns a focus sweep into a post-process rather than one full
// trace per plane.
struct FocusSample {
    double z         = 0.0;
    double rmsRadius = 0.0;   // mm
    double d86Radius = 0.0;   // mm
    double peakDensity = 0.0; // relative: flux inside the RMS radius / its area
};

std::vector<FocusSample> throughFocus(const SimulationResult& res,
                                      double z0, double z1, int steps);

// The plane with the smallest RMS radius, refined by a parabolic fit through
// the best sample and its neighbours. Returns z0 when there is nothing to fit.
double bestFocusZ(const std::vector<FocusSample>& sweep);

// ---- image quality ---------------------------------------------------------

// The frequency-domain half of the spot metrics.
//
// The spot metrics say how big the blur is; the MTF says what it does to
// contrast, which is the number an imaging system is actually specified by. It
// comes out of the same arrivals the spot metrics already use: a line spread
// function through the centroid, and its Fourier transform.
struct Mtf {
    bool valid = false;
    // Spatial frequency in cycles per millimetre at the receiver.
    std::vector<double> frequency;
    std::vector<double> tangential;    // modulation, 0..1
    std::vector<double> sagittal;
    // Where the average of the two falls to half. The single number an MTF is
    // usually quoted as.
    double cutoff50 = 0.0;
    // The diffraction limit for this aperture and wavelength, as a reference to
    // plot the geometric curve against: past it, no design does better.
    std::vector<double> diffractionLimit;
    double diffractionCutoff = 0.0;    // cycles/mm where the limit reaches zero
};

// `apertureRadius` and `focalLength` set the diffraction reference; pass 0 for
// either to leave it out. `wavelengthNm` defaults to the run's own mean.
Mtf modulationTransfer(const SimulationResult& res, int samples = 128,
                       double apertureRadius = 0.0, double focalLength = 0.0);

// Wavefront error, from the optical path length each arrival carries.
//
// Adding one double per branch bought all of this: an optical path difference
// map, an RMS wavefront error, a Strehl-style figure, and a diffraction-limit
// reference to plot the geometric spot against.
struct Wavefront {
    bool   valid = false;
    double rmsWaves = 0.0;      // RMS optical path difference, in waves
    double ptvWaves = 0.0;      // peak to valley
    // Where the arrivals actually converge, found as the point closest to all of
    // them. The path lengths are measured *to there* rather than to the
    // receiver, because a receiver that is not at the focus would otherwise
    // report its own defocus as though it were aberration -- and defocus is a
    // property of where the detector was put, not of how well the optic works.
    Vec3   focus;
    bool   focusFound = false;
    // Marechal's approximation: exp(-(2 pi sigma)^2). Above 0.8 the design is
    // conventionally called diffraction limited.
    //
    // It is an approximation for a *small* aberration and says nothing useful
    // about a large one: at a wave of error it is already 10^-17, and at ten it
    // underflows. `strehlMeaningful` is the honest gate -- past it the number to
    // read is the wavefront error itself, and the answer to "is this diffraction
    // limited" is simply no.
    double strehl = 0.0;
    bool   strehlMeaningful = false;
    double referenceOpl = 0.0;  // the mean the differences are taken about, mm
    // How many arrivals the figure was computed over, and how many the run had.
    // A wavefront only means something for the bundle that images: light which
    // reached the receiver without passing through the optic belongs to no
    // wavefront at all, so the core of the spot is what is measured and this is
    // the ratio that says how much of the light that was.
    std::size_t used = 0, total = 0;
    double coreFraction() const { return total ? double(used) / double(total) : 0.0; }

    // OPD binned over the receiver, in waves. Empty where there are too few
    // arrivals to fill it.
    int nx = 0, ny = 0;
    std::vector<double> opd;
    std::vector<int>    counts;
    bool   diffractionLimited() const { return valid && strehlMeaningful && strehl >= 0.8; }
};

Wavefront wavefrontError(const SimulationResult& res, int bins = 32);

// The Airy radius for a given f-number and wavelength: 1.22 lambda f/#. Plotted
// beside the geometric spot, it is what says whether a design has stopped being
// geometry-limited and started being diffraction-limited.
double airyRadiusMm(double fNumber, double wavelengthNm);

// ---- export ----------------------------------------------------------------

// CSV of the irradiance grid: a header row of x coordinates, then one row per y.
QString irradianceCsv(const SimulationResult& res);
// CSV of the far-field intensity: theta, flux/sr averaged over phi, then the
// full theta x phi table.
QString intensityCsv(const SimulationResult& res);
// A one-line-per-metric summary, for pasting into a report.
QString metricsCsv(const SimulationResult& res, const SpotMetrics& m);

// ---- luminaire interchange -------------------------------------------------

// The far-field grid is already the candela distribution a luminaire is
// specified by, so it is one unit conversion and one file writer away from
// being the format the entire lighting industry consumes. With these a LuxTrace
// result goes straight into DIALux, AGi32 or Relux.
//
// Both require photometric units: a run in watts is converted through the
// source spectrum's luminous efficacy, and a run whose spectrum carries no
// luminous flux at all (a line outside the visible band) is refused rather than
// exported as zeros.
QString iesLm63(const SimulationResult& res, const QString& luminaire = QString());
QString eulumdat(const SimulationResult& res, const QString& luminaire = QString());
// Whether the result can be exported at all, with the reason when it cannot.
bool canExportPhotometry(const SimulationResult& res, QString* whyNot = nullptr);

bool writeTextFile(const QString& path, const QString& text, QString* errorOut = nullptr);

} // namespace analysis
