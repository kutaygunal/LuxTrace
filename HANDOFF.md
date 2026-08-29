# Handoff — LuxTrace (Optical Design Studio)

Location: `C:/Users/kutay/Desktop/Projects/LightTools` (the folder still carries
the working name; the product, the executable and the CMake project are
**LuxTrace**)
Stack: **C++20, Qt 6.8.2 (MSVC 2022 64-bit), OpenCASCADE Technology 7.8.1 (vcpkg, x64-windows)**

**Naming.** `lux` is the SI unit of illuminance the app computes; `trace` is the
ray tracing. It reads alongside LightTools, TracePro and OpticStudio, and it is
short, pronounceable and spellable. The CMake project, the target, the
executable, the window title and the version block all say LuxTrace; the internal
library stays `optics_core`, since that is the engine rather than the product.

**Icon.** `resources/make_icon.py` generates the SVG, the PNG set and the
multi-size `.ico` from one set of geometry constants -- a converging ray bundle
through a biconvex lens. The generator is the checked-in source of truth rather
than the images, so the icon can be re-cut at any size and the three formats
cannot drift. Windows needs both resources: `LuxTrace.rc` carries the icon the
shell reads out of the executable before the process starts, and `luxtrace.qrc`
carries the one the running window uses.

---

## What this is

A desktop app that demonstrates LightTools-style optical design using OCCT as
the geometric kernel and Qt for the UI. Builds B-Rep geometry, runs a Monte Carlo
ray trace, and shows a detector irradiance heatmap.

| File | Responsibility |
|------|----------------|
| `CMakeLists.txt` | Builds `optics_core` (static lib) + the app + the tests; CTest wired up |
| `src/core/Vec3` | Small double-precision vector type used by the hot loops |
| `src/core/SurfaceOptics` | **The optical properties**, inherited by all three surface representations |
| `src/core/Optics` | Fresnel, Cauchy dispersion, cosine hemisphere, slope error, counter-based RNG |
| `src/core/GeometryProvider` | **Scene registry**: 25 parametric OCCT B-Rep scenes + their source placement, behind `count()` / `info()` / `paramInfo()` / `build()` |
| `src/core/MeshBuilder` | `BRepMesh_IncrementalMesh` tessellation -> flat triangle meshes |
| `src/core/TraceScene` | Flattened triangles + **SAH BVH**; `intersectTriangle`, `nearestHit`, and a brute-force reference |
| `src/core/RayTracer` | Monte Carlo tracer: multithreaded, cancellable, deterministic |
| `src/core/SimulationResult` | Irradiance grid, per-band grids, far-field intensity, receiver arrivals, energy accounting, timings |
| `src/core/Simulation` | Facade: geometry -> mesh -> BVH (cached per scene *and* parameters) -> trace -> result |
| `src/core/Analysis` | Spot metrics, cross-sections, encircled energy, through-focus propagation, CSV export |
| `src/core/Studies` | Convergence and through-focus sweep drivers |
| `src/core/ConfigIO` | JSON save/load of a whole `SimConfig` |
| `src/core/SimulationWorker` | `QThread` wrapper: async run, progress, cancellation |
| `src/core/StudyWorker` | The same, for a convergence sweep |
| `src/ui/OcctViewWidget` | **Embedded OCCT 3D viewport** with orbit / pan / zoom, WASD walkthrough, per-vertex ray colouring, a clipping plane and surface picking |
| `src/ui/HeatmapWidget` | Irradiance map: five colour maps, linear/log/sqrt scaling, RGB mode, movable cut line |
| `src/ui/PolarPlotWidget` | Far-field polar diagram (C0/C90 meridians) and angular map |
| `src/ui/PlotWidget` | Line plots with error bars, reference markers and a hover readout |
| `src/ui/*` | MainWindow (five tabs + menus + exports), ControlsPanel, RayDiagramWidget, Palette |
| `resources/` | Icon generator + the SVG/PNG/ICO it emits, the Qt resource and the Windows resource script |
| `src/main.cpp` | Entry + `--smoke <n> [threads]`, `--study <scene> <n>`, `--bench <n>`, `--meshcheck` |
| `test/TestMain.cpp`, `test/NewTests.inc` | 108 unit tests in 22 CTest suites, no external framework |

---

## Performance work (an earlier pass)

The tracer used to brute-force every triangle for every ray segment on the GUI
thread. Same workload, before and after:

