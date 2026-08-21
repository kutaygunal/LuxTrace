<img src="resources/luxtrace_128.png" align="right" width="104" alt="LuxTrace">

# LuxTrace

### Optical Design Studio

A desktop application for **optical and illumination design**, using
**OpenCASCADE Technology (OCCT)** as the geometric kernel and **Qt 6** for the UI.
It builds optical geometry as B-Rep solids, runs a Monte Carlo ray trace, and
reports what a real design review asks for: an irradiance map, a far-field
intensity distribution, spot metrics, a full energy budget, and an error bar on
the answer.

Language/stack: **C++20**, **Qt 6.8.2 (MSVC 2022)**, **OCCT 7.8.1 (vcpkg)**.

*The name is `lux`, the SI unit of illuminance the app computes, and `trace`,
for the ray tracing that gets there.*

---

## Capabilities

Benchmarked against what a commercial illumination-design package (LightTools,
TracePro, OpticStudio) is expected to do:

| Capability | LuxTrace |
|---|---|
| 3D solid modeling (B-Rep) | 25 scenes from OCCT primitives, revolved/extruded profiles and booleans, each with 2-4 editable dimensions |
| Light sources | Point / Lambertian / collimated, with a cone half-angle and a real emitting area (disc, rectangle, sphere) |
| Surface properties | Fresnel reflectance, fixed R/T split, diffuse scatter fraction, RMS roughness, bulk absorption, Cauchy dispersion |
| Ray tracing | Monte Carlo; Moller-Trumbore against a SAH BVH over the OCCT tessellation, multithreaded and deterministic |
| Reflection / refraction / TIR | Angle-dependent Fresnel split, Snell refraction, total internal reflection past the critical angle |
| Scattering | Cosine-weighted re-emission: diffusers, white reflectors, integrating spheres |
| Spectral tracing | Three RGB bands through n(lambda), rendered as real colour |
| Detector / irradiance | 2D flux grid, linear/log/sqrt scaling, five colour maps, movable cross-section cut |
| Far-field intensity | theta/phi binning into candela-per-steradian, drawn as a polar diagram or an angular map |
| Spot metrics | Peak, mean, uniformity, centroid, RMS radius, D50/D86 encircled energy, FWHM |
| Parameter studies | Convergence sweep with Monte Carlo error bars; through-focus sweep |
| 3D visualization | Embedded OCCT viewport with orbit/pan/zoom, WASD walkthrough, ray colouring, clipping plane and surface picking |
| Import/export | JSON configuration save/load; CSV irradiance, intensity and metrics; PNG of any view |

---

## The physics

Every optical property lives in one struct (`SurfaceOptics`) shared by the B-Rep,
mesh and BVH representations of a surface, so adding one reaches the tracer
without touching either conversion loop.

**Fresnel reflectance.** Unpolarised `R = (Rs + Rp) / 2` from the angle of
incidence: about 4 % for air against n = 1.5 at normal incidence, rising
continuously to 1 at grazing, and exactly 1 past the critical angle. This is what
makes a lens efficiency mean something — the old fixed 4 %/96 % split charged the
same loss to a ray arriving at 80 degrees as to one arriving straight on.

**Beer-Lambert bulk absorption.** `exp(-alpha * t)` over the distance actually
travelled inside a medium, with the medium's identity tracked along the path so
the right glass is charged. This is the loss that scales with path length rather
than with hit count, and it is the reason a 200 mm light guide is measurably
dimmer than a 100 mm one. Reported separately from surface absorption.

**Diffuse scattering.** A `scatter` fraction of an outgoing branch leaves
cosine-weighted about the surface normal instead of specularly, decided by a coin
flip rather than by splitting the branch — splitting would double the path tree
at every diffuse bounce, which an integrating sphere cannot afford, and the
roulette is unbiased. Unlocks diffusers, matte white reflectors and cavities.

**Surface roughness.** A Gaussian slope error jitters the interaction normal
before the laws are applied, which gives a mirror its factor of two in the
reflected ray for free and keeps refraction consistent with it.

**Dispersion.** Cauchy `n(lambda) = A + B / lambda^2`, with A fixed so the stored
index is exact at the d line. An RGB run splits the rays into 620 / 546 / 460 nm
bands, keeps a separate irradiance grid per band, and paints the heatmap in real
colour.

Roughness, scatter and absorption also have **scene-wide overrides**, so a
polished mirror can be turned matte and re-run without rebuilding any geometry —
they are ray-time properties, and re-tessellating an optic to change its polish
would be absurd.

## The scene library

25 scenes, all built from OCCT B-Rep and enumerated from one registry
(`GeometryProvider::Scene`), so the UI, the diagnostics and the tests pick them
up automatically. Each declares 2-4 editable dimensions (`paramInfo`), the last
of which is always the receiver position; the emitter follows the geometry, so
lengthening a paraboloid moves the source to its new focus.

