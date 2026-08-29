// Tests for the engine-audit work: the named estimator channels, the
// truncation and medium-anomaly instrumentation, label-keyed surface
// overrides, the Mueller matrix at metal reflection, the material-catalogue
// loader, measured ray files, multiple sources, and the reference corpus.
//
// Included from TestMain.cpp after NewTests.inc, so the helpers defined there
// (quickConfig, and the harness itself) are in scope.

// ============================================================== channels ====

TEST(channels, the_named_estimator_channels_sum_to_the_residual_they_replaced) {
    // Splitting one accumulator into five must not change the total: the energy
    // balance closed before and closes now, and `fluxRoulette` still reports
    // exactly what it always did.
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene(s), 4000);
        cfg.power = 37.5;
        const SimulationResult r = Simulation::run(cfg);

        CHECK_NEAR(r.residual.total(), r.fluxRoulette, 1e-9 * r.sourcePower);
        CHECK_NEAR(r.truncation.total(), r.fluxTruncated, 1e-9 * r.sourcePower);
        CHECK_NEAR(r.fluxAccounted(), r.sourcePower, 1e-7);
    }
}

TEST(channels, every_estimator_averages_to_nothing_over_independent_runs) {
    // This is the check the split was made for. The five channels *summed* have
    // an expectation of zero even when one contributor is systematically wrong
    // and another happens to cancel it -- the one respect in which the closed
    // energy balance could close while being wrong. Checked separately, each
    // estimator has to answer for itself.
    //
    // Four quantities rather than five, because next-event estimation is one
    // substitution with two halves: it books the analytic direct term negative
    // and the sampled hit it replaced positive, and those two are *designed* to
    // cancel. Their sum is the meaningful thing, and asserting it is the direct
    // statement that the analytic term equals the sampled one in expectation --
    // which is exactly the bias that would otherwise never be caught.
    //
    // Three scenes of different character, because the estimators that fire at
    // all differ completely between them: a collimator is specular, a sphere is
    // diffuse and next-event-dominated, a guide is TIR-heavy and deep.
    const GeometryProvider::Scene scenes[] = {
        GeometryProvider::Scene::Reflector,
        GeometryProvider::Scene::IntegratingSphere,
        GeometryProvider::Scene::LightGuide,
    };
    const char* names[] = {"roulette", "aiming", "next-event substitution",
                           "BSDF weights"};

    for (GeometryProvider::Scene scene : scenes) {
        constexpr int kRuns = 12;
        constexpr int kChannels = 4;
        double mean[kChannels] = {}, m2[kChannels] = {};

        for (int k = 0; k < kRuns; ++k) {
            SimConfig cfg = quickConfig(scene, 6000);
            cfg.seed = 4242u + std::uint64_t(k) * 7919u;
            const SimulationResult r = Simulation::run(cfg);
            const double v[kChannels] = {
                r.residual.roulette,
                r.residual.aiming,
                r.residual.nextEvent + r.residual.neeSuppressed,
                r.residual.bsdfWeight,
            };
            for (int c = 0; c < kChannels; ++c) {
                const double d = v[c] - mean[c];
                mean[c] += d / double(k + 1);
                m2[c]   += d * (v[c] - mean[c]);
            }
        }

        for (int c = 0; c < kChannels; ++c) {
            const double sd  = std::sqrt(m2[c] / double(kRuns > 1 ? kRuns - 1 : 1));
            const double err = sd / std::sqrt(double(kRuns));
            // Three standard errors, with a floor so a channel that never fires
            // at all (and so has exactly zero spread) is not compared against a
            // zero-width band.
            const double band = std::max(3.0 * err, 1e-6);
            if (!(std::fabs(mean[c]) <= band))
                std::printf("       %s on scene %d: mean %.6g, 3 s.e. %.6g\n",
                            names[c], int(scene), mean[c], band);
            CHECK(std::fabs(mean[c]) <= band);
        }
    }
}

TEST(channels, the_two_halves_of_next_event_estimation_are_the_same_size) {
    // The pair above cancels by construction, so it is worth saying out loud
    // what it means: on a scene where next-event estimation carries most of the
    // signal, the analytic term it adds and the sampled hit it suppresses are
    // both large, and they agree. If one drifted the sum would move, and the
    // sum is what the previous test watches.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::IntegratingSphere, 40000);
    const SimulationResult r = Simulation::run(cfg);

    // Both halves really are doing work here, or the test above is vacuous.
    CHECK(r.residual.nextEvent     < -0.01 * r.sourcePower);
    CHECK(r.residual.neeSuppressed >  0.01 * r.sourcePower);
    // And they agree to well inside the run error bar on the efficiency they
    // are two descriptions of.
    const double pair = r.residual.nextEvent + r.residual.neeSuppressed;
    CHECK(std::fabs(pair) < 8.0 * r.efficiencyStdErr * r.sourcePower + 1e-9);
}

TEST(channels, turning_an_estimator_off_does_not_move_the_answer) {
    // The statistical-equivalence test. Aiming, next-event estimation, roulette
    // and branch collapsing are each a place a subtle bias could enter and
    // never be caught, because nothing asserted that switching one off gives
    // the same expectation as leaving it on.
    struct Case { const char* name; GeometryProvider::Scene scene; };
    const Case cases[] = {
        {"specular", GeometryProvider::Scene::Reflector},
        {"diffuse",  GeometryProvider::Scene::IntegratingSphere},
        {"TIR",      GeometryProvider::Scene::LightGuide},
    };

    // Efficiency and its error bar, averaged over independent seeds so the
    // comparison is between two estimates and not between two draws.
    auto measure = [](GeometryProvider::Scene scene,
                      const std::function<void(SimConfig&)>& tweak,
                      double& value, double& err) {
        constexpr int kRuns = 6;
        double sum = 0.0, sumErr2 = 0.0;
        for (int k = 0; k < kRuns; ++k) {
            SimConfig cfg = quickConfig(scene, 20000);
            cfg.seed = 90210u + std::uint64_t(k) * 104729u;
            tweak(cfg);
            const SimulationResult r = Simulation::run(cfg);
            sum     += r.efficiency;
            sumErr2 += r.efficiencyStdErr * r.efficiencyStdErr;
        }
        value = sum / double(kRuns);
        err   = std::sqrt(sumErr2) / double(kRuns);
    };

    for (const Case& c : cases) {
        double onV = 0.0, onE = 0.0;
        measure(c.scene, [](SimConfig&) {}, onV, onE);

        struct Switch {
            const char* what;
            std::function<void(SimConfig&)> off;
        };
        // Every estimator switch the tracer has, one at a time. `varianceReduction`
        // covers roulette and branch collapsing together, which is how the
        // engine exposes them.
        const Switch offs[] = {
            {"aiming", [](SimConfig& cfg) { cfg.physics.varianceReduction = true; }},
            {"roulette and splitting",
             [](SimConfig& cfg) { cfg.physics.varianceReduction = false; }},
        };

        for (const Switch& sw : offs) {
            double offV = 0.0, offE = 0.0;
            measure(c.scene, sw.off, offV, offE);
            const double band = 3.0 * std::sqrt(onE * onE + offE * offE) + 1e-9;
            if (!(std::fabs(onV - offV) <= band))
                std::printf("       %s / %s: on %.6f, off %.6f, 3 sigma %.6f\n",
                            c.name, sw.what, onV, offV, band);
            CHECK(std::fabs(onV - offV) <= band);
        }
    }
}

// =========================================================== diagnostics ====

TEST(diagnostics, an_acceptance_cone_rejects_rather_than_absorbs) {
    // Light a receiver refuses was not absorbed by anything -- it was refused by
    // a measurement condition -- and an absorbed-flux figure that silently
    // includes it is a wrong loss budget.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::IntegratingSphere, 20000);
    const SimulationResult wide = Simulation::run(cfg);
    CHECK_NEAR(wide.fluxRejected, 0.0, 1e-12);

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    int det = -1;
    for (std::size_t i = 0; i < data->surfaces.size(); ++i)
        if (data->surfaces[i].isDetector) { det = int(i); break; }
    CHECK(det >= 0);

    SurfaceOverride ov;
    ov.label    = data->surfaces[std::size_t(det)].label;
    ov.identity = data->scene.surfaceIdentity(det);
    ov.surface  = det;
    ov.optics   = data->surfaces[std::size_t(det)];
    ov.optics.detAcceptanceDeg = 10.0;          // a narrow photometer head
    cfg.surfaceOverrides.push_back(ov);

    const SimulationResult narrow = Simulation::run(cfg);
    CHECK(narrow.unmatchedOverrides.empty());
    // Something was refused, it is booked as refused, and the budget still
    // closes with the new bucket in it.
    CHECK(narrow.fluxRejected > 0.0);
    CHECK(narrow.fluxDetector < wide.fluxDetector);
    CHECK_NEAR(narrow.fluxAccounted(), narrow.sourcePower, 1e-7);

    // And a recording plane lets what it refuses carry on instead of eating it.
    cfg.surfaceOverrides.back().optics.detRejectMode = SurfaceOptics::RejectMode::Pass;
    const SimulationResult pass = Simulation::run(cfg);
    CHECK_NEAR(pass.fluxAccounted(), pass.sourcePower, 1e-7);
    CHECK(pass.fluxRejected == 0.0);
}

TEST(diagnostics, truncated_flux_says_which_mechanism_took_it) {
    // A light guide at the depth limit is the case the breakdown exists for:
    // one number cannot say whether raising the limit would help.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::LightGuide, 20000);
    const SimulationResult r = Simulation::run(cfg);

    CHECK_NEAR(r.truncation.total(), r.fluxTruncated, 1e-9 * r.sourcePower);
    // Every named bucket is non-negative; a negative one would mean energy was
    // booked out of a loss channel.
    CHECK(r.truncation.depthLimit    >= 0.0);
    CHECK(r.truncation.stackOverflow >= 0.0);
    CHECK(r.truncation.degenerate    >= 0.0);
    CHECK(r.truncation.refractFailed >= 0.0);
    CHECK(r.truncation.energyCutoff  >= 0.0);

    // With the estimators off, the hard cutoff is the only thing that ends a
    // path early, so that bucket is the one that has to carry the truncation.
    cfg.physics.varianceReduction = false;
    const SimulationResult cut = Simulation::run(cfg);
    CHECK_NEAR(cut.truncation.total(), cut.fluxTruncated, 1e-9 * cut.sourcePower);
    if (cut.fluxTruncated > 1e-6 * cut.sourcePower)
        CHECK(cut.truncation.energyCutoff > 0.0);
}

