#pragma once
#include <QElapsedTimer>
#include <QSet>
#include <QWidget>
#include <vector>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
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

    // Replaces the displayed geometry. Cheap to call on every scene change.
    void setScene(GeometryProvider::Scene scene,
                  const std::vector<OpticalSurface>& surfaces);
    // Replaces the displayed ray paths (pass an empty vector to clear them).
    void setRays(const std::vector<RaySegment>& segments);

    void fitAll();
    void resetView();
    void setRaysVisible(bool visible);
    // Perspective is the default; orthographic is the familiar CAD projection,
    // but W/S walking has no visible effect in it.
    void setPerspective(bool on);

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

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
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
    void applyClip();
    void pickAt(const QPoint& pos);
    double sceneScale() const;

    Handle(V3d_Viewer)             m_viewer;
    Handle(V3d_View)               m_view;
    Handle(AIS_InteractiveContext) m_context;
    std::vector<Handle(AIS_Shape)> m_shapes;
    Handle(RayCloud)               m_rays;
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
    bool            m_raysVisible = true;
    bool            m_initFailed  = false;
    RayColor        m_rayColor    = RayColor::Energy;
    bool            m_detectorOnly = false;
    bool            m_clipOn      = false;
    int             m_clipAxis    = 0;
    double          m_clipPos     = 0.5;
    bool            m_clipFlipped = false;
};
