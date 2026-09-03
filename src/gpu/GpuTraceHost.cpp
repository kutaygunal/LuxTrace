// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "gpu/GpuTrace.h"
#include "gpu/GpuKernel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "core/Sampling.h"
#include "core/Spectrum.h"

#include <QStringList>

// The host half of the preview backend: it decides whether the scene is one the
// kernel can be trusted with, flattens it into plain numbers, and turns what
// comes back into a SimulationResult. Everything Qt and everything OCCT stops
// here.
namespace gputrace {
namespace {

void put3(float* dst, const Vec3& v) {
    dst[0] = float(v.x); dst[1] = float(v.y); dst[2] = float(v.z);
}

// Rounded outward, never to nearest.
//
// The reference keeps its bounds in double and they enclose their triangles
// exactly. Rounding each to the nearest float can shrink a box by half an
// ulp, and a ray that grazes a wall from the inside is then culled from a
// node that really does contain the triangle it was about to hit -- so it
// passes through the wall and is booked as escaped. A hierarchy is an
// acceleration structure: it is allowed to be loose and never to be tight.
float down(double x) {
    const float f = float(x);
    return double(f) > x ? std::nextafterf(f, -FLT_MAX) : f;
}
float up(double x) {
    const float f = float(x);
    return double(f) < x ? std::nextafterf(f, FLT_MAX) : f;
}

gpukernel::Node putNode(const TraceScene::BvhNode& n) {
    gpukernel::Node out{};
    for (int k = 0; k < 3; ++k) {
        out.bmin[k] = down(n.bmin[k]);
        out.bmax[k] = up(n.bmax[k]);
    }
    out.left  = n.left;
    out.right = n.right;
    out.count = n.count;
    out.axis  = n.axis;
    return out;
}

// Edges rounded from the rounded *vertices*, not rounded independently.
//
// Rounding v0 and e1 separately makes v0 + e1 disagree with the neighbouring
// triangle's own copy of that vertex by an ulp, so the mesh acquires a
// hairline crack along every shared edge. A ray sliding along a wall at
// grazing -- which is most of what happens inside an integrating sphere,
// where a cosine-weighted bounce is drawn near the tangent plane about as
// often as anywhere else -- crosses hundreds of those edges and eventually
// slips through one.
//
// Taking the difference of two floats gives that difference exactly
// (Sterbenz, for the near-equal magnitudes a mesh edge has), so v0 + e1
// reconstructs the neighbour's vertex bit for bit and the two triangles
// share one edge rather than two that nearly coincide.
void putTri(gpukernel::Tri& dst, const SceneTri& src) {
    float v1[3], v2[3];
    put3(dst.v0, src.v0);
    put3(v1, src.v0 + src.e1);
    put3(v2, src.v0 + src.e2);
    for (int k = 0; k < 3; ++k) {
        dst.e1[k] = v1[k] - dst.v0[k];
        dst.e2[k] = v2[k] - dst.v0[k];
    }
    put3(dst.n, src.n);
    dst.surf = src.surf;
}

} // namespace

bool available() {
    char name[256] = {0}, why[256] = {0};
    return gpukernel::probe(name, sizeof(name), why, sizeof(why));
}

QString deviceName() {
    char name[256] = {0}, why[256] = {0};
    if (!gpukernel::probe(name, sizeof(name), why, sizeof(why))) return QString();
    return QString::fromLatin1(name);
}

QString unavailableReason() {
    char name[256] = {0}, why[256] = {0};
    if (gpukernel::probe(name, sizeof(name), why, sizeof(why))) return QString();
    return QString::fromLatin1(why);
}

// The surfaces a run actually traces: the scene's, with the caller's per-surface
// edits applied over them, resolved by label exactly as the reference resolves
// them. Without this a surface a user had edited would be traced unedited on the
// preview and edited on the reference -- the two would disagree and the edit
// would appear not to work.
std::vector<SceneSurface> effectiveSurfaces(const TraceScene& scene,
                                            const std::vector<SceneSurface>& surfs,
                                            const TraceOptions& opt) {
    std::vector<SceneSurface> out = surfs;
    for (const SurfaceOverride& ov : opt.surfaceOverrides) {
        const int idx = scene.resolveSurface(ov.label, ov.identity, ov.surface);
        if (idx >= 0 && idx < int(out.size())) out[std::size_t(idx)] = ov.optics;
    }
    // The scene-wide overrides and the `scatter` / `roughness` shorthands
    // folded into the one BSDF a tracer reads -- through the reference's own
    // function, not a copy of it. Two backends that disagree about what a
    // surface *is* are not tracing the same scene.
    resolveScattering(out, opt.physics);
    return out;
}

Support supports(const TraceScene& scene, const std::vector<SceneSurface>& surfsIn,
                 const std::vector<SourceConfig>& srcs, const TraceOptions& opt) {
    Support s;
    auto no = [&](const QString& why) { s.reasons << why; };
    // Judged on the surfaces the run would actually trace, not on the scene's
    // originals: an edit can turn a polished surface into a scattering one.
    const std::vector<SceneSurface> surfs = effectiveSurfaces(scene, surfsIn, opt);

    if (!available()) no(QStringLiteral("no CUDA device: %1").arg(unavailableReason()));
    if (scene.detectors().empty()) no(QStringLiteral("no receiver"));
    if (scene.detectors().size() > std::size_t(gpukernel::kMaxReceivers))
        no(QStringLiteral("more receivers than one connection can reach"));
    if (srcs.empty()) no(QStringLiteral("no source"));
    for (const SourceConfig& c : srcs) {
        // A collimated source spends its angular draws on the beam disc, which
        // the kernel does not do; every other shape it does.
        if (c.type == SourceConfig::Type::Collimated && c.beamRadius > 0.0)
            no(QStringLiteral("a collimated beam with a radius"));
        if (c.rayFile && c.rayFile->valid()) no(QStringLiteral("a measured ray file"));
    }

    // GGX with Smith masking, ABg, Lambertian and a measured table all cross
    // now, and so does Henyey-Greenstein and Gegenbauer volume scattering.
    // What still does not is a measured or multi-layer coating: a table is
    // one more array, but a characteristic-matrix stack is a complex solve
    // per hit, and approximating it here would be inventing a coating.
    bool sawStack = false, sawMeta = false;
    for (const SceneSurface& f : surfs) {
        // A measured table crosses: at the one wavelength the preview traces,
        // it resolves to the same single residual the ideal model uses. A
        // characteristic-matrix stack does not -- it is a complex solve per
        // hit, and approximating it here would be inventing a coating.
        if (f.coating.model == coating::Model::Stack) sawStack = true;
        if (f.metasurface.active()) sawMeta = true;
    }
    if (sawStack && opt.physics.coatings)
        no(QStringLiteral("a multi-layer coating"));
    // A phase-gradient surface does not obey Snell law, and the kernel does.
    // Tracing it here would silently give the answer for the glass wafer
    // instead of for the device printed on it -- a plausible number for a
    // scene whose whole point is that it is not that number.
    if (sawMeta) no(QStringLiteral("a metasurface"));

    // Polarisation used to be refused here. The kernel carries a Stokes
    // vector per branch now, with the frame rotated into each plane of
    // incidence, and both the Fresnel split and the metal reflection are
    // decided by the state rather than by the unpolarised average of the two
    // -- which is the difference the state exists to express.
    // Dispersion only bites on a spectral run: with one line, n(lambda) is the
    // one number the surface already carries. Refusing it unconditionally would
    // turn away every monochromatic scene for a term that does nothing to them.
    // A spectral run draws a wavelength per ray and reads every
    // wavelength-dependent quantity at it, off curves the host sampled from
    // the reference own dispersion models. What it cannot do is carry a
    // *different* spectrum per source: the sampled curves and the inverse
    // CDF are one set per run.
    for (std::size_t i = 1; i < srcs.size(); ++i)
        if (srcs[i].spectrum.kind != srcs.front().spectrum.kind ||
            srcs[i].spectrum.wavelengthNm != srcs.front().spectrum.wavelengthNm ||
            srcs[i].spectrum.cct != srcs.front().spectrum.cct) {
            no(QStringLiteral("sources with different spectra"));
            break;
        }
    if (opt.strayPaths.enabled) no(QStringLiteral("stray-light path recording"));
    for (const SurfaceOverride& ov : opt.surfaceOverrides)
        if (scene.resolveSurface(ov.label, ov.identity, ov.surface) < 0) {
            no(QStringLiteral("a surface edit that matches no surface"));
            break;
        }

    s.ok = s.reasons.isEmpty();
    return s;
}

bool trace(const TraceScene& scene, const std::vector<SceneSurface>& surfsIn,
           const std::vector<SourceConfig>& srcs, const TraceOptions& opt,
           double totalPower, FluxUnit unit, SimulationResult& out, QString* error) {
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };

