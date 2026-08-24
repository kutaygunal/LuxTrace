#include "OcctViewWidget.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QString>
#include <QTimer>
#include <QWheelEvent>

#include <Aspect_DisplayConnection.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_Group.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Poly_Triangulation.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Quantity_Color.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <SelectMgr_Selection.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <WNT_Window.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Ray paths as a single AIS object.
//
// One AIS_Shape per segment would mean tens of thousands of presentations; a
// Graphic3d_ArrayOfSegments hands the whole set to the GPU as one primitive
// array instead. Colours travel with the vertices in that same array, so
// recolouring by energy or bounce count costs nothing extra at draw time.
// ---------------------------------------------------------------------------
class RayCloud : public AIS_InteractiveObject {
    DEFINE_STANDARD_RTTIEXT(RayCloud, AIS_InteractiveObject)
public:
    struct Vertex {
        gp_Pnt         p;
        Quantity_Color c;
    };

    explicit RayCloud(std::vector<Vertex> vertices) : m_verts(std::move(vertices)) {}

protected:
    void Compute(const Handle(PrsMgr_PresentationManager)&,
                 const Handle(Prs3d_Presentation)& thePrs,
                 const Standard_Integer) override {
        if (m_verts.empty()) return;
        Handle(Graphic3d_ArrayOfSegments) arr =
            new Graphic3d_ArrayOfSegments(Standard_Integer(m_verts.size()), 0,
                                          Graphic3d_ArrayFlags_VertexColor);
        for (const Vertex& v : m_verts) arr->AddVertex(v.p, v.c);
        Handle(Graphic3d_Group) group = thePrs->NewGroup();
        group->SetGroupPrimitivesAspect(
            new Graphic3d_AspectLine3d(Quantity_Color(1.0, 0.78, 0.35, Quantity_TOC_RGB),
                                       Aspect_TOL_SOLID, 1.0));
        group->AddPrimitiveArray(arr);
    }

    // Deliberately empty: rays are a visual overlay, and making tens of
    // thousands of segments selectable would both cost a large sensitive-entity
    // tree and put a wall of pickable lines in front of the optics, which are
    // the things worth clicking on.
    void ComputeSelection(const Handle(SelectMgr_Selection)&,
                          const Standard_Integer) override {}

private:
    std::vector<Vertex> m_verts;
};

IMPLEMENT_STANDARD_RTTIEXT(RayCloud, AIS_InteractiveObject)

// ---------------------------------------------------------------------------

namespace {

constexpr double kWalkFractionPerSecond = 0.55;   // of the scene size
constexpr double kLookRadiansPerPixel   = 0.005;
constexpr std::size_t kMaxDisplayedSegments = 6000;
constexpr int kClickSlopPixels = 4;   // a drag beyond this is not a click

// Whether the mesher has already been over this shape. Geometry that reaches
// the viewport through the scene cache carries the tracer's own triangulation;
// geometry straight out of a CAD file does not. The first face answers for the
// whole shape -- the mesher goes over all of them or none.
bool isTessellated(const TopoDS_Shape& shape) {
    TopExp_Explorer ex(shape, TopAbs_FACE);
    if (!ex.More()) return false;
    TopLoc_Location loc;
    return !BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc).IsNull();
}

// A ray colour ramp: cool where energy is low, hot where it is high.
Quantity_Color energyColor(double t) {
    t = std::clamp(t, 0.0, 1.0);
    // deep blue -> magenta -> orange -> white
    const double r = std::clamp(0.15 + 1.5 * t, 0.0, 1.0);
    const double g = std::clamp(-0.25 + 1.35 * t * t, 0.0, 1.0);
    const double b = std::clamp(0.85 - 1.1 * t + 0.55 * t * t * t, 0.0, 1.0);
    return Quantity_Color(r, g, b, Quantity_TOC_RGB);
}

Quantity_Color bounceColor(int depth, int maxDepth) {
    const double t = maxDepth > 0 ? std::clamp(double(depth) / double(maxDepth), 0.0, 1.0) : 0.0;
    // green (fresh) -> yellow -> red (many interactions)
    return Quantity_Color(std::clamp(2.0 * t, 0.0, 1.0),
                          std::clamp(2.0 - 2.0 * t, 0.0, 1.0) * 0.85 + 0.15 * (1.0 - t),
                          0.15, Quantity_TOC_RGB);
}

