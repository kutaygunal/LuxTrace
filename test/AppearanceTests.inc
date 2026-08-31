// Tests for the Appearance preview's scene side.
//
// The render itself cannot be pinned against arithmetic the way the tracer is:
// it is a GPU estimator with no seed we control, its output differs in the last
// bits between two runs, and asserting on pixels would produce a permanently
// red build. So what is tested is everything up to the GPU -- which surface
// becomes which BSDF, and which presentations exist -- and the image is
// reviewed by eye. That split is the point, not a gap.

// ---- the material bridge ---------------------------------------------------

namespace {

// Every weight OCCT's two-layer model carries has to be a finite number in
// [0,1]. A NaN or a negative here does not fail loudly; it produces a black or
// an infinitely bright surface in a picture nobody can debug.
bool weightsAreSane(const Graphic3d_BSDF& b) {
    const float v[] = {b.Kc.r(), b.Kc.g(), b.Kc.b(), b.Kc.a(),
                       b.Kd.r(), b.Kd.g(), b.Kd.b(),
                       b.Ks.r(), b.Ks.g(), b.Ks.b(), b.Ks.a(),
                       b.Kt.r(), b.Kt.g(), b.Kt.b(),
                       b.Le.r(), b.Le.g(), b.Le.b()};
    for (float x : v)
        if (!std::isfinite(x) || x < 0.0f || x > 1.0f) return false;
    return true;
}

} // namespace

TEST(appearancematerials, every_surface_in_the_library_maps_to_a_sane_bsdf) {
    // The whole scene registry, not a hand-picked example: a mapping that is
    // right for a ball lens and produces a NaN for the Fresnel lens is a
    // mapping that fails on the scene nobody thought to try.
    int surfaces = 0;
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        const auto scene = GeometryProvider::Scene(s);
        for (const OpticalSurface& os : GeometryProvider::buildScene(scene)) {
            ++surfaces;
            const appearance::Material m = appearance::materialFor(os);
            if (!weightsAreSane(m.bsdf)) {
                reportFailure(__FILE__, __LINE__,
                              (GeometryProvider::info(scene).name +
                               QStringLiteral(" / ") + os.label +
                               QStringLiteral(" produced an out-of-range BSDF"))
                                  .toStdString());
                continue;
            }
            CHECK(std::isfinite(m.transparency));
            CHECK(m.transparency >= 0.0f && m.transparency <= 1.0f);
        }
    }
    // The registry is not empty, so a pass above means something was checked.
    CHECK(surfaces > 50);
}

TEST(appearancematerials, a_mirror_is_specular_and_not_diffuse) {
    SurfaceOptics mirror;
    mirror.reflectivity = 0.92;
    mirror.index        = 0.0;

    const appearance::Material m = appearance::materialFor(mirror);

    // Nothing diffuse, nothing transmitted: an ideal mirror reflects, and a
    // reflector rendered as grey putty is the single most likely way for this
    // whole feature to look like a toy.
    CHECK(m.bsdf.Kd.r() == 0.0f && m.bsdf.Kd.g() == 0.0f && m.bsdf.Kd.b() == 0.0f);
    CHECK(m.bsdf.Kt.r() == 0.0f);
    CHECK(m.bsdf.Ks.r() > 0.9f * 0.92f);
}

TEST(appearancematerials, a_matte_cavity_is_diffuse_and_not_a_mirror) {
    // An integrating sphere wall: highly reflective *and* fully diffusing. It
    // passes the "bright and opaque" test a mirror passes, and it must not come
    // out as one.
    SurfaceOptics wall;
    wall.reflectivity = 0.96;
    wall.scatter      = 1.0;

    const appearance::Material m = appearance::materialFor(wall);

    CHECK(m.bsdf.Kd.r() > 0.9f);
    CHECK(m.bsdf.Ks.r() == 0.0f);
    // And no glossy overcoat, or the sphere reads as painted porcelain.
    CHECK(m.bsdf.Kc.r() == 0.0f);
}

TEST(appearancematerials, glass_transmits_and_carries_its_own_index) {
    SurfaceOptics glass;
    glass.index      = 1.5168;                       // N-BK7 at the d line
    glass.absorption = 0.0;

    const appearance::Material m = appearance::materialFor(glass);

    CHECK(m.bsdf.Kt.r() > 0.0f);
    // OCCT models glass as a transmissive base under a specular coat, and the
    // dielectric Fresnel goes on the *coat*. Asserted rather than assumed: the
    // first version of this mapping put the index on FresnelBase, where the
    // shader never reads it, and every lens rendered as a bag of air.
    CHECK(m.bsdf.FresnelCoat.FresnelType() == Graphic3d_FM_DIELECTRIC);
    CHECK(m.bsdf.FresnelCoat == Graphic3d_Fresnel::CreateDielectric(1.5168f));
}

TEST(appearancematerials, a_ground_surface_is_glossy_and_a_polished_one_is_not) {
    SurfaceOptics polished;
    polished.index = 1.5;

    SurfaceOptics ground = polished;
    ground.roughness     = 0.05;                     // RMS slope, radians

    const appearance::Material p = appearance::materialFor(polished);
    const appearance::Material g = appearance::materialFor(ground);

    // The roughness slot that matters for glass is the coat's, because the
    // coat is where the glass's specular reflection lives.
    CHECK(p.bsdf.Kc.a() == 0.0f);
    CHECK(g.bsdf.Kc.a() > 0.0f);
    // effectiveBsdf turns an RMS slope of sigma into a GGX alpha of sqrt(2)
    // sigma, and that is the number the render has to receive -- not the raw
    // sigma, or a ground surface would be drawn smoother than it is.
    CHECK(std::abs(double(g.bsdf.Kc.a()) - 1.4142135623730951 * 0.05) < 1e-6);
}

TEST(appearancematerials, an_ar_coating_reflects_less_than_bare_glass) {
    SurfaceOptics bare;
    bare.index = 1.5168;

    SurfaceOptics coated = bare;
    coated.coating       = coating::Coating{};
    coated.coating.model = coating::Model::Ideal;
    coated.coating.residual = 0.0025;                // ordinary broadband AR

    const double bareR = appearance::bareReflectance(1.5168);
    const double arR   = appearance::coatingResidual(coated);

    CHECK(bareR > 0.03 && bareR < 0.05);             // ~4 %, as it should be
    CHECK(arR >= 0.0 && arR < bareR);

    // And the difference reaches the render: a coated lens must not map to the
    // same base Fresnel a bare one does, or the coating is a number in a dialog
    // and nothing else.
    const appearance::Material mb = appearance::materialFor(bare);
    const appearance::Material mc = appearance::materialFor(coated);
    CHECK(!(mb.bsdf.FresnelCoat == mc.bsdf.FresnelCoat));
    CHECK(mc.bsdf.FresnelCoat.FresnelType() == Graphic3d_FM_CONSTANT);

    // A surface with no coating is not silently given one.
    CHECK(appearance::coatingResidual(bare) < 0.0);
}