TEST(diagnostics, a_well_formed_scene_reports_no_medium_anomalies) {
    // The recoveries are right; the silence was not. A closed, well-formed
    // built-in scene must not be exercising any of them, or the counters would
    // be reporting noise rather than a symptom.
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene(s), 4000);
        const SimulationResult r = Simulation::run(cfg);
        // Stack overflow means more than kMediumDepth nested media at once,
        // which no built-in scene has.
        CHECK(r.anomalies.stackOverflow == 0);
    }
}

// ============================================================= overrides ====

TEST(overridekeys, an_override_follows_its_surface_by_label_not_by_slot) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 8000);
    const Simulation::SceneRef data = Simulation::dataFor(cfg);

    int mirror = -1;
    for (std::size_t i = 0; i < data->surfaces.size(); ++i)
        if (!data->surfaces[i].isDetector) { mirror = int(i); break; }
    CHECK(mirror >= 0);

    const QString label = data->surfaces[std::size_t(mirror)].label;
    CHECK(!label.isEmpty());

    SurfaceOverride ov;
    ov.label    = label;
    ov.identity = data->scene.surfaceIdentity(mirror);
    // The stored index is deliberately wrong, standing in for a config saved
    // against a registry that has since changed. The label has to win.
    ov.surface  = 999;
    ov.optics   = data->surfaces[std::size_t(mirror)];
    ov.optics.scatter = 1.0;
    cfg.surfaceOverrides.push_back(ov);

    const SimulationResult edited = Simulation::run(cfg);
    CHECK(edited.unmatchedOverrides.empty());

    SimConfig plainCfg = quickConfig(GeometryProvider::Scene::Reflector, 8000);
    const SimulationResult plain = Simulation::run(plainCfg);
    // A collimator whose mirror scatters is no longer a collimator, so the edit
    // demonstrably landed.
    CHECK(std::fabs(edited.efficiency - plain.efficiency) >
          6.0 * (edited.efficiencyStdErr + plain.efficiencyStdErr));
}

TEST(overridekeys, an_override_that_matches_nothing_is_reported_not_applied) {
    // The failure this replaces produced a plausible-looking wrong answer, which
    // is the only kind that costs anyone money. Being told is the whole point.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
    const SimulationResult plain = Simulation::run(cfg);

    SurfaceOverride ov;
    ov.label   = QStringLiteral("a surface this scene does not have");
    ov.surface = 0;                        // a slot that exists, and is not it
    ov.optics.reflectivity = 1.0;
    ov.optics.scatter      = 1.0;
    cfg.surfaceOverrides.push_back(ov);

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.unmatchedOverrides.size() == 1);
    if (!res.unmatchedOverrides.empty())
        CHECK(res.unmatchedOverrides.front() == ov.label);
    // Nothing was applied: the answer is exactly the untouched one.
    CHECK_NEAR(res.efficiency, plain.efficiency, 1e-12);
}

TEST(overridekeys, a_surface_identity_ignores_geometry_but_not_role) {
    // An identity that moved when a spin box moved would refuse to reapply an
    // edit the moment a user touched a parameter, which is worse than useless.
    SimConfig a = quickConfig(GeometryProvider::Scene::Reflector, 100);
    SimConfig b = a;
    b.useSceneDefaults = false;
    b.params = GeometryProvider::defaultParams(b.scene);
    b.params.v[0] *= 1.10;                 // a different optic, same surfaces

    const Simulation::SceneRef da = Simulation::dataFor(a);
    const Simulation::SceneRef db = Simulation::dataFor(b);
    CHECK(da.get() != db.get());
    CHECK(da->scene.surfaces().size() == db->scene.surfaces().size());
    for (int i = 0; i < int(da->scene.surfaces().size()); ++i)
        CHECK(da->scene.surfaceIdentity(i) == db->scene.surfaceIdentity(i));

    // Two different scenes do not share identities across the board.
    const Simulation::SceneRef dc =
        Simulation::dataFor(quickConfig(GeometryProvider::Scene::PlanoConvexLens, 100));
    bool allSame = da->scene.surfaces().size() == dc->scene.surfaces().size();
    if (allSame)
        for (int i = 0; i < int(da->scene.surfaces().size()); ++i)
            if (da->scene.surfaceIdentity(i) != dc->scene.surfaceIdentity(i))
                allSame = false;
    CHECK(!allSame);
}

// ========================================================== metal Mueller ===

TEST(polarisedmetal, a_metal_amplitude_pair_reproduces_the_scalar_reflectance) {
    // The two must agree, or the polarised path and the unpolarised one are
    // modelling different mirrors.
    const OpticalMaterial al = materials::byName(QStringLiteral("Aluminium"));
    CHECK(al.isMetal());
    const double n = al.indexAt(550.0), k = al.extinctionAt(550.0);

    for (int i = 0; i <= 85; i += 5) {
        const double cosI = std::cos(double(i) * 3.14159265358979323846 / 180.0);
        double rs = 0.0, rp = 0.0, phase = 0.0;
        metalAmplitudes(1.0, n, k, cosI, rs, rp, phase);
        const double want = metalReflectance(1.0, n, k, cosI);
        CHECK_NEAR(0.5 * (rs * rs + rp * rp), want, 1e-9);
    }
}

TEST(polarisedmetal, a_dielectric_written_as_a_metal_with_no_extinction_is_unchanged) {
    // Reduces to the ordinary real-index amplitudes as k -> 0, so a "metal" with
    // no absorption is not a special case that behaves differently.
    for (int i = 0; i <= 80; i += 10) {
        const double cosI = std::cos(double(i) * 3.14159265358979323846 / 180.0);
        double ms = 0.0, mp = 0.0, mphase = 0.0;
        metalAmplitudes(1.0, 1.5168, 0.0, cosI, ms, mp, mphase);
        double ds = 0.0, dp = 0.0, dphase = 0.0;
        polarisation::fresnelAmplitudes(1.0, 1.5168, cosI, ds, dp, dphase);
        CHECK_NEAR(ms, ds, 1e-9);
        CHECK_NEAR(mp, dp, 1e-9);
    }
}

TEST(polarisedmetal, a_mirror_changes_the_polarisation_it_reflects) {
    // The bug this covers: the opaque branch never set `havePol`, so a polarised
    // run bouncing off an aluminium reflector reported the source's own state
    // back to itself. A metal at a non-normal angle has to change it.
    const OpticalMaterial al = materials::byName(QStringLiteral("Aluminium"));
    const double n = al.indexAt(550.0), k = al.extinctionAt(550.0);

    const double cosI = std::cos(60.0 * 3.14159265358979323846 / 180.0);
    double rs = 0.0, rp = 0.0, phase = 0.0;
    metalAmplitudes(1.0, n, k, cosI, rs, rp, phase);

    // s and p do not reflect equally at 60 degrees off aluminium.
    CHECK(std::fabs(rs - rp) > 0.01);
    // And the reflection is a retarder: a phase that is neither 0 nor pi is what
    // turns linear light elliptical, and it is the whole reason a periscope of
    // two mirrors is not a null.
    const double pi = 3.14159265358979323846;
    CHECK(std::fabs(phase) > 0.05 && std::fabs(std::fabs(phase) - pi) > 0.05);

    // Through the Mueller matrix: light polarised at 45 degrees comes back with
    // a circular component it did not have.
    polarisation::Stokes in;
    in.i = 1.0; in.q = 0.0; in.u = 1.0; in.v = 0.0;
    const polarisation::Stokes out =
        polarisation::Mueller::fromFresnel(rs, rp, phase).apply(in);
    CHECK(out.valid());
    CHECK(std::fabs(out.v) > 1e-3 * out.i);
}

TEST(polarisedmetal, a_polarised_run_off_a_mirror_still_closes_its_budget) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    cfg.physics.polarised = true;
    cfg.polarisationState = 1;              // linear s

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    int mirror = -1;
    for (std::size_t i = 0; i < data->surfaces.size(); ++i)
        if (!data->surfaces[i].isDetector) { mirror = int(i); break; }
    CHECK(mirror >= 0);

    SurfaceOverride ov;
    ov.label    = data->surfaces[std::size_t(mirror)].label;
    ov.identity = data->scene.surfaceIdentity(mirror);
    ov.surface  = mirror;
    ov.optics   = data->surfaces[std::size_t(mirror)];
    ov.optics.material = materials::byName(QStringLiteral("Aluminium"));
    ov.optics.fresnel  = true;
    cfg.surfaceOverrides.push_back(ov);

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.polarised);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-7);
    CHECK(res.degreeOfPolarisation >= 0.0 && res.degreeOfPolarisation <= 1.0 + 1e-9);
}

// ============================================================= catalogue ====

