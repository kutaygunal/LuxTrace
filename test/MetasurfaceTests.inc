// A surface that does not obey Snell's law.
//
// Everything else optical in this application follows from one boundary
// condition -- the tangential wavevector is continuous -- and a metasurface
// breaks it on purpose by adding a designed, position-dependent phase. The
// generalised Snell law is what comes out:
//
//     n_t sin(theta_t) - n_i sin(theta_i) = (lambda / 2 pi) dPhi/dx
//
// These tests are what keep that from being a plausible-looking approximation.
// Three of them are closed forms nobody has to trace to know -- the grating
// equation, the focal length a designed phase profile encodes, and how that
// focal length moves with wavelength -- and one is the property that matters
// most for a new law added to a working tracer: with the gradient switched off,
// the new code path has to give back the old answer bit for bit.

namespace {

// A flat glass wafer from z = 0 to z = `thick`, which is all the geometry a
// metalens has. The focusing is in the surface property and not in a shape.
MeshSurface wafer(double halfSize, double thick) {
    MeshSurface m;
    m.label = QStringLiteral("Wafer");
    const double h = halfSize;
    m.verts = {gp_Pnt(-h, -h, 0),      gp_Pnt(h, -h, 0),      gp_Pnt(h, h, 0),
               gp_Pnt(-h, h, 0),       gp_Pnt(-h, -h, thick), gp_Pnt(h, -h, thick),
               gp_Pnt(h, h, thick),    gp_Pnt(-h, h, thick)};
    auto tri = [&](int a, int b, int c, const gp_Dir& n) {
        Triangle t; t.v0 = a; t.v1 = b; t.v2 = c; t.normal = n;
        m.tris.push_back(t);
    };
    tri(0, 2, 1, gp_Dir(0, 0, -1));   tri(0, 3, 2, gp_Dir(0, 0, -1));   // entry
    tri(4, 5, 6, gp_Dir(0, 0, 1));    tri(4, 6, 7, gp_Dir(0, 0, 1));    // exit
    // The rim, so the solid is closed and the medium bookkeeping has an inside.
    tri(0, 1, 5, gp_Dir(0, -1, 0));   tri(0, 5, 4, gp_Dir(0, -1, 0));
    tri(1, 2, 6, gp_Dir(1, 0, 0));    tri(1, 6, 5, gp_Dir(1, 0, 0));
    tri(2, 3, 7, gp_Dir(0, 1, 0));    tri(2, 7, 6, gp_Dir(0, 1, 0));
    tri(3, 0, 4, gp_Dir(-1, 0, 0));   tri(3, 4, 7, gp_Dir(-1, 0, 0));
    m.index          = 1.5;
    m.transmissivity = 1.0;
    m.fresnel        = true;
    return m;
}

// Where one ray lands on a receiver `zDet` millimetres along the axis, and the
// direction it arrived along.
//
// A receiver rather than the recorded path: traceSingleRay follows both branches
// of every interface, so a wafer produces two Fresnel reflections and a leg
// inside the glass beside the one that went through, and picking the right leg
// out of that list is guesswork. An arrival is the thing the tracer exists to
// produce, and the transmitted branch is the one carrying almost all of it.
bool arrivalOf(const MeshSurface& optic, double zDet, const Vec3& from,
               const Vec3& dir, double lambdaNm, Vec3& point, Vec3& arrivedAlong) {
    MeshList meshes{optic, detectorSurface(4000.0, zDet, 4)};
    TraceScene scene;
    scene.build(meshes);

    SimulationResult res;
    PhysicsOptions phys;
    RayTracer::traceSingleRay(scene, from, dir, res, 1.0, phys, lambdaNm);

    double best = -1.0;
    for (const DetectorArrival& a : res.arrivals) {
        if (double(a.energy) <= best) continue;
        best = double(a.energy);
        point = a.p;
        arrivedAlong = a.d;
    }
    return best > 0.0;
}

} // namespace

