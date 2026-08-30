#include "render/AppearanceView.h"

#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <Aspect_DisplayConnection.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_CView.hxx>
#include <Graphic3d_DiagnosticInfo.hxx>
#include <Graphic3d_RenderingParams.hxx>
#include <Graphic3d_ToneMappingMethod.hxx>
#include <Graphic3d_TypeOfShadingModel.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <TCollection_AsciiString.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#include <Aspect_SkydomeBackground.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_PositionalLight.hxx>
#include <WNT_Window.hxx>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// The GL version the path tracer needs. OCCT's ray-tracing shaders are written
// against GLSL 4.0; below that it silently falls back to rasterization, which
// reads to a user as "the render button does nothing".
constexpr double kMinGlForPathTracing = 4.0;
// The adaptive screen sampler is built on atomic image operations, which are
// GL 4.4. Enabling it below that is what turns a working render into a blank
// one on an older driver.
constexpr double kMinGlForAdaptive = 4.4;
// The widest angular size OCCT will take for a directional light, which is what
// its soft shadow is built on. Past a quarter turn the setter throws.
constexpr double kMaxSmoothAngle = 1.5707963267948966;

// Where the sun is in the Sky mode's procedural dome, as a direction pointing
// *at* it. Stated once because two things have to agree about it: the dome that
// is drawn, and the light that does the lighting -- see applyLighting.
const gp_Dir kSunTowards(0.45, 0.70, 0.55);

// The leading major.minor of a GL version string such as
// "4.6.0 NVIDIA 552.22". Returns 0 when it does not start with a number, which
// is the honest answer for a string nobody recognises.
double parseGlVersion(const TCollection_AsciiString& s) {
    const QString text = QString::fromUtf8(s.ToCString()).trimmed();
    const QStringList parts = text.split(QLatin1Char(' ')).value(0).split(QLatin1Char('.'));
    if (parts.size() < 2) return 0.0;
    bool okMajor = false, okMinor = false;
    const int major = parts[0].toInt(&okMajor);
    const int minor = parts[1].toInt(&okMinor);
    if (!okMajor || !okMinor) return 0.0;
    return double(major) + double(minor) / 10.0;
}

// OCCT's buffer dump, as a QImage.
//
// Two formats, because the two dump paths choose differently. The offscreen
// buffer is asked for Image_Format_BGR32 -- four bytes per pixel in B, G, R,
// unused order, which is byte for byte what QImage::Format_RGB32 holds on a
// little-endian machine, so a row is a memcpy. ToPixMap allocates the image
// itself and picks Image_Format_RGB for a colour dump, three bytes packed,
// which has to be widened a pixel at a time. Both are dumped top-down, so row 0
// is the top row in both.
QImage toQImage(const Image_PixMap& pixels) {
    if (pixels.SizeX() == 0 || pixels.SizeY() == 0) return QImage();

    const int width  = int(pixels.SizeX());
    const int height = int(pixels.SizeY());
    QImage    image(width, height, QImage::Format_RGB32);
    if (image.isNull()) return QImage();

    switch (pixels.Format()) {
        case Image_Format_BGR32:
        case Image_Format_BGRA:
            for (int y = 0; y < height; ++y)
                std::memcpy(image.scanLine(y), pixels.Row(Standard_Size(y)),
                            std::size_t(width) * 4);
            return image;

        case Image_Format_RGB:
        case Image_Format_BGR: {
            const bool swap = pixels.Format() == Image_Format_RGB;
            for (int y = 0; y < height; ++y) {
                const Standard_Byte* src = pixels.Row(Standard_Size(y));
                QRgb* dst = reinterpret_cast<QRgb*>(image.scanLine(y));
                for (int x = 0; x < width; ++x, src += 3)
                    dst[x] = swap ? qRgb(src[0], src[1], src[2])
                                  : qRgb(src[2], src[1], src[0]);
            }
            return image;
        }

        default:
            // A format nobody asked for is not a picture worth guessing at.
            return QImage();
    }
}

QString lookup(const TColStd_IndexedDataMapOfStringString& info, const char* key) {
    const TCollection_AsciiString k(key);
    for (TColStd_IndexedDataMapOfStringString::Iterator it(info); it.More(); it.Next())
        if (it.Key().IsEqual(k)) return QString::fromUtf8(it.Value().ToCString());
    return QString();
}

} // namespace