| Workload | Before | After |
|---|---|---|
| 3 scenes x 30 000 rays (wall clock) | **48.1 s** | **0.17 s** |
| 3 scenes x 1 000 000 rays | ~27 min (extrapolated) | **1.35 s** |

Roughly **280x** end to end. Four things got it there:

1. **SAH BVH** over the flattened triangles (`TraceScene`), replacing the
   O(rays x tris x depth) scan. Reflector 2 486 tris -> depth-16 tree.
2. **Multithreading** over rays, dynamically scheduled in 512-ray chunks so
   uneven paths (a TIR bounce chain costs orders of magnitude more than an
   escaping ray) keep every core busy. 9-13x on 24 threads.
3. **Cached geometry**: `Simulation::sceneFor` builds OCCT tessellation + BVH
   once per scene and reuses it. Re-running the same scene skips ~15 ms of setup.
4. **Bounded path recording**: only the first 2 000 rays contribute diagram
   segments (capped at 30 000 total). The diagram subsamples anyway, and the old
   code recorded up to 200 000 segments per run.

Per-scene throughput (24 threads): reflector ~17 M rays/s, light guide ~9 M
rays/s, ball lens ~0.85 M rays/s. The lens is inherently the slow one: every hit
splits into a reflected and a transmitted branch, so a path is a tree, not a
chain.

### Determinism
Results are **bit-identical regardless of thread count**. Two things guarantee it:
- Each ray seeds its own RNG from its global index, so sampling never depends on
  scheduling.
- Scalar totals accumulate **per chunk** and reduce in chunk order. Reducing per
  thread would make the totals depend on which thread grabbed which chunk, in
  the last few ULPs. `--bench` asserts this, prints `[deterministic]` per row
  and exits non-zero if any row is not, which is what makes it a CI check
  rather than a remark.

The one exception, deliberately: the irradiance **bins** accumulate per thread
(a per-chunk 64x64 grid would cost 32 KB per chunk), so individual bins can
differ by ~1e-15 between thread counts. Tested with a tolerance, not exactly.

---

## UI: the simulation does not block the window

`SimulationWorker` (a `QThread`) runs the trace off the GUI thread.
- The **inputs freeze** for the duration of a run (scene, source, ray count and
  Run are disabled) so the configuration cannot change under a running trace.
- The **window itself stays responsive** — it repaints, moves and closes.
- A **progress bar** shows percent complete, starting in marquee mode while the
  one-off geometry + mesh + BVH build happens, then switching to 0-100 %.
- **Cancel** sets an atomic flag the workers poll at chunk boundaries; it stops
  within a few milliseconds. A cancelled run keeps the rays that did finish,
  normalises by them, and is displayed with a `CANCELLED - partial result` note.
- The result panel reports trace seconds and rays/s.

---

## The scene library (25 scenes)

The three original scenes became a flat registry. `GeometryProvider::Scene` is
now enumerated through `count()` / `info()`, so the combo box, `--smoke`,
`--bench`, `--meshcheck` and the tests all pick up a new scene with no edit of
their own; `SceneInfo` also carries the source placement, which used to be a
hardcoded switch inside `Simulation::sourceFor`.

Nine reflective (parabola, ellipsoid, sphere, off-axis parabola, trough, CPC,
cone, Cassegrain, corner cube), ten refractive (ball, plano-convex, biconvex,
plano-concave, half-ball, rod, Fresnel, axicon, microlens array, prism), four TIR
(round / square / tapered guides, Porro prism) and two scattering (integrating
sphere, diffuser plate). Built from revolved spline and polyline profiles,
extruded profiles, OCCT primitives and booleans; helpers for each live at the top
of `GeometryProvider.cpp`.

Every scene is now **parametric**: `paramInfo(scene)` returns 2-4 editable
dimensions with ranges, steps and tooltips, and the UI builds its spin boxes from
that rather than from a hardcoded list. The last slot is always the receiver
position, which is what makes moving the detector a parameter change rather than
a special case. `build()` returns the source placement alongside the shapes,
because a parametric scene moves its own focus -- lengthen a paraboloid and the
emitter has to follow it or the scene silently stops being a collimator
(`params.move_the_source_with_the_optic_that_focuses_it`).