// Approximate sRGB for a visible wavelength, so a spectral trace draws in the
// colour it actually is.
Quantity_Color wavelengthColor(double nm) {
    double r = 0.0, g = 0.0, b = 0.0;
    if (nm < 440.0)      { r = -(nm - 440.0) / 60.0; b = 1.0; }
    else if (nm < 490.0) { g = (nm - 440.0) / 50.0;  b = 1.0; }
    else if (nm < 510.0) { g = 1.0; b = -(nm - 510.0) / 20.0; }
    else if (nm < 580.0) { r = (nm - 510.0) / 70.0;  g = 1.0; }
    else if (nm < 645.0) { r = 1.0; g = -(nm - 645.0) / 65.0; }
    else                 { r = 1.0; }
    // Lift the floor: a pure spectral colour rendered faithfully is very dark
    // at the ends of the band, and an invisible ray is not informative.
    return Quantity_Color(0.15 + 0.85 * std::clamp(r, 0.0, 1.0),
                          0.15 + 0.85 * std::clamp(g, 0.0, 1.0),
                          0.15 + 0.85 * std::clamp(b, 0.0, 1.0), Quantity_TOC_RGB);
}

} // namespace

OcctViewWidget::OcctViewWidget(QWidget* parent) : QWidget(parent) {
    // OCCT renders into this widget's native window directly, so Qt must not
    // double-buffer or clear it.
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_NativeWindow);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &OcctViewWidget::stepWalk);
    timer->start(16);                 // ~60 Hz while keys are held
    m_walkClock.start();
}

OcctViewWidget::~OcctViewWidget() {
    if (!m_view.IsNull()) m_view->Remove();
}

void OcctViewWidget::initViewer() {
    if (!m_view.IsNull() || m_initFailed) return;

    try {
        Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
        Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(display);
        driver->ChangeOptions().swapInterval = 1;

        m_viewer = new V3d_Viewer(driver);
        m_viewer->SetDefaultLights();
        m_viewer->SetLightOn();
        m_context = new AIS_InteractiveContext(m_viewer);

        m_view = m_viewer->CreateView();
        Handle(WNT_Window) window = new WNT_Window(reinterpret_cast<Aspect_Drawable>(winId()));
        m_view->SetWindow(window);
        if (!window->IsMapped()) window->Map();

        m_view->SetBgGradientColors(Quantity_Color(0.10, 0.11, 0.14, Quantity_TOC_RGB),
                                    Quantity_Color(0.02, 0.02, 0.03, Quantity_TOC_RGB),
                                    Aspect_GFM_VER);
        m_view->ChangeRenderingParams().NbMsaaSamples = 4;
        // Perspective, not OCCT's default orthographic camera. Under an
        // orthographic projection, moving the eye and the centre together along
        // the view axis changes nothing on screen, so W/S would appear dead --
        // a walkthrough only means anything with a perspective frustum.
        m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Perspective);
        m_view->Camera()->SetFOVy(50.0);
        m_view->SetProj(V3d_XposYnegZpos);
        m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.08,
                                V3d_ZBUFFER);
        m_view->MustBeResized();
    } catch (const Standard_Failure& e) {
        // A machine without a usable OpenGL driver should not take the app down;
        // the rest of the UI keeps working without the 3D pane.
        m_initFailed = true;
        qWarning("OCCT 3D viewer unavailable: %s", e.GetMessageString());
    }
}

double OcctViewWidget::sceneScale() const {
    return m_sceneSize > 1e-9 ? m_sceneSize : 100.0;
}

