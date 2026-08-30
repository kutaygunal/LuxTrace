#pragma once
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

#include <AIS_InteractiveContext.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "render/AppearanceEmitters.h"
#include "render/AppearanceExport.h"
#include "render/AppearanceScene.h"
#include "render/RadianceMap.h"

class QImage;
class QTimer;

// The Appearance preview: what this part looks like switched on.
//
// It is *not* a photometric result, and nothing in the application reads it as
// one. OCCT's path tracer is an RGB graphics renderer with its own two-layer
// BSDF; it does not consult SurfaceOptics' scatter model, Coating's stack,
// Material's Sellmeier data or Polarisation, and its output is tone-mapped sRGB
// rather than cd/m^2. Luminance, angular and spatial luminance meters, and
// backward photometric analysis all remain work on *our* tracer. The label on
// the tab says so, and so does every image this view exports.
//
// A second V3d_View rather than a mode switch on the main viewport: that one
// carries the walkthrough camera, the clipping plane, surface picking, the
// transform gizmo and the ray overlay, none of which belong in a render, and
// toggling the rendering method on a live view throws the accumulated estimate
// away on every camera nudge the gizmo makes. The graphic driver is shared, so
// the cost is one extra GL context and not a second copy of the driver.
//
// Navigation
//   Left drag    orbit          Middle drag  pan          Wheel  zoom
//   F            fit the scene  R            reset the camera
// Any of them restarts the estimate, which is what progressive path tracing
// means: the image is a running average and moving the camera invalidates it.
class AppearanceView : public QWidget {
    Q_OBJECT
public:
    // Tone mapping, in the order the combo box offers it.
    enum class ToneMapping { Disabled = 0, Filmic };

    // Where the light in the picture comes from.
    //
    //   SceneEmitters  the optic's own sources and nothing else, against a dark
    //                  surround. What a luminaire actually looks like switched
    //                  on, and the only mode in which the emitters are the
    //                  brightest thing in frame.
    //   Studio         a three-point rig, for reading the *shape* of a part
    //                  that has no source, or whose source is not the subject.
    //   Sky            OCCT's procedural skydome as an environment light, which
    //                  is what makes metal read as metal and glass as glass:
    //                  a specular surface with nothing around it to reflect has
    //                  nothing to show.
    //
    // The scene's emitters are drawn in every mode. What changes is what else
    // is lit, and by what.
    enum class Lighting { SceneEmitters = 0, Studio, Sky };

    explicit AppearanceView(QWidget* parent = nullptr);
    ~AppearanceView() override;

    // The driver to share with the main viewport. Must be called before the
    // first paint to have any effect; a null handle means "make your own".
    void setSharedDriver(const Handle(Graphic3d_GraphicDriver)& driver);

    // Replaces what is rendered. Cheap enough to call on every geometry change:
    // the presentations are rebuilt, which costs a tessellation, and the
    // estimate restarts either way because the scene it was estimating is gone.
    void setSurfaces(const std::vector<OpticalSurface>& surfaces);

    // The run's emitters, exactly as the trace will place them -- taken from
    // Simulation::sourcesFor rather than derived a second way here, because a
    // renderer that placed the light somewhere else than the tracer would be a
    // renderer nobody could trust to check the geometry.
    void setSources(const std::vector<SourceConfig>& sources);
    // Whether any source has a real emitting area, so a caller can say why a
    // scene of point sources shows no glowing face anywhere.
    bool hasAreaEmitters() const;

    // The last run, and which of its receivers -- if any -- drives the emission.
    //
    // This is the piece that makes the render LuxTrace's rather than any CAD
    // viewer's. With a receiver nominated, the exit surface stops glowing
    // uniformly and glows with the distribution the trace computed: change the
    // source and the bright region moves, because the bright region is the
    // answer. -1 leaves the render lit by its sources alone.
    void setResult(const SimulationResult& result);
    void clearResult();
    void setRadianceSource(int detectorIndex);
    int  radianceSource() const { return m_radianceSource; }
    // The map currently driving the emission, for the caption that has to say
    // what it was built from and how much flux it carries.
    const appearance::RadianceMap& radianceMap() const { return m_radiance; }
    void setShowDetectors(bool on);
    bool showDetectors() const { return m_showDetectors; }