**Every scene was validated numerically, and two descriptions did not survive it.**
A scratch harness dumped the irradiance moments per scene, which showed:
- The **axicon** peaked at the *centre* (0.89 of peak), not as a ring. A cone
  only makes a ring from collimated light, and a diverging point source is not
  collimated. Fixed by putting a paraboloid collimator in front of it -- the
  centre is now at 0.16 of peak with the peak 14 bins out.
- The **cylindrical rod lens** was barely anisotropic (sd 15.6 vs 17.6) because
  the receiver sat well past the line focus and rays bypassing the rod washed it
  out. Fixed by moving the receiver to the image plane and sizing it so stray
  rays land outside it: now 9.8 vs 16.5.

Two descriptions were reworded instead, because the claim rather than the optic
was wrong: the prism deviates in one plane (it does not make "a pair of lobes"),
and the half-ball dome's benefit is escape probability, not beam narrowing.

Those claims are now tests (`patterns.*`), so they cannot rot: round scenes stay
round, one-axis optics stay elongated, the axicon keeps its dark centre, the
Cassegrain keeps its central obstruction, the CPC keeps beating a plain cone, and
the two retroreflectors are checked to have *no direct path* to their receiver,
so any flux they report is genuinely returned light.
`trace.every_registered_scene_is_well_formed` guards the rest: geometry present,
exactly one detector, sane optical properties, and light actually arriving.

---

## The 3D viewport

Previously nothing in the app rendered through OCCT: both right-hand panes were
hand-drawn QPainter output (the X-Z ray diagram and the irradiance heatmap), with
OCCT used purely as a geometry kernel. `OcctViewWidget` adds a real OCCT viewport.

- `V3d_Viewer` + `OpenGl_GraphicDriver` on a native `WNT_Window` taken from the
  widget's `winId()`. The widget sets `WA_PaintOnScreen` / `WA_NativeWindow` and
  returns a null `paintEngine()` so Qt never paints over OCCT's surface.
- Geometry is displayed as `AIS_Shape` per `OpticalSurface`, colour-coded and
  part-transparent by role: mirrors grey, refractive solids glassy blue,
  detectors green.
- Ray paths are one custom `AIS_InteractiveObject` (`RayCloud`) wrapping a single
  `Graphic3d_ArrayOfSegments`. One `AIS_Shape` per segment would mean tens of
  thousands of presentations; this is one primitive array. The display is thinned
  to ~4 000 legs because a full bundle draws as a solid tube that hides the optic
  it is bouncing off. The trace itself always uses every ray.
- The 3D view and the 2D X-Z diagram sit in a `QTabWidget`, so the flat
  projection is still available rather than replaced.
- Ray segments are now stored in 3D (`RaySegment` holds two `Vec3`). They used to
  be flattened to `QPointF(x, z)` at trace time, which threw away the Y component
  the viewer needs. The 2D diagram projects them on the way to the screen.

**Perspective is on by default and this matters.** OCCT's camera defaults to
orthographic, and under an orthographic projection translating the eye and the
centre together along the view axis produces no visual change whatsoever -- W/S
appear completely dead even though the camera really is moving. This cost a
debugging cycle: instrumentation confirmed key delivery, focus, and the camera
translation were all correct while nothing moved on screen. The **Perspective**
checkbox toggles back to orthographic for CAD-style inspection, at the cost of
the walkthrough.

---

---

## Physics, analysis and workflow pass

### Physics added

| Feature | Where | Effect |
|---|---|---|
| **Fresnel reflectance** | `optics::fresnelReflectance`, applied per refractive hit | 4 % at normal incidence rising to 1 at grazing, exactly 1 past the critical angle. Per-surface opt-in (`SurfaceOptics::fresnel`) with a global gate, so the old fixed split is still reachable for comparison |
| **Beer-Lambert absorption** | `tracePath`, over the leg just travelled | `exp(-alpha t)` inside a medium, reported separately as `fluxBulkAbsorbed`. 4.4 % of the light in the round guide |
| **Diffuse scattering** | `optics::cosineHemisphere`, Russian roulette per branch | Diffusers, matte white reflectors, integrating spheres |
| **Surface roughness** | `optics::perturbNormal` | Gaussian slope error on the interaction normal, so a mirror deviates a reflected ray by twice it without a separate factor |
| **Dispersion** | `optics::cauchyIndex` + per-band irradiance grids | Three RGB bands through n(lambda); the heatmap paints them as real colour |

The three properties that are ray-time rather than geometric (roughness, scatter,
absorption) also have **scene-wide overrides** in `PhysicsOptions`, so they can be
swept without rebuilding geometry.