void OcctViewWidget::setScene(GeometryProvider::Scene scene,
                              const std::vector<OpticalSurface>& surfaces) {
    initViewer();
    if (m_context.IsNull()) return;

    // Editing a dimension is not the same event as choosing a different optic.
    // The camera used to be thrown back to its default on both, so nudging a
    // focal length threw away whatever the user had lined up to look at; the
    // view is only reframed when what is on screen is genuinely a new subject.
    const int  key          = int(scene);
    const bool sceneChanged = !m_haveScene || key != m_sceneKey;

    // Same part count means the same parts with new dimensions, so the
    // presentations are reused and handed the new B-Rep in place. Tearing the
    // interactive context down and repopulating it costs a fresh presentation,
    // a fresh selection tree and a fresh structure per surface, all of which
    // are thrown away again on the next spin-box step.
    if (m_shapes.size() != surfaces.size()) {
        for (const auto& s : m_shapes) m_context->Remove(s, Standard_False);
        m_shapes.clear();
        m_shapes.reserve(surfaces.size());
    }

    Bnd_Box bounds;
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        const OpticalSurface& os    = surfaces[i];
        const bool            fresh = i >= m_shapes.size();

        Handle(AIS_Shape) shape;
        if (fresh) {
            shape = new AIS_Shape(os.shape);
        } else {
            shape = m_shapes[i];
            shape->SetShape(os.shape);
        }

        if (os.isDetector) {
            shape->SetColor(Quantity_Color(0.25, 0.85, 0.45, Quantity_TOC_RGB));
            shape->SetTransparency(0.55f);
        } else if (os.scatter > 0.5 && os.index <= 0.0) {
            // A matte white cavity: paint it as one, so an integrating sphere
            // does not read as a mirror.
            shape->SetColor(Quantity_Color(0.92, 0.92, 0.88, Quantity_TOC_RGB));
            shape->SetTransparency(0.35f);
        } else if (os.index > 0.0) {
            // Refractive solids: glassy and see-through, so rays stay visible.
            shape->SetColor(Quantity_Color(0.45, 0.72, 0.95, Quantity_TOC_RGB));
            shape->SetTransparency(0.65f);
        } else {
            shape->SetColor(Quantity_Color(0.78, 0.78, 0.82, Quantity_TOC_RGB));
            shape->SetTransparency(0.15f);
        }

        // Where the mesher has already been over this shape, say its tolerance
        // in absolute terms so OCCT reuses the triangulation that is already
        // there. The drawer's default deflection is a fraction of the bounding
        // box and never matches what the mesher used, so every rebuild used to
        // tessellate the optic a second time on the GUI thread purely in order
        // to draw it. Drawing the same facets the trace intersects is also the
        // more honest picture.
        //
        // Geometry that has not been meshed -- a part fresh out of a CAD file,
        // shown before anything is traced -- keeps the relative default, which
        // scales itself to the part and so stays smooth on a small one.
        if (isTessellated(os.shape)) {
            const Handle(Prs3d_Drawer)& drawer = shape->Attributes();
            drawer->SetTypeOfDeflection(Aspect_TOD_ABSOLUTE);
            drawer->SetMaximalChordialDeviation(os.meshDeflection);
            drawer->SetDeviationAngle(os.meshAngle);
        }

        shape->SetDisplayMode(AIS_Shaded);
        if (fresh) {
            // Selection mode 0 is the whole shape: that is what makes a surface
            // clickable, and it is why the ray cloud deliberately has none.
            m_context->Display(shape, AIS_Shaded, 0, Standard_False);
            m_shapes.push_back(shape);
        } else {
            m_context->Redisplay(shape, Standard_False, Standard_False);
        }

        BRepBndLib::Add(os.shape, bounds);
    }

    if (!bounds.IsVoid()) {
        double xm, ym, zm, xM, yM, zM;
        bounds.Get(xm, ym, zm, xM, yM, zM);
        m_bbMin[0] = xm; m_bbMin[1] = ym; m_bbMin[2] = zm;
        m_bbMax[0] = xM; m_bbMax[1] = yM; m_bbMax[2] = zM;
        m_sceneSize = std::max({xM - xm, yM - ym, zM - zm});
    }

    m_sceneKey  = key;
    m_haveScene = true;

    applyClip();
    if (sceneChanged) {
        resetView();
    } else if (!m_view.IsNull()) {
        // Depth range only: the optic may have grown past the old near/far
        // planes. Unlike FitAll this leaves the eye, the target and the zoom
        // exactly where the user put them.
        m_view->ZFitAll();
        m_view->Invalidate();
        update();
    }
}

void OcctViewWidget::setRays(const std::vector<RaySegment>& segments) {
    m_segments = segments;
    rebuildRays();
}

void OcctViewWidget::setRayColorMode(RayColor mode) {
    if (m_rayColor == mode) return;
    m_rayColor = mode;
    rebuildRays();
}

void OcctViewWidget::setDetectorRaysOnly(bool on) {
    if (m_detectorOnly == on) return;
    m_detectorOnly = on;
    rebuildRays();
}

