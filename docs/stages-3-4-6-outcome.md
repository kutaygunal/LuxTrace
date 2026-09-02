# LuxTrace — outcome of stages 3, 4 and 6

Companion to `stages-3-4-6-brief.md`, which is left as it was written: it is the
brief, and a brief that has been edited to match what happened stops being one.
This is what was done against it, what the acceptance criteria say now, and what
is still open.

Stage 5 (CAD breadth) remains excluded.

---

## 1. Where it stands

| Stage | Was | Now |
|---|---|---|
| 3a — the refractive bias | Plano-Convex Lens at −14.71σ, Axicon at −24.89σ at 8 M rays; `--refcheck all` reported 2 DIFFERS | Fixed. `--refcheck all 8000000` reports 0 differing across all 29 scenes |
| 3b — coverage the gate refused | 5 of 28 scenes refused: scattering, volume scattering, instancing, multiple sources | All of them traced and checked. Scattering (GGX/Smith, ABg, Lambertian, measured tables), volume scattering (Henyey-Greenstein and Gegenbauer), the two-level hierarchy, multiple sources and receivers, measured coating tables, spectral runs, and polarisation |
| 3c — variance reduction on the preview | None | Owen-scrambled Sobol on the emission stream with the error bar measured across independent scrambles rather than ray to ray, next-event estimation, emission aiming, and Russian roulette — each booking what it did not trace, so the budget still closes to 1e-9 |
| 4 — backward tracing and a luminance camera | Not started | `src/core/BackwardTracer.{h,cpp}` and `EnvironmentMap.{h,cpp}`. Pinhole and thin-lens cameras, analytic connections to every emitter, photometric accumulation in cd/m², progressive refinement, a uniform surround, an importance-sampled HDR environment, and a **Measure luminance...** button on the Appearance tab |
| 6 — meta-optics | Not started | `src/core/Metasurface.{h,cpp}` and `MetaFile.{h,cpp}`. The generalised Snell law, diffraction orders with efficiencies, polarisation, a vendor importer, and a Metalens scene |

Verification, all green:

```bash
build/Release/optics_tests.exe            # 493 tests, ~2.35 M checks, 0 failures
build/Release/LuxTrace.exe --validate     # 15 closed-form checks (was 13)
build/Release/LuxTrace.exe --bench 4000   # 58 scene x source, every one deterministic
build/Release/LuxTrace.exe --regress check test/reference/scenes.tsv
build/Release/LuxTrace.exe --refcheck all 8000000  # 28 traced, 0 differing, 1 refused
```

The one refusal is the metalens, refused **by name**: a phase-gradient surface
does not obey Snell's law and the kernel does, so tracing it there would
silently give the answer for the glass wafer instead of for the device printed
on it — a plausible number for a scene whose whole point is that it is not that
number.

---

## 2. Stage 3a — what the refractive bias actually was

The brief's diagnosis had ruled out Fresnel, coatings, absorption, normals,
epsilon, depth truncation, degenerate triangles, medium tracking, the reference's
own estimator and branch sampling, and pointed at single-precision geometry. It
was single precision, and the mechanism was narrower than "float is not enough".

**The hit point.** The kernel computed it as `o + d*t`. Möller-Trumbore produces
`t` as a ratio of triple products built from vectors the length of the whole ray,
so its relative error is a few ulp of *the distance travelled*: a ray crossing
250 mm lands with a couple of ten-thousandths of a millimetre of error along its
own direction. That is the same size as the epsilon the next leg is spawned with,
so the branch started on the wrong side of the surface it had just left,
immediately re-hit it, and refracted a second time at a face it had already
crossed. The ray then left along a direction no interface produced and was booked
as escaped.

The fix is `v0 + e1*u + e2*v` — the same point, anchored on the triangle instead
of on the ray, so its error is a few ulp of the *vertex* coordinates and does not
grow with how far the ray came. On a planar face whose vertices share a
coordinate, that coordinate comes out exact.

It reproduces cleanly. With Fresnel off and a `surfaceOverride` making the lens
purely refractive, the reference detects **exactly** 1.000000 of the light at a
20° half-angle — every ray must land — and the preview detected 0.991151. After
the fix it detects 1.000000 too, and that is now a test.

Two smaller things went with it, both real:

