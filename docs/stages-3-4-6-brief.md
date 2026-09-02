# LuxTrace — brief for stages 3 (remainder), 4 and 6

A handoff document. It assumes you have not seen this repository before and
gives you what you need to start: how to build it, how the codebase expects to
be worked in, exactly where the previous stages stopped, and three pieces of
work with acceptance criteria.

Stage 5 (CAD breadth — NX/Creo/Rhino import) is deliberately **not** in here.
It is mostly a licensing question rather than an engineering one, and the owner
excluded it.

---

## 1. Orientation

LuxTrace is a desktop optical and illumination design application: C++20, Qt
6.8.2 (MSVC 2022), OpenCASCADE 7.8.1 via vcpkg. It builds optical geometry as
B-Rep solids, tessellates them, runs a Monte Carlo forward ray trace over a SAH
BVH, and reports an irradiance map, a far-field intensity distribution, spot
metrics, an energy budget and an error bar.

There are now **two tracers**:

- **The reference** — `src/core/RayTracer.cpp`. CPU, double precision, full
  physics, variance reduction. This is the answer.
- **The preview** — `src/gpu/`. CUDA, single precision, a physics subset. Fast,
  and never trusted without being checked against the reference.

### Build

The `build/` directory in the repo is already configured. If you need to
reconfigure from scratch, the toolchain paths are not in the presets and you
must pass them:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/msvc2022_64
```

```bash
cmake --build build --config Release
```

CUDA is optional and detected at configure time (`check_language(CUDA)`). With
it, you get `optics_gpu` plus `GpuTraceHost.cpp`; without it, `GpuTraceStub.cpp`
is compiled instead and every caller links unchanged. There is no `#ifdef`
outside CMake — the question "is there a preview backend" is answered at run
time by `gputrace::available()`.

Two CUDA build traps, both already handled, both worth knowing if you touch
CMake: MSVC flags must be scoped `$<$<COMPILE_LANGUAGE:CXX>:...>` (nvcc reports
a stray `/MP` as *"a single input file is required"*, which says nothing about
the cause), and the `.cu` lives in its own target so Qt and OCCT include paths
never reach nvcc.

### Verify

```bash
build/Release/optics_tests.exe        # 449 tests, ~1.95 M checks, must be 0 failures
build/Release/LuxTrace.exe --validate # 13 closed-form optics checks
build/Release/LuxTrace.exe --bench 4000  # determinism across thread counts, all 28 scenes
build/Release/LuxTrace.exe --refcheck all 1000000   # every scene: preview vs reference
```

Headless JSON API (`--serve` for a stdio stream, `--job file.json` for a batch)
is the fastest way to run experiments. One operation per line in, one JSON
envelope per line out. `src/core/JobRunner.cpp` is the dispatcher.

---

## 2. How this codebase expects to be worked in

Read a few files before writing any. The conventions are strong and consistent,
and matching them matters more here than in most repositories.

- **Comments explain why, and name the failure mode avoided.** They are prose,
  they are specific, and they frequently describe the bug that motivated the
  code. Do not write comments that restate the code.
- **Every behavioural claim has a test.** Tests are named as sentences
  (`TEST(gpu, what_it_cannot_model_is_refused_by_name)`) and their bodies open
  with a comment saying what would break if the test did not exist.
- **Determinism is a contract, not an aspiration.** Scalars reduce in chunk
  order and are bit-identical across thread counts; grids are accumulated per
  thread and reduce in thread order, so a bin can move in its last place between
  two *different* thread counts but is exactly reproducible at a fixed one.
  `--bench` fails if a grid does not repeat. See `README.md` § Determinism —
  and do not weaken these without saying so out loud.
- **Energy accounts exactly.** `detected + absorbed + escaped + truncated +
  rejected + roulette == emitted` to 1e-9 in every scene. Closure is checked in
  tests. Note that closure proves bookkeeping, never physics — an earlier bug
  produced a completely wrong answer whose energy still closed to
  1.000000000.
- **Instrument rather than assume.** The tracer counts its own anomalies
  (`SimulationResult::anomalies` — unmatched exits, medium stack overflow,
  guessed indices) and reports them through the JSON API. When you add a
  recovery path, count it.
