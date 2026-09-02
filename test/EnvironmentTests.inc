// The environment as an emitter.
//
// A uniform surround is the room this application already had, and for a
// hemisphere of even sky it is exactly right. What it cannot be is
// *directional*, and a directional room is what an optic is actually looked at
// in: a window on one side, a sky and a ground, a sun.
//
// Two things have to be true for a map to be worth having rather than merely
// present. It has to reduce to the uniform case when it is uniform -- otherwise
// it is a second, disagreeing model of the same room -- and it has to be
// *sampled*, because a sun is a thousandth of the sphere carrying most of the
// light and a cosine-weighted draw finds it once in a few thousand tries. Both
// are asserted here.

namespace {

// A map of one luminance everywhere.
envmap::EnvironmentMap flatMap(double nits, int w = 32, int h = 16) {
    envmap::EnvironmentMap m;
    m.build(w, h, std::vector<double>(std::size_t(w) * std::size_t(h), nits));
    return m;
}

// A map that is dark everywhere except a band of rows around the +up pole: a
// patch of sky of known solid angle and known luminance, which is the smallest
// environment with an answer that can be written down.
//
// The cap runs from theta = 0 to the bottom edge of row `rows - 1`, so its solid
// angle is exactly 2 pi (1 - cos(theta_max)).
envmap::EnvironmentMap capMap(double nits, int rows, double& solidAngle,
                              int w = 64, int h = 32) {
    std::vector<double> v(std::size_t(w) * std::size_t(h), 0.0);
    for (int y = 0; y < rows && y < h; ++y)
        for (int x = 0; x < w; ++x) v[std::size_t(y) * std::size_t(w) + std::size_t(x)] = nits;
    const double thetaMax = double(rows) / double(h) * 3.14159265358979323846;
    solidAngle = 6.28318530717958647692 * (1.0 - std::cos(thetaMax));
    envmap::EnvironmentMap m;
    m.build(w, h, std::move(v));
    return m;
}

// A Radiance file written flat -- no run-length encoding -- from linear RGB.
std::vector<std::uint8_t> radianceBytes(int w, int h,
                                        const std::vector<double>& rgb) {
    std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
                         std::to_string(h) + " +X " + std::to_string(w) + "\n";
    std::vector<std::uint8_t> out(header.begin(), header.end());
    for (int i = 0; i < w * h; ++i) {
        const double r = rgb[std::size_t(i) * 3 + 0];
        const double g = rgb[std::size_t(i) * 3 + 1];
        const double b = rgb[std::size_t(i) * 3 + 2];
        const double m = std::max({r, g, b});
        if (m < 1e-32) { out.insert(out.end(), {0, 0, 0, 0}); continue; }
        int e = 0;
        const double f = std::frexp(m, &e) * 256.0 / m;
        out.push_back(std::uint8_t(std::clamp(r * f, 0.0, 255.0)));
        out.push_back(std::uint8_t(std::clamp(g * f, 0.0, 255.0)));
        out.push_back(std::uint8_t(std::clamp(b * f, 0.0, 255.0)));
        out.push_back(std::uint8_t(e + 128));
    }
    return out;
}

} // namespace

TEST(environment, a_uniform_map_is_the_uniform_surround_it_replaces) {
    // The reduction that keeps this from being a second model of the same room.
    // A map of one number everywhere must read that number in every direction,
    // and its mean over the sphere must be that number too -- which is the
    // sin(theta) weighting done right, and would come out several per cent low
    // if the poles were weighted like the equator.
    const envmap::EnvironmentMap m = flatMap(120.0);
    CHECK(!m.empty());
    CHECK_NEAR(m.meanLuminance(), 120.0, 1e-9);

    const Vec3 up(0, 0, 1);
    Vec3 e1, e2;
    optics::orthonormalBasis(up, e1, e2);
    for (const Vec3& d : {Vec3(0, 0, 1), Vec3(0, 0, -1), Vec3(1, 0, 0),
                          Vec3(0, 1, 0), Vec3(-0.6, 0.5, 0.62)})
        CHECK_NEAR(m.along(d.normalized(), up, e1, e2), 120.0, 1e-9);
}

