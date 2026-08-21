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

// ---- export ----------------------------------------------------------------

// CSV of the irradiance grid: a header row of x coordinates, then one row per y.
QString irradianceCsv(const SimulationResult& res);
// CSV of the far-field intensity: theta, flux/sr averaged over phi, then the
// full theta x phi table.
QString intensityCsv(const SimulationResult& res);
// A one-line-per-metric summary, for pasting into a report.
QString metricsCsv(const SimulationResult& res, const SpotMetrics& m);

bool writeTextFile(const QString& path, const QString& text, QString* errorOut = nullptr);

} // namespace analysis
