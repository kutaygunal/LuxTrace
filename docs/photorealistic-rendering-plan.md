# Photorealistic Rendering — Implementation Plan

**Target:** LuxTrace 1.0.0 · OCCT **7.8.1** (vcpkg `x64-windows`) · Qt 6.8.2 · MSVC 2022
**Status:** All five phases implemented
**Estimated effort:** 5–6 weeks for one developer, in five phases that each ship something
**Written:** 29 August 2026

---

## Progress

| Phase | State | Notes |
|---|---|---|
| 1 — a rendered view at all | **done** | `AppearanceView`, own `V3d_View` on the viewport's shared driver, path-tracing params, GL 4.0 capability probe with a named fallback to the rasterized PBR preview, progressive accumulation with a sample budget, Appearance tab appended last so no existing tab index moved |
| 2 — real materials | **done** | `AppearanceMaterials` full dispatch; both `SetBSDF` and `SetPBRMaterial`; per-channel conductor from `indexAt`/`extinctionAt`; coating residual folded into the Fresnel of the layer that actually carries it; detectors hidden by default. 15 tests across `appearancematerials` and `appearancescene` |
| 3 — light | **done** | `AppearanceEmitters`: emissive faces at L = flux/(A pi) from the run's own `SourceConfig`s, coloured by integrating the source's SPD through `SampledSpectrum`, normalised scene-wide so ratios between sources are right; positional lights with soft shadows for point sources; a `Lighting` combo (Scene emitters / Studio / Sky) with OCCT's procedural skydome as the environment; exposure, white point and tone mapping in the toolbar. 9 tests in `appearanceemitters` |
| 4 — the simulation bridge | **done** | `RadianceMap`: a nominated receiver's binned flux becomes an N x N grid of emissive facets on that receiver's own frame, each with its own `Le`. The facets partition the bins exactly -- one bin to one facet, never split -- so the emitted total *equals* the measured total rather than approximating it, and the test asserts equality. Per-band colour from `bandIrradiance` on a spectral run. An "Emit from" combo lists the run's receivers; the caption names the surface, the facet grid and the flux it carries. 9 tests in `appearanceradiance`, including an end-to-end pass over all 26 registry scenes |
| 5 — output and integration | **done** | `AppearanceView::renderToImage` runs its own accumulation loop against an offscreen FBO, so the export is converged at the size asked for rather than one noisy sample of it; a resolution/sample dialog, a cancellable progress dialog, and the caption written into the PNG's own description field. The render is a captioned figure in the HTML report. Camera presets and path-traced depth of field with a focus distance that tracks the scene. `AppearanceExport` holds the wording so the disclaimer cannot be dropped from one call site; 6 tests in `appearanceoutput` |

Three corrections the implementation forced on the plan, all verified against
the running library rather than the headers:

- **`Graphic3d_BSDF::CreateGlass` puts its dielectric Fresnel on the *coat*
  layer** (`Kc` / `FresnelCoat`), not the base, and leaves `Ks` unused. So glass
  roughness goes in `Kc.a()` and an AR coating replaces `FresnelCoat`. Writing
  either to the base compiles, runs, and does nothing at all.
- **`Graphic3d_Fresnel::CreateConductor(Vec3, Vec3)` returns a *Schlick*
  factor**, not a conductor one: OCCT reduces the per-channel (n, k) to the
  specular colour they imply. That is still exactly what §4.2 wanted -- the
  metal reaches the shader as a colour derived from our own catalogue -- but the
  `FresnelType()` a test asserts on is `Graphic3d_FM_SCHLICK`.
- **§5.1's "the brightest source maps near 1.0" is the right structure and the
  wrong number.** One scene-wide divisor applied once is exactly right; 1.0 is
  not, because a surface emitting a radiance of 1 renders after filmic tone
  mapping at EV 0 as roughly mid grey — an emitter that does not look switched
  on, which is the one thing phase 3 exists to fix. The normalised value is
  multiplied by a single stated constant (`kBrightestRadiance`), and exposure
  moves it from there.

Phases 5 and the showcase scene forced four more, and the first was a crash
rather than a correction:

