// The camera-side integrator.
//
// Everything else in this suite checks where light goes. These check what an
// observer sees, which is a different measurement with its own closed forms --
// and the reason they are worth writing down is that a renderer is very easy to
// make look right and very hard to make *be* right. A picture that is a factor
// of pi out is still a perfectly convincing picture.
//
// So the tests here are all oracles rather than comparisons: a Lambertian
// emitter has a luminance nobody has to trace to know, a lit matte wall has a
// uniform one, and a receiver and a camera pointed at the same scene have to
// agree with each other.

namespace {

// A camera looking at `target` from `eye`, framing `extentMm` across.
backward::CameraConfig cameraAt(const Vec3& eye, const Vec3& target, int px,
                                int spp, double extentMm) {
    backward::CameraConfig c;
    c.eye    = eye;
    c.target = target;
    c.up     = Vec3(0, 0, 1);
    if (std::fabs((target - eye).normalized().dot(Vec3(0, 0, 1))) > 0.9)
        c.up = Vec3(0, 1, 0);
    c.width  = px;
    c.height = px;
    c.samplesPerPixel = spp;
    c.seed   = 4242u;
    // A sensor and a focal length that put `extentMm` across the frame at the
    // distance the camera actually is.
    const double dist = std::max(1e-6, (target - eye).length());
    c.sensorWidthMm = 36.0;
    c.focalLengthMm = 36.0 * dist / std::max(1e-6, extentMm);
    return c;
}

// The mean luminance over the middle of the frame, away from the edges where a
// finite emitter stops filling the pixel.
double centreMean(const backward::LuminanceImage& img, double frac = 0.25) {
    const int mx = int(img.width * frac), my = int(img.height * frac);
    double sum = 0.0;
    int    n   = 0;
    for (int y = my; y < img.height - my; ++y)
        for (int x = mx; x < img.width - mx; ++x) { sum += img.at(x, y); ++n; }
    return n > 0 ? sum / double(n) : 0.0;
}

// A matte square at z = 0 facing +z: the wall a camera looks at and a receiver
// sits above.
MeshSurface matteWall(double halfSize, double reflectance) {
    MeshSurface m;
    m.label = QStringLiteral("Matte Wall");
    const double h = halfSize;
    m.verts = {gp_Pnt(-h, -h, 0), gp_Pnt(h, -h, 0), gp_Pnt(h, h, 0), gp_Pnt(-h, h, 0)};
    Triangle a; a.v0 = 0; a.v1 = 1; a.v2 = 2; a.normal = gp_Dir(0, 0, 1);
    Triangle b; b.v0 = 0; b.v1 = 2; b.v2 = 3; b.normal = gp_Dir(0, 0, 1);
    m.tris = {a, b};
    m.index        = 0.0;                 // opaque
    m.reflectivity = reflectance;
    m.scatter      = 1.0;                 // a perfect diffuser
    return m;
}

// A Lambertian rectangle of `side` mm radiating `power` lumens along `axis`.
SourceConfig areaEmitter(double side, double power, const gp_Pnt& at,
                         const gp_Dir& axis) {
    SourceConfig s;
    s.type         = SourceConfig::Type::Lambertian;
    s.shape        = SourceConfig::Shape::Rect;
    s.sizeA        = side;
    s.sizeB        = side;
    s.halfAngleDeg = 90.0;
    s.power        = power;
    s.fluxUnit     = FluxUnit::Lumen;
    s.origin       = at;
    s.axis         = axis;
    return s;
}

} // namespace

