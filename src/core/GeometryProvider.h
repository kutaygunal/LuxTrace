#pragma once
#include <vector>
#include <QString>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include "SurfaceOptics.h"

// A B-Rep shape together with its assigned optical behaviour.
// This mirrors how LightTools attaches optical properties to CAD parts.
struct OpticalSurface : SurfaceOptics {
    TopoDS_Shape shape;
    QString      label;

    // Tessellation hints, in millimetres and radians. These are not optical
    // properties -- they say how finely to approximate the B-Rep -- but they
    // govern optical accuracy all the same: a facet's normal is off by up to
    // half the angular deflection, and a mirror doubles that into the reflected
    // ray. The angular term is what limits how sharply a scene can focus; the
    // linear one barely matters on a curved optic.
    //
    // Left at the default for everything except an optic built from many small
    // repeated bodies, where the fine default multiplies out into a build that
    // takes seconds.
    double meshDeflection = 0.30;
    double meshAngle      = 0.06;

    // Where this part sits. Empty means "once, where the shape already is".
    // More than one entry makes it an instanced part: `shape` is tessellated
    // once about its own origin and placed at each transform, so an array of
    // twenty-five identical lenslets is one mesh and one hierarchy rather than
    // twenty-five of each. The transforms must be rigid -- rotation and
    // translation only -- because a scaled placement would change what a
    // distance means inside it.
    std::vector<gp_Trsf> placements;
};

// The numbers a scene exposes for editing. Small inline vector, not a heap
// allocation: the struct is copied once per study evaluation, so a heap buffer
// would defeat the copy-as-cache-key property. It stays usable as a cache key,
// comparable with == and writable into a config file without any allocation.
//
// `kMax` is the capacity. A scene uses the first `size()` slots; unused slots
// are kept zeroed so that equality, hashing and a cache key see the same
// contents no matter how a value was constructed.
//
// The last live slot of every scene is its receiver position, so a focus sweep
// or a detector move is a parameter change rather than a special case.
// `receiver()` is the single named accessor for that slot.
struct SceneParams {
    // Capacity. The old fixed-four bound left a scene with only three free
    // dimensions (the receiver always owns the final live slot); a cemented
    // doublet needs six, so the buffer is sized for six and scenes that use
    // fewer simply leave the tail as zeroed dead slots.
    static constexpr int kMax      = 6;
    static constexpr int kCapacity = kMax;

    // The number of live parameter slots this instance carries. Set by
    // GeometryProvider when it mints or cleans a param set; a value built by
    // hand (e.g. a widget filling v[0..n)) carries the natural default and is
    // re-derived the moment it passes through sanitise().
    int n = 0;

    // Inline storage, in place -- never on the heap. Unused slots stay 0.0.
    double v[kMax] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    SceneParams() = default;

    int  size() const { return n; }
    int  capacity() const { return kMax; }
    bool empty() const { return n == 0; }

    double& operator[](int i) { return v[i]; }
    const double& operator[](int i) const { return v[i]; }

    // The last live slot is the receiver position; this is the single accessor
    // callers use for it. n == 0 only ever occurs for a not-yet-populated
    // value, which returns slot 0 as a safe stand-in.
    double& receiver() { return v[(n > 0 ? n : 1) - 1]; }
    const double& receiver() const { return v[(n > 0 ? n : 1) - 1]; }

    bool operator==(const SceneParams& o) const {
        if (n != o.n) return false;
        for (int i = 0; i < kMax; ++i)
            if (v[i] != o.v[i]) return false;
        return true;
    }
    bool operator!=(const SceneParams& o) const { return !(*this == o); }
};

// Everything a spin box needs to edit one scene parameter.
struct SceneParamInfo {
    QString name;
    QString unit;
    double  min      = 0.0;
    double  max      = 1.0;
    double  def      = 0.0;
    double  step     = 1.0;
    int     decimals = 1;
    QString tip;
};

// A number a user reasons about, computed from the numbers they typed.
//
// Somebody editing "focal length" and "aperture" is really thinking in f-number,
// numerical aperture, acceptance angle, concentration ratio, etendue and where
// the paraxial focus lands. All of those follow from the parameters already on
// screen, and none of them used to be shown -- which is the difference between
// a form and an instrument.
struct DerivedQuantity {
    QString name;
    QString value;      // already formatted, because the sensible precision
                        // differs from one quantity to the next
    QString unit;
    QString tip;
};