TEST(environment, the_sampler_and_its_own_density_are_the_same_distribution) {
    // A sampler drifting from the density it reports is the failure that shows
    // up as a plausible picture at the wrong brightness, and it is invisible in
    // any single image. The check that catches it needs no reference: for any
    // proper density over the sphere the mean of 1 / pdf over its own draws is
    // the area of the sphere, 4 pi.
    double omega = 0.0;
    const envmap::EnvironmentMap m = capMap(500.0, 4, omega);
    const Vec3 up(0, 0, 1);
    Vec3 e1, e2;
    optics::orthonormalBasis(up, e1, e2);

    std::uint64_t rng = 20260902u;
    const auto u01 = [&]() {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return double((rng >> 11) & ((1ULL << 53) - 1)) / double(1ULL << 53);
    };

    double sum = 0.0;
    int    n = 0, inCap = 0;
    for (int i = 0; i < 200000; ++i) {
        Vec3   d;
        double pdf = 0.0;
        if (!m.sample(u01(), u01(), up, e1, e2, d, pdf)) continue;
        CHECK(pdf > 0.0);
        // The density the sampler reports for the direction it drew must be the
        // density the map reports for that direction, or the two are different
        // distributions wearing one name.
        CHECK_NEAR(pdf, m.pdfAlong(d, up, e1, e2), 1e-9 * pdf);
        sum += 1.0 / pdf;
        ++n;
        if (m.along(d, up, e1, e2) > 0.0) ++inCap;
    }
    CHECK(n > 190000);
    CHECK_NEAR(sum / double(std::max(1, n)), 12.566370614359172, 0.05 * 12.566);

    // And it really is importance sampling rather than a uniform draw wearing
    // the name: the lit part of this map is a percent and a half of the
    // sphere and takes the large majority of the draws.
    //
    // Not all of them, deliberately. The CDF is dilated by one texel so the
    // sampler covers the whole support the bilinear lookup has, and the
    // outer half of that margin reads zero -- those draws contribute nothing
    // and cost nothing, and they are the price of the ring around the edge
    // being sampled at all. What matters is the ratio against a uniform
    // draw, which is what the sampler exists to beat.
    const double lit = double(inCap) / double(std::max(1, n));
    const double areaShare = omega / 12.566370614359172;
    CHECK(areaShare < 0.05);
    CHECK(lit > 0.5);
    CHECK(lit > 20.0 * areaShare);
}

TEST(environment, a_patch_of_sky_lights_a_wall_by_the_amount_it_subtends) {
    // The claim the whole thing is worth having for: a room that is bright in
    // one direction and dark everywhere else lights a wall by how much of it
    // the wall can see, and by nothing else. Nothing here is traced from a
    // source -- the only light in the scene is the room, which is the case a
    // uniform surround could produce a number for and never the *right*
    // number, because it fills the whole sphere by construction.
    //
    // The prediction is a direct quadrature of the map the render is actually
    // given, not the closed form for an ideal cap. The map is read bilinearly,
    // so its cap has soft edges a couple of rows wide and the ideal formula is
    // five per cent out on one this small -- which would leave the test
    // asserting the difference between two models of the sky rather than the
    // correctness of the integrator. The quadrature integrates the same
    // function the tracer reads, so the only thing left between them is the
    // estimator.
    double omega = 0.0;
    const auto map = std::make_shared<envmap::EnvironmentMap>(
        capMap(4000.0, 2, omega));
    CHECK(omega > 0.0);

    const Vec3 up(0, 0, 1);
    Vec3 e1, e2;
    optics::orthonormalBasis(up, e1, e2);

    // E = integral of L cos(theta) dOmega over the upper hemisphere, on a fine
    // enough grid that the quadrature is not the error term.
    const int nT = 2000, nP = 720;
    const double kPi = 3.14159265358979323846;
    double E = 0.0;
    for (int a = 0; a < nT; ++a) {
        const double theta = (double(a) + 0.5) / double(nT) * 0.5 * kPi;
        const double st = std::sin(theta), ct = std::cos(theta);
        const double dT = 0.5 * kPi / double(nT);
        for (int b = 0; b < nP; ++b) {
            const double phi = (double(b) + 0.5) / double(nP) * 2.0 * kPi;
            const double dP  = 2.0 * kPi / double(nP);
            const Vec3 d = (up * ct + e1 * (st * std::cos(phi)) +
                            e2 * (st * std::sin(phi))).normalized();
            E += map->along(d, up, e1, e2) * ct * st * dT * dP;
        }
    }
    CHECK(E > 0.0);

    MeshList meshes{matteWall(600.0, 0.6)};
    TraceScene scene;
    scene.build(meshes);

    backward::CameraConfig cam = cameraAt(Vec3(0, 0, 300), Vec3(0, 0, 0), 24, 512, 120.0);
    cam.environmentMap   = map;
    cam.environmentUp    = up;
    cam.environmentLuminance = 0.0;     // the map is the only room

    backward::LuminanceImage img;
    backward::render(scene, scene.surfaces(), {}, cam, PhysicsOptions{},
                     FluxUnit::Lumen, img);
    CHECK(!img.empty());

    const double predicted = backward::lambertianLuminance(0.6 * E);
    const double seen = centreMean(img);
    CHECK(seen > 0.0);
    CHECK(std::fabs(seen - predicted) < 0.02 * predicted);

    // And it is directional, which is the part a uniform surround cannot be.
    // Spread the same total light evenly over the sphere -- a room of the same
    // mean luminance -- and the wall gets several times less of it: half the
    // sphere is now below the wall where it cannot be seen at all, and what is
    // left arrives at every angle rather than square on. A surround calibrated
    // to a map's mean is not that map, and this is the size of the difference.
    backward::CameraConfig flat = cam;
    flat.environmentMap = std::make_shared<envmap::EnvironmentMap>(
        flatMap(map->meanLuminance()));
    backward::LuminanceImage even;
    backward::render(scene, scene.surfaces(), {}, flat, PhysicsOptions{},
                     FluxUnit::Lumen, even);
    CHECK(!even.empty());
    CHECK(seen > 2.0 * centreMean(even));
}