TEST(camera, a_lambertian_emitter_reads_its_exitance_over_pi) {
    // The closed form the whole chain rests on, and the one a renderer gets
    // wrong by exactly pi if it treats a source power as a radiance.
    //
    // A Lambertian emitter of area A radiating flux Phi into its hemisphere has
    // exitance M = Phi / A and luminance L = M / pi, whatever direction it is
    // viewed from and whatever the distance -- the inverse square in the flux
    // and the inverse square in the solid angle cancel exactly, which is why
    // luminance is the quantity a camera measures and illuminance is not.
    TraceScene empty;                        // nothing to occlude or reflect

    SourceConfig src = areaEmitter(100.0, 1000.0, gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));

    // M in lm/mm^2 becomes lm/m^2 = lux, so the luminance comes out in cd/m^2.
    const double areaM2   = (src.sizeA * 1e-3) * (src.sizeB * 1e-3);
    const double exitance = src.power / areaM2;
    const double expected = backward::lambertianLuminance(exitance);

    PhysicsOptions phys;
    for (double dist : {300.0, 1200.0}) {
        // Head-on, and again from forty degrees off: a Lambertian emitter is
        // exactly as bright from either, which is the half of the definition a
        // cosine factor in the wrong place destroys.
        for (double tiltDeg : {0.0, 40.0}) {
            const double t = tiltDeg * 3.14159265358979323846 / 180.0;
            const Vec3 eye(dist * std::sin(t), 0.0, dist * std::cos(t));
            backward::LuminanceImage img;
            backward::render(empty, {}, {src},
                             cameraAt(eye, Vec3(0, 0, 0), 48, 16, 40.0),
                             phys, FluxUnit::Lumen, img);
            CHECK(!img.empty());
            // Every camera ray in the middle of the frame lands on the emitter,
            // so this is exact rather than sampled: no tolerance for noise, only
            // for arithmetic.
            CHECK_NEAR(centreMean(img), expected, 1e-9 * expected);
        }
    }
}

TEST(camera, a_source_uniform_in_solid_angle_is_not_a_lambertian_one) {
    // The two differ by a factor of two over a full hemisphere, and a renderer
    // that writes Phi / (A pi) for both is wrong about one of them. A uniform
    // emitter has Phi = L A 2 pi (1 - cos theta), so at a half-angle of ninety
    // degrees its radiance is Phi / (2 pi A) and not Phi / (pi A).
    TraceScene empty;
    SourceConfig src;
    src.type         = SourceConfig::Type::Point;    // uniform in solid angle
    src.shape        = SourceConfig::Shape::Disc;
    src.sizeA        = 50.0;
    src.halfAngleDeg = 90.0;
    src.power        = 500.0;
    src.fluxUnit     = FluxUnit::Lumen;
    src.origin       = gp_Pnt(0, 0, 0);
    src.axis         = gp_Dir(0, 0, 1);

    const double kPi    = 3.14159265358979323846;
    const double areaM2 = kPi * (src.sizeA * 1e-3) * (src.sizeA * 1e-3);
    const double expected = src.power / (2.0 * kPi * areaM2);

    backward::LuminanceImage img;
    backward::render(empty, {}, {src},
                     cameraAt(Vec3(0, 0, 400), Vec3(0, 0, 0), 48, 16, 30.0),
                     PhysicsOptions{}, FluxUnit::Lumen, img);
    CHECK(!img.empty());
    CHECK_NEAR(centreMean(img), expected, 1e-9 * expected);
    // And it is not the Lambertian answer, which is what makes the check bite.
    const double asLambertian = backward::lambertianLuminance(src.power / areaM2);
    CHECK(std::fabs(centreMean(img) - asLambertian) > 0.1 * expected);
}

