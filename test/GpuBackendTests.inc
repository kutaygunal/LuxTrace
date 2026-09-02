// The GPU preview backend.
//
// Two kinds of test here. The first kind runs everywhere, including on a build
// with no CUDA and on CI with no card: what the backend says about itself, and
// that it refuses by name what it cannot model. Those are the properties that
// keep a preview honest, and they are exactly the ones that would rot if they
// were only ever checked on a developer's machine with a GPU in it.
//
// The second kind needs a device and skips without one. It is the same
// comparison a person runs with --refcheck, asserted.

namespace {

// The reference and the preview for one scene, traced through the same
// geometry, the same sources and the same physics.
struct BackendPair {
    bool             ran = false;
    QString          refusal;
    SimulationResult cpu, gpu;
};

BackendPair tracedBothWays(GeometryProvider::Scene scene, int rays,
                           SourceConfig::Shape shape = SourceConfig::Shape::PointLike,
                           double sizeA = 0.0, double sizeB = 0.0) {
    BackendPair out;
    SimConfig cfg = quickConfig(scene, rays);
    cfg.shape    = shape;
    cfg.sizeA    = sizeA;
    cfg.sizeB    = sizeB;
    cfg.seed     = 11111;
    cfg.noiseMap = true;                 // the comparison needs measured floors
    out.cpu = Simulation::run(cfg);

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    TraceOptions opt;
    opt.seed     = 99999;                // a different draw, as a preview is
    opt.physics  = cfg.physics;
    opt.nTheta   = cfg.nTheta;
    opt.nPhi     = cfg.nPhi;
    opt.noiseMap = true;
    out.ran = gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                              cfg.power, cfg.fluxUnit, out.gpu, &out.refusal);
    return out;
}

} // namespace

TEST(gpu, the_backend_agrees_with_itself_about_whether_it_exists) {
    // Runs with or without a card. A backend that reports a device and no name,
    // or no device and no reason, is one nobody can act on.
    if (gputrace::available()) {
        CHECK(!gputrace::deviceName().isEmpty());
        CHECK(gputrace::unavailableReason().isEmpty());
    } else {
        CHECK(gputrace::deviceName().isEmpty());
        CHECK(!gputrace::unavailableReason().isEmpty());
    }
}

TEST(gpu, what_it_cannot_model_is_refused_by_name) {
    // The property the whole arrangement rests on: a preview must never quietly
    // skip a term. Every refusal carries a reason, and asking it to trace one
    // anyway fails instead of returning a plausible number.
    //
    // Each case here builds the thing that is refused rather than naming a scene
    // that happens to contain one. Three scenes used to stand in for this list --
    // the integrating sphere, the diffuser plate and the microlens array -- and
    // all three are traced now that the kernel models scattering and walks the
    // two-level hierarchy. A refusal test keyed to scenes says nothing once the
    // scenes move; keyed to the terms, it says what it means.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 2000);
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);

    struct Case {
        const char* what;
        void (*apply)(TraceOptions&, std::vector<SourceConfig>&,
                      std::vector<SceneSurface>&);
    };
    const Case cases[] = {
        {"stray-light path recording",
         [](TraceOptions& o, std::vector<SourceConfig>&, std::vector<SceneSurface>&) {
             o.strayPaths.enabled = true;
         }},
        {"a multi-layer coating",
         [](TraceOptions& o, std::vector<SourceConfig>&, std::vector<SceneSurface>& sf) {
             // A characteristic-matrix stack: complex arithmetic per hit, and
             // approximating it on the device would be inventing a coating.
             o.physics.coatings = true;
             sf.front().coating.model  = coating::Model::Stack;
             sf.front().coating.layers = 2;
         }},
        {"a collimated beam with a radius",
         [](TraceOptions&, std::vector<SourceConfig>& s, std::vector<SceneSurface>&) {
             s.front().type       = SourceConfig::Type::Collimated;
             s.front().beamRadius = 10.0;
         }},
        {"sources with different spectra",
         [](TraceOptions&, std::vector<SourceConfig>& s, std::vector<SceneSurface>&) {
             // A spectral run crosses now, but the sampled dispersion curves
             // and the inverse CDF are one set per run: two emitters with
             // different spectra would need two, and inventing one from the
             // other is exactly what this backend refuses to do.
             s.front().spectrum.kind = SpectrumConfig::Kind::Blackbody;
             SourceConfig second = s.front();
             second.spectrum.kind = SpectrumConfig::Kind::D65;
             s.push_back(second);
         }},
        {"a surface edit that matches no surface",
         [](TraceOptions& o, std::vector<SourceConfig>&, std::vector<SceneSurface>&) {
             SurfaceOverride ov;
             ov.label   = QStringLiteral("a surface this scene does not have");
             ov.surface = -1;
             o.surfaceOverrides.push_back(ov);
         }},
    };

    for (const Case& c : cases) {
        TraceOptions opt;
        opt.physics = cfg.physics;
        std::vector<SourceConfig> s  = srcs;
        std::vector<SceneSurface> sf = data->scene.surfaces();
        c.apply(opt, s, sf);

        const gputrace::Support sup = gputrace::supports(data->scene, sf, s, opt);
        CHECK(!sup.ok);
        CHECK(sup.reasons.contains(QLatin1String(c.what)));
        for (const QString& r : sup.reasons) CHECK(!r.isEmpty());

        SimulationResult res;
        QString err;
        CHECK(!gputrace::trace(data->scene, sf, s, opt, cfg.power, cfg.fluxUnit,
                               res, &err));
        CHECK(err.contains(QLatin1String(c.what)));
        // Nothing was written into a result that was never produced.
        CHECK(res.irradiance.empty());
    }
}

TEST(gpu, the_scenes_it_used_to_refuse_are_traced_and_checked) {
    if (!gputrace::available()) return;
    // The other half of the refusal contract, and the more easily lost one: a
    // scene that moves from refused to traced without being checked is worse
    // than one that is still refused. These five were the whole of what the gate
    // turned away -- three for scattering, two for instanced geometry -- and
    // each is now traced and indistinguishable from the reference.
    for (int sc : {int(GeometryProvider::Scene::IntegratingSphere),
                   int(GeometryProvider::Scene::DiffuserPlate),
                   int(GeometryProvider::Scene::ShowcaseLuminaire),
                   int(GeometryProvider::Scene::MicrolensArray),
                   int(GeometryProvider::Scene::LedArrayLuminaire)}) {
        const BackendPair p = tracedBothWays(GeometryProvider::Scene(sc), 200000);
        CHECK(p.ran);
        if (!p.ran) continue;
        const backendcheck::Report r = backendcheck::compare(p.cpu, p.gpu);
        CHECK(r.blocker.isEmpty());
        CHECK(r.ok);
    }
}