Efficiencies are for a Lambertian source at 50 000 rays, with the full physics on.

**Reflective**

| Scene | Eff. | What it shows |
|---|---|---|
| Parabolic Reflector | 83.8 % | Source at the focus is collimated: 1.3 degree beam FWHM |
| Elliptical Reflector | 59.9 % | One focus imaged onto the other |
| Spherical Reflector | 91.7 % | Spherical aberration, against the parabola's clean spot |
| Off-Axis Parabola | 20.6 % | Off-axis segment with the receiver offset to match |
| Parabolic Trough | 49.2 % | Collimates in x only: a line, not a spot |
| Compound Parabolic Concentrator | 22.7 % | Nonimaging: inside the acceptance angle in, the rest rejected |
| Conical Concentrator | 14.4 % | The naive funnel, measurably worse than the CPC |
| Cassegrain (two-mirror) | 9.9 % | Folded path; the secondary shadows the axis |
| Corner-Cube Retroreflector | 22.3 % | Three orthogonal mirrors send light back the way it came |

**Refractive**

| Scene | Eff. | What it shows |
|---|---|---|
| Ball Lens | 12.9 % | Strong focusing, heavy aberration |
| Plano-Convex Lens | 12.3 % | Source at the front focus, so the lens collimates |
| Biconvex Lens | 12.8 % | Roughly twice the power at the same radius |
| Plano-Concave Lens | 15.7 % | Negative power: the beam spreads |
| Half-Ball Lens (LED dome) | 68.3 % | Near-normal incidence lets light escape instead of being trapped |
| Cylindrical Rod Lens | 11.7 % | Power in x only: a point images to a line |
| Fresnel Lens (stepped) | 24.5 % | Annular facets, most of the deflection without the glass |
| Axicon (ring former) | 78.7 % | Collimator + cone: a ring with a dark centre |
| Microlens Array (5 x 5) | 11.6 % | Grid of lenslets; light between them passes straight through |
| Glass Prism | 50.9 % | Deviates in one plane; in RGB it separates the bands |

**Total internal reflection**

| Scene | Eff. | What it shows |
|---|---|---|
| Light Guide (round) | 74.3 % | Rays past the critical angle bounce to the far end |
| Square Light Guide | 77.0 % | Flat walls mix the beam differently |
| Tapered Light Guide | 59.9 % | A taper steepens rays until some break TIR and leak |
| Porro Prism (TIR retro) | 15.6 % | Two 45-degree faces fold the beam straight back |

**Scattering**

| Scene | Eff. | What it shows |
|---|---|---|
| Integrating Sphere | 26.7 % | Matte white cavity: every bounce is Lambertian, the port sees a uniform field |
| Diffuser Plate | 32.4 % | Transmissive diffuser washing a bright spot into an even glow |

The round light guide used to read 89 %. It now reads 74 % because its entrance
and exit faces pay a Fresnel reflection and its glass absorbs 4.4 % of the light
over the zig-zag path — both of which a real rod does and the old model did not.

With an isotropic **Point** source most scenes drop to roughly half: the backward
hemisphere is simply lost. That is the physically correct answer, and the app
does not quietly substitute a Lambertian emitter to hide it. A few scenes behave
differently for good reasons — the elliptical reflector does *better* with a
point source, because its Lambertian hemisphere aims straight at the hole in the
bottom of the cup.

The whole 25-scene library at 50 000 rays each traces in **0.51 s**. Energy is
accounted for exactly in every scene: `detected + absorbed + escaped + truncated
== emitted` to 1e-9.

### Determinism
A run is bit-identical no matter how many threads execute it. Every ray seeds
its own RNG from its index — for its emission direction, its emitting-area
sample and its scatter/roughness draws alike — and the scalar totals reduce in
chunk order rather than thread order. `--bench` checks this on every
scene/source combination.

## Measurements, not impressions

- **Spot metrics** are computed as densities (flux per mm^2), so they do not move
  when the receiver is re-binned: peak, mean over the lit bins, min/peak and
  mean/peak uniformity, flux centroid, RMS radius, D50/D86 encircled-energy radii
  and the FWHM of the profile through the centroid.
- **Far-field intensity** bins the direction every ray leaves the system on into
  theta/phi cells and divides by each cell's solid angle. Without that division an
  isotropic source reads as dark at the poles simply because those cells are
  small. The total over the sphere equals `escaped + detector` exactly.
- **Monte Carlo error bars.** Each run reports one standard error on its
  efficiency from the ray-to-ray spread. The convergence sweep traces the same
  scene at geometrically spaced ray counts, each with its own seed, and says
  whether the last two points agree within 2 sigma — which is the practical form
  of "have I traced enough rays?".