void OcctViewWidget::rebuildRays() {
    initViewer();
    if (m_context.IsNull()) return;

    if (!m_rays.IsNull()) {
        m_context->Remove(m_rays, Standard_False);
        m_rays.Nullify();
    }

    // Filter first, then thin: thinning first would spend most of the budget on
    // rays the filter is about to discard, and a 2 %-efficient scene would draw
    // almost nothing.
    std::vector<const RaySegment*> kept;
    kept.reserve(m_segments.size());
    for (const RaySegment& s : m_segments)
        if (!m_detectorOnly || s.reachedDetector()) kept.push_back(&s);

    if (!kept.empty()) {
        // A full bundle draws as a solid tube that hides the optic it is
        // bouncing off. Thinning it keeps the beam shape readable and lets the
        // geometry show through.
        const std::size_t stride = std::max<std::size_t>(1, kept.size() / kMaxDisplayedSegments);

        double maxEnergy = 0.0;
        int    maxDepth  = 1;
        for (std::size_t i = 0; i < kept.size(); i += stride) {
            maxEnergy = std::max(maxEnergy, double(kept[i]->energy));
            maxDepth  = std::max(maxDepth, int(kept[i]->depth));
        }
        if (maxEnergy <= 0.0) maxEnergy = 1.0;

        std::vector<RayCloud::Vertex> verts;
        verts.reserve(2 * (kept.size() / stride + 1));
        for (std::size_t i = 0; i < kept.size(); i += stride) {
            const RaySegment& s = *kept[i];
            Quantity_Color c(1.0, 0.78, 0.35, Quantity_TOC_RGB);
            switch (m_rayColor) {
            case RayColor::Energy:
                // Against the brightest leg on screen rather than against 1.0:
                // most scenes lose the bulk of their light immediately, and an
                // absolute scale would render every remaining ray the same dark.
                c = energyColor(double(s.energy) / maxEnergy);
                break;
            case RayColor::Bounces:
                c = bounceColor(int(s.depth), maxDepth);
                break;
            case RayColor::Wavelength:
                c = wavelengthColor(double(s.wavelengthNm));
                break;
            case RayColor::Uniform:
                break;
            }
            verts.push_back({gp_Pnt(s.a.x, s.a.y, s.a.z), c});
            verts.push_back({gp_Pnt(s.b.x, s.b.y, s.b.z), c});
        }

        m_rays = new RayCloud(std::move(verts));
        m_context->Display(m_rays, 0, -1, Standard_False);
        if (!m_raysVisible) m_context->Erase(m_rays, Standard_False);
    }

    if (!m_view.IsNull()) {
        m_view->Invalidate();
        update();
    }
}

void OcctViewWidget::setRaysVisible(bool visible) {
    m_raysVisible = visible;
    if (m_context.IsNull() || m_rays.IsNull()) return;
    if (visible) m_context->Display(m_rays, 0, -1, Standard_False);
    else         m_context->Erase(m_rays, Standard_False);
    m_view->Invalidate();
    update();
}

// ---- clipping --------------------------------------------------------------

void OcctViewWidget::setClipEnabled(bool on)      { m_clipOn = on;           applyClip(); }
void OcctViewWidget::setClipAxis(int axis)        { m_clipAxis = std::clamp(axis, 0, 2); applyClip(); }
void OcctViewWidget::setClipPosition(double t)    { m_clipPos = std::clamp(t, 0.0, 1.0); applyClip(); }
void OcctViewWidget::setClipFlipped(bool flipped) { m_clipFlipped = flipped; applyClip(); }

void OcctViewWidget::applyClip() {
    initViewer();
    if (m_view.IsNull()) return;

    if (!m_clip.IsNull()) {
        m_view->RemoveClipPlane(m_clip);
        m_clip.Nullify();
    }
    if (m_clipOn) {
        // A little slack past each end, so the extreme positions of the slider
        // fully open the section rather than shaving the last triangle.
        const double lo = m_bbMin[m_clipAxis], hi = m_bbMax[m_clipAxis];
        const double pad = 0.02 * std::max(1e-6, hi - lo);
        const double at = (lo - pad) + m_clipPos * ((hi + pad) - (lo - pad));

        gp_Dir normal(m_clipAxis == 0 ? 1 : 0, m_clipAxis == 1 ? 1 : 0, m_clipAxis == 2 ? 1 : 0);
        if (m_clipFlipped) normal.Reverse();
        gp_Pnt origin(m_clipAxis == 0 ? at : 0.0,
                      m_clipAxis == 1 ? at : 0.0,
                      m_clipAxis == 2 ? at : 0.0);
        m_clip = new Graphic3d_ClipPlane(gp_Pln(origin, normal));
        m_clip->SetCapping(Standard_True);
        m_clip->SetCappingColor(Quantity_Color(0.35, 0.36, 0.42, Quantity_TOC_RGB));
        m_clip->SetOn(Standard_True);
        m_view->AddClipPlane(m_clip);
    }
    m_view->Invalidate();
    update();
}

// ---- picking ---------------------------------------------------------------