TEST(gpu, a_closed_cavity_leaks_almost_nothing) {
    if (!gputrace::available()) return;
    // An integrating sphere is a closed diffuse shell: the reference escapes
    // exactly nothing from it, so any leak at all is a defect rather than a
    // disagreement inside sampling. It is the sharpest geometric oracle in the
    // library, and it is what sized two of the kernel constants -- the offset a
    // branch is spawned with, and how far along the new ray the search starts.
    //
    // A ray sliding along the inside of the wall re-enters through the
    // neighbouring facet, arrives on its outward side and is scattered about the
    // outward normal, which walks it out of the cavity. Three things took that
    // from three parts in ten thousand to a couple in a million: the mesh is
    // uploaded watertight, the triangle test is the watertight one, and the
    // search starts past the wedge where the neighbouring half-space intrudes.
    //
    // The bound is not zero and the number is stated rather than rounded away.
    // What is left is the floor of single-precision geometry: a stratified
    // emission stream finds the near-tangential configuration reliably where
    // independent uniforms found it about never, so the leak is visible with the
    // low-discrepancy sampler on and was not without it. Widening the search
    // further does not remove it and does start stepping over real hits.
    const BackendPair p =
        tracedBothWays(GeometryProvider::Scene::IntegratingSphere, 500000);
    CHECK(p.ran);
    if (!p.ran) return;
    CHECK(p.cpu.fluxEscaped == 0.0);       // the reference, as the oracle
    CHECK(p.gpu.fluxEscaped >= 0.0);
    CHECK(p.gpu.fluxEscaped < 1e-5 * p.gpu.sourcePower);
}

TEST(gpu, a_part_placed_many_times_is_traced_where_it_was_placed) {
    if (!gputrace::available()) return;
    // The two-level hierarchy. A kernel that walked only the single-level path
    // saw an empty scene for the microlens array and the LED luminaire, which is
    // why both were refused by name rather than traced as nothing.
    //
    // Placement is the part that a sigma test alone would not catch: a bug that
    // put all twenty-five lenslets at the origin still traces, still closes its
    // energy, and still reports an efficiency. The receiver map is what says
    // where the light landed, so it is compared as well as the scalars.
    for (int sc : {int(GeometryProvider::Scene::MicrolensArray),
                   int(GeometryProvider::Scene::LedArrayLuminaire)}) {
        const BackendPair p = tracedBothWays(GeometryProvider::Scene(sc), 300000);
        CHECK(p.ran);
        if (!p.ran) continue;
        const Simulation::SceneRef data =
            Simulation::dataFor(quickConfig(GeometryProvider::Scene(sc), 1000));
        CHECK(!data->scene.instances().empty());   // it really is instanced

        const backendcheck::Report r = backendcheck::compare(p.cpu, p.gpu);
        CHECK(r.blocker.isEmpty());
        CHECK(r.ok);
        bool sawMap = false;
        for (const backendcheck::MapComparison& m : r.maps)
            if (m.name == QStringLiteral("irradiance") && m.comparable) {
                sawMap = true;
                CHECK(m.ok);
            }
        CHECK(sawMap);
    }
}

TEST(gpu, every_source_carries_its_own_share_of_a_multi_source_run) {
    if (!gputrace::available()) return;
    // Two emitters at the same optic, one carrying three times the power of the
    // other. The reference partitions one global ray index space across the
    // sources and emits each ray already carrying power_s / rays_s; the preview
    // launches once per source and scales the same way. What must not happen is
    // the run reporting only the first emitter as the source power, which would
    // make a two-LED fixture look twice as efficient as it is.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 200000);
    cfg.seed     = 11111;
    cfg.noiseMap = true;
    cfg.power    = 1.0;
    SourceSpec extra;
    extra.label  = QStringLiteral("Second LED");
    extra.power  = 3.0;
    extra.offset = gp_Pnt(4.0, 0.0, 0.0);      // beside the first, same optic
    cfg.extraSources.push_back(extra);

    const SimulationResult ref = Simulation::run(cfg);
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    CHECK(srcs.size() == 2);

    TraceOptions opt;
    opt.seed     = 99999;
    opt.physics  = cfg.physics;
    opt.nTheta   = cfg.nTheta;
    opt.nPhi     = cfg.nPhi;
    opt.noiseMap = true;

    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                          cfg.power, cfg.fluxUnit, prev, &err));
    CHECK(prev.sources.size() == 2);
    // Four watts went in, not the one the caller named.
    CHECK_NEAR(prev.sourcePower, 4.0, 1e-9);
    CHECK_NEAR(prev.sourcePower, ref.sourcePower, 1e-9 * ref.sourcePower);

    double summed = 0.0;
    for (std::size_t i = 0; i < prev.sources.size(); ++i) {
        CHECK(prev.sources[i].rays > 0);
        CHECK_NEAR(prev.sources[i].power, ref.sources[i].power, 1e-9);
        summed += prev.sources[i].power;
    }
    CHECK_NEAR(summed, prev.sourcePower, 1e-9);
    // The budget still closes with two emitters in it.
    const double total = prev.fluxDetector + prev.fluxAbsorbed + prev.fluxEscaped
                       + prev.fluxTruncated + prev.fluxRejected + prev.fluxRoulette;
    CHECK_NEAR(total, prev.sourcePower, 1e-9 * prev.sourcePower);

    const backendcheck::Report r = backendcheck::compare(ref, prev);
    CHECK(r.blocker.isEmpty());
    CHECK(r.ok);
}

TEST(gpu, a_supported_scene_is_indistinguishable_from_the_reference) {
    if (!gputrace::available()) return;    // no card: nothing to compare
    for (int sc : {int(GeometryProvider::Scene::Reflector),
                   int(GeometryProvider::Scene::EllipticalReflector),
                   int(GeometryProvider::Scene::HalfBallLens),
                   int(GeometryProvider::Scene::LightGuide)}) {
        const BackendPair p = tracedBothWays(GeometryProvider::Scene(sc), 200000);
        CHECK(p.ran);
        if (!p.ran) continue;
        CHECK(p.gpu.backend == QStringLiteral("gpu"));
        const backendcheck::Report r = backendcheck::compare(p.cpu, p.gpu);
        CHECK(r.blocker.isEmpty());
        CHECK(r.ok);
    }
}