Medium tracking changed from a bool to the surface index of the medium the ray is
inside, which is what lets the right glass's absorption and dispersion be looked
up on the way out. It deliberately treats *any* transmission while already inside
a medium as an exit to vacuum: comparing the medium's identity against the
surface hit would be sharper for nested solids, but it would also mean a body
modelled as two sheets could never refract out of itself -- and that is exactly
how the TIR tests set up an exact angle of incidence.

### Sources

`SourceConfig` split into three independent axes: an angular law (point /
Lambertian / collimated) with a **cone half-angle**, an **emitting area** (point,
disc, rectangle, sphere) and a **spectrum**. The sphere emitter picks a point on
its surface and emits about the outward normal there, so etendue comes out right
-- which is what stops a concentrator fed by a point source from reporting a gain
no real optic could reach (`source.extended_emitters_cost_a_concentrator_its_gain`).

### Analysis

- `analysis::computeSpotMetrics` -- peak, mean, both uniformity ratios, centroid,
  RMS radius, D50/D86, FWHM. All **densities**, so they do not move when the
  receiver is re-binned; asserted against a synthetic uniform disc whose answers
  are closed-form.
- `analysis::crossSection` / `encircledEnergy` -- the profile through a
  click-placed cut, and the enclosed-flux curve.
- **Far-field intensity** -- theta/phi bins divided by each cell's solid angle.
  Without that division an isotropic source reads as dark at the poles simply
  because those cells are small. `intensity.totalFlux == escaped + detector`
  exactly, which is the invariant that keeps the binning honest.
- **Monte Carlo error bars** -- the tracer accumulates per-ray detector energy and
  its square, so every run reports one standard error on its efficiency.
- **Through focus as a post-process.** Every receiver arrival is recorded with the
  direction it came in on, so the arrivals propagate analytically to any nearby
  plane. One trace answers every plane instead of one trace per plane; the
  minimum is refined by a parabolic fit rather than quantised to the sweep step.

### Visualization and workflow

Ray legs now carry energy, depth, wavelength and a "reached the receiver" flag.
The flag is filled by walking *back up* the path from an arrival through a parent
index recorded per segment -- a path is a tree, so it cannot be known on the way
down. That drives both the colour modes and the receiver-paths-only filter, and
the filter is applied **before** thinning, or a 10 %-efficient scene would spend
its whole draw budget on rays the filter is about to discard.

The viewport gained a `Graphic3d_ClipPlane` section, and surface picking through
`AIS_InteractiveContext::MoveTo` -- distinguished from an orbit by whether the
mouse moved more than a few pixels between press and release, which costs no
modifier key. `RayCloud::ComputeSelection` stays empty on purpose: tens of
thousands of selectable segments would both cost a large sensitive-entity tree
and put a wall of pickable lines in front of the optics.

Workflow: JSON configuration save/load (scenes identified **by name**, since the
registry is an ordered list new scenes get inserted into), CSV export of the
grid / intensity / metrics, PNG export of any view (the 3D one through
`V3d_View::Dump`, since OCCT owns that framebuffer), and the two parameter
studies.

### Tessellation is an optical parameter

The mesh was built at OCCT's usual 0.5 rad angular deflection. That caps how
sharply any scene can focus -- a facet normal is off by up to half of it, and a
mirror doubles that into the reflected ray. Tightening it to 0.06 rad **halved**
the elliptical reflector's RMS spot radius (22.1 mm to 10.8 mm) for 20 % more
triangles, and moved its measured best focus from z = 91 mm to z = 138 mm against
a true focus at 160 mm. The linear deflection barely matters on a curved optic by
comparison, and pushing it from 0.30 to 0.05 mm bought nothing for 4x the
triangles.

`OpticalSurface` therefore carries `meshDeflection` / `meshAngle` hints. Only the
microlens array overrides them: 25 bodies at the default angle is half a million
triangles and a three-second build, and an array is a homogeniser rather than an
imaging optic.

---

## Bugs found and fixed in the previous pass