TEST(metasurface, a_zero_gradient_traces_bit_identically_to_the_glass_it_replaced) {
    // The test that keeps this feature from perturbing the twenty-eight scenes
    // that do not use it.
    //
    // The ordinary law is the generalised one with dPhi/dx == 0, so a
    // metasurface with no gradient is not *nearly* a plain refracting surface,
    // it is one. That has to hold to the last bit and not to a tolerance:
    // anything less means the new code path rounds differently, and a feature
    // nobody switched on would move every existing answer in its last digits.
    MeshList plain{wafer(60.0, 4.0), detectorSurface(400.0, 300.0, 32)};
    MeshList patterned = plain;
    patterned.front().metasurface.profile       = meta::Profile::Linear;
    patterned.front().metasurface.gradientPerMm = 0.0;      // no gradient at all
    patterned.front().metasurface.setIdealOrder(0, 1.0);    // and nothing lost

    TraceScene sceneA, sceneB;
    sceneA.build(plain);
    sceneB.build(patterned);
    CHECK(sceneB.surfaces().front().metasurface.active());

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 30.0;                    // a spread of incidence angles
    src.rays         = 200000;
    src.origin       = gp_Pnt(0, 0, -200);
    src.axis         = gp_Dir(0, 0, 1);

    TraceOptions opt;
    opt.seed = 11111;
    SimulationResult a, b;
    RayTracer::trace(sceneA, std::vector<SourceConfig>{src}, a, opt);
    RayTracer::trace(sceneB, std::vector<SourceConfig>{src}, b, opt);

    CHECK(a.raysEmitted == b.raysEmitted);
    CHECK(a.fluxDetector == b.fluxDetector);    // not CHECK_NEAR, deliberately
    CHECK(a.fluxAbsorbed == b.fluxAbsorbed);
    CHECK(a.fluxEscaped  == b.fluxEscaped);
    CHECK(a.efficiency   == b.efficiency);
    bool sameGrid = a.irradiance.size() == b.irradiance.size();
    for (std::size_t i = 0; i < a.irradiance.size() && sameGrid; ++i)
        if (a.irradiance[i] != b.irradiance[i]) sameGrid = false;
    CHECK(sameGrid);
}

TEST(metasurface, a_linear_gradient_obeys_the_grating_equation) {
    // The closed form the whole feature is written from, at normal incidence
    // where it reduces to sin(theta) = m lambda / Lambda for a grating of period
    // Lambda -- because a constant phase gradient of 2 pi / Lambda is what a
    // blazed grating of that period is.
    constexpr double kPeriodMm = 0.02;           // a 50 line/mm grating
    MeshSurface w = wafer(60.0, 4.0);
    w.metasurface.profile        = meta::Profile::Linear;
    w.metasurface.gradientPerMm  = optics::kTwoPi / kPeriodMm;
    w.metasurface.axis           = Vec3(1, 0, 0);
    w.metasurface.designLambdaNm = 550.0;

    for (int m : {1, 2, -1}) {
        MeshSurface g = w;
        g.metasurface.setIdealOrder(m, 1.0);
        for (double lambda : {450.0, 550.0, 650.0}) {
            Vec3 p, d;
            CHECK(arrivalOf(g, 1000.0, Vec3(0, 0, -50), Vec3(0, 0, 1), lambda, p, d));
            CHECK(d.z > 0.0);                    // still going forwards
            // sin(theta) = m lambda / Lambda, in the plane the gradient runs in.
            const double expected = double(m) * (lambda * 1e-6) / kPeriodMm;
            CHECK_NEAR(d.x, expected, 1e-9);
            CHECK_NEAR(d.y, 0.0, 1e-12);
        }
    }
}

TEST(metasurface, a_metalens_focuses_at_the_length_it_was_designed_for) {
    // A designed phase profile, checked against the geometry it encodes rather
    // than against another trace.
    //
    // Phi(r) = -(2 pi / lambda_d)(sqrt(r^2 + f^2) - f) is the phase that turns a
    // plane wave into one converging on a point f away, so a ray entering at
    // radius r has to leave along a direction that reaches the axis after
    // exactly f. Every radius has to agree on the same f, which is what makes
    // this a focus and not a smear -- so the test walks four of them, out to
    // three quarters of the aperture, where a spherical singlet would already
    // have fallen apart.
    constexpr double kFocal = 150.0;
    constexpr double kFace  = 4.0;               // the patterned face
    MeshSurface w = wafer(60.0, kFace);
    w.metasurface.profile        = meta::Profile::Metalens;
    w.metasurface.focalLengthMm  = kFocal;
    w.metasurface.designLambdaNm = 550.0;
    w.metasurface.centre         = Vec3(0, 0, kFace);
    w.metasurface.setIdealOrder(1, 1.0);

    for (double r : {5.0, 15.0, 30.0, 45.0}) {
        Vec3 p, d;
        CHECK(arrivalOf(w, kFace + kFocal, Vec3(r, 0, -50), Vec3(0, 0, 1),
                        550.0, p, d));
        CHECK(d.z > 0.0);
        // A receiver at the design focal length: every ray, whatever radius it
        // entered at, lands on the axis.
        CHECK(std::fabs(p.x) < 1e-6 * kFocal);
        CHECK(std::fabs(p.y) < 1e-9);
    }

    // And it really is a focus rather than a plane the rays happen to cross:
    // half a focal length short, the outermost ray is still half way out.
    Vec3 p, d;
    CHECK(arrivalOf(w, kFace + 0.5 * kFocal, Vec3(45.0, 0, -50), Vec3(0, 0, 1),
                    550.0, p, d));
    CHECK_NEAR(p.x, 22.5, 1e-4);
}