TEST(gpu, an_emitter_with_area_is_traced_as_one) {
    if (!gputrace::available()) return;
    // A point source and a 10 mm square die through the same optic must not
    // give the same answer -- if they do, the area is being ignored -- and the
    // square one must still match the reference.
    const BackendPair pt =
        tracedBothWays(GeometryProvider::Scene::HalfBallLens, 200000);
    const BackendPair area =
        tracedBothWays(GeometryProvider::Scene::HalfBallLens, 200000,
                       SourceConfig::Shape::Rect, 10.0, 10.0);
    CHECK(pt.ran);
    CHECK(area.ran);
    if (!pt.ran || !area.ran) return;

    CHECK(std::fabs(area.gpu.efficiency - pt.gpu.efficiency) > 1e-6);
    const backendcheck::Report r = backendcheck::compare(area.cpu, area.gpu);
    CHECK(r.blocker.isEmpty());
    CHECK(r.ok);
}

TEST(gpu, the_far_field_is_produced_and_comparable) {
    if (!gputrace::available()) return;
    // Half of what this application reports is the far field. A preview that
    // returns a receiver map and nothing else is half a result, and the
    // comparison would have nothing to say about the beam.
    const BackendPair p = tracedBothWays(GeometryProvider::Scene::Reflector, 200000);
    CHECK(p.ran);
    if (!p.ran) return;
    CHECK(!p.gpu.intensity.bin.empty());
    CHECK(!p.gpu.intensityMaster.empty());
    CHECK(p.gpu.intensityMasterTheta == p.cpu.intensityMasterTheta);
    CHECK(p.gpu.intensityMasterPhi   == p.cpu.intensityMasterPhi);

    const backendcheck::Report r = backendcheck::compare(p.cpu, p.gpu);
    bool sawFar = false;
    for (const backendcheck::MapComparison& m : r.maps)
        if (m.name == QStringLiteral("far field") && m.comparable) { sawFar = true; CHECK(m.ok); }
    CHECK(sawFar);
}

TEST(gpu, the_preview_accounts_for_its_energy) {
    if (!gputrace::available()) return;
    // The same closure the reference reports. A backend whose buckets do not
    // add up is not a fast answer, it is a fast guess.
    for (int sc : {int(GeometryProvider::Scene::Reflector),
                   int(GeometryProvider::Scene::HalfBallLens),
                   int(GeometryProvider::Scene::LightGuide)}) {
        const BackendPair p = tracedBothWays(GeometryProvider::Scene(sc), 100000);
        CHECK(p.ran);
        if (!p.ran) continue;
        const double total = p.gpu.fluxDetector + p.gpu.fluxAbsorbed + p.gpu.fluxEscaped
                           + p.gpu.fluxTruncated + p.gpu.fluxRejected;
        CHECK_NEAR(total, p.gpu.sourcePower, 1e-9 * p.gpu.sourcePower);
    }
}

TEST(gpu, the_preview_reproduces_itself) {
    if (!gputrace::available()) return;
    // Seeded from the ray index, so the launch geometry cannot change the
    // answer. This is the one determinism promise a GPU backend can make, and
    // it is worth strictly less than the reference's -- which is why the
    // reference is still the reference.
    const BackendPair a = tracedBothWays(GeometryProvider::Scene::HalfBallLens, 100000);
    const BackendPair b = tracedBothWays(GeometryProvider::Scene::HalfBallLens, 100000);
    CHECK(a.ran);
    if (!a.ran) return;
    CHECK(a.gpu.efficiency == b.gpu.efficiency);
    CHECK(a.gpu.raysHitDetector == b.gpu.raysHitDetector);
}

TEST(gpu, a_solid_the_light_only_refracts_through_loses_no_ray_to_its_own_surface) {
    if (!gputrace::available()) return;
    // The sharpest oracle this backend has, and the one that caught a bias the
    // sigma test could only see the shadow of.
    //
    // Aim a cone of light at a plano-convex lens narrow enough that every ray
    // enters the flat face and leaves through the cap, and take the interface
    // split away entirely -- nothing reflects, nothing is absorbed, no ray can
    // miss the receiver. The answer is then not "within sampling" but exactly
    // one, and the reference delivers exactly one.
    //
    // The preview used to deliver 0.9912. The missing ray in each hundred and
    // thirteen was landing back on the face it had just crossed: the hit point
    // came from o + d*t, whose float error over a 250 mm leg is the same size
    // as the epsilon the next leg is spawned with, so the branch started on the
    // wrong side of the surface and refracted through it a second time. It is a
    // bias rather than noise -- it fires only on oblique hits at curved
    // refracting faces -- and it is what made the lens and the axicon the two
    // scenes --refcheck reported as DIFFERS.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::PlanoConvexLens, 200000);
    cfg.halfAngleDeg = 20.0;             // inside the rim, so nothing can miss
    cfg.seed         = 11111;
    cfg.physics.fresnel    = false;
    cfg.physics.absorption = false;
    cfg.physics.coatings   = false;

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    int lens = -1;
    for (std::size_t i = 0; i < data->surfaces.size(); ++i)
        if (data->surfaces[i].index > 1.0) { lens = int(i); break; }
    CHECK(lens >= 0);
    if (lens < 0) return;

    SurfaceOverride ov;
    ov.label    = data->surfaces[std::size_t(lens)].label;
    ov.identity = data->scene.surfaceIdentity(lens);
    ov.surface  = lens;
    ov.optics   = data->surfaces[std::size_t(lens)];
    ov.optics.reflectivity   = 0.0;      // pure refraction: one path, no choice
    ov.optics.transmissivity = 1.0;
    ov.optics.absorption     = 0.0;
    cfg.surfaceOverrides.push_back(ov);

    const SimulationResult ref = Simulation::run(cfg);
    CHECK_NEAR(ref.efficiency, 1.0, 1e-12);

    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    TraceOptions opt;
    opt.seed    = 99999;
    opt.physics = cfg.physics;
    opt.nTheta  = cfg.nTheta;
    opt.nPhi    = cfg.nPhi;
    opt.surfaceOverrides = cfg.surfaceOverrides;

    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                          cfg.power, cfg.fluxUnit, prev, &err));
    // Not "close to one": every ray, because there is nowhere else for one to
    // go. A tolerance here would let the bias back in one ray at a time.
    CHECK_NEAR(prev.efficiency, 1.0, 1e-12);
    CHECK(prev.raysHitDetector == prev.raysEmitted);
}

