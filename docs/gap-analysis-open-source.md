# LuxTrace Against High-Star Open Source

Gap analysis · open-source landscape · focused on the high-star projects most
similar to LuxTrace.

**Subject** LuxTrace 1.0.0 · ~38k LoC C++20 · Qt 6.8.2 · OCCT 7.8.1
**Method** source audit + author knowledge of the open-source landscape.
Star counts are approximate, from memory, and are used only to rank relevance —
they are not the argument. The argument is what each project actually does.
**Scope** the high-star projects whose physics or geometry overlaps LuxTrace's.
Deliberately excluded: general 3D DCC tools (Blender, ~12k) and general
physical-simulation frameworks (Taichi, ~15k) — they are enormous and only
tangentially about optical design, so they are noted once and not audited line
by line.

---

## The one-sentence shape of the gap

LuxTrace is a **non-sequential Monte Carlo illumination engine with a thin
modeler**. The high-star projects that overlap it split into two camps, and the
gap is different against each:

- Against the **physically-based renderers** (LuxCoreRender, Mitsuba 3, PBRT-v4)
  LuxTrace is *ahead* on the illumination-specific analysis (irradiance,
  intensity, spot metrics, MTF, IES/EULUMDAT export, glass catalogues, coatings,
  polarisation) and *behind* on the transport algorithms (bidirectional path
  tracing, photon mapping, Metropolis, spectral rendering, GPU, differentiation).
- Against the **CAD / lighting-simulation tools** (FreeCAD, Radiance) LuxTrace
  is *ahead* on the physics and *behind* on the modeling surface and the
  architectural-lighting workflow.

The single most transferable lesson from all of them: **LuxTrace's own BVH and
intersection kernel are the one thing it should keep, and its forward-only
estimator is the one thing the renderers would all tell it to replace.**

---

## Category 1 — Physically-based Monte Carlo renderers

These are the closest cousins. All three are forward Monte Carlo light
transport engines with a BSDF/medium/emitter framework, which is exactly the
shape of LuxTrace's `SurfaceOptics` / `Material` / `Bsdf` / `Spectrum` core.

### LuxCoreRender (~2.5k stars) — the most directly comparable

The single most similar project in the open-source world, down to the name.
A physically-based renderer (the successor to LuxRender) with Monte Carlo path
tracing, light sources, a large BSDF/material library, volume scattering and
participating media, spectral rendering, and a plugin architecture. It is a
*renderer* — it produces photorealistic images, not photometric measurements —
but its physics core is the same family as LuxTrace's.

| Capability | LuxCoreRender | LuxTrace today | Verdict |
|---|---|---|---|
| Monte Carlo light transport | Bidirectional path tracing, photon mapping, Metropolis, path tracing | Forward-only + next-event estimation at diffuse bounces | **Gap** — LuxTrace has no backward/camera-side trace |
| GPU rendering | OpenCL, CPU+GPU hybrid | CPU-only (AVX2 scalar/wide leaf) | **Gap** |
| Spectral rendering | Full spectral | One wavelength sampled per ray | **Partial** |
| BSDF / material library | Very large, plugin-based | GGX, ABg, measured BSDF, Lambertian, HG volume | **Partial** — LuxTrace is narrower but pinned to catalogue values |
| Volume scattering / media | Participating media, heterogeneous | Scattering coefficient + HG phase, homogeneous | **Partial** |
| Photometric analysis | None (it is a renderer) | Irradiance, intensity, spot, MTF, IES/EULUMDAT | **LuxTrace ahead** |
| Glass catalogues / coatings / polarisation | Not a design tool | Full catalogue import, thin-film solver, Stokes/Mueller | **LuxTrace ahead** |
| Python API | Real Python API | stdio JSON runner | **Partial** |
| Plugin architecture | Yes | No — a new BSDF means editing `Bsdf.cpp` | **Gap** |

**The gap that matters:** LuxCoreRender's transport is *bidirectional* and
*GPU-accelerated*; LuxTrace's is forward-only and CPU-only. LuxTrace's
next-event estimation is the seed of a backward path, but there is no
camera-originated trace and no GPU path. Closing this is the difference between
"an illumination engine" and "an illumination engine that can also render what
it designs."

### Mitsuba 3 (~2.5k stars) — the differentiable one

A physically-based renderer built for **differentiable rendering** and inverse
design, with a plugin architecture, GPU support (CUDA/OptiX) and a real Python
API. It is the tool optics researchers actually use for inverse design.