- **Refuse rather than approximate silently.** This is the rule the preview
  backend is built on and it applies generally. A wrong number that looks right
  is the only kind that costs anyone money.

---

## 3. State of play

Stages 1, 2 and 3 were completed in a previous session and are **uncommitted on
`main`** — roughly 1,640 changed lines across 19 files plus 7 new ones. Get them
onto a branch as separate commits before you start, or you will not be able to
tell your work from theirs.

| Stage | Status |
|---|---|
| 1 — Immersed sources | Done. Medium stack seeded at emission by a point-in-solid parity probe. Validated against a closed form (30.70 % analytic vs 30.54 % traced). |
| 2 — Gegenbauer phase function, stray-light routes with surface sets, determinism | Done. |
| 3 — GPU preview with the CPU as reference | Built and shipping; one known systematic remains (§4). |
| 4 — Backward tracing and a luminance camera | **Not started** (§5). |
| 5 — CAD breadth | Excluded. |
| 6 — Meta-optics | **Not started** (§6). |

### Key files added in stage 3

| File | What it is |
|---|---|
| `src/core/BackendCheck.{h,cpp}` | The comparison. Takes two `SimulationResult`s and reports how far apart they are in units of their own error bars. Backend-agnostic on purpose, so it is testable with no GPU and is equally useful for "did this refactor move the answer". |
| `src/gpu/GpuKernel.h` | The device boundary. Plain structs, raw pointers, no Qt, no OCCT. |
| `src/gpu/GpuTrace.cu` | The kernel. Device code only. |
| `src/gpu/GpuTraceHost.cpp` | The gate (`supports()`), the flattening, the result. Everything Qt and OCCT stops here. |
| `src/gpu/GpuTraceStub.cpp` | Compiled when CUDA is absent. |
| `test/BackendCheckTests.inc`, `test/GpuBackendTests.inc` | Calibration of the instrument, and the backend. |

### How the comparison works — read this before touching anything

Two correct Monte Carlo runs of the same system **disagree**. The question is
never "are the numbers equal", it is "do they differ by more than sampling
explains". So:

- **Scalars** are compared in combined standard errors. The threshold is 4σ, not
  the conventional 2–3σ: the error bars were measured against the spread of 24
  independent seeds and come out at 0.99–1.16 of the true standard deviation, so
  3σ would reject two correct runs about once in three hundred. A backend that
  has actually dropped a term shows up at tens of σ.
- **Distributions** are compared by total variation distance — the fraction of
  light that would have to move to turn one map into the other — against the
  noise floor for two draws of that size.
- **The noise floor is measured, not modelled.** A count-based model was wrong by
  5.5× on the integrating sphere, because next-event estimation deposits a
  highly variable weight at nearly every diffuse bounce, so a bin's noise has
  little to do with how many rays reached it. Both backends now accumulate a
  per-bin sum of squared deposits (`irradianceVar`, `intensityMasterVar`, behind
  `TraceOptions::noiseMap`) and the floor comes from that. On the sphere the
  measured floor predicts the observed distance to 1.00.
- **The far field is compared on the uniform master grid**, not on the reported
  bins. The reported theta partition is beam-adaptive and derived from each
  run's own histogram, so two correct runs land on slightly different rings.
  Both backends build the reported far field through the same
  `finishIntensityGrid()` in `SimulationResult.cpp`.

If you add a backend, an approximation or an optimisation anywhere in this
project, this is how you demonstrate it did not change the answer.

---

## 4. Stage 3 — what is left

### 4a. The refractive bias (the one that matters)

**Symptom.** The preview delivers about **1 % less flux than the reference
through refracting surfaces**, and the deficit scales with how obliquely rays
cross the interface. It is invisible on the LED dome (rays cross at normal
incidence) and largest on the plano-convex lens and the axicon's cone — the two
scenes `--refcheck all` currently reports as DIFFERS. The missing energy appears
as `escaped`.

**It is a bias, not noise.** σ grows with ray count:

