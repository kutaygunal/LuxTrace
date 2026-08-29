#include "OcctViewWidget.h"

#include <QDebug>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QString>
#include <QTimer>
#include <QWheelEvent>

#include "core/SceneDocument.h"

#include <AIS_AnimationCamera.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_SequenceOfHClipPlane.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Poly_Triangulation.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_Text.hxx>
#include <Prs3d_TextAspect.hxx>
#include <TCollection_ExtendedString.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Quantity_Color.hxx>
#include <gp_Ax2.hxx>
#include <gp_Quaternion.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <SelectMgr_Selection.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <WNT_Window.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cstdio>
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

    // A word anchored in the scene. Only the overlay uses these: a ray cloud
    // has nothing to say, and a scene that names its own source and receiver
    // needs no documentation to be read.
    struct Label {
        gp_Pnt         at;
        QString        text;
        Quantity_Color colour;
    };

    explicit RayCloud(std::vector<Vertex> vertices, std::vector<Label> labels = {})
        : m_verts(std::move(vertices)), m_labels(std::move(labels)) {}

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

        for (const Label& l : m_labels) {
            Handle(Prs3d_TextAspect) aspect = new Prs3d_TextAspect();
            aspect->SetColor(l.colour);
            aspect->SetHeight(13.0);
            Handle(Graphic3d_Group) tg = thePrs->NewGroup();
            Prs3d_Text::Draw(tg, aspect,
                             TCollection_ExtendedString(l.text.toStdU16String().c_str()),
                             l.at);
        }
    }

    // Deliberately empty: rays are a visual overlay, and making tens of
    // thousands of segments selectable would both cost a large sensitive-entity
    // tree and put a wall of pickable lines in front of the optics, which are
    // the things worth clicking on.
    void ComputeSelection(const Handle(SelectMgr_Selection)&,
                          const Standard_Integer) override {}