- **The interpolated/facet normal arbitration.** The kernel oriented the shading
  normal by its own sign; the reference orients by the *facet* and falls back to
  the facet where the shading normal has tipped past the incident ray. It also
  re-solves a transmitted ray that comes out on the near side, and books the
  energy if the facet refuses it too. Rare, and a bias when it fires.
- **The ideal coating.** `Coating::reflectanceSP` clamps s and p to the residual
  *separately* and averages afterwards; the kernel clamped the average. Near
  Brewster's angle the p reflectance is already far below anything a coating
  leaves behind while the s reflectance is well above it, so the average clamps
  where only the s half should and the coating reflects roughly twice what the
  reference says. That was the whole of the axicon's residual −0.3 %: with
  coatings off the two had always agreed, which is what named the term.

## 3. Stage 3b — the coverage, and the geometry robustness it needed

Scattering, volume scattering, instancing and multiple sources were mechanical
ports of machinery that already existed, and `raytracer::resolveScattering` is
now called by both backends rather than copied — two backends that disagree
about what a surface *is* cannot be compared.

The geometry was not mechanical. An integrating sphere is a closed diffuse
shell: the reference escapes **exactly nothing** from it, so any leak at all is a
defect rather than a disagreement inside sampling, and it turned out to be the
sharpest geometric oracle in the library. It found four things:

1. **A cracked mesh.** Rounding `v0` and `e1` to float independently makes
   `v0 + e1` disagree with the neighbouring triangle's own copy of that vertex.
   Edges are now rounded *from the rounded vertices*, so the reconstruction is
   bit-exact and two triangles share one edge rather than two that nearly
   coincide.
2. **Tight bounds.** Node boxes rounded to nearest can shrink by half an ulp and
   cull a ray from a node that really does contain the triangle it was about to
   hit. They are rounded outward now.
3. **A non-watertight triangle test.** The reference's Möller-Trumbore carries a
   1e-9 edge slack, which is five million ulp in double and eighty in float. The
   kernel uses the watertight formulation (Woop, Benthin, Wald) instead, where
   the two triangles sharing an edge compute exactly negated determinants for it
   and a ray can never be rejected by both.
4. **The spawn wedge.** A branch offset along its facet's normal is inside that
   facet's half-space but, near a shared edge, outside the *neighbouring*
   facet's — offsetting by ε from a point on the edge lands ε·sin(dihedral)
   beyond the neighbour whatever ε is. The ray re-enters through that neighbour,
   arrives on its outward side, and a diffuse bounce is sampled about the
   outward normal, which walks it out of the cavity. The search now starts four
   offsets along the new ray, which is measured: at 1× the sphere lost sixteen
   rays in four million, at 2× three, at 4× none, and at 10× it starts stepping
   over real hits at normal incidence instead.

## 4. Stage 4 — the luminance camera

`backward::render` traces the same geometry, the same `SurfaceOptics`, the same
coatings and the same spectra the forward tracer does, and reports cd/m². The
estimator is the forward tracer's next-event estimation pointed the other way:
camera ray, walk, and at every scattering vertex connect analytically to every
emitter. Specular vertices carry no connection — a mirror has no direction to
connect along — and pass the path through, so the emitter is picked up by the
direct hit at the end of the chain instead. That split is what keeps the two ways
of reaching a light from being counted twice.

Acceptance:

- **Furnace test.** A large Lambertian emitter over a matte wall renders the
  wall uniform to within what the finite emitter's own solid angle allows.
- **Forward against backward, through `backendcheck::compare`.** A receiver
  just above a matte wall reports illuminance; a camera per receiver bin
  reports luminance; for a Lambertian surface the two are related by exactly
  ρ/π. The comparison is made bin for bin across the wall and sized in each
  estimator's own measured error rather than in a tolerance chosen after the
  fact — the forward run's per-bin variance and the camera's per-pixel
  standard error. A single central point agrees for a scalar factor of π and
  for nothing else; the profile is what catches a missing cosine, since that
  is exactly where a cosine varies. Perturbing the camera side by 3 % fails
  the test, which is how it is known to have teeth.
- **Closed form.** `L = M/π` is the fourteenth case in `--validate`, checked at
  two distances and two view angles — a Lambertian emitter is exactly as bright
  from either, which is the half of the definition a misplaced cosine destroys.