    // Path tracing on, or the rasterized PBR preview. The preview reads the
    // same materials through Graphic3d_PBRMaterial, so it is the same scene at
    // lower fidelity rather than a different one.
    void setPathTracing(bool on);
    bool pathTracing() const { return m_pathTracing; }

    // How many accumulated frames to stop at. The image is a running average;
    // this is where it stops improving and stops costing GPU time.
    void setSampleBudget(int frames);
    int  sampleBudget() const { return m_budget; }
    int  samples() const { return m_samples; }

    // Maximum path length. OCCT defaults to 3, which is too few for glass: a
    // ball lens needs a refraction in, a refraction out and something to hit.
    void setRayDepth(int depth);

    void setExposure(double ev);
    void setWhitePoint(double white);
    void setToneMapping(ToneMapping mode);
    void setLighting(Lighting mode);
    Lighting lighting() const { return m_lighting; }

    // Where the camera stands. A render is a photograph and these are the four
    // shots a design review asks for; Iso is the one the view opens on, because
    // it is the only one that shows a part's depth as well as its outline.
    enum class Camera { Iso = 0, Front, Side, Top };
    void setCamera(Camera view);

    // Depth of field, path tracing only -- it is an aperture the tracer samples
    // across, not a blur applied afterwards, so the rasterized preview has none
    // and cannot have any.
    //
    // The radius is in scene units, so it means the same thing on a 6 mm lens
    // and a 600 mm luminaire only if the user says so; 0 is a pinhole, which is
    // what every other view in the application is and what this one defaults
    // to. Focus distance 0 means "the middle of the scene", tracked as the
    // camera moves -- a fixed distance would slide out of focus on the first
    // orbit, which is exactly when a user would blame the renderer.
    void   setApertureRadius(double radius);
    double apertureRadius() const { return m_aperture; }
    void   setFocalDistance(double distance);
    double focalDistance() const { return m_focal; }
    // Eye to the centre of the scene, which is what focus distance 0 resolves
    // to. Zero when there is no camera and nothing to focus on.
    double autoFocalDistance() const;

    void fitAll();
    void resetView();
    // Whether the camera has ever been framed on a scene. The first build to
    // arrive frames it; later ones leave it where the user put it.
    bool framed() const { return m_framed; }
    // Throws the accumulated estimate away and starts again. Called for you on
    // any camera or parameter change; public because a "restart" button is the
    // honest control to offer beside a progressive renderer.
    void restart();

    // Whether this machine can path-trace at all, and why not when it cannot.
    // OCCT falls back to rasterization silently, which reads as "the render
    // button does nothing", so the answer is probed up front and stated.
    bool           capable() const { return m_capable; }
    const QString& capabilityReport() const { return m_capabilityReport; }

    // ---- output ------------------------------------------------------------
    //
    // How far the offscreen render has got, as (frames done, frames asked for).
    // Returning false cancels it and hands back the estimate as it stands,
    // which is a picture rather than nothing -- an accumulation loop that can
    // only be waited out is one a user kills with the task manager.
    using ExportProgress = std::function<bool(int, int)>;

    // Renders at an arbitrary resolution, independent of the window.
    //
    // The whole point of the offscreen path is that it runs *its own*
    // accumulation loop: an offscreen buffer starts its estimate from nothing,
    // so a single dump at 4K hands back one noisy sample of a picture the
    // window had already converged. The loop below redraws into the same
    // offscreen buffer `request.samples` times before reading it back once,
    // which is the same progressive estimate the window runs, at a size the
    // window never has to be.
    //
    // Nothing else redraws while this runs -- the pump is stopped and paints
    // are ignored -- so a caller may pump the event loop from `onProgress` to
    // keep a progress dialog alive without the two estimates fighting over the
    // view. The window's own estimate restarts when this returns, because the
    // buffer it was accumulated in has been resized underneath it.
    bool renderToImage(QImage& out, const appearance::ExportRequest& request,
                       const ExportProgress& onProgress = {});

