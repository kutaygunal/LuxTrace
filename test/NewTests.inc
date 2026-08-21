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

    // Sixty-four times the rays is eight times the precision. Monte Carlo makes
    // that a statement about the ratio, not an exact one, so allow a factor of
    // two either way.
    const double ratio = sweep.front().stdErr / std::max(sweep.back().stdErr, 1e-18);
    CHECK(ratio > 4.0 && ratio < 16.0);

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
    cfg.wavelengthNm     = 632.8;
    cfg.rays             = 250000;
    cfg.seed             = 777;
    cfg.threads          = 6;
    cfg.nTheta           = 45;
    cfg.nPhi             = 36;
    cfg.physics.fresnel           = false;
    cfg.physics.scattering        = false;
    cfg.physics.roughnessOverride = 0.02;
    cfg.physics.absorptionScale   = 3.5;

    SimConfig back;
    CHECK(configio::fromJson(configio::toJson(cfg), back));

    CHECK(back.scene == cfg.scene);
    CHECK(back.source == cfg.source);
    CHECK(back.shape == cfg.shape);
    CHECK(back.spectrum == cfg.spectrum);
    CHECK_NEAR(back.halfAngleDeg, cfg.halfAngleDeg, 1e-9);
    CHECK_NEAR(back.sizeA, cfg.sizeA, 1e-9);
    CHECK_NEAR(back.sizeB, cfg.sizeB, 1e-9);
    CHECK_NEAR(back.beamRadius, cfg.beamRadius, 1e-9);
    CHECK_NEAR(back.wavelengthNm, cfg.wavelengthNm, 1e-9);
    CHECK(back.rays == cfg.rays);
    CHECK(back.seed == cfg.seed);
    CHECK(back.threads == cfg.threads);
    CHECK(back.nTheta == cfg.nTheta);
    CHECK(back.nPhi == cfg.nPhi);
    CHECK(back.physics.fresnel == false);
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