class GeometryProvider {
public:
    // Scenes are a flat, ordered registry: the UI, the diagnostics and the
    // tests all enumerate them through count()/info() rather than hardcoding a
    // list, so adding one here is the only edit a new scene needs.
    enum class Scene {
        // --- reflective ---
        Reflector = 0,          // parabolic mirror, collimator
        EllipticalReflector,    // ellipsoid, focus-to-focus transfer
        SphericalReflector,     // spherical mirror, shows spherical aberration
        OffAxisParabola,        // off-axis paraboloid segment, off-axis receiver
        ParabolicTrough,        // extruded parabola, line focus
        Cpc,                    // compound parabolic concentrator (nonimaging)
        ConicalConcentrator,    // reflective funnel
        Cassegrain,             // folded two-mirror system
        CornerCube,             // three orthogonal mirrors, retroreflection

        // --- refractive ---
        Lens,                   // solid glass ball lens
        PlanoConvexLens,
        BiconvexLens,
        PlanoConcaveLens,       // diverging
        HalfBallLens,           // LED dome coupling
        CylindricalLens,        // rod lens, focuses one axis only
        FresnelLens,            // stepped annular facets
        Axicon,                 // conical lens, ring-shaped irradiance
        MicrolensArray,         // 5 x 5 array of ball lenses
        Prism,                  // triangular prism, beam deviation

        // --- total internal reflection ---
        LightGuide,             // round rod
        SquareLightGuide,       // square rod
        TaperedLightGuide,      // tapered rod, angle transformation
        PorroPrism,             // TIR retroreflector
        TirLens,                // TIR collimator: central lens plus a TIR wall

        // --- scattering ---
        IntegratingSphere,      // diffuse white cavity with an exit port
        DiffuserPlate,          // transmissive diffuser over a collimated beam

        // --- multi-source ---
        LedArrayLuminaire,      // a row of reflector cups, one emitter each

        // --- meta-optics ---
        Metalens,               // a phase-gradient flat lens, no curvature at all

        // --- showcase ---
        ShowcaseLuminaire,      // a whole fixture, built to be looked at

        Count
    };

    // Everything the rest of the app needs to know about a scene, apart from
    // its geometry: how it is labelled and where its source sits by default.
    struct SceneInfo {
        QString name;
        QString description;
        gp_Pnt  sourceOrigin;
        gp_Dir  sourceAxis;     // Lambertian hemisphere normal / Point is isotropic
    };

    // A built scene: its shapes plus where the source has to sit for those
    // shapes. The two travel together because a parametric scene moves its own
    // focus -- lengthen a paraboloid and the emitter has to follow it.
    struct SceneSetup {
        std::vector<OpticalSurface> surfaces;
        gp_Pnt sourceOrigin{0, 0, 0};
        gp_Dir sourceAxis{0, 0, 1};
        // What to call this geometry, where it did not come from the registry.
        // A scene built by `build` leaves it empty and is named by its enum;
        // one assembled around an imported file carries the file's name here,
        // because there is no enum that describes it.
        QString label;
        QString description;
    };

    static int  count() { return int(Scene::Count); }
    static const SceneInfo& info(Scene scene);

    // The editable numbers of a scene, in slot order. Never more than
    // SceneParams::kMax entries; the last is always the receiver position.
    static const std::vector<SceneParamInfo>& paramInfo(Scene scene);
    static SceneParams defaultParams(Scene scene);
    // Clamps every slot to its declared range and zeroes the unused ones, so a
    // hand-edited config file cannot produce degenerate geometry.
    static SceneParams sanitise(Scene scene, const SceneParams& params);

    // Where a scene expects its *additional* emitters to sit, as offsets in
    // millimetres from the one `build` places. Empty for a scene built around a
    // single emitter, which is every scene but the array: one emitter is what
    // an optic on an axis has, and inventing a lattice for it would put light
    // where the design does not have any.
    //
    // A scene that does declare one is saying something a description cannot:
    // a four-cup luminaire needs four sources at three known offsets, and
    // having the scene state them is the difference between clicking Add three
    // times and typing nine coordinates from the parameter block.
    static std::vector<gp_Pnt> sourceOffsets(Scene scene, const SceneParams& params);

    // Builds the shapes for a scene at the given parameters, together with the
    // source placement they imply. Always appends the planar detector.
    static SceneSetup build(Scene scene, const SceneParams& params);

    // Convenience: the scene at its default parameters.
    static std::vector<OpticalSurface> buildScene(Scene scene);

    // What the current parameters imply, without tracing anything. Computed
    // from closed-form optics, so it updates as a spin box moves rather than
    // waiting for a run -- which is what makes it teach the tool while the user
    // drives it.
    static std::vector<DerivedQuantity> derived(Scene scene, const SceneParams& params);
};