| Scene | 500 k rays | 8 M rays |
|---|---|---|
| Plano-Convex Lens | −3.86σ | −14.71σ |
| Axicon (ring former) | −5.07σ | −24.89σ |
| Ball Lens | −0.88σ | −2.01σ |
| Glass Prism | −1.30σ | −2.67σ |
| Light Guide (TIR) | +0.53σ | +1.12σ |
| Parabolic Reflector | +0.61σ | −0.83σ |
| Cassegrain | −0.04σ | −0.23σ |

Every refractive scene is negative and growing. Every mirror and TIR scene is
symmetric about zero and flat. That is the whole diagnosis in one table, and it
is the first thing to reproduce.

**Ruled out by measurement — do not repeat this work:**

1. Fresnel (bisected: disagreement survives with `physics.fresnel` off).
2. Coatings (survives with `coatings` off).
3. Absorption (survives with `absorption` off).
4. Interpolated vs facet normals (toggled; the interpolation demonstrably works
   — on the elliptical reflector, facet-only broadens the beam from 3.57° to
   6.62° while interpolated matches the reference exactly).
5. Ray epsilon (1e-5 → 1e-6; bit-identical result).
6. Depth truncation (`truncated` is exactly 0 on the preview).
7. Degenerate triangles (scale-relative Möller-Trumbore cutoff; bit-identical).
8. Medium-tracking anomalies (zero guessed-index events on the CPU for these
   scenes).
9. The reference's own estimator (`--refcheck <scene> --plain` traces the
   reference with Sobol, aiming, next-event estimation and Russian roulette all
   off; the gap persists).
10. **Branch sampling.** The preview samples one branch per interface where the
    reference follows both. Setting reflectivity to 0 and transmissivity to 1
    with a `surfaceOverride` removes branching entirely — pure refraction, no
    choice to make — and the preview is still −5.21σ.

**What is left.** Single-precision geometry through a refracting interface. The
strongest remaining lead is that the preview's beam is measurably *softer*: on
the pure-refraction test the far-field FWHM is 2.9187° against the reference's
2.8875° and the peak intensity is 1.7 % lower, so light is being spread slightly
wider and falling off the receiver.

**Suggested next experiments**, cheapest first:

- Promote just the refraction arithmetic to double in the kernel (`fresnelR`,
  the Snell branch, and the normal interpolation) leaving traversal in float.
  If the bias vanishes, it is precision and the fix is a targeted one.
- Store triangle vertices relative to the scene centroid before converting to
  float. Scenes sit at 100–300 mm from the origin, so a float loses ~1.5e-5 mm
  of resolution there that a recentred mesh would keep.
- Check the barycentric coordinates the kernel computes against the reference's
  for the same ray — a direct A/B on `h.u`, `h.v` for a handful of seeded rays
  would settle whether the hit parameterisation or the shading maths is at
  fault.

**Acceptance.** `--refcheck all 8000000` reports 0 differing, or the refractive
path is refused by the gate with a reason naming this limitation. Do not widen
the 4σ tolerance to make it pass — the tolerance is calibrated and the bias is
real.

### 4b. Coverage the gate currently refuses

Each of these is additive and independently useful. The gate in
`gputrace::supports()` must lose the corresponding refusal only when the kernel
genuinely models the thing.

| Refusal | Notes |
|---|---|
| Scattering surfaces | GGX with Smith masking, ABg and Lambertian all exist in `src/core/Bsdf.h`. Porting the sampler is the bulk of it; the receiver already handles diffuse arrivals. Unlocks the Integrating Sphere, Diffuser Plate and Showcase Luminaire. |
| Volume scattering | Henyey-Greenstein and Gegenbauer, both with analytic inverse CDFs, in `Bsdf.cpp`. Cheap once a medium is tracked, which it already is. |
| Instanced geometry | `TraceScene` carries a two-level hierarchy (`m_instances`, `m_blas`, `m_tlasNodes`). The kernel walks only the single-level path today. Unlocks the Microlens Array and LED Array Luminaire. |
| Measured / multi-layer coatings | `Coating::Table` and `Coating::Stack`. The stack is a characteristic-matrix solve with complex arithmetic — port carefully or keep refusing. |
| Multiple sources and receivers | Mechanical: the budget split by power already exists on the CPU (`Simulation::sourcesFor`). |
| Polarisation, spectral runs | Large. Probably belongs after stage 4. |