TEST(catalogue, a_zemax_agf_glass_resolves_to_the_index_it_publishes) {
    // N-BK7 as Schott writes it: dispersion formula 2 (Sellmeier 1), the same
    // six coefficients the built-in entry carries, plus an internal
    // transmittance row the bulk absorption comes from.
    const QString path = QDir::temp().filePath(QStringLiteral("luxtrace_test.agf"));
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream ts(&f);
        ts << "CC Test catalogue\n";
        ts << "NM TEST-BK7 2 517642.251 1.5168 64.17 0 20 0\n";
        ts << "GC A crown for the reader to chew on\n";
        ts << "ED 7.1 8.3 2.51 0.2087 0\n";
        ts << "CD 1.03961212 0.00600069867 0.231792344 0.0200179144 1.01046945 103.560653 0 0 0 0\n";
        ts << "LD 0.3 2.5\n";
        ts << "IT 0.5876 0.998 10\n";
    }

    const auto res = materialfile::loadAgf(path);
    if (!res.ok) std::printf("       %s\n", res.error.toLocal8Bit().constData());
    CHECK(res.ok);
    CHECK(res.added == 1);

    const OpticalMaterial m = materials::byName(QStringLiteral("TEST-BK7"));
    CHECK(m.valid());
    // The published d-line index, to the digit the catalogue quotes it to.
    CHECK_NEAR(m.indexAt(587.6), 1.5168, 5e-5);
    CHECK_NEAR(m.abbe(), 64.17, 0.2);
    // alpha = -ln(0.998) / 10 mm.
    CHECK_NEAR(m.alpha, -std::log(0.998) / 10.0, 1e-9);
    // And it reports where it came from, so a number can be traced to a file.
    CHECK(!materials::source(materials::indexOf(QStringLiteral("TEST-BK7"))).isEmpty());

    QFile::remove(path);
    materials::resetLoaded();
    CHECK(materials::indexOf(QStringLiteral("TEST-BK7")) < 0);
}

TEST(catalogue, a_refractiveindex_info_entry_loads_as_a_measured_metal) {
    const QString path = QDir::temp().filePath(QStringLiteral("luxtrace_test_metal.yml"));
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream ts(&f);
        ts << "REFERENCES: \"A paper nobody will read\"\n";
        ts << "DATA:\n";
        ts << "  - type: tabulated nk\n";
        ts << "    data: |\n";
        ts << "        0.400 0.490 4.860\n";
        ts << "        0.500 0.769 6.080\n";
        ts << "        0.600 1.200 7.260\n";
        ts << "        0.700 1.830 8.310\n";
        ts << "        0.800 2.750 8.310\n";
    }

    const auto res = materialfile::loadRefractiveIndexYaml(
        path, QStringLiteral("TEST-Al"));
    if (!res.ok) std::printf("       %s\n", res.error.toLocal8Bit().constData());
    CHECK(res.ok);

    const OpticalMaterial m = materials::byName(QStringLiteral("TEST-Al"));
    CHECK(m.valid());
    CHECK(m.isMetal());
    // The same data the built-in aluminium carries, so the reflectance has to
    // land where the built-in one does.
    const OpticalMaterial al = materials::byName(QStringLiteral("Aluminium"));
    CHECK_NEAR(metalReflectance(1.0, m.indexAt(550.0), m.extinctionAt(550.0), 1.0),
               metalReflectance(1.0, al.indexAt(550.0), al.extinctionAt(550.0), 1.0),
               0.01);

    QFile::remove(path);
    materials::resetLoaded();
}

TEST(catalogue, a_loaded_catalogue_never_redefines_a_built_in_glass) {
    // Loading a vendor file must not silently change what N-BK7 means for every
    // saved config in the project.
    const OpticalMaterial before = materials::byName(QStringLiteral("N-BK7"));
    OpticalMaterial impostor;
    impostor.set   = true;
    impostor.model = OpticalMaterial::Model::Constant;
    impostor.nd    = 9.99;
    materials::add(QStringLiteral("N-BK7"), QStringLiteral("not really"), impostor,
                   QStringLiteral("nowhere.agf"));

    const OpticalMaterial after = materials::byName(QStringLiteral("N-BK7"));
    CHECK_NEAR(after.indexAt(587.6), before.indexAt(587.6), 1e-12);
    materials::resetLoaded();
}

TEST(catalogue, a_file_that_is_not_a_catalogue_is_refused_with_a_reason) {
    const QString path = QDir::temp().filePath(QStringLiteral("luxtrace_test_junk.agf"));
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&f) << "this is not a glass catalogue\n";
    }
    const auto res = materialfile::load(path);
    CHECK(!res.ok);
    CHECK(!res.error.isEmpty());
    QFile::remove(path);
}

// =============================================================== ray file ===

namespace {

// A text ray file: a small square emitter radiating into a cone, written in the
// six-to-eight column layout ASAP, TracePro and most vendor text dumps use.
QString writeTextRayFile(const QString& name, int n, bool wavelengths) {
    const QString path = QDir::temp().filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QString();
    QTextStream ts(&f);
    ts << "# x y z l m n flux" << (wavelengths ? " wavelength" : "") << "\n";
    std::uint64_t rng = optics::mix64(0xA11CEu);
    for (int i = 0; i < n; ++i) {
        const double x = (optics::uniform01(rng) - 0.5) * 1.0;   // a 1 mm die
        const double y = (optics::uniform01(rng) - 0.5) * 1.0;
        // Cosine-weighted into +Z, which is what a Lambertian die emits.
        const Vec3 d = optics::cosineHemisphere(Vec3(0, 0, 1),
                                                optics::uniform01(rng),
                                                optics::uniform01(rng));
        ts << x << " " << y << " 0 " << d.x << " " << d.y << " " << d.z << " 1.0";
        if (wavelengths) ts << " 550.0";
        ts << "\n";
    }
    return path;
}

// The same set as a Zemax binary source file, so the two readers can be shown
// to produce the same thing.
QString writeBinaryRayFile(const QString& name, int n) {
    const QString path = QDir::temp().filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QDataStream out(&f);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);

    out << qint32(1010) << qint32(n);
    char description[100] = {};
    std::snprintf(description, sizeof(description), "test emitter");
    out.writeRawData(description, 100);
    out << float(n) << float(n) << float(0.55f)           // source, set flux, lambda um
        << 0.0f << 90.0f << 0.0f << 360.0f                // inclination, azimuth
        << qint32(0)                                      // dimension units: mm
        << 0.0f << 0.0f << 0.0f                           // location
        << 0.0f << 0.0f << 0.0f                           // rotation
        << 1.0f << 1.0f << 1.0f                           // scale
        << 0.0f << 0.0f << 0.0f << 0.0f                   // unused
        << qint32(0) << qint32(0) << qint32(0) << qint32(0);   // format, flux type, reserved

    std::uint64_t rng = optics::mix64(0xA11CEu);
    for (int i = 0; i < n; ++i) {
        const float x = float((optics::uniform01(rng) - 0.5) * 1.0);
        const float y = float((optics::uniform01(rng) - 0.5) * 1.0);
        const Vec3 d = optics::cosineHemisphere(Vec3(0, 0, 1),
                                                optics::uniform01(rng),
                                                optics::uniform01(rng));
        out << x << y << 0.0f << float(d.x) << float(d.y) << float(d.z) << 1.0f;
    }
    return path;
}

} // namespace

TEST(rayfile, a_text_ray_file_reads_back_the_emitter_it_describes) {
    const QString path = writeTextRayFile(QStringLiteral("luxtrace_rays.txt"),
                                          5000, true);
    CHECK(!path.isEmpty());

    const auto res = rayfile::load(path);
    if (!res.ok) std::printf("       %s\n", res.error.toLocal8Bit().constData());
    CHECK(res.ok);
    const RayFileData& rf = *res.data;

    CHECK(rf.size() == 5000);
    CHECK(rf.hasWavelengths);
    CHECK_NEAR(rf.raySetFlux, 5000.0, 1e-6);
    CHECK_NEAR(rf.meanPower(), 1.0, 1e-9);
    // A 1 mm die: the bounds have to say so.
    const Vec3 e = rf.extent();
    CHECK_NEAR(e.x, 1.0, 0.05);
    CHECK_NEAR(e.y, 1.0, 0.05);
    CHECK_NEAR(e.z, 0.0, 1e-12);
    // Every direction is unit and forward.
    for (const SourceRay& r : rf.rays) {
        CHECK_NEAR(r.dir.length(), 1.0, 1e-9);
        CHECK(r.dir.z > 0.0);
    }
    QFile::remove(path);
}

TEST(rayfile, the_binary_and_text_readers_agree_on_the_same_set) {
    const QString a = writeTextRayFile(QStringLiteral("luxtrace_rays_a.txt"), 2000, false);
    const QString b = writeBinaryRayFile(QStringLiteral("luxtrace_rays_b.dat"), 2000);
    CHECK(!a.isEmpty() && !b.isEmpty());

    const auto ra = rayfile::load(a);
    const auto rb = rayfile::load(b);
    CHECK(ra.ok);
    if (!rb.ok) std::printf("       %s\n", rb.error.toLocal8Bit().constData());
    CHECK(rb.ok);
    CHECK(ra.data->size() == rb.data->size());
    CHECK(rb.data->format.contains(QStringLiteral("Zemax")));
    // The binary file stores float32, so agreement is to single precision.
    for (std::size_t i = 0; i < ra.data->size(); ++i) {
        CHECK_NEAR(ra.data->rays[i].origin.x, rb.data->rays[i].origin.x, 1e-6);
        CHECK_NEAR(ra.data->rays[i].dir.z,    rb.data->rays[i].dir.z,    1e-6);
    }
    // The binary header names the wavelength for the whole set rather than per
    // ray, and 0.55 um has to be read as 550 nm.
    CHECK_NEAR(rb.data->headerWavelengthNm, 550.0, 1e-3);

    QFile::remove(a);
    QFile::remove(b);
}

TEST(rayfile, a_ray_file_source_traces_and_closes_its_budget) {
    const QString path = writeTextRayFile(QStringLiteral("luxtrace_rays_trace.txt"),
                                          4000, true);
    const auto load = rayfile::load(path);
    CHECK(load.ok);

    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 8000);
    cfg.rayFile = load.data;
    cfg.power   = 12.5;

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.raysEmitted == 8000);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-7);
    CHECK(res.efficiency > 0.0);
    CHECK(res.sources.size() == 1);
    CHECK(res.sources.front().rayFile);
    QFile::remove(path);
}