AppearanceView::AppearanceView(QWidget* parent) : QWidget(parent) {
    // OCCT renders into this widget's native window directly, so Qt must not
    // double-buffer or clear it.
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_NativeWindow);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);

    // The accumulation pump. A path-traced frame is one sample of a running
    // average, so the picture improves by being redrawn rather than by being
    // computed once; this is what redraws it, and it goes quiet the moment the
    // budget is reached so an idle tab costs no GPU at all.
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &AppearanceView::step);
}

AppearanceView::~AppearanceView() {
    if (!m_view.IsNull()) m_view->Remove();
}

void AppearanceView::setSharedDriver(const Handle(Graphic3d_GraphicDriver)& driver) {
    m_sharedDriver = driver;
}

// ---------------------------------------------------------------------------

void AppearanceView::initViewer() {
    if (!m_view.IsNull() || m_initFailed) return;

    try {
        Handle(Graphic3d_GraphicDriver) driver = m_sharedDriver;
        if (driver.IsNull()) {
            Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
            driver = new OpenGl_GraphicDriver(display);
        }

        // A viewer of its own, not the main viewport's. Sharing the *driver*
        // shares the GL resources; sharing the viewer would share the displayed
        // structures, and the ray overlay and the transform gizmo are exactly
        // what this view exists not to draw.
        m_viewer  = new V3d_Viewer(driver);
        m_context = new AIS_InteractiveContext(m_viewer);

        m_view = m_viewer->CreateView();
        Handle(WNT_Window) window = new WNT_Window(reinterpret_cast<Aspect_Drawable>(winId()));
        m_view->SetWindow(window);
        if (!window->IsMapped()) window->Map();

        // Perspective, unlike the CAD viewport. A render is a photograph, and a
        // photograph has a vanishing point; it is also the only projection the
        // path tracer's depth of field means anything under.
        m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Perspective);
        m_view->Camera()->SetFOVy(40.0);
        m_view->SetProj(V3d_XposYnegZpos);

        probeCapability();
        applyRenderingParams();
        applyLighting();
        // The scene itself is left to the dirty check in paintEvent, which is
        // the one place it is built and framed -- two of them would frame the
        // camera twice and disagree about which build the estimate belongs to.
        m_view->MustBeResized();
    } catch (const Standard_Failure& e) {
        m_initFailed       = true;
        m_capable          = false;
        m_capabilityReport = QStringLiteral("The 3D driver could not be started: %1")
                                 .arg(QString::fromUtf8(e.GetMessageString()));
        emit capabilityDetermined(false, m_capabilityReport);
    }
}