private:
    std::vector<Vertex> m_verts;
    std::vector<Label>  m_labels;
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
    // The viewport is where an object from the library becomes a position.
    setAcceptDrops(true);

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
        // Orthographic, which is OCCT's default and the projection a CAD user
        // reads a part in: parallel edges stay parallel and a dimension is the
        // same length wherever it sits on screen. The Perspective box switches
        // the frustum on, which is what makes W/S walking visible -- under an
        // orthographic projection, moving the eye and the centre together along
        // the view axis changes nothing on screen, so W/S appears dead.
        // The FOV is set here so the box has one ready when it is ticked.
        m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
        m_view->Camera()->SetFOVy(50.0);
        m_view->SetProj(V3d_XposYnegZpos);

        // The navigation cube stands in the lower-left corner, where the plain
        // trihedron used to be. It says everything the trihedron said -- it
        // carries its own X/Y/Z axes -- and it can be clicked, so the corner
        // stops being a read-only badge and becomes the control it looks like.
        buildViewCube();
        applyCornerWidget();
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
                              const std::vector<OpticalSurface>& surfaces,
                              bool keepCamera) {
    initViewer();
    if (m_context.IsNull()) return;

    // Editing a dimension is not the same event as choosing a different optic.
    // The camera used to be thrown back to its default on both, so nudging a
    // focal length threw away whatever the user had lined up to look at; the
    // view is only reframed when what is on screen is genuinely a new subject.
    const int  key          = int(scene);
    // The very first scene is always framed -- there is no viewpoint to keep
    // yet, and an unframed camera shows an empty screen.
    const bool sceneChanged = !m_haveScene || (key != m_sceneKey && !keepCamera);

    // Same part count means the same parts with new dimensions, so the
    // presentations are reused and handed the new B-Rep in place. Tearing the
    // interactive context down and repopulating it costs a fresh presentation,
    // a fresh selection tree and a fresh structure per surface, all of which
    // are thrown away again on the next spin-box step.
    if (m_shapes.size() != surfaces.size()) {
        // The gizmo holds handles to these. Letting it go first is what keeps
        // it from being attached to a presentation the context no longer has.
        if (!m_manipulator.IsNull() && m_manipulator->IsAttached()) {
            if (m_manipulator->HasActiveTransformation())
                m_manipulator->StopTransform(Standard_False);
            m_manipulator->DeactivateCurrentMode();
            m_manipulator->Detach();
        }
        m_draggingGizmo = false;
        for (const auto& s : m_shapes) m_context->Remove(s, Standard_False);
        m_shapes.clear();
        m_baseLook.clear();
        m_applied.clear();
        m_shown.clear();
        m_shapes.reserve(surfaces.size());
        // A different set of parts is a different set of things to hide, so the
        // visibility flags start again with them.
        m_visible.assign(surfaces.size(), true);
        m_highlight.clear();
    }
    if (m_visible.size() != surfaces.size()) m_visible.assign(surfaces.size(), true);

    // The receivers, remembered as frames so the overlay can outline them and
    // draw their acceptance cones.
    m_receivers.clear();
    for (const OpticalSurface& os : surfaces) {
        if (!os.isDetector) continue;
        // The mesher owns the receiver rectangle, so this is what MeshBuilder
        // would derive: the shape's own bounding box in the surface's frame.
        Bnd_Box b;
        BRepBndLib::Add(os.shape, b);
        if (b.IsVoid()) continue;
        double xm, ym, zm, xM, yM, zM;
        b.Get(xm, ym, zm, xM, yM, zM);
        ReceiverGlyph g;
        g.centre = gp_Pnt(0.5 * (xm + xM), 0.5 * (ym + yM), 0.5 * (zm + zM));
        g.u = gp_Dir(1, 0, 0);
        g.v = gp_Dir(0, 1, 0);
        g.n = gp_Dir(0, 0, 1);
        g.w = xM - xm;
        g.h = yM - ym;
        g.acceptanceDeg = os.detAcceptanceDeg;
        m_receivers.push_back(g);
    }

    // Which surfaces genuinely changed. An edit to one object recompiles the
    // whole document, but the document hands back the same B-Rep for every
    // object it did not rebuild, so this is usually a single index.
    bool anyChanged = false;

    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        const OpticalSurface& os    = surfaces[i];
        const bool            fresh = i >= m_shapes.size();
        const bool sameShape = !fresh && i < m_shown.size() && m_shown[i].IsEqual(os.shape);

        Handle(AIS_Shape) shape;
        if (fresh) {
            shape = new AIS_Shape(os.shape);
        } else {
            shape = m_shapes[i];
            if (!sameShape) shape->SetShape(os.shape);
        }

        // What this surface looks like when nothing has it selected. Kept, so
        // that clearing a highlight has something to put back.
        Appearance look;
        if (os.isDetector) {
            look = {Quantity_Color(0.25, 0.85, 0.45, Quantity_TOC_RGB), 0.55f};
        } else if (os.scatter > 0.5 && os.index <= 0.0) {
            // A matte white cavity: paint it as one, so an integrating sphere
            // does not read as a mirror.
            look = {Quantity_Color(0.92, 0.92, 0.88, Quantity_TOC_RGB), 0.35f};
        } else if (os.index > 0.0) {
            // Refractive solids: glassy and see-through, so rays stay visible.
            look = {Quantity_Color(0.45, 0.72, 0.95, Quantity_TOC_RGB), 0.65f};
        } else {
            look = {Quantity_Color(0.78, 0.78, 0.82, Quantity_TOC_RGB), 0.15f};
        }
        if (m_baseLook.size() <= i) m_baseLook.resize(i + 1);
        m_baseLook[i] = look;

        // The selected surface reads as selected: a scene tree and a viewport
        // that disagree about what is picked are worse than either alone.
        Appearance want = look;
        if (std::find(m_highlight.begin(), m_highlight.end(), int(i)) !=
            m_highlight.end())
            want = {Quantity_Color(1.0, 0.72, 0.25, Quantity_TOC_RGB), 0.25f};

        if (m_applied.size() <= i) m_applied.resize(i + 1);
        const bool lookChanged = fresh || m_applied[i] != want;
        if (lookChanged) {
            shape->SetColor(want.colour);
            shape->SetTransparency(want.transparency);
            m_applied[i] = want;
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
        if (!sameShape && isTessellated(os.shape)) {
            const Handle(Prs3d_Drawer)& drawer = shape->Attributes();
            drawer->SetTypeOfDeflection(Aspect_TOD_ABSOLUTE);
            drawer->SetMaximalChordialDeviation(os.meshDeflection);
            drawer->SetDeviationAngle(os.meshAngle);
        }

        if (m_shown.size() <= i) m_shown.resize(i + 1);
        m_shown[i] = os.shape;

        // A gizmo drag leaves its transform on the presentation, deliberately,
        // so the object stays where it was dropped while the document catches
        // up. This is the document catching up: the placement is in the B-Rep
        // now, and leaving it on the presentation as well would apply it twice.
        if (shape->LocalTransformation().Form() != gp_Identity) shape->ResetTransformation();

        if (fresh) {
            shape->SetDisplayMode(AIS_Shaded);
            // Selection mode 0 is the whole shape: that is what makes a surface
            // clickable, and it is why the ray cloud deliberately has none.
            m_context->Display(shape, AIS_Shaded, 0, Standard_False);
            m_shapes.push_back(shape);
            anyChanged = true;
        } else if (!sameShape || lookChanged) {
            // Recomputing a presentation costs a fresh triangulation upload and
            // a fresh selection tree. A surface showing the same B-Rep in the
            // same colour as a moment ago needs neither, and skipping it is
            // what keeps an edit to one object from redrawing the scene.
            m_context->Redisplay(shape, Standard_False, Standard_False);
            if (!sameShape) anyChanged = true;
        }
    }

    // The bounding box only moves when the geometry does, and reading it back
    // walks every shape in the scene.
    if (anyChanged) {
        Bnd_Box bounds;
        for (const OpticalSurface& os : surfaces) BRepBndLib::Add(os.shape, bounds);
        if (!bounds.IsVoid()) {
            double xm, ym, zm, xM, yM, zM;
            bounds.Get(xm, ym, zm, xM, yM, zM);
            m_bbMin[0] = xm; m_bbMin[1] = ym; m_bbMin[2] = zm;
            m_bbMax[0] = xM; m_bbMax[1] = yM; m_bbMax[2] = zM;
            m_sceneSize = std::max({xM - xm, yM - ym, zM - zm});
        }
    }

    // The overlay's glyphs are a fraction of this, so it is held until the
    // scene has genuinely changed size rather than tracked continuously.
    // Following every millimetre of a lens edit makes the emitter cone and the
    // receiver outline breathe while the user is editing something else --
    // which is the whole scene appearing to update, from one number.
    if (m_overlayScale <= 0.0 || m_sceneSize > 1.5 * m_overlayScale ||
        m_sceneSize < m_overlayScale / 1.5)
        m_overlayScale = m_sceneSize;

    m_sceneKey  = key;
    m_haveScene = true;

    // Hidden parts stay hidden across a parameter edit: the user hid them to be
    // able to see something, and a spin-box step is not a reason to undo that.
    for (std::size_t i = 0; i < m_shapes.size(); ++i)
        if (i < m_visible.size() && !m_visible[i])
            m_context->Erase(m_shapes[i], Standard_False);

    rebuildOverlay();
    applyClip();
    // The presentations the gizmo was holding may be different objects now, and
    // the object it is centred on has moved or changed size.
    refreshManipulator();
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

// ---- the overlay -----------------------------------------------------------
//
// A source glyph, the receiver planes with their acceptance cones, a corner
// axis triad and a scale bar.
//
// A first-time user cannot tell which grey translucent object is the
// measurement plane and which is the optic, and cannot tell how big any of it
// is. These four additions do more for comprehension than any amount of
// documentation, and they are lines: one primitive array, drawn the way the
// rays already are.

void OcctViewWidget::setSourceGlyphs(const std::vector<SourceGlyph>& sources) {
    m_sources = sources;
    rebuildOverlay();
}

void OcctViewWidget::setSourceGlyph(const gp_Pnt& origin, const gp_Dir& axis,
                                    double halfAngleDeg, bool collimated,
                                    double beamRadius) {
    SourceGlyph g;
    g.origin       = origin;
    g.axis         = axis;
    g.halfAngleDeg = halfAngleDeg;
    g.collimated   = collimated;
    g.beamRadius   = beamRadius;
    setSourceGlyphs({g});
}

void OcctViewWidget::clearSourceGlyph() {
    m_sources.clear();
    rebuildOverlay();
}

void OcctViewWidget::setOverlaysVisible(bool on) {
    if (m_overlaysOn == on) return;
    m_overlaysOn = on;
    rebuildOverlay();
}

void OcctViewWidget::setSurfaceVisible(int index, bool visible) {
    if (index < 0 || index >= int(m_shapes.size())) return;
    if (int(m_visible.size()) <= index) m_visible.resize(m_shapes.size(), true);
    m_visible[std::size_t(index)] = visible;
    if (m_context.IsNull()) return;
    if (visible) m_context->Display(m_shapes[std::size_t(index)], AIS_Shaded, 0,
                                    Standard_False);
    else         m_context->Erase(m_shapes[std::size_t(index)], Standard_False);
    // Handles floating over a body that is no longer drawn are handles for
    // something the user cannot see themselves moving. Only when it is a body
    // the gizmo is actually on, though: this is called once per surface when
    // the whole scene's visibility is applied.
    if (std::find(m_highlight.begin(), m_highlight.end(), index) != m_highlight.end())
        refreshManipulator();
    if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
}

void OcctViewWidget::setHighlightedSurface(int index) {
    if (index < 0) setHighlightedSurfaces({});
    else           setHighlightedSurfaces({index});
}

void OcctViewWidget::setHighlightedSurfaces(const std::vector<int>& indices) {
    if (m_highlight == indices) return;
    m_highlight = indices;
    if (m_context.IsNull()) return;
    // Consider every part rather than tracking the previous selection: a stale
    // highlight is the bug this exists to avoid. Only the ones whose colour
    // actually differs are repainted, though -- a selection change should cost
    // two presentations, not one per surface in the scene.
    bool touched = false;
    for (std::size_t i = 0; i < m_shapes.size(); ++i) {
        Appearance want = i < m_baseLook.size() ? m_baseLook[i] : Appearance{};
        if (std::find(m_highlight.begin(), m_highlight.end(), int(i)) !=
            m_highlight.end())
            want = {Quantity_Color(1.0, 0.72, 0.25, Quantity_TOC_RGB), 0.25f};

        if (m_applied.size() <= i) m_applied.resize(i + 1);
        if (m_applied[i] == want) continue;
        // Put the surface back to what it is, rather than leaving the last
        // selection painted on it.
        m_shapes[i]->SetColor(want.colour);
        m_shapes[i]->SetTransparency(want.transparency);
        m_applied[i] = want;
        m_context->Redisplay(m_shapes[i], Standard_False, Standard_False);
        touched = true;
    }
    refreshManipulator();
    if (touched && !m_view.IsNull()) { m_view->Invalidate(); update(); }
}

// ---- taking a drop from the object library ---------------------------------

static bool carriesObject(const QMimeData* mime) {
    return mime && mime->hasFormat(QLatin1String(scenedoc::dragMimeType()));
}

void OcctViewWidget::dragEnterEvent(QDragEnterEvent* e) {
    if (carriesObject(e->mimeData())) e->acceptProposedAction();
    else                              e->ignore();
}

void OcctViewWidget::dragMoveEvent(QDragMoveEvent* e) {
    if (carriesObject(e->mimeData())) e->acceptProposedAction();
    else                              e->ignore();
}

void OcctViewWidget::dropEvent(QDropEvent* e) {
    if (!carriesObject(e->mimeData())) { e->ignore(); return; }
    const QString key = QString::fromUtf8(
        e->mimeData()->data(QLatin1String(scenedoc::dragMimeType())));
    e->acceptProposedAction();

    gp_Pnt where(0, 0, 0);
    worldPointAt(e->position().toPoint(), where);
    emit objectDropped(key, where);
}

bool OcctViewWidget::worldPointAt(const QPoint& pos, gp_Pnt& out) {
    if (m_view.IsNull()) return false;

    // Landing on a part that is already there is the unambiguous case: the
    // depth is the depth of whatever was under the cursor.
    if (!m_context.IsNull()) {
        m_context->MoveTo(pos.x(), pos.y(), m_view, Standard_False);
        if (m_context->HasDetected()) {
            const Handle(SelectMgr_EntityOwner)& owner = m_context->DetectedOwner();
            if (!owner.IsNull() &&
                Handle(AIS_ViewCubeOwner)::DownCast(owner).IsNull()) {
                const gp_Pnt hit = m_context->MainSelector()->PickedPoint(1);
                // A degenerate pick reads as the origin, which is exactly the
                // answer the plane fallback exists to improve on.
                if (hit.SquareDistance(gp_Pnt(0, 0, 0)) > 1e-18) {
                    out = hit;
                    return true;
                }
            }
        }
    }

    // Otherwise: the eye ray through the cursor, met with the plane through the
    // scene centre facing the camera. That is the depth the user is looking at,
    // which is the only depth a click on empty space can mean.
    Standard_Real x = 0, y = 0, z = 0;
    m_view->Convert(pos.x(), pos.y(), x, y, z);
    Standard_Real dx = 0, dy = 0, dz = 0;
    m_view->Proj(dx, dy, dz);

    const gp_Pnt p0(x, y, z);
    const gp_Vec dir(dx, dy, dz);
    if (dir.Magnitude() < 1e-12) { out = p0; return true; }

    const gp_Pnt centre(0.5 * (m_bbMin[0] + m_bbMax[0]),
                        0.5 * (m_bbMin[1] + m_bbMax[1]),
                        0.5 * (m_bbMin[2] + m_bbMax[2]));
    const gp_Vec n    = dir.Normalized();
    const double denom = n.Dot(n);
    const double t     = gp_Vec(p0, centre).Dot(n) / denom;
    out = p0.Translated(n * t);

    // Whole millimetres: a drop is a gesture, not a measurement, and a lens at
    // z = 41.8371 is a number the user has to tidy up before it means anything.
    out = gp_Pnt(std::round(out.X()), std::round(out.Y()), std::round(out.Z()));
    return true;
}

bool OcctViewWidget::overlayUpToDate() const {
    if (!m_haveOverlay || m_drawnOverlaysOn != m_overlaysOn) return false;
    if (!m_overlaysOn) return true;                 // nothing drawn either way
    if (m_drawnScale != m_overlayScale) return false;
    for (int i = 0; i < 3; ++i)
        if (m_drawnBbMin[i] != m_bbMin[i] || m_drawnBbMax[i] != m_bbMax[i]) return false;

    if (m_drawnSources.size() != m_sources.size()) return false;
    for (std::size_t i = 0; i < m_sources.size(); ++i) {
        const SourceGlyph& a = m_drawnSources[i];
        const SourceGlyph& b = m_sources[i];
        if (!a.origin.IsEqual(b.origin, 1e-9) || !a.axis.IsEqual(b.axis, 1e-12) ||
            a.halfAngleDeg != b.halfAngleDeg || a.collimated != b.collimated ||
            a.beamRadius != b.beamRadius || a.label != b.label)
            return false;
    }

    if (m_drawnReceivers.size() != m_receivers.size()) return false;
    for (std::size_t i = 0; i < m_receivers.size(); ++i) {
        const ReceiverGlyph& a = m_drawnReceivers[i];
        const ReceiverGlyph& b = m_receivers[i];
        if (!a.centre.IsEqual(b.centre, 1e-9) || !a.u.IsEqual(b.u, 1e-12) ||
            !a.v.IsEqual(b.v, 1e-12) || !a.n.IsEqual(b.n, 1e-12) ||
            a.w != b.w || a.h != b.h || a.acceptanceDeg != b.acceptanceDeg)
            return false;
    }
    return true;
}

void OcctViewWidget::rebuildOverlay() {
    if (m_context.IsNull()) return;
    // Nothing it draws has moved, so what is on screen is already the answer.
    // Rebuilding it anyway is what made the emitter marker and the receiver
    // outline flicker on every step of an unrelated spin box.
    if (overlayUpToDate()) return;

    m_drawnOverlaysOn = m_overlaysOn;
    m_drawnSources    = m_sources;
    m_drawnReceivers  = m_receivers;
    m_drawnScale      = m_overlayScale;
    for (int i = 0; i < 3; ++i) { m_drawnBbMin[i] = m_bbMin[i]; m_drawnBbMax[i] = m_bbMax[i]; }
    m_haveOverlay = true;

    if (!m_overlay.IsNull()) {
        m_context->Remove(m_overlay, Standard_False);
        m_overlay.Nullify();
    }
    if (!m_overlaysOn) {
        if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
        return;
    }

    // The held scene size, not the live one -- see where m_overlayScale is set.
    const double scale = std::max(1e-6, m_overlayScale > 0.0 ? m_overlayScale : m_sceneSize);
    std::vector<RayCloud::Vertex> v;
    std::vector<RayCloud::Label>  labels;

    auto line = [&](const gp_Pnt& a, const gp_Pnt& b, const Quantity_Color& c) {
        v.push_back({a, c});
        v.push_back({b, c});
    };
    auto ring = [&](const gp_Pnt& centre, const gp_Dir& n, double radius,
                    const Quantity_Color& c, int steps = 48) {
        gp_Vec up(0, 0, 1);
        if (std::fabs(n.Dot(gp_Dir(0, 0, 1))) > 0.99) up = gp_Vec(1, 0, 0);
        gp_Vec e1 = gp_Vec(n).Crossed(up);
        if (e1.Magnitude() < 1e-12) return;
        e1.Normalize();
        gp_Vec e2 = gp_Vec(n).Crossed(e1);
        if (e2.Magnitude() < 1e-12) return;
        e2.Normalize();
        gp_Pnt prev;
        for (int i = 0; i <= steps; ++i) {
            const double t = 2.0 * M_PI * double(i) / double(steps);
            const gp_Vec off = e1 * (radius * std::cos(t)) + e2 * (radius * std::sin(t));
            const gp_Pnt p(centre.X() + off.X(), centre.Y() + off.Y(), centre.Z() + off.Z());
            if (i > 0) line(prev, p, c);
            prev = p;
        }
    };

    // ---- the sources --------------------------------------------------------
    //
    // Every one of them, not just the one the scene placed. A four-LED array
    // drew a single marker, so an offset typed into the source dialog could
    // only be checked by tracing and reading the pattern back -- and a typo
    // that put an emitter inside the optic had nothing on screen to say so.
    // Where the discs were last time, so the pickable faces below are only
    // rebuilt when they have actually moved: making a circular face per emitter
    // is kernel work, and the overlay is redrawn whenever the scene's bounding
    // box shifts -- which an edit to any object in it does.
    const std::vector<SourceDisc> previousDiscs = m_sourceDiscs;
    m_sourceDiscs.assign(m_sources.size(), SourceDisc{});
    for (std::size_t si = 0; si < m_sources.size(); ++si) {
        const SourceGlyph& src = m_sources[si];
        // Warmer and more saturated than the ray amber, so the emitter is not
        // mistaken for the brightest bundle leaving it. The scene's own source
        // keeps that amber and the added ones are a step lighter, so which one
        // the scene places is answerable at a glance.
        const Quantity_Color amber = si == 0
                                         ? Quantity_Color(1.0, 0.52, 0.12, Quantity_TOC_RGB)
                                         : Quantity_Color(1.0, 0.78, 0.34, Quantity_TOC_RGB);
        const double reach = 0.22 * scale;
        const gp_Vec ax(src.axis);

        labels.push_back({src.origin.Translated(gp_Vec(0, 0, 0.05 * scale)),
                          src.label.isEmpty() ? QStringLiteral("source") : src.label,
                          amber});

        // A short cross at the emitter, so its position is unambiguous even
        // when the cone is edge-on.
        const double tick = 0.03 * scale;
        for (int a = 0; a < 3; ++a) {
            gp_Vec d(a == 0 ? tick : 0.0, a == 1 ? tick : 0.0, a == 2 ? tick : 0.0);
            line(src.origin.Translated(-d), src.origin.Translated(d), amber);
        }

        if (src.collimated) {
            // A parallel bundle: the beam's own circle, extruded a little.
            const double r = src.beamRadius > 0.0 ? src.beamRadius : 0.05 * scale;
            ring(src.origin, src.axis, r, amber);
            m_sourceDiscs[si] = {src.origin, src.axis, r};
            const gp_Pnt tip = src.origin.Translated(ax * reach);
            ring(tip, src.axis, r, amber);
            for (int i = 0; i < 4; ++i) {
                const double t = 2.0 * M_PI * double(i) / 4.0;
                gp_Vec up(0, 0, 1);
                if (std::fabs(src.axis.Dot(gp_Dir(0, 0, 1))) > 0.99) up = gp_Vec(1, 0, 0);
                gp_Vec e1 = ax.Crossed(up); if (e1.Magnitude() < 1e-12) continue;
                e1.Normalize();
                gp_Vec e2 = ax.Crossed(e1); e2.Normalize();
                const gp_Vec off = e1 * (r * std::cos(t)) + e2 * (r * std::sin(t));
                line(src.origin.Translated(off), tip.Translated(off), amber);
            }
        } else {
            // The emission cone, as eight ribs and the circle they end on.
            //
            // The radius is fixed and the *length* follows from the angle,
            // rather than the other way round. Taking a fixed length and
            // opening it by tan(half) makes a near-hemispherical source -- the
            // default for a Lambertian emitter -- draw a cone fifty times the
            // size of the scene, and a very narrow one draw a spike too short
            // to see. This way a wide source reads as a flat disc and a narrow
            // one as a long spike, which is what those two things look like.
            //
            // A row of emitters has to stay a row of readable markers, so the
            // cone is also held under a share of the gap to its nearest
            // neighbour: four overlapping discs are one blur, and the picture
            // exists to show that the four are apart.
            const double half = std::clamp(src.halfAngleDeg, 1.0, 89.5);
            double r = 0.10 * scale;
            if (m_sources.size() > 1) {
                double nearest = 1e30;
                for (std::size_t sj = 0; sj < m_sources.size(); ++sj)
                    if (sj != si)
                        nearest = std::min(nearest,
                                           src.origin.Distance(m_sources[sj].origin));
                if (nearest < 1e29)
                    r = std::min(r, std::max(0.02 * scale, 0.35 * nearest));
            }
            const double reachC = std::clamp(r / std::tan(half * M_PI / 180.0),
                                             0.01 * scale, 0.30 * scale);
            const gp_Pnt tip = src.origin.Translated(ax * reachC);
            ring(tip, src.axis, r, amber);
            m_sourceDiscs[si] = {tip, src.axis, r};

            gp_Vec up(0, 0, 1);
            if (std::fabs(src.axis.Dot(gp_Dir(0, 0, 1))) > 0.99) up = gp_Vec(1, 0, 0);
            gp_Vec e1 = ax.Crossed(up);
            if (e1.Magnitude() > 1e-12) {
                e1.Normalize();
                gp_Vec e2 = ax.Crossed(e1);
                e2.Normalize();
                for (int i = 0; i < 8; ++i) {
                    const double t = 2.0 * M_PI * double(i) / 8.0;
                    const gp_Vec off = e1 * (r * std::cos(t)) + e2 * (r * std::sin(t));
                    line(src.origin, tip.Translated(off), amber);
                }
                // A second, half-size ring so the cone reads as a solid of
                // revolution rather than as a wire star.
                ring(src.origin.Translated(ax * (0.5 * reachC)), src.axis,
                     0.5 * r, amber, 32);
            }
        }
    }

    // ---- the receivers ------------------------------------------------------
    {
        const Quantity_Color green(0.35, 0.95, 0.55, Quantity_TOC_RGB);
        for (const ReceiverGlyph& g : m_receivers) {
            const gp_Vec eu(g.u), ev(g.v);
            const gp_Vec hu = eu * (0.5 * g.w), hv = ev * (0.5 * g.h);
            const gp_Pnt a = g.centre.Translated(-hu - hv);
            const gp_Pnt b = g.centre.Translated(hu - hv);
            const gp_Pnt c = g.centre.Translated(hu + hv);
            const gp_Pnt d = g.centre.Translated(-hu + hv);
            labels.push_back({d.Translated(gp_Vec(0, 0, 0.02 * scale)),
                              g.acceptanceDeg < 179.0
                                  ? QStringLiteral("receiver, accepts %1 deg")
                                        .arg(g.acceptanceDeg, 0, 'f', 0)
                                  : QStringLiteral("receiver"),
                              green});
            line(a, b, green); line(b, c, green); line(c, d, green); line(d, a, green);
            // The diagonals, so the plane reads as a plane rather than as a
            // wire square floating in front of the optic.
            line(a, c, green); line(b, d, green);

            // The acceptance cone, where it accepts less than everything: a
            // receiver that refuses light is a measurement condition, and it
            // should be visible that one is in force.
            if (g.acceptanceDeg < 179.0) {
                const double reach = 0.12 * scale;
                const double r     = reach * std::tan(std::min(89.0, g.acceptanceDeg)
                                                      * M_PI / 180.0);
                const gp_Pnt tip = g.centre.Translated(gp_Vec(g.n) * reach);
                ring(tip, g.n, r, green, 32);
                gp_Vec e1(g.u), e2(g.v);
                for (int i = 0; i < 4; ++i) {
                    const double t = 2.0 * M_PI * double(i) / 4.0;
                    const gp_Vec off = e1 * (r * std::cos(t)) + e2 * (r * std::sin(t));
                    line(g.centre, tip.Translated(off), green);
                }
            }
        }
    }

    // ---- the scale bar ------------------------------------------------------
    //
    // No axis triad here: the view already draws one in its lower-left corner,
    // labelled, in screen space (see TriedronDisplay in initViewer). A second
    // one anchored in the world would be two triads disagreeing about which
    // corner is which. The scale bar is the half of that pair that was actually
    // missing -- nothing on screen said how big any of this was.
    {
        // A bar of a round number of millimetres, chosen as the 1/2/5 step
        // nearest a fifth of the scene -- the same rule an axis tick uses.
        const double target = 0.2 * scale;
        const double decade = std::pow(10.0, std::floor(std::log10(std::max(1e-9, target))));
        double step = decade;
        for (double m : {1.0, 2.0, 5.0, 10.0})
            if (m * decade <= target * 1.5) step = m * decade;

        const Quantity_Color grey(0.75, 0.78, 0.85, Quantity_TOC_RGB);
        const gp_Pnt s0(m_bbMin[0], m_bbMin[1] - 0.14 * scale, m_bbMin[2]);
        const gp_Pnt s1 = s0.Translated(gp_Vec(step, 0, 0));
        line(s0, s1, grey);
        const double cap = 0.02 * scale;
        line(s0.Translated(gp_Vec(0, 0, -cap)), s0.Translated(gp_Vec(0, 0, cap)), grey);
        line(s1.Translated(gp_Vec(0, 0, -cap)), s1.Translated(gp_Vec(0, 0, cap)), grey);
        // Ticks along it, so the length can be read without a label: five of
        // them means the bar is five of whatever the caption says.
        for (int i = 1; i < 5; ++i) {
            const gp_Pnt t = s0.Translated(gp_Vec(step * double(i) / 5.0, 0, 0));
            line(t.Translated(gp_Vec(0, 0, -0.4 * cap)),
                 t.Translated(gp_Vec(0, 0, 0.4 * cap)), grey);
        }
        labels.push_back({s0.Translated(gp_Vec(0, 0, -2.0 * cap)),
                          QStringLiteral("%1 mm").arg(step, 0, 'g', 3), grey});
    }

    // The emitters' discs, as real faces, from the circles just drawn.
    bool discsMoved = previousDiscs.size() != m_sourceDiscs.size() ||
                      m_sourceMarkers.size() != m_sourceDiscs.size();
    for (std::size_t i = 0; !discsMoved && i < m_sourceDiscs.size(); ++i)
        discsMoved = !previousDiscs[i].centre.IsEqual(m_sourceDiscs[i].centre, 1e-9) ||
                     !previousDiscs[i].normal.IsEqual(m_sourceDiscs[i].normal, 1e-12) ||
                     previousDiscs[i].radius != m_sourceDiscs[i].radius;
    if (discsMoved) rebuildSourceMarkers();

    if (v.empty()) {
        if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
        return;
    }
    m_overlay = new RayCloud(std::move(v), std::move(labels));
    // Display mode 0 and no selection: this is furniture, and the optics are
    // the things worth clicking on.
    m_context->Display(m_overlay, 0, -1, Standard_False);
    if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
}

// A clickable disc at each emitter.
//
// The overlay is one line-primitive object with no selection at all, which is
// right for tens of thousands of ray segments and wrong for the handful of
// things a user actually wants to click. A source is drawn but never traced, so
// it has no surface in the scene: it was the one thing in the viewport that
// could be seen and not selected. These are the same circles the overlay draws,
// as faces, so what is pickable and what is visible cannot drift apart.
void OcctViewWidget::rebuildSourceMarkers() {
    if (m_context.IsNull()) return;
    for (const Handle(AIS_Shape)& marker : m_sourceMarkers)
        m_context->Remove(marker, Standard_False);
    m_sourceMarkers.assign(m_sourceDiscs.size(), Handle(AIS_Shape)());

    for (std::size_t i = 0; i < m_sourceDiscs.size(); ++i) {
        const SourceDisc& d = m_sourceDiscs[i];
        if (d.radius <= 1e-9) continue;

        const gp_Ax2 frame(d.centre, d.normal);
        BRepBuilderAPI_MakeEdge edge(gp_Circ(frame, d.radius));
        if (!edge.IsDone()) continue;
        BRepBuilderAPI_MakeWire wire(edge.Edge());
        if (!wire.IsDone()) continue;
        BRepBuilderAPI_MakeFace face(gp_Pln(frame), wire.Wire(), Standard_True);
        if (!face.IsDone()) continue;

        Handle(AIS_Shape) marker = new AIS_Shape(face.Shape());
        marker->SetDisplayMode(AIS_Shaded);
        // Nearly transparent: the disc is a handle to grab, not another thing
        // to look at. The overlay's own ring is what draws the emitter.
        m_context->Display(marker, AIS_Shaded, 0, Standard_False);
        m_sourceMarkers[i] = marker;
    }
    applySourceHighlight();
}

void OcctViewWidget::applySourceHighlight() {
    if (m_context.IsNull()) return;
    for (std::size_t i = 0; i < m_sourceMarkers.size(); ++i) {
        if (m_sourceMarkers[i].IsNull()) continue;
        const bool on = int(i) == m_highlightSource;
        m_sourceMarkers[i]->SetColor(
            on ? Quantity_Color(1.0, 0.72, 0.25, Quantity_TOC_RGB)
               : Quantity_Color(1.0, 0.52, 0.12, Quantity_TOC_RGB));
        m_sourceMarkers[i]->SetTransparency(on ? 0.35f : 0.80f);
        m_context->Redisplay(m_sourceMarkers[i], Standard_False, Standard_False);
    }
    if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
}

void OcctViewWidget::setHighlightedSource(int index) {
    if (m_highlightSource == index) return;
    m_highlightSource = index;
    applySourceHighlight();
    refreshManipulator();
}

// ---- the transform gizmo ----------------------------------------------------
//
// AIS_Manipulator is OCCT's own translate / rotate / scale handle set, and the
// arrangement here is Blender's: pick an object, choose a tool, drag a handle.
// One tool is shown at a time, because three sets of handles on one object
// leave nothing of the object left to click.
//
// The document is the one place an object's placement lives, so the gizmo does
// not get to keep its own answer. During a drag the presentation is moved
// directly -- that is what makes it follow the cursor at frame rate, with no
// tessellation in the way -- and on release the *whole* drag is reported once,
// written into the document, and comes back as real geometry. The presentation
// keeps the dragged position until it does, so there is no frame in which the
// object snaps back to where it started.

Handle(AIS_ManipulatorObjectSequence) OcctViewWidget::manipulatorTargets() const {
    Handle(AIS_ManipulatorObjectSequence) targets = new AIS_ManipulatorObjectSequence();
    for (int idx : m_highlight) {
        if (idx < 0 || idx >= int(m_shapes.size())) continue;
        // A hidden body has nothing on screen to hang a handle off.
        if (idx < int(m_visible.size()) && !m_visible[std::size_t(idx)]) continue;
        targets->Append(m_shapes[std::size_t(idx)]);
    }
    // An emitter is drawn but never traced, so it has no body at all: its
    // marker disc is the thing on screen, and it is what the gizmo grabs.
    if (targets->IsEmpty() && m_highlightSource >= 0 &&
        m_highlightSource < int(m_sourceMarkers.size()) &&
        !m_sourceMarkers[std::size_t(m_highlightSource)].IsNull())
        targets->Append(m_sourceMarkers[std::size_t(m_highlightSource)]);
    return targets;
}

void OcctViewWidget::refreshManipulator() {
    if (m_context.IsNull()) return;

    if (!m_manipulator.IsNull() && m_manipulator->IsAttached()) {
        if (m_manipulator->HasActiveTransformation()) m_manipulator->StopTransform(Standard_False);
        m_manipulator->DeactivateCurrentMode();
        m_manipulator->Detach();
    }
    m_draggingGizmo = false;

    Handle(AIS_ManipulatorObjectSequence) targets = manipulatorTargets();
    if (m_transformMode == TransformMode::None || targets->IsEmpty()) {
        if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
        return;
    }

    const AIS_ManipulatorMode mode = m_transformMode == TransformMode::Translate
                                         ? AIS_MM_Translation
                                         : m_transformMode == TransformMode::Rotate
                                               ? AIS_MM_Rotation
                                               : AIS_MM_Scaling;

    if (m_manipulator.IsNull()) {
        m_manipulator = new AIS_Manipulator();
        // Grabbing a handle on hover rather than on a separate click: a gizmo
        // that has to be selected before it can be dragged is two gestures for
        // what the user means as one.
        m_manipulator->SetModeActivationOnDetection(Standard_True);
        // A fixed size on screen. Without it the handles are sized from the
        // object, so they vanish on a small lens and swallow the scene on a
        // large one.
        m_manipulator->SetZoomPersistence(Standard_True);
    }

    // Only the active tool's handles are drawn. The others stay off rather than
    // being drawn inert, because an inert handle is still something the user
    // aims at and then finds does nothing.
    m_manipulator->SetPart(AIS_MM_Translation, mode == AIS_MM_Translation);
    m_manipulator->SetPart(AIS_MM_Rotation,    mode == AIS_MM_Rotation);
    m_manipulator->SetPart(AIS_MM_Scaling,     mode == AIS_MM_Scaling);
    m_manipulator->SetPart(AIS_MM_TranslationPlane, Standard_False);

    AIS_Manipulator::OptionsForAttach options;
    options.SetAdjustPosition(Standard_True)     // centred on what it moves
           .SetAdjustSize(Standard_False)        // zoom persistence sizes it
           .SetEnableModes(Standard_False);      // only the active one, below
    m_manipulator->Attach(targets, options);

    // Attach answers both "where" and "which way round" from the bounding box:
    // the centre of it, on the world axes. Both are wrong to keep re-asking.
    //
    // The axes are wrong the moment the object is off them -- the rings follow
    // the object while it is being turned, and squaring them up again on the
    // next attach made a rotation end with the gizmo snapping back.
    //
    // The centre is wrong for a subtler reason: the box is axis-aligned, so the
    // one round a turned body is not the turned box round the untouched one,
    // and its centre is a different point of the object every time. Measuring
    // it once and keeping it -- in the object's own coordinates, where it is a
    // point of the object and travels with it -- is what makes the gizmo stay
    // exactly where the drag left it.
    if (m_gizmoAnchorId != m_selectionId) {
        m_gizmoAnchor =
            m_manipulator->Position().Location().Transformed(m_selectionFrame.Inverted());
        m_gizmoAnchorId = m_selectionId;
    }

    gp_Trsf rotation = m_selectionFrame;
    rotation.SetTranslationPart(gp_Vec(0.0, 0.0, 0.0));
    rotation.SetScaleFactor(1.0);
    gp_Dir normal(0.0, 0.0, 1.0);
    gp_Dir xRef(1.0, 0.0, 0.0);
    normal.Transform(rotation);
    xRef.Transform(rotation);
    m_manipulator->SetPosition(
        gp_Ax2(m_gizmoAnchor.Transformed(m_selectionFrame), normal, xRef));

    m_manipulator->EnableMode(mode);

    if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
}

void OcctViewWidget::setSelectionFrame(int objectId, const gp_Trsf& world) {
    bool same = objectId == m_selectionId;
    for (int i = 1; i <= 3 && same; ++i)
        for (int j = 1; j <= 4 && same; ++j)
            same = m_selectionFrame.Value(i, j) == world.Value(i, j);
    if (same) return;

    // A different object needs its anchor measured afresh; the same object
    // having moved must keep the one it has, which is the whole point of it.
    if (objectId != m_selectionId) m_gizmoAnchorId = 0;
    m_selectionId    = objectId;
    m_selectionFrame = world;
    refreshManipulator();
}

void OcctViewWidget::setTransformMode(TransformMode mode) {
    if (m_transformMode == mode) return;
    m_transformMode = mode;
    refreshManipulator();
    emit transformModeChanged(int(mode));
}

// ---- the navigation cube ----------------------------------------------------
//
// AIS_ViewCube is an ordinary interactive object that pins itself to a screen
// corner through transform persistence: its facets, edges and corners are
// separate selection owners, each standing for a camera orientation, and
// clicking one animates the camera to it.
//
// Two things it needs from the host that are easy to get wrong, and both are
// why this is not simply "display it and hope":
//
//   The animation has to be driven. AIS_ViewCube::HandleClick will run the
//   whole thing itself, but it does so in a blocking loop that redraws the view
//   from inside the click handler -- the window stops answering for the
//   duration. This widget already has a 60 Hz timer for the WASD walk, so the
//   animation is started on click and advanced a frame at a time there.
//
//   It has to survive the section plane. The clip plane is added to the *view*,
//   so it cuts everything the view draws, and a sliced navigation cube in the
//   corner is a puzzle rather than a control. An empty clip-plane set marked
//   ToOverrideGlobal replaces the view's planes for this object alone.

void OcctViewWidget::buildViewCube() {
    if (m_context.IsNull() || !m_viewCube.IsNull()) return;

    m_viewCube = new AIS_ViewCube();

    // Pixels, not model units: transform persistence keeps it the same size
    // whatever the scene is. SetSize adapts the facet extension, the axes
    // padding and the font with it.
    m_viewCube->SetSize(58.0);
    m_viewCube->SetFontHeight(12.0);
    m_viewCube->SetBoxColor(Quantity_Color(0.24, 0.26, 0.32, Quantity_TOC_RGB));
    m_viewCube->SetTextColor(Quantity_Color(0.92, 0.94, 0.98, Quantity_TOC_RGB));
    m_viewCube->SetInnerColor(Quantity_Color(0.16, 0.17, 0.21, Quantity_TOC_RGB));

    // Half a second: long enough to read as a turn rather than a jump cut,
    // short enough not to be in the way.
    m_viewCube->SetDuration(0.45);
    // The camera's up direction comes back to vertical, so TOP is always the
    // same TOP however the free-look left the roll.
    m_viewCube->SetResetCamera(Standard_True);
    // Orientation only. The zoom and the pan are the user's -- refitting on
    // every cube click would throw away the framing they chose, which is the
    // same reason a parameter edit does not refit either. F still fits.
    m_viewCube->SetFitSelected(Standard_False);
    // Started here, advanced by stepWalk.
    m_viewCube->SetAutoStartAnimation(Standard_False);
    m_viewCube->SetFixedAnimationLoop(Standard_False);

    // Lower left, and far enough in that the axis labels clear the window edge:
    // the offset is to the cube's centre, and the labelled axes reach well past
    // the box itself.
    m_viewCube->SetTransformPersistence(
        new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_LEFT_LOWER,
                                    Graphic3d_Vec2i(95, 95)));

    // Immune to the section plane; see the note above.
    Handle(Graphic3d_SequenceOfHClipPlane) noClipping = new Graphic3d_SequenceOfHClipPlane();
    noClipping->SetOverrideGlobal(Standard_True);
    m_viewCube->SetClipPlanes(noClipping);

}

