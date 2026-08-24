// Tests for the physics, analysis and workflow added on top of the original
// tracer. Included by TestMain.cpp, which owns the harness and the shared
// helpers; keeping them in their own file stops one 2 000-line translation unit
// from being the only place a test can live.

// ============================================================== helpers =====

namespace {

// A planar glass half-space with the Fresnel model switched on, so an angle of
// incidence produces the reflectance the equations say it should.
MeshSurface fresnelPlane(double index, double halfSize = 400.0, double absorption = 0.0) {
    MeshSurface m = glassPlane(index, halfSize);
    m.fresnel    = true;
    m.absorption = absorption;
    return m;
}

// Sends one ray straight down +Z through a glass plane at z = 0 onto a receiver
// at z = D, and returns the fraction that arrived. At normal incidence nothing
// bends, so the answer is purely the surface and bulk terms.
double straightThroughGlass(const MeshSurface& glass, double D,
                            const PhysicsOptions& physics = PhysicsOptions{}) {
    MeshList meshes{glass, detectorSurface(2000.0, D)};
    TraceScene scene;
    scene.build(meshes);
    SimulationResult res;
    RayTracer::traceSingleRay(scene, Vec3(0, 0, -10), Vec3(0, 0, 1), res, 1.0, physics);
    return res.fluxDetector;
}

// A result with a known irradiance distribution, so a metric can be checked
// against arithmetic instead of against another Monte Carlo run.
//
// The grid holds flux per bin, so a disc of constant irradiance means a bin
// value proportional to the bin area. Filling every lit bin with the same number
// instead would describe a disc whose brightness depends on the binning, which
// is exactly the confusion the density metrics exist to avoid.
SimulationResult uniformDiscResult(double discRadius, int bins = 64, double size = 64.0,
                                   double density = 1.0) {
    SimulationResult r;
    r.nx = r.ny = bins;
    r.detW = r.detH = size;
    r.detCX = r.detCY = 0.0;
    r.detZ = 100.0;
    r.irradiance.assign(std::size_t(bins) * std::size_t(bins), 0.0);
    const double perBin = density * r.binArea();
    for (int iy = 0; iy < bins; ++iy) {
        for (int ix = 0; ix < bins; ++ix) {
            const double x = r.binX(ix), y = r.binY(iy);
            if (x * x + y * y <= discRadius * discRadius)
                r.irradiance[std::size_t(iy) * std::size_t(bins) + std::size_t(ix)] = perBin;
        }
    }
    for (double v : r.irradiance) r.fluxDetector += v;
    r.sourcePower = 1.0;
    return r;
}

SimConfig quickConfig(GeometryProvider::Scene scene, int rays) {
    SimConfig cfg;
    cfg.scene  = scene;
    cfg.source = SourceConfig::Type::Lambertian;
    cfg.rays   = rays;
    return cfg;
}

double rmsRadiusOf(const SimulationResult& res) {
    return analysis::computeSpotMetrics(res).rmsRadius;
}

} // namespace

// ============================================================== Fresnel =====

TEST(fresnel, normal_incidence_matches_the_closed_form) {
    // R(0) = ((n1 - n2) / (n1 + n2))^2, which is 4 % for air against n = 1.5 and
    // is the same crossing the interface either way.
    CHECK_NEAR(optics::fresnelReflectance(1.0, 1.5, 1.0), 0.04, 1e-12);
    CHECK_NEAR(optics::fresnelReflectance(1.5, 1.0, 1.0), 0.04, 1e-12);
    for (double n : {1.2, 1.4, 1.7, 2.0, 3.5}) {
        const double r = (1.0 - n) / (1.0 + n);
        CHECK_NEAR(optics::fresnelReflectance(1.0, n, 1.0), r * r, 1e-12);
    }
    // No interface at all reflects nothing.
    CHECK_NEAR(optics::fresnelReflectance(1.5, 1.5, 0.5), 0.0, 1e-12);
}

TEST(fresnel, rises_monotonically_to_one_at_grazing) {
    double prev = optics::fresnelReflectance(1.0, 1.5, 1.0);
    for (int deg = 1; deg <= 90; ++deg) {
        const double r = optics::fresnelReflectance(1.0, 1.5, std::cos(deg * kPi / 180.0));
        CHECK(r >= prev - 1e-12);          // unpolarised external reflection never dips
        CHECK(r <= 1.0 + 1e-12);
        prev = r;
    }
    CHECK_NEAR(optics::fresnelReflectance(1.0, 1.5, 0.0), 1.0, 1e-12);
}

TEST(fresnel, matches_the_textbook_equations_over_the_whole_range) {
    // Recomputed here from Rs and Rp directly, so the implementation is checked
    // against the equations rather than against itself.
    auto reference = [](double n1, double n2, double cosI) {
        const double sinT2 = (n1 / n2) * (n1 / n2) * (1.0 - cosI * cosI);
        if (sinT2 >= 1.0) return 1.0;
        const double cosT = std::sqrt(1.0 - sinT2);
        const double rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
        const double rp = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
        return 0.5 * (rs * rs + rp * rp);
    };
    for (double n : {1.33, 1.5, 1.9}) {
        for (int deg = 0; deg <= 89; ++deg) {
            const double c = std::cos(deg * kPi / 180.0);
            CHECK_NEAR(optics::fresnelReflectance(1.0, n, c), reference(1.0, n, c), 1e-12);
            CHECK_NEAR(optics::fresnelReflectance(n, 1.0, c), reference(n, 1.0, c), 1e-12);
        }
    }
}

TEST(fresnel, reaches_one_continuously_at_the_critical_angle) {
    const double index = 1.5;
    const double crit  = std::asin(1.0 / index);

    // Past the critical angle it is exactly total, not merely close.
    CHECK_NEAR(optics::fresnelReflectance(index, 1.0, std::cos(crit + 0.05)), 1.0, 1e-12);
    CHECK_NEAR(optics::fresnelReflectance(index, 1.0, std::cos(crit + 1e-9)), 1.0, 1e-12);

    // And it climbs to that continuously rather than jumping: the gap to 1 has
    // to shrink as the angle closes on the critical one. A step here would show
    // up as a hard ring in every light-guide irradiance pattern.
    double previousGap = 1.0;
    for (double eps : {0.2, 0.05, 0.01, 1e-3, 1e-5, 1e-7}) {
        const double gap = 1.0 - optics::fresnelReflectance(index, 1.0, std::cos(crit - eps));
        CHECK(gap > 0.0);              // strictly below total, short of the angle
        CHECK(gap < previousGap);      // and closing
        previousGap = gap;
    }
    CHECK(previousGap < 0.01);         // within a hair of the angle, within 1 % of total

    // Well short of it, internal reflection is still weak -- the guide would not
    // work at all if it were not.
    CHECK(optics::fresnelReflectance(index, 1.0, std::cos(crit - 0.4)) < 0.35);
}

TEST(fresnel, a_glass_plane_passes_ninety_six_percent_at_normal_incidence) {
    CHECK_NEAR(straightThroughGlass(fresnelPlane(1.5), 60.0), 0.96, 1e-9);
    // A denser glass reflects more, so less gets through.
    CHECK_NEAR(straightThroughGlass(fresnelPlane(2.0), 60.0), 1.0 - 1.0 / 9.0, 1e-9);
}

TEST(fresnel, switching_the_model_off_restores_the_fixed_split) {
    MeshSurface glass = fresnelPlane(1.5);
    glass.reflectivity   = 0.30;
    glass.transmissivity = 0.55;      // deliberately not 1 - R, so absorption shows

    PhysicsOptions off;
    off.fresnel = false;
    CHECK_NEAR(straightThroughGlass(glass, 60.0, off), 0.55, 1e-9);
    // With the model on, the fixed numbers are ignored entirely.
    CHECK_NEAR(straightThroughGlass(glass, 60.0), 0.96, 1e-9);
}

TEST(fresnel, costs_a_light_guide_real_efficiency) {
    // The entrance and exit faces of a guide are where its Fresnel loss lives.
    // Turning the model off can only make the guide look better.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::LightGuide, 8000);
    const double withFresnel = Simulation::run(cfg).efficiency;
    cfg.physics.fresnel = false;
    const double without = Simulation::run(cfg).efficiency;
    CHECK(withFresnel < without);
    CHECK(withFresnel > 0.5);        // but it is a loss, not a catastrophe
}

// =========================================================== absorption =====

TEST(absorption, follows_beer_lambert_over_the_travelled_path) {
    // Enter the glass at z = 0 and run to a receiver at z = L. With the fixed
    // split at T = 1 the only loss on the way is the bulk term, so the answer is
    // exp(-alpha L) and nothing else.
    for (double alpha : {0.001, 0.005, 0.02}) {
        for (double L : {20.0, 60.0, 150.0}) {
            MeshSurface glass = glassPlane(1.5, 400.0);   // fixed split, T = 1
            glass.absorption = alpha;
            // The tolerance is loose by a micron of path: a refracted branch is
            // restarted one surface epsilon past the interface, so the length
            // charged for is L - 1e-6 rather than L.
            CHECK_NEAR(straightThroughGlass(glass, L), std::exp(-alpha * L), 1e-6);
        }
    }
}

TEST(absorption, only_applies_inside_the_medium) {
    // The 10 mm the ray spends in vacuum before the glass must not be charged
    // for: if it were, the answer would be exp(-alpha (L + 10)).
    MeshSurface glass = glassPlane(1.5, 400.0);
    glass.absorption = 0.01;
    CHECK_NEAR(straightThroughGlass(glass, 50.0), std::exp(-0.5), 1e-6);
}

TEST(absorption, can_be_switched_off_and_scaled) {
    MeshSurface glass = glassPlane(1.5, 400.0);
    glass.absorption = 0.01;

    PhysicsOptions off;
    off.absorption = false;
    CHECK_NEAR(straightThroughGlass(glass, 50.0, off), 1.0, 1e-9);   // exact: no exp at all

    PhysicsOptions doubled;
    doubled.absorptionScale = 2.0;
    CHECK_NEAR(straightThroughGlass(glass, 50.0, doubled), std::exp(-1.0), 1e-6);
}

TEST(absorption, is_reported_separately_from_surface_losses) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::LightGuide, 4000);
    cfg.physics.absorptionScale = 20.0;    // a plastic pipe rather than glass
    const SimulationResult res = Simulation::run(cfg);

    CHECK(res.fluxBulkAbsorbed > 0.0);
    CHECK(res.fluxBulkAbsorbed <= res.fluxAbsorbed + 1e-12);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
}

TEST(absorption, makes_a_longer_guide_dimmer) {
    // Same rod, twice the length. Without a bulk term this pair would differ
    // only through the extra bounces; with one, length itself costs light.
    SimConfig shortGuide = quickConfig(GeometryProvider::Scene::LightGuide, 6000);
    shortGuide.useSceneDefaults = false;
    shortGuide.params = GeometryProvider::defaultParams(shortGuide.scene);
    shortGuide.params.v[1] = 120.0;                       // guide length
    shortGuide.params.v[2] = shortGuide.params.v[1] + 30.0;   // receiver just past it
    shortGuide.physics.absorptionScale = 20.0;

    SimConfig longGuide = shortGuide;
    longGuide.params.v[1] = 480.0;
    longGuide.params.v[2] = longGuide.params.v[1] + 30.0;

    const double s = Simulation::run(shortGuide).efficiency;
    const double l = Simulation::run(longGuide).efficiency;
    CHECK(l < s);
}

// ============================================================== scatter =====

TEST(scatter, cosine_hemisphere_is_cosine_weighted_and_stays_in_front) {
    std::uint64_t state = 987654321u;
    const Vec3 n(0.0, 0.0, 1.0);
    double meanCos = 0.0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        const Vec3 d = optics::cosineHemisphere(n, optics::uniform01(state),
                                                optics::uniform01(state));
        CHECK_NEAR(d.length(), 1.0, 1e-12);
        CHECK(d.z >= -1e-12);
        meanCos += d.z;
    }
    // A Lambertian emitter has E[cos] = 2/3; a uniform hemisphere would give 1/2,
    // so this distinguishes the two rather than merely checking the range.
    CHECK_NEAR(meanCos / N, 2.0 / 3.0, 0.01);
}

TEST(scatter, works_about_an_arbitrary_normal) {
    std::uint64_t state = 24680u;
    for (const Vec3& raw : {Vec3(0, 0, -1), Vec3(1, 2, -3), Vec3(-0.3, 0.9, 0.1)}) {
        const Vec3 n = raw.normalized();
        for (int i = 0; i < 400; ++i) {
            const Vec3 d = optics::cosineHemisphere(n, optics::uniform01(state),
                                                    optics::uniform01(state));
            CHECK(d.dot(n) >= -1e-12);
            CHECK_NEAR(d.length(), 1.0, 1e-12);
        }
    }
}

TEST(scatter, a_diffusing_mirror_destroys_the_focus) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 20000);
    const SimulationResult specular = Simulation::run(cfg);
    cfg.physics.scatterOverride = 1.0;
    const SimulationResult diffuse = Simulation::run(cfg);

    const analysis::SpotMetrics ms = analysis::computeSpotMetrics(specular);
    const analysis::SpotMetrics md = analysis::computeSpotMetrics(diffuse);

    // An ellipsoid images one focus onto the other; make every bounce Lambertian
    // and the image is gone. The receiver is small enough to clip the diffuse
    // halo, so the spot radius understates the change -- the peak irradiance and
    // the collected flux do not, because neither is bounded by the aperture.
    CHECK(md.rmsRadius > ms.rmsRadius);
    CHECK(md.peak < 0.3 * ms.peak);
    CHECK(diffuse.efficiency < 0.25 * specular.efficiency);
    CHECK_NEAR(diffuse.fluxAccounted(), diffuse.sourcePower, 1e-9);
}

TEST(scatter, conserves_energy_in_the_integrating_sphere) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::IntegratingSphere, 3000);
    const SimulationResult res = Simulation::run(cfg);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
    // A cavity with a small port and a high wall reflectance still delivers a
    // useful share of the light to that port -- that is the point of one.
    CHECK(res.efficiency > 0.05);
    CHECK(res.efficiency < 1.0);
}

TEST(scatter, a_diffuser_widens_the_pattern_it_transmits) {
    SimConfig clear = quickConfig(GeometryProvider::Scene::DiffuserPlate, 20000);
    clear.useSceneDefaults = false;
    clear.params = GeometryProvider::defaultParams(clear.scene);
    clear.params.v[1] = 0.0;             // diffusion off: a plain glass plate
    SimConfig hazy = clear;
    hazy.params.v[1] = 1.0;              // fully Lambertian exit face

    CHECK(rmsRadiusOf(Simulation::run(hazy)) > rmsRadiusOf(Simulation::run(clear)));
}

// ============================================================ roughness =====

TEST(roughness, a_zero_slope_error_leaves_the_normal_alone) {
    std::uint64_t state = 5u;
    const Vec3 n(0.0, 0.6, 0.8);
    const Vec3 p = optics::perturbNormal(n, 0.0, state);
    CHECK_NEAR(p.x, n.x, 1e-15);
    CHECK_NEAR(p.y, n.y, 1e-15);
    CHECK_NEAR(p.z, n.z, 1e-15);
}

TEST(roughness, tilts_the_normal_by_about_the_requested_sigma) {
    std::uint64_t state = 11u;
    const Vec3 n(0.0, 0.0, 1.0);
    const double sigma = 0.02;
    double sumSq = 0.0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        const Vec3 p = optics::perturbNormal(n, sigma, state);
        CHECK_NEAR(p.length(), 1.0, 1e-12);
        sumSq += std::acos(std::min(1.0, p.dot(n))) * std::acos(std::min(1.0, p.dot(n)));
    }
    // Two independent slope errors of sigma each, so the tilt off the original
    // normal has RMS sqrt(2) sigma.
    CHECK_NEAR(std::sqrt(sumSq / N), std::sqrt(2.0) * sigma, 0.2 * sigma);
}

TEST(roughness, blurs_a_focus_more_the_rougher_it_gets) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 20000);
    double previous = 0.0;
    for (double sigma : {0.0, 0.004, 0.012, 0.03}) {
        cfg.physics.roughnessOverride = sigma;
        const double rms = rmsRadiusOf(Simulation::run(cfg));
        CHECK(rms >= previous);
        previous = rms;
    }
    CHECK(previous > 0.0);
}

// ============================================================= spectral =====

TEST(spectral, cauchy_is_exact_at_the_reference_line) {
    // The index a surface is quoted at must survive the dispersion model
    // untouched, or a monochromatic trace would answer differently from the
    // non-dispersive one it is supposed to reproduce.
    CHECK_NEAR(optics::cauchyIndex(1.5, 0.0042, optics::kLambdaD), 1.5, 1e-15);
    CHECK_NEAR(optics::cauchyIndex(1.5, 0.0, 400.0), 1.5, 1e-15);
}

TEST(spectral, blue_bends_more_than_red) {
    const double blue = optics::cauchyIndex(1.5, 0.0042, 460.0);
    const double red  = optics::cauchyIndex(1.5, 0.0042, 620.0);
    CHECK(blue > 1.5);
    CHECK(red  < 1.5);
    // Abbe number of about 64 for this coefficient: n_F - n_C near 0.008.
    CHECK_NEAR(optics::cauchyIndex(1.5, 0.0042, 486.1) -
               optics::cauchyIndex(1.5, 0.0042, 656.3), 0.00803, 5e-4);
}

TEST(spectral, the_bands_add_up_to_the_whole_pattern) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Prism, 12000);
    cfg.spectrum = SourceConfig::Spectrum::Rgb;
    const SimulationResult res = Simulation::run(cfg);

    CHECK(res.spectral);
    CHECK(res.bandIrradiance.size() == res.irradiance.size() * 3);
    const std::size_t cells = res.irradiance.size();
    for (std::size_t i = 0; i < cells; ++i)
        CHECK_NEAR(res.bandIrradiance[i] + res.bandIrradiance[cells + i] +
                   res.bandIrradiance[2 * cells + i], res.irradiance[i], 1e-12);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
}

TEST(spectral, a_prism_deviates_blue_further_than_red) {
    // Fed by a collimated beam, so every ray meets the glass at the same angle
    // and the only thing separating the bands is the index. A point source would
    // wash the effect out under its own angular spread.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Prism, 200000);
    cfg.spectrum   = SourceConfig::Spectrum::Rgb;
    cfg.source     = SourceConfig::Type::Collimated;
    cfg.beamRadius = 30.0;

    // The prism is symmetric about its apex, so it splits the beam into two
    // deviated halves and the centroid sits back near the axis. The mean
    // distance from the axis is the deviation itself.
    auto meanDeviation = [](const SimulationResult& r, int band) {
        const std::size_t cells = r.irradiance.size();
        double sum = 0.0, sax = 0.0;
        for (int iy = 0; iy < r.ny; ++iy)
            for (int ix = 0; ix < r.nx; ++ix) {
                const double v = r.bandIrradiance[std::size_t(band) * cells +
                                                  std::size_t(iy) * std::size_t(r.nx) +
                                                  std::size_t(ix)];
                sum += v;
                sax += v * std::fabs(r.binX(ix) - r.detCX);
            }
        return sum > 0.0 ? sax / sum : 0.0;
    };

    const SimulationResult res = Simulation::run(cfg);
    const double red = meanDeviation(res, 0), green = meanDeviation(res, 1),
                 blue = meanDeviation(res, 2);
    CHECK(red > 0.0 && green > 0.0 && blue > 0.0);
    // Blue sees the highest index, so it bends most; green sits between.
    CHECK(blue > green);
    CHECK(green > red);
    CHECK(blue - red > 1.0);          // millimetres of separation, not noise

    // With dispersion off the three bands see one index and land together.
    cfg.physics.dispersion = false;
    const SimulationResult flat = Simulation::run(cfg);
    CHECK(std::fabs(meanDeviation(flat, 2) - meanDeviation(flat, 0)) < 0.2 * (blue - red));
}

// ============================================================ intensity =====

namespace {

// A scene that is almost entirely empty: one small triangle far off the axis, so
// the BVH has something to hold but virtually every ray escapes and is binned
// into the far field.
MeshList nearlyEmptyScene() {
    MeshSurface m;
    m.label = QStringLiteral("speck");
    m.verts = {gp_Pnt(900, 900, 900), gp_Pnt(901, 900, 900), gp_Pnt(900, 901, 900)};
    Triangle t;
    t.v0 = 0; t.v1 = 1; t.v2 = 2;
    t.normal = gp_Dir(0, 0, 1);
    m.tris.push_back(t);
    return MeshList{m};
}

} // namespace

TEST(intensity, an_isotropic_source_is_flat_per_steradian) {
    TraceScene scene;
    scene.build(nearlyEmptyScene());

    SourceConfig src;
    src.type = SourceConfig::Type::Point;
    src.rays = 200000;
    src.halfAngleDeg = 180.0;

    TraceOptions opt;
    opt.nTheta = 9;      // wide rings, so each holds enough rays to be quiet
    opt.nPhi   = 8;

    SimulationResult res;
    RayTracer::trace(scene, src, res, opt);
    CHECK(res.intensity.valid());

    // Raw flux per bin would fall off toward the poles simply because those
    // cells are small. Divided by solid angle, an isotropic source is flat --
    // which is exactly the normalisation that makes this a candela plot.
    double mean = 0.0;
    for (double v : res.intensity.profile) mean += v;
    mean /= double(res.intensity.profile.size());
    CHECK(mean > 0.0);
    for (double v : res.intensity.profile) CHECK_NEAR(v / mean, 1.0, 0.06);

    // Total power over the sphere: 4 pi steradians times the mean intensity.
    CHECK_NEAR(mean * 4.0 * kPi, src.power, 0.02);
}

TEST(intensity, accounts_for_every_ray_that_left_the_system) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.intensity.valid());
    // The far field bins the directions rays leave on: everything that escaped
    // plus everything that landed on the receiver, and nothing else.
    CHECK_NEAR(res.intensity.totalFlux, res.fluxEscaped + res.fluxDetector, 1e-9);
}

TEST(intensity, a_collimator_has_a_narrow_beam_and_a_diverger_a_wide_one) {
    const double collimated =
        Simulation::run(quickConfig(GeometryProvider::Scene::Reflector, 40000)).intensity.fwhmDeg;
    const double diverging =
        Simulation::run(quickConfig(GeometryProvider::Scene::PlanoConcaveLens, 40000))
            .intensity.fwhmDeg;

    CHECK(collimated > 0.0);
    CHECK(collimated < 25.0);          // a paraboloid at its focus makes a beam
    CHECK(diverging > collimated);
}

TEST(intensity, binning_can_be_switched_off) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 2000);
    cfg.nTheta = 0;
    const SimulationResult res = Simulation::run(cfg);
    CHECK(!res.intensity.valid());
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
}

// ============================================================== metrics =====

TEST(metrics, a_uniform_disc_reports_its_own_radius) {
    const double R = 10.0;
    const SimulationResult res = uniformDiscResult(R);
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);

    CHECK(m.valid);
    CHECK_NEAR(m.centroidX, 0.0, 0.05);
    CHECK_NEAR(m.centroidY, 0.0, 0.05);
    // For a uniform disc: RMS radius = R / sqrt(2), and the radius enclosing a
    // fraction f is R sqrt(f).
    CHECK_NEAR(m.rmsRadius, R / std::sqrt(2.0), 0.25);
    CHECK_NEAR(m.d50Radius, R * std::sqrt(0.50), 0.4);
    CHECK_NEAR(m.d86Radius, R * std::sqrt(0.86), 0.4);
    // Every lit bin holds the same flux, so both uniformity measures are 1.
    CHECK_NEAR(m.uniformity, 1.0, 1e-12);
    CHECK_NEAR(m.meanToPeak, 1.0, 1e-12);
    CHECK_NEAR(m.fwhmX, 2.0 * R, 1.5);
    CHECK_NEAR(m.fwhmY, 2.0 * R, 1.5);
}