    const std::vector<SceneSurface> surfs = effectiveSurfaces(scene, surfsIn, opt);
    const Support sup = supports(scene, surfsIn, srcs, opt);
    if (!sup.ok)
        return fail(QStringLiteral("the preview backend cannot take this scene: ") +
                    sup.reasons.join(QStringLiteral("; ")));

    // The one wavelength the preview traces at. A spectral source is refused by
    // supports(), so this is the line the whole run is on.
    const double lambdaNm = srcs.front().spectrum.wavelengthNm;

    const std::vector<SceneTri>&            tris  = scene.triangles();
    const std::vector<TriShading>&          shade = scene.shading();
    const std::vector<int>&                 order = scene.order();
    const std::vector<TraceScene::BvhNode>& nodes = scene.nodes();
    const DetectorInfo& det = scene.detectors().front();
    if ((tris.empty() || nodes.empty()) && scene.instances().empty())
        return fail(QStringLiteral("the scene has no geometry"));
    if (!det.valid || det.nx <= 0 || det.ny <= 0)
        return fail(QStringLiteral("the scene has no usable receiver"));

    std::vector<gpukernel::Tri> hTris(tris.size());
    for (std::size_t i = 0; i < tris.size(); ++i) putTri(hTris[i], tris[i]);
    std::vector<gpukernel::Shade> hShade;
    if (shade.size() == tris.size()) {
        hShade.resize(shade.size());
        for (std::size_t i = 0; i < shade.size(); ++i) {
            put3(hShade[i].n0, shade[i].n0);
            put3(hShade[i].n1, shade[i].n1);
            put3(hShade[i].n2, shade[i].n2);
        }
    }
    std::vector<gpukernel::Node> hNodes(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) hNodes[i] = putNode(nodes[i]);
    std::vector<gpukernel::Surf> hSurfs(surfs.size());
    // Every measured BSDF in the scene, end to end; each surface keeps the
    // slice it owns.
    std::vector<float> tableBeta, tableValue;
    for (std::size_t i = 0; i < surfs.size(); ++i) {
        // The index at the run wavelength, not the catalogue index at the d
        // line. A spectral run overrides this per ray off the sampled curve
        // below; a monochromatic run at any other line has only this number, and
        // reading it at 587.6 nm was a silent disagreement with the reference --
        // invisible across the scene library only because every scene there
        // happens to be quoted at the d line.
        hSurfs[i].index          = float(surfs[i].indexAt(lambdaNm, opt.physics.dispersion));
        hSurfs[i].reflectivity   = float(surfs[i].reflectivity);
        hSurfs[i].transmissivity = float(surfs[i].transmissivity);
        hSurfs[i].absorption     = opt.physics.absorption
                                       ? float(surfs[i].absorption * opt.physics.absorptionScale)
                                       : 0.0f;
        hSurfs[i].fresnel        = (surfs[i].fresnel && opt.physics.fresnel) ? 1 : 0;
        hSurfs[i].isDetector     = surfs[i].isDetector ? 1 : 0;
        // The metal's complex index at the run's wavelength. Sampled once here
        // rather than per hit: the preview is monochromatic by construction, so
        // there is one pair of numbers per surface for the whole run.
        hSurfs[i].metalN = 0.0f;
        hSurfs[i].metalK = 0.0f;
        hSurfs[i].coatResidual      = -1.0f;
        hSurfs[i].coatHighReflector = 0;
        hSurfs[i].mediumPriority    = surfs[i].mediumPriority;
        // The ideal model and the measured table both cross, because at one
        // wavelength they are the same thing: Coating::reflectanceSP reads a
        // table only to pick the residual it then rolls off exactly as the
        // ideal model does. The preview is monochromatic by construction, so
        // the table is resolved to that one number here rather than being
        // carried to the device and interpolated per hit -- which is not an
        // approximation, it is the same arithmetic done once.
        //
        // A characteristic-matrix stack is a different thing and is still
        // refused: it is a complex solve per hit, and approximating it here
        // would be inventing a coating.
        if (opt.physics.coatings) {
            const coating::Coating& c = surfs[i].coating;
            if (c.model == coating::Model::Ideal) {
                hSurfs[i].coatResidual      = float(c.residual);
                hSurfs[i].coatHighReflector = c.highReflector ? 1 : 0;
            } else if (c.model == coating::Model::Table && c.samples > 0) {
                hSurfs[i].coatResidual =
                    float(coating::tableReflectanceAt(c, lambdaNm));
                hSurfs[i].coatHighReflector = c.highReflector ? 1 : 0;
            }
        }
        // Keyed off the material and the physics switch, not off the surface's
        // own `fresnel` flag -- which is what the reference does, and a mirror
        // built by the registry does not set that flag.
        if (opt.physics.fresnel && surfs[i].material.isMetal()) {
            hSurfs[i].metalN = float(surfs[i].material.indexAt(lambdaNm));
            hSurfs[i].metalK = float(surfs[i].material.extinctionAt(lambdaNm));
        }

        // The scatter lobe, already resolved by effectiveSurfaces. The
        // total integrated scatter comes across as a number rather than as
        // the parameters behind it: for ABg and for a measured table it is
        // a four-hundred-step quadrature, which belongs once per run on the
        // host and not once per hit on the device.
        const bsdf::Surface& lobe = surfs[i].bsdf;
        hSurfs[i].bsdfModel    = int(lobe.model);
        hSurfs[i].bsdfAlpha    = float(lobe.alpha);
        hSurfs[i].abgA         = float(lobe.abgA);
        hSurfs[i].abgB         = float(lobe.abgB);
        hSurfs[i].abgG         = float(lobe.abgG);
        hSurfs[i].bsdfFraction = float(lobe.fraction);
        hSurfs[i].bsdfTis      = float(lobe.totalIntegratedScatter());
        hSurfs[i].tableFirst   = 0;
        hSurfs[i].tableCount   = 0;
        if (lobe.model == bsdf::Model::Table && lobe.samples > 0) {
            hSurfs[i].tableFirst = int(tableBeta.size());
            hSurfs[i].tableCount = lobe.samples;
            for (int k = 0; k < lobe.samples; ++k) {
                tableBeta.push_back(float(lobe.tableBeta[k]));
                tableValue.push_back(float(lobe.tableValue[k]));
            }
        }

        // Scattering inside the medium behind this surface, gated by the
        // same switch the reference gates it with.
        const bsdf::Volume& vol = surfs[i].volume;
        const bool wantVol = opt.physics.volumeScattering && vol.active();
        hSurfs[i].volCoeff = wantVol ? float(vol.coefficient) : 0.0f;
        hSurfs[i].volG     = float(vol.anisotropy);
        hSurfs[i].volAlpha = float(vol.alpha);
        hSurfs[i].volPhase = int(vol.phase);
    }