void AppearanceView::applyLighting() {
    if (m_viewer.IsNull()) return;

    std::vector<Handle(V3d_Light)> pending;
    try {
        // Everything this view put in, taken out again. Tracked rather than read
        // back off the viewer, so a light OCCT added on its own behalf is left
        // alone and the same light is never removed twice.
        for (const Handle(V3d_Light)& light : m_lights) m_viewer->DelLight(light);
        m_lights.clear();

        // Staged rather than handed to the viewer as they are built. OCCT
        // numbers the shadow-map *samplers* by light index -- the fragment
        // shader it writes reads occShadowMapSamplers[i] for light i -- but
        // fills the maps themselves by counting only the lights that cast one,
        // so THE_NB_SHADOWMAPS is the number of casters. Put a light that casts
        // no shadow ahead of one that does and the two numberings drift apart,
        // the generated shader indexes the sampler array past its end, and it
        // fails to compile ("error C1068: array index out of bounds") -- which
        // costs the whole shading program, not the shadow. One scene emitter
        // plus the studio's three suns was exactly that: a point light at index
        // 0, casters at 1..3, three maps. Ordering the casters first is what
        // holds the two numberings together, and it is done here, once, rather
        // than left to the order the rig below happens to be written in.
        auto add = [&pending](const Handle(V3d_Light)& light) { pending.push_back(light); };

        // Directional is the one light type OCCT 7.8 will cast a shadow from, so
        // this is the one place the flag is set. Every setter below throws outside
        // its own range -- intensity must be positive, the smoothing angle must be
        // within [0, pi/2] -- so the values are held to it here rather than trusted.
        auto directional = [](const gp_Dir& d, double intensity, double smooth) {
            Handle(V3d_DirectionalLight) light = new V3d_DirectionalLight(d, Quantity_NOC_WHITE);
            light->SetIntensity(Standard_ShortReal(std::max(1e-3, intensity)));
            // A non-zero angular size is what gives a soft shadow instead of the
            // hard-edged one a mathematical point casts.
            light->SetSmoothAngle(Standard_ShortReal(std::clamp(smooth, 0.0, kMaxSmoothAngle)));
            light->SetCastShadows(Standard_True);
            return light;
        };

        // The scene's own sources, in every mode, and *every* source rather
        // than only the point-like ones.
        //
        // A source with an area is already geometry with `Le` on it, displayed
        // with the scene, and that face is what the camera sees glowing. It is
        // not what lights the scene: OCCT's path tracer finds emissive geometry
        // only by a path landing on it, so a 5 mm die in a 250 mm luminaire is
        // never found and the picture is black at every exposure. The light
        // below is what illuminates; the face is what is seen. See
        // appearance::Emitter::intensity, which is where that is set out.
        //
        // No SetCastShadows here, deliberately: OCCT 7.8 implements shadow
        // casting for directional and spot lights only and *throws* on a
        // positional one, which -- uncaught, inside a rebuild -- took the whole
        // application down on any scene whose source is point-like, which is
        // most of the library. It would have bought nothing either way: the
        // header says shadow casting has no effect under ray tracing, where a
        // shadow is what happens when a path does not reach the light rather
        // than something a light has to be asked for. The smoothing radius,
        // which the path tracer does use, stays.
        for (const appearance::Emitter& e : m_emitters.emitters) {
            Handle(V3d_PositionalLight) light =
                new V3d_PositionalLight(e.origin, Quantity_NOC_WHITE);
            light->SetIntensity(Standard_ShortReal(std::max(1e-3, e.intensity)));
            light->SetSmoothRadius(Standard_ShortReal(std::max(0.0, e.smoothRadius)));
            add(light);
        }

        // The sky, as light rather than as a picture. OCCT's path tracer does
        // not gather from the background cube map -- an escaped path returns
        // the flat background colour, not the dome -- so a scene under the
        // skydome and nothing else renders black against a photograph of a
        // sky, which reads as a broken renderer rather than as a limitation.
        // A sun from the dome's own direction and a soft fill for the rest of
        // the hemisphere is what the drawn sky would have done, and it agrees
        // with the drawn sky because both read kSunTowards.
        if (m_lighting == Lighting::Sky) {
            add(directional(kSunTowards.Reversed(), 2.2, 0.02));

            Handle(V3d_AmbientLight) skyFill = new V3d_AmbientLight(
                Quantity_Color(0.55, 0.66, 0.82, Quantity_TOC_RGB));
            skyFill->SetIntensity(0.9f);
            add(skyFill);
        }

        // The studio rig. Used when the user asks for it, and as a fallback when
        // the scene has no light of its own -- a part with no source would
        // otherwise render as a black rectangle, which is a correct picture of
        // nothing and a useless one.
        const bool sceneLightsItself = !m_emitters.emitters.empty();
        const bool wantStudio = m_lighting == Lighting::Studio ||
                                (m_lighting == Lighting::SceneEmitters && !sceneLightsItself);

        if (wantStudio) {
            add(directional(gp_Dir(-0.45, -0.70, -0.55), 2.6, 0.05));
            add(directional(gp_Dir(0.75, -0.30, -0.25), 0.9, 0.10));
            add(directional(gp_Dir(0.10, 0.85, -0.35), 0.6, 0.10));

            Handle(V3d_AmbientLight) ambient = new V3d_AmbientLight(Quantity_NOC_WHITE);
            ambient->SetIntensity(0.25f);
            add(ambient);
        }
    } catch (const Standard_Failure& e) {
        // A light OCCT declines to build is a picture that is lit wrongly, not
        // an application that stops. The rig is half-built by then, which is why
        // the staged lights, the environment and the restart below are all
        // outside the guard: whatever was accepted still has to be shown. And it is said out loud,
        // because a render lit by fewer lights than it was given is a picture
        // that answers a slightly different question.
        emit lightingFailed(QStringLiteral("Some of the scene's lights could not be "
                                           "built, so the render is lit by the rest: %1")
                                .arg(QString::fromUtf8(e.GetMessageString())));
    }

    // Outside the guard, for the reason given there: whatever was accepted
    // before a light was refused still has to light the picture.
    std::stable_partition(pending.begin(), pending.end(),
                          [](const Handle(V3d_Light)& light) { return light->ToCastShadows(); });
    for (const Handle(V3d_Light)& light : pending) {
        m_viewer->AddLight(light);
        m_lights.push_back(light);
    }
    m_viewer->SetLightOn();

    applyEnvironment();
    restart();
}