- **`Graphic3d_CLight::SetCastShadows` throws on a positional light.** OCCT 7.8
  implements shadow casting for directional and spot lights only, and phase 3
  asked every point source for it — so the Appearance tab aborted the whole
  application on any scene whose source is point-like, which is most of the
  library. It bought nothing either way: the header states shadow casting has no
  effect under ray tracing, where a shadow is a path that did not reach the
  light rather than something a light is asked for. Found by rendering the view
  headless against the GPU rather than by reading, which is the only way it
  could have been found.
- **Repeated `Redraw()` into a bound offscreen FBO does accumulate**, which is
  what makes the arbitrary-resolution export worth having rather than a noisy
  dump. Measured, not assumed: at 480 x 360 the high-frequency roughness of the
  image falls 0.179 → 0.140 → 0.132 → 0.131 across 1, 8, 64 and 256 samples
  while the mean level settles to three decimal places, and the render time
  scales linearly with the count. `V3d_View::ToPixMap` stays as the fallback for
  a buffer the driver will not allocate, at one sample, because its tiling
  cannot carry an estimate across tiles.

- **OCCT's path tracer does not sample emissive geometry.** It finds an
  emitting face only when a path happens to land on one, so phase 3's central
  mechanism — a source with an area becomes geometry with `Le` on it — lights
  nothing at all unless the face is large in frame. Measured: the same scene
  with a 50 mm emitter lights up and with a 5 mm one is black at every exposure
  the toolbar offers, the die visible as a bright speck and everything around it
  unlit. So every emitter now becomes *both* a face and a co-located light: the
  face is what is seen, the light is what illuminates. This is the ordinary
  arrangement wherever area lights are both sampled and visible.
- **A positional light's intensity has to be scaled by the scene.** OCCT's
  falls off as 1/d², so the fixed intensity phase 3 used lit a 6 mm lens
  brilliantly and a 600 mm luminaire not at all — at 100 mm the same number
  arrives as 3e-4. It is now multiplied by the square of half the scene's own
  size, which cancels the falloff and leaves the exposure control starting
  somewhere usable.
- **`SurfaceOptics::scatter` reached the render as nothing on a transmissive
  surface.** It is a Lambertian lobe, and the glass branch of the material
  bridge only understood a microfacet alpha, so every diffusing part in the
  library — the diffuser plate, a luminaire's opal cover — was drawn as clear
  glass. The Lambertian fraction is now spent out of the transmitted lobe and
  into a white diffuse one, which is what an opal diffuser is.
- **The Sky mode's dome is a backdrop and not a light.** `SetImageBasedLighting`
  is real and reaches only the rasterized PBR model; under path tracing an
  escaped path returns the flat background colour rather than the dome. Tested,
  not assumed. The mode now carries a sun and a sky fill read from the same
  constant the dome's sun direction is read from, so the lighting agrees with
  the backdrop.

One thing the plan did not anticipate: **the built-in scenes default to
`Shape::PointLike`**, so out of the box most of them light the render with
positional lights rather than emissive plates. The plate path runs the moment a
source is given a size in the source dialog, and for a collimated source it runs
immediately, `beamRadius` being a real aperture. Nothing is wrong here — it is
what the source model says — but "each cup visibly emitting" means each cup lit
by its own point light until somebody types a die size.

Phase 4 needed one decision the plan left open: **which surface is the
"nominated exit surface"**. It is a *receiver* of the last run. That is the only
thing in LuxTrace that already carries a binned spatial distribution, so
nominating anything else would have meant computing a second one -- new physics,
which §4 of the plan explicitly rules out. In practice this means: to render a
diffuser plate glowing with its own distribution, put a receiver on it. The
combo lists whatever the run measured.

The plan's 32 x 32 is a *target*, not the grid. The receiver's own bin count
caps it and each facet swallows a whole number of bins, because subdividing
finer than the data would draw structure the trace did not compute -- which in a
picture is indistinguishable from structure it did.

Also worth recording: **OCCT 7.8 can generate an environment cube map itself**,
through `V3d_View::SetBackgroundSkydome` and `Aspect_SkydomeBackground`. §3 of
the plan budgeted for shipping a small set of cube-map images and a file
browser; none of that is needed, there is no asset to install and nothing to go
missing from a deployed tree.

---

## 0. What this is, and what it is not

OCCT ships a GPU path tracer inside the visualization module. It is not a sample or a
plugin — it lives in the same `V3d_View` that `OcctViewWidget` already drives, and it is
switched on by changing two fields on `Graphic3d_RenderingParams`. So a lit-appearance
image of a luminaire is genuinely within reach, and at a cost measured in weeks.