| Capability | Mitsuba 3 | LuxTrace today | Verdict |
|---|---|---|---|
| Differentiable rendering | Core feature (Dr.Jit autodiff) | None — optimisation is Nelder-Mead / CMA-ES over scalar metrics | **Gap** — the biggest single one |
| Inverse design | Gradient-based, end-to-end | Black-box search only | **Gap** |
| GPU | CUDA/OptiX | CPU-only | **Gap** |
| Plugin architecture | Yes | No | **Gap** |
| Python API | Yes | stdio JSON runner | **Partial** |
| Photometric analysis | None | Full | **LuxTrace ahead** |

**The gap that matters:** Mitsuba 3 can differentiate the rendered image with
respect to the scene parameters, which is what makes gradient-based inverse
design possible. LuxTrace's `--optimise` is a black-box search over a scalar
metric. Gradient-based design is the commercially decisive capability in modern
optical design, and it is the one thing LuxTrace has no path to.

### PBRT-v4 (~5k stars) — the reference transport

The reference physically-based renderer from the *Physically Based Rendering*
book. It is the canonical implementation of every light-transport algorithm.

| Capability | PBRT-v4 | LuxTrace today | Verdict |
|---|---|---|---|
| Light transport algorithms | BDPT, MLT, photon mapping, path tracing, all reference implementations | Forward-only + NEE | **Gap** |
| Spectral rendering | Full | One wavelength per ray | **Partial** |
| Camera models | Full (realistic, thin-lens, etc.) | None — LuxTrace has no camera | **Gap** |
| Photometric analysis | None | Full | **LuxTrace ahead** |
| Illumination design | None | Full | **LuxTrace ahead** |

**The gap that matters:** PBRT-v4 is the reference for *how* to do light
transport. LuxTrace's forward-only estimator is the one place its physics is
behind the state of the art. The NEE machinery is a head start, but BDPT/MLT
are the algorithms that would make LuxTrace's error bars tighter still.

---

## Category 2 — Ray tracing kernels

### Embree (~5k stars) — the high-performance intersection engine

Intel's ray tracing kernels: BVH construction, traversal, and intersection for
CPU. This is the one project whose *engine* overlaps LuxTrace's `TraceScene` /
`TraceSceneSimd` BVH.

| Capability | Embree | LuxTrace today | Verdict |
|---|---|---|---|
| BVH / intersection | Industry-standard, heavily optimised, SIMD | Own SAH BVH + AVX2 wide leaf | **LuxTrace keeps its own** |
| Instancing | Yes | Yes (one mesh, many transforms) | **Parity** |
| SIMD | AVX2/AVX-512, wide | AVX2 four-wide leaf | **Parity** |
| Photometric analysis | None | Full | **LuxTrace ahead** |

**The gap that matters:** none, and that is the point. LuxTrace's own BVH was
built and benchmarked against the alternatives (the README documents a four-wide
BVH and float bounding boxes that were tried and removed). Embree would be a
dependency swap, not a capability gain — LuxTrace's kernel is already at the
level where the renderers' kernels live. This is the one place LuxTrace should
**not** adopt the high-star project.

---

## Category 3 — Lighting / illumination simulation

### Radiance (LBNL, ~1k stars) — the architectural lighting tool

The classic physically-based lighting simulation system from Lawrence Berkeley
National Laboratory. It is the open-source tool for architectural and daylight
lighting — the closest thing to the *illumination* half of LuxTrace's name.

| Capability | Radiance | LuxTrace today | Verdict |
|---|---|---|---|
| Illuminance / luminance | Full, the reference for architectural lighting | Lux on receiver, candela far-field; **no luminance** | **Partial** — LuxTrace lacks luminance (flux per projected area per steradian) |
| Daylighting | Full (sky models, sun, daylight coefficients) | Sky is a backdrop in the Appearance preview only | **Gap** |
| Radiosity | Yes | No | **Gap** |
| IES / photometric files | Reads and writes | Writes IES/EULUMDAT, does not read them back as sources | **Partial** |
| Glass / coatings / polarisation | Limited | Full | **LuxTrace ahead** |
| Non-sequential optical design | No | Full | **LuxTrace ahead** |

**The gap that matters:** luminance and daylighting. Radiance is the reference
for the *architectural* side of illumination — glare, daylight, luminance maps —
which LuxTrace does not do. LuxTrace's own README lists luminance as absent.
This is a real, addressable gap (the existing LightTools audit estimates it at
1–2 months, shared with the far-field work).

---

## Category 4 — CAD / geometric modeling