void AppearanceView::applyEnvironment() {
    if (m_view.IsNull()) return;

    if (m_lighting == Lighting::Sky) {
        // OCCT generates the cube map itself, so there is no image to ship, no
        // path to resolve and nothing to go missing from an installed tree.
        // The sun is put where the key light would be, so switching between Sky
        // and Studio moves the quality of the light and not its direction.
        Aspect_SkydomeBackground sky;
        sky.SetSunDirection(kSunTowards);
        sky.SetCloudiness(0.35f);
        sky.SetFogginess(0.05f);
        sky.SetSize(512);
        m_view->SetBackgroundSkydome(sky, Standard_True);
        m_view->ChangeRenderingParams().UseEnvironmentMapBackground = Standard_True;
        // Image-based lighting from the cube map, which is real but reaches
        // only the rasterized PBR preview: OCCT bakes the dome into an
        // irradiance probe for that shading model, and the path tracer does not
        // consult it. Under path tracing the dome is a backdrop and nothing
        // else -- a scene lit by it alone comes back black, measured rather
        // than assumed. applyLighting puts in the sun and sky fill that make
        // the picture agree with the backdrop.
        m_view->SetImageBasedLighting(Standard_True);
        return;
    }

    // Near-black, so a lit part reads as lit. A render on the viewport's
    // blue-grey gradient is a render of a part against a backdrop; this is a
    // render of a part.
    m_view->ChangeRenderingParams().UseEnvironmentMapBackground = Standard_False;
    m_view->SetBgGradientColors(Quantity_Color(0.06, 0.06, 0.07, Quantity_TOC_RGB),
                                Quantity_Color(0.01, 0.01, 0.01, Quantity_TOC_RGB),
                                Aspect_GFM_VER);
}

void AppearanceView::probeCapability() {
    if (m_probed || m_view.IsNull()) return;
    m_probed = true;

    TColStd_IndexedDataMapOfStringString info;
    try {
        m_view->DiagnosticInformation(info, Graphic3d_DiagnosticInfo(
                                                Graphic3d_DiagnosticInfo_Device |
                                                Graphic3d_DiagnosticInfo_Limits));
    } catch (const Standard_Failure&) {
        // Fall through with an empty map: no answer is not a capable answer.
    }

    const QString version = lookup(info, "GLversion");
    const QString device  = lookup(info, "GLdevice");
    const QString vendor  = lookup(info, "GLvendor");
    const double  gl      = parseGlVersion(TCollection_AsciiString(version.toUtf8().constData()));

    m_capable   = gl >= kMinGlForPathTracing;
    m_adaptive  = gl >= kMinGlForAdaptive;

    if (m_capable) {
        m_capabilityReport =
            QStringLiteral("Path tracing on %1 (OpenGL %2)")
                .arg(device.isEmpty() ? vendor : device,
                     version.isEmpty() ? QStringLiteral("?") : version);
    } else {
        // Named, not shrugged at. "Unsupported" without saying by what is a
        // dead end for whoever has to decide whether to change machines.
        m_capabilityReport =
            QStringLiteral("Path tracing needs an OpenGL %1 renderer. This machine "
                           "reports %2 on %3, so the Appearance view falls back to "
                           "the rasterized preview.")
                .arg(kMinGlForPathTracing, 0, 'f', 1)
                .arg(version.isEmpty() ? QStringLiteral("no OpenGL version at all")
                                       : QStringLiteral("OpenGL ") + version,
                     device.isEmpty() ? QStringLiteral("an unnamed device") : device);
        m_pathTracing = false;
    }

    emit capabilityDetermined(m_capable, m_capabilityReport);
}