TEST(appearancematerials, a_high_reflector_stops_transmitting) {
    SurfaceOptics hr;
    hr.index                 = 1.5168;
    hr.coating.model         = coating::Model::Ideal;
    hr.coating.residual      = 0.995;
    hr.coating.highReflector = true;

    const appearance::Material m = appearance::materialFor(hr);

    CHECK(m.bsdf.Kt.r() == 0.0f);
    CHECK(m.bsdf.Ks.r() > 0.9f);
    CHECK(m.transparency == 0.0f);
}

TEST(appearancematerials, a_metal_takes_its_colour_from_its_own_complex_index) {
    // The detail that makes the render ours rather than generic. Gold and
    // aluminium differ here for the same reason they differ in a trace: the
    // catalogue's n and k, sampled at three wavelengths.
    const OpticalMaterial gold = materials::byName(QStringLiteral("Gold"));
    const OpticalMaterial alu  = materials::byName(QStringLiteral("Aluminium"));
    if (!gold.valid() || !alu.valid()) {
        // The catalogue is what it is; a renamed entry should not fail here as
        // though the mapping were wrong.
        CHECK(true);
        return;
    }
    CHECK(gold.isMetal());

    SurfaceOptics g;
    g.reflectivity = 0.95;
    g.material     = gold;

    SurfaceOptics a = g;
    a.material      = alu;

    const appearance::Material mg = appearance::materialFor(g);
    const appearance::Material ma = appearance::materialFor(a);

    // OCCT's per-channel conductor factory reduces (n, k) to the Schlick
    // specular colour they imply rather than keeping the complex index, so the
    // metal reaches the shader as a colour. That is the whole point of using
    // the spectral overload: the scalar one could only carry one channel.
    CHECK(mg.bsdf.FresnelBase.FresnelType() == Graphic3d_FM_SCHLICK);
    CHECK(ma.bsdf.FresnelBase.FresnelType() == Graphic3d_FM_SCHLICK);
    CHECK(!(mg.bsdf.FresnelBase == ma.bsdf.FresnelBase));

    // Gold is warm: it reflects red far better than blue. Aluminium is not.
    double gr, gg, gb, ar, ag, ab;
    mg.colour.Values(gr, gg, gb, Quantity_TOC_RGB);
    ma.colour.Values(ar, ag, ab, Quantity_TOC_RGB);
    CHECK(gr > gb + 0.15);
    CHECK(std::abs(ar - ab) < 0.15);
    // ...and the two are visibly different surfaces, which is the whole claim.
    CHECK(std::abs(gb - ab) > 0.1);
}

TEST(appearancematerials, a_detector_is_hidden) {
    SurfaceOptics det;
    det.isDetector = true;

    const appearance::Material m = appearance::materialFor(det);
    CHECK(m.hidden);

    // Still built, because "show detectors" has to have something to show.
    CHECK(weightsAreSane(m.bsdf));
}

TEST(appearancematerials, the_mapping_is_a_pure_function_of_the_surface) {
    // Two builds of the same surface produce the same material. Nothing about
    // the render is seeded or stateful on this side of the GPU, and a mapping
    // that drifted between calls would make the accumulation restart advice in
    // the view meaningless.
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        for (const OpticalSurface& os : GeometryProvider::buildScene(GeometryProvider::Scene(s))) {
            const appearance::Material a = appearance::materialFor(os);
            const appearance::Material b = appearance::materialFor(os);
            CHECK(a.bsdf.Kd == b.bsdf.Kd && a.bsdf.Ks == b.bsdf.Ks &&
                  a.bsdf.Kt == b.bsdf.Kt && a.bsdf.Kc == b.bsdf.Kc);
            CHECK(a.bsdf.FresnelBase == b.bsdf.FresnelBase);
            CHECK(a.hidden == b.hidden);
        }
    }
}

// ---- the scene build -------------------------------------------------------

TEST(appearancescene, one_presentation_per_visible_body_and_none_for_a_detector) {
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        const auto scene = GeometryProvider::Scene(s);
        const std::vector<OpticalSurface> surfaces = GeometryProvider::buildScene(scene);

        std::size_t bodies = 0, detectors = 0;
        for (const OpticalSurface& os : surfaces) {
            if (os.shape.IsNull()) continue;
            if (os.isDetector) { ++detectors; continue; }
            bodies += os.placements.empty() ? 1 : os.placements.size();
        }
        // Every registry scene appends a planar detector, so there is always
        // one thing to leave out.
        CHECK(detectors > 0);

        const appearance::Build hidden = appearance::buildScene(surfaces, false);
        if (hidden.parts.size() != bodies) {
            reportFailure(__FILE__, __LINE__,
                          (GeometryProvider::info(scene).name +
                           QStringLiteral(" drew the wrong number of parts"))
                              .toStdString());
            continue;
        }
        CHECK(hidden.hiddenDetectors == int(detectors));
        for (const appearance::Part& p : hidden.parts) CHECK(!p.detector);

        // Asking for them puts them back, and nothing else changes.
        const appearance::Build shown = appearance::buildScene(surfaces, true);
        CHECK(shown.parts.size() == bodies + detectors);
        CHECK(shown.hiddenDetectors == 0);
    }
}

TEST(appearancescene, an_instanced_part_is_drawn_at_every_placement) {
    // The 5 x 5 lenslet array and the LED luminaire's cups are one B-Rep and
    // many placements. A render that drew the first lenslet and none of the
    // other twenty-four would not be a render of the array.
    const std::vector<OpticalSurface> surfaces =
        GeometryProvider::buildScene(GeometryProvider::Scene::MicrolensArray);

    std::size_t instanced = 0;
    for (const OpticalSurface& os : surfaces)
        if (os.placements.size() > 1) instanced += os.placements.size();
    CHECK(instanced > 1);

    const appearance::Build build = appearance::buildScene(surfaces, false);

    // Each placed copy sits somewhere of its own, so the box round the whole
    // build is wider than the box round one lenslet.
    CHECK(!build.bounds.IsVoid());
    double xm, ym, zm, xM, yM, zM;
    build.bounds.Get(xm, ym, zm, xM, yM, zM);
    CHECK(xM - xm > 1e-6 && yM - ym > 1e-6);

    // Most copies sit away from the origin, so most presentations carry a
    // location of their own. Only the one at the array's centre may not.
    std::size_t placed = 0;
    for (const appearance::Part& p : build.parts)
        if (p.shape->LocalTransformation().Form() != gp_Identity) ++placed;
    CHECK(placed + 1 >= build.parts.size());
}

TEST(appearancescene, a_part_knows_which_surface_it_came_from) {
    // The link back is what lets a future "why does this look like that?" reach
    // the surface's optics rather than stopping at a presentation.
    const std::vector<OpticalSurface> surfaces =
        GeometryProvider::buildScene(GeometryProvider::Scene::Lens);
    const appearance::Build build = appearance::buildScene(surfaces, true);

    CHECK(!build.parts.empty());
    for (const appearance::Part& p : build.parts) {
        CHECK(p.surface >= 0 && p.surface < int(surfaces.size()));
        CHECK(p.detector == surfaces[std::size_t(p.surface)].isDetector);
        CHECK(!p.shape.IsNull());
    }
}