- **Through focus** is a post-process, not a re-trace. Every receiver arrival is
  recorded with the direction it came in on, so the arrivals can be propagated
  analytically to any nearby plane and the spot size read off. One trace answers
  every plane; the minimum is refined by a parabolic fit rather than quantised to
  the sweep step.

## Performance

The tracer was originally a single-threaded brute-force scan of every triangle
for every ray segment, running on the GUI thread. Same workload:

| Workload | Before | After |
|---|---|---|
| 3 scenes x 30 000 rays | 48.1 s | **0.17 s** |
| 3 scenes x 1 000 000 rays | ~27 min | **1.35 s** |

What changed: a SAH **BVH** over the flattened triangles, **multithreading** over
rays with dynamic 512-ray chunks, a **cached** geometry/mesh/BVH per (scene,
parameters) pair, and **bounded** ray-path recording for the diagram.

Tessellation is a separate lever on accuracy. The mesh used to be built at OCCT's
usual 0.5 rad angular deflection, which caps how sharply any scene can focus: a
facet normal is off by up to half that, and a mirror doubles it into the
reflected ray. Tightening it to 0.06 rad halved the elliptical reflector's RMS
spot radius (22.1 mm to 10.8 mm) for 20 % more triangles. Optics built from many
small repeated bodies carry their own coarser hint — the microlens array would
otherwise be half a million triangles and a three-second build.

## Build