    // The instanced half of the hierarchy, flattened.
    //
    // Every part's triangles, shading normals, order and nodes are concatenated
    // into one array each, and each part keeps the offsets of its own slice.
    // The node and order indices inside a part stay relative to it, so a part
    // placed twenty-five times is still uploaded once -- which is the whole
    // point of the two-level hierarchy and the reason the microlens array can
    // carry the same fine mesh every other optic in the library has.
    std::vector<gpukernel::Tri>   hBlasTris;
    std::vector<gpukernel::Shade> hBlasShade;
    std::vector<gpukernel::Node>  hBlasNodes;
    std::vector<int>              hBlasOrder;
    std::vector<gpukernel::Blas>  hBlas;
    std::vector<gpukernel::Inst>  hInsts;
    std::vector<gpukernel::Node>  hTlas;
    std::vector<int>              hTlasOrder;
    if (!scene.instances().empty()) {
        const std::vector<TraceScene::Blas>& parts = scene.blasParts();
        // Shading normals are all-or-nothing across the parts: a kernel that
        // read them for some placements and facet normals for others would
        // focus half a microlens array differently from the other half.
        bool everyPartShades = true;
        for (const TraceScene::Blas& b : parts)
            if (b.shade.size() != b.tris.size()) everyPartShades = false;

        hBlas.resize(parts.size());
        for (std::size_t bi = 0; bi < parts.size(); ++bi) {
            const TraceScene::Blas& b = parts[bi];
            hBlas[bi].triFirst   = int(hBlasTris.size());
            hBlas[bi].nodeFirst  = int(hBlasNodes.size());
            hBlas[bi].orderFirst = int(hBlasOrder.size());
            hBlas[bi].nodeCount  = int(b.nodes.size());
            for (std::size_t i = 0; i < b.tris.size(); ++i) {
                gpukernel::Tri tr{};
                putTri(tr, b.tris[i]);
                hBlasTris.push_back(tr);
                if (everyPartShades) {
                    gpukernel::Shade sh{};
                    put3(sh.n0, b.shade[i].n0);
                    put3(sh.n1, b.shade[i].n1);
                    put3(sh.n2, b.shade[i].n2);
                    hBlasShade.push_back(sh);
                }
            }
            for (const TraceScene::BvhNode& nd : b.nodes) hBlasNodes.push_back(putNode(nd));
            for (int v : b.order) hBlasOrder.push_back(v);
        }
        hInsts.resize(scene.instances().size());
        for (std::size_t i = 0; i < scene.instances().size(); ++i) {
            const Instance& in = scene.instances()[i];
            put3(hInsts[i].r0, in.r0);   put3(hInsts[i].r1, in.r1);
            put3(hInsts[i].r2, in.r2);   put3(hInsts[i].origin, in.origin);
            put3(hInsts[i].i0, in.i0);   put3(hInsts[i].i1, in.i1);
            put3(hInsts[i].i2, in.i2);
            for (int k = 0; k < 3; ++k) {
                hInsts[i].bmin[k] = down(in.bmin[k]);
                hInsts[i].bmax[k] = up(in.bmax[k]);
            }
            hInsts[i].blas = in.blas;
            hInsts[i].surf = in.surf;
        }
        for (const TraceScene::BvhNode& nd : scene.tlasNodes()) hTlas.push_back(putNode(nd));
        hTlasOrder = scene.tlasOrder();
    }