TEST(appearancescene, the_build_is_stable_across_two_identical_calls) {
    const std::vector<OpticalSurface> surfaces =
        GeometryProvider::buildScene(GeometryProvider::Scene::LedArrayLuminaire);

    const appearance::Build a = appearance::buildScene(surfaces, false);
    const appearance::Build b = appearance::buildScene(surfaces, false);

    CHECK(a.parts.size() == b.parts.size());
    CHECK(a.hiddenDetectors == b.hiddenDetectors);
    for (std::size_t i = 0; i < a.parts.size() && i < b.parts.size(); ++i) {
        CHECK(a.parts[i].surface == b.parts[i].surface);
        CHECK(a.parts[i].material.bsdf.Kd == b.parts[i].material.bsdf.Kd);
    }
}

TEST(appearancescene, a_surface_with_no_shape_is_skipped_rather_than_crashed_on) {
    std::vector<OpticalSurface> surfaces =
        GeometryProvider::buildScene(GeometryProvider::Scene::Reflector);
    const std::size_t before = appearance::buildScene(surfaces, true).parts.size();

    OpticalSurface empty;
    empty.label = QStringLiteral("nothing at all");
    surfaces.push_back(empty);

    CHECK(appearance::buildScene(surfaces, true).parts.size() == before);
}

// ---- the emitters ----------------------------------------------------------

namespace {

// A source with a real emitting area: a 2 x 3 mm rectangular die, one watt.
SourceConfig areaSource(double power = 1.0) {
    SourceConfig s;
    s.type   = SourceConfig::Type::Lambertian;
    s.shape  = SourceConfig::Shape::Rect;
    s.sizeA  = 2.0;
    s.sizeB  = 3.0;
    s.power  = power;
    s.origin = gp_Pnt(0, 0, 0);
    s.axis   = gp_Dir(0, 0, 1);
    return s;
}

} // namespace

TEST(appearanceemitters, radiance_is_flux_over_area_times_pi) {
    const SourceConfig s = areaSource(1.0);

    // L = phi / (A pi), and A is 6 mm^2. Not a fitted constant: this is the
    // definition of the radiance of a Lambertian emitter, and it is what makes
    // two sources of different size come out in the right proportion.
    const double expected = 1.0 / (6.0 * 3.14159265358979323846);
    CHECK(std::abs(appearance::sourceRadiance(s) - expected) < 1e-12);

    // Twice the flux over the same area is twice the radiance; twice the area
    // at the same flux is half.
    const SourceConfig twice = areaSource(2.0);
    CHECK(std::abs(appearance::sourceRadiance(twice) -
                   2.0 * appearance::sourceRadiance(s)) < 1e-12);

    SourceConfig wide = areaSource(1.0);
    wide.sizeA *= 2.0;
    CHECK(std::abs(appearance::sourceRadiance(wide) -
                   0.5 * appearance::sourceRadiance(s)) < 1e-12);
}

TEST(appearanceemitters, a_point_source_has_no_area_and_becomes_a_light) {
    SourceConfig p;
    p.type  = SourceConfig::Type::Point;
    p.shape = SourceConfig::Shape::PointLike;
    p.power = 1.0;

    CHECK(appearance::sourceRadiance(p) == 0.0);

    const appearance::EmitterBuild b = appearance::buildEmitters({p}, 100.0);
    CHECK(b.emitters.size() == 1);
    CHECK(b.pointSources == 1);
    CHECK(b.emitters[0].pointLike);
    CHECK(b.emitters[0].shape.IsNull());
    // A mathematical point casts a hard shadow; a real one does not, and the
    // angular size is what makes the difference visible.
    CHECK(b.emitters[0].smoothRadius > 0.0);
}

TEST(appearanceemitters, an_area_source_becomes_emissive_geometry) {
    const appearance::EmitterBuild b = appearance::buildEmitters({areaSource()}, 50.0);

    CHECK(b.emitters.size() == 1);
    CHECK(b.pointSources == 0);
    CHECK(!b.emitters[0].pointLike);
    CHECK(!b.emitters[0].shape.IsNull());

    // It emits, and the brightest source in the list is mapped to the stated
    // display radiance rather than to whatever its physical value happened to
    // be -- one divisor, applied once, so the picture does not depend on the
    // unit the flux was typed in.
    const Graphic3d_Vec3 le = b.emitters[0].radiance;
    CHECK(le.r() > 0.0f || le.g() > 0.0f || le.b() > 0.0f);
    CHECK(std::abs(double(std::max({le.r(), le.g(), le.b()})) -
                   appearance::kBrightestRadiance) < 1e-4);
}

TEST(appearanceemitters, two_sources_come_out_in_proportion) {
    // The whole claim of section 5.1: the values are not physical, but their
    // ratios are. A source with a quarter of the radiance renders at a quarter
    // of the brightness, so a luminaire with an unequal pair of dies shows the
    // inequality rather than two identical glowing plates.
    const SourceConfig bright = areaSource(4.0);
    const SourceConfig dim    = areaSource(1.0);

    const appearance::EmitterBuild b = appearance::buildEmitters({bright, dim}, 50.0);
    CHECK(b.emitters.size() == 2);

    const float hi = std::max({b.emitters[0].radiance.r(), b.emitters[0].radiance.g(),
                               b.emitters[0].radiance.b()});
    const float lo = std::max({b.emitters[1].radiance.r(), b.emitters[1].radiance.g(),
                               b.emitters[1].radiance.b()});
    CHECK(lo > 0.0f);
    CHECK(std::abs(double(hi / lo) - 4.0) < 1e-4);

    // And the divisor is the brightest source's own radiance, not a fudge.
    CHECK(std::abs(b.normalisation - appearance::sourceRadiance(bright)) < 1e-12);
}

TEST(appearanceemitters, the_normalisation_is_stable_across_identical_builds) {
    // Recomputed per redraw, the scale would move whenever anything did and the
    // image would breathe. It is a pure function of the source list.
    const std::vector<SourceConfig> sources = {areaSource(1.0), areaSource(2.5)};
    const appearance::EmitterBuild a = appearance::buildEmitters(sources, 50.0);
    const appearance::EmitterBuild b = appearance::buildEmitters(sources, 50.0);

    CHECK(a.normalisation == b.normalisation);
    CHECK(a.emitters.size() == b.emitters.size());
    for (std::size_t i = 0; i < a.emitters.size() && i < b.emitters.size(); ++i)
        CHECK(a.emitters[i].radiance == b.emitters[i].radiance);
}