TEST(metrics, are_densities_and_do_not_move_with_the_bin_count) {
    // The same physical disc, binned two ways. A metric that changes between
    // them is a property of the grid, not of the optic.
    const analysis::SpotMetrics coarse = analysis::computeSpotMetrics(uniformDiscResult(10.0, 32));
    const analysis::SpotMetrics fine   = analysis::computeSpotMetrics(uniformDiscResult(10.0, 128));
    CHECK_NEAR(coarse.peak, fine.peak, 1e-9);
    CHECK_NEAR(coarse.rmsRadius, fine.rmsRadius, 0.3);
    CHECK_NEAR(coarse.d86Radius, fine.d86Radius, 0.5);
}

TEST(metrics, an_offset_spot_is_described_by_its_size_not_its_offset) {
    SimulationResult res = uniformDiscResult(6.0);
    // Rebuild the same disc around (12, -8).
    res.irradiance.assign(res.irradiance.size(), 0.0);
    for (int iy = 0; iy < res.ny; ++iy)
        for (int ix = 0; ix < res.nx; ++ix) {
            const double dx = res.binX(ix) - 12.0, dy = res.binY(iy) + 8.0;
            if (dx * dx + dy * dy <= 36.0)
                res.irradiance[std::size_t(iy) * std::size_t(res.nx) + std::size_t(ix)] =
                    res.binArea();
        }

    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
    CHECK_NEAR(m.centroidX, 12.0, 0.2);
    CHECK_NEAR(m.centroidY, -8.0, 0.2);
    CHECK_NEAR(m.rmsRadius, 6.0 / std::sqrt(2.0), 0.25);
}

TEST(metrics, an_empty_receiver_is_reported_as_empty_not_as_a_divide_by_zero) {
    SimulationResult res = uniformDiscResult(10.0);
    res.irradiance.assign(res.irradiance.size(), 0.0);
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
    CHECK(m.litBins == 0);
    CHECK(std::isfinite(m.rmsRadius));
    CHECK(std::isfinite(m.uniformity));
    CHECK_NEAR(m.total, 0.0, 1e-12);
}

TEST(metrics, encircled_energy_climbs_from_zero_to_one) {
    const SimulationResult res = uniformDiscResult(10.0);
    const analysis::EncircledEnergy ee = analysis::encircledEnergy(res);
    CHECK(ee.radius.size() > 8);
    CHECK(ee.fraction.back() > 0.999);
    for (std::size_t i = 1; i < ee.fraction.size(); ++i) {
        CHECK(ee.fraction[i] >= ee.fraction[i - 1] - 1e-12);   // cumulative
        CHECK(ee.radius[i] > ee.radius[i - 1]);
    }
}

TEST(metrics, a_cross_section_cuts_where_it_is_asked_to) {
    const SimulationResult res = uniformDiscResult(10.0);
    const analysis::Profile through = analysis::crossSection(res, true, 0.0);
    const analysis::Profile past    = analysis::crossSection(res, true, 25.0);

    CHECK(through.coord.size() == std::size_t(res.nx));
    CHECK(through.peak > 0.0);
    CHECK_NEAR(past.peak, 0.0, 1e-12);   // outside the disc entirely

    // The lit run through the centre is the disc's diameter.
    int lit = 0;
    for (double v : through.value) if (v > 0.0) ++lit;
    CHECK_NEAR(double(lit), 20.0, 2.0);
}

TEST(metrics, csv_export_round_trips_the_grid_size) {
    const SimulationResult res = uniformDiscResult(10.0, 16);
    const QString csv = analysis::irradianceCsv(res);
    // A header row of x coordinates plus one row per y, after the comments.
    int dataRows = 0;
    for (const QString& line : csv.split(QLatin1Char('\n')))
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) ++dataRows;
    CHECK(dataRows == res.ny + 1);
    CHECK(csv.contains(QStringLiteral("y\\x")));
}

// ================================================================ focus =====

TEST(focus, finds_the_waist_of_a_converging_cone) {
    // Rays recorded on a plane at z = 0, all aimed at a point 100 mm past it.
    // Propagating them analytically must put the smallest spot exactly there.
    SimulationResult res;
    res.detZ = 0.0;
    res.nx = res.ny = 8;
    res.detW = res.detH = 80.0;
    res.irradiance.assign(64, 0.0);

    const double waistZ = 100.0, ringR = 10.0;
    for (int i = 0; i < 512; ++i) {
        const double a = 2.0 * kPi * double(i) / 512.0;
        DetectorArrival arr;
        arr.p = Vec3(ringR * std::cos(a), ringR * std::sin(a), 0.0);
        arr.d = (Vec3(0, 0, waistZ) - arr.p).normalized();
        arr.energy = 1.0f;
        res.arrivals.push_back(arr);
    }

    const std::vector<analysis::FocusSample> sweep =
        analysis::throughFocus(res, -100.0, 300.0, 81);
    CHECK(sweep.size() == 81);
    CHECK_NEAR(analysis::bestFocusZ(sweep), waistZ, 1.0);

    // At the plane the rays were recorded on, the spot is the ring they started
    // as; at the waist it has collapsed.
    CHECK_NEAR(sweep.front().rmsRadius, 2.0 * ringR, 1.0);   // z = -100, twice as far out
    double best = 1e300;
    for (const auto& f : sweep) best = std::min(best, f.rmsRadius);
    CHECK(best < 0.05);
}

TEST(focus, spreads_symmetrically_either_side_of_the_waist) {
    SimulationResult res;
    res.detZ = 0.0;
    const double waistZ = 100.0, ringR = 10.0;
    for (int i = 0; i < 256; ++i) {
        const double a = 2.0 * kPi * double(i) / 256.0;
        DetectorArrival arr;
        arr.p = Vec3(ringR * std::cos(a), ringR * std::sin(a), 0.0);
        arr.d = (Vec3(0, 0, waistZ) - arr.p).normalized();
        arr.energy = 1.0f;
        res.arrivals.push_back(arr);
    }
    const std::vector<analysis::FocusSample> sweep =
        analysis::throughFocus(res, waistZ - 40.0, waistZ + 40.0, 3);
    CHECK(sweep.size() == 3);
    CHECK_NEAR(sweep[0].rmsRadius, sweep[2].rmsRadius, 1e-6);
    CHECK(sweep[1].rmsRadius < sweep[0].rmsRadius);
}

TEST(focus, a_real_imaging_scene_focuses_near_its_receiver) {
    // The elliptical reflector images one focus onto the other, and the scene
    // puts the receiver there. The sweep should agree -- to within what a
    // tessellated mirror can actually do, which is the point of measuring it
    // rather than assuming it.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 60000);
    const SimulationResult res = Simulation::run(cfg);
    CHECK(!res.arrivals.empty());

    const studies::FocusStudy study = studies::throughFocus(res, 400.0);
    CHECK(study.valid);
    CHECK(study.samples.size() > 10);

    // A real waist: the best plane beats both ends of the sweep by a wide
    // margin, so this is a minimum rather than a plateau the fit landed on.
    CHECK(study.bestRms < 0.75 * study.samples.front().rmsRadius);
    CHECK(study.bestRms < 0.75 * study.samples.back().rmsRadius);
    // And it is inside the swept range, not pinned to an end.
    CHECK(study.bestZ > study.samples.front().z + 1e-9);
    CHECK(study.bestZ < study.samples.back().z - 1e-9);

    // Near the receiver the scene aimed it at, within a quarter of the throw.
    CHECK(std::fabs(study.bestZ - study.receiverZ) < 0.25 * std::fabs(study.receiverZ));
    // The receiver plane can only be as good as the best plane found.
    CHECK(study.bestRms <= rmsRadiusOf(res) + 1e-9);
}

TEST(focus, needs_arrivals_and_says_so_when_it_has_none) {
    SimulationResult empty;
    empty.detZ = 50.0;
    const studies::FocusStudy study = studies::throughFocus(empty, 100.0);
    CHECK(!study.valid);
    CHECK(study.samples.empty());
}

// ========================================================== convergence =====

TEST(convergence, error_bars_shrink_as_one_over_root_n) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 0);
    const std::vector<studies::ConvergencePoint> sweep =
        studies::convergence(cfg, 1000, 64000, 7);

    CHECK(sweep.size() == 7);
    CHECK(sweep.front().rays >= 1000);
    CHECK(sweep.back().rays >= 60000);

    // Sixty-four times the rays is *at least* eight times the precision. Plain
    // Monte Carlo converges at exactly one over root N, so eight is the floor;
    // a low-discrepancy sequence converges faster than that on a smooth
    // integrand, and the sweep is expected to beat it. What would be a bug is
    // converging more slowly, or not converging at all.
    const double ratio = sweep.front().stdErr / std::max(sweep.back().stdErr, 1e-18);
    if (!(ratio > 5.0))
        std::printf("       64x the rays bought only %.2fx the precision\n", ratio);
    CHECK(ratio > 5.0);
    CHECK(ratio < 5000.0);           // still a number, not a divide by nothing

    for (const auto& p : sweep) {
        CHECK(p.stdErr > 0.0);
        CHECK(p.efficiency >= 0.0 && p.efficiency <= 1.0);
    }
}

TEST(convergence, every_point_agrees_with_the_finest_one_inside_its_error_bar) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 0);
    const std::vector<studies::ConvergencePoint> sweep =
        studies::convergence(cfg, 2000, 64000, 6);
    const studies::ConvergencePoint& best = sweep.back();

    for (const auto& p : sweep) {
        const double sigma = std::sqrt(p.stdErr * p.stdErr + best.stdErr * best.stdErr);
        // Four sigma: with six points, a three-sigma bound would fail by chance
        // often enough to make the suite flaky rather than informative.
        CHECK(std::fabs(p.efficiency - best.efficiency) <= 4.0 * sigma + 1e-12);
    }
    CHECK(studies::hasConverged(sweep));
}

TEST(convergence, a_single_ray_run_still_reports_a_finite_error) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 1);
    const SimulationResult res = Simulation::run(cfg);
    CHECK(std::isfinite(res.efficiencyStdErr));
    CHECK(res.efficiencyStdErr >= 0.0);
}

// =============================================================== source =====

TEST(source, a_collimated_beam_is_parallel_and_inside_its_radius) {
    SourceConfig src;
    src.type = SourceConfig::Type::Collimated;
    src.axis = gp_Dir(0, 0, 1);
    src.origin = gp_Pnt(5, -3, 20);
    src.beamRadius = 12.0;

    double maxR = 0.0;
    for (std::size_t i = 0; i < 4000; ++i) {
        const EmittedRay r = RayTracer::sampleRay(src, i, 12345u);
        CHECK_NEAR(r.dir.x, 0.0, 1e-12);
        CHECK_NEAR(r.dir.y, 0.0, 1e-12);
        CHECK_NEAR(r.dir.z, 1.0, 1e-12);
        CHECK_NEAR(r.origin.z, 20.0, 1e-12);
        const double dx = r.origin.x - 5.0, dy = r.origin.y + 3.0;
        maxR = std::max(maxR, std::sqrt(dx * dx + dy * dy));
    }
    CHECK(maxR <= src.beamRadius + 1e-12);
    CHECK(maxR > 0.9 * src.beamRadius);   // the disc is actually filled
}

TEST(source, a_cone_angle_bounds_the_emission) {
    for (double half : {5.0, 20.0, 60.0}) {
        SourceConfig src;
        src.type = SourceConfig::Type::Point;
        src.axis = gp_Dir(0, 0, 1);
        src.halfAngleDeg = half;

        double widest = 0.0;
        for (std::size_t i = 0; i < 4000; ++i) {
            const Vec3 d = RayTracer::sampleDirection(src, i, 999u);
            widest = std::max(widest, std::acos(std::min(1.0, d.z)) * 180.0 / kPi);
        }
        CHECK(widest <= half + 1e-9);
        CHECK(widest > 0.85 * half);      // and it fills the cone it was given
    }
}

TEST(source, a_lambertian_cone_stays_cosine_weighted) {
    SourceConfig src;
    src.type = SourceConfig::Type::Lambertian;
    src.axis = gp_Dir(0, 0, 1);
    src.halfAngleDeg = 90.0;

    double meanCos = 0.0;
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
        const Vec3 d = RayTracer::sampleDirection(src, std::size_t(i), 12345u);
        CHECK(d.z >= -1e-12);
        meanCos += d.z;
    }
    CHECK_NEAR(meanCos / n, 2.0 / 3.0, 0.01);
}

TEST(source, an_extended_emitter_starts_rays_across_its_whole_area) {
    SourceConfig src;
    src.type   = SourceConfig::Type::Lambertian;
    src.shape  = SourceConfig::Shape::Rect;
    src.axis   = gp_Dir(0, 0, 1);
    src.sizeA  = 8.0;
    src.sizeB  = 3.0;

    double xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9;
    for (std::size_t i = 0; i < 8000; ++i) {
        const EmittedRay r = RayTracer::sampleRay(src, i, 7u);
        xmin = std::min(xmin, r.origin.x); xmax = std::max(xmax, r.origin.x);
        ymin = std::min(ymin, r.origin.y); ymax = std::max(ymax, r.origin.y);
        CHECK_NEAR(r.origin.z, 0.0, 1e-12);
    }
    CHECK_NEAR(xmax - xmin, src.sizeA, 0.15);
    CHECK_NEAR(ymax - ymin, src.sizeB, 0.15);
}

TEST(source, a_sphere_emitter_puts_every_ray_on_its_surface_facing_out) {
    SourceConfig src;
    src.type  = SourceConfig::Type::Lambertian;
    src.shape = SourceConfig::Shape::Sphere;
    src.sizeA = 4.0;
    src.origin = gp_Pnt(1, 2, 3);

    for (std::size_t i = 0; i < 4000; ++i) {
        const EmittedRay r = RayTracer::sampleRay(src, i, 3u);
        const Vec3 offset = r.origin - Vec3(1, 2, 3);
        CHECK_NEAR(offset.length(), src.sizeA, 1e-9);
        // Emitted about the outward normal, so no ray starts by going inward.
        CHECK(r.dir.dot(offset.normalized()) >= -1e-9);
    }
}

TEST(source, a_point_emitter_ignores_the_size_fields) {
    SourceConfig src;
    src.shape = SourceConfig::Shape::PointLike;
    src.sizeA = 50.0;
    src.sizeB = 50.0;
    src.origin = gp_Pnt(0, 0, -7);
    for (std::size_t i = 0; i < 500; ++i) {
        const EmittedRay r = RayTracer::sampleRay(src, i, 5u);
        CHECK_NEAR((r.origin - Vec3(0, 0, -7)).length(), 0.0, 1e-12);
    }
}

TEST(source, extended_emitters_cost_a_concentrator_its_gain) {
    // A point source has no etendue, so a concentrator fed by one can beat what
    // any real optic could do. Give the emitter area and the answer comes down.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Cpc, 20000);
    const double idealised = Simulation::run(cfg).efficiency;

    cfg.shape = SourceConfig::Shape::Disc;
    cfg.sizeA = 40.0;
    const double honest = Simulation::run(cfg).efficiency;

    CHECK(honest < idealised);
    CHECK(honest > 0.0);
}

// ================================================================= rays =====

TEST(rays, legs_carry_their_energy_and_depth) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::LightGuide, 4000);
    const SimulationResult res = Simulation::run(cfg);
    CHECK(!res.raySegments.empty());

    int deep = 0;
    for (const RaySegment& s : res.raySegments) {
        CHECK(s.energy > 0.0f);
        CHECK(s.energy <= 1.0f + 1e-6f);
        if (s.depth > 3) ++deep;
    }
    // A guide is a bounce chain, so plenty of its legs are far down a path.
    CHECK(deep > 0);
}

TEST(rays, receiver_paths_are_marked_all_the_way_back) {
    // One ray, straight through glass onto a receiver: both recorded legs belong
    // to the branch that arrived, so both must be marked.
    MeshList meshes{fresnelPlane(1.5), detectorSurface(2000.0, 60.0)};
    TraceScene scene;
    scene.build(meshes);

    SimulationResult single;
    RayTracer::traceSingleRay(scene, Vec3(0, 0, -10), Vec3(0, 0, 1), single);
    CHECK(single.raySegments.size() == 2);
    for (const RaySegment& s : single.raySegments) CHECK(s.reachedDetector());

    // In a scene that loses light, some legs are marked and some are not --
    // which is the whole point of the flag.
    const SimulationResult res =
        Simulation::run(quickConfig(GeometryProvider::Scene::Lens, 6000));
    std::size_t marked = 0;
    for (const RaySegment& s : res.raySegments) if (s.reachedDetector()) ++marked;
    CHECK(marked > 0);
    CHECK(marked < res.raySegments.size());
}

TEST(rays, arrivals_land_on_the_receiver_plane) {
    const SimulationResult res =
        Simulation::run(quickConfig(GeometryProvider::Scene::Reflector, 8000));
    CHECK(!res.arrivals.empty());
    for (const DetectorArrival& a : res.arrivals) {
        CHECK_NEAR(a.p.z, res.detZ, 1e-3);
        CHECK_NEAR(a.d.length(), 1.0, 1e-9);
        CHECK(a.energy > 0.0f);
        CHECK(std::fabs(a.p.x - res.detCX) <= 0.5 * res.detW + 1e-6);
        CHECK(std::fabs(a.p.y - res.detCY) <= 0.5 * res.detH + 1e-6);
    }
}

// =============================================================== params =====

TEST(params, every_scene_declares_a_usable_parameter_block) {
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto scene = GeometryProvider::Scene(i);
        const auto& infos = GeometryProvider::paramInfo(scene);

        CHECK(!infos.empty());
        CHECK(infos.size() <= SceneParams::kMax);
        for (const auto& p : infos) {
            CHECK(!p.name.isEmpty());
            CHECK(p.min < p.max);
            CHECK(p.def >= p.min && p.def <= p.max);
            CHECK(p.step > 0.0);
        }
        // The last slot is always the receiver position, by convention.
        CHECK(infos.back().name == QStringLiteral("Detector Z"));

        const SceneParams def = GeometryProvider::defaultParams(scene);
        CHECK(GeometryProvider::sanitise(scene, def) == def);
    }
}

TEST(params, are_clamped_and_nan_is_rejected) {
    const auto scene = GeometryProvider::Scene::LightGuide;
    const auto& infos = GeometryProvider::paramInfo(scene);

    SceneParams wild;
    wild.v[0] = -1e9;
    wild.v[1] = 1e9;
    wild.v[2] = std::nan("");
    const SceneParams safe = GeometryProvider::sanitise(scene, wild);

    CHECK_NEAR(safe.v[0], infos[0].min, 1e-12);
    CHECK_NEAR(safe.v[1], infos[1].max, 1e-12);
    CHECK_NEAR(safe.v[2], infos[2].def, 1e-12);   // NaN falls back rather than clamping
    for (std::size_t i = infos.size(); i < SceneParams::kMax; ++i)
        CHECK_NEAR(safe.v[i], 0.0, 1e-12);
}

TEST(params, actually_change_the_geometry) {
    const auto scene = GeometryProvider::Scene::LightGuide;
    SceneParams shortGuide = GeometryProvider::defaultParams(scene);
    shortGuide.v[1] = 100.0;
    SceneParams longGuide = shortGuide;
    longGuide.v[1] = 400.0;

    const Simulation::SceneRef a = Simulation::dataFor(scene, shortGuide);
    const Simulation::SceneRef b = Simulation::dataFor(scene, longGuide);
    CHECK(a.get() != b.get());
    CHECK(!a->scene.empty() && !b->scene.empty());

    // The receiver follows the guide, so the two scenes differ in extent.
    CHECK(b->scene.detector().center.z > a->scene.detector().center.z);
}

TEST(params, move_the_source_with_the_optic_that_focuses_it) {
    // The whole premise of the parabolic reflector is that the emitter sits at
    // the focus. Change the focal length and the emitter has to follow, or the
    // scene silently stops being a collimator.
    const auto scene = GeometryProvider::Scene::Reflector;
    for (double f : {40.0, 100.0, 180.0}) {
        SceneParams p = GeometryProvider::defaultParams(scene);
        p.v[0] = f;
        const GeometryProvider::SceneSetup setup = GeometryProvider::build(scene, p);
        CHECK_NEAR(setup.sourceOrigin.Z(), f, 1e-9);
    }
}

TEST(params, a_repeated_lookup_is_served_from_the_cache) {
    const auto scene = GeometryProvider::Scene::Lens;
    SceneParams p = GeometryProvider::defaultParams(scene);
    p.v[0] = 33.0;

    double first = 0.0, second = 0.0;
    const Simulation::SceneRef a = Simulation::dataFor(scene, p, &first);
    const Simulation::SceneRef b = Simulation::dataFor(scene, p, &second);
    CHECK(a.get() == b.get());
    CHECK(second == 0.0);           // no rebuild the second time
    CHECK(first >= 0.0);
}

TEST(params, geometry_stays_alive_while_a_reference_is_held) {
    // The variant cache is small, so a slider sweep evicts entries. A run that
    // is holding one must keep tracing valid geometry, not a dangling BVH.
    const auto scene = GeometryProvider::Scene::Lens;
    SceneParams p = GeometryProvider::defaultParams(scene);
    p.v[0] = 21.0;
    const Simulation::SceneRef pinned = Simulation::dataFor(scene, p);
    const std::size_t tris = pinned->scene.triangles().size();

    for (int i = 0; i < 24; ++i) {
        SceneParams q = GeometryProvider::defaultParams(scene);
        q.v[0] = 22.0 + i;
        (void)Simulation::dataFor(scene, q);
    }
    CHECK(pinned->scene.triangles().size() == tris);
    CHECK(!pinned->scene.empty());
}

// =============================================================== config =====

TEST(config, round_trips_every_field) {
    SimConfig cfg;
    cfg.scene            = GeometryProvider::Scene::TaperedLightGuide;
    cfg.useSceneDefaults = false;
    cfg.params           = GeometryProvider::defaultParams(cfg.scene);
    cfg.params.v[0]      = 88.0;
    cfg.params.v[1]      = 41.0;
    cfg.source           = SourceConfig::Type::Collimated;
    cfg.shape            = SourceConfig::Shape::Rect;
    cfg.spectrum         = SourceConfig::Spectrum::Rgb;
    cfg.halfAngleDeg     = 42.5;
    cfg.sizeA            = 3.5;
    cfg.sizeB            = 1.25;
    cfg.beamRadius       = 17.0;
    cfg.spectrum.wavelengthNm = 632.8;
    cfg.spectrum.cct          = 4200.0;
    cfg.power            = 1500.0;
    cfg.fluxUnit         = FluxUnit::Lumen;
    cfg.detectorBins     = 128;
    cfg.rays             = 250000;
    cfg.seed             = 777;
    cfg.threads          = 6;
    cfg.nTheta           = 45;
    cfg.nPhi             = 36;
    cfg.physics.fresnel           = false;
    cfg.physics.scattering        = false;
    cfg.physics.roughnessOverride = 0.02;
    cfg.physics.absorptionScale   = 3.5;
    cfg.physics.coatings          = false;
    cfg.physics.volumeScattering  = false;
    cfg.physics.polarised         = true;
    cfg.polarisationState         = 2;

    SimConfig back;
    CHECK(configio::fromJson(configio::toJson(cfg), back));

    CHECK(back.scene == cfg.scene);
    CHECK(back.source == cfg.source);
    CHECK(back.shape == cfg.shape);
    CHECK(back.spectrum.kind == cfg.spectrum.kind);
    CHECK_NEAR(back.halfAngleDeg, cfg.halfAngleDeg, 1e-9);
    CHECK_NEAR(back.sizeA, cfg.sizeA, 1e-9);
    CHECK_NEAR(back.sizeB, cfg.sizeB, 1e-9);
    CHECK_NEAR(back.beamRadius, cfg.beamRadius, 1e-9);
    CHECK_NEAR(back.spectrum.wavelengthNm, cfg.spectrum.wavelengthNm, 1e-9);
    CHECK_NEAR(back.spectrum.cct, cfg.spectrum.cct, 1e-9);
    CHECK_NEAR(back.power, cfg.power, 1e-9);
    CHECK(back.fluxUnit == cfg.fluxUnit);
    CHECK(back.detectorBins == cfg.detectorBins);
    CHECK(back.rays == cfg.rays);
    CHECK(back.seed == cfg.seed);
    CHECK(back.threads == cfg.threads);
    CHECK(back.nTheta == cfg.nTheta);
    CHECK(back.nPhi == cfg.nPhi);
    CHECK(back.physics.fresnel == false);
    CHECK(back.physics.coatings == false);
    CHECK(back.physics.volumeScattering == false);
    CHECK(back.physics.polarised == true);
    CHECK(back.polarisationState == 2);
    CHECK(back.physics.scattering == false);
    CHECK_NEAR(back.physics.roughnessOverride, 0.02, 1e-9);
    CHECK_NEAR(back.physics.absorptionScale, 3.5, 1e-9);
    CHECK(back.effectiveParams() == cfg.effectiveParams());
}