    gpukernel::Input in;
    in.tris  = hTris.data();  in.nTris  = int(hTris.size());
    in.shade = hShade.empty() ? nullptr : hShade.data();
    in.nodes = hNodes.data(); in.nNodes = int(hNodes.size());
    in.order = order.data();  in.nOrder = int(order.size());
    in.surfs = hSurfs.data(); in.nSurfs = int(hSurfs.size());
    in.tableBeta  = tableBeta.empty()  ? nullptr : tableBeta.data();
    in.tableValue = tableValue.empty() ? nullptr : tableValue.data();
    in.nTable     = int(tableBeta.size());
    in.blasTris  = hBlasTris.empty()  ? nullptr : hBlasTris.data();
    in.nBlasTris = int(hBlasTris.size());
    in.blasShade = hBlasShade.empty() ? nullptr : hBlasShade.data();
    in.blasNodes = hBlasNodes.empty() ? nullptr : hBlasNodes.data();
    in.nBlasNodes = int(hBlasNodes.size());
    in.blasOrder = hBlasOrder.empty() ? nullptr : hBlasOrder.data();
    in.nBlasOrder = int(hBlasOrder.size());
    in.blas      = hBlas.empty()      ? nullptr : hBlas.data();
    in.nBlas     = int(hBlas.size());
    in.insts     = hInsts.empty()     ? nullptr : hInsts.data();
    in.nInsts    = int(hInsts.size());
    in.tlas      = hTlas.empty()      ? nullptr : hTlas.data();
    in.nTlas     = int(hTlas.size());
    in.tlasOrder = hTlasOrder.empty() ? nullptr : hTlasOrder.data();
    in.nTlasOrder = int(hTlasOrder.size());

    // Every receiver in the scene, laid out the way the reference lays them
    // out: one grid holding them all end to end, each with the offset its bins
    // begin at. A second receiver is one more slice rather than a second run,
    // and the flat layout is what lets scatterDetectorGrids unpack both
    // backends the same way.
    const std::vector<DetectorInfo>& dets = scene.detectors();
    std::vector<gpukernel::Det> hDets(dets.size());
    std::size_t gridCells = 0;
    for (std::size_t i = 0; i < dets.size(); ++i) {
        const DetectorInfo& dd = dets[i];
        put3(hDets[i].center, dd.center);
        put3(hDets[i].u, dd.u);
        put3(hDets[i].v, dd.v);
        put3(hDets[i].n, dd.n);
        hDets[i].w  = float(dd.w);
        hDets[i].h  = float(dd.h);
        hDets[i].nx = dd.nx;
        hDets[i].ny = dd.ny;
        hDets[i].cosAcceptance = float(dd.cosAcceptance);
        hDets[i].rejectPasses  = dd.rejectPasses ? 1 : 0;
        hDets[i].surf          = dd.surf;
        hDets[i].cellFirst     = int(gridCells);
        gridCells += std::size_t(dd.nx) * std::size_t(dd.ny);
    }
    // Which receiver each surface is, or -1. The kernel reads it off the hit
    // rather than searching, which is what keeps a second receiver free.
    std::vector<int> hDetOfSurf(scene.surfaces().size(), -1);
    for (std::size_t i = 0; i < hDetOfSurf.size(); ++i)
        hDetOfSurf[i] = scene.detectorOfSurface(int(i));