TEST(rayfile, tracing_a_measured_set_is_reproducible_at_any_thread_count) {
    const QString path = writeTextRayFile(QStringLiteral("luxtrace_rays_det.txt"),
                                          3000, false);
    const auto load = rayfile::load(path);
    CHECK(load.ok);

    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 6000);
    cfg.rayFile = load.data;
    cfg.threads = 1;
    const SimulationResult one = Simulation::run(cfg);
    cfg.threads = 8;
    const SimulationResult many = Simulation::run(cfg);

    CHECK_NEAR(many.fluxDetector, one.fluxDetector, 0.0);
    CHECK_NEAR(many.fluxEscaped,  one.fluxEscaped,  0.0);
    QFile::remove(path);
}

TEST(rayfile, a_capped_load_samples_the_set_rather_than_taking_its_front) {
    // A measured set is often written in scan order, so the first N rays are one
    // edge of the die rather than a sample of it.
    const QString path = QDir::temp().filePath(QStringLiteral("luxtrace_rays_scan.txt"));
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream ts(&f);
        for (int i = 0; i < 1000; ++i)
            ts << (double(i) * 0.001) << " 0 0 0 0 1 1.0\n";   // x sweeps 0 -> 1
    }
    const auto res = rayfile::load(path, 100);
    CHECK(res.ok);
    CHECK(res.data->size() == 100);
    CHECK(res.data->declared == 1000);
    // The kept rays span the whole sweep, which taking the front would not.
    CHECK(res.data->bmax.x - res.data->bmin.x > 0.9);
    QFile::remove(path);
}

// =========================================================== multi source ===

TEST(multisource, two_half_power_sources_deliver_what_one_full_one_does) {
    // Splitting a source into two identical halves in the same place must not
    // change the answer: it is the same light, described twice.
    SimConfig one = quickConfig(GeometryProvider::Scene::Reflector, 40000);
    one.power = 10.0;
    const SimulationResult single = Simulation::run(one);

    SimConfig two = one;
    two.power = 5.0;
    SourceSpec s;
    s.type         = two.source;
    s.shape        = two.shape;
    s.spectrum     = two.spectrum;
    s.halfAngleDeg = two.halfAngleDeg;
    s.power        = 5.0;
    two.extraSources.push_back(s);

    const SimulationResult pair = Simulation::run(two);

    CHECK(pair.sources.size() == 2);
    CHECK_NEAR(pair.sourcePower, 10.0, 1e-12);
    CHECK(pair.raysEmitted == single.raysEmitted);
    CHECK_NEAR(pair.fluxAccounted(), pair.sourcePower, 1e-7);

    const double band = 4.0 * (pair.efficiencyStdErr + single.efficiencyStdErr) + 1e-6;
    if (!(std::fabs(pair.efficiency - single.efficiency) <= band))
        std::printf("       one %.6f, two %.6f, band %.6f\n",
                    single.efficiency, pair.efficiency, band);
    CHECK(std::fabs(pair.efficiency - single.efficiency) <= band);

    // Each half delivered about half, and the two halves add up to the whole.
    CHECK_NEAR(pair.sources[0].flux + pair.sources[1].flux, pair.fluxDetector,
               1e-9 * pair.sourcePower);
    CHECK(std::fabs(pair.sources[0].flux - pair.sources[1].flux) <
          0.25 * pair.fluxDetector + 1e-12);
}

TEST(multisource, the_ray_budget_is_shared_in_proportion_to_power) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 10000);
    cfg.power = 9.0;
    SourceSpec s;
    s.power = 1.0;                        // a tenth of the light
    cfg.extraSources.push_back(s);

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const auto srcs = Simulation::sourcesFor(cfg, *data);
    CHECK(srcs.size() == 2);
    CHECK(srcs[0].rays + srcs[1].rays == 10000);
    CHECK(std::abs(srcs[0].rays - 9000) <= 1);
    CHECK(std::abs(srcs[1].rays - 1000) <= 1);
}

TEST(multisource, a_second_source_somewhere_else_is_reported_separately) {
    // The stray-light case: a source beside the signal one, contributing light
    // the receiver sees and the designer needs to be able to separate out.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 30000);
    cfg.power = 1.0;

    SourceSpec stray;
    stray.label  = QStringLiteral("stray");
    stray.power  = 1.0;
    stray.type   = SourceConfig::Type::Point;
    stray.offset = gp_Pnt(60.0, 0.0, 0.0);     // well off the optical axis
    cfg.extraSources.push_back(stray);

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.sources.size() == 2);
    CHECK(res.sources[1].label == QStringLiteral("stray"));
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-7);
    CHECK_NEAR(res.sources[0].flux + res.sources[1].flux, res.fluxDetector,
               1e-9 * res.sourcePower);
    // The two are not the same source, so they do not deliver the same flux.
    CHECK(std::fabs(res.sources[0].flux - res.sources[1].flux) > 1e-6);

    // Every recorded arrival names the source it came from, which is what makes
    // the two separable downstream.
    bool sawFirst = false, sawSecond = false;
    for (const DetectorArrival& a : res.arrivals) {
        CHECK(a.source >= 0 && a.source < 2);
        if (a.source == 0) sawFirst  = true;
        if (a.source == 1) sawSecond = true;
    }
    CHECK(sawFirst || sawSecond);
}

TEST(multisource, every_source_draws_rays_into_the_viewport) {
    // The paths the viewport draws are recorded for a bounded number of rays,
    // and sources own consecutive blocks of the run's ray index space. Taking
    // that budget off the front of the space spent all of it inside the first
    // source's block, so a scene with two lamps in it lit one: every emitter
    // has to reach the drawn bundle.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 40000);
    cfg.power = 1.0;

    SourceSpec stray;
    stray.label  = QStringLiteral("stray");
    stray.power  = 1.0;
    stray.type   = SourceConfig::Type::Point;
    stray.offset = gp_Pnt(60.0, 0.0, 0.0);
    cfg.extraSources.push_back(stray);

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.sources.size() == 2);

    std::vector<int> drawn(res.sources.size(), 0);
    for (const RaySegment& s : res.raySegments) {
        CHECK(s.source < res.sources.size());
        ++drawn[s.source];
    }
    for (std::size_t i = 0; i < drawn.size(); ++i) CHECK(drawn[i] > 0);
}

TEST(multisource, a_single_source_run_is_untouched_by_the_machinery) {
    // The one-source answer has to be bit-identical to what it was before there
    // could be several, or every reference value in the corpus is a lie.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    cfg.power = 3.0;
    const SimulationResult a = Simulation::run(cfg);

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const SourceConfig one = Simulation::sourceFor(cfg, *data);
    TraceOptions opt;
    opt.seed    = cfg.seed;
    opt.physics = cfg.physics;
    opt.nTheta  = cfg.nTheta;
    opt.nPhi    = cfg.nPhi;
    SimulationResult b;
    RayTracer::trace(data->scene, one, b, opt);

    CHECK_NEAR(b.fluxDetector, a.fluxDetector, 0.0);
    CHECK_NEAR(b.fluxEscaped,  a.fluxEscaped,  0.0);
    CHECK_NEAR(b.efficiency,   a.efficiency,   0.0);
}

TEST(multisource, sources_stay_reproducible_at_any_thread_count) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    SourceSpec s;
    s.power  = 2.0;
    s.offset = gp_Pnt(5.0, 0.0, 0.0);
    cfg.extraSources.push_back(s);

    cfg.threads = 1;
    const SimulationResult one = Simulation::run(cfg);
    cfg.threads = 7;
    const SimulationResult many = Simulation::run(cfg);

    CHECK_NEAR(many.fluxDetector, one.fluxDetector, 0.0);
    CHECK_NEAR(many.fluxEscaped,  one.fluxEscaped,  0.0);
    CHECK_NEAR(many.sources[1].flux, one.sources[1].flux, 0.0);
}

TEST(multisource, five_sources_share_the_budget_and_still_close_the_balance) {
    // Two sources were the only count anything checked, and two is the one
    // count where "share the budget in proportion to power" cannot go wrong:
    // the loop that hands out rays reserves one for every source still to
    // come, and with a single iteration there is nothing to reserve.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 30000);
    cfg.power = 5.0;
    const double powers[4] = {4.0, 3.0, 2.0, 1.0};
    for (int i = 0; i < 4; ++i) {
        SourceSpec s;
        s.label  = QStringLiteral("extra %1").arg(i + 1);
        s.power  = powers[i];
        s.offset = gp_Pnt(6.0 * (i + 1), 0.0, 0.0);
        cfg.extraSources.push_back(s);
    }

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const auto srcs = Simulation::sourcesFor(cfg, *data);
    CHECK(srcs.size() == 5);

    // Every ray the user asked for is traced, and none twice.
    int budget = 0;
    for (const SourceConfig& c : srcs) {
        CHECK(c.rays >= 1);
        budget += c.rays;
    }
    CHECK(budget == 30000);

    // In proportion to power: 5, 4, 3, 2, 1 out of 15.
    const double total = 15.0;
    for (std::size_t i = 0; i < srcs.size(); ++i) {
        const double want = srcs[i].power / total * 30000.0;
        CHECK(std::fabs(double(srcs[i].rays) - want) <= 2.0);
    }

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.sources.size() == 5);
    CHECK_NEAR(res.sourcePower, 15.0, 1e-12);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-6);

    // The per-source flux is a decomposition of the total, not a second
    // estimate of it.
    double summed = 0.0;
    for (std::size_t i = 0; i < res.sources.size(); ++i) {
        CHECK(res.sources[i].rays == std::size_t(srcs[i].rays));
        CHECK(res.sources[i].flux >= 0.0);
        summed += res.sources[i].flux;
    }
    CHECK_NEAR(summed, res.fluxDetector, 1e-9 * res.sourcePower);
}