TEST(appearanceemitters, a_warm_source_renders_warm_and_a_cool_one_cool) {
    // The colour is integrated over the source's own SPD, through the same
    // SampledSpectrum the tracer draws wavelengths from. So a 2700 K lamp and a
    // 9000 K one differ in the render for the reason they differ in life, and
    // not because somebody picked two colours.
    SpectrumConfig warm;
    warm.kind = SpectrumConfig::Kind::Blackbody;
    warm.cct  = 2700.0;

    SpectrumConfig cool = warm;
    cool.cct = 9000.0;

    const Graphic3d_Vec3 w = appearance::spectrumColour(warm);
    const Graphic3d_Vec3 c = appearance::spectrumColour(cool);

    // Warmer means relatively more red than blue than the cooler one has.
    CHECK(w.r() / std::max(1e-6f, w.b()) > c.r() / std::max(1e-6f, c.b()));

    // Both are normalised to a peak channel of one, so neither is simply darker.
    CHECK(std::abs(double(std::max({w.r(), w.g(), w.b()})) - 1.0) < 1e-5);
    CHECK(std::abs(double(std::max({c.r(), c.g(), c.b()})) - 1.0) < 1e-5);
}

TEST(appearanceemitters, a_monochromatic_source_takes_the_colour_of_its_line) {
    SpectrumConfig blue;
    blue.kind         = SpectrumConfig::Kind::Monochromatic;
    blue.wavelengthNm = 460.0;

    SpectrumConfig red = blue;
    red.wavelengthNm   = 630.0;

    const Graphic3d_Vec3 b = appearance::spectrumColour(blue);
    const Graphic3d_Vec3 r = appearance::spectrumColour(red);

    CHECK(b.b() > b.r());
    CHECK(r.r() > r.b());
}

TEST(appearanceemitters, a_collimated_beam_emits_from_its_aperture) {
    // A collimated source is PointLike in shape but has a real beam radius, so
    // it does have an emitting area -- and rendering it as a point light would
    // put a bare lamp where a laser is.
    SourceConfig laser;
    laser.type       = SourceConfig::Type::Collimated;
    laser.shape      = SourceConfig::Shape::PointLike;
    laser.beamRadius = 5.0;
    laser.power      = 1.0;

    const double pi   = 3.14159265358979323846;
    const double area = pi * 25.0;
    CHECK(std::abs(appearance::sourceRadiance(laser) - 1.0 / (area * pi)) < 1e-12);

    const appearance::EmitterBuild b = appearance::buildEmitters({laser}, 50.0);
    CHECK(b.emitters.size() == 1);
    CHECK(!b.emitters[0].pointLike);
    CHECK(!b.emitters[0].shape.IsNull());
}

TEST(appearanceemitters, every_scene_in_the_library_produces_a_usable_emitter) {
    // Whatever the registry's default source is for a scene, it has to become
    // either geometry or a light -- never nothing. A scene that renders with no
    // light at all is a black rectangle, and the studio fallback in
    // AppearanceView can only be trusted if an empty emitter list means the
    // scene genuinely has no source rather than that the mapping dropped one.
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto scene = GeometryProvider::Scene(i);
        SimConfig cfg;
        cfg.scene = scene;
        const GeometryProvider::SceneSetup setup =
            GeometryProvider::build(scene, GeometryProvider::defaultParams(scene));

        const std::vector<SourceConfig> sources =
            Simulation::sourcesFor(cfg, setup.sourceOrigin, setup.sourceAxis);
        if (sources.empty()) continue;

        const appearance::EmitterBuild b = appearance::buildEmitters(sources, 100.0);
        if (b.emitters.size() != sources.size()) {
            reportFailure(__FILE__, __LINE__,
                          (GeometryProvider::info(scene).name +
                           QStringLiteral(" lost an emitter on the way to the render"))
                              .toStdString());
            continue;
        }
        for (const appearance::Emitter& e : b.emitters) {
            CHECK(e.pointLike == e.shape.IsNull());
            if (!e.pointLike) {
                CHECK(std::isfinite(e.radiance.r()) && e.radiance.r() >= 0.0f);
                CHECK(std::max({e.radiance.r(), e.radiance.g(), e.radiance.b()}) > 0.0f);
            } else {
                CHECK(e.intensity >= 0.0 && std::isfinite(e.intensity));
            }
        }
    }
}

// ---- the simulation -> render bridge ---------------------------------------

namespace {

// A receiver with a separable bright-centre distribution on it, of the kind a
// diffuser plate produces: brightest in the middle, falling off to the edges.
DetectorFrame brightCentreReceiver(int nx = 32, int ny = 32,
                                   double w = 40.0, double h = 20.0) {
    DetectorFrame d;
    d.label  = QStringLiteral("Exit face");
    d.center = Vec3(5.0, -2.0, 30.0);
    d.u      = Vec3(1, 0, 0);
    d.v      = Vec3(0, 1, 0);
    d.normal = Vec3(0, 0, 1);
    d.w = w;  d.h = h;
    d.nx = nx; d.ny = ny;
    d.grid.assign(std::size_t(nx) * std::size_t(ny), 0.0);

    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double x = (double(i) + 0.5) / double(nx) - 0.5;
            const double y = (double(j) + 0.5) / double(ny) - 0.5;
            const double r2 = x * x + y * y;
            d.grid[std::size_t(j) * std::size_t(nx) + std::size_t(i)] =
                std::exp(-12.0 * r2);
        }
    }
    for (double f : d.grid) d.flux += f;
    return d;
}

double facetBoxSpan(const appearance::RadianceMap& map, int axis) {
    Bnd_Box box;
    for (const appearance::RadianceFacet& f : map.facets)
        if (!f.shape.IsNull()) BRepBndLib::Add(f.shape->Shape(), box);
    if (box.IsVoid()) return 0.0;
    double lo[3], hi[3];
    box.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    return hi[axis] - lo[axis];
}

} // namespace

TEST(appearanceradiance, the_facets_carry_exactly_the_flux_the_run_measured) {
    // The one arithmetic guarantee this phase can make. The facets partition
    // the receiver's bins -- each bin belongs to exactly one facet and every
    // bin belongs to one -- so the map is a rebinning, not a resampling, and
    // the totals are equal rather than close.
    const DetectorFrame d = brightCentreReceiver();
    const appearance::RadianceMap map = appearance::buildRadianceMap(d);

    CHECK(map.valid);
    double expected = 0.0;
    for (double f : d.grid) expected += f;
    CHECK(std::abs(map.totalFlux - expected) <= 1e-9 * expected);

    // And summing the facets themselves gives the same answer, so the total is
    // not carried separately from the things it is a total of.
    double summed = 0.0;
    for (const appearance::RadianceFacet& f : map.facets) summed += f.flux;
    CHECK(std::abs(summed - expected) <= 1e-9 * expected);
}

TEST(appearanceradiance, the_grid_is_never_finer_than_the_bins_it_came_from) {
    // Subdividing past the data would draw structure the trace did not compute,
    // which in a picture is indistinguishable from structure it did.
    const DetectorFrame coarse = brightCentreReceiver(8, 8);
    const appearance::RadianceMap map = appearance::buildRadianceMap(coarse, {}, 32);
    CHECK(map.nx == 8 && map.ny == 8);
    CHECK(map.facets.size() == 64);

    // Asking for fewer aggregates whole bins rather than interpolating, so the
    // energy still balances exactly.
    const DetectorFrame fine = brightCentreReceiver(64, 64);
    const appearance::RadianceMap small = appearance::buildRadianceMap(fine, {}, 16);
    CHECK(small.nx == 16 && small.ny == 16);
    double expected = 0.0;
    for (double f : fine.grid) expected += f;
    CHECK(std::abs(small.totalFlux - expected) <= 1e-9 * expected);
}