TEST(gpu, a_coating_is_applied_to_s_and_p_and_not_to_their_average) {
    if (!gputrace::available()) return;
    // Coating::reflectanceSP clamps each polarisation to the coating's residual
    // and averages afterwards. Averaging first and clamping once is a different
    // number wherever the two polarisations differ, which is everywhere except
    // normal incidence -- and near Brewster's angle it is nearly double, because
    // the p reflectance is already far below anything a coating leaves while the
    // s reflectance is well above it.
    //
    // The axicon is where that shows: its cone is met at thirty-four degrees on
    // the way out, and the preview used to turn back about a third of a percent
    // more light than the reference does. Traced with coatings off the two
    // agreed all along, which is what named the term.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Axicon, 1000000);
    cfg.seed     = 11111;
    cfg.noiseMap = true;
    const SimulationResult ref = Simulation::run(cfg);

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    TraceOptions opt;
    opt.seed     = 99999;
    opt.physics  = cfg.physics;
    opt.nTheta   = cfg.nTheta;
    opt.nPhi     = cfg.nPhi;
    opt.noiseMap = true;

    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                          cfg.power, cfg.fluxUnit, prev, &err));
    CHECK(cfg.physics.coatings);         // the term under test is switched on
    const backendcheck::Report r = backendcheck::compare(ref, prev);
    CHECK(r.blocker.isEmpty());
    CHECK(r.ok);
}

TEST(gpu, the_preview_says_how_much_of_its_loss_was_the_glass) {
    if (!gputrace::available()) return;
    // The preview computes Beer-Lambert separately from surface absorption and
    // used to report only their sum, so a budget could say a tenth of the light
    // was absorbed without saying whether to look at the glass or the mirrors.
    // The reference splits them and so must this, or the two budgets are not
    // the same budget.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Axicon, 200000);
    cfg.seed = 11111;
    const SimulationResult ref = Simulation::run(cfg);
    CHECK(ref.fluxBulkAbsorbed > 0.0);   // the scene does have absorbing glass

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    TraceOptions opt;
    opt.seed    = 99999;
    opt.physics = cfg.physics;
    opt.nTheta  = cfg.nTheta;
    opt.nPhi    = cfg.nPhi;

    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                          cfg.power, cfg.fluxUnit, prev, &err));
    CHECK(prev.fluxBulkAbsorbed > 0.0);
    // A subset of the absorbed channel, never a sixth bucket beside it -- the
    // budget still has to close on the five.
    CHECK(prev.fluxBulkAbsorbed <= prev.fluxAbsorbed + 1e-12);
    CHECK_NEAR(prev.fluxBulkAbsorbed, ref.fluxBulkAbsorbed,
               0.05 * ref.fluxBulkAbsorbed);
    const double total = prev.fluxDetector + prev.fluxAbsorbed + prev.fluxEscaped
                       + prev.fluxTruncated + prev.fluxRejected;
    CHECK_NEAR(total, prev.sourcePower, 1e-9 * prev.sourcePower);
}

namespace {

// One preview run, with the emission stream switched between the two samplers.
SimulationResult previewWith(GeometryProvider::Scene scene, int rays,
                             bool lowDiscrepancy, std::uint64_t seed, bool* ok) {
    SimConfig cfg = quickConfig(scene, rays);
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    TraceOptions opt;
    opt.seed    = seed;
    opt.physics = cfg.physics;
    opt.nTheta  = cfg.nTheta;
    opt.nPhi    = cfg.nPhi;
    opt.estimator.lowDiscrepancy = lowDiscrepancy;
    SimulationResult res;
    QString err;
    *ok = gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                          cfg.power, cfg.fluxUnit, res, &err);
    return res;
}

// Spread of the efficiency over independent seeds -- the error the estimator
// actually achieved, as opposed to the one it reports.
double efficiencySpread(GeometryProvider::Scene scene, int rays,
                        bool lowDiscrepancy, int draws, bool* ok) {
    double mean = 0.0;
    std::vector<double> e;
    for (int i = 0; i < draws; ++i) {
        bool ran = false;
        const SimulationResult r =
            previewWith(scene, rays, lowDiscrepancy, 1000u + std::uint64_t(i) * 7919u, &ran);
        if (!ran) { *ok = false; return 0.0; }
        e.push_back(r.efficiency);
        mean += r.efficiency;
    }
    mean /= double(e.size());
    double m2 = 0.0;
    for (double v : e) m2 += (v - mean) * (v - mean);
    *ok = true;
    return std::sqrt(m2 / double(e.size() - 1));
}

} // namespace

TEST(gpu, the_low_discrepancy_emission_stream_is_the_one_it_says_it_is) {
    if (!gputrace::available()) return;
    // Two claims, and the second is the one that is easy to get wrong.
    //
    // First: an Owen-scrambled Sobol emission stream really does land its
    // samples more evenly, so the spread over independent seeds is smaller than
    // the same number of independent uniforms would give.
    //
    // Second: the *reported* error bar has to follow it down. A Sobol sequence
    // is not a set of independent samples, so the ray-to-ray spread reports the
    // error a plain Monte Carlo run of the same size would have had -- the whole
    // gain thrown away in the reporting, and worse than that here, because the
    // comparison against the reference is sized in those error bars. An
    // overstated bar makes every disagreement look acceptable.
    constexpr int kRays  = 100000;
    constexpr int kDraws = 12;
    bool ok = false;

    const double spreadQmc =
        efficiencySpread(GeometryProvider::Scene::Reflector, kRays, true, kDraws, &ok);
    CHECK(ok);
    if (!ok) return;
    const double spreadPlain =
        efficiencySpread(GeometryProvider::Scene::Reflector, kRays, false, kDraws, &ok);
    CHECK(ok);
    if (!ok) return;

    CHECK(spreadQmc > 0.0);
    CHECK(spreadPlain > 0.0);
    CHECK(spreadQmc < spreadPlain);

    // And the bar it reports is that spread, not the plain one. Twelve draws
    // measure a standard deviation to about twenty per cent, so the window is
    // wide -- what it has to catch is a bar that is out by the factor the two
    // samplers differ by, which is what reporting the wrong one would give.
    bool ran = false;
    const SimulationResult r =
        previewWith(GeometryProvider::Scene::Reflector, kRays, true, 4242u, &ran);
    CHECK(ran);
    if (!ran) return;
    CHECK(r.efficiencyStdErr > 0.4 * spreadQmc);
    CHECK(r.efficiencyStdErr < 2.5 * spreadQmc);
}

TEST(gpu, the_emission_stream_does_not_move_the_answer) {
    if (!gputrace::available()) return;
    // A variance reduction that changes the answer is not a variance reduction.
    // Both samplers have to land on the reference, and on each other, inside
    // their own error bars -- which is exactly the comparison the backend check
    // was built to make, pointed at two draws of the same backend.
    for (int sc : {int(GeometryProvider::Scene::Reflector),
                   int(GeometryProvider::Scene::PlanoConvexLens),
                   int(GeometryProvider::Scene::IntegratingSphere)}) {
        bool a = false, b = false;
        const SimulationResult qmc =
            previewWith(GeometryProvider::Scene(sc), 200000, true, 99999u, &a);
        const SimulationResult plain =
            previewWith(GeometryProvider::Scene(sc), 200000, false, 99999u, &b);
        CHECK(a);
        CHECK(b);
        if (!a || !b) continue;
        const double err = std::sqrt(qmc.efficiencyStdErr * qmc.efficiencyStdErr +
                                     plain.efficiencyStdErr * plain.efficiencyStdErr);
        CHECK(err > 0.0);
        CHECK(std::fabs(qmc.efficiency - plain.efficiency) < 4.0 * err);
        // The budget closes whichever stream emitted the rays.
        const double total = qmc.fluxDetector + qmc.fluxAbsorbed + qmc.fluxEscaped
                           + qmc.fluxTruncated + qmc.fluxRejected + qmc.fluxRoulette;
        CHECK_NEAR(total, qmc.sourcePower, 1e-9 * qmc.sourcePower);
    }
}