1. **RNG stream correlation (introduced then caught here).** The first version of
   the per-ray seeding strided the seed by `0x9E3779B97F4A7C15` — the same
   constant `splitmix64` adds internally. That made ray *i*'s second draw
   bit-identical to ray *i+1*'s first draw, so every ray's azimuth was a function
   of the next ray's polar angle. Mean direction and hemisphere split still
   looked perfect; only pairing those two draws exposes it. Now the index is
   hashed into the stream state. Regression test:
   `sampling.azimuth_of_one_ray_is_independent_of_the_next_rays_polar_angle`
   (verified to fail against the old seeding, correlation -0.13 vs tolerance 0.03).
2. **Segment cap applied per chunk**, so the 30 000-segment budget overshot to
   ~120 000. Now divided across the recording chunks up front.
3. **Detector centre was hardcoded to `(0,0,z)`** rather than the real centre of
   the receiver rectangle, so an off-axis detector would have binned into the
   wrong cells. Now the true centre; covered by
   `detector.offset_receiver_bins_around_its_own_centre`.
4. **`Poly_Triangulation::Triangles()` deprecation** — the last build warning.
   Build is now warning-free.

Previously fixed (kept for the record): `RayTracer::nearestHit` computed `tmin`
but never stored it, so every ray reported hit-distance 0.

## Found in this pass

1. **The Glass Prism was a Porro prism.** At its 70 mm apex over a 50 mm half
   width the roof faces sit at 54.5 degrees, well past the 41.8-degree critical
   angle, so an axial ray was totally internally reflected instead of refracted:
   the scene duplicated the Porro Prism beside it and could not disperse at all.
   The default apex is now 30 mm (31 degrees, comfortably below critical) and the
   scene deviates and separates colours as its description claims. Its efficiency
   moved from 21 % to 51 % as a result.
2. **The mesh was too coarse to image.** See the tessellation section above --
   this was invisible until spot metrics existed to measure it.
3. **Segment recording had no parent links**, so "which rays reached the
   receiver" could not be answered at all. Adding a per-segment parent index made
   it a walk back up from the arrival.
4. **The far-field CLI print under-sampled its own profile.** A 7-degree beam fell
   between two point reads and printed as zero across the board; it now prints
   grouped maxima and the peak angle.

---

## Handoff items — all closed

This pass closed everything the previous handoff left open on the physics and
workflow side:

| Item | Status |
|---|---|
| No Fresnel coefficients | **Done** — unpolarised angle-dependent R/T, per-surface with a global gate |
| No dispersion | **Done** — Cauchy n(lambda), three RGB bands, per-band irradiance grids |
| No picking or measurement in the viewport | **Done** — surface picking, clipping plane, ray colouring, receiver-path filter |
| Sources are all on-axis points | **Done** — cone angles, collimated beams, disc/rect/sphere emitters with real etendue |
| Ball-lens throughput | **Still open** — the branching tree is inherent; see below |
| No collision in the walkthrough | **Still open** |
| Not under version control | **Still open** |

And from the pass before that:

| Item | Status |
|---|---|
| Embed the OCCT 3D viewer | **Done** -- see the 3D viewport section above |
| More scenes | **Done** -- 3 became 25, behind a registry, and every one of them parametric |
| Tests / CTest (the biggest gap) | **Done** — now 216 tests in 35 suites; see below |
| Point -> Lambertian silently downgraded | **Done** — the downgrade is gone; a Point source is now traced as an isotropic 4-pi emitter and the combo box says so. Reflector efficiency is ~42.6 % for Point vs ~83.8 % for Lambertian, which is the physically correct halving |
| `traceRay` had 15 parameters | **Done** — collapsed into `TraceContext`; the recursion is now an explicit stack |
| Simulation blocked the GUI thread | **Done** — see UI section above |
| `MeshSurface::detCenter` misleading | **Done** — made correct and documented |
| BVH / spatial index | **Done** — SAH BVH in `TraceScene` |

### Test coverage
`ctest -C Release` (35 suites, 216 tests, ~1 290 000 checks):

Original suites — **intersect** (Moller-Trumbore branches, backface asserted to
hit *on purpose* since TIR depends on it), **bvh** (12 000 random rays per scene
against brute force, plus the axis-aligned-ray NaN case), **trace** (energy
conservation for every scene x source, efficiency bands, the Point-source
halving, bit-exact thread independence, reproducibility, cancellation),
**edge**, **tir**, **sampling**, **detector**, **worker**, **patterns**.

Added this pass:
- **fresnel** — checked against Rs and Rp recomputed inside the test over the
  whole angular range at three indices, which pins the implementation to the
  equations rather than to itself; the 4 % closed form; monotonic rise to 1;
  continuity into TIR (the gap to 1 has to *shrink* as the critical angle is
  approached — a step there would show as a hard ring in every guide pattern).