    in.dets       = hDets.empty() ? nullptr : hDets.data();
    in.nDets      = int(hDets.size());
    in.detOfSurf  = hDetOfSurf.empty() ? nullptr : hDetOfSurf.data();
    in.nDetOfSurf = int(hDetOfSurf.size());
    in.gridCells  = int(gridCells);
    // Connect every diffuse bounce to every receiver analytically instead of
    // hoping a random walk finds it. Order-of-magnitude variance reduction on an
    // integrating sphere or behind a diffuser; nothing at all on a purely
    // specular scene, where there are no diffuse bounces to connect.
    in.nextEvent  = opt.estimator.nextEventEstimation ? 1 : 0;
    // Emit only into the cone the scene occupies. A source that radiates
    // into a hemisphere at an optic filling a twentieth of the sky spends
    // most of its rays on directions that provably hit nothing.
    in.aim = opt.estimator.aimAtScene ? 1 : 0;
    put3(in.boundsCentre, scene.boundsCenter());
    in.boundsRadius = float(scene.boundsRadius());
    // Follow a dim branch with a probability rather than dropping it.
    in.roulette = opt.physics.varianceReduction ? 1 : 0;

    // The run spectrum, and every wavelength-dependent surface quantity sampled
    // across the band it covers.
    //
    // The reference evaluates dispersion exactly -- Sellmeier, Cauchy, a
    // measured table, four models between them -- and porting all four to the
    // device would be four more chances to disagree about what a glass is. So
    // each curve is sampled once per run, here, off the reference's own
    // functions, and the device interpolates. At 256 samples the spacing is a
    // nanometre and a half and the interpolation error in n is under a part in a
    // million, which is below what a float carries: it is not a second
    // dispersion model, it is the reference's curve read at a resolution the
    // arithmetic cannot tell from exact.
    SampledSpectrum spec;
    spec.build(srcs.front().spectrum);
    const bool spectral = !spec.monochromatic();

    std::vector<float> invCdf;
    if (!spec.monochromatic() && !spec.rgbBands()) {
        invCdf.resize(std::size_t(SampledSpectrum::kBins) + 1);
        for (int b = 0; b <= SampledSpectrum::kBins; ++b) {
            // The sampler is exposed only as sample(index, u), so the inverse
            // CDF is read back through it at the u each bin edge stands for.
            const double u = double(b) / double(SampledSpectrum::kBins);
            invCdf[std::size_t(b)] = float(spec.sample(0, std::min(u, 1.0)));
        }
    }
    in.invCdf     = invCdf.empty() ? nullptr : invCdf.data();
    in.spdBins    = invCdf.empty() ? 0 : SampledSpectrum::kBins;
    in.spdMono    = spec.monochromatic() ? 1 : 0;
    in.spdRgb     = spec.rgbBands() ? 1 : 0;
    in.monoLambda = float(lambdaNm);
    in.meanV      = float(spec.efficacy() / 683.0);
    in.unitLumen  = (unit == FluxUnit::Lumen) ? 1 : 0;
    in.spectral   = spectral ? 1 : 0;

    std::vector<float> specTable;
    if (spectral) {
        constexpr int kSpecSamples = 256;
        const double lo = std::max(200.0, srcs.front().spectrum.minNm);
        const double hi = std::max(lo + 1.0, srcs.front().spectrum.maxNm);
        in.specSamples = kSpecSamples;
        in.specMinNm   = float(lo);
        in.specMaxNm   = float(hi);
        specTable.assign(surfs.size() * 4 * std::size_t(kSpecSamples), 0.0f);
        for (std::size_t i = 0; i < surfs.size(); ++i) {
            const SceneSurface& sf = surfs[i];
            for (int k = 0; k < kSpecSamples; ++k) {
                const double l = lo + (hi - lo) * double(k) / double(kSpecSamples - 1);
                const std::size_t base = (i * 4) * std::size_t(kSpecSamples);
                specTable[base + 0 * kSpecSamples + std::size_t(k)] =
                    float(sf.indexAt(l, opt.physics.dispersion));
                if (sf.material.isMetal()) {
                    specTable[base + 1 * kSpecSamples + std::size_t(k)] =
                        float(sf.material.indexAt(l));
                    specTable[base + 2 * kSpecSamples + std::size_t(k)] =
                        float(sf.material.extinctionAt(l));
                }
                if (opt.physics.coatings && sf.coating.model == coating::Model::Table)
                    specTable[base + 3 * kSpecSamples + std::size_t(k)] =
                        float(coating::tableReflectanceAt(sf.coating, l));
                else
                    specTable[base + 3 * kSpecSamples + std::size_t(k)] =
                        float(sf.coating.residual);
            }
        }
        in.specTable = specTable.data();
    }

    // The same master resolution the reference accumulates on, so both far
    // fields are built by finishIntensityGrid from grids of the same shape.
    const int nTheta = std::max(0, opt.nTheta);
    const int nPhi   = nTheta > 0 ? std::max(1, opt.nPhi) : 0;
    in.nThetaMaster  = nTheta > 0 ? std::max(nTheta, 720) : 0;
    in.nPhi          = nPhi;
    in.wantVariance  = opt.noiseMap ? 1 : 0;