TEST(gpu, connecting_every_diffuse_bounce_to_the_receiver_tightens_the_answer) {
    if (!gputrace::available()) return;
    // Next-event estimation, the largest of the three variance reductions the
    // preview was missing and the one the integrating sphere exists to reward: a
    // random walk inside a diffuse cavity spends nearly every ray missing the
    // port, and connecting to it analytically removes the search entirely.
    //
    // Two claims. The answer does not move -- both estimators land on the
    // reference -- and the receiver map gets sharper, which is the whole point.
    const auto shoot = [&](bool nee) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene::IntegratingSphere, 200000);
        cfg.seed     = 11111;
        cfg.noiseMap = true;
        const Simulation::SceneRef data = Simulation::dataFor(cfg);
        const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
        TraceOptions opt;
        opt.seed     = 99999;
        opt.physics  = cfg.physics;
        opt.nTheta   = cfg.nTheta;
        opt.nPhi     = cfg.nPhi;
        opt.noiseMap = true;
        opt.estimator.nextEventEstimation = nee;
        SimulationResult res;
        QString err;
        const bool ok = gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                                        cfg.power, cfg.fluxUnit, res, &err);
        CHECK(ok);
        return res;
    };
    const SimulationResult with = shoot(true);
    const SimulationResult without = shoot(false);
    if (with.irradiance.empty() || without.irradiance.empty()) return;

    // The budget closes with the two new residual channels in it: the analytic
    // term entered negative, and the sampled arrival it suppressed.
    CHECK_NEAR(with.fluxAccounted(), with.sourcePower, 1e-9 * with.sourcePower);
    CHECK(with.residual.nextEvent < 0.0);        // delivered without a ray
    CHECK(with.residual.neeSuppressed > 0.0);    // and the arrival it replaced

    // Both land on the reference.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::IntegratingSphere, 200000);
    cfg.seed     = 11111;
    cfg.noiseMap = true;
    const SimulationResult ref = Simulation::run(cfg);
    for (const SimulationResult* r : {&with, &without}) {
        const backendcheck::Report rep = backendcheck::compare(ref, *r);
        CHECK(rep.blocker.isEmpty());
        CHECK(rep.ok);
    }

    // And the map is sharper. Bin-to-bin roughness is what a noisy receiver map
    // has and a converged one does not, so it is what the connection buys.
    const auto roughness = [](const SimulationResult& r) {
        double sum = 0.0, total = 0.0;
        for (int y = 0; y < r.ny; ++y)
            for (int x = 1; x < r.nx; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(r.nx) + std::size_t(x);
                sum   += std::fabs(r.irradiance[i] - r.irradiance[i - 1]);
                total += r.irradiance[i];
            }
        return total > 0.0 ? sum / total : 0.0;
    };
    CHECK(roughness(with) < 0.8 * roughness(without));
}

TEST(gpu, more_than_one_receiver_is_traced_and_each_gets_its_own_grid) {
    if (!gputrace::available()) return;
    // The gate used to refuse a scene with anything but exactly one receiver.
    // One grid holds them all end to end now, the way the reference lays them
    // out, and both backends unpack it through the same function -- two
    // unpackings of one layout are two chances to disagree about which bin is
    // which.
    //
    // A small receiver in front of a large one, so the near one takes the core
    // of the beam and the far one takes what got past it. Distinct labels,
    // because a surface is keyed by what it is: two receivers sharing a label
    // are one surface with two frames, and only one of them could be reached.
    MeshSurface nearRec = detectorSurface(80.0, 200.0, 16);
    nearRec.label = QStringLiteral("Near receiver");
    MeshSurface farRec = detectorSurface(600.0, 600.0, 16);
    farRec.label = QStringLiteral("Far receiver");
    MeshList meshes{nearRec, farRec};
    TraceScene scene;
    scene.build(meshes);
    CHECK(scene.detectors().size() == 2);

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 25.0;
    src.rays         = 300000;
    src.origin       = gp_Pnt(0, 0, -200);
    src.axis         = gp_Dir(0, 0, 1);

    TraceOptions opt;
    opt.seed     = 11111;
    opt.noiseMap = true;
    SimulationResult ref;
    RayTracer::trace(scene, std::vector<SourceConfig>{src}, ref, opt);

    TraceOptions gopt = opt;
    gopt.seed = 99999;
    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(scene, scene.surfaces(), {src}, gopt, 1.0,
                          FluxUnit::Watt, prev, &err));
    CHECK(prev.detectors.size() == 2);
    if (prev.detectors.size() != 2) return;

    double flux0 = 0.0, flux1 = 0.0;
    for (double v : prev.detectors[0].grid) flux0 += v;
    for (double v : prev.detectors[1].grid) flux1 += v;
    CHECK(flux0 > 0.0);
    CHECK(flux1 > 0.0);
    // The far one is seven times wider and catches what the near one missed.
    CHECK(flux1 > flux0);
    // Which is the check that matters: the two slices between them are the
    // whole detected flux, so neither is empty and neither is counted twice.
    CHECK_NEAR(flux0 + flux1, prev.fluxDetector, 1e-9 * prev.fluxDetector);

    const backendcheck::Report r = backendcheck::compare(ref, prev);
    CHECK(r.blocker.isEmpty());
    CHECK(r.ok);
    CHECK_NEAR(prev.fluxAccounted(), prev.sourcePower, 1e-9 * prev.sourcePower);
}