### FreeCAD (~20k stars) — the OCCT modeler

The highest-star project that shares LuxTrace's geometric kernel (OCCT). It is
a full parametric solid modeler; LuxTrace is a ray tracer with a thin modeler.

| Capability | FreeCAD | LuxTrace today | Verdict |
|---|---|---|---|
| Interactive solid modeling | Full (sketch, extrude, Boolean, fillet) | 17 fixed primitive types + STEP/IGES import + transform gizmo | **Gap** — the widest in the report |
| Optical surface shapes | General CAD | Sphere, paraboloid, cone, cylinder, plane, Fresnel | **Gap** |
| Assembly / groups / transforms | Full | Flat object list with parent ids, groups, gizmo | **Partial** |
| Physics / illumination | None | Full | **LuxTrace ahead** |

**The gap that matters:** modeling. FreeCAD shows what OCCT can do when the
modeling surface is exposed to the user. LuxTrace's geometry is hard-coded in
`GeometryProvider.cpp` / `SceneDocument.cpp`; the user cannot draw a part. This
is the same structural gap the LightTools audit identifies as the widest one,
and it is the one that gates everything downstream (an optimizer with nothing to
vary, a receiver with nothing to measure).

---

## The comparison table

| Project | Stars* | What it is | Overlap | LuxTrace ahead | Gap to close |
|---|---|---|---|---|---|
| **LuxCoreRender** | ~2.5k | Physically-based Monte Carlo renderer | Physics core | Photometric analysis, catalogues, coatings, polarisation | Bidirectional transport, GPU, spectral, plugin arch |
| **Mitsuba 3** | ~2.5k | Differentiable renderer | Physics core | Photometric analysis | **Differentiation / inverse design**, GPU, plugin arch |
| **PBRT-v4** | ~5k | Reference renderer | Transport algorithms | Photometric analysis, design | BDPT/MLT, spectral, camera models |
| **Embree** | ~5k | Ray tracing kernel | BVH/intersection | Photometric analysis | None — keep own kernel |
| **Radiance** | ~1k | Architectural lighting | Illumination | Optical design, catalogues | **Luminance, daylighting**, radiosity |
| **FreeCAD** | ~20k | OCCT CAD modeler | Geometric kernel | Physics | **Interactive modeling** |

\* Approximate, from memory, for ranking only.

---

## Verdict and roadmap

**The physics gap is narrow and mostly deliberate; the modeling gap is wide and
structural; the design-automation gap is the commercially decisive one.** This
is the same conclusion as the LightTools audit, and the high-star open-source
landscape confirms it from the other direction.

**What LuxTrace should keep (do not adopt the high-star project):**
- Its own BVH / intersection kernel. Embree would be a dependency swap, not a
  capability gain — LuxTrace's kernel is already at renderer-kernel level.
- Its photometric analysis. No high-star renderer does irradiance, intensity,
  spot metrics, MTF, IES/EULUMDAT export, glass catalogues, coatings or
  polarisation. This is LuxTrace's genuine lead.

**What LuxTrace should adopt (the real gaps, in priority order):**
1. **Differentiation / inverse design** (from Mitsuba 3). Gradient-based design
   is the modern, commercially decisive capability. LuxTrace's `--optimise` is
   black-box search. This is the biggest single gap.
2. **Bidirectional / backward transport** (from LuxCoreRender, PBRT-v4). The
   NEE machinery is the head start; a camera-originated trace is the same
   estimator with a different endpoint. This also unlocks photorealistic
   rendering.
3. **Luminance and daylighting** (from Radiance). LuxTrace's own README lists
   luminance as absent. Small, addressable, and it is the architectural side of
   the name.
4. **Interactive modeling** (from FreeCAD). The widest structural gap, and the
   one that gates everything downstream. Expose OCCT's Booleans and surface
   shapes to the user instead of hard-coding them in the scene builders.
5. **GPU rendering** (from LuxCoreRender, Mitsuba 3). CPU-only today; a GPU path
   would change the product's performance character.
6. **Plugin architecture** (from all three renderers). A new BSDF means editing
   `Bsdf.cpp` and recompiling. A plugin hook on the interaction path would make
   LuxTrace extensible the way the renderers are.

**The honest one-liner:** LuxTrace is a simulator with a thin modeler; the
high-star renderers are renderers with no photometric analysis. The gap is not
that LuxTrace is behind on physics — it is that it is forward-only, CPU-only,
non-differentiable, and cannot draw the part. Those four are the roadmap.
