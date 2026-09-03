// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QElapsedTimer>
#include <QSet>
#include <QWidget>
#include <vector>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Manipulator.hxx>
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
//   1            put the transform gizmo away
//   2 / 3 / 4    move, turn or resize the selected object with the gizmo
//
// The navigation cube in the upper-right corner is the other way to aim the
// camera: click a face for a standard view, an edge for a 45-degree one, a
// corner for an isometric. It only turns the camera -- the zoom and the pan the
// user chose are theirs, and F is still what frames the scene.
class OcctViewWidget : public QWidget {
    Q_OBJECT
public:
    // Which gizmo is on the selected object, if any.
    //
    // One at a time: three sets of handles on the same object at the same time
    // is a target the size of the object with no room left to click the object
    // itself. None is a mode of its own rather than an absence, because putting
    // the handles away is something the user asks for -- they sit over the
    // thing they move.
    enum class TransformMode { None = 0, Translate, Rotate, Scale };

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
    // `keepCamera` overrides the reframing above: an assembled scene changes
    // key the moment the first object is dropped into a tutorial, and that drop
    // is an edit, not a change of subject, so the camera stays put.
    void setScene(GeometryProvider::Scene scene,
                  const std::vector<OpticalSurface>& surfaces,
                  bool keepCamera = false);
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
    // Which emitter reads as selected, or -1 for none. Sources have no surface
    // to highlight -- they are not geometry -- so they carry their own.
    void setHighlightedSource(int index);

    void fitAll();
    void resetView();
    void setRaysVisible(bool visible);
    // Orthographic is the default -- the familiar CAD projection, and what a
    // drawing is read in. Perspective is what makes W/S walking visible, so it
    // is switched on when the user wants to walk through the optic.
    void setPerspective(bool on);

    // Whether the lower-left corner holds the navigation cube or the plain
    // trihedron it replaced. The cube carries its own X/Y/Z axes, so it says
    // everything the trihedron said and can be clicked as well; switching it
    // off puts the trihedron back rather than emptying the corner.
    void setViewCubeVisible(bool on);
    bool viewCubeVisible() const { return m_cubeVisible; }

    // Puts the gizmo on whatever is selected, or takes it off. A mode with
    // nothing selected shows nothing and is remembered for the next selection,
    // which is what makes picking one object after another keep the tool.
    void setTransformMode(TransformMode mode);
    TransformMode transformMode() const { return m_transformMode; }

    // Where the gizmo sits and which way its handles point: the selected
    // object's world placement, with the id of the object it belongs to.
    //
    // The viewport does not know where an object is or how it is turned -- the
    // document does -- and AIS_Manipulator::Attach answers both questions from
    // the bounding box, which is the right answer once and the wrong one after
    // that. The id is what lets the two be told apart: the same object moving
    // is not the same event as a different object being picked.
    void setSelectionFrame(int objectId, const gp_Trsf& world);

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

    // The graphic driver this viewport runs on, so a second view can share the
    // GL resources rather than starting a driver of its own. Null until the
    // viewport has been painted once, which is when the driver is created.
    //
    // Deliberately one-directional: something else may read the viewport's
    // driver, and the viewport reads nothing back.
    Handle(Graphic3d_GraphicDriver) graphicDriver() const;

