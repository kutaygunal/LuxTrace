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

// The numbers a scene exposes for editing. Fixed-size and trivially copyable so
// it can be a cache key, compared with == and written into a config file
// without any allocation.
//
// The last live slot of every scene is its receiver position, so a focus sweep
// or a detector move is a parameter change rather than a special case.
struct SceneParams {
    static constexpr int kMax = 4;
    double v[kMax] = {0.0, 0.0, 0.0, 0.0};

    bool operator==(const SceneParams& o) const {
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

        // --- scattering ---
        IntegratingSphere,      // diffuse white cavity with an exit port
        DiffuserPlate,          // transmissive diffuser over a collimated beam

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