void AppearanceView::applyRenderingParams() {
    if (m_view.IsNull()) return;

    Graphic3d_RenderingParams& p = m_view->ChangeRenderingParams();

    const bool trace = m_pathTracing && m_capable;

    p.Method                      = trace ? Graphic3d_RM_RAYTRACING
                                          : Graphic3d_RM_RASTERIZATION;
    p.IsGlobalIlluminationEnabled = trace;             // path tracing, not Whitted
    p.SamplesPerPixel             = trace ? 1 : 0;     // one per redraw; frames accumulate
    p.RaytracingDepth             = m_depth;           // OCCT defaults to 3, too few for glass
    p.IsAntialiasingEnabled       = Standard_True;
    p.IsTransparentShadowEnabled  = Standard_True;     // light through glass
    p.TwoSidedBsdfModels          = Standard_True;     // our solids are two-sided
    p.CoherentPathTracingMode     = Standard_False;
    p.AdaptiveScreenSampling      = trace && m_adaptive;
    p.RadianceClampingValue       = 30.0f;             // no fireflies off a bright emitter
    p.ToneMappingMethod           = m_tone == ToneMapping::Filmic
                                        ? Graphic3d_ToneMappingMethod_Filmic
                                        : Graphic3d_ToneMappingMethod_Disabled;
    p.Exposure                    = Standard_ShortReal(m_exposure);
    p.WhitePoint                  = Standard_ShortReal(m_whitePoint);

    // Depth of field. The tracer samples across a real aperture rather than
    // blurring afterwards, so it costs samples -- a wide aperture is noisier at
    // the same frame count -- and it does nothing at all in the rasterized
    // preview, which has no aperture to sample.
    p.CameraApertureRadius        = trace ? Standard_ShortReal(m_aperture) : 0.0f;
    p.CameraFocalPlaneDist =
        Standard_ShortReal(m_focal > 0.0 ? m_focal : std::max(1e-3, autoFocalDistance()));
    // MSAA is meaningless against a path tracer -- the estimate is already
    // supersampled -- and it is incompatible with the adaptive sampler.
    p.NbMsaaSamples               = trace ? 0 : 4;

    // The environment is a property of the lighting mode, not of the renderer,
    // and applyEnvironment owns it. Restated here because switching between
    // path tracing and rasterization rewrites the parameter block around it.
    p.UseEnvironmentMapBackground = m_lighting == Lighting::Sky;

    // The rasterized fallback reads the same materials, through the PBR half of
    // the mapping. Same scene at lower fidelity, not a different scene -- which
    // is the whole reason AppearanceMaterials fills both models from one
    // dispatch instead of leaving the preview on Phong.
    p.ShadingModel = Graphic3d_TypeOfShadingModel_Pbr;

    restart();
}

void AppearanceView::rebuildScene() {
    if (m_context.IsNull()) return;
    m_dirty = false;

    m_context->RemoveAll(Standard_False);
    m_build = appearance::buildScene(m_surfaces, m_showDetectors);
    for (const appearance::Part& part : m_build.parts)
        m_context->Display(part.shape, AIS_Shaded, -1 /* not selectable */, Standard_False);

    // How big the scene is, taken from the box the parts were just built into.
    // A point source's shadow softness is a fraction of this, because a
    // penumbra fixed in millimetres is right on a lens and invisible on a
    // luminaire.
    m_sceneSize = 100.0;
    if (!m_build.bounds.IsVoid()) {
        double xm, ym, zm, xM, yM, zM;
        m_build.bounds.Get(xm, ym, zm, xM, yM, zM);
        const double span = std::max({xM - xm, yM - ym, zM - zm});
        if (span > 1e-9) m_sceneSize = span;
    }

    // The emitters are part of the scene, not part of the lighting rig: a
    // source with a real area is a face with `Le` on it, and the reflector
    // around it is lit by the thing it was built around.
    m_emitters = appearance::buildEmitters(m_sources, m_sceneSize);
    for (const appearance::Emitter& e : m_emitters.emitters)
        if (!e.shape.IsNull())
            m_context->Display(e.shape, AIS_Shaded, -1, Standard_False);

    // The traced distribution on the nominated exit surface, as geometry.
    // `Graphic3d_BSDF::Le` is one Vec3 per material and OCCT 7.8.1 has no
    // emission map on the path-traced BSDF, so a distribution can only be a
    // grid of faces -- one `Le` each, taken from the bins it covers.
    m_radiance = appearance::RadianceMap();
    if (m_haveResult && m_radianceSource >= 0 &&
        m_radianceSource < int(m_result.detectors.size())) {
        // The per-band grid exists only for the first receiver, which is the
        // one SimulationResult mirrors its spectral split from. Any other
        // receiver renders neutral rather than borrowing a colour that is not
        // its own.
        static const std::vector<double> kNoBands;
        const bool spectral = m_radianceSource == 0 && m_result.spectral;
        m_radiance = appearance::buildRadianceMap(
            m_result.detectors[std::size_t(m_radianceSource)],
            spectral ? m_result.bandIrradiance : kNoBands);
        for (const appearance::RadianceFacet& f : m_radiance.facets)
            if (!f.shape.IsNull())
                m_context->Display(f.shape, AIS_Shaded, -1, Standard_False);
    }

    // The point sources among them are lights, so relighting has to follow the
    // build rather than precede it.
    applyLighting();
    restart();
}