### 4c. Variance reduction on the preview

The preview has none — no Owen-scrambled Sobol, no next-event estimation, no
aiming, no Russian roulette. It therefore needs more rays than the reference for
the same error bar, and the wall-clock advantage is smaller than the raw ray-rate
advantage. Sobol emission is the cheapest of the three to port and the most
valuable; next-event estimation matters enormously on diffuse scenes and should
follow scattering support.

---

## 5. Stage 4 — backward tracing and a luminance camera

**This is the one genuine architectural absence, and the largest single piece of
work in the project. Budget months, not weeks.**

### Why

Everything LuxTrace measures today starts at a source. A camera-side integrator
answers a different and commercially important question: *what does an observer
see*. It is what turns the Appearance tab from a labelled preview into a
measurement in cd/m², and it is the capability the commercial comparison
(Keysight LightTools 2026, which ships VisionSym) leads with after GPU speed.

### What already exists to build on

- **Next-event estimation** — `nextEventEstimate()` in `RayTracer.cpp` connects a
  diffuse bounce analytically to every receiver, with occlusion testing and a
  declined-estimate path for receivers that subtend too much solid angle. That
  is half the machinery of a backward integrator, pointed the other way.
- **BSDF models** — `bsdf::Surface` carries GGX/Smith, ABg, measured tables and
  Lambertian, shared by the B-Rep, mesh and BVH representations of a surface.
  The tracer's sampling sites are around `RayTracer.cpp:1383`.
- **Spectral machinery** — `SpectrumConfig` carries SPDs, CIE colour matching and
  V(λ), and reports luminous efficacy in lm/W. This is what makes a photometric
  (rather than RGB) camera possible: weight radiance by V(λ) and you have cd/m².
- **`TraceScene`** — the same BVH, occlusion queries and detector frames.
- **`AppearanceView`** — the existing OCCT path-traced preview. Read its header
  comment: it documents precisely why it is *not* photometric (RGB, its own
  two-layer BSDF, no `SurfaceOptics`, no coatings, no dispersion, tone-mapped
  sRGB). Do not extend it. Build beside it and let it be replaced.

### Plan

1. **Camera model.** A pinhole and a thin-lens camera with a real focal length,
   aperture and sensor size, placed like a `DetectorInfo` is. Per-pixel primary
   ray generation, stratified within the pixel.
2. **Path integrator.** Camera ray → BSDF sample → walk. At every vertex,
   connect to the emitters analytically (the mirror image of the existing NEE)
   and add the contribution. Russian roulette on throughput.
3. **Light sampling.** The scene's sources are analytic — point, Lambertian and
   collimated, with disc/rect/sphere emitting areas. Each needs a "sample a
   point and direction, return the pdf" routine. The sphere and rect cases are
   standard; the collimated case has a degenerate solid angle and must be
   handled explicitly rather than sampled.
4. **Photometric accumulation.** Accumulate spectral radiance, weight by V(λ),
   scale by 683 lm/W. The output is a luminance image in cd/m², not an RGB
   picture. Report it as such.
5. **Progressive refinement.** Accumulate into a running mean with a sample
   count per pixel so the image forms while it runs, as the forward tracer's
   partial results already do (`TraceControl::partial`).
6. **Environment sources.** An HDR environment as an emitter, once the above
   works.

### Acceptance

- **Furnace test.** A closed diffuse box with uniform emission must render a
  uniform image at exactly the analytic radiance. Any energy loss shows up
  immediately.
- **Forward/backward agreement.** A diffuse box, or a Lambertian emitter behind a
  diffuser, measured by the forward tracer's receiver and by the backward
  camera, must agree inside their error bars — use `backendcheck::compare`,
  which already does exactly this job and does not care which engine produced
  either side.
- **Closed form.** A Lambertian emitter of exitance M viewed head-on reads
  L = M/π cd/m². Add it to `--validate`, which is where the other thirteen
  closed forms live.
- The Appearance tab's caption stops saying "not a measurement", and every test
  asserting that caption (`AppearanceTests.inc`) is updated deliberately rather
  than deleted.

---

## 6. Stage 6 — meta-optics