TEST(gpu, a_measured_coating_table_crosses_and_a_stack_still_does_not) {
    if (!gputrace::available()) return;
    // Coating::reflectanceSP reads a measured table only to pick the residual it
    // then rolls off exactly as the ideal model does. The preview is
    // monochromatic by construction, so the table resolves to one number per
    // run -- which is not an approximation, it is the same arithmetic done once
    // on the host instead of per hit on the device.
    //
    // A characteristic-matrix stack is a different thing and is still refused:
    // it is a complex solve per hit, and approximating it here would be
    // inventing a coating.
    MeshSurface glass = glassPlane(1.5, 300.0);
    glass.fresnel = true;
    glass.coating.model   = coating::Model::Table;
    glass.coating.samples = 3;
    glass.coating.lambdaNm[0] = 450.0; glass.coating.reflectance[0] = 0.02;
    glass.coating.lambdaNm[1] = 550.0; glass.coating.reflectance[1] = 0.004;
    glass.coating.lambdaNm[2] = 650.0; glass.coating.reflectance[2] = 0.03;

    MeshList meshes{glass, detectorSurface(1200.0, 400.0, 16)};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 30.0;
    src.rays         = 300000;
    src.origin       = gp_Pnt(0, 0, -200);
    src.axis         = gp_Dir(0, 0, 1);
    src.spectrum     = SpectrumConfig::Kind::Monochromatic;
    src.spectrum.wavelengthNm = 500.0;      // between two table points

    TraceOptions opt;
    opt.seed     = 11111;
    opt.noiseMap = true;
    SimulationResult ref;
    RayTracer::trace(scene, std::vector<SourceConfig>{src}, ref, opt);

    TraceOptions gopt = opt;
    gopt.seed = 99999;
    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(scene, scene.surfaces(), {src}, gopt, 1.0,
                          FluxUnit::Watt, prev, &err));
    const backendcheck::Report r = backendcheck::compare(ref, prev);
    CHECK(r.blocker.isEmpty());
    CHECK(r.ok);

    // The stack, by contrast, is refused by name.
    MeshSurface stacked = glassPlane(1.5, 300.0);
    stacked.coating.model  = coating::Model::Stack;
    stacked.coating.layers = 2;
    MeshList stackMeshes{stacked, detectorSurface(1200.0, 400.0, 8)};
    TraceScene stackScene;
    stackScene.build(stackMeshes);
    const gputrace::Support sup =
        gputrace::supports(stackScene, stackScene.surfaces(), {src}, gopt);
    CHECK(!sup.ok);
    CHECK(sup.reasons.contains(QStringLiteral("a multi-layer coating")));
}

TEST(gpu, aiming_and_roulette_book_what_they_do_not_trace) {
    if (!gputrace::available()) return;
    // The other two variance reductions. Neither fires on most of the library --
    // aiming declines when the source sits inside the scene bounds, which is
    // where the registry puts nearly all of them, and a branch has to lose nine
    // decades before roulette sees it -- so the case is built rather than looked
    // for.
    //
    // A small optic a long way from a wide emitter is exactly what aiming is
    // for: the source radiates into a hemisphere and the geometry fills a
    // fraction of a degree of it.
    MeshList meshes{glassPlane(1.5, 12.0), detectorSurface(200.0, 40.0, 16)};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 90.0;
    src.rays         = 200000;
    src.origin       = gp_Pnt(0, 0, -4000);    // far outside the scene bounds
    src.axis         = gp_Dir(0, 0, 1);

    TraceOptions opt;
    opt.seed     = 11111;
    opt.noiseMap = true;
    opt.estimator.aimAtScene = true;
    SimulationResult ref;
    RayTracer::trace(scene, std::vector<SourceConfig>{src}, ref, opt);
    CHECK(ref.residual.aiming != 0.0);          // it really did fire

    TraceOptions gopt = opt;
    gopt.seed = 99999;
    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(scene, scene.surfaces(), {src}, gopt, 1.0,
                          FluxUnit::Watt, prev, &err));
    CHECK(prev.residual.aiming != 0.0);
    // Aimed, the traced share is a small fraction of the emitted one and the
    // rest is booked as escaped rather than traced into the void.
    CHECK(prev.fluxEscaped > 0.9 * prev.sourcePower);
    CHECK_NEAR(prev.fluxAccounted(), prev.sourcePower, 1e-9 * prev.sourcePower);

    const backendcheck::Report r = backendcheck::compare(ref, prev);
    CHECK(r.blocker.isEmpty());
    CHECK(r.ok);

    // Roulette: a long absorbing guide drives a branch down until it is followed
    // with a probability rather than outright. Whichever way that goes, the
    // buckets still close.
    SimConfig guide = quickConfig(GeometryProvider::Scene::LightGuide, 200000);
    guide.seed = 11111;
    guide.physics.absorptionScale = 400.0;      // many decades over the guide
    const Simulation::SceneRef gdata = Simulation::dataFor(guide);
    const std::vector<SourceConfig> gsrcs = Simulation::sourcesFor(guide, *gdata);
    TraceOptions ropt;
    ropt.seed    = 99999;
    ropt.physics = guide.physics;
    ropt.nTheta  = guide.nTheta;
    ropt.nPhi    = guide.nPhi;
    SimulationResult rouletted;
    CHECK(gputrace::trace(gdata->scene, gdata->scene.surfaces(), gsrcs, ropt,
                          guide.power, guide.fluxUnit, rouletted, &err));
    CHECK_NEAR(rouletted.fluxAccounted(), rouletted.sourcePower,
               1e-9 * rouletted.sourcePower);
}