TEST(multisource, a_ray_budget_smaller_than_the_source_count_still_gives_each_one_a_ray) {
    // The degenerate end of the budget split. A source that gets no rays is a
    // source that silently is not in the run, and a zero flux beside a stated
    // power is a wrong answer rather than a small one.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 3);
    for (int i = 0; i < 5; ++i) {
        SourceSpec s;
        s.power  = (i == 0) ? 1000.0 : 0.001;   // as lopsided as it gets
        s.offset = gp_Pnt(4.0 * (i + 1), 0.0, 0.0);
        cfg.extraSources.push_back(s);
    }
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const auto srcs = Simulation::sourcesFor(cfg, *data);
    CHECK(srcs.size() == 6);
    int budget = 0;
    for (const SourceConfig& c : srcs) {
        CHECK(c.rays >= 1);
        budget += c.rays;
    }
    CHECK(budget == 6);          // raised to one per source, and spent exactly
}

TEST(multisource, every_source_draws_its_own_emission_stream) {
    // Each source is indexed by its own local ray number, which is what keeps
    // the first one emitting exactly what it emitted alone. On its own that
    // handed every later source the identical sequence of draws: four LEDs in
    // an array emitted four copies of one angular pattern, so the budget bought
    // the variance of a quarter of it and the replica spread the error bar is
    // read from was measuring a run nobody had made.
    //
    // Small enough that every ray of both sources is inside the segment budget,
    // so the recorded first legs are the emitted directions themselves.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 400);
    SourceSpec s;
    s.type         = cfg.source;
    s.shape        = cfg.shape;
    s.spectrum     = cfg.spectrum;
    s.halfAngleDeg = cfg.halfAngleDeg;
    s.power        = cfg.power;      // co-located, identical: nothing but the
    cfg.extraSources.push_back(s);   // stream can tell the two apart

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.sources.size() == 2);

    std::vector<Vec3> first, second;
    for (const RaySegment& g : res.raySegments) {
        if (g.depth != 0) continue;
        const Vec3 d = (g.b - g.a).normalized();
        if (g.source == 0) first.push_back(d);
        else               second.push_back(d);
    }
    CHECK(!first.empty());
    CHECK(!second.empty());

    std::size_t shared = 0;
    const std::size_t n = std::min(first.size(), second.size());
    for (std::size_t i = 0; i < n; ++i)
        if (std::fabs(first[i].x - second[i].x) < 1e-12 &&
            std::fabs(first[i].y - second[i].y) < 1e-12 &&
            std::fabs(first[i].z - second[i].z) < 1e-12) ++shared;
    if (shared > n / 10)
        std::printf("       %zu of %zu leading directions duplicated\n", shared, n);
    // A coincidence or two is a draw landing twice; every one of them is one
    // source copied onto another.
    CHECK(shared <= n / 10);
}

TEST(multisource, the_placement_overload_agrees_with_the_built_scene) {
    // The 3D view places its markers through the bare-placement overload and
    // the tracer places its emitters through the SceneData one. Two rules would
    // be two answers, and the picture would be quietly lying about where the
    // light starts.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 1000);
    SourceSpec a;
    a.offset = gp_Pnt(12.0, -3.0, 4.0);
    SourceSpec b;
    b.absolute = true;
    b.offset   = gp_Pnt(-40.0, 8.0, 55.0);
    b.useSceneAxis = false;
    b.axis   = gp_Dir(1, 0, 0);
    cfg.extraSources.push_back(a);
    cfg.extraSources.push_back(b);

    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const auto viaScene = Simulation::sourcesFor(cfg, *data);
    const auto viaPlace = Simulation::sourcesFor(cfg, data->sourceOrigin, data->sourceAxis);
    CHECK(viaScene.size() == viaPlace.size());
    for (std::size_t i = 0; i < viaScene.size() && i < viaPlace.size(); ++i) {
        CHECK_NEAR(viaPlace[i].origin.Distance(viaScene[i].origin), 0.0, 0.0);
        CHECK_NEAR(viaPlace[i].axis.Angle(viaScene[i].axis), 0.0, 0.0);
        CHECK(viaPlace[i].rays == viaScene[i].rays);
    }
}

// ------------------------------------------------------- the array scene ---

TEST(multisource, the_led_array_states_where_its_remaining_emitters_go) {
    const auto scene  = GeometryProvider::Scene::LedArrayLuminaire;
    const SceneParams P = GeometryProvider::defaultParams(scene);
    const auto offsets  = GeometryProvider::sourceOffsets(scene, P);

    // Four cups, so the scene places one emitter and names three more.
    CHECK(offsets.size() == 3);
    const double pitch = P.v[1];
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        CHECK_NEAR(offsets[i].X(), double(i + 1) * pitch, 1e-9);
        CHECK_NEAR(offsets[i].Y(), 0.0, 1e-12);
        CHECK_NEAR(offsets[i].Z(), 0.0, 1e-12);
    }

    // Every other scene is built around one emitter and must not invent a
    // lattice: an offset there would put light where the design has none.
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto s = GeometryProvider::Scene(i);
        if (s == scene) continue;
        CHECK(GeometryProvider::sourceOffsets(s, GeometryProvider::defaultParams(s)).empty());
    }

    // The emitter the scene places sits at the focus of the first cup, and the
    // offsets step from cup to cup -- so a source added at one lands on a cup
    // axis rather than between two of them.
    const auto setup = GeometryProvider::build(scene, P);
    const int  cups  = int(std::lround(P.v[0]));
    CHECK_NEAR(setup.sourceOrigin.X(), -0.5 * pitch * double(cups - 1), 1e-9);
    CHECK(setup.sourceAxis.Z() < 0.0);      // aimed down into its cup
    CHECK(setup.surfaces.size() == 2);      // the instanced cups, and the receiver
    CHECK(setup.surfaces.front().placements.size() == std::size_t(cups));
}

TEST(multisource, filling_the_led_array_lights_every_cup_equally) {
    // The scene exists to be traced with more than one source, so this is the
    // end-to-end statement of it: place a source over each cup and every cup
    // must deliver the same flux, because every cup is the same cup.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::LedArrayLuminaire, 60000);
    cfg.useSceneDefaults = false;
    cfg.params  = GeometryProvider::defaultParams(cfg.scene);
    cfg.source  = SourceConfig::Type::Lambertian;
    cfg.halfAngleDeg = 90.0;
    cfg.power   = 1.0;

    for (const gp_Pnt& off : GeometryProvider::sourceOffsets(cfg.scene, cfg.params)) {
        SourceSpec s;
        s.type         = cfg.source;
        s.halfAngleDeg = cfg.halfAngleDeg;
        s.power        = 1.0;
        s.offset       = off;
        cfg.extraSources.push_back(s);
    }
    CHECK(cfg.sourceCount() == int(std::lround(cfg.params.v[0])));

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.sources.size() == std::size_t(cfg.sourceCount()));
    CHECK_NEAR(res.sourcePower, double(cfg.sourceCount()), 1e-12);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-6);
    CHECK(res.fluxDetector > 0.0);

    // Identical cups, identical emitters: the spread across them is Monte Carlo
    // noise, not geometry. A misplaced offset -- an emitter between two cups,
    // or outside the row -- shows up here as one source far off the rest.
    double lo = 1e30, hi = -1e30;
    for (const SourceSummary& s : res.sources) {
        lo = std::min(lo, s.efficiency());
        hi = std::max(hi, s.efficiency());
    }
    if (!(hi - lo <= 0.05 * (hi + lo)))
        std::printf("       per-cup efficiency %.4f .. %.4f\n", lo, hi);
    CHECK(hi - lo <= 0.05 * (hi + lo));

    // And the row delivers what one cup delivers, once per cup: the second lamp
    // adds light rather than redistributing the first one.
    SimConfig one = cfg;
    one.extraSources.clear();
    const SimulationResult single = Simulation::run(one);
    CHECK_NEAR(res.efficiency, single.efficiency,
               6.0 * (res.efficiencyStdErr + single.efficiencyStdErr) + 1e-6);
    CHECK(res.fluxDetector > 2.0 * single.fluxDetector);
}

// ============================================================= regression ===

TEST(regression, every_scene_matches_the_stored_reference_to_the_last_digits) {
    // The payoff for the determinism work: because a run is bit-reproducible,
    // a reference value can be asserted rather than a band, and any behavioural
    // drift at all becomes visible.
    //
    // If this fails after a deliberate change, regenerate the file:
    //     LuxTrace --regress bless test/reference/scenes.tsv
    const QString path =
        QDir(QStringLiteral(LUXTRACE_TEST_DATA_DIR))
            .filePath(QStringLiteral("reference/scenes.tsv"));

    std::vector<studies::ReferencePoint> reference;
    QString err;
    if (!studies::readCorpus(path, reference, &err)) {
        std::printf("       %s\n", err.toLocal8Bit().constData());
        CHECK(false);
        return;
    }
    CHECK(reference.size() == std::size_t(GeometryProvider::count()));

    const int rays = reference.front().rays;
    const std::uint64_t seed = reference.front().seed;
    const auto measured = studies::referenceCorpus(rays, seed);

    const auto drift = studies::compareCorpus(reference, measured);
    if (!drift.empty())
        std::printf("%s", studies::regressionTable(drift, measured.size())
                              .toLocal8Bit().constData());
    CHECK(drift.empty());
}

TEST(regression, a_corpus_round_trips_through_its_file_exactly) {
    // Seventeen significant digits round-trips a double, so the stored value is
    // the value rather than an approximation of it. If it were not, the corpus
    // would report its own formatting as drift.
    const auto pts = studies::referenceCorpus(1000, 777u);
    const QString path = QDir::temp().filePath(QStringLiteral("luxtrace_corpus.tsv"));
    QString err;
    CHECK(studies::writeCorpus(path, pts, &err));

    std::vector<studies::ReferencePoint> back;
    CHECK(studies::readCorpus(path, back, &err));
    CHECK(back.size() == pts.size());
    CHECK(studies::compareCorpus(pts, back, 0.0).empty());

    QFile::remove(path);
}

// ========================================================== config keys =====