What it is **not** is a photometric measurement. OCCT's path tracer is an RGB graphics
renderer with its own two-layer BSDF model. It does not consult `SurfaceOptics`,
`Coating`, `Material`'s Sellmeier data, `Bsdf`'s ABg or Henyey–Greenstein lobes, or
`Polarisation`. Its output is tone-mapped sRGB, not cd/m². If we ship it as an analysis
result, the application will contain two light-transport models that disagree with each
other, and the energy budget that closes to 1e-9 everywhere else will not close here.

**So it ships as an *Appearance preview*, explicitly labelled**, in exactly the way the
README already distinguishes the drawn diffraction-limit overlay from a computed PSF, and
GRIN, birefringence and fluorescence from what the tracer models. A stated boundary reads
as judgement. An unstated one reads as an omission.

This buys us:

- The image a design review and a datasheet want: *what does this part look like switched on*
- Emitter, reflector and diffuser geometry read at a glance, with real glass and real metal
- A second, independent visual check that the geometry being traced is the geometry intended

It does **not** close the photometric half of the gap: luminance in cd/m², angular and
spatial luminance meters, and backward photometric analysis all remain open and all remain
work on *our* tracer, not on OCCT's.

---

## 1. Two corrections to the source research

The research this plan came from is broadly right, and two points in it need adjusting
before they turn into work.

### 1.1 The ~6 000 ray-leg cap is not an architecture problem