TEST(metasurface, its_chromatic_dispersion_runs_the_other_way_and_is_far_larger) {
    // The signature of a phase-gradient lens, and the one a monochromatic test
    // cannot see at all.
    //
    // The deflection is proportional to wavelength, so the focal length goes as
    // lambda_design / lambda: red focuses *nearer* and blue *further*. A
    // refractive lens does the exact opposite, because its power comes from
    // n(lambda) - 1 and n falls with wavelength. Getting the sign backwards
    // gives a lens that looks entirely reasonable and is wrong about the one
    // thing meta-optics is difficult for.
    constexpr double kFocal = 150.0;
    constexpr double kFace  = 4.0;
    constexpr double kR     = 20.0;
    MeshSurface w = wafer(60.0, kFace);
    w.metasurface.profile        = meta::Profile::Metalens;
    w.metasurface.focalLengthMm  = kFocal;
    w.metasurface.designLambdaNm = 550.0;
    w.metasurface.centre         = Vec3(0, 0, kFace);
    w.metasurface.setIdealOrder(1, 1.0);

    // Where a ray entering at radius kR crosses the axis, from the direction it
    // left along.
    const auto focusAt = [&](double lambda) {
        Vec3 p, d;
        if (!arrivalOf(w, 1000.0, Vec3(kR, 0, -50), Vec3(0, 0, 1), lambda, p, d))
            return 0.0;
        return d.x < 0.0 ? -kR * d.z / d.x : 0.0;
    };
    const double blue  = focusAt(450.0);
    const double green = focusAt(550.0);
    const double red   = focusAt(650.0);

    // The ordering first, because a sign error is the failure that matters.
    CHECK(blue > green);
    CHECK(green > red);
    // And the magnitude. Away from the design wavelength the ray entering at
    // radius r crosses the axis at sqrt((r^2 + f^2) / k^2 - r^2), with
    // k = lambda / lambda_d -- which is f lambda_d / lambda near the axis and
    // departs from it further out. That departure is the metalens's own
    // spherical aberration off design, and it is real physics rather than
    // slack: the paraxial formula is half a millimetre out at this radius, so
    // asserting it to a tolerance that admits the difference would be asserting
    // the wrong thing.
    const auto predicted = [&](double lambda) {
        const double k = lambda / 550.0;
        return std::sqrt((kR * kR + kFocal * kFocal) / (k * k) - kR * kR);
    };
    CHECK_NEAR(green, kFocal, 1e-6 * kFocal);
    CHECK_NEAR(blue,  predicted(450.0), 1e-6 * kFocal);
    CHECK_NEAR(red,   predicted(650.0), 1e-6 * kFocal);
    // Which is inside a per cent of the paraxial f lambda_d / lambda either way,
    // so that relation is still the one a reader should carry away.
    CHECK(std::fabs(blue - kFocal * 550.0 / 450.0) < 0.01 * kFocal);
    CHECK(std::fabs(red  - kFocal * 550.0 / 650.0) < 0.01 * kFocal);
    // Nearly forty per cent across the visible band, where a crown-glass singlet
    // moves by about two and in the other direction.
    CHECK((blue - red) / green > 0.30);
}

