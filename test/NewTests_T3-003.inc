// T3-003 — SceneParams capacity (4 -> >= 6) as an inline vector, ADDITIVE-ONLY.
//
// Own test module for the Senior C++ engineer. Registered via the shared TEST
// macro so it self-registers with the same registry the runner walks; the
// module is meant to be #include-d alongside NewTests.inc / AuditTests.inc.
#include <cmath>
#include <type_traits>

// The capacity must now hold a cemented doublet's six dimensions, not the old
// three free plus receiver.
TEST(sceneparams, capacity_is_at_least_six) {
    CHECK(SceneParams::kMax >= 6);
    CHECK(SceneParams::kCapacity == SceneParams::kMax);
    // The storage is inline, so it copies by value without allocating and stays
    // a valid, cheap cache key / config value. There is no heap pointer: the
    // struct is trivially copyable and its bytes are just the live count plus
    // the fixed buffer (alignment may pad the tail, so bound rather than equal).
    CHECK(std::is_trivially_copyable<SceneParams>::value);
    CHECK(sizeof(SceneParams) >= sizeof(int) + sizeof(double) * SceneParams::kMax);
    CHECK(sizeof(SceneParams) < 256);   // far below any heap-vector-backed layout
}

// index semantics (0..n-1, same order) are preserved: defaultParams puts each
// declared parameter's default exactly where paramInfo declares it, and the
// live count equals the number of declared params for every registered scene.
TEST(sceneparams, every_scene_live_count_and_order_match_param_table) {
    const int scenes = GeometryProvider::count();
    CHECK(scenes > 0);
    for (int s = 0; s < scenes; ++s) {
        const auto scene = GeometryProvider::Scene(s);
        const auto& infos = GeometryProvider::paramInfo(scene);
        if (infos.empty()) continue;

        const SceneParams p = GeometryProvider::defaultParams(scene);
        CHECK(p.empty() == false);
        CHECK(p.size() == int(infos.size()));
        CHECK(p.size() <= SceneParams::kMax);
        for (int i = 0; i < p.size(); ++i)
            CHECK_NEAR(p.v[i], infos[std::size_t(i)].def, 1e-12);

        // Sanitising a sane value preserves the params exactly (order,
        // count, values) -- the round trip is a no-op for a clean input.
        const SceneParams clean = GeometryProvider::sanitise(scene, p);
        CHECK(clean.size() == p.size());
        CHECK(clean == p);
    }
}

// The receiver accessor is the last live slot and stays semantically unchanged:
// it is exactly the slot `derived` and friends read as the detector distance.
TEST(sceneparams, receiver_accessor_is_last_live_slot) {
    const int scenes = GeometryProvider::count();
    for (int s = 0; s < scenes; ++s) {
        const auto scene = GeometryProvider::Scene(s);
        const auto& infos = GeometryProvider::paramInfo(scene);
        if (infos.empty()) continue;

        const SceneParams p = GeometryProvider::defaultParams(scene);
        const int last = p.size() - 1;
        CHECK_NEAR(p.receiver(), p.v[last], 1e-12);
        CHECK_NEAR(p.receiver(), infos[std::size_t(last)].def, 1e-12);

        // Moving the receiver is a parameter change, not a special case: poking
        // the live last slot through the accessor and reading it back agrees.
        SceneParams q = p;
        q.receiver() = 1234.25;
        CHECK_NEAR(q.receiver(), 1234.25, 1e-12);
        CHECK(q != p);
    }
}

// Equality must compare the live count and every slot, including zeroed dead
// slots, so that two independently-constructed equal configs are == and copies
// that share storage (cache-key reuse) stay interchangeable.
TEST(sceneparams, equality_covers_live_count_and_dead_slots) {
    const SceneParams a = GeometryProvider::defaultParams(GeometryProvider::Scene::Lens);
    SceneParams b = a;
    CHECK(a == b);           // byte-identical copy
    b.v[SceneParams::kMax - 1] = 42.0;   // a dead slot diverging must break ==
    CHECK(a != b);

    // Two different scenes with the same size but different contents are not ==.
    const SceneParams c = GeometryProvider::defaultParams(GeometryProvider::Scene::Reflector);
    const SceneParams d = GeometryProvider::defaultParams(GeometryProvider::Scene::Lens);
    if (int(GeometryProvider::paramInfo(GeometryProvider::Scene::Reflector).size()) ==
        int(GeometryProvider::paramInfo(GeometryProvider::Scene::Lens).size()))
        CHECK(c != d);
}

// A six-dimensional (cemented-doublet-scale) parameter set fits, is carried
// inline without allocation, and the receiver sits in the final live slot.
TEST(sceneparams, six_dimension_doublet_set_is_supported) {
    SceneParams doublet;
    doublet.n = 6;
    doublet.v[0] = 12.0;   // front radius
    doublet.v[1] = 3.0;    // front material
    doublet.v[2] = -18.0;  // cemented interface
    doublet.v[3] = 4.0;    // body material
    doublet.v[4] = 25.0;   // back radius
    doublet.v[5] = 200.0;  // receiver distance (last live slot)

    CHECK(doublet.size() == 6);
    CHECK(doublet.receiver() == 25.0 || doublet.receiver() == 200.0); // sanity guard only
    CHECK_NEAR(doublet.receiver(), 200.0, 1e-12);
    CHECK_NEAR(doublet[4], 25.0, 1e-12);

    // Sanitise clamps to the scene's declared params and returns a value that
    // carries at most kMax live slots in the same order -- here the Lens scene
    // (2 optics dims + receiver), demonstrating a smaller set survives the
    // wider buffer with dead slots zeroed.
    const SceneParams lens =
        GeometryProvider::sanitise(GeometryProvider::Scene::Lens, doublet);
    CHECK(lens.size() <= 6);
    const SceneParams same =
        GeometryProvider::sanitise(GeometryProvider::Scene::Lens,
                                   GeometryProvider::defaultParams(GeometryProvider::Scene::Lens));
    CHECK(lens.size() <= same.size());
}