The research warns against one `AIS_InteractiveObject` per ray and recommends a single
GPU buffer of segments with per-vertex colour. **That is already what LuxTrace does.**
See [OcctViewWidget.cpp:93](../src/ui/OcctViewWidget.cpp#L93):

```cpp
Handle(Graphic3d_ArrayOfSegments) arr =
    new Graphic3d_ArrayOfSegments(Standard_Integer(m_verts.size()), 0,
                                  Graphic3d_ArrayFlags_VertexColor);
```

One array, one custom presentation, per-vertex colour. The ~6 000 cap is a **legibility**
decision recorded in the README — a denser bundle draws as a solid tube that hides the
optic being examined — not a performance ceiling and not an OCCT limitation. It can be
raised independently of this work (thinning, per-leg alpha, or a density-aware subsample)
and should be tracked as its own item rather than folded in here.

Relevant to this plan for a different reason: **ray legs will not appear in the rendered
view at all.** OCCT's path tracer traces triangle geometry; line primitives are not part
of its BVH. That is correct behaviour for us — rays belong to the diagnostic view, and the
Appearance view exists to answer a different question.

### 1.2 One API name in the research is wrong, and two concepts are conflated

Verified against `/c/src/vcpkg/installed/x64-windows/include/opencascade` at
`OCC_VERSION_MAJOR 7`, `OCC_VERSION_MINOR 8`:

| Research says | OCCT 7.8.1 actually has |
|---|---|
| `params.CoherentPathTracingEnabled` | **`CoherentPathTracingMode`** |
| `Graphic3d_RM_RAYTRACING`, `IsGlobalIlluminationEnabled`, `RaytracingDepth`, `SamplesPerPixel` | correct as written |

And the conflation: `Graphic3d_PBRMaterial` (metallic / roughness / IOR / emission) drives
the **rasterized** PBR shading model, `Graphic3d_TypeOfShadingModel_Pbr`. The **path
tracer** consumes `Graphic3d_BSDF`. They are different objects on different code paths.
`Graphic3d_MaterialAspect` carries both, and `Graphic3d_BSDF::CreateMetallicRoughness()`
converts one into the other. The plan below sets both deliberately, so the fast raster
mode and the path-traced mode agree with each other.

---

## 2. Verified OCCT 7.8.1 API surface

Everything below was read out of the installed headers, not recalled.

### 2.1 Switching the renderer — `Graphic3d_RenderingParams`

```cpp
Graphic3d_RenderingParams& p = m_view->ChangeRenderingParams();

p.Method                      = Graphic3d_RM_RAYTRACING;   // vs Graphic3d_RM_RASTERIZATION
p.IsGlobalIlluminationEnabled = Standard_True;             // path tracing, not Whitted
p.SamplesPerPixel             = 1;                         // per redraw; frames accumulate
p.RaytracingDepth             = 8;                         // default 3 — too low for glass
p.IsAntialiasingEnabled       = Standard_True;
p.IsTransparentShadowEnabled  = Standard_True;             // light through glass
p.TwoSidedBsdfModels          = Standard_True;             // our solids are two-sided
p.CoherentPathTracingMode     = Standard_False;
p.AdaptiveScreenSampling      = Standard_True;             // spend samples where noisy
p.RadianceClampingValue       = 30.0f;                     // kill fireflies from the emitter
p.ToneMappingMethod           = Graphic3d_ToneMappingMethod_Filmic;  // or _Disabled
p.Exposure                    = 0.0f;
p.WhitePoint                  = 1.0f;
p.NbMsaaSamples               = 0;                         // MSAA is meaningless here
```

Also available and worth exposing later: `CameraApertureRadius` and `CameraFocalPlaneDist`
give real depth of field, path-tracing only. `RayTracingTileSize` and `NbRayTracingTiles`
tune the adaptive sampler. `RebuildRayTracingShaders` forces a shader rebuild after a
material change if one is ever needed.

### 2.2 Materials — `Graphic3d_BSDF`

OCCT uses a **two-layer** model, documented in the header: a base layer that is diffuse,
glossy or transmissive, covered by one glossy/specular coat. Public fields:

| Field | Type | Meaning |
|---|---|---|
| `Kc` | `Vec4` | weight of the **coat** specular/glossy BRDF |
| `Kd` | `Vec3` | weight of the base **diffuse** BRDF |
| `Ks` | `Vec4` | weight of the base **specular/glossy** BRDF |
| `Kt` | `Vec3` | weight of the base specular/glossy **BTDF** (transmission) |
| `Le` | `Vec3` | **radiance emitted by the surface** |
| `Absorption` | `Vec4` | volume scattering colour and density |
| `FresnelCoat` | `Graphic3d_Fresnel` | Fresnel reflectance of the coat layer |
| `FresnelBase` | `Graphic3d_Fresnel` | Fresnel reflectance of the base layer |

Factory methods (all `static`):

```cpp
Graphic3d_BSDF::CreateDiffuse (const Graphic3d_Vec3& weight);
Graphic3d_BSDF::CreateMetallic(const Graphic3d_Vec3& weight,
                               const Graphic3d_Fresnel& fresnel,
                               Standard_ShortReal roughness);
Graphic3d_BSDF::CreateTransparent(const Graphic3d_Vec3& weight,
                                  const Graphic3d_Vec3& absorptionColor,
                                  Standard_ShortReal absorptionCoeff);
Graphic3d_BSDF::CreateGlass  (const Graphic3d_Vec3& weight,
                              const Graphic3d_Vec3& absorptionColor,
                              Standard_ShortReal absorptionCoeff,
                              Standard_ShortReal refractionIndex);
Graphic3d_BSDF::CreateMetallicRoughness(const Graphic3d_PBRMaterial& pbr);
```

`Graphic3d_Fresnel` (declared in `Graphic3d_BSDF.hxx`, no header of its own):

```cpp
Graphic3d_Fresnel::CreateSchlick    (const Graphic3d_Vec3& specularColor);
Graphic3d_Fresnel::CreateConstant   (Standard_ShortReal reflection);
Graphic3d_Fresnel::CreateDielectric (Standard_ShortReal refractionIndex);
Graphic3d_Fresnel::CreateConductor  (Standard_ShortReal n, Standard_ShortReal k);
Graphic3d_Fresnel::CreateConductor  (const Graphic3d_Vec3& n, const Graphic3d_Vec3& k);  // per-channel
```

That **per-channel conductor overload is the one that matters to us** — see §4.2.

Attach with `Graphic3d_MaterialAspect::SetBSDF(...)` and `SetPBRMaterial(...)`.

### 2.3 Lights — `Graphic3d_CLight`

`SetIntensity(float)`, `SetSmoothRadius(float)` (positional — soft shadows),
`SetSmoothAngle(float)` (directional), `SetRange(float)`.

### 2.4 Environment and output

- `V3d_View::SetBackgroundCubeMap(const Handle(Graphic3d_CubeMap)&, ...)`, with
  `Graphic3d_CubeMapPacked` (single cross/strip image) or `Graphic3d_CubeMapSeparate`
  (six files). Set `UseEnvironmentMapBackground = true` to light the scene from it.
- `V3d_View::ToPixMap(Image_PixMap&, params)` for offscreen render at arbitrary resolution —
  this is how a render reaches the HTML report without a screenshot.
- `V3d_View::DiagnosticInformation(dict, Graphic3d_DiagnosticInfo)` for the capability
  probe in §6.1.

---

## 3. Architecture

The rule: **the renderer reads the scene, and nothing reads the renderer.** No existing
type gains a dependency on it, and deleting the whole subsystem must leave a working
application.

```
              SceneDocument  ──────────────┐
             (objects, optics,             │  reads
              placements, sources)         │
                    │                      ▼
                    │            AppearanceMaterials      ← new, core, no Qt widgets
                    │            SurfaceOptics -> Graphic3d_BSDF
                    │                      │              + Graphic3d_PBRMaterial
                    │                      ▼
        Simulation ─┴──> SimulationResult ─> AppearanceScene  ← new, builds AIS + emitters
                          (irradiance,              │
                           arrivals)                ▼
                                            AppearanceView    ← new, Qt widget
                                            (own V3d_View, path-tracing params,
                                             progressive accumulation, tone controls)
```

### New files

| File | Responsibility |
|---|---|
| `src/render/AppearanceMaterials.{h,cpp}` | The one place `SurfaceOptics` becomes `Graphic3d_BSDF`. Pure translation, no OCCT viewer types, unit-testable headless. |
| `src/render/AppearanceScene.{h,cpp}` | Walks a compiled `SceneDocument`, builds `AIS_Shape`s with those materials, builds emissive geometry for each source, and decides what is hidden (detectors). |
| `src/render/AppearanceView.{h,cpp}` | The Qt widget: its own `V3d_View`, the path-tracing parameter block, sample accumulation, exposure/tone controls, capability probe and fallback. |
| `src/render/RadianceMap.{h,cpp}` | Phase 4 only. Turns a `SimulationResult` into per-facet emitted radiance for a nominated exit surface. |

### Why a second `V3d_View` rather than a mode switch on the existing one

`OcctViewWidget` carries the walkthrough camera, the clipping plane, surface picking, the
transform gizmo and the ray overlay. None of those belong in a render, several would have
to be suppressed, and toggling `Method` on a live view resets accumulation on every camera
nudge the gizmo makes. A separate view in its own tab, sharing the `Graphic3d_GraphicDriver`
and `V3d_Viewer`, keeps both code paths simple. Cost is one extra GL context.

---

## 4. The material bridge — the crux of the work

This is where the plan either earns its keep or produces a plastic-looking toy. Everything
below reads data LuxTrace already has.

### 4.1 Dispatch

```
SurfaceOptics
   │
   ├── isDetector ─────────────► hidden in Appearance mode (§4.5)
   │
   ├── index == 0  (opaque) ────► metal or painted surface
   │     ├── material.valid() && material is a metal ──► CreateMetallic + CreateConductor
   │     └── otherwise ─────────► CreateDiffuse(reflectivity) with a glossy coat
   │
   └── index > 0   (dielectric) ► glass
         ├── effectiveBsdf() specular ──► CreateGlass
         └── effectiveBsdf() rough ─────► hand-built BSDF: Kt + Ks(roughness), FresnelBase
```

### 4.2 Metals — use our own complex index

`Material` already carries a complex refractive index for Al, Ag and Au, and `indexAt()`
already evaluates a material at a wavelength. So instead of guessing a colour, sample the
metal at three wavelengths and hand OCCT a per-channel conductor:

```cpp
// Representative sRGB primaries. Not colorimetrically exact — deliberately so; this is
// an appearance preview and the three-sample approximation is what makes gold look gold.
constexpr double kR = 610.0, kG = 550.0, kB = 465.0;

Graphic3d_Vec3 n(float(m.indexAt(kR)),     float(m.indexAt(kG)),     float(m.indexAt(kB)));
Graphic3d_Vec3 k(float(m.extinctionAt(kR)), float(m.extinctionAt(kG)), float(m.extinctionAt(kB)));

const float alpha = float(o.effectiveBsdf().alpha);   // GGX alpha, already the RMS-slope form
Graphic3d_BSDF bsdf = Graphic3d_BSDF::CreateMetallic(
    Graphic3d_Vec3(1.0f), Graphic3d_Fresnel::CreateConductor(n, k), alpha);
```

This is the detail that makes the render *ours* rather than generic: an aluminium reflector
and a gold one will differ for the same reason they differ in the tracer.

> **Check before writing:** confirm the accessor name for the extinction coefficient on
> `OpticalMaterial` — the plan assumes something like `extinctionAt(λ)` alongside
> `indexAt(λ)`. If only a complex-valued accessor exists, take its imaginary part.

For a non-metal opaque surface (a painted housing, a white cavity), `CreateDiffuse` weighted
by `reflectivity`, with `Kc` and `FresnelCoat = CreateSchlick(...)` giving a slight sheen
where `roughness` is low. An integrating-sphere wall should come out matte white and does.

### 4.3 Glass — index and absorption straight from the optic

```cpp
const float n     = float(o.indexAt(587.6, /*dispersion*/ false));
const float alpha = float(o.absorption);     // Beer-Lambert, 1/mm — scene units are mm

Graphic3d_BSDF bsdf = Graphic3d_BSDF::CreateGlass(
    Graphic3d_Vec3(1.0f),          // weight
    absorptionColour(o),           // from the material's internal transmittance, §4.4
    alpha,
    n);
```

Dispersion is **not** available to the path tracer — OCCT traces RGB, not wavelengths — so
a prism will not split light in the Appearance view. Say so in the UI tooltip. The Ray
Diagram and the Irradiance tab are where dispersion is shown, and they already show it.

### 4.4 Coatings — fold the residual into `FresnelBase`

OCCT's coat layer (`Kc` / `FresnelCoat`) *adds* a reflecting layer. An AR coating does the
opposite. So do not model an AR coating as an OCCT coat. Instead, replace the base layer's
dielectric Fresnel with a constant at the coating's residual reflectance:

```cpp
if (o.coating.isAntiReflective())
    bsdf.FresnelBase = Graphic3d_Fresnel::CreateConstant(float(o.coating.residualAt(550.0)));
```

An HR coating on a dielectric becomes `CreateConstant(R)` with a high `R` and `Kt` zeroed.
This is an approximation, it is the right one for appearance, and it means a coated lens
does not read as a bare one — a visible ~4 % per surface difference at grazing angles.

Absorption colour comes from the material's internal transmittance sampled at the same three
wavelengths as §4.2, so a thick piece of a yellowish glass tints correctly.

### 4.5 Detectors and hidden objects

A receiver is a measurement plane, not a part. In the Appearance view it would sit in front
of the optic and block the shot. Default: **detectors hidden**, with a checkbox to show them
as a faint matte target for framing. `SceneDocument::effectiveVisible()` already answers the
visibility question for everything else, and the Appearance scene honours it unchanged.

---

## 5. Sources — making the light come from the light

### 5.1 Emissive geometry, not lights, for area sources

`Graphic3d_BSDF::Le` is emitted radiance per surface. For a source with a real emitting area
this is exactly right: build a small plate (rectangular `sizeA × sizeB`) or disc (`beamRadius`)
at the source's world placement, and set `Le`.

The radiance value should be *relative but physically ordered*, so that two sources of
different power and different colour temperature look right against each other:

```
Lambertian emitter:   L = Φ / (A · π)        [W/m²/sr]
```

Convert to RGB through the machinery `Spectrum` already has — the linear sRGB weights for a
monochromatic stimulus, integrated over the source's SPD — then divide by a scene-wide
normalisation so the brightest source maps near 1.0 and `Exposure` does the rest. Store the
normalisation factor so it is stable across redraws; recompute it only when sources change,
otherwise the image will breathe.

**This is the honest half-step.** It gives correct *relative* brightness and correct colour
between sources. It is not calibrated cd/m², and the UI must not imply that it is.

### 5.2 Point sources

An isotropic point source has no area to emit from. Use a real OCCT light:

```cpp
Handle(V3d_PositionalLight) light = new V3d_PositionalLight(position);
light->SetIntensity(intensity);
light->SetSmoothRadius(0.5f);     // > 0 gives soft shadows; 0 is a hard point light
```

### 5.3 Measured ray files

A `RayFileData` source has an emitting-surface extent (`RayFile.h` records it). Use that
extent as the emissive plate and its integrated spectrum as the colour. The individual rays
are not used by the renderer — that is the point of §0.

---

## 6. Robustness

### 6.1 Capability probe, before showing the tab

OCCT falls back to rasterization silently if path tracing is unsupported, which reads as
"the render button does nothing". Probe up front:

```cpp
TColStd_IndexedDataMapOfStringString info;
m_view->DiagnosticInformation(info, Graphic3d_DiagnosticInfo_Device
                                  | Graphic3d_DiagnosticInfo_Limits);
```

Require an OpenGL 4.0-class context. Below that, disable the tab and say why, naming the
renderer and version reported — the same standard the CAD importer already meets by
reporting a bad file rather than throwing.

### 6.2 Progressive accumulation and input freezing

Frames accumulate while the camera is still; any camera change restarts the estimate. So the
Appearance tab needs the same discipline as a trace: show the accumulated sample count, let
the user stop when it is good enough, and freeze the material and source inputs for the
duration. `SimulationWorker`'s pattern — freeze, progress, cancel — is the precedent, though
no worker thread is needed here since accumulation happens on the GL thread between redraws.

### 6.3 Determinism

Do not claim it. The path tracer is a GPU estimator with no seed we control, and two runs
will differ in the last bits. Everywhere else in LuxTrace determinism is a guarantee checked
in CI; here it is not, and the difference must be stated rather than discovered. This is one
more reason the render is not an analysis output.

---

## 7. Phases

Each phase ends with something usable, and each is independently revertible.

### Phase 1 — A rendered view at all *(≈1 week)*
- `AppearanceView` widget with its own `V3d_View` sharing the existing driver and viewer
- Path-tracing parameter block from §2.1, hard-coded
- Capability probe and graceful disable (§6.1)
- Every object rendered with a single stand-in material; scene geometry only
- **Done when:** a parabolic reflector renders, converges visibly, and does not crash on
  a machine without a capable GPU

### Phase 2 — Real materials *(≈1.5 weeks)*
- `AppearanceMaterials`: the full §4 dispatch, metals through the per-channel conductor
- Both `SetBSDF` and `SetPBRMaterial`, so the raster preview matches the traced render
- Coating residual folded into `FresnelBase`
- Detector hiding
- **Done when:** the ball lens reads as glass, the aluminium reflector as aluminium, the
  gold-coated one visibly different, and the integrating sphere as matte white

### Phase 3 — Light *(≈1 week)*
- Emissive plates and discs from `SourceSpec`, radiance per §5.1
- Positional lights with soft shadows for point sources
- Environment cube map with a small built-in set (studio, sky, dark) plus a file browse
- Exposure, white point and tone-mapping toggle in the UI
- **Done when:** the LED array luminaire renders lit, with each cup visibly emitting

### Phase 4 — The simulation → render bridge *(≈1.5 weeks)*
This is the phase that makes the feature distinctly LuxTrace's rather than a generic viewer.

Take the traced result on a nominated exit surface — a diffuser plate, a light-guide exit
face — and drive the emitted radiance from it, so the render shows *the distribution the
trace computed* rather than a uniform glow.

**The constraint, verified:** `Graphic3d_BSDF::Le` is a `Vec3` per material. OCCT 7.8.1 has
no texture-driven emission map on the path-traced BSDF. So the distribution must become
geometry:

- Subdivide the nominated exit face into an N×N grid of facets (32×32 = 1 024 is ample and
  costs nothing in a static render)
- Give each facet its own `AIS_Shape` and its own `Le`, sampled from the irradiance grid
- Reuse `SimulationResult`'s existing grid directly; no new physics

The alternative — patching OCCT's GLSL — is not worth it and would fork the dependency.

- **Done when:** a diffuser plate that the trace shows as bright-centre / dim-edge renders
  that way, and switching the source moves the bright region

### Phase 5 — Output and integration *(≈1 week)*
- Offscreen render at arbitrary resolution via `ToPixMap`, so quality is not tied to window size
- PNG export, and a render embedded in the one-click HTML report, captioned as an appearance
  preview with the sample count and the fact it is not a photometric result
- Camera presets and depth of field via `CameraApertureRadius` / `CameraFocalPlaneDist`
- Documentation: a **Notes / limits** entry and a line in the README capability table that
  says *appearance preview*, not *rendering*

---

## 8. Testing

The renderer cannot be pinned against arithmetic the way the tracer is, so test what can be
tested and be honest that the image itself is reviewed by eye.

| Suite | What it asserts |
|---|---|
| `appearancematerials` | Headless, no GL. Every `SurfaceOptics` in the 26-scene library maps to a `Graphic3d_BSDF` with finite, in-range weights; a mirror produces `Kd == 0`; glass produces `Kt > 0` and the right `FresnelBase` index; an AR-coated surface produces a lower base reflectance than a bare one; a detector maps to hidden. |
| `appearancescene` | A compiled `SceneDocument` produces one presentation per visible object and none for hidden ones; source count matches emitter count; the radiance normalisation is stable across two identical builds. |
| `appearanceradiance` | Phase 4. The facet grid's total emitted power matches the `SimulationResult` grid it was built from, to a stated tolerance. |
| `appearanceshowcase` | The demonstration scene: that it carries a metal, a transmissive and a matte surface for the material bridge to exercise; that its floor and base stay behind the emitter, so a prop for the picture never intercepts light on its way to the receiver; and that the scene declares the emitter it is built around while every other scene declines to. |
| `appearanceoutput` | Phase 5. Headless. What a typed resolution becomes — clamped to what a GPU will allocate, a missing dimension filled from the view's aspect — and, the one that matters, that every caption the module can produce still carries "not a photometric result", names the receiver that drove the emission and quotes its flux in the run's own unit. |
| manual | A checked-in reference render per scene, reviewed by eye on change. Not a CI gate — GPU output is not bit-stable and asserting on pixels would produce a permanently red build. |

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Path tracing unsupported on the target machine | Probe and disable with a named reason (§6.1). The feature is additive; nothing else depends on it. |
| Second GL context costs memory or conflicts with the main viewport | Share `Graphic3d_GraphicDriver` and `V3d_Viewer`. If it still conflicts, fall back to toggling `Method` on the existing view and accept the accumulation resets. |
| The render is mistaken for an analysis result | Labelled in the tab, in the export caption, in the report, and in the README's limits section. This risk is managed by writing, not by code. |
| Material mapping produces plastic-looking output | Phase 2 is where this is won or lost; the per-channel conductor (§4.2) is the specific reason it should not. Budget review time at the end of Phase 2 rather than at the end of Phase 5. |
| Scope creep toward "make the render photometric" | It isn't, and making it so is the 9–18 month backward-tracing item in the gap analysis, not this. Any request to read cd/m² off this view is a different project. |

---

## 10. What this deliberately does not do

Stated here so it does not have to be rediscovered:

- **No luminance.** The render has no cd/m². Angular and spatial luminance meters remain open.
- **No spectral rendering.** RGB only, so no dispersion, no colour-over-angle from the render.
- **No polarisation, no coatings beyond a residual, no ABg or Henyey–Greenstein.** The path
  tracer's material model is its own.
- **No rays in the render.** Line primitives are not path traced; the diagnostic view keeps them.
- **No determinism guarantee**, unlike every other output the application produces.
- **It does not advance backward photometric analysis.** OCCT's path tracer is camera-side,
  but it is camera-side with someone else's physics.

---

## 11. Effort summary

| Phase | Scope | Estimate |
|---|---|---|
| 1 | Rendered view, params, capability probe | 1 week |
| 2 | Material bridge | 1.5 weeks |
| 3 | Emissive sources, environment, tone controls | 1 week |
| 4 | Simulation-driven radiance on an exit surface | 1.5 weeks |
| 5 | Offscreen output, report integration, docs | 1 week |
| | **Total** | **≈6 weeks** |

Phases 1–3 alone (≈3.5 weeks) produce a shippable appearance preview. Phase 4 is what makes
it LuxTrace's rather than any CAD viewer's, and is the one worth defending if the schedule
compresses.

---

## References

- Installed headers: `C:/src/vcpkg/installed/x64-windows/include/opencascade`
  (`Graphic3d_RenderingParams.hxx`, `Graphic3d_BSDF.hxx`, `Graphic3d_PBRMaterial.hxx`,
  `Graphic3d_CLight.hxx`, `Graphic3d_ToneMappingMethod.hxx`, `V3d_View.hxx`)
- Open Cascade's own CADRays is the existence proof: a GPU path tracer built on this same
  engine, reading STEP/IGES/BREP directly.
- [Gap analysis II — Illumination Module](gap-analysis-illumination-module.html), item 01,
  which this plan partially closes and item 02, which it does not.