void AppearanceView::setResult(const SimulationResult& result) {
    m_result     = result;
    m_haveResult = true;
    // A receiver index outlives the run it was chosen in only while the new run
    // still has that receiver.
    if (m_radianceSource >= int(m_result.detectors.size())) m_radianceSource = -1;
    m_dirty = true;
    if (!m_context.IsNull() && isVisible()) rebuildScene();
}

void AppearanceView::clearResult() {
    m_haveResult     = false;
    m_result         = SimulationResult();
    m_radianceSource = -1;
    m_dirty          = true;
    if (!m_context.IsNull() && isVisible()) rebuildScene();
}

void AppearanceView::setRadianceSource(int detectorIndex) {
    if (m_radianceSource == detectorIndex) return;
    m_radianceSource = detectorIndex;
    m_dirty          = true;
    if (!m_context.IsNull()) rebuildScene();
}

void AppearanceView::setSources(const std::vector<SourceConfig>& sources) {
    m_sources = sources;
    m_dirty   = true;
    if (!m_context.IsNull() && isVisible()) rebuildScene();
}

bool AppearanceView::hasAreaEmitters() const {
    for (const appearance::Emitter& e : m_emitters.emitters)
        if (!e.pointLike) return true;
    return false;
}

void AppearanceView::setLighting(Lighting mode) {
    if (m_lighting == mode) return;
    m_lighting = mode;
    applyLighting();
}

// ---------------------------------------------------------------------------

void AppearanceView::setSurfaces(const std::vector<OpticalSurface>& surfaces) {
    m_surfaces = surfaces;
    m_dirty    = true;
    // Deferred unless the tab is on screen. The presentations cost a
    // tessellation each, and a scene edit that nobody is looking at should not
    // pay for one -- the next paint builds what is current then.
    if (!m_context.IsNull() && isVisible()) {
        rebuildScene();
        if (!m_framed && !m_build.parts.empty()) fitAll();
    }
}

void AppearanceView::setShowDetectors(bool on) {
    if (m_showDetectors == on) return;
    m_showDetectors = on;
    m_dirty         = true;
    if (!m_context.IsNull()) rebuildScene();
}

void AppearanceView::setPathTracing(bool on) {
    if (m_pathTracing == on) return;
    m_pathTracing = on;
    applyRenderingParams();
}

void AppearanceView::setSampleBudget(int frames) {
    m_budget = std::max(1, frames);
    if (m_samples < m_budget && !m_view.IsNull()) m_timer->start();
    emit progress(m_samples, m_budget);
}

void AppearanceView::setRayDepth(int depth) {
    depth = std::clamp(depth, 1, 32);
    if (m_depth == depth) return;
    m_depth = depth;
    applyRenderingParams();
}

void AppearanceView::setExposure(double ev) {
    if (m_exposure == ev) return;
    m_exposure = ev;
    applyRenderingParams();
}

void AppearanceView::setWhitePoint(double white) {
    white = std::clamp(white, 0.1, 8.0);
    if (m_whitePoint == white) return;
    m_whitePoint = white;
    applyRenderingParams();
}

void AppearanceView::setToneMapping(ToneMapping mode) {
    if (m_tone == mode) return;
    m_tone = mode;
    applyRenderingParams();
}

void AppearanceView::restart() {
    m_samples = 0;
    // The camera has usually just moved, and focus distance 0 means "the middle
    // of the scene" -- a distance that follows the eye rather than a number
    // fixed when the aperture was opened. Written straight onto the parameter
    // block rather than through applyRenderingParams, which restarts.
    if (!m_view.IsNull() && m_aperture > 0.0 && m_focal <= 0.0)
        m_view->ChangeRenderingParams().CameraFocalPlaneDist =
            Standard_ShortReal(std::max(1e-3, autoFocalDistance()));
    if (!m_view.IsNull()) {
        m_view->Invalidate();
        m_timer->start();
    }
    emit progress(m_samples, m_budget);
}

void AppearanceView::fitAll() {
    if (m_view.IsNull()) return;
    m_framed = true;
    m_view->FitAll(0.05, Standard_False);
    m_view->ZFitAll();
    restart();
}

void AppearanceView::resetView() { setCamera(Camera::Iso); }