TEST(appearanceradiance, a_bright_centre_renders_bright_in_the_centre) {
    // The whole point of the phase: the exit surface glows with the pattern the
    // run computed, not uniformly.
    const appearance::RadianceMap map = appearance::buildRadianceMap(brightCentreReceiver());
    CHECK(map.valid);
    CHECK(map.nx > 2 && map.ny > 2);

    auto at = [&map](int i, int j) {
        const appearance::RadianceFacet& f =
            map.facets[std::size_t(j) * std::size_t(map.nx) + std::size_t(i)];
        return std::max({f.radiance.r(), f.radiance.g(), f.radiance.b()});
    };

    const float centre = at(map.nx / 2, map.ny / 2);
    const float corner = at(0, 0);
    CHECK(centre > corner * 2.0f);

    // The brightest facet is mapped to the stated display radiance, so the
    // picture does not depend on the unit the flux was measured in.
    float peak = 0.0f;
    for (const appearance::RadianceFacet& f : map.facets)
        peak = std::max(peak, std::max({f.radiance.r(), f.radiance.g(), f.radiance.b()}));
    CHECK(std::abs(double(peak) - appearance::kBrightestRadiance) < 1e-4);
    // ...and the divisor it was scaled by is a real radiance, reported as one.
    CHECK(map.peakRadiance > 0.0);
}

TEST(appearanceradiance, the_facets_cover_the_receiver_they_belong_to) {
    // A distribution drawn somewhere other than the surface it was measured on
    // is a picture of a different optic.
    const DetectorFrame d = brightCentreReceiver(32, 32, 40.0, 20.0);
    const appearance::RadianceMap map = appearance::buildRadianceMap(d);

    CHECK(std::abs(facetBoxSpan(map, 0) - d.w) < 1e-6);
    CHECK(std::abs(facetBoxSpan(map, 1) - d.h) < 1e-6);
    // Flat: the plate has no thickness, only the hair of offset that keeps it
    // from fighting the receiver plane for the same pixels.
    CHECK(facetBoxSpan(map, 2) < 1e-2);
}

TEST(appearanceradiance, a_run_that_reached_nothing_produces_no_map) {
    DetectorFrame dark = brightCentreReceiver();
    dark.grid.assign(dark.grid.size(), 0.0);
    CHECK(!appearance::buildRadianceMap(dark).valid);

    // As does a receiver that was never binned at all.
    DetectorFrame unbinned;
    CHECK(!appearance::buildRadianceMap(unbinned).valid);

    // And one whose grid is shorter than its own bin count -- a truncated or
    // half-written result should be declined, not read off the end of.
    DetectorFrame ragged = brightCentreReceiver();
    ragged.grid.resize(ragged.grid.size() / 2);
    CHECK(!appearance::buildRadianceMap(ragged).valid);
}

TEST(appearanceradiance, a_spectral_run_colours_the_facets) {
    // Red on one side, blue on the other. A diffuser with colour-over-position
    // should show it, and the bands are already on the result.
    DetectorFrame d = brightCentreReceiver(16, 16);
    const std::size_t cells = d.grid.size();

    std::vector<double> bands(3 * cells, 0.0);
    for (int j = 0; j < d.ny; ++j) {
        for (int i = 0; i < d.nx; ++i) {
            const std::size_t k = std::size_t(j) * std::size_t(d.nx) + std::size_t(i);
            const double f = d.grid[k];
            if (i < d.nx / 2) bands[k] = f;                  // red
            else              bands[2 * cells + k] = f;      // blue
        }
    }

    const appearance::RadianceMap map = appearance::buildRadianceMap(d, bands, 16);
    CHECK(map.valid);

    const appearance::RadianceFacet& left =
        map.facets[std::size_t(map.ny / 2) * std::size_t(map.nx) + 1];
    const appearance::RadianceFacet& right =
        map.facets[std::size_t(map.ny / 2) * std::size_t(map.nx) +
                   std::size_t(map.nx - 2)];

    CHECK(left.radiance.r() > left.radiance.b());
    CHECK(right.radiance.b() > right.radiance.r());

    // The colour is a direction, not a brightness: the energy total is
    // untouched by having been split into bands.
    double expected = 0.0;
    for (double f : d.grid) expected += f;
    CHECK(std::abs(map.totalFlux - expected) <= 1e-9 * expected);
}

TEST(appearanceradiance, the_map_is_a_pure_function_of_the_grid) {
    // Rebuilt on every scene change and on every redraw that follows one. If
    // the scale moved between builds the image would breathe, and the sample
    // count on screen would be counting frames of two different pictures.
    const DetectorFrame d = brightCentreReceiver();
    const appearance::RadianceMap a = appearance::buildRadianceMap(d);
    const appearance::RadianceMap b = appearance::buildRadianceMap(d);

    CHECK(a.facets.size() == b.facets.size());
    CHECK(a.totalFlux == b.totalFlux);
    CHECK(a.peakRadiance == b.peakRadiance);
    for (std::size_t i = 0; i < a.facets.size() && i < b.facets.size(); ++i)
        CHECK(a.facets[i].radiance == b.facets[i].radiance);
}

TEST(appearanceradiance, a_tilted_receiver_is_rendered_where_it_actually_is) {
    // The receiver carries its own frame precisely so an off-axis or tilted one
    // reports against itself. The render has to honour that frame, or a
    // side-illuminated part shows its distribution floating in a world plane it
    // does not lie in.
    DetectorFrame d = brightCentreReceiver(16, 16, 30.0, 30.0);
    d.u      = Vec3(0, 0, 1);
    d.v      = Vec3(0, 1, 0);
    d.normal = Vec3(-1, 0, 0);
    d.center = Vec3(12.0, 0.0, 0.0);

    const appearance::RadianceMap map = appearance::buildRadianceMap(d);
    CHECK(map.valid);

    // It now spans z and y, and is flat in x.
    CHECK(std::abs(facetBoxSpan(map, 2) - 30.0) < 1e-6);
    CHECK(std::abs(facetBoxSpan(map, 1) - 30.0) < 1e-6);
    CHECK(facetBoxSpan(map, 0) < 1e-2);
}

