#pragma once
#include <QString>

#include "core/SimulationResult.h"

// What an appearance render says about itself when it leaves the screen.
//
// Phases 1 to 4 put a picture in a tab, where a permanent strip under it says
// what the picture is not. An exported PNG and a figure in a design-review
// report have no strip: they travel, they get pasted into slide decks, and they
// arrive beside irradiance maps and candela plots that *are* measurements. The
// risk register calls this out by name -- "the render is mistaken for an
// analysis result" -- and says it is managed by writing rather than by code.
//
// So the writing lives here, in one place, as functions rather than as string
// literals copied into three call sites. The tab strip, the save-status line
// and the report caption all read from these, and a test asserts that every
// caption this module can produce still carries the disclaimer. A sentence that
// can be dropped by editing one call site is a sentence that will be.
//
// The request normalisation is here for the same reason it is here rather than
// in the view: it is arithmetic on four integers, it decides what a user typing
// "40000" into a resolution box actually gets, and it is worth asserting on
// without a GL context in the room.
namespace appearance {

// Declared rather than included: this header is pulled into the window that
// owns the export dialog, and RadianceMap drags AIS_Shape in behind it.
struct RadianceMap;

// The bounds an offscreen render is held to.
//
// The ceiling is a framebuffer that a mainstream GPU will actually allocate --
// 8192 square is the GL 4.x guaranteed minimum for a colour attachment, and
// asking past it fails inside the driver where the reason is invisible. The
// floor is a picture there is any point in producing. The sample ceiling is not
// a hardware limit; it is the point past which another frame changes nothing
// visible and only costs a user their afternoon.
inline constexpr int kMinExportDimension = 16;
inline constexpr int kMaxExportDimension = 8192;
inline constexpr int kMinExportSamples   = 1;
inline constexpr int kMaxExportSamples   = 8192;

// One offscreen render, as asked for.
struct ExportRequest {
    int width   = 1920;
    int height  = 1080;
    // Frames to accumulate before the buffer is read back. The offscreen render
    // runs its own accumulation loop, so this is independent of whatever the
    // on-screen estimate happens to have reached.
    int samples = 512;
};

// Holds a request to what can be rendered, and fills in a missing dimension
// from the aspect of the view it came from.
//
// A zero width or height means "match the view", which is what makes "give me
// this at 4K wide" a single number rather than an arithmetic exercise the user
// has to get right for the render not to be stretched.
ExportRequest normaliseExport(int width, int height, int samples, double viewAspect);

// The sentence that travels with every image this view produces. Stated once,
// used by the tab strip, the export status line and the report caption.
QString notPhotometricNote();

// What the emission was driven by, or an empty string when the render was lit
// by its sources alone.
//
// The flux is quoted because it is the one physical number in the picture: the
// distribution is the run's and its total is in the run's own unit, while the
// brightness on screen is a normalisation with no unit at all.
QString distributionNote(const RadianceMap& map, FluxUnit unit);

// The caption an exported render carries, wherever it lands. Says how big it
// is, how far the estimate got, what drove the emission, and what the image is
// not -- in that order, because the first three are what a reader wants and the
// fourth is what they need.
QString exportCaption(const ExportRequest& request, const QString& driven = QString());

// The same for the strip under the live view, where the estimate is still
// running and the interesting number is how far along it is.
QString liveCaption(int samples, int budget, const QString& driven = QString());

} // namespace appearance
