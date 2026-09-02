#pragma once
#include <QString>

#include "Metasurface.h"

// Reading a metasurface out of what an EM solver produced.
//
// Meta-optic data does not arrive as a phase gradient and a number. It arrives
// as a table: for each wavelength, and often for each incidence angle, the
// complex transmission coefficient of each propagating diffraction order, per
// polarisation. That is what an RCWA or FDTD solve of one unit cell gives, and
// it is what a vendor ships alongside a part.
//
// What a ray tracer needs from it is the modulus squared -- the fraction of the
// incident power that leaves in each order -- so this reads the file and
// reduces it. The phase between s and p is not reduced away because it is
// unimportant; it is dropped because nothing downstream carries a retardance
// for a diffraction order yet, and inventing one would be worse than saying so.
//
// The pattern is the one MaterialFile and RayFile already follow, and it is
// followed here for the same reason: read what the vendor wrote, report what was
// loaded, and degrade *loudly* on anything unrecognised. A metasurface silently
// read as ninety per cent efficient because a column was misread is exactly the
// plausible wrong number this application is built to refuse.
namespace metafile {

// What came out of one file, whether or not it worked.
struct Result {
    bool    ok = false;
    QString error;
    // Human-readable summary of what was read: the format, the band, how many
    // wavelengths and which orders. Shown rather than logged, because "it
    // loaded" is not the same as "it loaded what you meant".
    QString summary;

    meta::Efficiency efficiency;
    // Design wavelength, when the file names one. 0 when it does not, and the
    // caller keeps whatever the surface already had.
    double designLambdaNm = 0.0;
    // Grating period in millimetres, when the file names one; the phase
    // gradient 2 pi / period follows from it.
    double periodMm = 0.0;

    int wavelengths = 0;
    int orders      = 0;
    // Rows the reader understood the shape of but could not use -- an order past
    // what the surface carries, say. Counted rather than ignored.
    int skipped     = 0;
    bool polarising = false;
};

// Reads a metasurface efficiency table.
//
// The format is deliberately the plainest thing that can carry the data, because
// every vendor exports something different and a converter is a script rather
// than a parser:
//
//   # comments and blank lines are ignored
//   design_wavelength_nm 550
//   period_um            0.42
//   # lambda_nm  order  eff_s  eff_p        (eff_p optional; s is used for both)
//   450          0      0.041  0.038
//   450          1      0.882  0.871
//   ...
//
// Complex coefficients are accepted too, as four columns after the order --
// re_s im_s re_p im_p -- and reduced to their moduli squared here, which is what
// an RCWA export actually contains.
Result load(const QString& path);

// The same, from text already in memory. What the file reader is written on top
// of, and what the tests drive.
Result parse(const QString& text);

} // namespace metafile