// Which orientation widget occupies the lower-left corner.
//
// One or the other, never both: two of them in the same corner disagreeing
// about which way is up would be worse than either. Turning the cube off brings
// the plain trihedron back rather than leaving the corner empty, because
// knowing which way the model is facing is not optional.
void OcctViewWidget::applyCornerWidget() {
    if (m_view.IsNull() || m_context.IsNull()) return;

    if (m_cubeVisible) {
        m_view->TriedronErase();
        if (!m_viewCube.IsNull()) m_context->Display(m_viewCube, Standard_False);
    } else {
        if (!m_viewCube.IsNull()) m_context->Erase(m_viewCube, Standard_False);
        m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.08,
                                V3d_ZBUFFER);
    }
    m_view->Invalidate();
    update();
}

void OcctViewWidget::setViewCubeVisible(bool on) {
    if (m_cubeVisible == on) return;
    m_cubeVisible = on;
    applyCornerWidget();
}

void OcctViewWidget::startViewCubeAnimation(const Handle(AIS_ViewCubeOwner)& owner) {
    if (m_viewCube.IsNull() || owner.IsNull() || m_view.IsNull()) return;
    // StartAnimation reads the view off the animation object rather than being
    // handed one, so it has to be bound before the first click and after any
    // rebuild of the view.
    m_viewCube->ViewAnimation()->SetView(m_view);
    m_viewCube->StartAnimation(owner);
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
    int found       = -1;
    int foundSource = -1;
    if (m_context->HasDetected()) {
        const Handle(SelectMgr_EntityOwner)& owner = m_context->DetectedOwner();

        // A cube facet is not a surface: it is a camera instruction, and it
        // must not clear the surface selection or report a pick of -1 that the
        // scene tree would act on.
        Handle(AIS_ViewCubeOwner) cubeOwner = Handle(AIS_ViewCubeOwner)::DownCast(owner);
        if (!cubeOwner.IsNull()) {
            startViewCubeAnimation(cubeOwner);
            m_view->Invalidate();
            update();
            return;
        }

        // Held by value, deliberately.
        //
        // Selectable() returns a handle *by value*, and OCCT converts between
        // handle types through a conversion operator that reinterpret_casts to
        // a reference. So binding the result to a `const Handle(...)&` of a
        // different type -- which is how this was written, and which looks free
        // -- binds to a temporary that is not lifetime-extended: it dies at the
        // end of the declaration and takes its reference count with it. Every
        // pick then downcast a dangling handle, got null, and reported empty
        // space, so nothing in the 3D view could be selected at all.
        Handle(AIS_InteractiveObject) detected;
        if (!owner.IsNull()) {
            const Handle(SelectMgr_SelectableObject) selectable = owner->Selectable();
            detected = Handle(AIS_InteractiveObject)::DownCast(selectable);
        }
        if (!detected.IsNull()) {
            for (std::size_t i = 0; i < m_shapes.size(); ++i)
                if (m_shapes[i] == detected) { found = int(i); break; }
            if (found < 0)
                for (std::size_t i = 0; i < m_sourceMarkers.size(); ++i)
                    if (!m_sourceMarkers[i].IsNull() && m_sourceMarkers[i] == detected) {
                        foundSource = int(i);
                        break;
                    }
        }
    }

    m_context->ClearSelected(Standard_False);
    if (found >= 0) m_context->SelectDetected();

    m_view->Invalidate();
    update();
    // One or the other, never both: reporting an emitter *and* an empty surface
    // pick would select the source and then immediately clear it again.
    if (foundSource >= 0) emit sourcePicked(foundSource);
    else                  emit surfacePicked(found);
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

    // A press on a gizmo handle starts a transform, not an orbit. The hover
    // above has already told the manipulator which handle the cursor is on,
    // which is what HasActiveMode is answering.
    if (m_dragButton == Qt::LeftButton && !m_manipulator.IsNull() &&
        m_manipulator->HasActiveMode()) {
        m_draggingGizmo = true;
        m_gizmoDelta    = gp_Trsf();
        m_manipulator->StartTransform(e->pos().x(), e->pos().y(), m_view);
        return;
    }

    // Pressing a cube facet is the start of a click, not of an orbit: turning
    // the scene from a control that exists to stop the scene turning by hand
    // would be the opposite of what the cube is for.
    if (m_dragButton == Qt::LeftButton && !m_hoverOnCube)
        m_view->StartRotation(e->pos().x(), e->pos().y());
}

void OcctViewWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_view.IsNull()) return;

    if (m_dragButton == Qt::NoButton) {
        // Nothing is being dragged, so this is a hover: ask the context what is
        // under the cursor. That is what makes a cube facet light up before it
        // is clicked, and it tells the user which surface a click would pick.
        if (!m_context.IsNull()) {
            m_context->MoveTo(e->pos().x(), e->pos().y(), m_view, Standard_False);
            const bool wasOnCube = m_hoverOnCube;
            m_hoverOnCube =
                m_context->HasDetected() &&
                !Handle(AIS_ViewCubeOwner)::DownCast(m_context->DetectedOwner()).IsNull();
            setCursor(m_hoverOnCube ? Qt::PointingHandCursor : Qt::ArrowCursor);
            if (wasOnCube != m_hoverOnCube || m_context->HasDetected()) {
                m_view->Invalidate();
                update();
            }
        }
        m_lastMouse = e->pos();
        return;
    }

    if (m_draggingGizmo && !m_manipulator.IsNull()) {
        // Moves the presentation, not the document. The object follows the
        // cursor with no kernel work at all, and what the drag amounted to is
        // written down once, when it ends.
        m_gizmoDelta = m_manipulator->Transform(e->pos().x(), e->pos().y(), m_view);
        m_lastMouse  = e->pos();
        m_view->Invalidate();
        update();
        return;
    }

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

    if (m_draggingGizmo) {
        m_draggingGizmo = false;
        if (!m_manipulator.IsNull()) {
            // Applied, not cancelled: the presentation stays where the user
            // dropped it until the document has been told and the real geometry
            // comes back. Cancelling here would snap the object home for the
            // frame or two that takes.
            m_manipulator->StopTransform(Standard_True);
            m_manipulator->DeactivateCurrentMode();
        }
        if (m_gizmoDelta.Form() != gp_Identity) emit objectTransformed(m_gizmoDelta);
        m_gizmoDelta = gp_Trsf();
        if (!m_view.IsNull()) { m_view->Invalidate(); update(); }
        return;
    }

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
    // G / R / S are Blender's transform tools, and S is also this viewport's
    // walk-backward key. They cannot both win, so the one that wins is the one
    // that has something to act on: with an object selected there is a
    // transform to choose, and with nothing selected there is not -- so a
    // walkthrough keeps all six of its keys, and clicking empty space is how
    // you hand them back.
    const bool haveTarget = !manipulatorTargets()->IsEmpty();
    if (haveTarget) {
        switch (e->key()) {
        case Qt::Key_G: setTransformMode(TransformMode::Translate); return;
        case Qt::Key_R: setTransformMode(TransformMode::Rotate);    return;
        case Qt::Key_S: setTransformMode(TransformMode::Scale);     return;
        default: break;
        }
    }
    switch (e->key()) {
    case Qt::Key_F: fitAll(); return;
    // Escape puts the gizmo away whether or not anything is selected, because
    // "get this off my object" has to work when the handles are what is in the
    // way of clicking the object.
    case Qt::Key_Escape: setTransformMode(TransformMode::None); return;
    // Reset lost R to the gizmo, so it has a key of its own that nothing else
    // wants rather than one that means two things.
    case Qt::Key_Home: resetView(); return;
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
    if (m_view.IsNull()) return;

    // The cube's swing to a standard view, one frame at a time. Doing it here
    // rather than inside HandleClick is what keeps the window answering while
    // the camera moves.
    if (!m_viewCube.IsNull() && m_viewCube->HasAnimation()) {
        m_viewCube->UpdateAnimation(Standard_True);
        m_view->Invalidate();
        update();
    }

    if (m_keysDown.isEmpty()) return;
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