void OcctViewWidget::pickAt(const QPoint& pos) {
    if (m_context.IsNull() || m_view.IsNull()) return;
    m_context->MoveTo(pos.x(), pos.y(), m_view, Standard_False);

    // NOTE: OCCT 7.8's DetectedInteractive() dereferences its internal
    // myLastPicked with no null check, so clicking empty space crashes inside
    // the library. HasDetected()/DetectedOwner() are the null-safe accessors:
    // guard first, then resolve the owner's selectable ourselves.
    int found = -1;
    if (m_context->HasDetected()) {
        const Handle(SelectMgr_EntityOwner)& owner = m_context->DetectedOwner();
        Handle(AIS_InteractiveObject) detected;
        if (!owner.IsNull()) {
            const Handle(Standard_Transient)& sel = owner->Selectable();
            detected = Handle(AIS_InteractiveObject)::DownCast(sel);
        }
        if (!detected.IsNull())
            for (std::size_t i = 0; i < m_shapes.size(); ++i)
                if (m_shapes[i] == detected) { found = int(i); break; }
    }

    m_context->ClearSelected(Standard_False);
    if (found >= 0) m_context->SelectDetected();

    m_view->Invalidate();
    update();
    emit surfacePicked(found);
}

// ---- painting --------------------------------------------------------------

void OcctViewWidget::fitAll() {
    if (m_view.IsNull()) return;
    m_view->FitAll(0.05, Standard_False);
    m_view->ZFitAll();
    m_view->Invalidate();
    update();
}

void OcctViewWidget::setPerspective(bool on) {
    if (m_view.IsNull()) return;
    m_view->Camera()->SetProjectionType(on ? Graphic3d_Camera::Projection_Perspective
                                           : Graphic3d_Camera::Projection_Orthographic);
    fitAll();
}

void OcctViewWidget::resetView() {
    if (m_view.IsNull()) return;
    m_view->SetProj(V3d_XposYnegZpos);
    m_view->SetTwist(0.0);
    fitAll();
}

bool OcctViewWidget::saveImage(const QString& path) {
    if (m_view.IsNull()) return false;
    m_view->Redraw();
    return m_view->Dump(path.toUtf8().constData()) == Standard_True;
}

void OcctViewWidget::paintEvent(QPaintEvent*) {
    initViewer();
    if (m_view.IsNull()) return;
    m_view->Redraw();
}

void OcctViewWidget::resizeEvent(QResizeEvent*) {
    if (m_view.IsNull()) return;
    m_view->MustBeResized();
}

// ---- navigation ------------------------------------------------------------

void OcctViewWidget::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    if (m_view.IsNull()) return;
    m_lastMouse   = e->pos();
    m_pressPos    = e->pos();
    m_dragButton  = e->button();
    if (m_dragButton == Qt::LeftButton)
        m_view->StartRotation(e->pos().x(), e->pos().y());
}

void OcctViewWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_view.IsNull() || m_dragButton == Qt::NoButton) return;
    const QPoint delta = e->pos() - m_lastMouse;

    switch (m_dragButton) {
    case Qt::LeftButton:      // orbit around the scene
        m_view->Rotation(e->pos().x(), e->pos().y());
        break;
    case Qt::MiddleButton:    // pan
        m_view->Pan(delta.x(), -delta.y());
        break;
    case Qt::RightButton:     // free look, for walking through the optic
        freeLook(-delta.x() * kLookRadiansPerPixel, -delta.y() * kLookRadiansPerPixel);
        break;
    default:
        break;
    }

    m_lastMouse = e->pos();
    m_view->Invalidate();
    update();
}

void OcctViewWidget::mouseReleaseEvent(QMouseEvent* e) {
    const Qt::MouseButton button = m_dragButton;
    m_dragButton = Qt::NoButton;

    // A left press that did not really move is a click, not an orbit: that is
    // what distinguishes picking a surface from turning the scene, without
    // spending a modifier key on it.
    if (button == Qt::LeftButton) {
        const QPoint d = e->pos() - m_pressPos;
        if (std::abs(d.x()) <= kClickSlopPixels && std::abs(d.y()) <= kClickSlopPixels)
            pickAt(e->pos());
    }
}