    // The emission stream, over the reference's own direction numbers. A
    // second table built here would be a second chance to disagree about
    // what the sequence is, and the whole comparison rests on the two
    // backends differing only where they are meant to.
    std::vector<unsigned int> sobolDir(sampling::kMaxDim * 32, 0u);
    for (int d = 0; d < sampling::kMaxDim; ++d)
        for (int b = 0; b < 32; ++b)
            sobolDir[std::size_t(d) * 32 + std::size_t(b)] =
                sampling::directions().v[d][b];
    in.sobolDir       = sobolDir.data();
    in.lowDiscrepancy = opt.estimator.lowDiscrepancy ? 1 : 0;
    in.replicas       = opt.estimator.lowDiscrepancy
                            ? std::clamp(opt.estimator.replicas, 2, 256)
                            : 1;
    std::vector<double> repDet(std::size_t(in.replicas), 0.0);
    std::vector<double> repEmit(std::size_t(in.replicas), 0.0);
    // Replicas partition each source block rather than the run as a whole,
    // so every replica holds a share of every emitter and the spread across
    // them still measures the error of the combined answer.
    std::vector<double> repDetOne(std::size_t(in.replicas), 0.0);
    std::vector<double> repEmitOne(std::size_t(in.replicas), 0.0);

    const std::size_t cells = std::size_t(in.gridCells);
    const std::size_t angleCells =
        in.nThetaMaster > 0 ? std::size_t(in.nThetaMaster) * std::size_t(in.nPhi) : 0;

    // One launch per source, accumulated into one set of grids.
    //
    // The reference partitions a single global ray index space across the
    // sources and emits each ray already carrying power_s / rays_s, so one
    // receiver grid holds every source correctly weighted. This does the same
    // arithmetic with the loop on the outside, because a launch carries one
    // emitter: the kernel would otherwise need a per-ray branch over a source
    // array to answer a question the host has already answered. A two-source
    // run costs two launches and one grid, and for a single source it is the
    // normalisation this always had.
    //
    // The total power is the sum over the sources rather than the caller's
    // figure, which is what the reference does: a second emitter adds its own
    // power to the run, and reporting the primary's alone would make a
    // two-LED fixture look twice as efficient as it is.
    out = SimulationResult{};
    double runPower = 0.0;
    for (const SourceConfig& c : srcs) runPower += c.power;
    if (runPower <= 0.0) runPower = totalPower;

    std::vector<double> grid(cells, 0.0), gridSq;
    if (opt.noiseMap) gridSq.assign(cells, 0.0);
    // Three colour bands over the first receiver, on a spectral run only.
    // They sum to the irradiance grid exactly, which is what every
    // band-wise reading of the result depends on.
    std::vector<double> bandGrid;
    if (spectral) bandGrid.assign(3 * cells, 0.0);
    std::vector<double> angles, anglesSq;
    if (angleCells > 0) {
        angles.assign(angleCells, 0.0);
        if (opt.noiseMap) anglesSq.assign(angleCells, 0.0);
    }

    double totals[6] = {0, 0, 0, 0, 0, 0};
    double residualBsdf = 0.0, residualNee = 0.0, residualSkip = 0.0;
    double residualAim  = 0.0, residualRoul = 0.0;
    double fluxVar = 0.0;
    std::size_t hits = 0, raysEmitted = 0;
    unsigned long long anomalies[3] = {0, 0, 0};
    // Flux-weighted Stokes of what reached the receiver, summed over the
    // sources in the same flux units the grids are in, so two sources of
    // different power combine by how much light each actually delivered.
    double arrivedStokes[4] = {0, 0, 0, 0};
    double seconds = 0.0;
    out.sources.clear();
    out.sources.reserve(srcs.size());

    std::vector<double> sGrid(cells, 0.0), sGridSq, sAngles, sAnglesSq, sBand;
    if (opt.noiseMap) sGridSq.assign(cells, 0.0);
    if (spectral) sBand.assign(3 * cells, 0.0);
    if (angleCells > 0) {
        sAngles.assign(angleCells, 0.0);
        if (opt.noiseMap) sAnglesSq.assign(angleCells, 0.0);
    }