void AppearanceView::setCamera(Camera view) {
    if (m_view.IsNull()) return;
    // The optical axis is z and the scene is built around it, so "front" looks
    // along -y at the x-z plane the ray diagram already draws, and "side" looks
    // along x. Iso is the three-quarter view the tab opens on.
    switch (view) {
        case Camera::Front: m_view->SetProj(V3d_Yneg); break;
        case Camera::Side:  m_view->SetProj(V3d_Xpos); break;
        case Camera::Top:   m_view->SetProj(V3d_Zpos); break;
        case Camera::Iso:
        default:            m_view->SetProj(V3d_XposYnegZpos); break;
    }
    m_view->SetTwist(0.0);
    fitAll();
}

// ---- depth of field --------------------------------------------------------

double AppearanceView::autoFocalDistance() const {
    if (m_view.IsNull()) return 0.0;

    // The middle of what is drawn, not the camera's own centre of rotation:
    // after a pan those are different points, and the one worth being in focus
    // is the geometry.
    gp_Pnt target = m_view->Camera()->Center();
    if (!m_build.bounds.IsVoid()) {
        double xm, ym, zm, xM, yM, zM;
        m_build.bounds.Get(xm, ym, zm, xM, yM, zM);
        target = gp_Pnt(0.5 * (xm + xM), 0.5 * (ym + yM), 0.5 * (zm + zM));
    }
    return m_view->Camera()->Eye().Distance(target);
}

void AppearanceView::setApertureRadius(double radius) {
    radius = std::max(0.0, radius);
    if (m_aperture == radius) return;
    m_aperture = radius;
    applyRenderingParams();
}

void AppearanceView::setFocalDistance(double distance) {
    distance = std::max(0.0, distance);
    if (m_focal == distance) return;
    m_focal = distance;
    applyRenderingParams();
}

// ---- output ----------------------------------------------------------------

double AppearanceView::viewAspect() const {
    const int w = width(), h = height();
    if (w <= 0 || h <= 0) return 0.0;
    return double(w) / double(h);
}

bool AppearanceView::renderToImage(QImage& out, const appearance::ExportRequest& request,
                                   const ExportProgress& onProgress) {
    if (m_view.IsNull()) return false;

    // The window's estimate stands aside for the duration. Its accumulation
    // buffers are about to be resized under it, so it has already lost; what
    // this prevents is a paint arriving mid-loop -- from a progress dialog
    // pumping the event loop -- and redrawing the view at window size, which
    // would reset the offscreen estimate on every frame and produce noise at
    // any sample count.
    m_exporting = true;
    m_timer->stop();

    const Handle(Graphic3d_CView)& cview = m_view->View();
    Handle(Standard_Transient) previousFbo = cview->FBO();
    Handle(Standard_Transient) fbo;
    // The camera aspect has to match the buffer, or the render is the window's
    // framing stretched to the file's shape.
    const Standard_Real previousAspect = m_view->Camera()->Aspect();

    bool ok = false;
    int  drawn = 0;
    try {
        fbo = cview->FBOCreate(request.width, request.height);
        if (!fbo.IsNull()) {
            // What the driver actually gave us, which on a modest GPU can be
            // smaller than what was asked for.
            Standard_Integer w = request.width, h = request.height, wMax = 0, hMax = 0;
            cview->FBOGetDimensions(fbo, w, h, wMax, hMax);
            if (w > 0 && h > 0) {
                cview->SetFBO(fbo);
                m_view->Camera()->SetAspect(double(w) / double(h));
                // A new buffer size restarts OCCT's accumulation by itself; the
                // invalidate says so rather than relying on it.
                m_view->Invalidate();

                const bool trace = m_pathTracing && m_capable;
                // Rasterization has nothing to accumulate: one frame is the
                // whole answer and five hundred of them are the same answer.
                const int wanted = trace ? request.samples : 1;

                for (int i = 0; i < wanted; ++i) {
                    m_view->Redraw();
                    ++drawn;
                    if (onProgress && !onProgress(drawn, wanted)) break;
                }

                Image_PixMap pixels;
                pixels.SetTopDown(true);
                if (pixels.InitZero(Image_Format_BGR32, Standard_Size(w), Standard_Size(h)) &&
                    cview->BufferDump(pixels, Graphic3d_BT_RGB)) {
                    out = toQImage(pixels);
                    ok  = !out.isNull();
                }
            }
        } else {
            // No offscreen buffer of that size. OCCT's own dump path tiles the
            // render across several smaller ones, so it can still produce the
            // image -- at one sample, because each tile is drawn once and the
            // accumulation cannot be carried across them. A noisy 4K picture
            // with a caption that says how many samples it has beats no
            // picture, and the caller's caption says exactly that.
            Image_PixMap pixels;
            pixels.SetTopDown(true);
            if (m_view->ToPixMap(pixels, request.width, request.height, Graphic3d_BT_RGB)) {
                out   = toQImage(pixels);
                ok    = !out.isNull();
                drawn = ok ? 1 : 0;
            }
        }
    } catch (const Standard_Failure&) {
        ok = false;
    }

    // Everything put back, in the reverse order it was taken.
    if (!fbo.IsNull()) {
        cview->SetFBO(previousFbo);
        cview->FBORelease(fbo);
    }
    m_view->Camera()->SetAspect(previousAspect);
    m_exporting = false;
    m_view->MustBeResized();
    restart();

    return ok;
}