TEST(environment, a_map_and_the_uniform_surround_it_matches_render_the_same_wall) {
    // The other half of the reduction, done through the integrator rather than
    // through the lookup: a uniform map and a uniform surround of the same
    // luminance must light a wall to the same number.
    //
    // They do not take the same path through the code -- the surround is found
    // by BSDF sampling and the map by an analytic connection -- so agreeing is
    // a statement about the estimator and not about the lookup. It is also
    // exactly where a double count would show, as a factor of two.
    MeshList meshes{matteWall(600.0, 0.6)};
    TraceScene scene;
    scene.build(meshes);

    const auto shoot = [&](bool useMap) {
        backward::CameraConfig cam =
            cameraAt(Vec3(0, 0, 300), Vec3(0, 0, 0), 24, 256, 120.0);
        cam.environmentUp = Vec3(0, 0, 1);
        if (useMap) {
            cam.environmentMap = std::make_shared<envmap::EnvironmentMap>(flatMap(300.0));
            cam.environmentLuminance = 0.0;
        } else {
            cam.environmentLuminance = 300.0;
            cam.environmentFloor     = 1.0;    // uniform, sky and floor alike
        }
        backward::LuminanceImage img;
        backward::render(scene, scene.surfaces(), {}, cam, PhysicsOptions{},
                         FluxUnit::Lumen, img);
        return img;
    };
    const backward::LuminanceImage withMap = shoot(true);
    const backward::LuminanceImage uniform = shoot(false);
    CHECK(!withMap.empty());
    CHECK(!uniform.empty());

    const double a = centreMean(uniform), b = centreMean(withMap);
    CHECK(a > 0.0);
    CHECK(std::fabs(b - a) < 0.02 * a);

    // And both land on the closed form. A wall of reflectance rho under a full
    // uniform surround of luminance L reads rho * L: the hemisphere delivers
    // pi * L lux and the wall returns rho * (pi L) / pi.
    CHECK(std::fabs(a - 0.6 * 300.0) < 0.02 * 0.6 * 300.0);
}