TEST(config, identifies_a_scene_by_name_not_by_index) {
    // Scenes are an ordered registry that new entries get inserted into. An
    // index would silently reopen a saved setup against a different optic.
    SimConfig cfg;
    cfg.scene = GeometryProvider::Scene::Cpc;
    const QString json = configio::toJson(cfg);
    CHECK(json.contains(GeometryProvider::info(GeometryProvider::Scene::Cpc).name));

    SimConfig back;
    CHECK(configio::fromJson(json, back));
    CHECK(back.scene == GeometryProvider::Scene::Cpc);
}

TEST(config, an_unknown_scene_name_falls_back_instead_of_failing) {
    SimConfig back;
    CHECK(configio::fromJson(QStringLiteral(
        "{\"scene\":{\"name\":\"Warp Coil\"},\"run\":{\"rays\":4242}}"), back));
    CHECK(back.rays == 4242);
    CHECK(int(back.scene) >= 0 && int(back.scene) < GeometryProvider::count());
}

TEST(config, a_truncated_file_keeps_the_defaults_for_what_is_missing) {
    const SimConfig defaults;
    SimConfig back;
    CHECK(configio::fromJson(QStringLiteral("{}"), back));
    CHECK(back.rays == defaults.rays);
    CHECK(back.source == defaults.source);
    CHECK(back.physics.fresnel == defaults.physics.fresnel);
    CHECK(back.effectiveParams() == defaults.effectiveParams());
}

TEST(config, rejects_text_that_is_not_json_at_all) {
    SimConfig back;
    QString err;
    CHECK(!configio::fromJson(QStringLiteral("not json {{{"), back, &err));
    CHECK(!err.isEmpty());
}

TEST(config, clamps_hostile_numbers_into_range) {
    SimConfig back;
    CHECK(configio::fromJson(QStringLiteral(
        "{\"run\":{\"rays\":-5,\"nPhi\":0},"
        "\"source\":{\"halfAngleDeg\":9999,\"sizeA\":-3},"
        "\"physics\":{\"absorptionScale\":-8}}"), back));
    CHECK(back.rays >= 1);
    CHECK(back.nPhi >= 1);
    CHECK(back.halfAngleDeg <= 180.0);
    CHECK(back.sizeA >= 0.0);
    CHECK(back.physics.absorptionScale >= 0.0);

    // And the clamped config still produces a runnable scene.
    back.rays = 200;
    const SimulationResult res = Simulation::run(back);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
}

TEST(config, survives_a_file_round_trip) {
    SimConfig cfg;
    cfg.scene = GeometryProvider::Scene::Axicon;
    cfg.rays  = 4321;
    const QString path = QDir::temp().filePath(QStringLiteral("occt-optics-config-test.json"));

    QString err;
    CHECK(configio::save(path, cfg, &err));
    SimConfig back;
    CHECK(configio::load(path, back, &err));
    CHECK(back.scene == cfg.scene);
    CHECK(back.rays == cfg.rays);
    QFile::remove(path);
}

// ================================================================= media ====
// Phase 1: smooth normals, consistent winding, a medium stack, an unbiased
// estimator and unbiased arrival sampling.

namespace {

// A cube of glass from `z0` to `z1`, built as an OCCT solid so its faces carry
// real orientations and the mesher can read exact normals off them.
OpticalSurface glassBox(double halfXY, double z0, double z1, double index,
                        int priority = 0) {
    OpticalSurface o;
    o.shape = BRepPrimAPI_MakeBox(gp_Pnt(-halfXY, -halfXY, z0),
                                  gp_Pnt(halfXY, halfXY, z1)).Shape();
    o.label = QStringLiteral("box");
    o.index = index;
    o.transmissivity = 1.0;
    o.mediumPriority = priority;
    return o;
}

OpticalSurface planarDetector(double size, double z) {
    const double h = 0.5 * size;
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(-h, -h, z));
    poly.Add(gp_Pnt(h, -h, z));
    poly.Add(gp_Pnt(h, h, z));
    poly.Add(gp_Pnt(-h, h, z));
    poly.Close();
    OpticalSurface o;
    o.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    o.label = QStringLiteral("Detector");
    o.isDetector = true;
    return o;
}

} // namespace

TEST(media, a_solid_is_entered_once_and_left_once) {
    // One ray straight through a glass block. The bulk term is charged over the
    // block's thickness and over nothing else, which only holds if the entry
    // face pushed the medium and the exit face popped it.
    const double alpha = 0.01, thickness = 40.0;
    OpticalSurface box = glassBox(200.0, 20.0, 20.0 + thickness, 1.5);
    box.absorption = alpha;

    TraceScene scene;
    scene.build(MeshBuilder::build({box, planarDetector(600.0, 120.0)}));

    SimulationResult res;
    RayTracer::traceSingleRay(scene, Vec3(0, 0, -10), Vec3(0, 0, 1), res);
    CHECK_NEAR(res.fluxDetector, std::exp(-alpha * thickness), 1e-5);
    CHECK_NEAR(res.fluxAccounted(), 1.0, 1e-9);
}

TEST(media, a_nested_solid_refracts_against_its_host_not_against_air) {
    // A denser block wholly inside a lighter one. Crossing the inner boundary is
    // a 1.5 -> 1.8 interface, so its critical angle is the one for that pair --
    // and it has none at all in the entering direction. Treating the crossing as
    // glass -> air (which is what a single medium index forces) would give the
    // inner surface a critical angle of 33.7 degrees and lose the ray.
    OpticalSurface outer = glassBox(200.0, 0.0, 200.0, 1.5);
    OpticalSurface inner = glassBox(60.0, 60.0, 140.0, 1.8);
    inner.mediumPriority = 1;

    TraceScene scene;
    scene.build(MeshBuilder::build({outer, inner, planarDetector(900.0, 400.0)}));

    // 40 degrees inside the outer glass is past the 1.5 -> 1.0 critical angle
    // (41.8 is the limit, and the tilt below is measured after refraction), so
    // this is exactly the case a single-index tracer gets wrong.
    for (double tiltDeg : {0.0, 15.0, 30.0}) {
        const double a = tiltDeg * kPi / 180.0;
        SimulationResult res;
        RayTracer::traceSingleRay(scene, Vec3(0, 0, -10),
                                  Vec3(std::sin(a), 0.0, std::cos(a)), res);
        CHECK(res.fluxDetector > 0.9);
        CHECK_NEAR(res.fluxAccounted(), 1.0, 1e-9);
    }
}

TEST(media, priority_resolves_two_solids_that_overlap) {
    // The same two blocks, overlapping rather than nested. Inside the overlap
    // the ray is in both; the higher priority is the one that governs, so the
    // exit index is the inner block's and not whichever face happened to be
    // crossed last.
    OpticalSurface a = glassBox(200.0, 0.0, 120.0, 1.5, /*priority=*/0);
    OpticalSurface b = glassBox(200.0, 80.0, 200.0, 1.9, /*priority=*/5);

    TraceScene scene;
    scene.build(MeshBuilder::build({a, b, planarDetector(900.0, 400.0)}));

    SimulationResult res;
    RayTracer::traceSingleRay(scene, Vec3(0, 0, -10), Vec3(0, 0, 1), res);
    // Normal incidence: nothing bends and nothing is lost, whatever the indices,
    // so the only thing this can catch is a stack that lost its footing.
    CHECK_NEAR(res.fluxDetector, 1.0, 1e-9);
    CHECK_NEAR(res.fluxAccounted(), 1.0, 1e-9);
}

TEST(media, an_exact_normal_beats_the_facet_it_was_read_from) {
    // A sphere meshed coarsely. Every shading normal must sit within a whisker
    // of the analytic normal at its own vertex -- that is what "read off the
    // B-Rep" means -- while the facet normals are visibly off it.
    OpticalSurface ball;
    ball.shape = BRepPrimAPI_MakeSphere(50.0).Shape();
    ball.meshAngle = 0.5;
    ball.meshDeflection = 2.0;
    ball.index = 1.5;

    const MeshList meshes = MeshBuilder::build({ball});
    CHECK(meshes.size() == 1);
    const MeshSurface& m = meshes[0];
    CHECK(m.vnorm.size() == m.verts.size());
    CHECK(m.tris.size() > 8);

    double worstVertex = 0.0, worstFacet = 0.0;
    for (const auto& t : m.tris) {
        for (int k = 0; k < 3; ++k) {
            const int vi = (k == 0) ? t.v0 : (k == 1) ? t.v1 : t.v2;
            Vec3 exact(m.verts[std::size_t(vi)].X(), m.verts[std::size_t(vi)].Y(),
                       m.verts[std::size_t(vi)].Z());
            exact.normalize();                       // outward radius == normal
            const Vec3 got(m.vnorm[std::size_t(vi)]);
            worstVertex = std::max(worstVertex, std::acos(std::clamp(got.dot(exact), -1.0, 1.0)));
            const Vec3 fn(t.normal);
            worstFacet = std::max(worstFacet, std::acos(std::clamp(fn.dot(exact), -1.0, 1.0)));
        }
    }
    CHECK(worstVertex < 1e-6);          // exact, to the evaluation tolerance
    CHECK(worstFacet > 0.05);           // and the facet it came from is not
}

TEST(media, winding_points_every_facet_out_of_the_solid) {
    // A closed solid's facet normals must all point outward, or "d . n > 0"
    // cannot mean "leaving" and the medium stack has nothing to stand on.
    // Testing it on the sphere means the outward direction is known in closed
    // form at every facet.
    OpticalSurface ball;
    ball.shape = BRepPrimAPI_MakeSphere(30.0).Shape();
    ball.index = 1.5;

    const MeshList meshes = MeshBuilder::build({ball});
    CHECK(meshes.size() == 1);
    const MeshSurface& m = meshes[0];
    CHECK(m.tris.size() > 100);

    int outward = 0;
    for (const auto& t : m.tris) {
        const gp_Pnt& A = m.verts[std::size_t(t.v0)];
        const gp_Pnt& B = m.verts[std::size_t(t.v1)];
        const gp_Pnt& C = m.verts[std::size_t(t.v2)];
        Vec3 centroid((A.X() + B.X() + C.X()) / 3.0, (A.Y() + B.Y() + C.Y()) / 3.0,
                      (A.Z() + B.Z() + C.Z()) / 3.0);
        centroid.normalize();
        if (Vec3(t.normal).dot(centroid) > 0.0) ++outward;
    }
    CHECK(outward == int(m.tris.size()));
}

TEST(media, russian_roulette_keeps_the_budget_closed_and_the_answer_unbiased) {
    // Roulette conserves energy only in expectation, so the residual is booked
    // rather than hidden: the five buckets still sum to the emitted power to the
    // last bit, and the residual itself averages to nothing.
    for (int s : {int(GeometryProvider::Scene::LightGuide),
                  int(GeometryProvider::Scene::IntegratingSphere),
                  int(GeometryProvider::Scene::BiconvexLens)}) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene(s), 20000);
        const SimulationResult res = Simulation::run(cfg);
        CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
        // The estimator's noise is small next to the answer it is estimating.
        CHECK(std::fabs(res.fluxRoulette) < 0.02 * res.sourcePower);
    }
}

TEST(media, the_hard_cutoff_no_longer_eats_energy) {
    // Truncation used to be a physical loss channel that grew with path length.
    // It is now a diagnostic that a healthy run leaves at zero.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::LightGuide, 20000);
    const SimulationResult res = Simulation::run(cfg);
    CHECK_NEAR(res.fluxTruncated, 0.0, 1e-12);
}

TEST(media, arrivals_are_a_fair_sample_not_the_first_ones_seen) {
    // A guide produces far more arrivals than a small budget can keep, and the
    // ones it produces late are the long, heavily-bounced, low-energy paths.
    // Compared against the same run recording *every* arrival, a reservoir has
    // to reproduce the energy distribution; keeping the first N reproduces only
    // whatever the earliest trapped ray happened to do.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::LightGuide, 40000);
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    const SourceConfig src = Simulation::sourceFor(cfg, *data);

    auto meanArrivalEnergy = [&](std::size_t cap, std::size_t* kept) {
        TraceOptions opt;
        opt.seed       = cfg.seed;
        opt.physics    = cfg.physics;
        opt.arrivalCap = cap;
        SimulationResult res;
        RayTracer::trace(data->scene, src, res, opt);
        double sum = 0.0;
        for (const auto& a : res.arrivals) sum += double(a.energy);
        *kept = res.arrivals.size();
        return res.arrivals.empty() ? 0.0 : sum / double(res.arrivals.size());
    };

    std::size_t keptAll = 0, keptFew = 0;
    const double everyArrival = meanArrivalEnergy(4000000, &keptAll);
    const double sampled      = meanArrivalEnergy(2000, &keptFew);

    CHECK(keptAll > 20 * keptFew);          // the budget really is the binding limit
    CHECK(keptFew > 1000);
    CHECK(everyArrival > 0.0);
    // Within a few per cent, on a sample of two thousand. The censored version
    // of this sample sat far outside that.
    CHECK(std::fabs(sampled - everyArrival) < 0.15 * everyArrival);
}

TEST(media, optical_path_length_is_recorded_and_physical) {
    // Straight through a block of glass to a receiver: the optical path is the
    // geometric path with the glass counted n times over. Nothing else in the
    // analysis layer can produce an OPD map until this number exists.
    const double thickness = 40.0, index = 1.5, zDet = 120.0;
    TraceScene scene;
    scene.build(MeshBuilder::build({glassBox(200.0, 20.0, 20.0 + thickness, index),
                                    planarDetector(600.0, zDet)}));

    SimulationResult res;
    RayTracer::traceSingleRay(scene, Vec3(0, 0, -10), Vec3(0, 0, 1), res);
    CHECK(!res.arrivals.empty());
    const double geometric = zDet - (-10.0);
    const double expected  = geometric + (index - 1.0) * thickness;
    CHECK_NEAR(double(res.arrivals[0].opl), expected, 1e-3);
}

TEST(media, smooth_normals_shrink_the_focus_of_a_curved_mirror) {
    // The elliptical reflector transfers one focus to the other, so its spot is
    // a direct read of how well the tracer knows the mirror's normal. With the
    // facet normal it was tens of millimetres across; with the surface normal it
    // is a fraction of one.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 60000);
    const SimulationResult res = Simulation::run(cfg);
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
    CHECK(m.valid);
    CHECK(m.rmsRadius < 3.0);

    const studies::FocusStudy focus = studies::throughFocus(res, 60.0);
    CHECK(focus.valid);
    // And the best focus is where the ellipse says it is, not tens of
    // millimetres short of it.
    CHECK(std::fabs(focus.bestZ - focus.receiverZ) < 6.0);
}

// =============================================================== material ===

TEST(material, the_catalogue_reproduces_published_glass) {
    // Sellmeier coefficients are only worth shipping if they give back the
    // numbers the catalogue page they came from prints.
    struct Want { const char* name; double nd; double abbe; };
    const Want wants[] = {
        {"N-BK7",        1.5168, 64.2},
        {"N-SF11",       1.7847, 25.7},
        {"Fused Silica", 1.4585, 67.8},
    };
    for (const Want& w : wants) {
        const OpticalMaterial m = materials::byName(QLatin1String(w.name));
        CHECK(m.valid());
        CHECK_NEAR(m.indexAt(587.5618), w.nd, 5e-4);
        CHECK_NEAR(m.abbe(), w.abbe, 0.5);
    }
}

TEST(material, dispersion_is_monotonic_across_the_visible) {
    // Normal dispersion: blue always sees a higher index than red. A fit that
    // diverges outside the band it was made for would break this.
    for (int i = 0; i < materials::count(); ++i) {
        const OpticalMaterial m = materials::at(i);
        if (m.isMetal() || m.abbe() == 0.0) continue;
        double prev = m.indexAt(400.0);
        for (double l = 410.0; l <= 760.0; l += 10.0) {
            const double n = m.indexAt(l);
            CHECK(n <= prev + 1e-9);
            CHECK(n > 1.0 && n < 2.5);
            prev = n;
        }
    }
}

TEST(material, a_flint_disperses_far_more_than_a_crown) {
    const OpticalMaterial crown = materials::byName(QStringLiteral("N-BK7"));
    const OpticalMaterial flint = materials::byName(QStringLiteral("N-SF11"));
    const double dCrown = crown.indexAt(486.1) - crown.indexAt(656.3);
    const double dFlint = flint.indexAt(486.1) - flint.indexAt(656.3);
    CHECK(dFlint > 3.0 * dCrown);
}

TEST(material, metal_reflectance_matches_the_normal_incidence_formula) {
    // At normal incidence the complex Fresnel result collapses to
    // ((n-1)^2 + k^2) / ((n+1)^2 + k^2), which is worth pinning separately from
    // the general expression that has to reproduce it.
    for (const char* name : {"Aluminium", "Silver", "Gold"}) {
        const OpticalMaterial m = materials::byName(QLatin1String(name));
        CHECK(m.isMetal());
        for (double l : {450.0, 550.0, 650.0, 750.0}) {
            const double n = m.indexAt(l), k = m.extinctionAt(l);
            const double want = ((n - 1.0) * (n - 1.0) + k * k) /
                                ((n + 1.0) * (n + 1.0) + k * k);
            CHECK_NEAR(metalReflectance(1.0, n, k, 1.0), want, 1e-9);
        }
    }
}

TEST(material, metal_reflectance_dips_then_climbs_to_one_at_grazing) {
    // The pseudo-Brewster dip is the signature of an absorbing reflector, and it
    // is exactly what a flat 0.95 cannot express.
    const OpticalMaterial al = materials::byName(QStringLiteral("Aluminium"));
    const double n = al.indexAt(550.0), k = al.extinctionAt(550.0);
    const double normal = metalReflectance(1.0, n, k, 1.0);
    const double dip    = metalReflectance(1.0, n, k, std::cos(80.0 * kPi / 180.0));
    const double graze  = metalReflectance(1.0, n, k, std::cos(89.5 * kPi / 180.0));
    CHECK(normal > 0.90 && normal < 0.94);
    CHECK(dip < normal);
    CHECK(graze > normal);
    CHECK(graze < 1.0 + 1e-12);
    // And it never leaves [0, 1] anywhere in between.
    for (int d = 0; d <= 90; ++d) {
        const double R = metalReflectance(1.0, n, k, std::cos(d * kPi / 180.0));
        CHECK(R >= 0.0 && R <= 1.0);
    }
}

TEST(material, gold_is_a_poor_blue_mirror_and_a_good_red_one) {
    const OpticalMaterial au = materials::byName(QStringLiteral("Gold"));
    const double blue = metalReflectance(1.0, au.indexAt(450.0), au.extinctionAt(450.0), 1.0);
    const double red  = metalReflectance(1.0, au.indexAt(750.0), au.extinctionAt(750.0), 1.0);
    CHECK(blue < 0.55);
    CHECK(red  > 0.93);
}

TEST(material, a_surface_keeps_its_own_index_and_borrows_only_the_dispersion) {
    // Assigning a catalogue material sets the reference index to the published
    // value; overriding the index afterwards moves the whole curve rather than
    // being silently ignored, which is what lets a what-if trace work at all.
    SurfaceOptics s;
    s.material = materials::byName(QStringLiteral("N-BK7"));
    s.index    = s.material.nd;
    CHECK_NEAR(s.indexAt(587.5618), 1.5168, 5e-4);
    const double spread = s.indexAt(460.0) - s.indexAt(620.0);

    s.index = 1.9;
    CHECK_NEAR(s.indexAt(587.5618), 1.9, 5e-4);
    CHECK_NEAR(s.indexAt(460.0) - s.indexAt(620.0), spread, 1e-9);
    // And with dispersion switched off every wavelength collapses onto it.
    CHECK_NEAR(s.indexAt(460.0, false), 1.9, 1e-12);
}

// =============================================================== spectrum ===

TEST(spectrum, photopic_peaks_at_555_nm_and_dies_outside_the_visible) {
    const double peak = spectrum::photopic(555.0);
    CHECK(peak > 0.95);
    for (double l : {380.0, 400.0, 700.0, 760.0})
        CHECK(spectrum::photopic(l) < 0.1 * peak);
    CHECK_NEAR(spectrum::photopic(300.0), 0.0, 1e-12);
    CHECK_NEAR(spectrum::photopic(900.0), 0.0, 1e-12);
}

TEST(spectrum, band_weights_always_sum_to_one) {
    // This is the invariant the three colour grids stand on: they have to add up
    // to the irradiance grid, whatever the spectrum.
    for (auto kind : {SpectrumConfig::Kind::Rgb, SpectrumConfig::Kind::D65,
                      SpectrumConfig::Kind::Blackbody}) {
        SpectrumConfig cfg;
        cfg.kind = kind;
        SampledSpectrum spec;
        spec.build(cfg);
        for (double l = 380.0; l <= 780.0; l += 5.0) {
            double w[3];
            spec.bandWeights(l, w);
            CHECK(w[0] >= 0.0 && w[1] >= 0.0 && w[2] >= 0.0);
            CHECK_NEAR(w[0] + w[1] + w[2], 1.0, 1e-9);
        }
    }
}

TEST(spectrum, a_blackbody_gets_hotter_and_bluer) {
    // Wien's law: the mean of the sampled distribution has to move toward the
    // blue as the temperature rises.
    double previous = 1e9;
    for (double T : {2000.0, 3000.0, 5000.0, 8000.0}) {
        SpectrumConfig cfg;
        cfg.kind = SpectrumConfig::Kind::Blackbody;
        cfg.cct  = T;
        SampledSpectrum spec;
        spec.build(cfg);
        CHECK(spec.meanWavelength() < previous);
        CHECK(spec.meanWavelength() > 380.0 && spec.meanWavelength() < 780.0);
        previous = spec.meanWavelength();
    }
}

TEST(spectrum, sampling_reproduces_the_distribution_it_was_built_from) {
    // The inverse CDF has to give back the SPD: a histogram of many draws must
    // track D65's own shape, not a uniform band.
    SpectrumConfig cfg;
    cfg.kind = SpectrumConfig::Kind::D65;
    SampledSpectrum spec;
    spec.build(cfg);

    constexpr int kBins = 20;
    std::vector<double> hist(kBins, 0.0);
    std::uint64_t rng = 99;
    constexpr int kDraws = 200000;
    for (int i = 0; i < kDraws; ++i) {
        const double l = spec.sample(std::size_t(i), optics::uniform01(rng));
        CHECK(l >= 379.0 && l <= 781.0);
        const int b = std::clamp(int((l - 380.0) / 400.0 * kBins), 0, kBins - 1);
        hist[std::size_t(b)] += 1.0;
    }
    // D65 is broad, so every 20 nm bin has to carry real weight, and none of
    // them may run away with the distribution.
    for (double h : hist) {
        const double share = h / double(kDraws);
        CHECK(share > 0.01);
        CHECK(share < 0.12);
    }
}