TEST(gpu, a_polarised_run_crosses_and_reports_the_state_the_reference_does) {
    if (!gputrace::available()) return;
    // The last term the preview refused. It is worth carrying because the
    // unpolarised average cannot express any of what it is for: at Brewster's
    // angle s-polarised light reflects strongly off glass and p-polarised light
    // reflects nothing at all, and averaging the two throws away exactly the
    // difference the state exists to describe.
    //
    // The geometry is that angle. A narrow beam meets a glass plane at
    // atan(1.5) = 56.31 degrees and a plate off to the side catches what comes
    // back, so what the receiver sees is the reflected branch and only it.
    const double h = 200.0;
    const double x = 1.5 * h;              // tan(Brewster) for n = 1.5

    // A receiver is centred on the axis by construction, and this one must sit
    // where the reflected beam lands instead.
    const auto offsetPlate = [](double cx, double z, double size, int bins,
                                const QString& label) {
        const double s = 0.5 * size;
        MeshSurface m;
        m.label = label;
        m.verts = {gp_Pnt(cx - s, -s, z), gp_Pnt(cx + s, -s, z),
                   gp_Pnt(cx + s, s, z), gp_Pnt(cx - s, s, z)};
        Triangle a; a.v0 = 0; a.v1 = 1; a.v2 = 2; a.normal = gp_Dir(0, 0, 1);
        Triangle b; b.v0 = 0; b.v1 = 2; b.v2 = 3; b.normal = gp_Dir(0, 0, 1);
        m.tris = {a, b};
        m.isDetector = true;
        m.detCenter  = gp_Pnt(cx, 0, z);
        m.detW = size; m.detH = size;
        m.detNX = bins; m.detNY = bins;
        return m;
    };

    MeshSurface glass = glassPlane(1.5, 200.0);
    glass.fresnel = true;
    MeshList meshes{glass, offsetPlate(x, h, 240.0, 16, QStringLiteral("reflected"))};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 2.0;                // narrow, so the angle is the angle
    src.rays         = 400000;
    src.origin       = gp_Pnt(-x, 0, h);
    src.axis         = gp_Dir(x, 0, -h);   // at the origin, and at Brewster

    const auto shoot = [&](int state, bool gpu) {
        std::vector<SourceConfig> s{src};
        s.front().polarisationState = state;
        TraceOptions opt;
        opt.seed             = gpu ? 99999 : 11111;
        opt.physics.polarised = true;
        opt.noiseMap         = true;
        SimulationResult r;
        if (gpu) {
            QString err;
            const bool ok = gputrace::trace(scene, scene.surfaces(), s, opt, 1.0,
                                            FluxUnit::Watt, r, &err);
            CHECK(ok);
        } else {
            RayTracer::trace(scene, s, r, opt);
        }
        return r;
    };

    // Unpolarised in. What comes back off glass at Brewster is very nearly pure
    // s, which is a thing a scalar reflectance cannot report at all.
    const SimulationResult refUnpol = shoot(0, false);
    const SimulationResult gpuUnpol = shoot(0, true);
    CHECK(refUnpol.polarised);
    CHECK(gpuUnpol.polarised);
    CHECK(refUnpol.degreeLinear > 0.9);
    CHECK_NEAR(gpuUnpol.degreeLinear, refUnpol.degreeLinear, 0.02);
    CHECK_NEAR(gpuUnpol.degreeOfPolarisation, refUnpol.degreeOfPolarisation, 0.02);
    // The reflected beam is s-polarised: q positive, u nothing, so the angle of
    // the linear part is along the s axis it is measured from.
    CHECK(std::fabs(gpuUnpol.polarisationAngleDeg -
                    refUnpol.polarisationAngleDeg) < 2.0);

    const backendcheck::Report rep = backendcheck::compare(refUnpol, gpuUnpol);
    CHECK(rep.blocker.isEmpty());
    CHECK(rep.ok);
    CHECK_NEAR(gpuUnpol.fluxAccounted(), gpuUnpol.sourcePower,
               1e-9 * gpuUnpol.sourcePower);

    // And the split really is decided by the state rather than by the average
    // of the two. An s-polarised source reflects most of what an unpolarised one
    // would; a p-polarised source at this angle reflects almost nothing. A model
    // that averaged would give these two the same answer.
    const SimulationResult refS = shoot(1, false);
    const SimulationResult refP = shoot(2, false);
    const SimulationResult gpuS = shoot(1, true);
    const SimulationResult gpuP = shoot(2, true);
    CHECK(refS.fluxDetector > 20.0 * refP.fluxDetector);
    CHECK(gpuS.fluxDetector > 20.0 * gpuP.fluxDetector);
    CHECK_NEAR(gpuS.fluxDetector, refS.fluxDetector, 0.03 * refS.fluxDetector);
    // The p case is a null and near zero, so it is compared against the run it
    // is a null of rather than against itself.
    CHECK(std::fabs(gpuP.fluxDetector - refP.fluxDetector) <
          0.01 * refS.fluxDetector);
    // Both extremes stay fully polarised through a single reflection: an
    // interface is a diattenuator, and a diattenuator cannot depolarise a beam
    // that was already in one of its own eigenstates.
    CHECK(gpuS.degreeLinear > 0.98);
    CHECK(gpuP.degreeLinear > 0.98);
}

TEST(gpu, a_mirror_turns_the_state_on_the_preview_the_way_it_does_on_the_reference) {
    if (!gputrace::available()) return;
    // Metal reflection is the largest single source of polarisation change in
    // most reflective systems: aluminium at sixty degrees turns linear light
    // elliptical, so a periscope of two mirrors is not a null. Before this the
    // reflected branch inherited the incoming state verbatim and a polarised
    // run off a mirror reported the source back to itself -- which is the
    // failure this test would catch.
    const double h = 200.0;
    const double x = h * 1.7320508075688772;    // 60 degrees from the normal

    const auto offsetPlate = [](double cx, double z, double size, int bins,
                                const QString& label) {
        const double s = 0.5 * size;
        MeshSurface m;
        m.label = label;
        m.verts = {gp_Pnt(cx - s, -s, z), gp_Pnt(cx + s, -s, z),
                   gp_Pnt(cx + s, s, z), gp_Pnt(cx - s, s, z)};
        Triangle a; a.v0 = 0; a.v1 = 1; a.v2 = 2; a.normal = gp_Dir(0, 0, 1);
        Triangle b; b.v0 = 0; b.v1 = 2; b.v2 = 3; b.normal = gp_Dir(0, 0, 1);
        m.tris = {a, b};
        m.isDetector = true;
        m.detCenter  = gp_Pnt(cx, 0, z);
        m.detW = size; m.detH = size;
        m.detNX = bins; m.detNY = bins;
        return m;
    };

    MeshSurface mirror = glassPlane(0.0, 300.0);
    mirror.label        = QStringLiteral("mirror");
    mirror.index        = 0.0;
    mirror.reflectivity = 1.0;              // superseded by the complex index
    mirror.material     = materials::byName(QStringLiteral("Aluminium"));

    MeshList meshes{mirror, offsetPlate(x, h, 300.0, 16, QStringLiteral("caught"))};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 2.0;
    src.rays         = 300000;
    src.origin       = gp_Pnt(-x, 0, h);
    src.axis         = gp_Dir(x, 0, -h);
    // At 45 degrees to the plane of incidence, which is the one input that has
    // somewhere to go: a beam already s or p leaves a mirror still s or p, and
    // only a state between them picks up the retardance.
    src.polarisationState = 3;              // circular, the strongest probe of V

    TraceOptions opt;
    opt.physics.polarised = true;
    opt.noiseMap = true;
    opt.seed = 11111;
    SimulationResult ref;
    RayTracer::trace(scene, std::vector<SourceConfig>{src}, ref, opt);

    TraceOptions gopt = opt;
    gopt.seed = 99999;
    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(scene, scene.surfaces(), {src}, gopt, 1.0,
                          FluxUnit::Watt, prev, &err));

    CHECK(ref.polarised);
    CHECK(prev.polarised);
    // The mirror really did turn it: circular in, and what comes out is no
    // longer circular. If the branch had inherited the source's state this
    // difference would be exactly zero, which is the bug this replaces.
    CHECK(ref.degreeLinear > 1e-3);
    CHECK_NEAR(prev.degreeLinear, ref.degreeLinear, 0.02);
    CHECK_NEAR(prev.degreeOfPolarisation, ref.degreeOfPolarisation, 0.02);

    const backendcheck::Report rep = backendcheck::compare(ref, prev);
    CHECK(rep.blocker.isEmpty());
    CHECK(rep.ok);
    CHECK_NEAR(prev.fluxAccounted(), prev.sourcePower, 1e-9 * prev.sourcePower);
}