TEST(metasurface, a_pattern_lives_on_one_face_and_not_on_the_glass_behind_it) {
    // A metasurface is a film on one face of a wafer. A surface property belongs
    // to every face of the solid that carries it, so without saying which face
    // is patterned a metalens deflects on the way in and again on the way out
    // and focuses at half the length it was designed for.
    //
    // That is not hypothetical: it is what the first version of this did, and
    // what a focus sweep found. The default is the leaving face, which is how a
    // transmissive metalens is built; Both is a metasurface doublet, and it is
    // available rather than assumed.
    constexpr double kFocal = 150.0;
    constexpr double kFace  = 4.0;
    constexpr double kR     = 20.0;
    const auto crossingFor = [&](meta::Face face) {
        MeshSurface w = wafer(60.0, kFace);
        w.metasurface.profile        = meta::Profile::Metalens;
        w.metasurface.face           = face;
        w.metasurface.focalLengthMm  = kFocal;
        w.metasurface.designLambdaNm = 550.0;
        w.metasurface.centre         = Vec3(0, 0, kFace);
        w.metasurface.setIdealOrder(1, 1.0);
        Vec3 p, d;
        if (!arrivalOf(w, 1000.0, Vec3(kR, 0, -50), Vec3(0, 0, 1), 550.0, p, d))
            return 0.0;
        return d.x < 0.0 ? -kR * d.z / d.x : 0.0;
    };

    CHECK_NEAR(crossingFor(meta::Face::Leaving), kFocal, 1e-6 * kFocal);
    // Patterned on both faces, the same wafer focuses at about half as far --
    // twice the phase gradient is twice the power.
    const double both = crossingFor(meta::Face::Both);
    CHECK(both > 0.0);
    CHECK(both < 0.62 * kFocal);
}

TEST(metasurface, the_energy_closes_across_every_order_and_the_remainder) {
    // A grating splits its light between orders and loses the rest, and the
    // budget has to hold whatever the split is.
    //
    // What makes this worth asserting rather than assuming: the orders are
    // sampled, so the branch that leaves carries the *total* efficiency and not
    // its own order's, and what no propagating order took is absorbed at the
    // surface. An implementation that scaled by the sampled order's efficiency
    // instead would be dark by the ratio, and one that forgot the remainder
    // would gain light out of nothing -- and both would still produce a
    // perfectly reasonable-looking picture.
    MeshSurface w = wafer(60.0, 4.0);
    w.metasurface.profile        = meta::Profile::Linear;
    w.metasurface.gradientPerMm  = optics::kTwoPi / 0.03;
    w.metasurface.designLambdaNm = 550.0;
    // A real device: most of the light in the design order, some in the
    // neighbours, and a fifth of it nowhere at all.
    w.metasurface.efficiency.flat[1 + meta::kMaxOrder]  = 0.55;
    w.metasurface.efficiency.flat[0 + meta::kMaxOrder]  = 0.15;
    w.metasurface.efficiency.flat[2 + meta::kMaxOrder]  = 0.07;
    w.metasurface.efficiency.flat[-1 + meta::kMaxOrder] = 0.03;
    for (int i = 0; i < meta::kOrderSlots; ++i)
        w.metasurface.efficiency.flatP[i] = w.metasurface.efficiency.flat[i];
    CHECK_NEAR(w.metasurface.efficiency.total(550.0), 0.80, 1e-12);

    MeshList meshes{w, detectorSurface(2000.0, 400.0, 16)};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 25.0;
    src.rays         = 400000;
    src.origin       = gp_Pnt(0, 0, -200);
    src.axis         = gp_Dir(0, 0, 1);

    TraceOptions opt;
    opt.seed = 11111;
    SimulationResult res;
    RayTracer::trace(scene, std::vector<SourceConfig>{src}, res, opt);
    CHECK(res.raysEmitted > 0);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9 * res.sourcePower);
    // And the surface really did eat the twenty per cent no order took: without
    // it the absorbed channel would hold only the Fresnel and bulk terms, which
    // are a few per cent between them here.
    CHECK(res.fluxAbsorbed > 0.10 * res.sourcePower);
}

TEST(metasurface, an_order_that_cannot_propagate_is_booked_rather_than_invented) {
    // A grating fine enough asks for a tangential wavevector larger than the
    // outgoing medium can carry. That order is evanescent: it has no propagating
    // direction and it does not exist at that angle.
    //
    // The temptation is to clamp it to grazing, which produces a ray, keeps the
    // energy and is wrong. What has to happen is that the light it was carrying
    // is booked as absorbed and the budget still closes.
    MeshSurface w = wafer(60.0, 4.0);
    w.metasurface.profile        = meta::Profile::Linear;
    // A period of 300 nm against 550 nm light: sin(theta) would have to be 1.83.
    w.metasurface.gradientPerMm  = optics::kTwoPi / 0.0003;
    w.metasurface.designLambdaNm = 550.0;
    w.metasurface.setIdealOrder(1, 1.0);

    MeshList meshes{w, detectorSurface(2000.0, 400.0, 8)};
    TraceScene scene;
    scene.build(meshes);

    SourceConfig src;
    src.type         = SourceConfig::Type::Lambertian;
    src.halfAngleDeg = 10.0;
    src.rays         = 100000;
    src.origin       = gp_Pnt(0, 0, -200);
    src.axis         = gp_Dir(0, 0, 1);

    TraceOptions opt;
    opt.seed = 11111;
    SimulationResult res;
    RayTracer::trace(scene, std::vector<SourceConfig>{src}, res, opt);
    // Nothing that crossed the wafer reached the receiver: the only order the
    // device has does not propagate.
    CHECK(res.efficiency < 0.02);
    CHECK(res.fluxAbsorbed > 0.5 * res.sourcePower);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9 * res.sourcePower);
}