TEST(spectrum, a_daylight_spectrum_has_a_believable_efficacy) {
    SpectrumConfig cfg;
    cfg.kind = SpectrumConfig::Kind::D65;
    SampledSpectrum spec;
    spec.build(cfg);
    // Full-spectrum daylight over 380..780 nm runs near 200 lm/W of radiant
    // power; a monochromatic 555 nm line is the 683 lm/W ceiling.
    CHECK(spec.efficacy() > 120.0 && spec.efficacy() < 300.0);

    SpectrumConfig mono;
    mono.wavelengthNm = 555.0;
    SampledSpectrum line;
    line.build(mono);
    CHECK_NEAR(line.efficacy(), 683.0, 40.0);
}

// ================================================================== units ===

TEST(units, a_run_in_watts_scales_linearly_with_the_source_flux) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    cfg.power = 1.0;
    const SimulationResult one = Simulation::run(cfg);
    cfg.power = 250.0;
    const SimulationResult big = Simulation::run(cfg);

    CHECK_NEAR(big.efficiency, one.efficiency, 1e-12);
    CHECK_NEAR(big.fluxDetector, 250.0 * one.fluxDetector, 1e-9);
    CHECK_NEAR(big.fluxAccounted(), 250.0, 1e-9);
    for (std::size_t i = 0; i < one.irradiance.size(); ++i)
        CHECK_NEAR(big.irradiance[i], 250.0 * one.irradiance[i], 1e-9);
}

TEST(units, a_photometric_run_delivers_the_lumens_it_was_asked_for) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 40000);
    cfg.spectrum.kind = SpectrumConfig::Kind::D65;
    cfg.fluxUnit      = FluxUnit::Lumen;
    cfg.power         = 1200.0;

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.unit == FluxUnit::Lumen);
    // Every bucket still closes, in lumens.
    CHECK_NEAR(res.fluxAccounted(), 1200.0, 1e-7);
    CHECK(res.fluxDetector > 0.0 && res.fluxDetector < 1200.0);
    CHECK(res.luminousEfficacy > 100.0);
}

TEST(units, photometric_weighting_favours_green_over_deep_red) {
    // Two monochromatic runs of the same geometry and the same lumens. The eye
    // sees almost nothing at 700 nm, so it takes far more radiant power to make
    // those lumens -- and the run reports that through the efficacy.
    auto efficacyAt = [](double nm) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
        cfg.spectrum.wavelengthNm = nm;
        cfg.fluxUnit = FluxUnit::Lumen;
        return Simulation::run(cfg).luminousEfficacy;
    };
    CHECK(efficacyAt(555.0) > 600.0);
    CHECK(efficacyAt(700.0) < 50.0);
    CHECK(efficacyAt(555.0) > 20.0 * efficacyAt(700.0));
}

TEST(units, the_two_units_agree_only_where_the_optics_are_colourless) {
    // A radiometric efficiency and a luminous one are the same ratio only for a
    // system that treats every wavelength alike. Switch the wavelength-dependent
    // physics off and the two must agree; switch it on and they must not --
    // which is the entire reason for carrying both.
    auto pair = [](bool chromatic) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 60000);
        cfg.spectrum.kind = SpectrumConfig::Kind::D65;
        cfg.physics.dispersion = chromatic;
        // An aluminium mirror's reflectance moves with wavelength; the fixed
        // split does not.
        cfg.physics.fresnel = chromatic;
        cfg.fluxUnit = FluxUnit::Watt;
        const SimulationResult w = Simulation::run(cfg);
        cfg.fluxUnit = FluxUnit::Lumen;
        const SimulationResult l = Simulation::run(cfg);
        return std::pair<SimulationResult, SimulationResult>(w, l);
    };

    const auto flat = pair(false);
    CHECK(std::fabs(flat.first.efficiency - flat.second.efficiency) <
          4.0 * (flat.first.efficiencyStdErr + flat.second.efficiencyStdErr) + 1e-9);

    const auto colour = pair(true);
    // Aluminium reflects less in the blue, and the eye weights the green; over
    // two bounces that is a real, reportable difference between the two units.
    const double gap = std::fabs(colour.first.efficiency - colour.second.efficiency);
    const double band = colour.first.efficiencyStdErr + colour.second.efficiencyStdErr;
    if (!(gap > 4.0 * band))
        std::printf("       chromatic gap %.6f vs error band %.6f\n", gap, band);
    CHECK(gap > 4.0 * band);
    // Real, but small: a mirror is not a filter.
    CHECK(gap < 0.05);
}

// ============================================================== detectors ===

TEST(detectors, resolution_is_settable_and_conserves_the_total) {
    // A finer grid redistributes the same flux into more bins; it must not
    // create or destroy any of it.
    double coarseTotal = 0.0;
    for (int bins : {32, 64, 256}) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
        cfg.detectorBins = bins;
        const SimulationResult res = Simulation::run(cfg);
        CHECK(res.nx == bins);
        CHECK(res.ny == bins);
        CHECK(int(res.irradiance.size()) == bins * bins);
        double sum = 0.0;
        for (double v : res.irradiance) sum += v;
        if (bins == 32) coarseTotal = sum;
        else            CHECK_NEAR(sum, coarseTotal, 1e-9);
    }
}

TEST(detectors, each_receiver_bins_against_its_own_frame) {
    // Two receivers in one scene. The second used to terminate rays and book
    // their flux against the *first* one's frame; each now carries its own grid,
    // and the two grids have to add up to the total.
    const double h = 200.0;
    auto plate = [](double size, double z, double cx, int bins) {
        const double s = 0.5 * size;
        BRepBuilderAPI_MakePolygon poly;
        poly.Add(gp_Pnt(cx - s, -s, z));
        poly.Add(gp_Pnt(cx + s, -s, z));
        poly.Add(gp_Pnt(cx + s, s, z));
        poly.Add(gp_Pnt(cx - s, s, z));
        poly.Close();
        OpticalSurface o;
        o.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
        o.label = QStringLiteral("Detector");
        o.isDetector = true;
        o.detNX = o.detNY = bins;
        return o;
    };
    std::vector<OpticalSurface> surfaces{plate(100.0, h, -80.0, 16),
                                         plate(100.0, h, +80.0, 32)};
    TraceScene scene;
    scene.build(MeshBuilder::build(surfaces));
    CHECK(scene.detectors().size() == 2);

    SourceConfig src;
    src.type         = SourceConfig::Type::Point;
    src.halfAngleDeg = 180.0;
    src.rays         = 200000;

    TraceOptions opt;
    opt.nTheta = 0;
    SimulationResult res;
    RayTracer::trace(scene, src, res, opt);

    CHECK(res.detectors.size() == 2);
    CHECK(res.detectors[0].nx == 16);
    CHECK(res.detectors[1].nx == 32);
    CHECK(res.detectors[0].flux > 0.0);
    CHECK(res.detectors[1].flux > 0.0);
    CHECK_NEAR(res.detectors[0].flux + res.detectors[1].flux, res.fluxDetector, 1e-9);

    // Symmetric placement, symmetric geometry: the two must agree to the noise.
    CHECK(std::fabs(res.detectors[0].flux - res.detectors[1].flux) <
          8.0 * res.efficiencyStdErr + 1e-9);

    // Each grid holds its own receiver's flux, not the other one's.
    for (const auto& d : res.detectors) {
        double sum = 0.0;
        for (double v : d.grid) sum += v;
        CHECK_NEAR(sum, d.flux, 1e-9);
    }
}

TEST(detectors, an_acceptance_cone_rejects_light_arriving_too_steeply) {
    // A photometer with a 20 degree acceptance sees only the rays inside it.
    // Narrowing the cone can only ever reduce what is counted.
    auto fluxWithCone = [](double coneDeg) {
        const double h = 200.0, size = 600.0;
        const double s = 0.5 * size;
        BRepBuilderAPI_MakePolygon poly;
        poly.Add(gp_Pnt(-s, -s, h));
        poly.Add(gp_Pnt(s, -s, h));
        poly.Add(gp_Pnt(s, s, h));
        poly.Add(gp_Pnt(-s, s, h));
        poly.Close();
        OpticalSurface o;
        o.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
        o.label = QStringLiteral("Detector");
        o.isDetector = true;
        o.detNX = o.detNY = 32;
        o.detAcceptanceDeg = coneDeg;

        TraceScene scene;
        scene.build(MeshBuilder::build({o}));

        SourceConfig src;
        src.type         = SourceConfig::Type::Point;
        src.halfAngleDeg = 180.0;
        src.rays         = 100000;
        TraceOptions opt;
        opt.nTheta = 0;
        SimulationResult res;
        RayTracer::trace(scene, src, res, opt);
        CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
        return res.fluxDetector;
    };

    const double open   = fluxWithCone(180.0);
    const double medium = fluxWithCone(40.0);
    const double tight  = fluxWithCone(10.0);
    CHECK(open > medium);
    CHECK(medium > tight);
    CHECK(tight > 0.0);

    // An isotropic source at distance h: the flux inside a half-angle theta of
    // the receiver normal is the solid-angle fraction (1 - cos theta) / 2.
    for (double cone : {10.0, 40.0}) {
        const double want = 0.5 * (1.0 - std::cos(cone * kPi / 180.0));
        CHECK_NEAR(fluxWithCone(cone), want, 0.02 * want + 0.002);
    }
}

TEST(detectors, a_tilted_receiver_reports_its_own_plane) {
    // The frame used to be "assume +Z". A receiver at 30 degrees has to describe
    // itself: its normal, its in-plane axes and its extents in that plane.
    const double a = 30.0 * kPi / 180.0;
    const Vec3 n(std::sin(a), 0.0, std::cos(a));
    Vec3 u(std::cos(a), 0.0, -std::sin(a));
    const Vec3 v(0, 1, 0);
    const Vec3 c(0, 0, 150.0);
    const double half = 60.0;

    BRepBuilderAPI_MakePolygon poly;
    for (int i = 0; i < 4; ++i) {
        const double su = (i == 0 || i == 3) ? -1.0 : 1.0;
        const double sv = (i < 2) ? -1.0 : 1.0;
        const Vec3 p = c + u * (su * half) + v * (sv * half);
        poly.Add(gp_Pnt(p.x, p.y, p.z));
    }
    poly.Close();
    OpticalSurface o;
    o.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    o.label = QStringLiteral("Tilted receiver");
    o.isDetector = true;
    o.detNX = o.detNY = 24;

    const MeshList meshes = MeshBuilder::build({o});
    CHECK(meshes.size() == 1);
    CHECK_NEAR(std::fabs(Vec3(meshes[0].detNormal).dot(n)), 1.0, 1e-6);
    CHECK_NEAR(meshes[0].detW, 2.0 * half, 1e-6);
    CHECK_NEAR(meshes[0].detH, 2.0 * half, 1e-6);
    CHECK_NEAR(Vec3(meshes[0].detCenter).z, c.z, 1e-6);

    TraceScene scene;
    scene.build(meshes);
    const DetectorInfo& d = scene.detector();
    // The frame is orthonormal and right-handed, whatever the tilt.
    CHECK_NEAR(d.u.dot(d.v), 0.0, 1e-9);
    CHECK_NEAR(d.u.dot(d.n), 0.0, 1e-9);
    CHECK_NEAR(d.u.cross(d.v).dot(d.n), 1.0, 1e-6);

    // And a ray aimed at a known point in that plane bins where it should.
    SimulationResult res;
    const Vec3 target = c + u * (0.5 * half);
    Vec3 dir = target;
    dir.normalize();
    RayTracer::traceSingleRay(scene, Vec3(0, 0, 0), dir, res);
    CHECK_NEAR(res.fluxDetector, 0.0, 1.0);      // it lands, whatever the optics
    CHECK(!res.arrivals.empty());
}

// ============================================================= photometry ===

TEST(photometry, ies_export_carries_the_candela_distribution) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 40000);
    cfg.spectrum.kind = SpectrumConfig::Kind::LedPhosphor;
    cfg.power         = 1000.0;
    cfg.fluxUnit      = FluxUnit::Lumen;
    const SimulationResult res = Simulation::run(cfg);

    CHECK(analysis::canExportPhotometry(res));
    const QString ies = analysis::iesLm63(res, QStringLiteral("Test"));
    CHECK(ies.startsWith(QStringLiteral("IESNA:LM-63-2002")));
    CHECK(ies.contains(QStringLiteral("TILT=NONE")));

    // The lamp lumens on the header line are the flux the far field carries, and
    // the table that follows has one candela value per (theta, phi) cell.
    const QStringList lines = ies.split(QLatin1Char('\n'));
    int tilt = -1;
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].startsWith(QStringLiteral("TILT="))) { tilt = i; break; }
    CHECK(tilt >= 0);
    const QStringList head = lines[tilt + 1].split(QLatin1Char(' '), Qt::SkipEmptyParts);
    CHECK(head.size() >= 5);
    CHECK_NEAR(head[1].toDouble(), res.intensity.totalFlux, 0.01 * res.intensity.totalFlux);
    CHECK(head[3].toInt() == res.intensity.nTheta);
    CHECK(head[4].toInt() == res.intensity.nPhi);

    double biggest = 0.0;
    for (int i = tilt + 3; i < lines.size(); ++i)
        for (const QString& tok : lines[i].split(QLatin1Char(' '), Qt::SkipEmptyParts))
            biggest = std::max(biggest, tok.toDouble());
    // The brightest number in the file is the brightest (theta, phi) cell -- not
    // the peak of the phi-averaged profile, which is a different quantity.
    double brightestCell = 0.0;
    for (double v : res.intensity.perSteradian) brightestCell = std::max(brightestCell, v);
    CHECK_NEAR(biggest, brightestCell, 0.02 * brightestCell + 1e-9);
}

TEST(photometry, eulumdat_export_is_normalised_per_1000_lumens) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    cfg.spectrum.kind = SpectrumConfig::Kind::D65;
    const SimulationResult res = Simulation::run(cfg);
    const QString ldt = analysis::eulumdat(res, QStringLiteral("Test"));
    CHECK(ldt.startsWith(QStringLiteral("LuxTrace")));
    const QStringList lines = ldt.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    // Header block plus the C-plane angles, the gamma angles and the table.
    CHECK(lines.size() > 42 + res.intensity.nTheta + res.intensity.nPhi);
    CHECK(lines[3].toInt() == res.intensity.nPhi);
    CHECK(lines[5].toInt() == res.intensity.nTheta);
}

TEST(photometry, a_run_outside_the_visible_band_refuses_rather_than_lying) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
    cfg.spectrum.wavelengthNm = 1550.0;      // telecom, invisible
    const SimulationResult res = Simulation::run(cfg);
    QString why;
    CHECK(!analysis::canExportPhotometry(res, &why));
    CHECK(!why.isEmpty());
}

// ============================================================= validation ===

TEST(validation, the_tracer_agrees_with_closed_form_optics) {
    // The same table --validate prints. Everything in it is derived outside the
    // tracer, so this is the test that says the physics is right rather than
    // merely self-consistent.
    const auto cases = studies::validate(120000);
    CHECK(cases.size() >= 6);
    for (const auto& c : cases) {
        if (!c.passed)
            std::printf("       validation case '%s': expected %.6g, got %.6g (tol %.3g)\n",
                        c.name.toUtf8().constData(), c.expected, c.measured, c.tolerance);
        CHECK(c.passed);
    }
}

// =============================================================== estimator ==
// Phase 3: low-discrepancy emission, aiming at the scene, next-event
// estimation, and an error bar that stays honest under all three.

TEST(estimator, sobol_covers_the_unit_square_far_more_evenly_than_random) {
    // The point of a low-discrepancy sequence: no cell of a grid is left empty
    // and none is crowded. Measured as the worst departure from the expected
    // count over a 16 x 16 grid of a 256-point set, which for a stratified
    // sequence is zero and for random samples is large.
    constexpr int kSide = 16, kPoints = kSide * kSide;
    auto worstCell = [](bool sobol) {
        std::vector<int> hist(kPoints, 0);
        std::uint64_t rng = 7;
        for (int i = 0; i < kPoints; ++i) {
            const double x = sobol ? sampling::sobol(std::uint32_t(i), 0, 12345u)
                                   : optics::uniform01(rng);
            const double y = sobol ? sampling::sobol(std::uint32_t(i), 1, 12345u)
                                   : optics::uniform01(rng);
            const int cx = std::clamp(int(x * kSide), 0, kSide - 1);
            const int cy = std::clamp(int(y * kSide), 0, kSide - 1);
            ++hist[std::size_t(cy) * kSide + std::size_t(cx)];
        }
        int worst = 0;
        for (int c : hist) worst = std::max(worst, std::abs(c - 1));
        return worst;
    };
    // Sobol in base 2 puts exactly one point in every cell of a 16 x 16 grid at
    // 256 points; Owen scrambling preserves that.
    CHECK(worstCell(true) == 0);
    CHECK(worstCell(false) >= 2);
}

TEST(estimator, sobol_stays_inside_the_unit_interval_in_every_dimension) {
    for (int dim = 0; dim < 10; ++dim) {
        double lo = 1.0, hi = 0.0, mean = 0.0;
        constexpr int kN = 4096;
        for (int i = 0; i < kN; ++i) {
            const double v = sampling::sobol(std::uint32_t(i), dim, 99u);
            CHECK(v >= 0.0 && v < 1.0);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            mean += v;
        }
        // Uniform on [0, 1): mean a half, and both ends reached.
        CHECK_NEAR(mean / double(kN), 0.5, 0.02);
        CHECK(lo < 0.02);
        CHECK(hi > 0.98);
    }
}

TEST(estimator, a_scrambled_sequence_is_reproducible_and_seed_dependent) {
    for (int i = 0; i < 64; ++i) {
        CHECK_NEAR(sampling::sobol(std::uint32_t(i), 2, 5u),
                   sampling::sobol(std::uint32_t(i), 2, 5u), 0.0);
    }
    int different = 0;
    for (int i = 0; i < 64; ++i)
        if (sampling::sobol(std::uint32_t(i), 2, 5u) !=
            sampling::sobol(std::uint32_t(i), 2, 6u)) ++different;
    CHECK(different > 60);
}

namespace {

// Efficiency and reported error over `runs` independent seeds.
struct SeedSpread {
    double mean = 0.0;
    double observed = 0.0;   // standard error of the mean, measured across seeds
    double reported = 0.0;   // what a single run said its error was
};

SeedSpread spreadOverSeeds(SimConfig cfg, int runs) {
    std::vector<double> eff;
    double reported = 0.0;
    for (int k = 0; k < runs; ++k) {
        cfg.seed = 1000u + std::uint64_t(k) * 7919u;
        const SimulationResult r = Simulation::run(cfg);
        eff.push_back(r.efficiency);
        reported += r.efficiencyStdErr;
    }
    SeedSpread s;
    for (double e : eff) s.mean += e;
    s.mean /= double(eff.size());
    double m2 = 0.0;
    for (double e : eff) m2 += (e - s.mean) * (e - s.mean);
    s.observed = std::sqrt(m2 / (double(eff.size()) - 1.0));
    s.reported = reported / double(runs);
    return s;
}

} // namespace

TEST(estimator, the_reported_error_bar_matches_the_spread_it_claims_to_predict) {
    // The one claim an error bar makes: run it again with a different seed and
    // the answer moves by about this much. Under a low-discrepancy sequence the
    // ray-to-ray spread is no longer that number, which is why the error is
    // measured across independent scrambles instead -- and this is the test that
    // says the replacement is honest rather than merely smaller.
    for (int s : {int(GeometryProvider::Scene::Reflector),
                  int(GeometryProvider::Scene::BiconvexLens),
                  int(GeometryProvider::Scene::IntegratingSphere)}) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene(s), 20000);
        const SeedSpread sp = spreadOverSeeds(cfg, 12);
        CHECK(sp.observed > 0.0);
        CHECK(sp.reported > 0.0);
        // Twelve seeds pin the observed spread down to roughly 20 %, so a factor
        // of 2.5 either way is the tightest honest band. What matters is that
        // the reported figure is not an order of magnitude adrift in either
        // direction: optimistic would be a lie, pessimistic would throw the
        // whole point of the sampler away.
        const double ratio = sp.reported / sp.observed;
        if (!(ratio > 0.4 && ratio < 2.5))
            std::printf("       scene %d: reported %.5g, observed %.5g (ratio %.2f)\n",
                        s, sp.reported, sp.observed, ratio);
        CHECK(ratio > 0.4 && ratio < 2.5);
    }
}

TEST(estimator, low_discrepancy_sampling_beats_random_at_the_same_ray_count) {
    // The payoff, measured rather than asserted: the same ray budget, the same
    // scene, and a smaller spread over independent runs.
    auto spread = [](bool ld) {
        std::vector<double> eff;
        for (int k = 0; k < 12; ++k) {
            SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 8000);
            cfg.seed = 500u + std::uint64_t(k) * 104729u;
            const Simulation::SceneRef data = Simulation::dataFor(cfg);
            TraceOptions opt;
            opt.seed    = cfg.seed;
            opt.physics = cfg.physics;
            opt.estimator.lowDiscrepancy = ld;
            SimulationResult r;
            RayTracer::trace(data->scene, Simulation::sourceFor(cfg, *data), r, opt);
            eff.push_back(r.efficiency);
        }
        double mean = 0.0;
        for (double e : eff) mean += e;
        mean /= double(eff.size());
        double m2 = 0.0;
        for (double e : eff) m2 += (e - mean) * (e - mean);
        return std::sqrt(m2 / (double(eff.size()) - 1.0));
    };
    const double random = spread(false);
    const double sobol  = spread(true);
    CHECK(random > 0.0);
    CHECK(sobol < random);
    if (!(sobol < 0.5 * random))
        std::printf("       sobol spread %.5g vs random %.5g\n", sobol, random);
    CHECK(sobol < 0.5 * random);
}

TEST(estimator, aiming_at_the_scene_does_not_move_the_answer) {
    // A source far enough from the optic that most of its sky is empty: aiming
    // stops tracing into the void, and the efficiency it reports must be the one
    // the brute-force run reports.
    auto run = [](bool aim, std::uint64_t seed) {
        const double h = 400.0;
        const double s = 30.0;
        BRepBuilderAPI_MakePolygon poly;
        poly.Add(gp_Pnt(-s, -s, h));
        poly.Add(gp_Pnt(s, -s, h));
        poly.Add(gp_Pnt(s, s, h));
        poly.Add(gp_Pnt(-s, s, h));
        poly.Close();
        OpticalSurface d;
        d.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
        d.label = QStringLiteral("Detector");
        d.isDetector = true;
        d.detNX = d.detNY = 16;

        TraceScene scene;
        scene.build(MeshBuilder::build({d}));

        SourceConfig src;
        src.type         = SourceConfig::Type::Point;
        src.halfAngleDeg = 180.0;
        src.rays         = 60000;

        TraceOptions opt;
        opt.seed   = seed;
        opt.nTheta = 30;
        opt.nPhi   = 12;
        opt.estimator.aimAtScene    = aim;
        opt.estimator.lowDiscrepancy = false;
        SimulationResult r;
        RayTracer::trace(scene, src, r, opt);
        return r;
    };

    double aimed = 0.0, plain = 0.0, aimedErr = 0.0, plainErr = 0.0;
    constexpr int kRuns = 8;
    for (int k = 0; k < kRuns; ++k) {
        const SimulationResult a = run(true, 900u + std::uint64_t(k) * 31u);
        const SimulationResult b = run(false, 900u + std::uint64_t(k) * 31u);
        aimed += a.efficiency;  aimedErr += a.efficiencyStdErr;
        plain += b.efficiency;  plainErr += b.efficiencyStdErr;
        // Every bucket still closes, aiming or not.
        CHECK_NEAR(a.fluxAccounted(), a.sourcePower, 1e-9);
        CHECK_NEAR(b.fluxAccounted(), b.sourcePower, 1e-9);
    }
    aimed /= kRuns; plain /= kRuns; aimedErr /= kRuns; plainErr /= kRuns;

    // The closed form: an isotropic source sees the receiver through the solid
    // angle it subtends.
    const double a2 = 30.0 * 30.0, h2 = 400.0 * 400.0;
    const double want = 4.0 * std::asin(a2 / (a2 + h2)) / (4.0 * kPi);
    CHECK_NEAR(aimed, want, 0.06 * want);
    CHECK_NEAR(plain, want, 0.06 * want);

    // And the point of it: far fewer rays wasted on directions that hit nothing.
    if (!(aimedErr < 0.5 * plainErr))
        std::printf("       aimed err %.5g vs plain %.5g\n", aimedErr, plainErr);
    CHECK(aimedErr < 0.5 * plainErr);
}