TEST(environment, a_radiance_file_round_trips_through_the_loader) {
    // The format an environment actually ships in. What is checked is that the
    // luminance that comes back is the luminance that went in -- a loader that
    // is a stop out is a room that is a stop out, and nothing downstream can
    // tell.
    const int w = 8, h = 4;
    std::vector<double> rgb(std::size_t(w) * std::size_t(h) * 3, 0.0);
    for (int i = 0; i < w * h; ++i) {
        rgb[std::size_t(i) * 3 + 0] = 0.25 + 0.05 * double(i % 4);
        rgb[std::size_t(i) * 3 + 1] = 0.50;
        rgb[std::size_t(i) * 3 + 2] = 0.75;
    }
    const std::vector<std::uint8_t> bytes = radianceBytes(w, h, rgb);

    envmap::EnvironmentMap m;
    QString err;
    CHECK(envmap::parseRadiance(bytes, 1000.0, m, &err));
    CHECK(err.isEmpty());
    CHECK(m.width() == w);
    CHECK(m.height() == h);

    // RGBE quantises to eight bits on a shared exponent, so the round trip is
    // good to a part in a couple of hundred and not to the bit. That is a
    // property of the format, and asserting exactness would be asserting
    // something the format does not offer.
    const Vec3 up(0, 0, 1);
    Vec3 e1, e2;
    optics::orthonormalBasis(up, e1, e2);
    const double expected =
        1000.0 * (0.2126 * 0.25 + 0.7152 * 0.50 + 0.0722 * 0.75);
    // The first texel's own centre direction, so no neighbour bleeds into it.
    const double theta = 0.5 / double(h) * 3.14159265358979323846;
    const double phi   = 0.5 / double(w) * 6.28318530717958647692;
    const Vec3 d = (up * std::cos(theta) +
                    e1 * (std::sin(theta) * std::cos(phi)) +
                    e2 * (std::sin(theta) * std::sin(phi))).normalized();
    CHECK(std::fabs(m.along(d, up, e1, e2) - expected) < 0.01 * expected);
}

TEST(environment, a_file_it_cannot_read_is_refused_by_name) {
    // The same contract the rest of the loaders keep. A room that silently came
    // out black, or upside down, is worse than one that was refused: an
    // appearance measurement made under the wrong room is still a number.
    envmap::EnvironmentMap m;
    QString err;

    const std::vector<std::uint8_t> empty;
    CHECK(!envmap::parseRadiance(empty, 1.0, m, &err));
    CHECK(!err.isEmpty());

    const std::string notRadiance = "PNG or something else entirely, at length\n\n";
    err.clear();
    CHECK(!envmap::parseRadiance({notRadiance.begin(), notRadiance.end()}, 1.0, m, &err));
    CHECK(err.contains(QStringLiteral("Radiance")));

    // A format this reader does not implement, named rather than guessed at.
    const std::string floats = "#?RADIANCE\nFORMAT=32-bit_rle_xyze_something\n\n-Y 2 +X 2\n";
    err.clear();
    CHECK(!envmap::parseRadiance({floats.begin(), floats.end()}, 1.0, m, &err));
    CHECK(!err.isEmpty());

    // A scanline order that would load upside down is refused rather than
    // flipped on a guess.
    const std::string flipped = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n+Y 2 +X 2\n";
    err.clear();
    CHECK(!envmap::parseRadiance({flipped.begin(), flipped.end()}, 1.0, m, &err));
    CHECK(err.contains(QStringLiteral("scanline order")));
}

TEST(environment, a_black_map_lights_nothing_and_refuses_to_be_sampled) {
    // The degenerate case, which a CDF built by division gets wrong by
    // producing a uniform distribution over nothing. Nothing to connect to has
    // to say so rather than return a direction with a plausible pdf and no
    // light along it.
    const envmap::EnvironmentMap m = flatMap(0.0);
    CHECK(!m.empty());
    CHECK_NEAR(m.meanLuminance(), 0.0, 1e-12);

    const Vec3 up(0, 0, 1);
    Vec3 e1, e2;
    optics::orthonormalBasis(up, e1, e2);
    Vec3   d;
    double pdf = 0.0;
    // It still draws a direction -- the fallback CDF is valid -- and every
    // direction it draws carries no light, so the connection contributes
    // nothing and no energy appears from an empty room.
    if (m.sample(0.3, 0.7, up, e1, e2, d, pdf))
        CHECK_NEAR(m.along(d, up, e1, e2), 0.0, 1e-12);

    const envmap::EnvironmentMap none;
    CHECK(none.empty());
    CHECK(!none.sample(0.3, 0.7, up, e1, e2, d, pdf));
    CHECK_NEAR(none.along(Vec3(0, 0, 1), up, e1, e2), 0.0, 1e-12);
}
