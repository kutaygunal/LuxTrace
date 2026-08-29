#pragma once
#include <QElapsedTimer>
#include <QSet>
#include <QWidget>
#include <vector>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewCube.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "core/GeometryProvider.h"
#include "core/SimulationResult.h"

class RayCloud;

// A real OpenCASCADE 3D viewport (V3d_View + AIS_InteractiveContext) embedded in
// a Qt widget. This is the only pane that actually renders through OCCT: the
// heatmap and the X-Z diagram are hand-drawn QPainter output.
//
// Navigation
//   Left drag    orbit around the scene centre (a left *click* picks a surface)
//   Middle drag  pan
//   Right drag   free look (turn in place, for walking through the optic)
//   Wheel        zoom toward the cursor
//   W / S        walk forward / back        A / D  strafe left / right
//   E / Q        rise / drop                Shift  x4 speed, Ctrl  x0.25
//   F            fit the whole scene        R      reset to the default view
//
// The navigation cube in the upper-right corner is the other way to aim the
// camera: click a face for a standard view, an edge for a 45-degree one, a
// corner for an isometric. It only turns the camera -- the zoom and the pan the
// user chose are theirs, and F is still what frames the scene.
class OcctViewWidget : public QWidget {
    Q_OBJECT
public:
    // What the colour of a ray leg means.
    enum class RayColor {
        Uniform = 0,   // one amber for everything
        Energy,        // how much of the ray is still travelling: shows the losses
        Bounces,       // interaction count: shows where a path gets trapped
        Wavelength,    // real colour, for a spectral run
    };

    explicit OcctViewWidget(QWidget* parent = nullptr);
    ~OcctViewWidget() override;

    // Replaces the displayed geometry. Cheap to call on every scene change:
    // the presentations are reused when the part list keeps its shape, so a
    // parameter edit swaps the B-Rep behind each surface rather than rebuilding
    // the interactive context.
    //
    // The camera is only reframed when `scene` differs from the one on screen.
    // A dimension change keeps it exactly where it is -- reframing on every
    // edit threw away the viewpoint the user had chosen, which is the whole
    // reason for looking at a parameter in 3D in the first place.
    void setScene(GeometryProvider::Scene scene,
                  const std::vector<OpticalSurface>& surfaces);
    // Replaces the displayed ray paths (pass an empty vector to clear them).
    void setRays(const std::vector<RaySegment>& segments);

    // Where the light comes from and where it is measured, so the scene reads
    // as an experiment rather than as a pile of grey solids.
    //
    // A first-time user cannot tell which of two translucent objects is the
    // measurement plane, and no amount of documentation fixes that as cheaply
    // as drawing it. `halfAngleDeg` >= 180 draws no cone.
    // One emitter, as the overlay draws it. A run has as many of these as the
    // configuration has sources: a luminaire with four LEDs that drew one
    // marker was showing three of them nowhere at all, so the only way to tell
    // an offset had landed where it was meant to was to trace and read the
    // pattern back.
    struct SourceGlyph {
        gp_Pnt  origin{0, 0, 0};
        gp_Dir  axis{0, 0, 1};
        double  halfAngleDeg = 180.0;
        bool    collimated   = false;
        double  beamRadius   = 0.0;
        QString label;
    };

    void setSourceGlyphs(const std::vector<SourceGlyph>& sources);
    void setSourceGlyph(const gp_Pnt& origin, const gp_Dir& axis, double halfAngleDeg,
                        bool collimated, double beamRadius);
    void clearSourceGlyph();
    // Draw the corner axis triad and the scale bar.
    void setOverlaysVisible(bool on);

    // Hide one surface, so an internal face can be seen at all.
    void setSurfaceVisible(int index, bool visible);
    // Which surface the scene tree has selected, drawn so the two agree.
    void setHighlightedSurface(int index);
    // The same, for a selection that covers more than one surface -- a group,
    // or an object the compiler split into several bodies.
    void setHighlightedSurfaces(const std::vector<int>& indices);

    void fitAll();
    void resetView();
    void setRaysVisible(bool visible);
    // Perspective is the default; orthographic is the familiar CAD projection,
    // but W/S walking has no visible effect in it.
    void setPerspective(bool on);

    // Whether the lower-left corner holds the navigation cube or the plain
    // trihedron it replaced. The cube carries its own X/Y/Z axes, so it says
    // everything the trihedron said and can be clicked as well; switching it
    // off puts the trihedron back rather than emptying the corner.
    void setViewCubeVisible(bool on);
    bool viewCubeVisible() const { return m_cubeVisible; }

    void setRayColorMode(RayColor mode);
    // Draw only the legs belonging to a path that reached the receiver. On a
    // low-efficiency scene this is the difference between a fog of rays and an
    // answer to "where does the light that works actually go?".
    void setDetectorRaysOnly(bool on);

    // Section plane through the optic, so internal TIR bounces are visible from
    // outside. `axis` is 0/1/2 for x/y/z and `position` runs 0..1 across the
    // scene's bounding box.
    void setClipEnabled(bool on);
    void setClipAxis(int axis);
    void setClipPosition(double position);
    void setClipFlipped(bool flipped);