TEST(camera, a_watt_run_and_a_lumen_run_differ_by_the_efficacy) {
    // The output is cd/m^2 whichever unit the sources were configured in. A run
    // in lumens is photometric already; a run in watts is weighted by V(lambda)
    // and scaled by 683 lm/W on the way out. Getting that wrong gives a picture
    // right in shape and out by two orders of magnitude in value, which is the
    // failure a luminance measurement exists to prevent.
    TraceScene empty;
    SourceConfig src = areaEmitter(80.0, 1.0, gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    src.spectrum = SpectrumConfig::Kind::Monochromatic;
    src.spectrum.wavelengthNm = 555.0;       // the peak of V(lambda)

    const auto shoot = [&](FluxUnit unit) {
        SourceConfig s = src;
        s.fluxUnit = unit;
        backward::LuminanceImage img;
        backward::render(empty, {}, {s},
                         cameraAt(Vec3(0, 0, 500), Vec3(0, 0, 0), 32, 9, 30.0),
                         PhysicsOptions{}, unit, img);
        return centreMean(img);
    };
    const double lumens = shoot(FluxUnit::Lumen);
    const double watts  = shoot(FluxUnit::Watt);
    CHECK(lumens > 0.0);
    // 683 V(lambda) is exactly the factor between the two, and nothing else.
    // V(555) is 1 by definition, to the last digit the tabulated curve holds.
    CHECK_NEAR(watts, 683.0 * spectrum::photopic(555.0) * lumens, 1e-9 * watts);
    // And the factor really is the efficacy: V peaks at 555 nm, where the
    // analytic CIE fit this application carries sits within a few per cent of
    // one. The exact check above is the unit conversion; this one is only here
    // so a reader can see that 683 V is about 683.
    CHECK_NEAR(spectrum::photopic(555.0), 1.0, 0.03);
}

TEST(camera, a_lit_diffuse_wall_renders_uniform) {
    // The furnace test, which is the one that catches a lost cosine, a missing
    // pi or a shadow ray that starts on the wrong side.
    //
    // A large Lambertian emitter facing a matte wall lights it, and the wall
    // radiates rho / pi times the illuminance it receives. What the test asserts
    // is the *uniformity*: an integrator that drops a cosine, samples the
    // emitter non-uniformly or gets the area-to-solid-angle change wrong gives a
    // wall that is brighter in the middle, and no amount of averaging hides a
    // gradient.
    MeshList meshes{matteWall(200.0, 0.8)};
    TraceScene scene;
    scene.build(meshes);
    CHECK(!scene.triangles().empty());

    // An emitter of the same size, 300 mm above it and facing down.
    const SourceConfig src =
        areaEmitter(400.0, 10000.0, gp_Pnt(0, 0, 300), gp_Dir(0, 0, -1));

    backward::LuminanceImage img;
    // From beside and above the wall, on the same side as the emitter and
    // outside its footprint, so a cosine error at the camera
    // would show as a gradient too.
    backward::render(scene, scene.surfaces(), {src},
                     cameraAt(Vec3(340, 0, 240), Vec3(0, 0, 0), 40, 64, 120.0),
                     PhysicsOptions{}, FluxUnit::Lumen, img);
    CHECK(!img.empty());

    double lo = 1e300, hi = 0.0;
    const int m = img.width / 4;
    for (int y = m; y < img.height - m; ++y)
        for (int x = m; x < img.width - m; ++x) {
            lo = std::min(lo, img.at(x, y));
            hi = std::max(hi, img.at(x, y));
        }
    CHECK(lo > 0.0);
    // The middle of a 400 mm wall under a 400 mm emitter 300 mm away is not
    // perfectly uniform -- the emitter subtends less solid angle at the edge of
    // the crop than at the centre -- so the window is what that geometry gives,
    // and what it catches is a gradient many times larger.
    CHECK(hi < 1.25 * lo);
}

TEST(camera, the_camera_and_the_receiver_measure_the_same_scene) {
    // Forward against backward, which is the acceptance test the whole stage is
    // written to.
    //
    // A matte wall under a Lambertian emitter: the forward tracer puts a
    // receiver just above the wall and reports illuminance; the camera looks at
    // the wall and reports luminance. For a Lambertian surface the two are
    // related by exactly rho / pi, so either one predicts the other and there is
    // nowhere for a factor to hide.
    //
    // The comparison is made bin for bin across the wall rather than at one
    // point, and it is made through backendcheck::compare, which sizes a
    // disagreement in each estimator's own measured error rather than in a
    // tolerance chosen after the fact. A single central point agrees for a
    // scalar factor of pi and for nothing else; the profile across the wall is
    // what catches a missing cosine, since that is exactly where a cosine
    // varies.
    MeshList meshes{matteWall(1200.0, 0.7), detectorSurface(600.0, 1.0, 16)};
    TraceScene scene;
    scene.build(meshes);

    // Small and close, so the falloff across the receiver is a factor of two
    // rather than a few per cent. A flat profile would compare nothing.
    SourceConfig src = areaEmitter(120.0, 5000.0, gp_Pnt(0, 0, 400), gp_Dir(0, 0, -1));
    src.rays = 4000000;

    TraceOptions opt;
    opt.seed     = 11111;
    opt.nTheta   = 0;                         // no far field wanted here
    opt.noiseMap = true;                      // the comparison needs error bars
    SimulationResult fwd;
    RayTracer::trace(scene, std::vector<SourceConfig>{src}, fwd, opt);
    CHECK(fwd.raysEmitted > 0);
    CHECK(fwd.nx == 16);
    CHECK(fwd.irradianceVar.size() == fwd.irradiance.size());
    if (fwd.irradiance.empty() || fwd.irradianceVar.size() != fwd.irradiance.size())
        return;

    // The receiver grid holds flux per bin, so it becomes an illuminance by
    // dividing by the bin area in square metres, and the luminance a camera must
    // see by rho / pi on top of that.
    const double perLux = 1.0 / (fwd.binArea() * 1e-6);
    const double toLum  = 0.7 / 3.14159265358979323846 * perLux;

    const int    row  = fwd.ny / 2;
    const double yc   = (double(row) + 0.5) / double(fwd.ny) * fwd.detH - 0.5 * fwd.detH;

    std::vector<double> refProfile, refVar, camProfile, camVar;
    for (int j = 0; j < fwd.nx; ++j) {
        const std::size_t i = std::size_t(row) * std::size_t(fwd.nx) + std::size_t(j);
        refProfile.push_back(fwd.irradiance[i] * toLum);
        // sqrt() of a variance entry is a one-sigma bar on the bin beside it, so
        // the same linear factor carries it across.
        refVar.push_back(fwd.irradianceVar[i] * toLum * toLum);

        // One camera per bin, looking straight down at that bin's centre and
        // framing only it. Luminance does not depend on where it is viewed from
        // for a Lambertian surface, so a head-on view per bin is not an
        // approximation of the receiver's geometry -- it is the same quantity
        // measured the other way round.
        const double xc = (double(j) + 0.5) / double(fwd.nx) * fwd.detW - 0.5 * fwd.detW;
        const double binW = fwd.detW / double(fwd.nx);
        backward::LuminanceImage img;
        backward::render(scene, scene.surfaces(), {src},
                         cameraAt(Vec3(xc, yc, 500), Vec3(xc, yc, 0), 12, 192, binW),
                         PhysicsOptions{}, FluxUnit::Lumen, img);
        CHECK(!img.empty());
        if (img.empty()) return;
        camProfile.push_back(centreMean(img));
        // The error on that mean: the pixels are independent samples of the same
        // patch, so their own standard errors combine in quadrature.
        double s2 = 0.0;
        int    n  = 0;
        const int mx = int(img.width * 0.25), my = int(img.height * 0.25);
        for (int y = my; y < img.height - my; ++y)
            for (int x = mx; x < img.width - mx; ++x) {
                const double e = img.stdErr[std::size_t(y) * std::size_t(img.width) +
                                            std::size_t(x)];
                s2 += e * e;
                ++n;
            }
        camVar.push_back(n > 0 ? s2 / (double(n) * double(n)) : 0.0);
    }

    // The two estimators, dressed as two runs so the one function that already
    // knows how to hold two measurements against each other can do it. Neither
    // is a "backend" in the GPU sense and the shape of the struct says more than
    // either fills: the energy buckets are what a forward run books and what a
    // camera has no notion of, so they are left at zero on both sides and the
    // rows they produce assert nothing. What is asserted is the level, through
    // `efficiency`, and the profile, through the map.
    const auto asRun = [&](const std::vector<double>& v, const std::vector<double>& var,
                           std::size_t rays) {
        SimulationResult r;
        r.nx = int(v.size());
        r.ny = 1;
        r.detW = 1.0;
        r.detH = 1.0;
        r.unit = FluxUnit::Lumen;
        r.irradiance    = v;
        r.irradianceVar = var;
        r.raysEmitted     = rays;
        r.raysHitDetector = rays;
        double sum = 0.0, s2 = 0.0;
        for (std::size_t i = 0; i < v.size(); ++i) { sum += v[i]; s2 += var[i]; }
        const double n = double(std::max<std::size_t>(1, v.size()));
        r.sourcePower      = 1.0;
        r.fluxDetector     = sum / n;                  // mean luminance, cd/m^2
        r.efficiency       = r.fluxDetector;
        r.efficiencyStdErr = std::sqrt(s2) / n;
        return r;
    };
    const SimulationResult a = asRun(refProfile, refVar, fwd.raysEmitted);
    const SimulationResult b = asRun(camProfile, camVar, fwd.raysEmitted);

    const backendcheck::Report rep = backendcheck::compare(a, b);
    CHECK(rep.blocker.isEmpty());
    CHECK(rep.ok);

    // And the level is a real number rather than two matching zeros.
    CHECK(a.efficiency > 0.0);
    CHECK(b.efficiency > 0.0);
}

TEST(camera, it_renders_the_same_image_however_many_threads_run_it) {
    // The determinism contract, which a camera has to keep for the same reason
    // the tracer does: a luminance figure somebody quotes has to be the same one
    // tomorrow. Every sample is seeded from the pixel index and the sample
    // index, so which worker rendered which row cannot reach the answer.
    TraceScene empty;
    SourceConfig src;
    src.type     = SourceConfig::Type::Lambertian;
    src.shape    = SourceConfig::Shape::Sphere;
    src.sizeA    = 40.0;
    src.power    = 700.0;
    src.fluxUnit = FluxUnit::Lumen;
    src.origin   = gp_Pnt(0, 0, 0);

    backward::CameraConfig cam = cameraAt(Vec3(0, -400, 0), Vec3(0, 0, 0), 24, 9, 120.0);
    backward::LuminanceImage one, many;
    cam.threads = 1;
    backward::render(empty, {}, {src}, cam, PhysicsOptions{}, FluxUnit::Lumen, one);
    cam.threads = 8;
    backward::render(empty, {}, {src}, cam, PhysicsOptions{}, FluxUnit::Lumen, many);

    CHECK(one.luminance.size() == many.luminance.size());
    CHECK(!one.luminance.empty());
    bool identical = true;
    for (std::size_t i = 0; i < one.luminance.size(); ++i)
        if (one.luminance[i] != many.luminance[i]) identical = false;
    CHECK(identical);
}

TEST(camera, the_surround_is_reached_by_sampling_and_never_counted_twice) {
    // A room, and the one arrangement that keeps it honest.
    //
    // The analytic emitters are reached by a connection at every scattering
    // vertex; the surround is reached only by a path that leaves the scene
    // without hitting anything. Nothing ever connects to it, so it needs no flag
    // to keep it out of a second count -- and a matte wall in a uniform room of
    // luminance L must read exactly rho * L, because a Lambertian surface under
    // uniform illumination returns what it is given times its reflectance.
    MeshList meshes{matteWall(300.0, 0.6)};
    TraceScene scene;
    scene.build(meshes);

    // One emitter is required by the integrator and this one carries nothing,
    // so the room is the only light in the picture.
    SourceConfig dark = areaEmitter(10.0, 0.0, gp_Pnt(0, 0, 900), gp_Dir(0, 0, -1));
    dark.power = 1e-12;

    backward::CameraConfig cam =
        cameraAt(Vec3(200, 0, 260), Vec3(0, 0, 0), 32, 256, 120.0);
    cam.environmentLuminance = 1000.0;
    cam.environmentFloor     = 1.0;          // a uniform room, floor and all

    backward::LuminanceImage img;
    backward::render(scene, scene.surfaces(), {dark}, cam,
                     PhysicsOptions{}, FluxUnit::Lumen, img);
    CHECK(!img.empty());
    // Half the surround is below the wall and cannot reach its front face, so a
    // wall facing up sees the upper hemisphere only: rho * L.
    CHECK_NEAR(centreMean(img), 0.6 * 1000.0, 0.04 * 0.6 * 1000.0);
}

TEST(camera, the_image_forms_while_it_runs) {
    // Progressive refinement, which is what makes a long render worth watching
    // and is not the same thing as rendering quickly.
    //
    // Each pass adds one sample to every pixel, so a partial is a noisy whole
    // picture rather than a sharp fraction of one. What the test asserts is
    // exactly that: every partial covers the entire frame, the sample count
    // climbs, and the last one is the finished image.
    TraceScene empty;
    const SourceConfig src =
        areaEmitter(150.0, 3000.0, gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));

    backward::CameraConfig cam = cameraAt(Vec3(0, 0, 500), Vec3(0, 0, 0), 16, 24, 60.0);
    std::vector<std::uint32_t> seen;
    backward::RenderControl ctl;
    ctl.partialIntervalMs = 0;               // every pass, so the test is not timed
    ctl.partial = [&](const backward::LuminanceImage& p) {
        CHECK(p.width == cam.width);
        CHECK(p.height == cam.height);
        // A partial is the whole frame at fewer samples, never part of a frame.
        std::uint32_t lo = 0xffffffffu, hi = 0u;
        for (std::uint32_t s : p.samples) { lo = std::min(lo, s); hi = std::max(hi, s); }
        CHECK(lo == hi);
        CHECK(lo > 0);
        seen.push_back(lo);
    };

    backward::LuminanceImage img;
    backward::render(empty, {}, {src}, cam, PhysicsOptions{}, FluxUnit::Lumen, img, ctl);
    CHECK(seen.size() >= 2);
    bool climbing = true;
    for (std::size_t i = 1; i < seen.size(); ++i)
        if (seen[i] <= seen[i - 1]) climbing = false;
    CHECK(climbing);
    CHECK(!seen.empty() && seen.back() == std::uint32_t(cam.samplesPerPixel));
}

