#pragma once
#include <QString>
#include <cstddef>
#include <memory>
#include <vector>

#include "SimulationResult.h"   // FluxUnit
#include "Vec3.h"

// Measured source ray files.
//
// LED vendors publish their parts as ray sets: a few hundred thousand rays,
// each with a position on the emitting surface, a direction, a flux and often a
// wavelength, measured on a goniophotometer. Reading one turns "model an LED"
// into "use this LED", and it is the single most-requested interoperation in
// illumination design.
//
// It needs no engine change. A ray file is an EmittedRay stream, and emission
// is the only place that branches: everything downstream already carries a
// per-ray weight, wavelength and origin.
//
// Three formats cover what people actually have:
//
//   Zemax binary source file (.dat) -- a 208-byte header (identifier 1010, a
//   ray count, the source and ray-set flux, the flux unit, a dimension unit and
//   a ray format) followed by seven or eight float32 per ray. TracePro and most
//   vendor downloads use this same layout for interchange.
//
//   ASAP .dis and the plain text ray files TracePro and several vendors ship --
//   whitespace or comma separated, six to eight numbers per line, with
//   whatever header lines the writer felt like adding. Read tolerantly: a line
//   that is not a full row of numbers is a comment, whatever it says.
//
// The set is loaded once and shared: a ray file is tens of megabytes and a
// study traces the same one hundreds of times.
struct SourceRay {
    Vec3   origin;
    Vec3   dir;                    // unit
    double power        = 1.0;     // in the file's own flux unit
    double wavelengthNm = 0.0;     // 0 == the file did not say
};

struct RayFileData {
    QString path;
    QString label;
    QString format;                // human-readable, for the report

    std::vector<SourceRay> rays;

    // Sum of the ray powers in the file. This is the ray *set's* flux, which is
    // not the same as the source's: a vendor commonly ships a subset of the
    // rays a full measurement produced, and the header carries both.
    double raySetFlux = 0.0;
    // The flux of the source the set was drawn from, where the file says so.
    double sourceFlux = 0.0;
    // The unit those two are quoted in. A ray file that says lumens and a run
    // that asks for watts is a real mismatch, and it is worth saying so rather
    // than silently rescaling.
    FluxUnit unit = FluxUnit::Watt;

    // True when at least one ray carried its own wavelength. A set without them
    // takes the run's spectrum instead, which is what a monochromatic
    // measurement of a white LED needs.
    bool     hasWavelengths = false;
    // The single wavelength the header declares, where there is one.
    double   headerWavelengthNm = 0.0;

    // Millimetres per file unit, from the header's dimension code. Applied on
    // load, so everything below is already in millimetres.
    double   unitScale = 1.0;

    // Extent of the emitting surface, in millimetres. What makes it possible to
    // say "this is a 1 mm die" rather than to find out by tracing.
    Vec3     bmin, bmax;

    // How many rays the file said it held, before any cap was applied. A capped
    // load is still unbiased -- the rays are a random sample of a random sample
    // -- but the user should be told it happened.
    std::size_t declared = 0;

    bool   valid()   const { return !rays.empty(); }
    std::size_t size() const { return rays.size(); }
    // Mean ray power, the divisor that turns a ray's flux into a weight around
    // one -- which is the convention every other emitter in the tracer uses.
    double meanPower() const {
        return rays.empty() ? 0.0 : raySetFlux / double(rays.size());
    }
    Vec3   extent() const { return bmax - bmin; }
    // A one-line summary for the import dialog and the run report.
    QString summary() const;
};

namespace rayfile {

struct LoadResult {
    bool    ok = false;
    QString error;
    std::shared_ptr<const RayFileData> data;
};

// Reads a ray file, dispatching on its content rather than on its extension:
// vendors ship binary sets as .dat, .ray, .txt and .dis indiscriminately, so
// the identifier word is tried first and the text reader is the fallback.
//
// `maxRays` caps how many rays are kept, 0 for all of them. A capped load takes
// an evenly-strided subset rather than the leading block: a ray file is often
// written in emission order, so the first N rays of a scanned measurement are
// one edge of the die rather than a sample of it.
LoadResult load(const QString& path, std::size_t maxRays = 0);

LoadResult loadZemaxBinary(const QString& path, std::size_t maxRays = 0);
LoadResult loadText(const QString& path, std::size_t maxRays = 0);

// The filter string for a file dialog, kept beside the readers.
QString fileFilter();

} // namespace rayfile