- **absorption** — `exp(-alpha L)` for three coefficients x three lengths; that
  the vacuum leg before the glass is not charged for; the on/off and scale gates;
  a longer guide being dimmer than a short one.
- **scatter** / **roughness** — E[cos] = 2/3 (which distinguishes cosine-weighted
  from uniform-hemisphere), an arbitrary normal, slope-error RMS of
  `sqrt(2) sigma`, a diffusing mirror collapsing an ellipsoid's peak irradiance
  by 3x and its efficiency by 4x.
- **spectral** — Cauchy exact at the reference line, blue above and red below it,
  the three bands summing to the whole pattern bin by bin, and a prism separating
  them in the right order under a collimated beam.
- **intensity** — an isotropic source flat per steradian (the normalisation test),
  total flux equal to `escaped + detector`, a collimator narrower than a diverger.
- **metrics** — a synthetic uniform disc: RMS radius `R/sqrt(2)`, encircled-energy
  radius `R sqrt(f)`, uniformity exactly 1, and the same answers at 32 and 128
  bins.
- **focus** — a synthetic converging cone whose waist has to be found to within a
  millimetre; symmetry either side of it; a real scene checked for an interior
  minimum rather than one pinned to the end of the sweep.
- **convergence** — error bars shrinking as `1/sqrt(N)` over a 64x range, and
  every point agreeing with the finest one inside its own bar.
- **source** — collimated rays parallel and inside their radius, cone angles
  bounding *and filling* the emission, extended emitters spanning their area, a
  sphere emitter on its own surface facing outward, and etendue costing a
  concentrator its gain.
- **rays** — segment energy and depth, receiver paths marked all the way back to
  the source, arrivals landing on the receiver plane.
- **params** — every scene's parameter block well formed, clamping and NaN
  rejection, parameters actually changing geometry, the source following the
  optic, cache hits, and geometry staying alive while a reference is held (the
  variant cache is small enough to evict during a slider sweep).
- **config** — a full round trip, scenes matched by name not index, unknown names
  and truncated files falling back to defaults, hostile numbers clamped into a
  still-runnable configuration.

---

## The engine review pass

An outside engineering review of the tracer listed eighteen features and ranked
what each needed to stand next to LightTools, TracePro and OpticStudio. It was
worked through in the five phases it proposed. What each bought:

**Phase 1 — the accuracy ceiling.** Surface normals read off the exact B-Rep and
interpolated across the facet; consistent face winding; a medium stack with
priorities; Russian roulette and branch collapsing in place of the hard energy
cutoff; reservoir-sampled arrivals. The elliptical reflector's spot went from
10.8 mm RMS to 0.99 mm and its best focus from 22 mm off the analytic answer to
1.5 mm. Truncation, which used to be a silent loss channel, now reads zero.

**Phase 2 — becoming measurable.** A glass catalogue with Sellmeier dispersion
and complex-index metals; absolute flux in watts or lumens; a real spectrum
sampled one wavelength per ray; detector frames, resolution and acceptance cones;
IES LM-63 and EULUMDAT export; and `--validate`, which checks the tracer against
seven closed forms derived outside it. Residuals land between 0.00 % and 0.35 %.

**Phase 3 — becoming fast enough to iterate.** Owen-scrambled Sobol emission with
replicated scrambling, emission aiming, next-event estimation, progressive
results, and instanced geometry. Error bars 1.5x to 28x tighter at the same ray
count. A four-wide BVH and float bounding boxes were built, benchmarked, found
not to pay on this workload, and removed.

**Phase 4 — becoming a design tool.** Parameter sweeps in one and two dimensions,
Nelder-Mead and CMA-ES optimisation, derived quantities beside the parameters,
run comparison, editable per-surface optics, a one-click HTML report, and
STEP/IGES import.

**Phase 5 — depth.** GGX / ABg / measured BSDFs and Henyey-Greenstein volume
scattering; coatings from an ideal residual to a characteristic-matrix solver;
polarisation via Stokes vectors and Mueller matrices; Monte Carlo tolerancing
with a yield and a sensitivity ranking; MTF, OPD and Strehl.

## What is still open