TEST(estimator, aiming_still_reports_where_the_missed_light_went) {
    // Aiming must not delete the part of the beam that misses the optic: that is
    // exactly the part a luminaire designer is looking at. An isotropic source
    // has to stay flat per steradian whether or not its rays were aimed.
    auto profileOf = [](bool aim) {
        TraceScene scene;
        scene.build(nearlyEmptyScene());
        SourceConfig src;
        src.type         = SourceConfig::Type::Point;
        src.halfAngleDeg = 180.0;
        src.rays         = 200000;
        TraceOptions opt;
        opt.nTheta = 9;
        opt.nPhi   = 8;
        opt.estimator.aimAtScene = aim;
        SimulationResult r;
        RayTracer::trace(scene, src, r, opt);
        return r;
    };
    const SimulationResult aimed = profileOf(true);
    CHECK(aimed.intensity.valid());
    double mean = 0.0;
    for (double v : aimed.intensity.profile) mean += v;
    mean /= double(aimed.intensity.profile.size());
    CHECK(mean > 0.0);
    for (double v : aimed.intensity.profile) CHECK_NEAR(v / mean, 1.0, 0.06);
    CHECK_NEAR(mean * 4.0 * kPi, aimed.sourcePower, 0.02);
}

TEST(estimator, next_event_estimation_does_not_move_the_answer) {
    // The scenes where it fires: a diffusing cavity and a transmissive diffuser.
    // Connecting each bounce to the receiver analytically has to give the same
    // efficiency the random walk gives, only sooner.
    for (int s : {int(GeometryProvider::Scene::IntegratingSphere),
                  int(GeometryProvider::Scene::DiffuserPlate)}) {
        auto run = [&](bool nee) {
            SimConfig cfg = quickConfig(GeometryProvider::Scene(s), 60000);
            const Simulation::SceneRef data = Simulation::dataFor(cfg);
            TraceOptions opt;
            opt.seed    = cfg.seed;
            opt.physics = cfg.physics;
            opt.estimator.nextEventEstimation = nee;
            SimulationResult r;
            RayTracer::trace(data->scene, Simulation::sourceFor(cfg, *data), r, opt);
            return r;
        };
        const SimulationResult with = run(true);
        const SimulationResult without = run(false);

        CHECK_NEAR(with.fluxAccounted(), with.sourcePower, 1e-9);
        const double band = 4.0 * (with.efficiencyStdErr + without.efficiencyStdErr) + 1e-6;
        if (!(std::fabs(with.efficiency - without.efficiency) < band))
            std::printf("       scene %d: nee %.5f vs plain %.5f (band %.5f)\n",
                        s, with.efficiency, without.efficiency, band);
        CHECK(std::fabs(with.efficiency - without.efficiency) < band);
    }
}

TEST(estimator, next_event_estimation_tightens_a_diffusing_cavity) {
    auto err = [](bool nee) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene::IntegratingSphere, 40000);
        const Simulation::SceneRef data = Simulation::dataFor(cfg);
        TraceOptions opt;
        opt.seed    = cfg.seed;
        opt.physics = cfg.physics;
        opt.estimator.nextEventEstimation = nee;
        SimulationResult r;
        RayTracer::trace(data->scene, Simulation::sourceFor(cfg, *data), r, opt);
        return r.efficiencyStdErr;
    };
    const double with = err(true), without = err(false);
    CHECK(with > 0.0 && without > 0.0);
    if (!(with < without))
        std::printf("       nee err %.5g vs plain %.5g\n", with, without);
    CHECK(with < without);
}

TEST(estimator, every_estimator_leaves_the_budget_closed_to_the_last_bit) {
    // Roulette, aiming and next-event estimation all trade exact per-run
    // bookkeeping for variance, and all book the difference. The four physical
    // buckets plus that residual still sum to the emitted power exactly.
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene(s), 4000);
        cfg.power = 37.5;
        const SimulationResult r = Simulation::run(cfg);
        CHECK_NEAR(r.fluxAccounted(), r.sourcePower, 1e-7);
    }
}

// =============================================================== instances ==

namespace {

// One ball lens, placed in a grid. The same optic either way: `instanced`
// chooses whether it is described as one part with placements or as that many
// separate shapes.
std::vector<OpticalSurface> lensGrid(int n, double radius, double pitch, bool instanced) {
    std::vector<OpticalSurface> out;
    OpticalSurface lens;
    lens.index          = 1.5;
    lens.transmissivity = 1.0;
    lens.fresnel        = false;
    lens.label          = QStringLiteral("Lens");
    lens.meshAngle      = 0.10;

    const double x0 = -0.5 * pitch * (n - 1);
    if (instanced) {
        lens.shape = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), radius).Shape();
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                gp_Trsf t;
                t.SetTranslation(gp_Vec(x0 + i * pitch, x0 + j * pitch, 0.0));
                lens.placements.push_back(t);
            }
        out.push_back(lens);
    } else {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                builder.Add(compound, BRepPrimAPI_MakeSphere(
                    gp_Pnt(x0 + i * pitch, x0 + j * pitch, 0.0), radius).Shape());
        lens.shape = compound;
        out.push_back(lens);
    }

    const double half = 0.5 * std::max(400.0, 3.0 * pitch * n);
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(-half, -half, 300.0));
    poly.Add(gp_Pnt(half, -half, 300.0));
    poly.Add(gp_Pnt(half, half, 300.0));
    poly.Add(gp_Pnt(-half, half, 300.0));
    poly.Close();
    OpticalSurface det;
    det.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    det.label = QStringLiteral("Detector");
    det.isDetector = true;
    det.detNX = det.detNY = 32;
    out.push_back(det);
    return out;
}

} // namespace

TEST(instances, one_mesh_stands_in_for_every_placement) {
    TraceScene scene;
    scene.build(MeshBuilder::build(lensGrid(5, 6.0, 18.0, /*instanced=*/true)));

    CHECK(scene.instances().size() == 25);
    CHECK(scene.instancedTriangles() > 100);
    // The whole point: the triangles are stored once, not twenty-five times.
    CHECK(scene.triangles().size() < 16);          // only the receiver
    CHECK(!scene.empty());

    // And every placement really is somewhere different.
    double spread = 0.0;
    for (const auto& a : scene.instances())
        for (const auto& b : scene.instances())
            spread = std::max(spread, (a.origin - b.origin).length());
    CHECK(spread > 60.0);
}

TEST(instances, the_hierarchy_agrees_with_brute_force_over_the_placements) {
    TraceScene scene;
    scene.build(MeshBuilder::build(lensGrid(4, 8.0, 22.0, /*instanced=*/true)));

    Rng rng(4242);
    int hits = 0;
    for (int i = 0; i < 3000; ++i) {
        const Vec3 o(rng.range(-60.0, 60.0), rng.range(-60.0, 60.0), -80.0);
        Vec3 d(rng.range(-0.5, 0.5), rng.range(-0.5, 0.5), 1.0);
        d.normalize();
        RayHit fast, slow;
        const bool a = scene.nearestHit(o, d, fast);
        const bool b = scene.nearestHitBruteForce(o, d, slow);
        CHECK(a == b);
        if (!a) continue;
        ++hits;
        CHECK_NEAR(fast.t, slow.t, 1e-9);
        CHECK(scene.surfaceIndexOf(fast) == scene.surfaceIndexOf(slow));
        // The normals resolve to the same world-space direction either way.
        CHECK_NEAR(scene.geometricNormal(fast).dot(scene.geometricNormal(slow)), 1.0, 1e-9);
    }
    CHECK(hits > 500);
}

TEST(instances, an_instanced_array_traces_the_same_optic_as_a_copied_one) {
    // The physics cannot depend on how the geometry was described. Same mesh
    // fineness on both sides, so the only difference is instancing itself.
    auto run = [](bool instanced) {
        TraceScene scene;
        scene.build(MeshBuilder::build(lensGrid(3, 8.0, 24.0, instanced)));
        SourceConfig src;
        src.type       = SourceConfig::Type::Collimated;
        src.beamRadius = 36.0;
        src.origin     = gp_Pnt(0, 0, -60.0);
        src.axis       = gp_Dir(0, 0, 1);
        src.rays       = 60000;
        TraceOptions opt;
        opt.nTheta = 30;
        opt.nPhi   = 12;
        SimulationResult r;
        RayTracer::trace(scene, src, r, opt);
        return r;
    };
    const SimulationResult inst = run(true);
    const SimulationResult copy = run(false);

    CHECK_NEAR(inst.fluxAccounted(), inst.sourcePower, 1e-9);
    CHECK_NEAR(copy.fluxAccounted(), copy.sourcePower, 1e-9);
    CHECK(inst.efficiency > 0.05);
    const double band = 5.0 * (inst.efficiencyStdErr + copy.efficiencyStdErr) + 2e-3;
    if (!(std::fabs(inst.efficiency - copy.efficiency) < band))
        std::printf("       instanced %.5f vs copied %.5f (band %.5f)\n",
                    inst.efficiency, copy.efficiency, band);
    CHECK(std::fabs(inst.efficiency - copy.efficiency) < band);
}

TEST(instances, describing_a_part_once_costs_far_less_to_build) {
    // The reason the microlens array needed a hand-coarsened mesh: twenty-five
    // copies meant twenty-five tessellations and a hierarchy over all of them.
    auto meshCount = [](bool instanced) {
        const MeshList meshes = MeshBuilder::build(lensGrid(5, 6.0, 18.0, instanced));
        std::size_t tris = 0;
        for (const auto& m : meshes) tris += m.tris.size();
        return tris;
    };
    const std::size_t instanced = meshCount(true);
    const std::size_t copied    = meshCount(false);
    CHECK(instanced > 0);
    // One lenslet plus a receiver, against twenty-five lenslets plus a receiver.
    CHECK(copied > 15 * instanced);
}

TEST(instances, a_rigid_placement_preserves_distances) {
    // The transform is rotation and translation only, which is what lets a hit
    // distance found inside a placement be used as a world distance without
    // rescaling. A scaled placement would silently break every `t` the tracer
    // compares.
    OpticalSurface lens;
    lens.shape          = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 10.0).Shape();
    lens.index          = 1.5;
    lens.transmissivity = 1.0;
    lens.label          = QStringLiteral("Lens");
    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)), 0.7);
    t.SetTranslationPart(gp_Vec(40.0, 0.0, 0.0));
    lens.placements = {gp_Trsf{}, t};

    TraceScene scene;
    scene.build(MeshBuilder::build({lens}));
    CHECK(scene.instances().size() == 2);
    for (const auto& in : scene.instances()) {
        // Orthonormal rows, and the inverse really is the transpose.
        CHECK_NEAR(in.r0.length(), 1.0, 1e-12);
        CHECK_NEAR(in.r1.length(), 1.0, 1e-12);
        CHECK_NEAR(in.r2.length(), 1.0, 1e-12);
        CHECK_NEAR(in.r0.dot(in.r1), 0.0, 1e-12);
        Rng rng(7);
        for (int i = 0; i < 40; ++i) {
            const Vec3 p(rng.range(-50, 50), rng.range(-50, 50), rng.range(-50, 50));
            const Vec3 back = in.dirToWorld(in.toLocal(p)) + in.origin;
            CHECK_NEAR((back - p).length(), 0.0, 1e-9);
        }
    }

    // A ray aimed straight at the second placement finds it at the distance the
    // geometry says, not at some rescaled one.
    RayHit hit;
    Vec3 d(1, 0, 0);
    CHECK(scene.nearestHit(Vec3(-100, 0, 0), d, hit));
    CHECK_NEAR(hit.t, 90.0, 0.2);       // the sphere at the origin, radius 10
}

// ================================================================== cad =====
// The competitive gap the review put first: an illumination tool is bought to
// be run on a customer's CAD, not on twenty-five demonstration scenes.

namespace {

// Writes a shape out as STEP so the reader can be tested against a real file
// rather than against a mock of one.
bool writeStep(const TopoDS_Shape& shape, const QString& path) {
    STEPControl_Writer writer;
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) return false;
    return writer.Write(path.toLocal8Bit().constData()) == IFSelect_RetDone;
}

QString scratchFile(const QString& name) {
    return QDir(QDir::tempPath()).filePath(name);
}

} // namespace

TEST(cad, a_step_file_round_trips_into_traceable_geometry) {
    const QString path = scratchFile(QStringLiteral("luxtrace_ball.step"));
    QFile::remove(path);
    const TopoDS_Shape ball = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 25.0).Shape();
    CHECK(writeStep(ball, path));

    const cadimport::ImportResult r = cadimport::read(path);
    if (!r.ok) std::printf("       import failed: %s\n", r.error.toUtf8().constData());
    CHECK(r.ok);
    CHECK(r.format == QStringLiteral("STEP"));
    CHECK(!r.shapes.empty());
    CHECK(r.totalFaces >= 1);
    // A 50 mm ball, whatever the file called it.
    CHECK_NEAR(r.bboxMax[0] - r.bboxMin[0], 50.0, 0.5);
    CHECK_NEAR(r.bboxMax[2] - r.bboxMin[2], 50.0, 0.5);

    // And it meshes and traces like any other optic.
    std::vector<OpticalSurface> surfaces =
        cadimport::toSurfaces(r, QStringLiteral("N-BK7"), /*reflective=*/false);
    CHECK(!surfaces.empty());
    CHECK(surfaces[0].index > 1.4);
    surfaces.push_back(cadimport::makeReceiver(400.0, 200.0, 32));

    TraceScene scene;
    scene.build(MeshBuilder::build(surfaces));
    CHECK(scene.triangles().size() > 100);
    CHECK(scene.detector().valid);

    SourceConfig src;
    src.type       = SourceConfig::Type::Collimated;
    src.beamRadius = 20.0;
    src.origin     = gp_Pnt(0, 0, -80.0);
    src.axis       = gp_Dir(0, 0, 1);
    src.rays       = 20000;
    SimulationResult res;
    RayTracer::trace(scene, src, res);

    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
    CHECK(res.efficiency > 0.1);
    QFile::remove(path);
}

TEST(cad, an_assembly_arrives_as_one_surface_per_part) {
    const QString path = scratchFile(QStringLiteral("luxtrace_assembly.step"));
    QFile::remove(path);
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (int i = 0; i < 3; ++i)
        builder.Add(compound, BRepPrimAPI_MakeBox(
            gp_Pnt(30.0 * i, 0, 0), 10.0, 10.0, 10.0).Shape());
    CHECK(writeStep(compound, path));

    const cadimport::ImportResult r = cadimport::read(path);
    CHECK(r.ok);
    // Three parts, not one lump: which is what lets a lens and its housing carry
    // different optics without going back to the CAD tool.
    CHECK(r.shapes.size() == 3);
    for (const auto& s : r.shapes) CHECK(s.faceCount == 6);
    CHECK(r.totalFaces == 18);

    // And a single part splits further, one surface per face.
    const auto faces = cadimport::facesOf(r.shapes[0], QStringLiteral("Aluminium"),
                                          /*reflective=*/true);
    CHECK(faces.size() == 6);
    for (const auto& f : faces) {
        CHECK(f.reflectivity > 0.5);
        CHECK(f.index == 0.0);
    }
    QFile::remove(path);
}

TEST(cad, the_scale_factor_converts_a_file_in_the_wrong_units) {
    // Metres against millimetres is the mismatch that bites first, and it is a
    // multiplication rather than a re-export.
    const QString path = scratchFile(QStringLiteral("luxtrace_scale.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 2.0, 2.0, 2.0).Shape(), path));

    const cadimport::ImportResult one = cadimport::read(path, 1.0);
    const cadimport::ImportResult big = cadimport::read(path, 1000.0);
    CHECK(one.ok && big.ok);
    CHECK_NEAR(one.bboxMax[0] - one.bboxMin[0], 2.0, 1e-6);
    CHECK_NEAR(big.bboxMax[0] - big.bboxMin[0], 2000.0, 1e-3);
    QFile::remove(path);
}

TEST(cad, a_file_that_is_not_cad_is_reported_rather_than_thrown) {
    const QString path = scratchFile(QStringLiteral("luxtrace_notcad.step"));
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("this is not a STEP file, it is a sentence.\n");
    }
    const cadimport::ImportResult r = cadimport::read(path);
    CHECK(!r.ok);
    CHECK(!r.error.isEmpty());
    QFile::remove(path);

    const cadimport::ImportResult missing =
        cadimport::read(scratchFile(QStringLiteral("luxtrace_no_such_file.step")));
    CHECK(!missing.ok);
    CHECK(missing.error.contains(QStringLiteral("No such file")));
}

TEST(cad, an_import_is_traced_instead_of_the_selected_scene) {
    // The bug this covers: an imported part reached the 3D view and stopped
    // there, so pressing Run traced whichever built-in scene the controls still
    // had selected. The geometry on screen and the geometry the rays went
    // through were two different solids -- which reads, from the outside,
    // exactly like "the rays are still following the previous shape".
    const QString path = scratchFile(QStringLiteral("luxtrace_offset_box.step"));
    QFile::remove(path);
    // Deliberately nowhere near the origin, so geometry belonging to any other
    // scene cannot be mistaken for this one.
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(200.0, 300.0, -20.0),
                                        40.0, 40.0, 40.0).Shape(), path));

    const cadimport::ImportResult r = cadimport::read(path);
    CHECK(r.ok);
    auto setup = std::make_shared<GeometryProvider::SceneSetup>(
        cadimport::makeScene(r, QStringLiteral("N-BK7"), /*reflective=*/false, 16));
    setup->label = QStringLiteral("offset box");
    CHECK(setup->surfaces.size() == 2);       // the part, and the receiver for it

    SimConfig cfg;
    // A scene is still selected, as it always is, and it must not be consulted.
    cfg.scene    = GeometryProvider::Scene::Reflector;
    cfg.imported = setup;
    cfg.rays     = 4000;
    cfg.source   = SourceConfig::Type::Collimated;
    cfg.detectorBins = 16;
    CHECK(cfg.tracesImport());
    CHECK(cfg.sceneName() == QStringLiteral("offset box"));

    // The geometry the tracer is handed is the file's, not the registry's.
    const Simulation::SceneRef data = Simulation::dataFor(cfg);
    CHECK(data->scene.detector().valid);
    CHECK_NEAR(data->scene.detector().center.x, 220.0, 1e-6);
    CHECK_NEAR(data->scene.detector().center.y, 320.0, 1e-6);
    // And the source is placed against that part rather than at the scene's own
    // focus, which for the reflector is the origin.
    CHECK_NEAR(data->sourceOrigin.X(), 220.0, 1e-6);
    CHECK_NEAR(data->sourceOrigin.Y(), 320.0, 1e-6);
    CHECK(data->sourceOrigin.Z() < -20.0);

    const auto& builtIn = Simulation::sceneFor(GeometryProvider::Scene::Reflector);
    CHECK(std::fabs(builtIn.detector().center.x - data->scene.detector().center.x) > 1.0);

    // A whole run agrees: the rays land on the imported part's receiver.
    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.raysEmitted == 4000);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
    CHECK(res.raysHitDetector > 0);
    CHECK_NEAR(res.detCX, 220.0, 1e-6);
    CHECK_NEAR(res.detCY, 320.0, 1e-6);
    QFile::remove(path);
}

TEST(cad, an_imported_scene_places_its_receiver_and_source_around_the_part) {
    // A CAD origin is wherever the designer left it. A receiver pinned to the
    // world origin measures whatever happens to be there instead of the part.
    const QString path = scratchFile(QStringLiteral("luxtrace_placement.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(-10.0, 40.0, 0.0),
                                        20.0, 20.0, 100.0).Shape(), path));
    const cadimport::ImportResult r = cadimport::read(path);
    CHECK(r.ok);

    const GeometryProvider::SceneSetup setup =
        cadimport::makeScene(r, QStringLiteral("N-BK7"), false);
    CHECK(setup.surfaces.size() == 2);
    CHECK(setup.surfaces.back().isDetector);
    CHECK(!setup.surfaces.front().isDetector);
    CHECK(setup.surfaces.front().index > 1.4);      // glass, with Fresnel splitting
    CHECK(setup.surfaces.front().fresnel);

    // The largest extent is the 100 mm depth, so the standoff is 75 mm each way
    // and the whole arrangement is centred on the part in x and y.
    CHECK_NEAR(setup.sourceOrigin.X(), 0.0, 1e-6);
    CHECK_NEAR(setup.sourceOrigin.Y(), 50.0, 1e-6);
    CHECK_NEAR(setup.sourceOrigin.Z(), -75.0, 1e-6);
    CHECK_NEAR(setup.sourceAxis.Z(), 1.0, 1e-12);

    TraceScene scene;
    scene.build(MeshBuilder::build(setup.surfaces));
    CHECK(scene.detector().valid);
    CHECK_NEAR(scene.detector().center.y, 50.0, 1e-6);
    CHECK_NEAR(scene.detector().center.z, 175.0, 1e-6);
    CHECK_NEAR(scene.detector().w, 150.0, 1e-6);     // 1.5 x the largest extent
    QFile::remove(path);
}