- **The Appearance caption** now says the preview is a picture *and* names the
  measurement, and `AppearanceTests.inc` was changed with it rather than
  deleted. The measurement it names is on the same tab: **Measure
  luminance...** renders the view the user has already framed through
  `backward::render` and writes a CSV of cd/m², with peak, mean, log mean and
  the truncated share reported beside it. The two share the *view* and nothing
  else — framing is what the preview is genuinely good at, and everything
  after the pose comes from the document.

Also there: a uniform surround with a darker floor (most of this library is
specular, and a mirror in a black room lit by a point source is a correct black
rectangle), progressive refinement by passes rather than by rows, and
bit-identical output at any thread count.

### The room

A uniform surround was the room this had, and for an even sky it is exactly
right. What it cannot be is *directional*, which is what a reflective optic is
actually looked at in — and a mirror in an even room is a silhouette rather
than a shape.

`envmap::EnvironmentMap` is a latitude-longitude map of luminance in cd/m²,
loaded from Radiance `.hdr` with a calibration scale (the file's own numbers
are relative, and pretending otherwise would be inventing a photometric
quantity). Two things make it usable rather than merely present:

- **It is sampled.** A sun is a thousandth of the sphere carrying most of the
  light, and a cosine-weighted draw finds it once in a few thousand tries. A
  2D inverse CDF over the map, weighted by luminance times sin θ, connects to
  it analytically at every scattering vertex — the same estimator shape the
  emitters already use, and gated by the same `allowDirect` flag so the room
  cannot be counted twice.
- **The CDF is dilated by one texel.** The lookup is bilinear, so a dark texel
  beside a bright one still reads bright over half its width, and weighting the
  CDF by the stored value gives that half-texel no probability at all. It is
  then reached only by the 1 % defensive uniform share: a thin ring around
  every hard edge, carrying real light, drawn a hundred times too rarely and
  weighted a hundred times too heavily. The estimator stays unbiased and the
  variance is ruinous — a small bright window read **ten per cent high over
  half a million samples, off four of them**. Dilating makes the support the
  sampler covers the same support the lookup has, and the same measurement
  then lands within 0.25 % of a direct quadrature of the map.

A uniform map and a uniform surround of the same luminance light the same wall
to the same number, through two different paths through the code — the
surround by BSDF sampling, the map by an analytic connection. That is the
reduction that keeps this from being a second, disagreeing model of the same
room, and it is exactly where a double count would show as a factor of two.

A scene with a room and no lamps in it is now a scene: `render` used to refuse
an empty source list, which was right while the only surround was decorative
and wrong once the room became an emitter.

Driven headlessly by `--camera <scene> [px] [spp] [csv] [env.hdr] [cd/m² per
unit]` and by the `camera` operation on the JSON API.

## 5. Stage 6 — meta-optics

A metasurface adds a designed, position-dependent phase to the wavefront, so the
tangential wavevector picks up its gradient:

```
n_t sin(theta_t) - n_i sin(theta_i) = (lambda / 2 pi) dPhi/dx
```

Three profiles (a linear gradient, a radial one, and the metalens phase), a
per-order efficiency table over wavelength and polarisation, order sampling with
the evanescent orders booked as absorbed, a Mueller diattenuator for the
polarised case, and a reader for what an EM solver exports.

Acceptance:

- **Reduces to the base case.** With the gradient at zero the surface traces
  *bit-identically* to the glass it replaced — asserted with `==`, not
  `CHECK_NEAR`.
- **Focuses to its design NA.** `sin(theta) = r / sqrt(r² + f²)` at three
  quarters of the aperture, matched to 1.4e-16, and it is the fifteenth case in
  `--validate`.
- **Energy closes** across every order and the remainder, to 1e-9.
- **Chromatic behaviour reversed and large.** Blue focuses further and red
  nearer — the opposite of every refractive lens in the library — and the
  magnitude matches `sqrt((r² + f²)/k² − r²)` with `k = λ/λ_d` exactly, which is
  `f·λ_d/λ` near the axis. Thirty-eight per cent across the visible band against
  a crown-glass singlet's two, in the other direction.

One thing that had to be said explicitly: **a pattern lives on one face**. A
surface property belongs to every face of the solid that carries it, so without
`Metasurface::face` a metalens deflects on the way in and again on the way out
and focuses at half its design length. That is not hypothetical — it is what the
first version did, and what a focus sweep found.

The preview backend refuses a metasurface by name. The kernel obeys Snell's law;
tracing one there would give the answer for the glass wafer instead of for the
device printed on it.

---

## 6. What the preview still refuses, and why