    for (std::size_t si = 0; si < srcs.size(); ++si) {
        const SourceConfig& src = srcs[si];
        put3(in.src.origin, Vec3(src.origin));
        put3(in.src.axis,   Vec3(src.axis));
        in.src.type  = int(src.type);
        in.src.shape = int(src.shape);          // PointLike, Disc, Rect, Sphere
        in.src.sizeA = float(src.sizeA);
        in.src.sizeB = float(src.sizeB);
        in.src.beamRadius = float(src.beamRadius);
        // The solids this emitter sits inside, found by the reference own
        // containment probe so the two backends cannot disagree about where
        // a source is. An LED die in its own encapsulant starts its rays in
        // that encapsulant; without this the first interface refracts
        // against the wrong pair of indices.
        {
            const std::vector<int> inside =
                mediaContainingPoint(scene, surfs, Vec3(src.origin));
            in.src.mediaN = int(std::min<std::size_t>(inside.size(), 8));
            for (int k = 0; k < in.src.mediaN; ++k) in.src.media[k] = inside[std::size_t(k)];
        }
        {
            double half = src.halfAngleDeg;
            if (src.type == SourceConfig::Type::Lambertian) half = std::min(half, 90.0);
            half = std::clamp(half, 0.0, 180.0);
            in.src.cosHalfAngle = float(std::cos(half * 3.14159265358979323846 / 180.0));
        }
        // The state this source emits, normalised to unit intensity.
        // Unpolarised is the default and the only state an ordinary
        // illumination source is in; the polarised ones are the same four
        // the reference offers, read from the same field, because two
        // spellings of what "circular" means would be two answers.
        in.polarised = opt.physics.polarised ? 1 : 0;
        in.emittedState[0] = 1.0f;
        in.emittedState[1] = 0.0f;
        in.emittedState[2] = 0.0f;
        in.emittedState[3] = 0.0f;
        switch (src.polarisationState) {
        case 1: in.emittedState[1] =  1.0f; break;   // linear s
        case 2: in.emittedState[1] = -1.0f; break;   // linear p
        case 3: in.emittedState[3] =  1.0f; break;   // circular
        default: break;
        }
        in.rays = std::max<long long>(1, src.rays);
        // A different stream per source, mixed rather than added so two sources
        // one apart in the list do not trace nearly the same rays.
        in.seed = opt.seed ^ (0x9e3779b97f4a7c15ULL * (si + 1));

        gpukernel::Output res;
        res.grid     = sGrid.data();
        res.gridSq   = sGridSq.empty()   ? nullptr : sGridSq.data();
        res.angles   = sAngles.empty()   ? nullptr : sAngles.data();
        res.anglesSq = sAnglesSq.empty() ? nullptr : sAnglesSq.data();
        res.bandGrid = sBand.empty() ? nullptr : sBand.data();
        res.repDet   = repDetOne.data();
        res.repEmit  = repEmitOne.data();
        std::fill(repDetOne.begin(),  repDetOne.end(),  0.0);
        std::fill(repEmitOne.begin(), repEmitOne.end(), 0.0);
        std::fill(sBand.begin(), sBand.end(), 0.0);

        char err[256] = {0};
        if (!gpukernel::run(in, res, err, sizeof(err)))
            return fail(QStringLiteral("the device failed: %1").arg(QString::fromLatin1(err)));

        // Every ray was emitted carrying one, so the scale is simply the flux
        // each ray of this source stands for. There is no aiming and no
        // roulette here, which is what makes this the one place in the codebase
        // where that is true.
        const double rays  = double(in.rays);
        const double scale = src.power / rays;

        for (std::size_t i = 0; i < cells; ++i) grid[i] += sGrid[i] * scale;
        if (!gridSq.empty())
            for (std::size_t i = 0; i < cells; ++i) gridSq[i] += sGridSq[i] * scale * scale;
        for (std::size_t i = 0; i < bandGrid.size(); ++i) bandGrid[i] += sBand[i] * scale;
        for (std::size_t i = 0; i < angles.size(); ++i) angles[i] += sAngles[i] * scale;
        if (!anglesSq.empty())
            for (std::size_t i = 0; i < anglesSq.size(); ++i)
                anglesSq[i] += sAnglesSq[i] * scale * scale;

        for (int k = 0; k < 6; ++k) totals[k] += res.totals[k] * scale;
        for (std::size_t k = 0; k < repDet.size(); ++k) {
            repDet[k]  += repDetOne[k]  * scale;
            repEmit[k] += repEmitOne[k] * scale;
        }
        residualBsdf += res.residualBsdf * scale;
        residualNee  += res.residualNee * scale;
        residualSkip += res.residualSkipDet * scale;
        residualAim  += res.residualAiming * scale;
        residualRoul += res.residualRoulette * scale;
        for (int k = 0; k < 3; ++k) anomalies[k] += res.anomalies[k];
        for (int k = 0; k < 4; ++k) arrivedStokes[k] += res.detStokes[k] * scale;
        hits        += std::size_t(res.hits);
        raysEmitted += std::size_t(in.rays);
        seconds     += double(res.milliseconds) / 1000.0;

        // The variance of the detected flux, source by source and added: the
        // sources are independent draws, so their variances add where their
        // error bars would not.
        const double m1 = res.moments[0], m2 = res.moments[1];
        if (rays > 1.0)
            fluxVar += scale * scale * std::max(0.0, m2 - m1 * m1 / rays);

        SourceSummary sum;
        sum.label   = src.label;
        sum.power   = src.power;
        sum.flux    = res.totals[0] * scale;
        sum.rays    = std::size_t(in.rays);
        sum.rayFile = false;
        out.sources.push_back(sum);

        if (si + 1 < srcs.size()) {
            std::fill(sGrid.begin(), sGrid.end(), 0.0);
            std::fill(sGridSq.begin(), sGridSq.end(), 0.0);
            std::fill(sAngles.begin(), sAngles.end(), 0.0);
            std::fill(sAnglesSq.begin(), sAnglesSq.end(), 0.0);
        }
    }

    out.nx = det.nx; out.ny = det.ny;
    out.detW = det.w; out.detH = det.h;
    out.detCX = det.center.dot(det.u);
    out.detCY = det.center.dot(det.v);
    out.detZ  = det.center.z;
    out.detU = det.u; out.detV = det.v; out.detN = det.n;
    out.unit = unit;
    out.backend = QStringLiteral("gpu");
    out.sourcePower = runPower;
    out.raysEmitted = raysEmitted;
    out.raysHitDetector = hits;