TEST(cad, imported_geometry_is_tessellated_once_and_reused) {
    // A run, a partial redraw and a convergence sweep all ask for the same
    // geometry. Rebuilding the hierarchy each time would make an import cost
    // more per trace than a built-in scene does.
    const QString path = scratchFile(QStringLiteral("luxtrace_cached.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10.0, 10.0, 10.0).Shape(), path));
    const cadimport::ImportResult r = cadimport::read(path);
    CHECK(r.ok);
    auto setup = std::make_shared<GeometryProvider::SceneSetup>(
        cadimport::makeScene(r, QStringLiteral("N-BK7"), false));

    double first = -1.0, second = -1.0;
    const Simulation::SceneRef a = Simulation::dataFor(setup, 0, &first);
    const Simulation::SceneRef b = Simulation::dataFor(setup, 0, &second);
    CHECK(a == b);
    CHECK(second == 0.0);       // the second ask built nothing

    // A different receiver resolution is a different build, because the grid is
    // baked into the mesh.
    const Simulation::SceneRef c = Simulation::dataFor(setup, 32);
    CHECK(c != a);
    CHECK(c->scene.detector().nx == 32);
    QFile::remove(path);
}

TEST(cad, a_study_over_dimensions_declines_on_imported_geometry) {
    // An imported solid has no dimensions to vary. Sweeping one would trace the
    // identical geometry at every step and draw a flat line, which reads as a
    // result rather than as the absence of one.
    const QString path = scratchFile(QStringLiteral("luxtrace_nosweep.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10.0, 10.0, 10.0).Shape(), path));
    const cadimport::ImportResult r = cadimport::read(path);
    CHECK(r.ok);

    SimConfig cfg;
    cfg.scene    = GeometryProvider::Scene::Lens;
    cfg.rays     = 200;
    cfg.imported = std::make_shared<GeometryProvider::SceneSetup>(
        cadimport::makeScene(r, QStringLiteral("N-BK7"), false));

    CHECK(studies::parameterSweep(cfg, 0, 1.0, 5.0, 3, studies::Metric::Efficiency, 1).empty());
    CHECK(!studies::optimise(cfg, {0}, studies::Objective{}, studies::Optimiser::NelderMead,
                             4).valid);
    std::vector<studies::Tolerance> tols(1);
    tols[0].slotIndex = 0;
    tols[0].amount    = 0.5;
    CHECK(!studies::tolerance(cfg, tols, studies::Metric::Efficiency, 0.5, true, 8).valid);

    // The ray-count sweep does apply -- it varies the trace, not the optic.
    const auto conv = studies::convergence(cfg, 200, 800, 3);
    CHECK(conv.size() == 3);
    QFile::remove(path);
}

TEST(cad, a_part_can_be_lit_from_any_side) {
    // A prism lit down its extrusion axis is a slab: the beam meets two
    // parallel faces and comes out undeviated. The deviation the part exists
    // for only appears when the light crosses a slanted face, and a CAD file
    // cannot be re-exported rotated from inside this app -- so the direction is
    // a choice at import.
    const QString path = scratchFile(QStringLiteral("luxtrace_sideways.step"));
    QFile::remove(path);
    CHECK(writeStep(BRepPrimAPI_MakeBox(gp_Pnt(-10.0, -10.0, -50.0),
                                        20.0, 20.0, 100.0).Shape(), path));
    const cadimport::ImportResult r = cadimport::read(path);
    CHECK(r.ok);

    const GeometryProvider::SceneSetup along =
        cadimport::makeScene(r, QStringLiteral("N-BK7"), false, 32, gp_Dir(0, 0, 1));
    const GeometryProvider::SceneSetup across =
        cadimport::makeScene(r, QStringLiteral("N-BK7"), false, 32, gp_Dir(1, 0, 0));

    // Down the long axis: clear the 100 mm depth, then stand off 75 mm.
    CHECK_NEAR(along.sourceOrigin.Z(), -125.0, 1e-6);
    CHECK_NEAR(along.sourceAxis.Z(), 1.0, 1e-12);
    // Across it: clear only the 20 mm width, so the same standoff sits closer.
    CHECK_NEAR(across.sourceOrigin.X(), -85.0, 1e-6);
    CHECK_NEAR(across.sourceOrigin.Y(), 0.0, 1e-6);
    CHECK_NEAR(across.sourceOrigin.Z(), 0.0, 1e-6);
    CHECK_NEAR(across.sourceAxis.X(), 1.0, 1e-12);

    // The receiver turns with the beam, and reports against its own axes rather
    // than against a world plane it does not lie in.
    TraceScene scene;
    scene.build(MeshBuilder::build(across.surfaces));
    const DetectorInfo& det = scene.detector();
    CHECK(det.valid);
    CHECK_NEAR(std::fabs(det.n.x), 1.0, 1e-9);
    CHECK_NEAR(det.center.x, 85.0, 1e-6);
    CHECK_NEAR(det.w, 150.0, 1e-6);
    CHECK_NEAR(det.u.dot(det.n), 0.0, 1e-12);
    CHECK_NEAR(det.v.dot(det.n), 0.0, 1e-12);

    SimConfig cfg;
    cfg.imported = std::make_shared<GeometryProvider::SceneSetup>(across);
    cfg.source   = SourceConfig::Type::Collimated;
    cfg.beamRadius = 8.0;
    cfg.rays     = 4000;
    const SimulationResult res = Simulation::run(cfg);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
    CHECK(res.raysHitDetector > 0);
    // The grid is indexed in the receiver's own frame, so the arrivals land in
    // it whichever way it faces.
    const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
    CHECK(m.valid);
    CHECK(m.litBins > 0);
    QFile::remove(path);
}

// ================================================================ sweep =====

TEST(sweep, finds_the_focus_of_an_elliptical_reflector) {
    // The ellipse transfers one focus to the other, so its spot size against the
    // semi-major axis has a minimum exactly where the geometry says. A sweep
    // that cannot find that is not a design tool.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 20000);
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    CHECK(!infos.empty());
    const SceneParamInfo& info = infos[0];

    const auto points = studies::parameterSweep(cfg, 0, info.min, info.max, 11,
                                                studies::Metric::RmsRadius);
    CHECK(points.size() == 11);

    std::size_t bestIdx = 0;
    for (std::size_t i = 1; i < points.size(); ++i)
        if (points[i].value < points[bestIdx].value) bestIdx = i;
    // Not at an end of the range: a real interior minimum.
    CHECK(bestIdx > 0 && bestIdx + 1 < points.size());
    CHECK(points[bestIdx].value < 0.25 * points.front().value);
    // And it is the design value the scene ships with.
    CHECK_NEAR(points[bestIdx].parameter, GeometryProvider::defaultParams(cfg.scene).v[0],
               1.5 * (info.max - info.min) / 10.0);

    // Every point carries an error bar, and it is small next to the movement the
    // sweep is showing.
    for (const auto& p : points) {
        CHECK(p.stdErr >= 0.0);
        CHECK(std::isfinite(p.value));
    }
    CHECK(points[bestIdx].stdErr < 0.2 * (points.front().value - points[bestIdx].value));
}

TEST(sweep, stays_inside_the_range_the_scene_declares) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    const SceneParamInfo& info = infos[0];
    // Asked for far more than the scene can be built at.
    const auto points = studies::parameterSweep(cfg, 0, info.min - 1000.0,
                                                info.max + 1000.0, 5,
                                                studies::Metric::Efficiency, 1);
    CHECK(points.size() == 5);
    for (const auto& p : points) {
        CHECK(p.parameter >= info.min - 1e-9);
        CHECK(p.parameter <= info.max + 1e-9);
        CHECK(p.value >= 0.0 && p.value <= 100.0);
    }
}

TEST(sweep, a_two_dimensional_sweep_covers_the_pair) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::BiconvexLens, 3000);
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    CHECK(infos.size() >= 2);
    const studies::Sweep2D grid =
        studies::parameterSweep2D(cfg, 0, 1, 5, 4, studies::Metric::RmsRadius);
    CHECK(grid.valid());
    CHECK(grid.a.size() == 5);
    CHECK(grid.b.size() == 4);
    CHECK(grid.values.size() == 20);
    CHECK(grid.hi > grid.lo);
    for (double v : grid.values) CHECK(std::isfinite(v));

    // A pair that is not a pair is refused rather than answered.
    CHECK(!studies::parameterSweep2D(cfg, 0, 0, 4, 4, studies::Metric::RmsRadius).valid());
}

TEST(sweep, every_metric_produces_a_finite_number) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 6000);
    const SimulationResult res = Simulation::run(cfg);
    for (int i = 0; i < int(studies::Metric::Count); ++i) {
        const auto m = studies::Metric(i);
        const double v = studies::metricValue(res, m);
        CHECK(std::isfinite(v));
        CHECK(!studies::metricName(m).isEmpty());
        CHECK(!studies::metricTip(m).isEmpty());
    }
    // Efficiency is reported as a percentage, matching the unit it is labelled
    // with. A metric whose number and whose unit disagree is a bug in a report.
    CHECK(studies::metricUnit(studies::Metric::Efficiency, FluxUnit::Watt) ==
          QStringLiteral("%"));
    CHECK_NEAR(studies::metricValue(res, studies::Metric::Efficiency),
               100.0 * res.efficiency, 1e-9);
}

// ============================================================= optimise =====

TEST(optimise, nelder_mead_improves_on_the_starting_design) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::BiconvexLens, 8000);
    studies::Objective obj;
    obj.metric = studies::Metric::RmsRadius;
    obj.goal   = studies::Objective::Goal::Minimise;

    const auto r = studies::optimise(cfg, {0, 1}, obj, studies::Optimiser::NelderMead, 60);
    CHECK(r.valid);
    CHECK(r.evaluations > 10);
    CHECK(r.evaluations <= 60 + 4);
    CHECK(r.bestValue < r.startValue);
    CHECK(!r.history.empty());
    CHECK(r.method == QStringLiteral("Nelder-Mead"));

    // The best-so-far curve only ever improves, which is what makes it readable
    // as a convergence plot.
    for (std::size_t i = 1; i < r.history.size(); ++i)
        CHECK(r.history[i].merit <= r.history[i - 1].merit + 1e-12);

    // And the answer is inside the ranges the scene declares.
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    for (int s : {0, 1}) {
        CHECK(r.best.v[s] >= infos[std::size_t(s)].min - 1e-9);
        CHECK(r.best.v[s] <= infos[std::size_t(s)].max + 1e-9);
    }
}

TEST(optimise, cmaes_improves_on_the_starting_design) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Cpc, 8000);
    studies::Objective obj;
    obj.metric = studies::Metric::Efficiency;
    obj.goal   = studies::Objective::Goal::Maximise;

    const auto r = studies::optimise(cfg, {0, 1}, obj, studies::Optimiser::Cmaes, 60);
    CHECK(r.valid);
    CHECK(r.bestValue > r.startValue);
    CHECK(r.method == QStringLiteral("CMA-ES"));
    CHECK(r.bestValue <= 100.0);
}

TEST(optimise, a_target_objective_closes_on_the_number_asked_for) {
    // "Hit this uniformity" is a different search from "make it as good as
    // possible", and the merit has to know the difference.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 8000);
    studies::Objective obj;
    obj.metric = studies::Metric::RmsRadius;
    obj.goal   = studies::Objective::Goal::Target;
    obj.target = 6.0;

    const auto start = studies::optimise(cfg, {0}, obj, studies::Optimiser::NelderMead, 4);
    const auto r     = studies::optimise(cfg, {0}, obj, studies::Optimiser::NelderMead, 50);
    CHECK(r.valid);
    CHECK(std::fabs(r.bestValue - obj.target) < std::fabs(start.startValue - obj.target));
    CHECK(std::fabs(r.bestValue - obj.target) < 1.0);
}

TEST(optimise, the_objective_is_a_function_not_a_moving_target) {
    // The determinism guarantee is what makes a derivative-free search converge:
    // asking for the same design twice has to give the same number, or the
    // simplex is chasing noise.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 6000);
    cfg.useSceneDefaults = false;
    cfg.params = GeometryProvider::defaultParams(cfg.scene);
    const double a = studies::metricValue(Simulation::run(cfg), studies::Metric::Efficiency);
    const double b = studies::metricValue(Simulation::run(cfg), studies::Metric::Efficiency);
    CHECK_NEAR(a, b, 0.0);
}

TEST(optimise, an_empty_parameter_list_is_refused_rather_than_searched) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 2000);
    studies::Objective obj;
    const auto r = studies::optimise(cfg, {}, obj, studies::Optimiser::NelderMead, 20);
    CHECK(!r.valid);
    CHECK(r.evaluations == 0);
}

TEST(optimise, cancellation_stops_the_search_and_keeps_the_best_so_far) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    studies::Objective obj;
    obj.metric = studies::Metric::Efficiency;

    std::atomic<bool> cancel{false};
    TraceControl ctl;
    ctl.cancel = &cancel;
    int seen = 0;
    ctl.progress = [&](std::size_t done, std::size_t) {
        seen = int(done);
        if (done >= 6) cancel.store(true, std::memory_order_relaxed);
    };

    const auto r = studies::optimise(cfg, {0, 1}, obj, studies::Optimiser::NelderMead, 200, ctl);
    CHECK(r.cancelled);
    CHECK(r.valid);              // whatever it found before stopping is still an answer
    CHECK(r.evaluations < 200);
    CHECK(seen > 0);
}

// ============================================================== derived =====

TEST(derived, every_scene_reports_something_a_designer_reasons_in) {
    // A user edits "focal length" and "aperture"; what they are thinking in is
    // f-number, acceptance angle, concentration and where the focus lands. If a
    // scene has parameters at all, it has to say what they imply.
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto scene = GeometryProvider::Scene(i);
        const auto list = GeometryProvider::derived(scene, GeometryProvider::defaultParams(scene));
        CHECK(!list.empty());
        for (const auto& d : list) {
            CHECK(!d.name.isEmpty());
            CHECK(!d.value.isEmpty());
            CHECK(!d.value.contains(QStringLiteral("nan"), Qt::CaseInsensitive));
            CHECK(!d.value.contains(QStringLiteral("inf"), Qt::CaseInsensitive));
        }
    }
}

TEST(derived, the_lensmaker_equation_appears_where_it_belongs) {
    // The plano-convex lens's focal length is R/(n-1). The strip has to say so,
    // and say it in millimetres.
    const auto scene = GeometryProvider::Scene::PlanoConvexLens;
    const SceneParams p = GeometryProvider::defaultParams(scene);
    const auto list = GeometryProvider::derived(scene, p);

    bool found = false;
    for (const auto& d : list)
        if (d.name == QStringLiteral("Focal length")) {
            found = true;
            CHECK(d.unit == QStringLiteral("mm"));
            CHECK_NEAR(d.value.toDouble(), p.v[0] / (1.5168 - 1.0), 0.15);
        }
    CHECK(found);
}

TEST(derived, the_concentration_limit_appears_on_a_cpc) {
    const auto scene = GeometryProvider::Scene::Cpc;
    const SceneParams p = GeometryProvider::defaultParams(scene);
    const auto list = GeometryProvider::derived(scene, p);
    bool found = false;
    for (const auto& d : list)
        if (d.name == QStringLiteral("Concentration")) {
            found = true;
            const double s = std::sin(p.v[1] * kPi / 180.0);
            CHECK_NEAR(d.value.toDouble(), 1.0 / (s * s), 0.1);
        }
    CHECK(found);
}

TEST(derived, the_sphere_multiplier_predicts_what_the_trace_finds) {
    // The strongest kind of derived quantity: one the tracer can be checked
    // against. If the closed form and the Monte Carlo disagree, one of them is
    // wrong and the user should be able to see which.
    const auto scene = GeometryProvider::Scene::IntegratingSphere;
    const SceneParams p = GeometryProvider::defaultParams(scene);
    double predicted = 0.0;
    for (const auto& d : GeometryProvider::derived(scene, p))
        if (d.name == QStringLiteral("Expected throughput")) predicted = d.value.toDouble();
    CHECK(predicted > 0.0);

    SimConfig cfg = quickConfig(scene, 40000);
    cfg.source = SourceConfig::Type::Point;
    const SimulationResult res = Simulation::run(cfg);
    // The scene's receiver is a flat plate inside the cavity rather than a hole
    // in its wall, so the closed form is the right order and not the last digit.
    if (!(std::fabs(100.0 * res.efficiency - predicted) < 0.5 * predicted))
        std::printf("       predicted %.3f %%, traced %.3f %%\n",
                    predicted, 100.0 * res.efficiency);
    CHECK(std::fabs(100.0 * res.efficiency - predicted) < 0.5 * predicted);
}

// =============================================================== report =====

TEST(report, carries_the_run_and_stamps_it_so_it_can_be_reproduced) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    cfg.seed = 24601;
    const SimulationResult res = Simulation::run(cfg);

    report::Content content;
    content.config  = cfg;
    content.result  = res;
    content.metrics = analysis::computeSpotMetrics(res);
    content.notes   = QStringLiteral("A note that has to survive.");

    const QString html = report::html(content);
    CHECK(html.startsWith(QStringLiteral("<!doctype html>")));
    CHECK(html.contains(QStringLiteral("</html>")));
    CHECK(html.contains(GeometryProvider::info(cfg.scene).name));
    CHECK(html.contains(QStringLiteral("A note that has to survive.")));
    // The seed and the ray count are what make it reproducible rather than
    // merely readable.
    CHECK(html.contains(QStringLiteral("seed 24601")));
    CHECK(html.contains(QString::number(res.raysEmitted)));
    // Every parameter the scene declares.
    for (const auto& info : GeometryProvider::paramInfo(cfg.scene))
        CHECK(html.contains(info.name));
    // And the energy budget, closed.
    CHECK(html.contains(QStringLiteral("Energy budget")));
    CHECK(html.contains(QStringLiteral("Accounted")));
    CHECK(html.contains(QStringLiteral("Efficiency")));
}

TEST(report, embeds_its_figures_so_it_is_one_file) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
    report::Content content;
    content.config  = cfg;
    content.result  = Simulation::run(cfg);
    content.metrics = analysis::computeSpotMetrics(content.result);

    report::Figure f;
    f.title   = QStringLiteral("A figure");
    f.caption = QStringLiteral("with a caption");
    f.image   = QImage(16, 12, QImage::Format_RGB32);
    f.image.fill(Qt::red);
    content.figures.push_back(f);

    const QString html = report::html(content);
    CHECK(html.contains(QStringLiteral("A figure")));
    CHECK(html.contains(QStringLiteral("with a caption")));
    // Inlined as a data URI: one file to mail, no folder of assets to lose.
    CHECK(html.contains(QStringLiteral("src='data:image/png;base64,")));
    CHECK(!html.contains(QStringLiteral(".png'")));
}

TEST(report, says_whether_a_difference_between_two_runs_is_real) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    const SimulationResult a = Simulation::run(cfg);
    cfg.useSceneDefaults = false;
    cfg.params = GeometryProvider::defaultParams(cfg.scene);
    cfg.params.v[0] *= 0.6;                     // a genuinely different optic
    const SimulationResult b = Simulation::run(cfg);

    report::Content content;
    content.config          = cfg;
    content.result          = a;
    content.metrics         = analysis::computeSpotMetrics(a);
    content.hasComparison   = true;
    content.comparison      = b;
    content.comparisonLabel = QStringLiteral("the shorter dish");

    const QString html = report::html(content);
    CHECK(html.contains(QStringLiteral("the shorter dish")));
    CHECK(html.contains(QStringLiteral("Uncertainty")));
    // Two designs that differ by far more than their error bars: the report has
    // to say so rather than leaving the reader to eyeball it.
    CHECK(html.contains(QStringLiteral("sigma")));
}

TEST(report, writes_a_file_and_reports_a_path_it_cannot) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 2000);
    report::Content content;
    content.config  = cfg;
    content.result  = Simulation::run(cfg);
    content.metrics = analysis::computeSpotMetrics(content.result);

    const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("luxtrace_report.html"));
    QFile::remove(path);
    QString err;
    CHECK(report::write(path, content, &err));
    CHECK(QFileInfo::exists(path));
    CHECK(QFileInfo(path).size() > 1000);
    QFile::remove(path);

    CHECK(!report::write(QDir(QDir::tempPath()).filePath(
              QStringLiteral("no_such_directory_here/report.html")), content, &err));
    CHECK(!err.isEmpty());
}

// ============================================================== overrides ===

TEST(overrides, editing_a_surface_changes_the_trace_without_touching_the_geometry) {
    // The point of applying these at trace time: the tessellation and the
    // hierarchy are shared, so a polish change costs a trace and not a rebuild.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 20000);
    const SimulationResult plain = Simulation::run(cfg);

    const Simulation::SceneRef before = Simulation::dataFor(cfg);

    // Find the mirror and make it matte.
    int mirror = -1;
    for (std::size_t i = 0; i < before->surfaces.size(); ++i)
        if (!before->surfaces[i].isDetector) { mirror = int(i); break; }
    CHECK(mirror >= 0);

    SurfaceOverride ov;
    ov.surface = mirror;
    ov.optics  = before->surfaces[std::size_t(mirror)];
    ov.optics.scatter = 1.0;                    // a Lambertian mirror
    cfg.surfaceOverrides.push_back(ov);

    const SimulationResult edited = Simulation::run(cfg);
    const Simulation::SceneRef after = Simulation::dataFor(cfg);

    // Same geometry object: no rebuild happened.
    CHECK(before.get() == after.get());
    CHECK_NEAR(edited.fluxAccounted(), edited.sourcePower, 1e-9);
    // A collimator whose mirror scatters is no longer a collimator.
    CHECK(std::fabs(edited.efficiency - plain.efficiency) >
          6.0 * (edited.efficiencyStdErr + plain.efficiencyStdErr));
    CHECK(edited.intensity.fwhmDeg > 3.0 * plain.intensity.fwhmDeg);
}

TEST(overrides, an_out_of_range_surface_is_ignored_rather_than_crashing) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
    const SimulationResult plain = Simulation::run(cfg);

    SurfaceOverride ov;
    ov.surface = 9999;
    ov.optics.reflectivity = 1.0;
    cfg.surfaceOverrides.push_back(ov);
    SurfaceOverride neg;
    neg.surface = -3;
    cfg.surfaceOverrides.push_back(neg);

    const SimulationResult res = Simulation::run(cfg);
    CHECK_NEAR(res.efficiency, plain.efficiency, 1e-12);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
}

// ================================================================= bsdf =====

TEST(bsdf, a_lambertian_lobe_is_cosine_distributed) {
    // The one case with a closed form to check against: the fraction of samples
    // inside a cone of half-angle theta is sin^2(theta).
    bsdf::Surface s;
    s.model = bsdf::Model::Lambertian;
    const Vec3 n(0, 0, 1);
    std::uint64_t rng = 1234;

    constexpr int kN = 200000;
    int inside30 = 0, inside60 = 0;
    for (int i = 0; i < kN; ++i) {
        Vec3 o;
        double w = 1.0;
        CHECK(s.sample(Vec3(0, 0, -1), n, Vec3(0, 0, 1), rng, o, w));
        CHECK(o.dot(n) > -1e-12);
        CHECK_NEAR(w, 1.0, 1e-12);
        const double th = std::acos(std::clamp(o.dot(n), -1.0, 1.0));
        if (th < 30.0 * kPi / 180.0) ++inside30;
        if (th < 60.0 * kPi / 180.0) ++inside60;
    }
    const double s30 = std::sin(30.0 * kPi / 180.0);
    const double s60 = std::sin(60.0 * kPi / 180.0);
    CHECK_NEAR(double(inside30) / kN, s30 * s30, 0.01);
    CHECK_NEAR(double(inside60) / kN, s60 * s60, 0.01);
}

TEST(bsdf, a_microfacet_lobe_narrows_as_the_surface_is_polished) {
    // The property the Gaussian tilt was reaching for, now with a distribution
    // that has a shadowing-masking term and a sampling weight.
    auto spread = [](double alpha) {
        bsdf::Surface s;
        s.model = bsdf::Model::Microfacet;
        s.alpha = alpha;
        const Vec3 n(0, 0, 1);
        const Vec3 wi = Vec3(0.3, 0.0, -1.0).normalized();
        Vec3 spec = wi - n * (2.0 * wi.dot(n));
        spec.normalize();

        std::uint64_t rng = 99;
        double sum = 0.0;
        int taken = 0;
        for (int i = 0; i < 40000; ++i) {
            Vec3 o;
            double w = 1.0;
            if (!s.sample(wi, n, spec, rng, o, w)) continue;
            sum += std::acos(std::clamp(o.dot(spec), -1.0, 1.0));
            ++taken;
        }
        CHECK(taken > 20000);
        return taken ? sum / taken : 0.0;
    };
    const double rough = spread(0.30);
    const double fine  = spread(0.05);
    CHECK(rough > fine);
    CHECK(fine < 0.15);          // a polished surface stays near the specular ray
    CHECK(rough > 0.15);
}

TEST(bsdf, a_microfacet_lobe_conserves_energy) {
    // The failure the Gaussian jitter had: rejecting a tilt that flipped past
    // the incident ray biased the result toward specular by however often that
    // happened. A distribution with correct weights does not.
    for (double alpha : {0.05, 0.2, 0.5}) {
        bsdf::Surface s;
        s.model = bsdf::Model::Microfacet;
        s.alpha = alpha;
        for (double incidenceDeg : {5.0, 40.0, 70.0}) {
            const double a = incidenceDeg * kPi / 180.0;
            const Vec3 n(0, 0, 1);
            const Vec3 wi = Vec3(std::sin(a), 0.0, -std::cos(a)).normalized();
            Vec3 spec = wi - n * (2.0 * wi.dot(n));
            spec.normalize();

            std::uint64_t rng = 4242;
            double sum = 0.0;
            constexpr int kN = 60000;
            for (int i = 0; i < kN; ++i) {
                Vec3 o;
                double w = 1.0;
                if (s.sample(wi, n, spec, rng, o, w)) sum += w;
            }
            // The mean weight is the share of the incident energy the lobe
            // sends back out: at or below one, and close to it away from
            // grazing where the shadowing term genuinely removes some.
            const double mean = sum / kN;
            if (!(mean > 0.55 && mean < 1.02))
                std::printf("       alpha %.2f at %.0f deg: mean weight %.4f\n",
                            alpha, incidenceDeg, mean);
            CHECK(mean > 0.55 && mean < 1.02);
        }
    }
}