    // Qt must not paint over the native surface OCCT draws into.
    QPaintEngine* paintEngine() const override { return nullptr; }

signals:
    // A surface was clicked. `index` indexes the vector passed to setScene, or
    // -1 when the click landed on nothing.
    void surfacePicked(int index);
    // An emitter's marker was clicked. `index` indexes the vector last passed
    // to setSourceGlyphs.
    //
    // A source is drawn but not traced, so it has no surface in the scene and
    // nothing for a pick to land on: an emitter was the one thing in the
    // viewport that could be seen and not selected. The disc the overlay
    // already draws at it is now a real face, and this is what it reports.
    void sourcePicked(int index);
    // The selected object was dragged by the gizmo. `delta` is what the drag
    // did, in world coordinates -- not where the object ended up, because the
    // viewport does not know where it started: the document does.
    //
    // Emitted once, when the drag ends. During the drag the presentation is
    // moved directly, which is what makes it follow the cursor; recompiling the
    // scene per frame would put a tessellation between the mouse and the
    // picture.
    void objectTransformed(const gp_Trsf& delta);
    // The gizmo was put away or changed from inside the viewport (Esc, or a
    // G/R/S press), so the buttons that offer the same choice can agree.
    void transformModeChanged(int mode);
    // Something was dragged in from the object library and dropped at `where`.
    // The viewport does not know what an object is; it knows where the cursor
    // was in three dimensions, which is the part only it can answer.
    void objectDropped(const QString& typeKey, const gp_Pnt& where);
    // The Delete key was pressed while an object was selected. `objectId` is
    // the selected object's id, as the document knows it -- the viewport does
    // not remove anything itself, because it does not own the document. The
    // owner decides whether to confirm and what to remove.
    void deleteRequested(int objectId);

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
    // Whether the overlay on screen already draws what the members below say.
    // The glyphs are sized as a fraction of the scene, so redrawing them every
    // time any part of it changes size makes the emitter marker and the
    // receiver outline twitch while a completely unrelated object is being
    // edited -- and the user reads that as "everything updated".
    bool overlayUpToDate() const;
    // The clickable disc at each emitter, from the circles rebuildOverlay just
    // drew, so the pickable thing and the visible thing cannot drift apart.
    void rebuildSourceMarkers();
    void applySourceHighlight();
    // Puts the gizmo on the current selection, or takes it off. Called whenever
    // either of those could have changed: the selection, the mode, or the
    // presentations the gizmo is attached to.
    void refreshManipulator();
    // The presentations the gizmo should be attached to, which is the selected
    // bodies -- or the selected emitter's marker, an emitter having no body.
    Handle(AIS_ManipulatorObjectSequence) manipulatorTargets() const;
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
    Handle(AIS_Manipulator)        m_manipulator;
    TransformMode                  m_transformMode = TransformMode::None;
    // Whether the press that started this drag landed on a gizmo handle. While
    // it did, the drag moves the object and must not orbit the camera.
    bool                           m_draggingGizmo = false;
    // What the drag has done so far, as the manipulator reports it. Kept
    // because StopTransform does not hand it back.
    gp_Trsf                        m_gizmoDelta;
    // The selected object's world placement, as the document has it, and which
    // object that is.
    gp_Trsf                        m_selectionFrame;
    int                            m_selectionId = 0;
    // Where on the object the handles sit, in the object's own coordinates.
    //
    // Taken from the middle of the body the first time the gizmo goes on it and
    // then kept, because it has to be a point *of the object*: an axis-aligned
    // bounding box round a turned body has a different centre than one round
    // the body before it turned, so re-measuring it after every drag walked the
    // gizmo off the thing it belongs to.
    gp_Pnt                         m_gizmoAnchor{0.0, 0.0, 0.0};
    int                            m_gizmoAnchorId = 0;
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
        bool operator==(const Appearance& o) const {
            return colour.IsEqual(o.colour) && transparency == o.transparency;
        }
        bool operator!=(const Appearance& o) const { return !(*this == o); }
    };
    std::vector<Appearance> m_baseLook;
    // What is actually set on each presentation right now, highlight included.
    // A surface whose shape and colour are both unchanged is not redisplayed at
    // all: recomputing every presentation in the scene because one lens grew a
    // millimetre is most of what made an edit feel like a rebuild.
    std::vector<Appearance> m_applied;
    // The shape each presentation is currently showing. compile() hands back
    // the same B-Rep for an object nobody touched, so this compares equal for
    // everything but the object being edited.
    std::vector<TopoDS_Shape> m_shown;
    bool            m_overlaysOn  = true;
    bool            m_cubeVisible = true;
    // Whether the last hover landed on the cube. A press there must not start
    // an orbit, or dragging off a facet would turn the scene instead of doing
    // nothing.
    bool            m_hoverOnCube = false;
    std::vector<SourceGlyph> m_sources;
    // Where the overlay drew each emitter's circle: its centre, its normal and
    // its radius, recorded as it is drawn.
    struct SourceDisc {
        gp_Pnt centre;
        gp_Dir normal{0, 0, 1};
        double radius = 0.0;
    };
    std::vector<SourceDisc>        m_sourceDiscs;
    std::vector<Handle(AIS_Shape)> m_sourceMarkers;
    int                            m_highlightSource = -1;
    // The receivers, as the frames the mesher gave them, so the overlay can
    // outline them and draw their acceptance cones.
    struct ReceiverGlyph {
        gp_Pnt centre;
        gp_Dir u, v, n;
        double w = 0.0, h = 0.0;
        double acceptanceDeg = 180.0;
        // Which surface it outlines. The outline is a picture *of* that body,
        // so it goes when the body does -- otherwise switching a receiver off
        // erases the plane and leaves its green rectangle hanging in the air
        // until the next rebuild catches up.
        int    surface = -1;
    };
    std::vector<ReceiverGlyph> m_receivers;
    // The receivers whose surface is currently drawn. This is what the overlay
    // is built from and compared against.
    std::vector<ReceiverGlyph> shownReceivers() const;
    // The scene size the overlay is drawn against. Held rather than tracked:
    // it only follows m_sceneSize once the scene has genuinely changed size, so
    // nudging a dimension does not resize every marker in the viewport.
    double          m_overlayScale = 0.0;
    // What the overlay currently on screen was drawn from.
    std::vector<SourceGlyph>   m_drawnSources;
    std::vector<ReceiverGlyph> m_drawnReceivers;
    double          m_drawnScale     = -1.0;
    double          m_drawnBbMin[3]  = {0, 0, 0};
    double          m_drawnBbMax[3]  = {0, 0, 0};
    bool            m_drawnOverlaysOn = false;
    bool            m_haveOverlay     = false;
    bool            m_clipOn      = false;
    int             m_clipAxis    = 0;
    double          m_clipPos     = 0.5;
    bool            m_clipFlipped = false;
};