    // Split the flat all-receivers grid back out per receiver, through the
    // reference's own function: two unpackings of one layout are two
    // chances to disagree about which bin is which.
    fillDetectorFrames(scene, out);
    scatterDetectorGrids(scene, grid, out);
    out.spectral         = spectral;
    out.luminousEfficacy = spec.efficacy();
    out.meanWavelengthNm = spec.meanWavelength();
    if (!bandGrid.empty()) {
        const std::size_t n = out.irradiance.size();
        out.bandIrradiance.assign(3 * n, 0.0);
        for (int b = 0; b < 3; ++b)
            for (std::size_t i = 0; i < n && i < cells; ++i)
                out.bandIrradiance[std::size_t(b) * n + i] =
                    bandGrid[std::size_t(b) * cells + i];
    }
    if (!gridSq.empty()) {
        // The noise map describes the first receiver, which is the one the
        // comparison and the heatmap read.
        out.irradianceVar.assign(out.irradiance.size(), 0.0);
        for (std::size_t i = 0; i < out.irradiance.size() && i < gridSq.size(); ++i)
            out.irradianceVar[i] = gridSq[i];
    }

    out.fluxDetector  = totals[0];
    out.fluxAbsorbed  = totals[1];
    out.fluxEscaped   = totals[2];
    out.fluxTruncated = totals[3];
    out.fluxRejected  = totals[4];
    // Part of fluxAbsorbed, reported apart from it: a budget that says a tenth
    // of the light was absorbed without saying how much of that was the glass
    // rather than the mirrors sends the reader to the wrong surface.
    out.fluxBulkAbsorbed = totals[5];
    // What a BSDF sampling weight created or destroyed. Its expectation is
    // zero -- it is the estimator's noise made visible, not a loss channel --
    // and the five physical buckets only close to 1e-9 with it counted.
    // Each estimator channel named apart rather than summed: a channel that
    // is systematically wrong must not be able to hide behind one that
    // happens to cancel it. Their total is the residual the budget closes on.
    out.residual.bsdfWeight    = residualBsdf;
    out.residual.nextEvent     = residualNee;
    out.residual.neeSuppressed = residualSkip;
    out.residual.aiming        = residualAim;
    out.residual.roulette      = residualRoul;
    out.fluxRoulette           = out.residual.total();
    // The recoveries the kernel made, counted the way the reference counts its
    // own. A preview reporting an efficiency without them cannot say whether
    // the index pairs behind it were the geometry's or a guess.
    if (opt.physics.polarised) {
        // The same three numbers the reference reports, from the same sums,
        // so a polarised preview can be compared against it rather than
        // merely looked at.
        const double i = arrivedStokes[0];
        const double q = arrivedStokes[1];
        const double u = arrivedStokes[2];
        const double v = arrivedStokes[3];
        out.polarised = true;
        out.degreeOfPolarisation =
            i > 0.0 ? std::sqrt(q * q + u * u + v * v) / i : 0.0;
        out.degreeLinear = i > 0.0 ? std::sqrt(q * q + u * u) / i : 0.0;
        out.polarisationAngleDeg =
            0.5 * std::atan2(u, q) * 180.0 / 3.14159265358979323846;
    }
    out.anomalies.unmatchedExit = std::size_t(anomalies[0]);
    out.anomalies.stackOverflow = std::size_t(anomalies[1]);
    out.anomalies.guessedIndex  = std::size_t(anomalies[2]);
    out.efficiency = runPower > 0.0 ? out.fluxDetector / runPower : 0.0;
    // The error on the detected fraction, from the spread of the per-ray
    // detected weight, summed over the sources because they are independent.
    out.efficiencyStdErr = runPower > 0.0 ? std::sqrt(fluxVar) / runPower : 0.0;
    // With a low-discrepancy sequence the samples are deliberately not
    // independent, so the ray-to-ray spread above overstates the error --
    // it reports the error a plain Monte Carlo run of the same size would
    // have had, which is the whole gain thrown away in the reporting. The
    // spread across independent scrambles is the honest measure, and it is
    // what the comparison against the reference is sized in.
    if (in.replicas > 1) {
        int    used = 0;
        double mean = 0.0;
        std::vector<double> frac(repDet.size(), 0.0);
        for (std::size_t k = 0; k < repDet.size(); ++k) {
            if (repEmit[k] <= 0.0) continue;
            frac[k] = repDet[k] / repEmit[k];
            mean += frac[k];
            ++used;
        }
        if (used > 1) {
            mean /= double(used);
            double m2 = 0.0;
            for (std::size_t k = 0; k < repDet.size(); ++k) {
                if (repEmit[k] <= 0.0) continue;
                const double d = frac[k] - mean;
                m2 += d * d;
            }
            out.efficiencyStdErr = std::sqrt(m2 / (double(used) - 1.0) / double(used));
        }
    }

    if (!angles.empty()) {
        if (!anglesSq.empty()) {
            out.intensityMasterTheta = in.nThetaMaster;
            out.intensityMasterPhi   = in.nPhi;
            out.intensityMaster      = angles;          // before it is folded
            out.intensityMasterVar   = std::move(anglesSq);
        }
        finishIntensityGrid(out.intensity, std::move(angles), nTheta, nPhi);
    }

    out.traceSeconds = seconds;
    out.buildSeconds = 0.0;
    // Ray paths and receiver arrivals are left empty on purpose: the preview
    // does not record them, and filling them with plausible zeros would make a
    // partial result read as a whole one. The one measurement that costs is
    // chromaticity, which the reference derives from the arrivals own
    // wavelengths -- a spectral preview run reports its bands and its efficacy
    // but not an (x, y), and says so by leaving them at zero rather than by
    // reporting the mean wavelength as a colour.
    return true;
}

} // namespace gputrace