**Weeks, not months — because the machinery it needs is largely present.** Do
this before considering AR waveguides.

### Why this and not waveguides

The commercial comparison describes meta-optic support as representing
EM-derived behaviour as wavelength- and polarisation-dependent optical surface
properties, applied through extended BSDF and surface models. LuxTrace already
has measured BSDF tables, measured `R(λ)` coating tables, a characteristic-matrix
thin-film solver and Mueller-matrix polarisation. The gap is narrower than it
looks.

The AR waveguide designer — k-space layout, input couplers / eye-pupil expanders
/ output couplers, surface-relief and volume Bragg gratings, rigorous EM coupling
— is a product in its own right. The recommendation on record is not to compete
there.

### What a metasurface actually needs

A metasurface does not obey Snell's law. It imposes a spatially varying phase
gradient dΦ/dx on the wavefront, and the outgoing direction follows the
generalised Snell law:

```
n_t sin(θ_t) − n_i sin(θ_i) = (λ / 2π) · dΦ/dx
```

So the new physics is **a surface that deflects by a designed, wavelength-
dependent angle**, with an efficiency per diffraction order. That is a new
`SurfaceOptics` behaviour, not a new tracer.

### Plan

1. **A phase-gradient surface type.** Extend `SurfaceOptics` with an optional
   metasurface descriptor: a phase gradient (constant, radial, or sampled over
   the surface for a metalens), and a per-order efficiency table indexed by
   wavelength and incidence angle. Keep it out of the hot path behind a null
   check, as `bsdf` and `coating` already are.
2. **Order sampling.** At a hit, enumerate the propagating orders, sample one in
   proportion to its efficiency, and set the outgoing direction from the
   generalised Snell law. Energy not in any propagating order is absorbed and
   booked — the energy budget must still close to 1e-9.
3. **Polarisation.** The efficiency table gains a polarisation index and the
   interaction applies a Mueller matrix, reusing `src/core/Polarisation.{h,cpp}`.
4. **Import.** Meta-optic data comes out of an EM solver as tabulated complex
   coefficients. Follow the pattern already established by `MaterialFile.cpp`
   (Zemax `.agf`, refractiveindex.info `.yml`) and `RayFile.cpp` (Zemax `.dat`,
   ASAP `.dis`, TracePro text): read the vendor's format, report what was
   loaded, and degrade loudly rather than silently on an unknown one.
5. **A scene.** The library has 28 parametric scenes in
   `GeometryProvider::Scene`, each declaring 2–4 editable dimensions. Add a
   metalens so the feature is demonstrable and regression-tested like everything
   else.

### Acceptance

- **Reduces to the base case.** At zero phase gradient the surface must trace
  bit-identically to ordinary refraction. This is the test that keeps the
  feature from perturbing the other 28 scenes.
- **A metalens focuses to its design NA**, checked against the analytic focal
  length, and added to `--validate`.
- **Energy closes** across all diffraction orders including the absorbed
  remainder.
- **Chromatic behaviour is right in sign and magnitude**: a phase-gradient lens
  has strong, *reversed* chromatic dispersion compared with a refractive lens.
  A test that traces three wavelengths and checks the focal shift ordering will
  catch a sign error that a monochromatic test cannot.

---

## 7. Suggested order

1. Commit the existing stage 1–3 work to a branch. It is uncommitted and you
   cannot otherwise separate your changes from it.
2. **Stage 3a** — the refractive bias. It is bounded, it is the only *known
   wrong* thing in the tree, and the diagnosis is most of the way done.
3. **Stage 6** — meta-optics. Best ratio of capability to effort, and it lands
   on machinery that already exists.
4. **Stage 3b** — scattering and instancing on the preview, which together
   unlock five more scenes.
5. **Stage 4** — backward tracing. The largest piece; start it when nothing
   smaller is outstanding.

A closing note on judgement, because it is the thing this project is actually
built around: speed is easy to demonstrate and easy to fake, correctness is
neither. The reason the preview backend is trustworthy is not that it is fast —
it is that it refuses what it cannot model and is checked against a reference on
every scene, and that the two scenes where it currently fails are reported rather
than hidden. Keep that arrangement whatever you add.
