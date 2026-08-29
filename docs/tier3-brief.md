# TIER 3 — The Engine's Own Ceiling (6 gaps)

High-level brief for the Planner. These are known, honest limits on what the LuxTrace
optical engine can model. Two of them quietly undercut capabilities the README advertises
as finished (T3-001, T3-002) — those move first. Work the rest to reduce structural
limits and state the model boundary once, clearly.

## T3-001 — The far field bins uniformly in theta  [STRUCTURAL, move first]
- Problem: theta bins are a flat `180 / nTheta` degrees. At the default 90 bins that is 2
  deg per bin — so the parabolic reflector's 1.3 deg beam (the sharpest result in the scene
  library) is resolved by less than a single bin, while cells near the equator are starved of
  samples relative to their solid angle. This same grid exports as IES LM-63 and EULUMDAT, so
  the headlined interoperation claim is weakest exactly where a photometric file matters most.
- Evidence: `src/.../SimulationResult.h:141` — `thetaStepDeg()` returns `180.0 / nTheta`.
- Fix: equal-solid-angle theta binning, or a beam-adaptive grid that concentrates bins where
  the flux is. Handoff item 3 identifies this; promote it out of "still open".

## T3-002 — Measured data truncated to eight points at load  [STRUCTURAL, move first]
- Problem: three fixed-size tables cap what a measurement can carry: coating reflectance at 8
  samples, material n and k at 8 samples, BSDF at 16. Every non-Sellmeier glass from an `.agf`
  or refractiveindex.info file is resampled onto those 8 points across the band. The formats
  are real and the resampling is deliberate and documented in source — but "measured R(lambda)
  tables" and "measured BSDF" are advertised capabilities; nothing in the UI says the curve was
  reduced at point of use.
- Evidence: `Coating.h:28-29` `kMaxLayers=8 kMaxSamples=8`; `Material.h:28` `kMaxSamples=8`;
  `Bsdf.h:38` `kMaxSamples=16`; `MaterialFile.cpp:211,480,505`.
- Fix: raise the caps behind a heap-allocated table off the hot path, AND/OR surface the
  resampling in the material description + coating inspector so fidelity is visible where the
  number is read.

## T3-003 — Scene parameters still capped at four  [STRUCTURAL]
- Problem: `SceneParams::kMax = 4`, and the last slot is always the receiver position — so three
  free dimensions per scene. A cemented doublet needs six. This bounds not just geometry but the
  optimiser's search space, the 2D sweep's factor pairs, and the tolerance study's sensitivity
  ranking.
- Evidence: `GeometryProvider.h:46`.
- Fix: the struct is copied per study evaluation, so use a small inline vector (not a heap one).
  Handoff item 2.

## T3-004 — Detector grids copied per thread  [KNOWN]
- Problem: every thread holds a full copy of the all-receivers bin grid, and a progressive
  snapshot copies them again. At 64x64 that is 32 KB/thread (invisible); at 1024x1024 it is
  8 MB/thread — receiver resolution is bounded by core count on exactly the machines that could
  trace enough rays to fill it.
- Evidence: `RayTracer.cpp:2083-2114`.
- Fix: atomic accumulation into one grid above a size threshold; keep the per-thread path for
  small receivers where its determinism and speed both pay.

## T3-005 — Intersection is still against the tessellation  [KNOWN]
- Problem: normals are exact now (removed most of what the mesh cost), so residual error is hit
  position rather than surface direction. What has not been built is the curvature-driven
  adaptive meshing that would let triangle count fall while position accuracy rises — the mesh is
  still uniform per body, whether the face is an optically active asphere or a mounting flange.
- Evidence: `MeshBuilder.cpp` BRepMesh deflection; Handoff item 1.
- Fix: deflection driven by local curvature and by whether the face carries non-trivial optics.

## T3-006 — Physics deliberately out of scope, stated in five places  [KNOWN]
- Problem: no wave propagation (diffraction limit drawn as overlay, Strehl = Marechal approx, no
  computed PSF), no gradient-index media, no birefringence (Polarisation.h), no fluorescence
  (phosphor exists as a source spectrum, not a converting medium — a real limit for white-LED
  work), no thermal coupling, no ghost/stray-light attribution. Each omission is defensible.
  What is missing is ONE place in the README that states them together, so a reader evaluating
  the tool against LightTools/OpticStudio learns the boundary from you instead of discovering it.
- Evidence: `Polarisation.h:10`; `Spectrum.h` carries phosphor as an SPD only; no GRIN,
  fluorescence, or thermal code in `src/`.
- Fix: add a "What this does not model" section to the README. The existing Notes/limits section
  is the right voice and is already half of it.

## Project facts
- Repo: `C:/Users/kutay/Desktop/Projects/LuxTrace` (git, branch `main`, CMake + C++ engine).
- Build dir `build/` exists. There ARE uncommitted changes (new `src/core/PythonEnv.cpp`,
  modified `src/ui/PythonPanel.*`, `python/runner.py`, tests, `HANDOFF.md`, `README.md`) from a
  prior session — they are NOT part of this cycle; keep working on top, only DevOps commits.
- Only DevOps commits/pushes. Engineers implement, Test Agent writes+runs real tests, Verifier
  is the independent gate.