Prerequisites:
- CMake >= 3.21
- Qt 6 (set `CMAKE_PREFIX_PATH` to your Qt MSVC kit)
- OpenCASCADE via vcpkg: `vcpkg install opencascade --triplet x64-windows`

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/msvc2022_64
```

```bash
cmake --build build --config Release
```

Runtime DLLs: copy the OCCT `bin/*.dll` (from the vcpkg triplet) next to the
exe, and run `windeployqt` for the Qt DLLs/plugins.

## Test

```bash
cd build && ctest -C Release
```

108 tests in 22 suites, no external framework. Beyond the original coverage
(Moller-Trumbore branches, BVH vs brute force, energy conservation, TIR critical
angle, edge cases, sampling statistics, detector binning, the async worker, and
the irradiance patterns each scene claims to produce), the physics and analysis
are pinned against arithmetic rather than against another simulation:

- **fresnel** — checked against Rs and Rp recomputed in the test over the whole
  angular range and three indices; the 4 % closed form at normal incidence;
  monotonic rise to 1 at grazing; continuity into total internal reflection.
- **absorption** — `exp(-alpha L)` for three coefficients and three lengths; that
  the vacuum leg before the glass is not charged for; the on/off and scale gates.
- **scatter / roughness** — cosine-weighted re-emission (E[cos] = 2/3, which
  distinguishes it from a uniform hemisphere), slope error RMS, and that a
  diffusing mirror collapses an ellipsoid's focus.
- **spectral** — Cauchy exact at the reference line, blue bending more than red,
  the three bands summing to the whole pattern, and a prism separating them.
- **intensity** — an isotropic source reading flat per steradian, and the far
  field accounting for exactly `escaped + detector`.
- **metrics / focus** — a synthetic uniform disc whose RMS radius is `R/sqrt(2)`
  and whose encircled-energy radius is `R sqrt(f)`; a synthetic converging cone
  whose waist the sweep has to find to within a millimetre.
- **convergence** — error bars shrinking as `1/sqrt(N)` across a 64x range.
- **params / config** — every scene's parameter block, clamping and NaN
  rejection, geometry staying alive while a run holds it, and a JSON round trip
  that identifies scenes by name rather than by registry index.

## Run

Run `LuxTrace.exe`, pick a scene, adjust its dimensions, choose a source
and press **Run**. The trace runs on a worker thread: the inputs freeze for the
duration, a progress bar tracks it, and **Cancel** stops it within milliseconds
while keeping the rays that already finished.

Five tabs over a metrics panel:

| Tab | Shows |
|---|---|
| **3D View** | The OCCT viewport: B-Rep geometry and traced ray paths, coloured by energy, bounce count or wavelength; a receiver-paths-only filter; a clipping plane to slice the optic open |
| **Ray Diagram (X-Z)** | The flat projection, where a whole ray fan reads at once |
| **Irradiance** | The heatmap with five colour maps and linear/log/sqrt scaling, a click-to-move cross-section cut, and the encircled-energy curve |
| **Intensity** | The far-field polar diagram (with C0 and C90 meridians) or the angular map |
| **Studies** | Convergence with error bars, and the through-focus sweep |

Viewport navigation:

| Input | Action |
|---|---|
| Left drag | orbit around the scene |
| Left click | pick a surface and show its optical properties |
| Middle drag | pan |
| Right drag | free look (turn in place) |
| Wheel | zoom toward the cursor |
| `W` / `S` | walk forward / back |
| `A` / `D` | strafe left / right |
| `E` / `Q` | rise / drop |
| `Shift` / `Ctrl` | x4 / x0.25 walk speed |
| `F` / `R` | fit scene / reset view |

Walking needs the **Perspective** checkbox on (it is by default): under an
orthographic projection, moving along the view axis changes nothing on screen.

Headless diagnostics (all with `QT_QPA_PLATFORM=offscreen`):

```bash
LuxTrace.exe --smoke 50000
```

```bash
LuxTrace.exe --study 1 200000
```

```bash
LuxTrace.exe --bench 100000
```

```bash
LuxTrace.exe --meshcheck
```

`--smoke` traces every scene with efficiency, error bar, spot radius, uniformity,
beam FWHM and bulk-absorption share. `--study <scene> <rays>` prints the full
analysis for one scene: its parameters, the energy budget, the spot metrics, the
far-field profile, the through-focus result and a convergence sweep. `--bench`
compares 1-thread against all-threads on every scene/source combination and
asserts determinism. `--meshcheck` dumps mesh/BVH stats with a BVH-vs-brute-force
spot check.

## Project layout

```
src/
  main.cpp              entry point + --smoke / --study / --bench / --meshcheck
  core/
    SurfaceOptics       the optical properties, shared by all three surface forms
    Optics              Fresnel, Cauchy, cosine hemisphere, slope error, RNG
    GeometryProvider    scene registry: 25 parametric OCCT scenes + their sources
    MeshBuilder         BRepMesh tessellation -> triangle meshes
    TraceScene          flattened triangles + SAH BVH + intersection
    RayTracer           Monte Carlo tracing, threaded and deterministic
    SimulationResult    irradiance grid, far field, arrivals, energy accounting
    Simulation          facade: geometry -> mesh -> BVH (cached) -> trace
    Analysis            spot metrics, profiles, encircled energy, CSV export
    Studies             convergence and through-focus sweeps
    ConfigIO            JSON save/load of a whole setup
    SimulationWorker    QThread wrapper: async run, progress, cancellation
    StudyWorker         the same, for a convergence sweep
  ui/
    MainWindow          layout, tabs, menus, exports
    ControlsPanel       scene, geometry parameters, source, physics, run
    OcctViewWidget      OCCT viewport: navigation, ray colouring, clipping, picking
    HeatmapWidget       irradiance map with colour maps, scaling and a cut line
    PolarPlotWidget     far-field polar diagram and angular map
    PlotWidget          line plots with error bars, markers and a hover readout
    RayDiagramWidget    2D (X-Z) ray-path diagram
    Palette             colour maps and display scaling
test/
  TestMain.cpp          harness + the original suites
  NewTests.inc          physics, analysis and workflow suites
resources/
  make_icon.py          the icon generator -- edit this, not the images
  luxtrace.svg          vector master, emitted by the generator
  luxtrace_*.png        raster sizes for the Qt resource
  luxtrace.ico          multi-size Windows icon
  luxtrace.qrc          Qt resource: the icon compiled into the binary
  LuxTrace.rc           Windows resource: executable icon + version block
```

### Branding

The icon is a converging ray bundle through a biconvex lens -- five amber rays
entering from the left, refracting at both glass surfaces and meeting at a bright
focus. It is what the app does, and the silhouette still reads at 16 pixels,
where the generator drops to a simplified three-ray version rather than
downsampling the detailed one into a smear.

`resources/make_icon.py` is the source of truth: it emits the SVG, the PNGs and
the ICO from one set of geometry constants, so the three cannot drift apart.
Re-cut them with:

```bash
python resources/make_icon.py
```

## Notes / limits
- Ray-surface intersection uses tessellated meshes (Moller-Trumbore against a
  BVH), not exact B-Rep surface intersection. The angular deflection of that
  tessellation, not the ray tracer, is what limits how sharply a scene can focus.
- Scattering uses a Russian-roulette choice between specular and diffuse rather
  than splitting the branch. It is unbiased, but a single ray's path is one
  outcome, not an average over both.
- Fresnel is unpolarised: rays carry no polarisation state, so Brewster's angle
  shows in the reflectance but not as a polarising effect.
- Dispersion is three bands, not a continuous spectrum. A prism separates them
  into three beams rather than a smooth rainbow.
- The through-focus sweep propagates recorded arrivals in a straight line, so it
  is only valid where nothing stands between the planes — near the receiver,
  which is the interesting region.
- "Detector arrivals" counts path branches, not rays — one ray can arrive more
  than once after splitting. It is a heavy-tailed statistic, so judge a run by
  flux, not by that count.
- The 3D viewport shows at most ~6 000 ray legs; a denser bundle draws as a solid
  tube that hides the optic. The physics always uses every ray.