    // Writes the viewport to an image file. OCCT owns the framebuffer, so this
    // has to go through the view's own dump rather than QWidget::grab, which
    // would capture whatever the compositor happens to have over the native
    // window instead.
    bool saveImage(const QString& path);

    // Qt must not paint over the native surface OCCT draws into.
    QPaintEngine* paintEngine() const override { return nullptr; }

signals:
    // A surface was clicked. `index` indexes the vector passed to setScene, or
    // -1 when the click landed on nothing.
    void surfacePicked(int index);
    // Something was dragged in from the object library and dropped at `where`.
    // The viewport does not know what an object is; it knows where the cursor
    // was in three dimensions, which is the part only it can answer.
    void objectDropped(const QString& typeKey, const gp_Pnt& where);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private slots:
    void stepWalk();

private:
    void initViewer();
    void applyWalk(double seconds);
    void freeLook(double dYaw, double dPitch);
    void rebuildRays();
    void rebuildOverlay();
    void buildViewCube();
    // Puts either the navigation cube or the plain trihedron in the lower-left
    // corner -- one or the other, never both.
    void applyCornerWidget();
    // Swings the camera to the orientation a cube facet stands for. Started
    // here and advanced by the widget's own timer rather than by
    // AIS_ViewCube::HandleClick, which runs the whole animation in a blocking
    // loop and would freeze the window for its duration.
    void startViewCubeAnimation(const Handle(AIS_ViewCubeOwner)& owner);
    void applyClip();
    void pickAt(const QPoint& pos);
    // Where a screen point lands in the scene: on the geometry under the cursor
    // if there is any, and otherwise on the plane through the scene centre
    // facing the camera. Without the fallback a drop onto empty space has no
    // depth at all, and "at the origin" is not where the user pointed.
    bool worldPointAt(const QPoint& pos, gp_Pnt& out);
    double sceneScale() const;

    Handle(V3d_Viewer)             m_viewer;
    Handle(V3d_View)               m_view;
    Handle(AIS_InteractiveContext) m_context;
    std::vector<Handle(AIS_Shape)> m_shapes;
    std::vector<bool>              m_visible;
    Handle(RayCloud)               m_rays;
    // The source glyph, the receiver outlines, the axis triad and the scale
    // bar, all as one line-primitive object for the same reason the rays are:
    // a presentation per line would be a presentation per line.
    Handle(RayCloud)               m_overlay;
    Handle(AIS_ViewCube)           m_viewCube;
    Handle(Graphic3d_ClipPlane)    m_clip;

    // The segments as traced, kept so a colour-mode or filter change can redraw
    // them without another trace.
    std::vector<RaySegment> m_segments;

    QSet<int>       m_keysDown;
    QPoint          m_lastMouse;
    QPoint          m_pressPos;
    Qt::MouseButton m_dragButton = Qt::NoButton;
    QElapsedTimer   m_walkClock;
    double          m_sceneSize   = 100.0;
    double          m_bbMin[3]    = {0, 0, 0};
    double          m_bbMax[3]    = {1, 1, 1};
    // Which scene the displayed shapes belong to, so a parameter edit can be
    // told apart from a change of optic and leave the camera alone.
    int             m_sceneKey    = -1;
    bool            m_haveScene   = false;
    bool            m_raysVisible = true;
    bool            m_initFailed  = false;
    RayColor        m_rayColor    = RayColor::Energy;
    bool            m_detectorOnly = false;
    // Which surfaces read as selected. A vector rather than one index: a
    // group in the scene tree covers several bodies at once.
    std::vector<int> m_highlight;
    // What each shape looks like when it is *not* selected. Highlighting
    // overwrites a presentation's colour, and AIS keeps it -- so without
    // somewhere to put the original back, every surface that had ever been
    // selected stayed amber and the viewport slowly filled up with a selection
    // that was no longer there.
    struct Appearance {
        Quantity_Color colour{0.78, 0.78, 0.82, Quantity_TOC_RGB};
        float          transparency = 0.15f;
    };
    std::vector<Appearance> m_baseLook;
    bool            m_overlaysOn  = true;
    bool            m_cubeVisible = true;
    // Whether the last hover landed on the cube. A press there must not start
    // an orbit, or dragging off a facet would turn the scene instead of doing
    // nothing.
    bool            m_hoverOnCube = false;
    std::vector<SourceGlyph> m_sources;
    // The receivers, as the frames the mesher gave them, so the overlay can
    // outline them and draw their acceptance cones.
    struct ReceiverGlyph {
        gp_Pnt centre;
        gp_Dir u, v, n;
        double w = 0.0, h = 0.0;
        double acceptanceDeg = 180.0;
    };
    std::vector<ReceiverGlyph> m_receivers;
    bool            m_clipOn      = false;
    int             m_clipAxis    = 0;
    double          m_clipPos     = 0.5;
    bool            m_clipFlipped = false;
};