TEST(bsdf, abg_is_a_straight_line_on_a_log_log_plot) {
    // The signature of the model, and the reason its three numbers are fittable
    // to a scatterometer trace: past the core the BSDF is a power law of slope
    // -g, so a decade in angle is g decades in scatter.
    for (double g : {1.5, 2.0, 3.0}) {
        bsdf::Surface s;
        s.model = bsdf::Model::Abg;
        s.abgA  = 1e-4;
        s.abgB  = 0.001;
        s.abgG  = g;

        // Well outside the core, so B^g is negligible against beta^g.
        const double b1 = 0.02, b2 = 0.2;
        const double slope = std::log(s.value(b2) / s.value(b1)) / std::log(b2 / b1);
        CHECK_NEAR(slope, -g, 0.05);

        // Inside the core it is flat, which is what B means.
        CHECK_NEAR(s.value(0.0) / s.value(0.05 * s.abgB), 1.0, 0.02);
    }

    // A wider core with the same A holds the level down over a wider angle, so
    // it scatters less in total -- which is the parametrisation, and worth
    // pinning because the intuition runs the other way.
    bsdf::Surface s;
    s.model = bsdf::Model::Abg;
    s.abgA  = 1e-4;
    s.abgB  = 0.001;
    s.abgG  = 2.0;
    bsdf::Surface wide = s;  wide.abgB = 0.01;
    CHECK(wide.value(0.0) < s.value(0.0));
    CHECK(wide.totalIntegratedScatter() < s.totalIntegratedScatter());
    // And more A is always more scatter, at every angle.
    bsdf::Surface strong = s; strong.abgA = 1e-3;
    for (double b : {0.0, 0.01, 0.1, 0.5})
        CHECK(strong.value(b) > s.value(b));
}

TEST(bsdf, total_integrated_scatter_is_the_number_a_spec_sheet_carries) {
    // A scatter specification is written as a TIS, so the model has to be able
    // to report one -- and it has to move the way the parameters say.
    bsdf::Surface s;
    s.model = bsdf::Model::Abg;
    s.abgB  = 0.005;
    s.abgG  = 2.0;

    double previous = -1.0;
    for (double a : {1e-6, 1e-5, 1e-4, 1e-3}) {
        s.abgA = a;
        const double tis = s.totalIntegratedScatter();
        CHECK(tis >= 0.0 && tis <= 1.0);
        CHECK(tis > previous);
        previous = tis;
    }
    // A specular surface scatters nothing at all.
    bsdf::Surface spec;
    CHECK(spec.isSpecular());
    CHECK_NEAR(spec.totalIntegratedScatter(), 0.0, 0.0);
}

TEST(bsdf, a_sample_never_goes_below_the_surface) {
    // Whatever the model, an outgoing ray inside the solid is not a scatter, it
    // is a bug. The caller keeps the specular direction instead.
    std::uint64_t rng = 7;
    for (auto model : {bsdf::Model::Microfacet, bsdf::Model::Abg, bsdf::Model::Lambertian}) {
        bsdf::Surface s;
        s.model = model;
        s.alpha = 0.4;
        s.abgA  = 1e-3;
        s.abgB  = 0.05;
        for (double incidenceDeg : {10.0, 45.0, 80.0, 88.0}) {
            const double a = incidenceDeg * kPi / 180.0;
            const Vec3 n(0, 0, 1);
            const Vec3 wi = Vec3(std::sin(a), 0.0, -std::cos(a)).normalized();
            Vec3 spec = wi - n * (2.0 * wi.dot(n));
            spec.normalize();
            for (int i = 0; i < 4000; ++i) {
                Vec3 o;
                double w = 1.0;
                if (!s.sample(wi, n, spec, rng, o, w)) continue;
                CHECK(o.dot(n) > -1e-9);
                CHECK(std::isfinite(w));
                CHECK(w >= 0.0);
            }
        }
    }
}

TEST(bsdf, the_henyey_greenstein_phase_function_integrates_to_one) {
    // A phase function that does not is a medium that creates or destroys light.
    for (double g : {-0.6, -0.2, 0.0, 0.3, 0.8}) {
        constexpr int kN = 2000;
        double total = 0.0;
        for (int i = 0; i < kN; ++i) {
            const double c0 = -1.0 + 2.0 * double(i) / kN;
            const double c1 = -1.0 + 2.0 * double(i + 1) / kN;
            const double cm = 0.5 * (c0 + c1);
            // 2 pi d(cos theta) is the solid-angle measure.
            total += bsdf::henyeyGreenstein(cm, g) * 2.0 * kPi * (c1 - c0);
        }
        CHECK_NEAR(total, 1.0, 0.01);
    }
}

TEST(bsdf, volume_scattering_sends_light_forward_or_back_as_asked) {
    bsdf::Volume v;
    v.coefficient = 0.1;
    CHECK(v.active());

    auto meanCosine = [](double g) {
        bsdf::Volume vol;
        vol.coefficient = 0.1;
        vol.anisotropy  = g;
        const Vec3 d(0, 0, 1);
        std::uint64_t rng = 555;
        double sum = 0.0;
        constexpr int kN = 100000;
        for (int i = 0; i < kN; ++i) sum += vol.scatter(d, rng).dot(d);
        return sum / kN;
    };
    // The mean cosine of the Henyey-Greenstein phase function is g itself.
    CHECK_NEAR(meanCosine(0.0), 0.0, 0.01);
    CHECK_NEAR(meanCosine(0.7), 0.7, 0.02);
    CHECK_NEAR(meanCosine(-0.5), -0.5, 0.02);
}

TEST(bsdf, the_free_path_is_exponentially_distributed) {
    // A mean free path of 1/sigma is what makes a scattering coefficient mean
    // anything.
    for (double sigma : {0.02, 0.5, 5.0}) {
        bsdf::Volume v;
        v.coefficient = sigma;
        std::uint64_t rng = 31337;
        double sum = 0.0;
        constexpr int kN = 200000;
        for (int i = 0; i < kN; ++i) sum += v.sampleDistance(rng);
        CHECK_NEAR(sum / kN, 1.0 / sigma, 0.02 / sigma);
    }
    bsdf::Volume clear;
    CHECK(!clear.active());
    std::uint64_t rng = 1;
    CHECK(clear.sampleDistance(rng) > 1e29);   // never scatters
}

TEST(bsdf, a_volume_scatterer_spreads_a_beam_without_losing_it) {
    // The feature that was absent entirely, and among the most used in luminaire
    // design: every white diffusing plastic is a volume scatterer.
    auto run = [](double coefficient, double anisotropy = 0.0) {
        OpticalSurface slab;
        slab.shape = BRepPrimAPI_MakeBox(gp_Pnt(-200, -200, 0), 400.0, 400.0, 30.0).Shape();
        slab.label = QStringLiteral("Diffusing slab");
        slab.index = 1.5;
        slab.transmissivity = 1.0;
        slab.reflectivity   = 0.0;
        slab.fresnel = false;
        slab.volume.coefficient = coefficient;
        slab.volume.anisotropy  = anisotropy;

        const double h = 400.0;
        BRepBuilderAPI_MakePolygon poly;
        poly.Add(gp_Pnt(-h, -h, 300.0));
        poly.Add(gp_Pnt(h, -h, 300.0));
        poly.Add(gp_Pnt(h, h, 300.0));
        poly.Add(gp_Pnt(-h, h, 300.0));
        poly.Close();
        OpticalSurface det;
        det.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
        det.label = QStringLiteral("Detector");
        det.isDetector = true;
        det.detNX = det.detNY = 64;

        TraceScene scene;
        scene.build(MeshBuilder::build({slab, det}));

        SourceConfig src;
        src.type       = SourceConfig::Type::Collimated;
        src.beamRadius = 3.0;
        src.origin     = gp_Pnt(0, 0, -40.0);
        src.axis       = gp_Dir(0, 0, 1);
        src.rays       = 60000;
        TraceOptions opt;
        opt.nTheta = 60;
        opt.nPhi   = 24;
        SimulationResult r;
        RayTracer::trace(scene, src, r, opt);
        return r;
    };

    const SimulationResult clear = run(0.0);
    const SimulationResult milky = run(0.08);

    CHECK_NEAR(clear.fluxAccounted(), clear.sourcePower, 1e-9);
    CHECK_NEAR(milky.fluxAccounted(), milky.sourcePower, 1e-9);

    // Scattering spreads the beam: the spot gets wider and the far field opens.
    const analysis::SpotMetrics mc = analysis::computeSpotMetrics(clear);
    const analysis::SpotMetrics mm = analysis::computeSpotMetrics(milky);
    CHECK(mc.valid && mm.valid);
    if (!(mm.rmsRadius > 3.0 * mc.rmsRadius))
        std::printf("       clear rms %.3f, milky rms %.3f\n", mc.rmsRadius, mm.rmsRadius);
    CHECK(mm.rmsRadius > 3.0 * mc.rmsRadius);
    CHECK(milky.intensity.fwhmDeg > clear.intensity.fwhmDeg);

    // It is a scatterer and not an absorber. Isotropic scattering turns some of
    // the beam round, so less reaches the receiver -- but that light leaves the
    // slab rather than being destroyed in it, which is exactly the difference
    // between a diffuser and a filter.
    CHECK(milky.fluxDetector < clear.fluxDetector);
    CHECK(milky.fluxEscaped > clear.fluxEscaped);
    CHECK_NEAR(milky.fluxAbsorbed, clear.fluxAbsorbed, 1e-9);
    CHECK_NEAR(milky.fluxAbsorbed, 0.0, 1e-9);

    // Forward scattering keeps far more of it going the right way, which is what
    // a filled polymer actually does and why the asymmetry parameter is worth
    // carrying at all.
    const SimulationResult forward = run(0.08, 0.8);
    CHECK_NEAR(forward.fluxAccounted(), forward.sourcePower, 1e-9);
    if (!(forward.fluxDetector > milky.fluxDetector))
        std::printf("       forward %.4f vs isotropic %.4f\n",
                    forward.fluxDetector, milky.fluxDetector);
    CHECK(forward.fluxDetector > milky.fluxDetector);
    // And it still spreads the beam, or it would not be a diffuser.
    const analysis::SpotMetrics mf = analysis::computeSpotMetrics(forward);
    CHECK(mf.valid);
    CHECK(mf.rmsRadius > mc.rmsRadius);
}

// ============================================================== coating =====

TEST(coating, an_ar_coating_suppresses_the_bare_reflection) {
    // The point of the whole feature: an uncoated multi-element system
    // overstates its loss by roughly 3.5 % per surface against any real lens.
    const double n1 = 1.0, n2 = 1.5168;
    const double bare = optics::fresnelReflectance(n1, n2, 1.0);
    CHECK_NEAR(bare, 0.0417, 1e-3);

    for (const char* which : {"MgF2 single layer", "Broadband AR", "V-coat (laser line)"}) {
        const coating::Coating c = coating::byName(QLatin1String(which));
        CHECK(c.active());
        const double r = c.reflectance_(n1, n2, 1.0, 550.0, bare);
        CHECK(r < bare);
        CHECK(r >= 0.0);
    }
    // And they are in the order their names imply.
    const double mg = coating::byName(QStringLiteral("MgF2 single layer"))
                          .reflectance_(n1, n2, 1.0, 550.0, bare);
    const double bb = coating::byName(QStringLiteral("Broadband AR"))
                          .reflectance_(n1, n2, 1.0, 550.0, bare);
    const double v  = coating::byName(QStringLiteral("V-coat (laser line)"))
                          .reflectance_(n1, n2, 1.0, 550.0, bare);
    CHECK(mg > bb);
    CHECK(bb > v);
}

TEST(coating, a_high_reflector_reflects_more_than_bare_glass) {
    const double n1 = 1.0, n2 = 1.5168;
    const double bare = optics::fresnelReflectance(n1, n2, 1.0);
    const coating::Coating hr = coating::byName(QStringLiteral("Dielectric HR"));
    CHECK(hr.active());
    const double r = hr.reflectance_(n1, n2, 1.0, 550.0, bare);
    CHECK(r > 0.99);
    CHECK(r <= 1.0);
    // Better than any metal, which is the reason to use one.
    const OpticalMaterial ag = materials::byName(QStringLiteral("Silver"));
    CHECK(r > metalReflectance(1.0, ag.indexAt(550.0), ag.extinctionAt(550.0), 1.0));
}

TEST(coating, gets_worse_at_angle_the_way_a_real_stack_does) {
    // A coating is designed at normal incidence; opening the angle costs it.
    const double n1 = 1.0, n2 = 1.5168;
    const coating::Coating ar = coating::byName(QStringLiteral("Broadband AR"));
    double previous = -1.0;
    for (double deg : {0.0, 20.0, 40.0, 60.0, 75.0}) {
        const double c = std::cos(deg * kPi / 180.0);
        const double bare = optics::fresnelReflectance(n1, n2, c);
        const double r = ar.reflectance_(n1, n2, c, 550.0, bare);
        CHECK(r >= previous - 1e-12);      // never improves as the angle opens
        CHECK(r <= bare + 1e-12);          // and never worse than no coating at all
        previous = r;
    }
}

TEST(coating, cannot_defeat_total_internal_reflection) {
    // Past the critical angle the interface reflects everything, whatever is on
    // it: a coating cannot make light leave a medium it cannot leave. Without
    // this, an AR coating on a light guide would destroy it.
    const double n1 = 1.5168, n2 = 1.0;
    const double crit = std::asin(n2 / n1);
    for (const char* which : {"Broadband AR", "V-coat (laser line)", "MgF2 single layer"}) {
        const coating::Coating c = coating::byName(QLatin1String(which));
        for (double over : {0.01, 0.1, 0.3}) {
            const double cosI = std::cos(crit + over);
            const double bare = optics::fresnelReflectance(n1, n2, cosI);
            CHECK_NEAR(bare, 1.0, 1e-12);
            CHECK_NEAR(c.reflectance_(n1, n2, cosI, 550.0, bare), 1.0, 1e-12);
        }
    }
}

TEST(coating, a_solved_stack_agrees_with_the_coating_it_models) {
    // A quarter wave of MgF2 on crown glass has a closed form at its design
    // wavelength: R = ((n0 n2 - n1^2) / (n0 n2 + n1^2))^2. The characteristic
    // matrix has to reproduce it, or it is not solving anything.
    const coating::Coating q = coating::byName(QStringLiteral("Quarter-wave MgF2 (solved)"));
    CHECK(q.active());

    const double n0 = 1.0, n1 = 1.38, n2 = 1.5168;
    const double num = n0 * n2 - n1 * n1;
    const double den = n0 * n2 + n1 * n1;
    const double want = (num / den) * (num / den);

    const double bare = optics::fresnelReflectance(n0, n2, 1.0);
    const double got = q.reflectance_(n0, n2, 1.0, 550.0, bare);
    CHECK_NEAR(got, want, 5e-4);
    CHECK(got < bare);

    // Away from the design wavelength it gets worse, which is the whole reason a
    // broadband coating costs more than a single layer.
    const double offBand = q.reflectance_(n0, n2, 1.0, 420.0,
                                          optics::fresnelReflectance(n0, n2, 1.0));
    CHECK(offBand > got);
}

TEST(coating, closes_the_gap_a_real_lens_would_not_have_had) {
    // The measurable claim: a coated lens passes more light than a bare one, by
    // about the Fresnel loss of its two surfaces.
    auto efficiency = [](const char* film) {
        auto surfaces = GeometryProvider::buildScene(GeometryProvider::Scene::BiconvexLens);
        for (auto& s : surfaces)
            if (!s.isDetector) s.coating = coating::byName(QLatin1String(film));
        TraceScene scene;
        scene.build(MeshBuilder::build(surfaces));
        SimulationResult r;
        RayTracer::trace(scene,
                         Simulation::sourceFor(GeometryProvider::Scene::BiconvexLens,
                                               SourceConfig::Type::Lambertian, 40000),
                         r);
        CHECK_NEAR(r.fluxAccounted(), r.sourcePower, 1e-9);
        return r.efficiency;
    };
    const double bare   = efficiency("None");
    const double coated = efficiency("Broadband AR");
    CHECK(coated > bare);
    // Two surfaces at roughly 4 % each: the gain is several per cent of the
    // transmitted light, not a rounding difference.
    CHECK(coated / bare > 1.03);
    CHECK(coated / bare < 1.25);
}

// ============================================================ polarised =====

TEST(polarised, brewsters_angle_reflects_no_p_polarised_light) {
    // The textbook case the unpolarised average cannot express at all: at
    // Brewster's angle the p reflectance is exactly zero, and the reflected beam
    // is completely s-polarised.
    const double n1 = 1.0, n2 = 1.5168;
    const double brewster = std::atan(n2 / n1);
    double rs = 0.0, rp = 0.0, phase = 0.0;
    polarisation::fresnelAmplitudes(n1, n2, std::cos(brewster), rs, rp, phase);
    CHECK_NEAR(rp, 0.0, 1e-9);
    CHECK(rs > 0.3);

    // And the Mueller matrix built from them turns unpolarised light into fully
    // polarised light.
    const polarisation::Mueller m = polarisation::Mueller::fromFresnel(rs, rp, phase);
    const polarisation::Stokes out = m.apply(polarisation::Stokes::unpolarised());
    CHECK(out.i > 0.0);
    CHECK_NEAR(out.degree(), 1.0, 1e-9);
    CHECK(out.valid());
}

TEST(polarised, the_unpolarised_average_is_what_it_always_was) {
    // A polarised trace has to agree with the unpolarised one whenever the light
    // is not polarised, or the new path is a different tracer rather than a
    // refinement of the old.
    const double n1 = 1.0, n2 = 1.5168;
    for (int deg = 0; deg <= 85; deg += 5) {
        const double c = std::cos(deg * kPi / 180.0);
        double rs = 0.0, rp = 0.0, phase = 0.0;
        polarisation::fresnelAmplitudes(n1, n2, c, rs, rp, phase);
        const polarisation::Stokes out =
            polarisation::Mueller::fromFresnel(rs, rp, phase)
                .apply(polarisation::Stokes::unpolarised(1.0));
        CHECK_NEAR(out.i, optics::fresnelReflectance(n1, n2, c), 1e-12);
    }
}

TEST(polarised, total_internal_reflection_carries_a_phase) {
    // Past the critical angle both amplitudes are unity and what is left is the
    // phase between them -- which is how a Fresnel rhomb turns linear light into
    // circular, and which an unpolarised model has no way to express.
    const double n1 = 1.5168, n2 = 1.0;
    const double crit = std::asin(n2 / n1);
    bool sawPhase = false;
    for (double over : {0.05, 0.2, 0.4, 0.7}) {
        double rs = 0.0, rp = 0.0, phase = 0.0;
        polarisation::fresnelAmplitudes(n1, n2, std::cos(crit + over), rs, rp, phase);
        CHECK_NEAR(rs, 1.0, 1e-12);
        CHECK_NEAR(rp, 1.0, 1e-12);
        if (std::fabs(phase) > 0.1) sawPhase = true;
        // Unit magnitude means no energy is lost, whatever the phase.
        const polarisation::Stokes out =
            polarisation::Mueller::fromFresnel(rs, rp, phase)
                .apply(polarisation::Stokes::linearS(1.0));
        CHECK_NEAR(out.i, 1.0, 1e-12);
    }
    CHECK(sawPhase);

    // Linear light at 45 degrees to the plane picks up circularity from it,
    // which is the whole trick.
    double rs = 0.0, rp = 0.0, phase = 0.0;
    polarisation::fresnelAmplitudes(n1, n2, std::cos(crit + 0.25), rs, rp, phase);
    polarisation::Stokes in;
    in.i = 1.0; in.q = 0.0; in.u = 1.0; in.v = 0.0;    // linear at 45 degrees
    const polarisation::Stokes out =
        polarisation::Mueller::fromFresnel(rs, rp, phase).apply(in);
    CHECK(std::fabs(out.v) > 0.05);
}

TEST(polarised, a_polariser_and_a_retarder_do_what_they_are_named) {
    // An ideal polariser passes half of unpolarised light and all of light
    // already aligned with it, and nothing at all crossed.
    const polarisation::Mueller p0 = polarisation::Mueller::polariser(0.0);
    CHECK_NEAR(p0.apply(polarisation::Stokes::unpolarised(1.0)).i, 0.5, 1e-12);
    CHECK_NEAR(p0.apply(polarisation::Stokes::linearS(1.0)).i, 1.0, 1e-12);
    CHECK_NEAR(p0.apply(polarisation::Stokes::linearP(1.0)).i, 0.0, 1e-12);
    // What it passes is fully polarised, whatever went in.
    CHECK_NEAR(p0.apply(polarisation::Stokes::unpolarised(1.0)).degree(), 1.0, 1e-9);

    // Crossed polarisers pass nothing. Malus's law at 90 degrees.
    const polarisation::Mueller p90 = polarisation::Mueller::polariser(kPi / 2.0);
    CHECK_NEAR((p90 * p0).apply(polarisation::Stokes::unpolarised(1.0)).i, 0.0, 1e-12);
    // At 45 degrees between them, a quarter gets through.
    const polarisation::Mueller p45 = polarisation::Mueller::polariser(kPi / 4.0);
    CHECK_NEAR((p45 * p0).apply(polarisation::Stokes::unpolarised(1.0)).i, 0.25, 1e-12);

    // A quarter-wave plate at 45 degrees turns linear into circular.
    const polarisation::Mueller qwp =
        polarisation::Mueller::retarder(kPi / 4.0, kPi / 2.0);
    const polarisation::Stokes circ = qwp.apply(polarisation::Stokes::linearS(1.0));
    CHECK_NEAR(std::fabs(circ.v), 1.0, 1e-9);
    CHECK_NEAR(circ.i, 1.0, 1e-12);
    // And a half-wave plate rotates linear light rather than making it circular.
    const polarisation::Stokes turned =
        polarisation::Mueller::retarder(kPi / 4.0, kPi).apply(polarisation::Stokes::linearS(1.0));
    CHECK_NEAR(turned.q, -1.0, 1e-9);
    CHECK_NEAR(turned.v, 0.0, 1e-9);
}

TEST(polarised, a_stokes_vector_stays_physically_realisable) {
    // More polarised than total is not a state light can be in, so nothing the
    // interface does may produce one.
    const double n1 = 1.0, n2 = 1.5168;
    for (int deg = 0; deg <= 89; ++deg) {
        double rs = 0.0, rp = 0.0, phase = 0.0;
        polarisation::fresnelAmplitudes(n1, n2, std::cos(deg * kPi / 180.0), rs, rp, phase);
        const polarisation::Mueller m = polarisation::Mueller::fromFresnel(rs, rp, phase);
        for (const auto& in : {polarisation::Stokes::unpolarised(1.0),
                               polarisation::Stokes::linearS(1.0),
                               polarisation::Stokes::linearP(1.0),
                               polarisation::Stokes::circular(1.0)}) {
            const polarisation::Stokes out = m.apply(in);
            CHECK(out.valid());
            CHECK(out.i <= in.i + 1e-12);      // an interface never adds light
            CHECK(out.degree() <= 1.0 + 1e-9);
        }
    }
}