TEST(camera, an_aperture_blurs_what_is_out_of_focus_and_not_what_is_in_it) {
    // The thin lens. A pinhole has everything sharp; a real aperture does not,
    // and a luminance meter has a real aperture. What has to be true is that
    // opening one up leaves the plane in focus alone and softens the edge of
    // something well away from it.
    TraceScene empty;
    // A small emitter at the focus, and a second one much closer to the camera.
    const SourceConfig atFocus = areaEmitter(60.0, 2000.0, gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    const SourceConfig nearby  = areaEmitter(60.0, 2000.0, gp_Pnt(0, 0, 300), gp_Dir(0, 0, 1));

    const auto edgeSoftness = [&](const SourceConfig& s, double fNumber) {
        backward::CameraConfig cam =
            cameraAt(Vec3(0, 0, 600), Vec3(0, 0, 0), 64, 64, 160.0);
        cam.fNumber = fNumber;               // 0 is a pinhole
        backward::LuminanceImage img;
        backward::render(empty, {}, {s}, cam, PhysicsOptions{}, FluxUnit::Lumen, img);
        // How many pixels sit between fully lit and fully dark: a sharp edge has
        // almost none, a blurred one has a band.
        double lo = 1e300, hi = 0.0;
        for (double v : img.luminance) { lo = std::min(lo, v); hi = std::max(hi, v); }
        int partial = 0;
        for (double v : img.luminance)
            if (v > lo + 0.1 * (hi - lo) && v < hi - 0.1 * (hi - lo)) ++partial;
        return partial;
    };

    // The emitter at the focus stays as sharp as the pinhole draws it.
    const int focusPin  = edgeSoftness(atFocus, 0.0);
    const int focusOpen = edgeSoftness(atFocus, 1.4);
    CHECK(focusOpen <= focusPin + 8);

    // The one three hundred millimetres nearer does not.
    const int nearPin  = edgeSoftness(nearby, 0.0);
    const int nearOpen = edgeSoftness(nearby, 1.4);
    CHECK(nearOpen > nearPin + 8);
}