void OcctViewWidget::wheelEvent(QWheelEvent* e) {
    if (m_view.IsNull()) return;
    const int steps = e->angleDelta().y();
    if (steps == 0) return;

    // Zoom toward the cursor rather than the view centre.
    const QPoint pos = e->position().toPoint();
    m_view->StartZoomAtPoint(pos.x(), pos.y());
    const double factor = steps > 0 ? 1.0 : -1.0;
    const int span = std::max(8, width() / 12);
    m_view->ZoomAtPoint(0, 0, int(factor * span), 0);
    m_view->Invalidate();
    update();
}

void OcctViewWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_F: fitAll();    return;
    case Qt::Key_R: resetView(); return;
    default: break;
    }
    if (!e->isAutoRepeat()) m_keysDown.insert(e->key());
    QWidget::keyPressEvent(e);
}

void OcctViewWidget::keyReleaseEvent(QKeyEvent* e) {
    if (!e->isAutoRepeat()) m_keysDown.remove(e->key());
    QWidget::keyReleaseEvent(e);
}

void OcctViewWidget::focusOutEvent(QFocusEvent* e) {
    // Otherwise a key held while the window loses focus walks forever.
    m_keysDown.clear();
    QWidget::focusOutEvent(e);
}

void OcctViewWidget::stepWalk() {
    const double seconds = double(m_walkClock.restart()) / 1000.0;
    if (m_view.IsNull() || m_keysDown.isEmpty()) return;
    applyWalk(std::min(seconds, 0.1));   // clamp, so a stall is not a teleport
}

void OcctViewWidget::applyWalk(double seconds) {
    const Handle(Graphic3d_Camera)& cam = m_view->Camera();

    gp_Dir forward = cam->Direction();
    gp_Dir up      = cam->Up();
    gp_Vec right   = gp_Vec(forward).Crossed(gp_Vec(up));

    gp_Vec move(0, 0, 0);
    if (m_keysDown.contains(Qt::Key_W)) move += gp_Vec(forward);
    if (m_keysDown.contains(Qt::Key_S)) move -= gp_Vec(forward);
    if (m_keysDown.contains(Qt::Key_D)) move += right;
    if (m_keysDown.contains(Qt::Key_A)) move -= right;
    if (m_keysDown.contains(Qt::Key_E)) move += gp_Vec(up);
    if (m_keysDown.contains(Qt::Key_Q)) move -= gp_Vec(up);
    if (move.SquareMagnitude() < 1e-18) return;

    move.Normalize();
    double speed = sceneScale() * kWalkFractionPerSecond;
    const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
    if (mods & Qt::ShiftModifier)   speed *= 4.0;
    if (mods & Qt::ControlModifier) speed *= 0.25;

    const gp_Vec step = move * (speed * seconds);
    cam->SetEye(cam->Eye().Translated(step));
    cam->SetCenter(cam->Center().Translated(step));

    m_view->ZFitAll();          // keep near/far planes around the new position
    m_view->Invalidate();
    update();
}

void OcctViewWidget::freeLook(double dYaw, double dPitch) {
    const Handle(Graphic3d_Camera)& cam = m_view->Camera();

    const gp_Pnt eye     = cam->Eye();
    const gp_Dir forward = cam->Direction();
    const gp_Dir up      = cam->Up();
    const gp_Dir right   = forward.Crossed(up);

    // Yaw about world Z keeps the horizon level, which is what makes a
    // walkthrough feel right; pitch is about the camera's own right axis.
    gp_Trsf yaw;
    yaw.SetRotation(gp_Ax1(eye, gp_Dir(0, 0, 1)), dYaw);
    gp_Trsf pitch;
    pitch.SetRotation(gp_Ax1(eye, right), dPitch);

    gp_Dir newForward = forward.Transformed(yaw * pitch);

    // Re-level the up vector against world Z, so repeated turns cannot
    // accumulate roll. Near the poles there is no horizon to level against, so
    // the previous up is kept.
    gp_Dir newUp = up.Transformed(yaw * pitch);
    const gp_Dir worldZ(0, 0, 1);
    if (std::fabs(newForward.Dot(worldZ)) < 0.999) {
        const gp_Vec levelRight = gp_Vec(newForward).Crossed(gp_Vec(worldZ));
        gp_Vec levelUp = levelRight.Crossed(gp_Vec(newForward));
        if (levelUp.SquareMagnitude() > 1e-18) {
            levelUp.Normalize();
            if (levelUp.Z() < 0.0) levelUp.Reverse();
            newUp = gp_Dir(levelUp);
        }
    }

    const double distance = std::max(cam->Distance(), 1e-6);
    cam->SetCenter(eye.Translated(gp_Vec(newForward) * distance));
    cam->SetUp(newUp);
    m_view->ZFitAll();
}