A preview that quietly skips a term is worse than one that refuses. Every
refusal carries a reason and asking it to trace one anyway fails rather than
returning a plausible number, and the test that guards this is keyed to the
*terms* rather than to scenes — a refusal test keyed to scenes says nothing once
the scenes move.

| Refused | Why |
|---|---|
| A multi-layer coating | A characteristic-matrix stack is a complex solve per hit. A measured *table* crosses, because it resolves to one number per run — the same arithmetic done once on the host instead of per hit on the device. |
| A metasurface | It does not obey Snell's law and the kernel does. |
| Stray-light path recording | Per-path history off a device that has no place to put it. |
| A collimated beam with a radius | An emitting aperture the device emitter does not model. |
| Sources with different spectra | The sampled dispersion curves and the inverse CDF are one set per run. Two emitters would need two, and inventing one from the other is exactly what this refuses. |
| A measured ray file | The rays are read, not drawn. |
| A surface edit matching no surface | An edit that silently did nothing is an answer to a question nobody asked. |
| More than eight receivers, or none | A fixed device array, and a run with nothing to measure. |

Everything else crosses. What moved during this work: scattering, volume
scattering, instancing, multiple sources, multiple receivers, measured coating
tables, spectral runs, and polarisation.

### Polarisation on the preview

The kernel carries a Stokes vector per branch and rotates it into each plane of
incidence before the interface acts on it — the step that is easy to leave out
and impossible to see the absence of in a single number. Both the Fresnel split
and the metal reflection are then decided by the *state* rather than by the
unpolarised average of the two, which is the difference the state exists to
express: at Brewster's angle s-polarised light reflects strongly off glass and
p-polarised light reflects nothing at all, and averaging throws away exactly
that.

Two comparison tests. Unpolarised light off glass at atan(1.5) comes back better
than 90 % linearly polarised on both backends and they agree on the degree to
0.02; an s-polarised source delivers more than twenty times the flux a
p-polarised one does through the same geometry, which a model that averaged
could not produce. And circular light off an aluminium mirror at 60° comes back
elliptical — if the reflected branch had inherited the source's state verbatim
that difference would be exactly zero, which is the bug the test replaces.

### Two bugs the spectral comparison found

Writing the broadband prism comparison turned up two real disagreements that
every existing check had missed, both because the whole scene library happens to
be quoted at the d line:

1. **A monochromatic run read the catalogue index, not `n(λ)`.** The host
   uploaded `surfs[i].index` — the index at 587.6 nm — so a monochromatic run at
   450 nm and one at 650 nm put the prism's spot in exactly the same place while
   the reference separated them by two bins. It now uploads
   `indexAt(lambdaNm, dispersion)`, and the two backends agree on the separation
   to four decimals.
2. **The spectral draw was off the low-discrepancy stream.** The reference draws
   the wavelength on Sobol dimension 4; the device took a fresh uniform. On a
   photometric run every ray is weighted by V at its own wavelength and the run
   divides by the mean V of the distribution, so a spectral draw whose sampled
   mean is a fraction of a per cent off leaves the whole budget a fraction of a
   per cent from closing — 0.28 %, at 7σ. The device now draws it on the same
   dimension.

---

## 7. What is still open

- **The preview leaks about a part in a million from a closed cavity.** Down
  from three parts in ten thousand, and the residue is the floor of
  single-precision geometry: a stratified emission stream finds the
  near-tangential configuration reliably where independent uniforms found it
  about never. Bounded and asserted at 1e-5 of the source rather than rounded
  away.
- **Multi-layer coatings and metasurfaces are still refused** on the preview,
  both by name and for the reasons in the table above. Neither is a port; both
  are a second implementation of a model, and a second implementation is a
  second chance to disagree.
- **Metasurface retardance.** The efficiency table carries moduli per
  polarisation; the phase between s and p is dropped, because nothing downstream
  carries a retardance for a diffraction order yet. Inventing one would be worse
  than saying so.
- **The environment map is monochrome.** It is stored as luminance in cd/m², so
  a room can be bright or dim in any direction but has no colour, and a scene
  lit only by it takes the d line for its dispersion. Colour would mean a
  spectral map and a spectral connection, which is a larger change than it
  looks.
- **The luminance measurement blocks the window while it runs.** It has a
  progress dialog and a working Stop, which is the same arrangement the PNG
  export uses, but it is not a background job and a large one will sit there.