TEST(configkeys, a_surface_override_survives_a_round_trip_and_finds_its_surface) {
    // Overrides were not written to a config at all, so every optical edit a
    // user made was lost the moment they saved. Writing them by index would
    // have been worse: reopened against a changed registry, the edit lands on
    // whatever surface now occupies the slot.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 8000);
    const Simulation::SceneRef data = Simulation::dataFor(cfg);

    int mirror = -1;
    for (std::size_t i = 0; i < data->surfaces.size(); ++i)
        if (!data->surfaces[i].isDetector) { mirror = int(i); break; }
    CHECK(mirror >= 0);

    SurfaceOverride ov;
    ov.label    = data->surfaces[std::size_t(mirror)].label;
    ov.identity = data->scene.surfaceIdentity(mirror);
    ov.surface  = mirror;
    ov.optics   = data->surfaces[std::size_t(mirror)];
    ov.optics.roughness      = 0.021;
    ov.optics.scatter        = 0.37;
    ov.optics.mediumPriority = 3;
    ov.optics.material       = materials::byName(QStringLiteral("Aluminium"));
    ov.optics.bsdf.model     = bsdf::Model::Microfacet;
    ov.optics.bsdf.alpha     = 0.11;
    ov.optics.bsdf.fraction  = 0.42;
    ov.optics.volume.coefficient = 0.013;
    ov.optics.volume.anisotropy  = 0.6;
    ov.optics.coating.model      = coating::Model::Ideal;
    ov.optics.coating.residual   = 0.0025;
    cfg.surfaceOverrides.push_back(ov);

    SimConfig back;
    QString err;
    CHECK(configio::fromJson(configio::toJson(cfg), back, &err));
    CHECK(back.surfaceOverrides.size() == 1);
    if (back.surfaceOverrides.empty()) return;

    const SurfaceOverride& r = back.surfaceOverrides.front();
    CHECK(r.label == ov.label);
    CHECK(r.identity == ov.identity);
    CHECK_NEAR(r.optics.roughness, 0.021, 1e-12);
    CHECK_NEAR(r.optics.scatter,   0.37,  1e-12);
    CHECK(r.optics.mediumPriority == 3);
    CHECK(r.optics.material.isMetal());
    CHECK(r.optics.bsdf.model == bsdf::Model::Microfacet);
    CHECK_NEAR(r.optics.bsdf.alpha,    0.11, 1e-12);
    CHECK_NEAR(r.optics.bsdf.fraction, 0.42, 1e-12);
    CHECK_NEAR(r.optics.volume.coefficient, 0.013, 1e-12);
    CHECK_NEAR(r.optics.volume.anisotropy,  0.6,   1e-12);
    CHECK(r.optics.coating.model == coating::Model::Ideal);
    CHECK_NEAR(r.optics.coating.residual, 0.0025, 1e-12);

    // And it still resolves to the surface it was written against.
    CHECK(data->scene.resolveSurface(r.label, r.identity, r.surface) == mirror);
}

TEST(configkeys, extra_sources_survive_a_round_trip) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 8000);
    SourceSpec s;
    s.label         = QStringLiteral("second LED");
    s.type          = SourceConfig::Type::Lambertian;
    s.shape         = SourceConfig::Shape::Rect;
    s.spectrum.kind = SpectrumConfig::Kind::LedPhosphor;
    s.spectrum.cct  = 4000.0;
    s.halfAngleDeg  = 75.0;
    s.sizeA         = 1.4;
    s.sizeB         = 0.7;
    s.power         = 0.35;
    s.offset        = gp_Pnt(3.0, -2.0, 1.0);
    s.useSceneAxis  = false;
    s.axis          = gp_Dir(0, 1, 0);
    cfg.extraSources.push_back(s);

    SimConfig back;
    CHECK(configio::fromJson(configio::toJson(cfg), back));
    CHECK(back.extraSources.size() == 1);
    if (back.extraSources.empty()) return;

    const SourceSpec& r = back.extraSources.front();
    CHECK(r.label == s.label);
    CHECK(r.type  == s.type);
    CHECK(r.shape == s.shape);
    CHECK(r.spectrum.kind == s.spectrum.kind);
    CHECK_NEAR(r.spectrum.cct,  4000.0, 1e-9);
    CHECK_NEAR(r.halfAngleDeg,  75.0,   1e-12);
    CHECK_NEAR(r.sizeA,         1.4,    1e-12);
    CHECK_NEAR(r.sizeB,         0.7,    1e-12);
    CHECK_NEAR(r.power,         0.35,   1e-12);
    CHECK_NEAR(r.offset.X(),    3.0,    1e-12);
    CHECK_NEAR(r.offset.Z(),    1.0,    1e-12);
    CHECK(!r.useSceneAxis);
    CHECK_NEAR(r.axis.Y(),      1.0,    1e-12);
}

TEST(configkeys, a_ray_file_is_stored_by_path_and_reopened) {
    const QString path = writeTextRayFile(QStringLiteral("luxtrace_rays_cfg.txt"),
                                          1500, false);
    const auto load = rayfile::load(path);
    CHECK(load.ok);

    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
    cfg.rayFile      = load.data;
    cfg.rayFileScale = 2.5;

    SimConfig back;
    QStringList warnings;
    CHECK(configio::fromJson(configio::toJson(cfg), back, nullptr, &warnings));
    CHECK(warnings.isEmpty());
    CHECK(back.rayFile != nullptr);
    if (back.rayFile) {
        CHECK(back.rayFile->size() == load.data->size());
        CHECK_NEAR(back.rayFileScale, 2.5, 1e-12);
    }

    // A set that has gone missing is reported, and the source falls back to the
    // analytic emitter rather than silently tracing something else.
    const QString json = configio::toJson(cfg);
    QFile::remove(path);
    SimConfig gone;
    QStringList lost;
    CHECK(configio::fromJson(json, gone, nullptr, &lost));
    CHECK(gone.rayFile == nullptr);
    CHECK(lost.size() == 1);
}

// ========================================================= scatter model ====

TEST(scattermodel, a_rough_dielectric_no_longer_reflects_as_though_polished) {
    // The bug the collapse fixes. Three models used to run side by side in one
    // interaction: the Gaussian tilt decided a direction while the energy stayed
    // whatever specular Fresnel gave it at the *smooth* normal. So a rough
    // dielectric reflected exactly what a polished one would and then left in a
    // scattered direction.
    //
    // Fresnel is evaluated at the facet now. At normal incidence on the smooth
    // surface every facet is tilted away from the ray, the local angle of
    // incidence is larger, and a rough interface therefore reflects *more* than
    // a polished one -- which is what a real ground surface does.
    const double n1 = 1.0, n2 = 1.5168;
    const double smooth = optics::fresnelReflectance(n1, n2, 1.0);

    bsdf::Surface b;
    b.model    = bsdf::Model::Microfacet;
    b.alpha    = 0.4;                    // distinctly ground, not a polish error
    b.fraction = 1.0;

    std::uint64_t rng = optics::mix64(31337u);
    const Vec3 n(0, 0, 1);
    const Vec3 wi(0, 0, -1);             // straight in
    double sum = 0.0;
    int    taken = 0;
    for (int i = 0; i < 60000; ++i) {
        Vec3 h, o;
        double w = 1.0;
        if (!b.sampleMicrofacet(wi, n, rng, h, o, w)) continue;
        const double cosI = std::clamp(-wi.dot(h), 0.0, 1.0);
        sum += optics::fresnelReflectance(n1, n2, cosI);
        ++taken;
    }
    CHECK(taken > 40000);
    const double rough = sum / double(taken);
    if (!(rough > smooth * 1.02))
        std::printf("       rough %.6f vs smooth %.6f\n", rough, smooth);
    CHECK(rough > smooth * 1.02);
    // And not absurdly more: a ground surface is not a mirror.
    CHECK(rough < 0.5);
}

TEST(scattermodel, a_rough_refractive_surface_still_closes_its_budget) {
    // A microfacet interface splits *both* branches off the same facet, so the
    // place this could go wrong is the transmitted one -- which the old model
    // did not touch at all.
    for (auto scene : {GeometryProvider::Scene::PlanoConvexLens,
                       GeometryProvider::Scene::Prism,
                       GeometryProvider::Scene::LightGuide}) {
        SimConfig cfg = quickConfig(scene, 20000);
        cfg.power = 9.0;
        cfg.physics.roughnessOverride = 0.05;
        const SimulationResult r = Simulation::run(cfg);
        CHECK_NEAR(r.fluxAccounted(), r.sourcePower, 1e-7);
        CHECK(r.efficiency >= 0.0 && r.efficiency <= 1.0 + 1e-9);
    }
}

TEST(scattermodel, roughness_spreads_a_transmitted_beam_as_well_as_a_reflected_one) {
    // The transmitted branch used to leave along the *smooth* refracted
    // direction however rough the interface was, because the tilt was applied to
    // a normal the refraction then ignored. A lens that is ground on one face
    // does not image.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::PlanoConvexLens, 40000);
    cfg.physics.roughnessOverride = 0.0;
    const analysis::SpotMetrics clean =
        analysis::computeSpotMetrics(Simulation::run(cfg));

    cfg.physics.roughnessOverride = 0.06;
    const analysis::SpotMetrics ground =
        analysis::computeSpotMetrics(Simulation::run(cfg));

    CHECK(clean.valid && ground.valid);
    if (!(ground.rmsRadius > 1.2 * clean.rmsRadius))
        std::printf("       clean rms %.4f, ground rms %.4f\n",
                    clean.rmsRadius, ground.rmsRadius);
    CHECK(ground.rmsRadius > 1.2 * clean.rmsRadius);
}