1. **Ray-surface intersection is still against the tessellation**, not the exact
   B-Rep. It matters far less than it did — the *normals* are exact now, so what
   a coarse mesh costs is hit position rather than surface direction — but an
   adaptive mesh driven by curvature and by whether a face is optically active
   (review item G-4) is still unbuilt, and would let the triangle count fall
   further.
2. **`SceneParams` is still capped at four doubles** (review item G-5). A doublet
   with two radii, two thicknesses, a spacing and a receiver is already six.
3. **The far field bins uniformly in theta** (review item D-2), so a 1.3-degree
   beam is resolved by less than one 2-degree bin and the polar cells are starved
   of samples. Equal-solid-angle binning is the fix.
4. **Imported CAD is not in the scene list and does not persist.** Import now
   assembles a full traceable scene -- the parts, a receiver beyond them and the
   source placement that aims at them, along whichever of the six axes was
   chosen -- and a run traces that rather than the selected scene
   (`SimConfig::imported`). What is still missing: the import does not appear in
   the scene combo, so touching a geometry parameter replaces it and the file has
   to be imported again; a saved config records the selected scene rather than
   the file; and studies that vary a dimension decline on it, since an imported
   solid declares none. The source origin and axis are fixed at import rather
   than being editable afterwards.
5. **The through-focus sweep assumes clear space** between the receiver and the
   swept planes, since it propagates recorded arrivals in a straight line. True
   near the receiver, which is the region of interest, but it would quietly
   mislead across an intervening optic; review item A-5 is to refuse the range
   rather than answer it.
6. **`raysHitDetector` counts path branches and next-event connections**, not
   rays. Heavy-tailed, so it swings run to run while the flux stays stable.
   Documented in `SimulationResult.h`; judge runs by flux. Labelled "Detector
   arrivals" in the UI.
7. **Per-thread detector grids will not scale past a fine receiver** (review item
   P-4). At 64 x 64 they cost 32 KB per thread; at 1024 x 1024 they are 8 MB per
   thread, and a progressive snapshot copies them.
8. **No collision in the walkthrough** — the camera passes straight through
   geometry.
9. **No logging or diagnostics capture** (review item R-5), and the config format
   string has no versioned migration path.

## Build / run

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/msvc2022_64
cmake --build build --config Release
cd build && ctest -C Release
```

### CI
`.github/workflows/ci.yml` runs exactly that on every push to `main` and every
pull request, on `windows-latest` with the same kit: MSVC 2022 x64 (pinned by
the generator, not by the image), Qt 6.8.2 and
OCCT 7.8.1 from a vcpkg pinned to commit `0b63bdd3` -- the commit this box is
on, so a green run is evidence about the build that ships rather than about
some other OCCT.

The three checks are separate steps, because they fail for three different
reasons: `ctest` (a unit regression), `--validate` (a physics regression; the
run uploads its HTML table as an artefact) and `--bench` (a determinism
regression). Qt and the vcpkg binary packages are cached: a cold cache builds
OCCT from source and takes the better part of an hour, a warm one is minutes.
`--bench` runs at 50 000 rays rather than the documented 100 000 -- determinism
does not depend on the ray count, and timings from a shared runner are not
worth waiting for.

Diagnostics (all headless, `QT_QPA_PLATFORM=offscreen`):
- `LuxTrace.exe --smoke 50000 [threads]` — every scene with efficiency and
  its error bar, spot radius, uniformity, beam FWHM, bulk-absorption share,
  timings and an energy-conservation figure.
- `LuxTrace.exe --study <scene> <rays>` — the full analysis for one scene:
  parameters, energy budget, spot metrics, far-field profile, through-focus
  result and a convergence sweep. This is the headless equivalent of the UI's
  metrics panel and Studies tab.
- `LuxTrace.exe --bench 100000` — every scene x source combination,
  1-thread vs all-threads, with the determinism check.
- `LuxTrace.exe --meshcheck` — mesh/BVH stats and a BVH-vs-brute-force
  spot check.

The test binary is emitted into `build/Release/` alongside the deployed OCCT and
Qt DLLs, so `ctest` needs no hand-built `PATH`.

## Environment notes
- OCCT via vcpkg at `C:/src/vcpkg/installed/x64-windows` (OpenCASCADE 7.8.1).
- Qt 6.8.2 MSVC kit at `C:/Qt/6.8.2/msvc2022_64`.
- After a clean rebuild, re-copy OCCT DLLs + run `windeployqt` into `build/Release/`.