bool AppearanceView::saveImage(const QString& path) {
    if (m_view.IsNull()) return false;
    m_view->Redraw();
    return m_view->Dump(path.toUtf8().constData()) == Standard_True;
}

// ---------------------------------------------------------------------------

void AppearanceView::step() {
    // A hidden tab accumulates nothing. The pump is stopped rather than left
    // spinning, so switching away from the render costs nothing at all; the
    // next paint starts it again where it left off. An export owns the view
    // outright for its duration, for the same reason.
    if (m_view.IsNull() || m_exporting || !isVisible()) {
        m_timer->stop();
        return;
    }

    if (m_samples >= m_budget) {
        m_timer->stop();
        return;
    }

    m_view->Redraw();
    ++m_samples;
    emit progress(m_samples, m_budget);

    // Rasterization has nothing to accumulate: one frame is the whole answer,
    // so the pump stops immediately rather than burning the GPU redrawing an
    // identical picture five hundred times.
    if (!(m_pathTracing && m_capable)) {
        m_samples = m_budget;
        m_timer->stop();
        emit progress(m_samples, m_budget);
    }
}

void AppearanceView::paintEvent(QPaintEvent*) {
    initViewer();
    if (m_view.IsNull()) return;
    // A paint arriving while the offscreen render holds the view would redraw
    // at window size and reset the estimate it is accumulating. The export
    // repaints the window itself when it is done.
    if (m_exporting) return;
    if (m_dirty) {
        rebuildScene();
        if (!m_framed && !m_build.parts.empty()) fitAll();
    }
    m_view->Redraw();
    if (m_samples < m_budget) m_timer->start();
}

void AppearanceView::resizeEvent(QResizeEvent*) {
    if (m_view.IsNull() || m_exporting) return;
    m_view->MustBeResized();
    // A resized framebuffer is a different estimate; carrying the old sample
    // count over would report a converged image that is not there.
    restart();
}

// ---- navigation ------------------------------------------------------------
//
// Every one of these restarts the estimate. That is not a limitation being
// worked around, it is what a progressive renderer is: the image is an average
// over frames taken from one camera, and moving the camera invalidates every
// frame already in it.

void AppearanceView::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    if (m_view.IsNull()) return;
    m_lastMouse  = e->pos();
    m_dragButton = e->button();
    if (e->button() == Qt::LeftButton) m_view->StartRotation(e->pos().x(), e->pos().y());
}

void AppearanceView::mouseMoveEvent(QMouseEvent* e) {
    if (m_view.IsNull() || m_dragButton == Qt::NoButton) return;

    if (m_dragButton == Qt::LeftButton) {
        m_view->Rotation(e->pos().x(), e->pos().y());
        restart();
    } else if (m_dragButton == Qt::MiddleButton) {
        m_view->Pan(e->pos().x() - m_lastMouse.x(), m_lastMouse.y() - e->pos().y());
        restart();
    }
    m_lastMouse = e->pos();
}

void AppearanceView::mouseReleaseEvent(QMouseEvent*) { m_dragButton = Qt::NoButton; }

void AppearanceView::wheelEvent(QWheelEvent* e) {
    if (m_view.IsNull()) return;
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    m_view->SetZoom(std::pow(1.15, steps), Standard_True);
    restart();
}

void AppearanceView::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_F: fitAll();    return;
        case Qt::Key_R: resetView(); return;
        default: break;
    }
    QWidget::keyPressEvent(e);
}