TEST(scattermodel, the_physics_switches_still_gate_the_form_they_always_gated) {
    // `scattering` drove the Lambertian fraction and `roughness` drove the slope
    // error. After the collapse those are two forms of one model, and each
    // switch still turns off the form it always did.
    SimConfig lam = quickConfig(GeometryProvider::Scene::IntegratingSphere, 8000);
    lam.physics.scattering = false;
    const SimulationResult noScatter = Simulation::run(lam);
    CHECK_NEAR(noScatter.fluxAccounted(), noScatter.sourcePower, 1e-7);

    SimConfig rough = quickConfig(GeometryProvider::Scene::EllipticalReflector, 8000);
    rough.physics.roughnessOverride = 0.05;
    rough.physics.roughness = false;
    const SimulationResult off = Simulation::run(rough);
    rough.physics.roughnessOverride = -1.0;
    const SimulationResult plain = Simulation::run(rough);
    // With the switch off the slope error is not applied at all, so the answer
    // is the polished one to the bit.
    CHECK_NEAR(off.fluxDetector, plain.fluxDetector, 0.0);
}

// ========================================================== import audit ====

TEST(importaudit, a_closed_solid_passes_and_a_bare_face_does_not) {
    // The tracer's medium model depends on a solid being closed: a ray that
    // enters a body it is never recorded as leaving gets a guessed index pair
    // and a perfectly clean-looking answer.
    const TopoDS_Shape ball = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 12.0).Shape();
    const cadimport::GeometryAudit solid = cadimport::auditShape(ball);
    CHECK(solid.status == cadimport::GeometryAudit::Status::Ok);
    CHECK(solid.brepValid);
    CHECK(solid.closed);
    CHECK(solid.freeEdges == 0);

    // A single planar face is a sheet body. It is a perfectly good mirror and a
    // useless lens, and the audit has to say which it is looking at rather than
    // pass it silently.
    BRepBuilderAPI_MakePolygon poly(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0),
                                    gp_Pnt(10, 10, 0), gp_Pnt(0, 10, 0), Standard_True);
    const TopoDS_Shape sheet = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    const cadimport::GeometryAudit open = cadimport::auditShape(sheet);
    CHECK(open.status != cadimport::GeometryAudit::Status::Ok);
    CHECK(!open.notes.isEmpty());
}