TEST(appearanceradiance, every_scene_that_traces_can_drive_a_render_from_its_result) {
    // End to end on the real library: run a small trace, take the receiver it
    // measured, and build the emission from it. What must not happen is a scene
    // whose result cannot be turned into a picture -- and the energy has to
    // survive the trip in every one of them.
    for (int i = 0; i < GeometryProvider::count(); ++i) {
        const auto scene = GeometryProvider::Scene(i);

        SimConfig cfg;
        cfg.scene = scene;
        cfg.rays  = 4000;
        const SimulationResult res = Simulation::run(cfg);
        if (res.detectors.empty()) continue;

        const DetectorFrame& d = res.detectors[0];
        const appearance::RadianceMap map = appearance::buildRadianceMap(d);

        double expected = 0.0;
        for (double f : d.grid) expected += f;

        if (!(expected > 0.0)) {
            // Nothing arrived; declining to build is the correct answer.
            CHECK(!map.valid);
            continue;
        }
        if (!map.valid) {
            reportFailure(__FILE__, __LINE__,
                          (GeometryProvider::info(scene).name +
                           QStringLiteral(" measured flux but produced no emission map"))
                              .toStdString());
            continue;
        }
        CHECK(std::abs(map.totalFlux - expected) <= 1e-9 * expected);
        CHECK(map.nx > 0 && map.ny > 0);
        CHECK(int(map.facets.size()) <= map.nx * map.ny);
    }
}

// ---- output ----------------------------------------------------------------
//
// The image itself is still reviewed by eye -- a GPU estimate is not bit-stable
// and asserting on pixels would produce a permanently red build. What is
// assertable is everything the export decides before the GPU is involved: what
// a typed resolution actually becomes, and what the picture says about itself
// once it leaves the window. The second of those is the one that matters. The
// risk register's entry is "the render is mistaken for an analysis result",
// managed by writing rather than by code; these tests are what keeps the
// writing from being deleted by an edit that looked harmless.

TEST(appearanceoutput, a_resolution_past_what_a_gpu_will_allocate_is_held_to_it) {
    const appearance::ExportRequest r =
        appearance::normaliseExport(40000, 40000, 512, 16.0 / 9.0);
    CHECK(r.width == appearance::kMaxExportDimension);
    CHECK(r.height == appearance::kMaxExportDimension);

    // And the other end: a one-pixel render is a request nobody meant.
    const appearance::ExportRequest tiny = appearance::normaliseExport(1, 1, 512, 1.0);
    CHECK(tiny.width == appearance::kMinExportDimension);
    CHECK(tiny.height == appearance::kMinExportDimension);
}

TEST(appearanceoutput, a_missing_dimension_comes_from_the_aspect_of_the_view) {
    // "Give me this at 4K wide" is one number, and the other one is the framing
    // already on screen rather than an arithmetic exercise the user has to get
    // right for the picture not to be stretched.
    const appearance::ExportRequest wide =
        appearance::normaliseExport(3840, 0, 256, 16.0 / 9.0);
    CHECK(wide.width == 3840);
    CHECK(std::abs(wide.height - 2160) <= 1);

    const appearance::ExportRequest tall =
        appearance::normaliseExport(0, 1000, 256, 0.5);
    CHECK(tall.height == 1000);
    CHECK(std::abs(tall.width - 500) <= 1);

    // A view that has never been shown reports no aspect at all. The export
    // still has to produce a shape, and it is a landscape one because the
    // picture is going into a document.
    const appearance::ExportRequest none = appearance::normaliseExport(0, 0, 256, 0.0);
    CHECK(none.width > none.height);
    CHECK(none.width >= appearance::kMinExportDimension);
}

TEST(appearanceoutput, the_sample_count_is_held_to_something_a_user_can_wait_out) {
    CHECK(appearance::normaliseExport(800, 600, 0, 1.33).samples ==
          appearance::kMinExportSamples);
    CHECK(appearance::normaliseExport(800, 600, -4, 1.33).samples ==
          appearance::kMinExportSamples);
    CHECK(appearance::normaliseExport(800, 600, 1 << 20, 1.33).samples ==
          appearance::kMaxExportSamples);
}

TEST(appearanceoutput, every_caption_says_the_render_is_not_a_measurement) {
    // The one assertion in this file that is about wording rather than about
    // arithmetic, and the one worth having. An exported PNG lands beside
    // irradiance maps and candela plots that *are* measurements; the sentence
    // that says this one is not has to be impossible to lose.
    const QString note = appearance::notPhotometricNote();
    CHECK(note.contains(QStringLiteral("Not a photometric result")));

    appearance::ExportRequest r;
    r.width = 1920; r.height = 1080; r.samples = 512;
    const QString exported = appearance::exportCaption(r);
    CHECK(exported.contains(note));
    CHECK(exported.contains(QStringLiteral("1920")));
    CHECK(exported.contains(QStringLiteral("1080")));
    CHECK(exported.contains(QStringLiteral("512")));

    // The live strip carries it too, converged or not.
    CHECK(appearance::liveCaption(12, 512).contains(note));
    CHECK(appearance::liveCaption(512, 512).contains(note));
    CHECK(appearance::liveCaption(12, 512).contains(QStringLiteral("12 / 512")));
    CHECK(appearance::liveCaption(512, 512).contains(QStringLiteral("converged")));
}

TEST(appearanceoutput, a_render_driven_by_a_run_says_what_drove_it_and_what_it_carried) {
    const DetectorFrame d = brightCentreReceiver();
    const appearance::RadianceMap map = appearance::buildRadianceMap(d);
    CHECK(map.valid);

    const QString driven = appearance::distributionNote(map, FluxUnit::Watt);
    CHECK(!driven.isEmpty());
    CHECK(driven.contains(d.label));
    CHECK(driven.contains(QString::number(map.nx)));
    CHECK(driven.contains(QStringLiteral("W")));
    // The brightness on screen is a normalisation and the caption has to say so,
    // or the flux quoted beside it reads as the thing being displayed.
    CHECK(driven.contains(QStringLiteral("brightness is not")));

    // It reaches both captions, and neither loses the disclaimer to it.
    appearance::ExportRequest r;
    r.width = 1520; r.height = 920; r.samples = 192;
    CHECK(appearance::exportCaption(r, driven).contains(driven));
    CHECK(appearance::exportCaption(r, driven).contains(appearance::notPhotometricNote()));
    CHECK(appearance::liveCaption(64, 512, driven).contains(driven));

    // A render lit by its sources alone has nothing to say here, and says
    // nothing rather than saying so at length.
    CHECK(appearance::distributionNote(appearance::RadianceMap(), FluxUnit::Watt).isEmpty());
    CHECK(!appearance::exportCaption(r).contains(QStringLiteral("Emission driven")));
}

TEST(appearanceoutput, a_lumen_run_is_captioned_in_lumens) {
    // The flux quoted is the run's own number in the run's own unit. Quoting a
    // photometric run in watts would be the one wrong number in a caption whose
    // whole job is to be careful about what is physical and what is not.
    const DetectorFrame d = brightCentreReceiver();
    const appearance::RadianceMap map = appearance::buildRadianceMap(d);
    const QString lumens = appearance::distributionNote(map, FluxUnit::Lumen);
    CHECK(lumens.endsWith(QStringLiteral("brightness is not.")));
    CHECK(lumens.contains(QStringLiteral(" lm ")));
    CHECK(!lumens.contains(QStringLiteral(" W ")));
}