TEST(gpu, a_broadband_run_disperses_on_the_preview_the_way_it_does_on_the_reference) {
    if (!gputrace::available()) return;
    // A spectral run draws a wavelength per ray and reads every
    // wavelength-dependent quantity at it. The device does not carry the four
    // dispersion models: the host samples each curve once per run off the
    // reference's own functions and uploads it, so what is checked here is
    // whether that sampling is faithful enough that the two backends land on
    // the same prism.
    //
    // A prism is the scene for it. Its whole output is where each wavelength
    // went, so a preview that quietly evaluated n at one wavelength would put
    // the spot in the wrong place rather than merely shading it differently.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Prism, 300000);
    cfg.seed              = 11111;
    cfg.noiseMap          = true;
    cfg.spectrum          = SpectrumConfig::Kind::D65;
    cfg.physics.dispersion = true;
    // Narrow, so the light goes through the prism rather than around it. A
    // hemispherical emitter floods the 600 mm receiver with light that never
    // met the glass, and the deviated beam -- the whole subject here -- is a
    // few percent of what lands.
    cfg.halfAngleDeg      = 6.0;
    const SimulationResult ref = Simulation::run(cfg);

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    TraceOptions opt;
    opt.seed     = 99999;
    opt.physics  = cfg.physics;
    opt.nTheta   = cfg.nTheta;
    opt.nPhi     = cfg.nPhi;
    opt.noiseMap = true;
    SimulationResult prev;
    QString err;
    CHECK(gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                          cfg.power, cfg.fluxUnit, prev, &err));

    const backendcheck::Report rep = backendcheck::compare(ref, prev);
    CHECK(rep.blocker.isEmpty());
    CHECK(rep.ok);
    CHECK_NEAR(prev.fluxAccounted(), prev.sourcePower, 1e-9 * prev.sourcePower);

    // The three colour bands are the dispersion made visible, and they are a
    // decomposition of the irradiance grid rather than a second measurement of
    // it: bin by bin they must add back up to it exactly, or the heatmap and
    // the number under it are describing different runs.
    CHECK(prev.bandIrradiance.size() == 3 * prev.irradiance.size());
    if (prev.bandIrradiance.size() == 3 * prev.irradiance.size()) {
        double worst = 0.0, total = 0.0;
        for (std::size_t i = 0; i < prev.irradiance.size(); ++i) {
            const double sum = prev.bandIrradiance[i] +
                               prev.bandIrradiance[prev.irradiance.size() + i] +
                               prev.bandIrradiance[2 * prev.irradiance.size() + i];
            worst = std::max(worst, std::fabs(sum - prev.irradiance[i]));
            total += prev.irradiance[i];
        }
        CHECK(worst <= 1e-9 * std::max(1e-30, total));
    }

    // The sharper claim, and the one that says the device really reads n at
    // each ray's own wavelength rather than once at the d line: the same prism
    // at two wavelengths puts the spot in two places.
    //
    // It is done with two monochromatic runs rather than with the band
    // centroids of the broadband one above. Broadband, red and blue are two
    // overlapping smears whose centroids sit a few hundredths of a bin apart on
    // this receiver -- below the sampling noise, so asserting on their
    // difference would be asserting on noise. Two narrow spots resolve it.
    // The width of the pattern across the dispersing axis, not its centre. This
    // prism is symmetric about x = 0, so both halves of the beam deviate
    // outward by the same angle and the centroid does not move with wavelength
    // at all -- a centroid test here would read 31.50 for every colour and
    // assert nothing. What dispersion changes is how far out they land.
    const auto spotX = [](const SimulationResult& r) {
        double num = 0.0, den = 0.0, sq = 0.0;
        for (int y = 0; y < r.ny; ++y)
            for (int x = 0; x < r.nx; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(r.nx) +
                                      std::size_t(x);
                num += r.irradiance[i] * double(x);
                den += r.irradiance[i];
                sq  += r.irradiance[i] * double(x) * double(x);
            }
        if (!(den > 0.0)) return 0.0;
        const double mean = num / den;
        return std::sqrt(std::max(0.0, sq / den - mean * mean));
    };
    const auto atLine = [&](double nm, bool gpu) {
        SimConfig mono = cfg;
        mono.spectrum = SpectrumConfig::Kind::Monochromatic;
        mono.spectrum.wavelengthNm = nm;
        if (!gpu) return Simulation::run(mono);
        const Simulation::SceneRef md = Simulation::dataFor(mono);
        const std::vector<SourceConfig> ms = Simulation::sourcesFor(mono, *md);
        TraceOptions mo = opt;
        mo.physics = mono.physics;
        SimulationResult r;
        QString e;
        CHECK(gputrace::trace(md->scene, md->scene.surfaces(), ms, mo, mono.power,
                              mono.fluxUnit, r, &e));
        return r;
    };
    const double gpuBlue = spotX(atLine(450.0, true));
    const double gpuRed  = spotX(atLine(650.0, true));
    const double refBlue = spotX(atLine(450.0, false));
    const double refRed  = spotX(atLine(650.0, false));
    // Blue is deviated further than red, because a flint's index is higher
    // there -- the direction of the effect, not merely its presence. A backend
    // reading n once would put the two spots on top of each other, and one
    // reading it backwards would pass every energy check above and still be
    // wrong about the only thing a prism is for.
    CHECK(gpuBlue > gpuRed + 0.5);
    CHECK(refBlue > refRed + 0.5);
    // And by the same amount as the reference, to well inside a bin: the device
    // does not evaluate a dispersion model, it interpolates the curve the host
    // sampled off the reference's own, so any daylight here is that sampling.
    CHECK(std::fabs((gpuBlue - gpuRed) - (refBlue - refRed)) <
          0.05 * (refBlue - refRed));

    // The same scene in lumens. Every ray carries the photometric weight of its
    // own wavelength at emission, which is what makes the detected total
    // photometric without a second grid anywhere -- and the two backends have to
    // agree about that weighting too, not only about where the light went.
    SimConfig lum = cfg;
    lum.fluxUnit = FluxUnit::Lumen;
    const SimulationResult refLm = Simulation::run(lum);
    SimulationResult prevLm;
    CHECK(gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                          lum.power, lum.fluxUnit, prevLm, &err));
    const backendcheck::Report repLm = backendcheck::compare(refLm, prevLm);
    CHECK(repLm.blocker.isEmpty());
    CHECK(repLm.ok);
}