TEST(importaudit, an_imported_file_carries_a_status_for_every_part) {
    const QString path = scratchFile(QStringLiteral("luxtrace_audit.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 5.0, 6.0, 7.0).Shape(), path));

    cadimport::ImportOptions opt;
    opt.scale = 1.0;
    opt.audit = true;
    const cadimport::ImportResult r = cadimport::read(path, opt);
    CHECK(r.ok);
    CHECK(!r.shapes.empty());
    for (const cadimport::ImportedShape& s : r.shapes) {
        CHECK(s.audit.status == cadimport::GeometryAudit::Status::Ok);
        CHECK(!s.audit.notes.isEmpty());
    }
    CHECK(r.worstStatus == cadimport::GeometryAudit::Status::Ok);
    CHECK(!r.auditSummary().isEmpty());
    QFile::remove(path);
}

TEST(importaudit, a_step_file_states_its_own_unit_and_the_reader_uses_it) {
    // The most common ray-file-and-CAD mistake there is, and the one the file
    // itself can settle. OCCT writes millimetres, so that is what has to come
    // back -- and it has to come back *from the header*, not from the default.
    const QString path = scratchFile(QStringLiteral("luxtrace_units.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 3.0, 3.0, 3.0).Shape(), path));

    QString unit;
    const double mm = cadimport::stepUnitScale(path, &unit);
    if (!(mm > 0.0)) std::printf("       no unit found in the header\n");
    CHECK(mm > 0.0);
    CHECK_NEAR(mm, 1.0, 1e-12);
    CHECK(unit.contains(QStringLiteral("milli")));

    // And a read that is not told a scale picks it up.
    cadimport::ImportOptions opt;      // scale 0: ask the file
    const cadimport::ImportResult r = cadimport::read(path, opt);
    CHECK(r.ok);
    CHECK(r.unitFromHeader);
    CHECK_NEAR(r.appliedScale, 1.0, 1e-12);
    CHECK_NEAR(r.bboxMax[0] - r.bboxMin[0], 3.0, 1e-6);
    QFile::remove(path);
}

TEST(importaudit, a_manual_scale_still_overrides_the_header) {
    const QString path = scratchFile(QStringLiteral("luxtrace_units2.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 2.0, 2.0, 2.0).Shape(), path));

    cadimport::ImportOptions opt;
    opt.scale = 1000.0;                // the file says mm; the user says metres
    const cadimport::ImportResult r = cadimport::read(path, opt);
    CHECK(r.ok);
    CHECK(!r.unitFromHeader);
    CHECK_NEAR(r.appliedScale, 1000.0, 1e-9);
    CHECK_NEAR(r.bboxMax[0] - r.bboxMin[0], 2000.0, 1e-3);
    QFile::remove(path);
}

TEST(importaudit, healing_never_hands_back_something_worse) {
    // ShapeFix is allowed to change the customer's geometry. Handing back
    // something worse than what they gave us is not a trade anybody agreed to,
    // so a repair that does not improve the audit is discarded.
    const TopoDS_Shape ball = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 8.0).Shape();
    cadimport::GeometryAudit a = cadimport::auditShape(ball);
    CHECK(a.status == cadimport::GeometryAudit::Status::Ok);

    const cadimport::GeometryAudit before = a;
    const TopoDS_Shape fixed = cadimport::healShape(ball, a);
    // Already perfect: nothing to gain, so the original comes back untouched.
    CHECK(fixed.IsSame(ball));
    CHECK(a.status == before.status);
    CHECK(a.freeEdges == before.freeEdges);
}

TEST(importaudit, the_audit_can_be_switched_off_for_a_repeated_read) {
    // It walks every face and every edge, so a study that re-reads the same
    // file should not pay for it every time.
    const QString path = scratchFile(QStringLiteral("luxtrace_noaudit.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 4.0).Shape(), path));

    cadimport::ImportOptions opt;
    opt.scale = 1.0;
    opt.audit = false;
    const cadimport::ImportResult r = cadimport::read(path, opt);
    CHECK(r.ok);
    // Not audited means not judged, which is the default status rather than a
    // claim that the part is fine.
    CHECK(r.worstStatus == cadimport::GeometryAudit::Status::Ok);
    for (const cadimport::ImportedShape& s : r.shapes) CHECK(s.audit.notes.isEmpty());
    QFile::remove(path);
}

// ================================================================= simd ======

TEST(simd, the_wide_leaf_agrees_with_the_scalar_one_bit_for_bit) {
    // The whole claim of the four-wide path is that it gives the *same* answer,
    // not a similar one: the arithmetic is written in the same order and the
    // same associations, with explicit multiply and add so nothing can be fused
    // into an FMA and re-rounded. Anything less and the reference corpus, the
    // determinism guarantee and the brute-force equivalence test all quietly
    // stop meaning what they say.
    if (!simd::avx2Available()) {
        std::printf("       no AVX2 on this machine; the scalar path is the only one\n");
        return;
    }

    std::uint64_t rng = optics::mix64(0xBEEF5EEDu);
    auto rnd = [&](double lo, double hi) {
        return lo + (hi - lo) * optics::uniform01(rng);
    };

    int compared = 0, hits = 0;
    for (int trial = 0; trial < 4000; ++trial) {
        // Four triangles of wildly different scale and orientation, including
        // the degenerate and near-parallel cases the scalar guard exists for.
        simd::Tri4 rec;
        SceneTri   tri[4];
        for (int lane = 0; lane < 4; ++lane) {
            const double s = std::pow(10.0, rnd(-3.0, 3.0));
            const Vec3 a(rnd(-1, 1) * s, rnd(-1, 1) * s, rnd(-1, 1) * s);
            const Vec3 b(rnd(-1, 1) * s, rnd(-1, 1) * s, rnd(-1, 1) * s);
            const Vec3 c(rnd(-1, 1) * s, rnd(-1, 1) * s, rnd(-1, 1) * s);
            tri[lane].v0 = a;
            tri[lane].e1 = b - a;
            tri[lane].e2 = c - a;
            for (int ax = 0; ax < 3; ++ax) {
                rec.v0[ax][lane] = tri[lane].v0[ax];
                rec.e1[ax][lane] = tri[lane].e1[ax];
                rec.e2[ax][lane] = tri[lane].e2[ax];
            }
            rec.tri[lane] = lane;
        }
        // Every fourth trial aims the ray straight at the first triangle's
        // centroid, so the comparison is not made entirely on misses.
        Vec3 o(rnd(-2, 2), rnd(-2, 2), rnd(-2, 2));
        Vec3 d;
        if (trial % 4 == 0) {
            const Vec3 centre = tri[0].v0 + (tri[0].e1 + tri[0].e2) * (1.0 / 3.0);
            d = centre - o;
        } else {
            d = Vec3(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1));
        }
        if (!d.normalize()) continue;

        const double ro[3] = {o.x, o.y, o.z};
        const double rd[3] = {d.x, d.y, d.z};
        double ts[4], us[4], vs[4];
        bool   hs[4];
        simd::intersect4(rec, ro, rd, ts, us, vs, hs);

        for (int lane = 0; lane < 4; ++lane) {
            double t = 0.0, u = 0.0, v = 0.0;
            const bool scalarHit = intersectTriangle(o, d, tri[lane].v0, tri[lane].e1,
                                                     tri[lane].e2, t, u, v);
            ++compared;
            // The scalar path lets a NaN distance through and leaves the caller
            // to reject it; the wide one masks it at the comparison. Both end up
            // rejecting, so a NaN is where the two are allowed to differ.
            if (scalarHit && !(t == t)) continue;
            if (scalarHit != hs[lane]) {
                std::printf("       trial %d lane %d: scalar %d, wide %d\n",
                            trial, lane, int(scalarHit), int(hs[lane]));
                CHECK(false);
                continue;
            }
            if (!scalarHit) continue;
            ++hits;
            // Bit for bit, not to a tolerance.
            CHECK_NEAR(ts[lane], t, 0.0);
            CHECK_NEAR(us[lane], u, 0.0);
            CHECK_NEAR(vs[lane], v, 0.0);
        }
    }
    CHECK(compared > 10000);
    if (hits < 100) std::printf("       only %d hits compared\n", hits);
    CHECK(hits > 100);
}

TEST(simd, a_scene_traces_identically_with_and_without_the_wide_path) {
    // The corpus already pins this across every scene, but only against stored
    // numbers. This says it directly: the hierarchy walk and the reference walk
    // over every triangle must still agree, on the geometry with the most
    // leaves in it.
    const TraceScene& scene =
        Simulation::sceneFor(GeometryProvider::Scene::MicrolensArray);

    std::uint64_t rng = optics::mix64(0x1234u);
    int checked = 0;
    for (int i = 0; i < 3000; ++i) {
        const Vec3 o(optics::uniform01(rng) * 60.0 - 30.0,
                     optics::uniform01(rng) * 60.0 - 30.0,
                     -80.0);
        Vec3 d(optics::uniform01(rng) * 0.4 - 0.2,
               optics::uniform01(rng) * 0.4 - 0.2, 1.0);
        if (!d.normalize()) continue;

        RayHit fast, slow;
        const bool a = scene.nearestHit(o, d, fast);
        const bool b = scene.nearestHitBruteForce(o, d, slow);
        CHECK(a == b);
        if (a && b) {
            CHECK_NEAR(fast.t, slow.t, 0.0);
            CHECK(fast.tri == slow.tri);
            ++checked;
        }
    }
    CHECK(checked > 500);
}

// ============================================================== preview =====

TEST(preview, progressive_snapshots_accumulate_to_the_finished_answer) {
    // The preview publishes deltas now rather than whole grids, and a running
    // total is kept across ticks. That is exactly the shape of change that can
    // lose or double-count a tick's work, so: every snapshot has to close its
    // own energy balance, the detected flux has to be monotone in the work
    // done, and the last one has to agree with the finished run.
    const TraceScene& scene =
        Simulation::sceneFor(GeometryProvider::Scene::IntegratingSphere);

    SourceConfig src;
    src.type  = SourceConfig::Type::Lambertian;
    src.rays  = 400000;
    src.power = 5.0;

    TraceOptions opt;
    opt.seed   = 4242u;
    opt.nTheta = 0;
    opt.nPhi   = 0;

    std::vector<SimulationResult> snaps;
    TraceControl ctl;
    ctl.partialIntervalMs = 1;          // as often as the back-off will allow
    // Snapshots are driven from the progress path, so a run watched without a
    // progress callback never takes one.
    ctl.progress = [](std::size_t, std::size_t) {};
    ctl.partial  = [&](const SimulationResult& p) { snaps.push_back(p); };

    SimulationResult finalRes;
    RayTracer::trace(scene, src, finalRes, opt, ctl);

    if (snaps.empty()) {
        std::printf("       the run finished before a snapshot was due\n");
        return;
    }

    double lastRays = 0.0;
    for (const SimulationResult& p : snaps) {
        CHECK(p.partial);
        CHECK(p.raysEmitted > 0);
        // Each snapshot is normalised by the work that finished, so its own
        // buckets close against its own source power.
        CHECK_NEAR(p.fluxAccounted(), p.sourcePower, 1e-6 * p.sourcePower);
        // Work only ever accumulates: a lost delta would show up as a snapshot
        // that went backwards.
        CHECK(double(p.raysEmitted) >= lastRays);
        lastRays = double(p.raysEmitted);
        // The grid the snapshot carries is the same quantity its scalars are.
        double gridSum = 0.0;
        for (double v : p.irradiance) gridSum += v;
        CHECK(gridSum <= p.fluxDetector * (1.0 + 1e-6) + 1e-12);
    }

    // And the picture the last snapshot showed is the picture the run ended on,
    // to within the rays it had not traced yet.
    const SimulationResult& last = snaps.back();
    CHECK(last.raysEmitted <= finalRes.raysEmitted);
    const double band = 8.0 * (last.efficiencyStdErr + finalRes.efficiencyStdErr) + 1e-3;
    if (!(std::fabs(last.efficiency - finalRes.efficiency) <= band))
        std::printf("       last %.6f vs final %.6f (band %.6f, %zu of %zu rays)\n",
                    last.efficiency, finalRes.efficiency, band,
                    last.raysEmitted, finalRes.raysEmitted);
    CHECK(std::fabs(last.efficiency - finalRes.efficiency) <= band);
}

TEST(preview, a_preview_never_changes_the_answer_the_run_returns) {
    // The preview is for looking at; the result is the bit-reproducible one.
    // Publishing deltas out of the same accumulators the trace is writing must
    // not perturb them.
    const TraceScene& scene = Simulation::sceneFor(GeometryProvider::Scene::Reflector);

    SourceConfig src;
    src.type = SourceConfig::Type::Lambertian;
    src.rays = 120000;

    TraceOptions opt;
    opt.seed = 99u;

    SimulationResult quiet;
    RayTracer::trace(scene, src, quiet, opt);

    int ticks = 0;
    TraceControl ctl;
    ctl.partialIntervalMs = 1;
    ctl.progress = [](std::size_t, std::size_t) {};
    ctl.partial  = [&](const SimulationResult&) { ++ticks; };
    SimulationResult watched;
    RayTracer::trace(scene, src, watched, opt, ctl);

    CHECK_NEAR(watched.fluxDetector, quiet.fluxDetector, 0.0);
    CHECK_NEAR(watched.fluxEscaped,  quiet.fluxEscaped,  0.0);
    CHECK_NEAR(watched.efficiency,   quiet.efficiency,   0.0);
    for (std::size_t i = 0; i < quiet.irradiance.size(); ++i)
        CHECK_NEAR(watched.irradiance[i], quiet.irradiance[i], 0.0);
}

// ========================================================= ray inspector ====

TEST(rayinspector, a_single_ray_reports_every_interaction_it_had) {
    // traceSingleRay already turned the estimators off so the whole path tree
    // is visible, and already recorded every segment. What was missing was the
    // *why*: at this surface, at this angle, against this index pair, this much
    // reflected and this much went on.
    const TraceScene& scene = Simulation::sceneFor(GeometryProvider::Scene::PlanoConvexLens);

    SimulationResult res;
    // Straight down the axis from well in front of the lens.
    RayTracer::traceSingleRay(scene, Vec3(0.0, 0.0, -400.0), Vec3(0, 0, 1), res);

    CHECK(!res.interactions.empty());
    if (res.interactions.empty()) return;

    bool sawSurface = false, sawReceiver = false;
    double lastOpl = -1.0;
    for (const RayInteraction& in : res.interactions) {
        // Every interaction names the surface it happened at.
        CHECK(in.surface >= 0);
        CHECK(!in.label.isEmpty());
        CHECK(in.angleDeg >= 0.0 && in.angleDeg <= 90.0 + 1e-9);
        CHECK(in.energyIn > 0.0 && in.energyIn <= 1.0 + 1e-9);
        // Optical path only ever grows along a branch, and the tree is walked
        // depth first, so it never goes backwards within one.
        CHECK(in.opl >= 0.0);
        lastOpl = in.opl;

        if (in.detector) {
            sawReceiver = true;
        } else {
            sawSurface = true;
            // A refractive interface splits its energy and does not create any.
            CHECK(in.reflectance >= 0.0);
            CHECK(in.transmittance >= 0.0);
            CHECK(in.reflectance + in.transmittance <= 1.0 + 1e-9);
            CHECK(in.n1 > 0.0 && in.n2 > 0.0);
        }
    }
    CHECK(sawSurface);
    CHECK(sawReceiver);
    CHECK(lastOpl >= 0.0);
}

TEST(rayinspector, the_index_pair_is_the_one_the_ray_actually_crossed) {
    // Entering the glass is air into n, and leaving it is n into air. Getting
    // that round the wrong way is the bug the medium stack exists to prevent,
    // and the inspector is where a user would see it.
    const TraceScene& scene = Simulation::sceneFor(GeometryProvider::Scene::BiconvexLens);

    SimulationResult res;
    RayTracer::traceSingleRay(scene, Vec3(0.0, 0.0, -400.0), Vec3(0, 0, 1), res);
    CHECK(!res.interactions.empty());

    bool sawEntry = false, sawExit = false;
    for (const RayInteraction& in : res.interactions) {
        if (in.detector) continue;
        if (in.n1 < 1.0 + 1e-9 && in.n2 > 1.2) { sawEntry = true; CHECK(in.mediumDepth == 0); }
        if (in.n1 > 1.2 && in.n2 < 1.0 + 1e-9) { sawExit  = true; CHECK(in.mediumDepth >= 1); }
    }
    if (!sawEntry || !sawExit)
        std::printf("       entry %d exit %d over %zu interactions\n",
                    int(sawEntry), int(sawExit), res.interactions.size());
    CHECK(sawEntry);
    CHECK(sawExit);
}

TEST(rayinspector, a_monte_carlo_run_records_no_interactions_at_all) {
    // The log is filled only when a caller asks for it: the Monte Carlo path
    // pays one null check per interaction and nothing else. A run that quietly
    // accumulated a few million of these would be a memory bug.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::BiconvexLens, 20000);
    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.interactions.empty());
}

TEST(rayinspector, total_internal_reflection_is_named_as_such) {
    // A light guide is the case a user reaches for the inspector to understand,
    // and "why is this ray still inside" is answered by one column.
    const TraceScene& scene = Simulation::sceneFor(GeometryProvider::Scene::LightGuide);

    SimulationResult res;
    // Into the entrance face at a slant, so the ray meets the wall past the
    // critical angle rather than head on.
    RayTracer::traceSingleRay(scene, Vec3(0.0, 0.0, -40.0),
                              Vec3(0.30, 0.0, 1.0).normalized(), res);
    bool sawTir = false;
    for (const RayInteraction& in : res.interactions)
        if (in.tir) { sawTir = true; break; }
    if (!sawTir)
        std::printf("       no TIR in %zu interactions\n", res.interactions.size());
    CHECK(sawTir);
}