TEST(metasurface, the_preview_backend_refuses_it_by_name) {
    // The rule the whole preview arrangement rests on, applied to the newest
    // physics in the tracer. The kernel obeys Snell's law; a metasurface does
    // not. Tracing one there would give the answer for the glass wafer instead
    // of for the device printed on it -- a plausible number for a scene whose
    // entire point is that it is not that number.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Metalens, 2000);
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const std::vector<SourceConfig> srcs = Simulation::sourcesFor(cfg, *data);
    TraceOptions opt;
    opt.physics = cfg.physics;

    const gputrace::Support sup =
        gputrace::supports(data->scene, data->scene.surfaces(), srcs, opt);
    CHECK(!sup.ok);
    CHECK(sup.reasons.contains(QStringLiteral("a metasurface")));

    SimulationResult res;
    QString err;
    CHECK(!gputrace::trace(data->scene, data->scene.surfaces(), srcs, opt,
                           cfg.power, cfg.fluxUnit, res, &err));
    CHECK(err.contains(QStringLiteral("a metasurface")));
    CHECK(res.irradiance.empty());
}

TEST(metafile, a_vendor_table_is_read_and_reduced_to_what_a_tracer_needs) {
    // Meta-optic data arrives as a table from an EM solve: per wavelength, per
    // order, per polarisation, usually as complex transmission coefficients. A
    // ray tracer needs the modulus squared, so the reader reduces it -- and the
    // point of the test is that it reduces the *right* thing and interpolates
    // between the wavelengths it was given rather than past them.
    const QString text = QStringLiteral(
        "# an RCWA export, reduced\n"
        "design_wavelength_nm 550\n"
        "period_um            0.42\n"
        "\n"
        "# lambda  order  eff_s  eff_p\n"
        "450       0      0.10   0.10\n"
        "450       1      0.70   0.70\n"
        "550       0      0.06   0.06\n"
        "550       1      0.90   0.90\n"
        "650       0      0.12   0.12\n"
        "650       1      0.74   0.74\n");
    const metafile::Result r = metafile::parse(text);
    CHECK(r.ok);
    CHECK(r.error.isEmpty());
    CHECK(r.wavelengths == 3);
    CHECK(r.orders == 1);
    CHECK_NEAR(r.designLambdaNm, 550.0, 1e-12);
    CHECK_NEAR(r.periodMm, 0.00042, 1e-12);
    CHECK(!r.polarising);
    // The summary is shown to somebody, so it has to say what was loaded.
    CHECK(r.summary.contains(QStringLiteral("3 wavelength")));
    CHECK(r.summary.contains(QStringLiteral("550")));

    double es = 0.0, ep = 0.0;
    r.efficiency.at(1, 550.0, es, ep);
    CHECK_NEAR(es, 0.90, 1e-12);
    CHECK_NEAR(ep, 0.90, 1e-12);
    // Between samples, linearly.
    r.efficiency.at(1, 500.0, es, ep);
    CHECK_NEAR(es, 0.80, 1e-12);
    // And flat outside the measured band rather than extrapolated: past the
    // wavelengths an EM solve covered, the honest answer is the last one it gave.
    r.efficiency.at(1, 300.0, es, ep);
    CHECK_NEAR(es, 0.70, 1e-12);
    r.efficiency.at(1, 900.0, es, ep);
    CHECK_NEAR(es, 0.74, 1e-12);
    // An order the file never mentioned carries nothing.
    r.efficiency.at(3, 550.0, es, ep);
    CHECK_NEAR(es, 0.0, 1e-12);
}