    // The view as it stands on screen, at the size it stands there, through the
    // view's own buffer dump rather than QWidget::grab -- which would capture
    // whatever the compositor happens to have over the native window instead.
    // Costs nothing and throws no estimate away, which is what makes it the
    // right thing for a quick capture and the wrong thing for a document.
    bool saveImage(const QString& path);

    // Width over height of the drawn area, for an export that wants to match
    // the framing on screen from one dimension.
    double viewAspect() const;

    // OCCT draws into this widget's native surface; Qt must not paint over it.
    QPaintEngine* paintEngine() const override { return nullptr; }

signals:
    // The estimate advanced. `samples` of `budget` frames accumulated.
    void progress(int samples, int budget);
    // The capability probe finished. Emitted once, after the first paint,
    // because there is no GL context to ask before that.
    void capabilityDetermined(bool ok, const QString& why);
    // A light the viewer would not build. The picture is still drawn, lit by
    // whatever was accepted -- but a scene lit differently than it was asked to
    // be has to say so, or the render quietly answers a question nobody asked.
    void lightingFailed(const QString& why);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private slots:
    void step();

private:
    void initViewer();
    void probeCapability();
    void applyLighting();
    void applyEnvironment();
    void applyRenderingParams();
    void rebuildScene();

    Handle(Graphic3d_GraphicDriver) m_sharedDriver;
    Handle(V3d_Viewer)              m_viewer;
    Handle(V3d_View)                m_view;
    Handle(AIS_InteractiveContext)  m_context;

    std::vector<OpticalSurface> m_surfaces;
    appearance::Build           m_build;
    std::vector<SourceConfig>   m_sources;
    appearance::EmitterBuild    m_emitters;
    // The run whose distribution is being rendered, kept by value: the view
    // outlives any one result and a dangling pointer into the last one is not a
    // trade worth making for a grid of doubles.
    SimulationResult            m_result;
    bool                        m_haveResult   = false;
    int                         m_radianceSource = -1;
    appearance::RadianceMap     m_radiance;
    double                      m_sceneSize = 100.0;
    // The lights this view put in the viewer, so they can be taken out again
    // exactly. Asking the viewer for its list would also collect anything OCCT
    // added on its own behalf.
    std::vector<Handle(V3d_Light)> m_lights;
    // Whether `m_surfaces` has changed since the presentations were built.
    // Rebuilding costs a tessellation per body, so it waits until the tab is
    // actually looked at: a user who never opens this tab pays nothing for it,
    // and every geometry edit would otherwise tessellate the scene twice.
    bool m_dirty = true;

    QTimer* m_timer = nullptr;

    // Accumulated frames since the last restart, and where to stop.
    int  m_samples = 0;
    int  m_budget  = 512;
    int  m_depth   = 8;

    bool        m_pathTracing   = true;
    bool        m_showDetectors = false;
    double      m_exposure      = 0.0;
    double      m_whitePoint    = 1.0;
    // Aperture radius in scene units, 0 for a pinhole; focus distance in scene
    // units, 0 for "wherever the middle of the scene is".
    double      m_aperture      = 0.0;
    double      m_focal         = 0.0;
    ToneMapping m_tone          = ToneMapping::Filmic;
    Lighting    m_lighting      = Lighting::SceneEmitters;

    // Whether the adaptive sampler is used. Needs a GL 4.4-class context for
    // the atomic image operations it is built on, so it follows the probe
    // rather than being asserted.
    bool m_adaptive = false;

    // Set while the offscreen render owns the view. Paints and accumulation
    // steps are ignored for its duration, so a caller may pump the event loop
    // to keep a progress dialog responsive without the window's estimate and
    // the export's estimate redrawing over each other.
    bool m_exporting = false;

    bool    m_framed     = false;
    bool    m_capable    = false;
    bool    m_probed     = false;
    bool    m_initFailed = false;
    QString m_capabilityReport;

    QPoint          m_lastMouse;
    Qt::MouseButton m_dragButton = Qt::NoButton;
};