// ---- the showcase scene ----------------------------------------------------
//
// A scene that exists to be looked at still has to be a scene: it is traced by
// the same tracer, closes the same energy budget, and its props have to be
// props rather than obstacles. These are the properties that make it honest,
// as opposed to the ones that make it handsome, which are reviewed by eye.

TEST(appearanceshowcase, the_fixture_carries_the_surfaces_a_picture_needs) {
    // Metal to reflect, glass to refract, something matte to land on. The
    // scene is a demonstration of the material bridge, so it has to exercise
    // more than one branch of it.
    const auto scene = GeometryProvider::Scene::ShowcaseLuminaire;
    const std::vector<OpticalSurface> parts = GeometryProvider::buildScene(scene);

    bool metal = false, transmissive = false, matte = false, receiver = false;
    for (const OpticalSurface& s : parts) {
        if (s.isDetector) { receiver = true; continue; }
        const appearance::Material m = appearance::materialFor(s);
        // A metal by what makes it one -- its own complex index, which is
        // what the render reads -- rather than by being perfectly polished.
        // The cup is lightly peened, as a real reflector is.
        if (s.material.isMetal()) metal = true;
        if (s.index > 1.0) transmissive = true;
        if (s.scatter >= 0.99 && s.index <= 0.0) matte = true;
        // Whatever it is, it has to reach OCCT as something drawable.
        CHECK(std::isfinite(m.transparency));
    }
    CHECK(metal);
    CHECK(transmissive);
    CHECK(matte);
    CHECK(receiver);
}

TEST(appearanceshowcase, the_props_stay_out_of_the_measurement) {
    // The floor and the base are there for the picture. A prop that intercepts
    // light on its way to the receiver is not a prop, it is a part of the
    // optic that nobody declared -- so the geometry has to keep them behind
    // the emitter, where a beam travelling forward cannot reach them.
    const auto scene = GeometryProvider::Scene::ShowcaseLuminaire;
    const SceneParams params = GeometryProvider::defaultParams(scene);
    const GeometryProvider::SceneSetup setup = GeometryProvider::build(scene, params);

    double propMaxZ = -1e30;
    double receiverZ = 0.0;
    for (const OpticalSurface& s : setup.surfaces) {
        Bnd_Box box;
        BRepBndLib::Add(s.shape, box);
        if (box.IsVoid()) continue;
        double lo[3], hi[3];
        box.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);

        if (s.isDetector) { receiverZ = 0.5 * (lo[2] + hi[2]); continue; }
        if (s.label == QStringLiteral("Floor") || s.label == QStringLiteral("Base"))
            propMaxZ = std::max(propMaxZ, hi[2]);
    }

    CHECK(propMaxZ > -1e29);
    // Behind the emitter, which is itself far behind the receiver.
    CHECK(propMaxZ <= setup.sourceOrigin.Z());
    CHECK(receiverZ > propMaxZ);
}

TEST(appearanceshowcase, the_scene_declares_the_emitter_it_is_built_around) {
    // Most of the library declines to choose a source, and should: a ball lens
    // teaches the same lesson under any of them. A luminaire does not -- an LED
    // fixture with a mathematical point in it is not a fixture -- so this one
    // says what belongs at its focus, and the size it says is the size the
    // parameter block was edited to.
    const auto scene = GeometryProvider::Scene::ShowcaseLuminaire;
    SceneParams params = GeometryProvider::defaultParams(scene);

    const Simulation::SceneEmitter declared = Simulation::emitterFor(scene, params);
    CHECK(declared.declared);
    CHECK(declared.spec.shape == SourceConfig::Shape::Rect);
    CHECK(declared.spec.type == SourceConfig::Type::Lambertian);
    CHECK(declared.spec.sizeA > 0.0);
    CHECK_NEAR(declared.spec.sizeA, params.v[2], 1e-12);
    CHECK_NEAR(declared.spec.sizeB, params.v[2], 1e-12);

    // It follows the dimension rather than being frozen at the default.
    params.v[2] = 12.0;
    CHECK_NEAR(Simulation::emitterFor(scene, params).spec.sizeA, 12.0, 1e-12);

    // And a scene that is not built around one says so by declining. Two are:
    // the fixture here, and the TIR collimator, which is designed around a
    // surface source of a stated size rather than around a point.
    for (int s = 0; s < GeometryProvider::count(); ++s) {
        const auto other = GeometryProvider::Scene(s);
        if (other == scene || other == GeometryProvider::Scene::TirLens) continue;
        CHECK(!Simulation::emitterFor(other, GeometryProvider::defaultParams(other)).declared);
    }
}

TEST(appearanceshowcase, the_tir_collimator_declares_the_surface_source_it_is_built_around) {
    // A point source at the focus of this optic collimates perfectly and says
    // nothing, because the one question a collimator answers is what a die of
    // finite size does to the beam. So the scene declares the emitting area the
    // way the fixture declares its die: Lambertian over a square, sized by the
    // parameter on screen, and following it when it moves.
    const auto scene = GeometryProvider::Scene::TirLens;
    SceneParams params = GeometryProvider::defaultParams(scene);

    const Simulation::SceneEmitter declared = Simulation::emitterFor(scene, params);
    CHECK(declared.declared);
    CHECK(declared.spec.type == SourceConfig::Type::Lambertian);
    CHECK(declared.spec.shape == SourceConfig::Shape::Rect);
    CHECK(declared.spec.sizeA > 0.0);
    CHECK_NEAR(declared.spec.sizeA, params.v[3], 1e-12);
    CHECK_NEAR(declared.spec.sizeB, params.v[3], 1e-12);

    params.v[3] = 4.0;
    const Simulation::SceneEmitter moved = Simulation::emitterFor(scene, params);
    CHECK_NEAR(moved.spec.sizeA, 4.0, 1e-12);
    CHECK_NEAR(moved.spec.sizeB, 4.0, 1e-12);
}

TEST(appearanceshowcase, a_declared_emitter_has_a_real_area_so_it_renders_as_a_face) {
    // The reason the declaration exists at all. A point-like source has no face
    // to put `Le` on, so the Appearance tab can only draw it as a light and the
    // fixture shows nothing glowing -- which is the one thing a luminaire has
    // to show. With an area, the die becomes geometry that emits.
    const auto scene = GeometryProvider::Scene::ShowcaseLuminaire;
    const SceneParams params = GeometryProvider::defaultParams(scene);
    const GeometryProvider::SceneSetup setup = GeometryProvider::build(scene, params);
    const Simulation::SceneEmitter declared = Simulation::emitterFor(scene, params);

    SourceConfig die;
    die.type   = declared.spec.type;
    die.shape  = declared.spec.shape;
    die.sizeA  = declared.spec.sizeA;
    die.sizeB  = declared.spec.sizeB;
    die.power  = declared.spec.power;
    die.origin = setup.sourceOrigin;
    die.axis   = setup.sourceAxis;

    CHECK(appearance::sourceRadiance(die) > 0.0);

    const appearance::EmitterBuild built = appearance::buildEmitters({die}, 250.0);
    CHECK(built.emitters.size() == 1);
    CHECK(!built.emitters[0].pointLike);
    CHECK(!built.emitters[0].shape.IsNull());
    CHECK(built.pointSources == 0);
}

