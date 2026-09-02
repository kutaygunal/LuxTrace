// Is the instrument calibrated?
//
// The whole case for a fast preview backend rests on a comparison that can tell
// "a different draw of the same answer" from "a different answer". So the
// comparison itself has to be tested against both, and it has to be tested
// without a GPU anywhere near it -- two CPU runs on different seeds are exactly
// the situation a GPU preview will be in, minus the hardware.

TEST(backendcheck, two_draws_of_the_same_run_agree) {
    // Different seeds, same everything else. This is the pass case, and it has
    // to pass comfortably rather than barely: a comparison that only just
    // accepts two draws of one distribution has no room left to accept a real
    // preview.
    for (int sc : {int(GeometryProvider::Scene::Reflector),
                   int(GeometryProvider::Scene::HalfBallLens),
                   int(GeometryProvider::Scene::IntegratingSphere)}) {
        SimConfig a = quickConfig(GeometryProvider::Scene(sc), 60000);
        a.seed = 11111;
        a.noiseMap = true;
        SimConfig b = a;
        b.seed = 99999;

        const backendcheck::Report r =
            backendcheck::compare(Simulation::run(a), Simulation::run(b));
        CHECK(r.blocker.isEmpty());
        CHECK(r.ok);
        // The receiver map should sit near its own noise floor, not merely
        // inside the tolerance.
        for (const backendcheck::MapComparison& m : r.maps)
            if (m.comparable && m.name == QStringLiteral("irradiance"))
                CHECK(m.ratio < 2.0);
    }
}

TEST(backendcheck, an_identical_result_is_exactly_zero_apart) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::HalfBallLens, 20000);
    cfg.noiseMap = true;
    const SimulationResult one = Simulation::run(cfg);
    const backendcheck::Report r = backendcheck::compare(one, one);
    CHECK(r.ok);
    for (const backendcheck::Comparison& c : r.scalars) CHECK_NEAR(c.sigma, 0.0, 1e-12);
    for (const backendcheck::MapComparison& m : r.maps)
        if (m.comparable) CHECK_NEAR(m.distance, 0.0, 1e-12);
}

TEST(backendcheck, a_different_optic_does_not_pass) {
    // The fail case. If this ever passes, the comparison is decorative: a
    // preview could be modelling the wrong physics and be waved through.
    SimConfig a = quickConfig(GeometryProvider::Scene::HalfBallLens, 60000);
    a.noiseMap = true;
    SimConfig b = a;
    b.useSceneDefaults = false;
    b.params = GeometryProvider::defaultParams(GeometryProvider::Scene::HalfBallLens);
    b.params.v[0] *= 0.6;            // a visibly smaller dome
    b.seed = 99999;

    const backendcheck::Report r =
        backendcheck::compare(Simulation::run(a), Simulation::run(b));
    CHECK(!r.ok);
}

TEST(backendcheck, dropping_a_physical_effect_does_not_pass) {
    // The failure mode a growing GPU backend actually has: the geometry is
    // right, the rays are right, and one term of the physics is missing.
    SimConfig a = quickConfig(GeometryProvider::Scene::LightGuide, 60000);
    a.noiseMap = true;
    a.physics.absorption = true;
    SimConfig b = a;
    b.physics.absorption = false;     // the preview "hasn't got round to" it
    b.seed = 99999;

    const backendcheck::Report r =
        backendcheck::compare(Simulation::run(a), Simulation::run(b));
    CHECK(!r.ok);
}

TEST(backendcheck, the_measured_floor_beats_a_count_model_where_it_matters) {
    // The integrating sphere is the case that forced this. Next-event
    // estimation connects nearly every diffuse bounce to the receiver, so a bin
    // holds hundreds of arrivals and is still far noisier than their count
    // suggests. A floor derived from ray counts called two correct runs five
    // times too far apart; a floor derived from the runs' own per-bin variance
    // predicts the distance they actually show.
    SimConfig a = quickConfig(GeometryProvider::Scene::IntegratingSphere, 60000);
    a.noiseMap = true;
    a.seed = 11111;
    SimConfig b = a;
    b.seed = 99999;

    const SimulationResult ra = Simulation::run(a);
    const SimulationResult rb = Simulation::run(b);
    CHECK(!ra.irradianceVar.empty());

    const double dist  = backendcheck::totalVariation(ra.irradiance, rb.irradiance);
    const double bad   = backendcheck::noiseFloorFor(ra.irradiance, rb.irradiance,
                                                     ra.raysHitDetector, rb.raysHitDetector);
    const double good  = backendcheck::measuredNoiseFloor(ra.irradiance, ra.irradianceVar,
                                                          rb.irradiance, rb.irradianceVar);
    CHECK(bad > 0.0);
    CHECK(good > 0.0);
    // The count model is out by more than a factor of three; the measured one
    // lands within a factor of two of the distance it is predicting.
    CHECK(dist / bad > 3.0);
    CHECK(dist / good > 0.5);
    CHECK(dist / good < 2.0);
}

TEST(backendcheck, the_noise_floor_shrinks_as_the_root_of_the_sample) {
    // The floor is what separates sampling from disagreement, so its scaling
    // has to be right: four times the rays, half the floor.
    std::vector<double> a(400, 0.0), b(400, 0.0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = 1.0 + 0.001 * double(i);
        b[i] = 1.0 + 0.001 * double(i);
    }
    const double f1 = backendcheck::noiseFloorFor(a, b, 10000, 10000);
    const double f4 = backendcheck::noiseFloorFor(a, b, 40000, 40000);
    CHECK(f1 > 0.0);
    CHECK_NEAR(f4 / f1, 0.5, 1e-9);
}

TEST(backendcheck, total_variation_is_a_fraction_of_the_light_that_moved) {
    // Two distributions sharing no support are one apart; identical ones are
    // zero apart; and moving a tenth of the light moves the distance a tenth.
    std::vector<double> a{1.0, 0.0}, b{0.0, 1.0};
    CHECK_NEAR(backendcheck::totalVariation(a, b), 1.0, 1e-12);
    CHECK_NEAR(backendcheck::totalVariation(a, a), 0.0, 1e-12);
    std::vector<double> c{0.9, 0.1};
    CHECK_NEAR(backendcheck::totalVariation(a, c), 0.1, 1e-12);
}

TEST(backendcheck, incomparable_runs_are_refused_rather_than_scored) {
    // A verdict on two runs that were never measuring the same thing is worse
    // than no verdict, because it reads as one.
    SimConfig a = quickConfig(GeometryProvider::Scene::HalfBallLens, 8000);
    a.detectorBins = 64;
    SimConfig b = a;
    b.detectorBins = 128;

    const backendcheck::Report r =
        backendcheck::compare(Simulation::run(a), Simulation::run(b));
    CHECK(!r.blocker.isEmpty());
    CHECK(!r.ok);
}