TEST(metafile, complex_coefficients_are_reduced_to_moduli_squared) {
    // What an RCWA or FDTD export actually contains. The tracer needs |t|^2 and
    // the reader is where that reduction happens -- once, on load, rather than
    // per hit.
    const QString text = QStringLiteral(
        "# lambda order re_s im_s re_p im_p\n"
        "550 1 0.6 0.5 0.2 0.1\n");
    const metafile::Result r = metafile::parse(text);
    CHECK(r.ok);
    double es = 0.0, ep = 0.0;
    r.efficiency.at(1, 550.0, es, ep);
    CHECK_NEAR(es, 0.6 * 0.6 + 0.5 * 0.5, 1e-12);
    CHECK_NEAR(ep, 0.2 * 0.2 + 0.1 * 0.1, 1e-12);
    CHECK(r.polarising);
    CHECK(r.efficiency.polarising);
}

TEST(metafile, it_degrades_loudly_rather_than_silently) {
    // The property the whole import side is written for. A metasurface read as
    // ninety per cent efficient because a column was misread is exactly the
    // plausible wrong number this application refuses to produce, so every way
    // of misreading one has to come back as a refusal with a reason.
    struct Case { const char* text; const char* expect; };
    const Case cases[] = {
        {"nonsense_key 4\n550 1 0.9\n",                 "unknown key"},
        {"550 1\n",                                     "at least one efficiency"},
        {"550 banana 0.9\n",                            "not a diffraction order"},
        {"550 1 1.4\n",                                 "not a fraction"},
        // Orders that sum past the incident power: the wrong columns, not a
        // rounding problem.
        {"550 0 0.6\n550 1 0.7\n",                      "more power than arrived"},
        {"# nothing but comments\n",                    "no efficiency rows"},
    };
    for (const Case& c : cases) {
        const metafile::Result r = metafile::parse(QString::fromLatin1(c.text));
        CHECK(!r.ok);
        CHECK(r.error.contains(QLatin1String(c.expect)));
        CHECK(r.efficiency.samples == 0);
    }

    // An order past what the surface carries is understood, dropped and
    // *counted* -- the file was not wrong, it just said more than this can hold.
    const metafile::Result deep = metafile::parse(
        QStringLiteral("550 0 0.5\n550 9 0.2\n"));
    CHECK(deep.ok);
    CHECK(deep.skipped == 1);
    CHECK(deep.summary.contains(QStringLiteral("not kept")));
}

TEST(metasurface, a_polarising_device_splits_by_the_state_and_not_by_the_average) {
    // A metasurface can diffract s and p by different amounts -- that is what a
    // polarisation-sensitive meta-grating is for -- and on a polarised run the
    // share that leaves has to follow the *state* rather than the unpolarised
    // average of the two efficiencies.
    //
    // So: a device that passes s and blocks p, met by s-polarised light and then
    // by p-polarised light. Averaging would send half through either way; the
    // right answer is nearly all and nearly none.
    MeshSurface w = wafer(60.0, 4.0);
    w.metasurface.profile        = meta::Profile::Linear;
    w.metasurface.gradientPerMm  = optics::kTwoPi / 0.05;
    w.metasurface.designLambdaNm = 550.0;
    w.metasurface.efficiency.flat[1 + meta::kMaxOrder]  = 0.96;   // s
    w.metasurface.efficiency.flatP[1 + meta::kMaxOrder] = 0.02;   // p
    w.metasurface.efficiency.polarising = true;

    MeshList meshes{w, detectorSurface(4000.0, 600.0, 8)};
    TraceScene scene;
    scene.build(meshes);

    const auto efficiencyFor = [&](int polState) {
        SourceConfig src;
        src.type              = SourceConfig::Type::Lambertian;
        src.halfAngleDeg      = 4.0;
        src.rays              = 200000;
        src.origin            = gp_Pnt(0, 0, -200);
        src.axis              = gp_Dir(0, 0, 1);
        src.polarisationState = polState;       // 1 = linear s, 2 = linear p
        TraceOptions opt;
        opt.seed = 11111;
        opt.physics.polarised = true;
        SimulationResult res;
        RayTracer::trace(scene, std::vector<SourceConfig>{src}, res, opt);
        return res.efficiency;
    };
    const double sPol = efficiencyFor(1);
    const double pPol = efficiencyFor(2);

    CHECK(sPol > 0.5);
    CHECK(pPol < 0.1);
    // Which is the point: an unpolarised average would have given the same
    // number for both, and about half way between these.
    CHECK(sPol > 5.0 * pPol);
}