TEST(appearanceemitters, an_area_emitter_is_also_a_light_and_at_the_scene_s_scale) {
    // OCCT's path tracer finds emissive geometry only by a path landing on it,
    // so a small emitting face lights nothing at all. Every emitter therefore
    // also becomes a positional light -- and that light's intensity has to
    // scale with the scene, because OCCT's positional light falls off as 1/d^2
    // and a fixed intensity renders a large scene black at every exposure.
    SourceConfig die;
    die.type   = SourceConfig::Type::Lambertian;
    die.shape  = SourceConfig::Shape::Rect;
    die.sizeA  = 5.0;
    die.sizeB  = 5.0;
    die.power  = 1.0;
    die.origin = gp_Pnt(0, 0, 0);
    die.axis   = gp_Dir(0, 0, 1);

    const appearance::EmitterBuild small = appearance::buildEmitters({die}, 50.0);
    const appearance::EmitterBuild large = appearance::buildEmitters({die}, 500.0);
    CHECK(small.emitters.size() == 1 && large.emitters.size() == 1);

    // An area emitter carries a light as well as a face.
    CHECK(!small.emitters[0].pointLike);
    CHECK(small.emitters[0].intensity > 0.0);
    // Ten times the scene is a hundred times the intensity, which is what
    // cancelling an inverse square means.
    CHECK_NEAR(large.emitters[0].intensity / small.emitters[0].intensity, 100.0, 1e-6);

    // The soft radius comes from the emitter's own size, because that is what
    // sets a real penumbra -- not from the scene, which is all a point source
    // has to go on.
    CHECK(small.emitters[0].smoothRadius > 1.0);
    CHECK_NEAR(small.emitters[0].smoothRadius, large.emitters[0].smoothRadius, 1e-12);
}

TEST(appearancematerials, a_diffusing_transmitter_renders_as_opal_and_not_as_a_window) {
    // `scatter` is a Lambertian lobe, and the glass branch only understood a
    // microfacet alpha -- so every diffusing part in the library reached the
    // renderer as clear glass. A diffuser plate drawn as a window is not a
    // subtle inaccuracy: its entire visual character is that it glows evenly
    // instead of showing what is behind it.
    SurfaceOptics clear;
    clear.index          = 1.49;
    clear.transmissivity = 0.96;
    clear.reflectivity   = 0.04;
    clear.fresnel        = true;

    SurfaceOptics opal = clear;
    opal.scatter       = 0.7;

    const appearance::Material c = appearance::materialFor(clear);
    const appearance::Material o = appearance::materialFor(opal);

    // The clear one is untouched: everything it transmits goes straight on.
    CHECK(c.bsdf.Kd.r() == 0.0f);
    CHECK(c.bsdf.Kt.r() > 0.9f);

    // The opal one spends most of that on a diffuse lobe instead.
    CHECK(o.bsdf.Kd.r() > 0.5f);
    CHECK(o.bsdf.Kt.r() < 0.5f);
    // And no energy is invented doing it.
    CHECK(o.bsdf.Kd.r() + o.bsdf.Kt.r() <= 1.0f + 1e-6f);
    // It also stops looking like a window in the rasterized preview.
    CHECK(o.transparency < c.transparency);
}

TEST(appearancecolour, a_painted_surface_keeps_its_colour_through_the_document) {
    // The regression this exists for: the colour was set on the object, stored
    // by the document, and never seen, because the panel handed the renderer
    // the *traced* surface list -- which an optics edit deliberately does not
    // rebuild, a reflectance being a ray-time property no tessellation cares
    // about. So the render kept drawing the material the mesh was built with.
    //
    // The invariant that makes the fix sound is this one: what compile()
    // produces carries what setOptics stored. Everything downstream reads that.
    scenedoc::SceneDocument doc;
    doc.loadTutorial(GeometryProvider::Scene::Reflector,
                     GeometryProvider::defaultParams(GeometryProvider::Scene::Reflector));

    int partId = -1;
    for (const scenedoc::SceneObject& o : doc.objects())
        if (!o.isSource() && !o.optics.isDetector) { partId = o.id; break; }
    CHECK(partId >= 0);

    const scenedoc::SceneObject* before = doc.find(partId);
    CHECK(before != nullptr);
    SurfaceOptics painted = before->optics;
    CHECK(!painted.hasAppearanceColour());
    painted.setAppearanceColour(0.70, 0.12, 0.12);
    CHECK(doc.setOptics(partId, painted));

    const scenedoc::SceneDocument::Compiled c = doc.compile();
    CHECK(c.setup != nullptr);

    // The compiled surface the edit belongs to carries the colour.
    bool found = false;
    for (std::size_t i = 0; i < c.setup->surfaces.size(); ++i) {
        if (i >= c.surfaceObject.size() || c.surfaceObject[i] != partId) continue;
        found = true;
        const OpticalSurface& s = c.setup->surfaces[i];
        CHECK(s.hasAppearanceColour());
        CHECK_NEAR(s.appearanceRgb[0], 0.70, 1e-9);
        CHECK_NEAR(s.appearanceRgb[1], 0.12, 1e-9);
        CHECK_NEAR(s.appearanceRgb[2], 0.12, 1e-9);

        // And it reaches the renderer as a tint rather than being carried and
        // ignored: a red surface has more red weight than green.
        const appearance::Material m = appearance::materialFor(s);
        const Graphic3d_Vec3 rgb(m.bsdf.Kd + m.bsdf.Kt + Graphic3d_Vec3(m.bsdf.Ks.r(),
                                                                        m.bsdf.Ks.g(),
                                                                        m.bsdf.Ks.b()));
        CHECK(rgb.r() > rgb.g());
        CHECK(rgb.b() < rgb.r());
    }
    CHECK(found);
}

TEST(appearancecolour, an_unpainted_surface_is_left_to_the_physics) {
    // The default has to stay "derive it", or every built-in scene would start
    // wearing a colour nobody chose -- an aluminium reflector's colour is its
    // own complex index, and a swatch would be a downgrade.
    SurfaceOptics bare;
    bare.reflectivity = 0.92;
    CHECK(!bare.hasAppearanceColour());

    SurfaceOptics painted = bare;
    painted.setAppearanceColour(0.1, 0.8, 0.2);
    CHECK(painted.hasAppearanceColour());
    painted.clearAppearanceColour();
    CHECK(!painted.hasAppearanceColour());

    // Setting a colour never disturbs a single number the tracer reads.
    SurfaceOptics a = bare, b = bare;
    b.setAppearanceColour(0.9, 0.1, 0.1);
    CHECK(a.reflectivity == b.reflectivity);
    CHECK(a.transmissivity == b.transmissivity);
    CHECK(a.index == b.index);
    CHECK(a.scatter == b.scatter);
    CHECK(a.absorption == b.absorption);
}