TEST(polarised, a_trace_reports_the_polarisation_it_produced) {
    // The end-to-end claim: switch it on and the run says how polarised the
    // light reaching the receiver is.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::BiconvexLens, 40000);
    cfg.physics.polarised = true;
    const SimulationResult res = Simulation::run(cfg);

    CHECK(res.polarised);
    CHECK(res.degreeOfPolarisation >= 0.0 && res.degreeOfPolarisation <= 1.0);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
    // An unpolarised source through a lens picks up a little polarisation from
    // the Fresnel split at the surfaces -- real, and small.
    CHECK(res.degreeOfPolarisation < 0.3);

    // And switching it on must not move the answer for an unpolarised source.
    SimConfig plain = cfg;
    plain.physics.polarised = false;
    const SimulationResult unpol = Simulation::run(plain);
    CHECK(!unpol.polarised);
    if (!(std::fabs(res.efficiency - unpol.efficiency) < 1e-9))
        std::printf("       polarised %.9f vs unpolarised %.9f\n",
                    res.efficiency, unpol.efficiency);
    CHECK_NEAR(res.efficiency, unpol.efficiency, 1e-9);
}

TEST(polarised, a_polarised_source_survives_to_the_receiver) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::BiconvexLens, 30000);
    cfg.physics.polarised = true;
    cfg.polarisationState = 1;                 // linear, s

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.polarised);
    CHECK_NEAR(res.fluxAccounted(), res.sourcePower, 1e-9);
    // Light that went in polarised comes out mostly polarised: refraction
    // through glass is not a depolariser.
    CHECK(res.degreeOfPolarisation > 0.5);
}

// ========================================================= image quality ====

TEST(imagequality, the_airy_radius_is_the_diffraction_limit_it_claims) {
    // 1.22 lambda f/#, in millimetres. The reference marker that says when a
    // design has stopped being geometry-limited.
    CHECK_NEAR(analysis::airyRadiusMm(2.0, 550.0), 1.22 * 550e-6 * 2.0, 1e-12);
    CHECK_NEAR(analysis::airyRadiusMm(8.0, 550.0), 4.0 * analysis::airyRadiusMm(2.0, 550.0), 1e-12);
    // A slower lens has a bigger Airy disc, and a bluer wavelength a smaller one.
    CHECK(analysis::airyRadiusMm(11.0, 550.0) > analysis::airyRadiusMm(2.8, 550.0));
    CHECK(analysis::airyRadiusMm(4.0, 450.0) < analysis::airyRadiusMm(4.0, 650.0));
    // Nonsense in, zero out, rather than a number nobody should use.
    CHECK_NEAR(analysis::airyRadiusMm(0.0, 550.0), 0.0, 0.0);
    CHECK_NEAR(analysis::airyRadiusMm(4.0, 0.0), 0.0, 0.0);
}

TEST(imagequality, a_tighter_spot_carries_contrast_to_higher_frequencies) {
    // The claim an MTF makes: a smaller blur passes finer detail. Measured
    // against the app's own spot metric, so the two have to agree about which
    // design is better.
    auto measure = [](GeometryProvider::Scene scene, int rays) {
        SimConfig cfg = quickConfig(scene, rays);
        const SimulationResult res = Simulation::run(cfg);
        const analysis::Mtf mtf = analysis::modulationTransfer(res);
        const analysis::SpotMetrics m = analysis::computeSpotMetrics(res);
        CHECK(mtf.valid);
        CHECK(m.valid);
        CHECK(mtf.frequency.size() == mtf.tangential.size());
        CHECK(mtf.frequency.size() == mtf.sagittal.size());
        // Every MTF starts at one and never rises above it.
        CHECK_NEAR(mtf.tangential.front(), 1.0, 1e-9);
        for (double v : mtf.tangential) CHECK(v >= -1e-12 && v <= 1.0 + 1e-9);
        for (double v : mtf.sagittal)   CHECK(v >= -1e-12 && v <= 1.0 + 1e-9);
        return std::pair<double, double>(mtf.cutoff50, m.rmsRadius);
    };

    // The elliptical reflector images one focus onto the other; the spherical
    // one aberrates badly. The frequency each carries contrast to has to rank
    // them the same way the spot size does.
    const auto sharp = measure(GeometryProvider::Scene::EllipticalReflector, 40000);
    const auto blunt = measure(GeometryProvider::Scene::SphericalReflector, 40000);
    CHECK(sharp.second < blunt.second);            // smaller spot
    CHECK(sharp.first  > blunt.first);             // and higher cutoff
    CHECK(sharp.first > 0.0);
}

TEST(imagequality, the_diffraction_limit_bounds_what_any_design_can_do) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 30000);
    const SimulationResult res = Simulation::run(cfg);
    // A 60 mm aperture at 200 mm: an f/1.7 system.
    const analysis::Mtf mtf = analysis::modulationTransfer(res, 128, 60.0, 200.0);
    CHECK(mtf.valid);
    CHECK(mtf.diffractionLimit.size() == mtf.frequency.size());
    CHECK(mtf.diffractionCutoff > 0.0);

    // The reference curve is the incoherent MTF of a circular aperture: one at
    // zero frequency, zero at the cutoff, monotonic in between.
    CHECK_NEAR(mtf.diffractionLimit.front(), 1.0, 1e-9);
    double previous = 2.0;
    for (double v : mtf.diffractionLimit) {
        CHECK(v >= -1e-12 && v <= 1.0 + 1e-9);
        CHECK(v <= previous + 1e-12);
        previous = v;
    }
    // A geometric spot far bigger than the Airy disc cannot beat it anywhere.
    for (std::size_t i = 0; i < mtf.frequency.size(); ++i)
        CHECK(0.5 * (mtf.tangential[i] + mtf.sagittal[i]) <= mtf.diffractionLimit[i] + 0.02);
}

TEST(imagequality, the_wavefront_error_comes_out_of_the_path_lengths) {
    // What one extra double per branch bought: an OPD map, an RMS wavefront
    // error and a Strehl figure, all from the optical path each arrival carries.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 40000);
    const SimulationResult res = Simulation::run(cfg);
    const analysis::Wavefront w = analysis::wavefrontError(res);

    CHECK(w.valid);
    CHECK(w.rmsWaves >= 0.0);
    CHECK(w.ptvWaves >= w.rmsWaves);
    CHECK(w.strehl >= 0.0 && w.strehl <= 1.0);
    CHECK(w.referenceOpl > 0.0);
    CHECK(w.nx > 0 && w.ny > 0);
    CHECK(w.opd.size() == std::size_t(w.nx) * std::size_t(w.ny));
    for (double v : w.opd) CHECK(std::isfinite(v));

    // A well-corrected design has a smaller wavefront error than an aberrated
    // one, which is the same ranking every other metric gives.
    SimConfig bad = quickConfig(GeometryProvider::Scene::SphericalReflector, 40000);
    const analysis::Wavefront wb = analysis::wavefrontError(Simulation::run(bad));
    CHECK(wb.valid);
    if (!(w.rmsWaves < wb.rmsWaves))
        std::printf("       elliptical %.2f waves vs spherical %.2f waves\n",
                    w.rmsWaves, wb.rmsWaves);
    CHECK(w.rmsWaves < wb.rmsWaves);
    // Neither of these mirrors is anywhere near diffraction limited, so the
    // Strehl figure is not the one to rank them by -- and the flag says so
    // rather than leaving a reader to compare two underflowed zeros.
    CHECK(!w.diffractionLimited());
    CHECK(!wb.diffractionLimited());
    CHECK(w.rmsWaves > 0.25);
}

TEST(imagequality, strehl_follows_marechal) {
    // The relation the figure is defined by, so a reader can check it: a
    // fourteenth of a wave RMS is the 0.8 Strehl that "diffraction limited"
    // conventionally means.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 20000);
    const analysis::Wavefront w = analysis::wavefrontError(Simulation::run(cfg));
    CHECK(w.valid);
    const double phase = 2.0 * kPi * w.rmsWaves;
    CHECK_NEAR(w.strehl, std::exp(-std::min(phase * phase, 700.0)), 1e-9);
    CHECK(w.diffractionLimited() == (w.strehlMeaningful && w.strehl >= 0.8));
    CHECK(w.strehlMeaningful == (w.rmsWaves <= 0.25));

    // And the relation itself, on a wavefront small enough for it to hold.
    analysis::Wavefront ideal;
    ideal.rmsWaves = 1.0 / 14.0;
    const double p2 = 2.0 * kPi * ideal.rmsWaves;
    CHECK_NEAR(std::exp(-p2 * p2), 0.82, 0.02);   // the conventional 0.8 at lambda/14
}

TEST(imagequality, a_well_corrected_imaging_bundle_has_a_small_wavefront_error) {
    // The strongest statement about the metric: a paraxial bundle through a
    // decent lens, landing on its own focus, has to come out as a fraction of a
    // wave -- and Fermat is why. Every ray of a perfect image travels the same
    // optical path from source to image point, so what the spread measures once
    // the path is carried on to the convergence point is the aberration alone.
    const OpticalMaterial bk7 = materials::byName(QStringLiteral("N-BK7"));
    const double R = 100.0, t = 6.0;
    const double f = R / (bk7.nd - 1.0);

    const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, t - R), R).Shape();
    const TopoDS_Shape slab =
        BRepPrimAPI_MakeBox(gp_Pnt(-60.0, -60.0, 0.0), 120.0, 120.0, t).Shape();
    OpticalSurface lens;
    lens.shape          = BRepAlgoAPI_Common(sphere, slab).Shape();
    lens.label          = QStringLiteral("Plano-convex");
    lens.material       = bk7;
    lens.index          = bk7.nd;
    lens.transmissivity = 1.0;
    lens.reflectivity   = 0.0;
    lens.fresnel        = false;      // no ghosts: this is about the wavefront
    lens.meshAngle      = 0.02;

    // A receiver just big enough to hold the focus, so nothing that missed the
    // lens can land on it and pretend to be part of the imaging bundle.
    const double half = 6.0;
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(-half, -half, t + f));
    poly.Add(gp_Pnt(half, -half, t + f));
    poly.Add(gp_Pnt(half, half, t + f));
    poly.Add(gp_Pnt(-half, half, t + f));
    poly.Close();
    OpticalSurface det;
    det.shape = BRepBuilderAPI_MakeFace(poly.Wire()).Shape();
    det.label = QStringLiteral("Detector");
    det.isDetector = true;
    det.detNX = det.detNY = 64;

    TraceScene scene;
    scene.build(MeshBuilder::build({lens, det}));

    SourceConfig src;
    src.type       = SourceConfig::Type::Collimated;
    src.beamRadius = 4.0;              // paraxial
    src.origin     = gp_Pnt(0, 0, -40.0);
    src.axis       = gp_Dir(0, 0, 1);
    src.rays       = 60000;
    TraceOptions opt;
    opt.nTheta     = 0;
    opt.arrivalCap = 200000;
    SimulationResult res;
    RayTracer::trace(scene, src, res, opt);
    CHECK(res.efficiency > 0.5);

    const analysis::Wavefront w = analysis::wavefrontError(res);
    CHECK(w.valid);
    CHECK(w.focusFound);
    // The rays converge where the lensmaker's equation says they do.
    CHECK_NEAR(w.focus.z, t + f, 2.0);
    CHECK(w.coreFraction() > 0.8);
    if (!(w.rmsWaves < 5.0))
        std::printf("       %.3f waves over %.0f %% of the arrivals\n",
                    w.rmsWaves, 100.0 * w.coreFraction());
    CHECK(w.rmsWaves < 5.0);

    // Open the aperture and the spherical aberration grows: a wavefront error
    // that did not would not be measuring one.
    src.beamRadius = 24.0;
    SimulationResult wide;
    RayTracer::trace(scene, src, wide, opt);
    const analysis::Wavefront ww = analysis::wavefrontError(wide);
    CHECK(ww.valid);
    if (!(ww.rmsWaves > w.rmsWaves))
        std::printf("       narrow %.3f waves, wide %.3f waves\n", w.rmsWaves, ww.rmsWaves);
    CHECK(ww.rmsWaves > w.rmsWaves);
}

TEST(imagequality, the_wavefront_is_measured_over_the_bundle_that_images) {
    // Light that reaches the receiver without going through the optic belongs to
    // no wavefront: its path length differs by the geometry of the scene rather
    // than by any aberration. The figure says how much of the light it used, so
    // a reader can tell a measurement from a mixture.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::BiconvexLens, 30000);
    const analysis::Wavefront w = analysis::wavefrontError(Simulation::run(cfg));
    CHECK(w.valid);
    CHECK(w.total > 0);
    CHECK(w.used > 0);
    CHECK(w.used <= w.total);
    CHECK(w.coreFraction() > 0.0 && w.coreFraction() <= 1.0);

    // A mirror collects everything it reports, so its core is the whole bundle.
    SimConfig mirror = quickConfig(GeometryProvider::Scene::EllipticalReflector, 30000);
    const analysis::Wavefront wm = analysis::wavefrontError(Simulation::run(mirror));
    CHECK(wm.valid);
    CHECK(wm.coreFraction() > 0.95);
}

TEST(imagequality, too_few_arrivals_is_declined_rather_than_invented) {
    SimulationResult empty;
    CHECK(!analysis::modulationTransfer(empty).valid);
    CHECK(!analysis::wavefrontError(empty).valid);
}

// =========================================================== tolerancing ====

TEST(tolerancing, reports_a_yield_and_the_tolerance_that_dominates_it) {
    // The question a manufacturer actually asks: not "how good is the nominal
    // design" but "what fraction of production will pass".
    SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 8000);
    const auto& infos = GeometryProvider::paramInfo(cfg.scene);
    CHECK(infos.size() >= 2);

    std::vector<studies::Tolerance> tols;
    studies::Tolerance a;
    a.slotIndex = 0;                       // semi-major axis: the focus depends on it
    a.amount    = 6.0;
    a.shape     = studies::Tolerance::Shape::Gaussian;
    tols.push_back(a);
    studies::Tolerance b;
    b.slotIndex = 1;                       // semi-minor axis: it barely matters
    b.amount    = 0.5;
    b.shape     = studies::Tolerance::Shape::Gaussian;
    tols.push_back(b);

    const auto study = studies::tolerance(cfg, tols, studies::Metric::RmsRadius,
                                          /*criterion=*/4.0, /*passIsAbove=*/false, 80);
    CHECK(study.valid);
    CHECK(study.samples == 80);
    CHECK(study.values.size() == 80);
    CHECK(study.stdDev > 0.0);
    CHECK(study.yield >= 0.0 && study.yield <= 1.0);
    CHECK(study.best <= study.worst);      // for a metric where smaller is better
    CHECK(study.nominal > 0.0);

    // The histogram covers every sample.
    int counted = 0;
    for (int c : study.binCount) counted += c;
    CHECK(counted == study.samples);

    // And the sensitivity ranking names the dimension to tighten. The semi-major
    // axis moves the focus; the semi-minor one is along for the ride.
    CHECK(study.sensitivity.size() == 2);
    CHECK(study.sensitivity[0].slotIndex == 0);
    CHECK(study.sensitivity[0].share > study.sensitivity[1].share);
    CHECK(study.sensitivity[0].share > 0.5);
    CHECK_NEAR(study.sensitivity[0].share + study.sensitivity[1].share, 1.0, 1e-9);
}

TEST(tolerancing, the_same_seed_reproduces_the_same_production_run) {
    // The determinism story is what makes an ensemble reproducible, which
    // tolerancing tools rarely are: a yield figure can be checked rather than
    // merely quoted.
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 4000);
    std::vector<studies::Tolerance> tols;
    studies::Tolerance t;
    t.slotIndex = 0;
    t.amount    = 4.0;
    tols.push_back(t);

    const auto a = studies::tolerance(cfg, tols, studies::Metric::Efficiency, 70.0, true, 24);
    const auto b = studies::tolerance(cfg, tols, studies::Metric::Efficiency, 70.0, true, 24);
    CHECK(a.valid && b.valid);
    CHECK(a.values.size() == b.values.size());
    for (std::size_t i = 0; i < a.values.size(); ++i)
        CHECK_NEAR(a.values[i], b.values[i], 0.0);
    CHECK_NEAR(a.yield, b.yield, 0.0);

    // A different seed is a different production run.
    cfg.seed += 1;
    const auto c = studies::tolerance(cfg, tols, studies::Metric::Efficiency, 70.0, true, 24);
    bool anyDifferent = false;
    for (std::size_t i = 0; i < a.values.size() && i < c.values.size(); ++i)
        if (std::fabs(a.values[i] - c.values[i]) > 1e-12) anyDifferent = true;
    CHECK(anyDifferent);
}

TEST(tolerancing, a_tighter_tolerance_is_a_narrower_spread) {
    // The relation the whole study exists to show, and the reason a sensitivity
    // ranking is actionable: tighten the dimension and the spread closes.
    auto spread = [](double amount) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 6000);
        std::vector<studies::Tolerance> tols;
        studies::Tolerance t;
        t.slotIndex = 0;
        t.amount    = amount;
        tols.push_back(t);
        const auto s = studies::tolerance(cfg, tols, studies::Metric::RmsRadius,
                                          3.0, false, 48);
        CHECK(s.valid);
        return s;
    };
    const auto loose = spread(10.0);
    const auto tight = spread(1.0);
    CHECK(tight.stdDev < loose.stdDev);
    CHECK(tight.yield >= loose.yield - 1e-12);
    // The nominal design is the same either way; only the spread around it moves.
    CHECK_NEAR(tight.nominal, loose.nominal, 1e-9);
}

TEST(tolerancing, the_distribution_shapes_behave_as_named) {
    // A uniform tolerance fills its band; a bimodal one sits at the edges, which
    // is the worst case a supplier can ship while still passing inspection.
    auto run = [](studies::Tolerance::Shape shape) {
        SimConfig cfg = quickConfig(GeometryProvider::Scene::EllipticalReflector, 4000);
        std::vector<studies::Tolerance> tols;
        studies::Tolerance t;
        t.slotIndex = 0;
        t.amount    = 8.0;
        t.shape     = shape;
        tols.push_back(t);
        return studies::tolerance(cfg, tols, studies::Metric::RmsRadius, 3.0, false, 64);
    };
    const auto uniform  = run(studies::Tolerance::Shape::Uniform);
    const auto gaussian = run(studies::Tolerance::Shape::Gaussian);
    const auto bimodal  = run(studies::Tolerance::Shape::Bimodal);
    CHECK(uniform.valid && gaussian.valid && bimodal.valid);
    // Everything at the limits spreads the metric furthest; a Gaussian, which
    // concentrates near nominal, spreads it least.
    // All three span the same limit, so they differ only in how they fill it.
    //
    // The spot size of this reflector has a minimum at the nominal design, so
    // the metric is V-shaped in the perturbation -- and on a V, a bimodal
    // tolerance puts every part at the same distance from the bottom and so
    // produces almost no *spread* while producing the worst *parts*. Which is
    // exactly why a yield study reports both, and why reading only the standard
    // deviation of a worst-case distribution is how a fragile design passes
    // review.
    CHECK(bimodal.mean > uniform.mean);
    CHECK(uniform.mean > gaussian.mean);
    // A Gaussian concentrates near nominal, so it disturbs the design least.
    CHECK(gaussian.stdDev < uniform.stdDev);
    CHECK(gaussian.mean < bimodal.mean);
    // And none of them moves the nominal design, only the population around it.
    CHECK_NEAR(uniform.nominal, gaussian.nominal, 1e-9);
    CHECK_NEAR(uniform.nominal, bimodal.nominal, 1e-9);
    CHECK(uniform.nominal <= gaussian.mean + 1e-9);   // nominal is the best case
}

TEST(tolerancing, refuses_a_study_with_nothing_to_perturb) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 2000);
    CHECK(!studies::tolerance(cfg, {}, studies::Metric::Efficiency, 50.0, true, 20).valid);

    studies::Tolerance zero;
    zero.slotIndex = 0;
    zero.amount    = 0.0;                  // a tolerance of nothing is not one
    CHECK(!studies::tolerance(cfg, {zero}, studies::Metric::Efficiency, 50.0, true, 20).valid);

    studies::Tolerance bad;
    bad.slotIndex = 99;
    bad.amount    = 1.0;
    CHECK(!studies::tolerance(cfg, {bad}, studies::Metric::Efficiency, 50.0, true, 20).valid);
}

TEST(tolerancing, exports_the_ensemble_it_measured) {
    SimConfig cfg = quickConfig(GeometryProvider::Scene::Reflector, 3000);
    std::vector<studies::Tolerance> tols;
    studies::Tolerance t;
    t.slotIndex = 0;
    t.amount    = 5.0;
    tols.push_back(t);
    const auto study = studies::tolerance(cfg, tols, studies::Metric::Efficiency,
                                          70.0, true, 32);
    CHECK(study.valid);

    const QString csv = studies::toleranceCsv(study, cfg);
    CHECK(csv.contains(QStringLiteral("tolerance analysis")));
    CHECK(csv.contains(QStringLiteral("yield")));
    CHECK(csv.contains(QStringLiteral("sensitivity")));
    CHECK(csv.contains(QStringLiteral("histogram")));
    // The seed, because the same seed reproduces the same production run.
    CHECK(csv.contains(QString::number(cfg.seed)));
    // Every sample is in it.
    CHECK(csv.count(QLatin1Char('\n')) > 32);
}

// =============================================================== geometry ===
// The viewport rebuilds its geometry through GeometryWorker, so the async
// plumbing the window depends on -- delivery off the GUI thread, generation
// stamps, and the coalescing that keeps a spin-box drag from queueing a build
// per step -- is covered here rather than only by dragging a spin box.

TEST(geometry, delivers_the_build_under_the_stamp_it_returned) {
    GeometryWorker worker;
    Simulation::SceneRef got;
    quint64 gotGen = 0;

    QObject::connect(&worker, &GeometryWorker::geometryReady,
                     [&](Simulation::SceneRef data, quint64 gen) {
                         got = std::move(data);
                         gotGen = gen;
                     });

    SimConfig cfg;
    cfg.scene            = GeometryProvider::Scene::Reflector;
    cfg.useSceneDefaults = false;
    cfg.params           = GeometryProvider::defaultParams(cfg.scene);
    cfg.params.v[0]     += 3.0;

    const quint64 gen = worker.request(cfg);
    CHECK(gen != 0);
    // The call returns immediately; the caller keeps running its event loop.
    CHECK(waitFor([&] { return gotGen != 0; }));

    CHECK(gotGen == gen);
    CHECK(!!got);
    CHECK(!got->surfaces.empty());
    CHECK(!got->scene.triangles().empty());
}

TEST(geometry, a_burst_of_requests_coalesces_onto_the_last_one) {
    GeometryWorker worker;
    std::vector<quint64> delivered;

    QObject::connect(&worker, &GeometryWorker::geometryReady,
                     [&](Simulation::SceneRef, quint64 gen) { delivered.push_back(gen); });

    SimConfig cfg;
    cfg.scene            = GeometryProvider::Scene::Reflector;
    cfg.useSceneDefaults = false;

    // What a drag across a spin box looks like: a new set of dimensions per
    // step, faster than any one of them can be built.
    quint64 last = 0;
    for (int i = 1; i <= 12; ++i) {
        cfg.params       = GeometryProvider::defaultParams(cfg.scene);
        cfg.params.v[0] += 0.5 * i;
        last = worker.request(cfg);
    }

    // The dimensions the drag ended on always arrive.
    CHECK(waitFor([&] {
        return !delivered.empty() && delivered.back() == last;
    }));
    // The ones it passed through mostly do not: the point of coalescing is that
    // the work is spent on the state the user stopped at.
    CHECK(delivered.size() < 12);
    // Nothing is invented, and nothing arrives out of order.
    for (std::size_t i = 0; i < delivered.size(); ++i) {
        CHECK(delivered[i] >= 1 && delivered[i] <= last);
        if (i > 0) CHECK(delivered[i] > delivered[i - 1]);
    }
}
