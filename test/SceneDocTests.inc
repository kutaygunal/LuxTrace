// Tests for the assembled-scene document.
//
// The document is what turns "pick one of twenty-six optics" into "put a lens
// here and a source there", and it reaches the engine by compiling into the
// same GeometryProvider::SceneSetup a CAD import has always produced. So what
// is worth pinning down is the compile: that every object becomes a body the
// tracer can see, that the emitters come out placed where the tree says, that
// the surface-to-object map a pick depends on is right, and that the link back
// to a tutorial breaks exactly when it should and not before.

TEST(scenedoc, every_creatable_type_builds_geometry) {
    // A type in the library that compiles to nothing is a row a user can drag
    // in and then find has done nothing at all.
    for (scenedoc::ObjectType type : scenedoc::creatableTypes()) {
        if (type == scenedoc::ObjectType::Source) continue;   // an emitter is not a body

        scenedoc::SceneDocument doc;
        doc.add(type, gp_Pnt(0, 0, 0));
        const auto compiled = doc.compile();

        const QString name = scenedoc::typeInfo(type).name;
        CHECK(compiled.setup != nullptr);
        if (compiled.setup->surfaces.size() != 1) {
            reportFailure(__FILE__, __LINE__,
                          (name + QStringLiteral(" built no geometry")).toStdString());
            continue;
        }
        CHECK(!compiled.setup->surfaces[0].shape.IsNull());
        // And it has to survive tessellation, or it is a body the tracer cannot
        // actually intersect.
        const MeshList mesh = MeshBuilder::build(compiled.setup->surfaces);
        if (mesh.empty() || mesh[0].tris.empty())
            reportFailure(__FILE__, __LINE__,
                          (name + QStringLiteral(" tessellated to nothing")).toStdString());
    }
}

TEST(scenedoc, an_object_is_built_where_it_was_placed) {
    scenedoc::SceneDocument doc;
    const int id = doc.add(scenedoc::ObjectType::Sphere, gp_Pnt(40.0, -15.0, 7.5));
    scenedoc::SceneObject* o = doc.find(id);
    CHECK(o != nullptr);
    o->p[0] = 5.0;                       // radius

    const auto compiled = doc.compile();
    CHECK(compiled.setup->surfaces.size() == 1);

    Bnd_Box box;
    BRepBndLib::Add(compiled.setup->surfaces[0].shape, box);
    CHECK(!box.IsVoid());
    double xm, ym, zm, xM, yM, zM;
    box.Get(xm, ym, zm, xM, yM, zM);
    CHECK_NEAR(0.5 * (xm + xM), 40.0, 1e-6);
    CHECK_NEAR(0.5 * (ym + yM), -15.0, 1e-6);
    CHECK_NEAR(0.5 * (zm + zM), 7.5, 1e-6);
}

TEST(scenedoc, a_group_moves_everything_under_it) {
    scenedoc::SceneDocument doc;
    const int group = doc.add(scenedoc::ObjectType::Group, gp_Pnt(100.0, 0.0, 0.0));
    const int ball  = doc.add(scenedoc::ObjectType::Sphere, gp_Pnt(0.0, 0.0, 20.0), group);
    doc.find(ball)->p[0] = 4.0;

    const gp_Trsf world = doc.worldPlacement(ball);
    const gp_Pnt  where = gp_Pnt(0, 0, 0).Transformed(world);
    // The child's own offset, plus its group's.
    CHECK_NEAR(where.X(), 100.0, 1e-9);
    CHECK_NEAR(where.Z(), 20.0, 1e-9);

    CHECK(doc.isAncestorOf(group, ball));
    CHECK(!doc.isAncestorOf(ball, group));
    // A group cannot be dropped onto its own child: that would cut both out of
    // the tree.
    CHECK(!doc.reparent(group, ball));
}

TEST(scenedoc, the_first_source_is_the_primary_and_the_rest_are_extras) {
    scenedoc::SceneDocument doc;
    const int a = doc.add(scenedoc::ObjectType::Source, gp_Pnt(0, 0, -50.0));
    const int b = doc.add(scenedoc::ObjectType::Source, gp_Pnt(10.0, 0, -50.0));
    doc.find(a)->source.power = 3.0;
    doc.find(b)->source.power = 1.0;

    const auto compiled = doc.compile();
    CHECK(compiled.havePrimary);
    CHECK_NEAR(compiled.primary.power, 3.0, 1e-12);
    CHECK(compiled.extraSources.size() == 1);
    CHECK_NEAR(compiled.extraSources[0].power, 1.0, 1e-12);

    // Both are written absolute: an offset relative to "the scene emitter"
    // means nothing once the emitters are the objects.
    CHECK(compiled.extraSources[0].absolute);
    CHECK(!compiled.extraSources[0].useSceneAxis);
    CHECK_NEAR(compiled.extraSources[0].offset.X(), 10.0, 1e-9);
    CHECK_NEAR(compiled.setup->sourceOrigin.Z(), -50.0, 1e-9);
}

TEST(scenedoc, a_rotated_source_emits_along_its_own_axis) {
    scenedoc::SceneDocument doc;
    const int id = doc.add(scenedoc::ObjectType::Source, gp_Pnt(0, 0, 0));
    // Ninety degrees about Y takes local +Z to world +X.
    double rot[3] = {0.0, 90.0, 0.0};
    doc.setPlacement(id, gp_Pnt(0, 0, 0), rot);

    const auto compiled = doc.compile();
    CHECK(compiled.havePrimary);
    CHECK_NEAR(compiled.setup->sourceAxis.X(), 1.0, 1e-9);
    CHECK_NEAR(compiled.setup->sourceAxis.Z(), 0.0, 1e-9);

    // And the round trip through setLocalAxis lands back on the same direction.
    scenedoc::SceneObject probe;
    probe.setLocalAxis(gp_Dir(0.0, 1.0, 0.0));
    const gp_Dir back = probe.localAxis();
    CHECK_NEAR(back.X(), 0.0, 1e-9);
    CHECK_NEAR(back.Y(), 1.0, 1e-9);
    CHECK_NEAR(back.Z(), 0.0, 1e-9);
}

TEST(scenedoc, a_scene_with_no_source_says_so_rather_than_tracing_nothing) {
    scenedoc::SceneDocument doc;
    doc.add(scenedoc::ObjectType::BallLens, gp_Pnt(0, 0, 0));
    const auto compiled = doc.compile();
    CHECK(!compiled.havePrimary);
    CHECK(!compiled.warnings.isEmpty());
}

TEST(scenedoc, the_surface_map_names_the_object_each_body_came_from) {
    // This is what makes clicking a solid in the 3D view select the right row:
    // a pick indexes the compiled surface list and has to come back as an id.
    scenedoc::SceneDocument doc;
    const int lens = doc.add(scenedoc::ObjectType::BiconvexLens, gp_Pnt(0, 0, 0));
    doc.add(scenedoc::ObjectType::Source, gp_Pnt(0, 0, -80.0));   // contributes no surface
    const int det  = doc.add(scenedoc::ObjectType::Detector, gp_Pnt(0, 0, 120.0));

    const auto compiled = doc.compile();
    CHECK(compiled.surfaceObject.size() == compiled.setup->surfaces.size());
    CHECK(compiled.surfaceObject.size() == 2);
    CHECK(compiled.surfaceObject[0] == lens);
    CHECK(compiled.surfaceObject[1] == det);
    CHECK(compiled.setup->surfaces[1].isDetector);
}

TEST(scenedoc, a_tutorial_loads_as_one_object_per_part_and_stays_linked) {
    const auto scene = GeometryProvider::Scene::Reflector;
    scenedoc::SceneDocument doc;
    doc.loadTutorial(scene, GeometryProvider::defaultParams(scene));

    CHECK(doc.linkedToTutorial());
    CHECK(doc.sourceCount() >= 1);

    const std::size_t parts = GeometryProvider::buildScene(scene).size();
    std::size_t       seen  = 0;
    for (const scenedoc::SceneObject& o : doc.objects())
        if (o.type == scenedoc::ObjectType::TutorialPart) ++seen;
    CHECK(seen == parts);

    // Every part carries the shape the registry built, so a compile of the
    // document is the same geometry the registry hands over.
    const auto compiled = doc.compile();
    CHECK(compiled.setup->surfaces.size() == parts);
    CHECK(compiled.havePrimary);
}

TEST(scenedoc, editing_optics_keeps_the_link_and_moving_a_part_breaks_it) {
    const auto scene = GeometryProvider::Scene::Reflector;
    scenedoc::SceneDocument doc;
    doc.loadTutorial(scene, GeometryProvider::defaultParams(scene));

    int part = 0;
    for (const scenedoc::SceneObject& o : doc.objects())
        if (o.type == scenedoc::ObjectType::TutorialPart) { part = o.id; break; }
    CHECK(part != 0);

    // Renaming, hiding and re-specifying a surface all leave the geometry
    // exactly as the builder made it, so the parameter block still describes
    // what is on screen and the sweeps still have dimensions to vary.
    CHECK(doc.setName(part, QStringLiteral("Primary")));
    CHECK(doc.setVisible(part, false));
    SurfaceOptics optics = doc.find(part)->optics;
    optics.reflectivity  = 0.55;
    CHECK(doc.setOptics(part, optics));
    CHECK(doc.linkedToTutorial());
    CHECK(doc.find(part)->opticsEdited);

    // Moving one does not.
    double rot[3] = {0.0, 0.0, 0.0};
    CHECK(doc.setPlacement(part, gp_Pnt(0, 0, 12.0), rot));
    CHECK(!doc.linkedToTutorial());
}

TEST(scenedoc, dropping_an_object_into_a_tutorial_detaches_it) {
    const auto scene = GeometryProvider::Scene::Lens;
    scenedoc::SceneDocument doc;
    doc.loadTutorial(scene, GeometryProvider::defaultParams(scene));
    CHECK(doc.linkedToTutorial());

    doc.add(scenedoc::ObjectType::FlatMirror, gp_Pnt(0, 0, 60.0));
    CHECK(!doc.linkedToTutorial());

    // And the mirror is traced alongside the tutorial's own parts.
    const auto compiled = doc.compile();
    CHECK(compiled.setup->surfaces.size() ==
          GeometryProvider::buildScene(scene).size() + 1);
}

TEST(scenedoc, a_dimension_change_keeps_object_ids_and_optical_edits) {
    const auto scene = GeometryProvider::Scene::Reflector;
    scenedoc::SceneDocument doc;
    doc.loadTutorial(scene, GeometryProvider::defaultParams(scene));

    int part = 0;
    for (const scenedoc::SceneObject& o : doc.objects())
        if (o.type == scenedoc::ObjectType::TutorialPart) { part = o.id; break; }

    SurfaceOptics optics = doc.find(part)->optics;
    optics.reflectivity  = 0.42;
    doc.setOptics(part, optics);

    SceneParams moved = GeometryProvider::defaultParams(scene);
    moved.v[0] *= 1.2;
    doc.setTutorialParams(moved);

    // The selection survives a spin-box drag, and so does a reflectivity
    // somebody typed: lengthening the optic is not a reason to throw either away.
    CHECK(doc.linkedToTutorial());
    const scenedoc::SceneObject* after = doc.find(part);
    CHECK(after != nullptr);
    CHECK(after->opticsEdited);
    CHECK_NEAR(after->optics.reflectivity, 0.42, 1e-12);
}

TEST(scenedoc, deleting_a_group_takes_its_children_with_it) {
    scenedoc::SceneDocument doc;
    const int group = doc.add(scenedoc::ObjectType::Group, gp_Pnt(0, 0, 0));
    doc.add(scenedoc::ObjectType::Sphere, gp_Pnt(0, 0, 0), group);
    doc.add(scenedoc::ObjectType::Sphere, gp_Pnt(0, 0, 10.0), group);
    doc.add(scenedoc::ObjectType::Source, gp_Pnt(0, 0, -10.0));
    CHECK(doc.count() == 4);

    CHECK(doc.remove(group) == 3);
    CHECK(doc.count() == 1);
    CHECK(doc.compile().setup->surfaces.empty());
}

TEST(scenedoc, duplicating_a_group_copies_the_whole_subtree) {
    scenedoc::SceneDocument doc;
    const int group = doc.add(scenedoc::ObjectType::Group, gp_Pnt(0, 0, 0));
    doc.add(scenedoc::ObjectType::Sphere, gp_Pnt(0, 0, 0), group);
    doc.add(scenedoc::ObjectType::Sphere, gp_Pnt(0, 0, 10.0), group);

    const int copy = doc.duplicate(group);
    CHECK(copy != 0 && copy != group);
    CHECK(doc.count() == 6);
    CHECK(doc.childrenOf(copy).size() == 2);
    // Two bodies before, four after.
    CHECK(doc.compile().setup->surfaces.size() == 4);
}

TEST(scenedoc, an_assembled_scene_survives_a_json_round_trip) {
    scenedoc::SceneDocument doc;
    const int group = doc.add(scenedoc::ObjectType::Group, gp_Pnt(5.0, 0, 0));
    const int lens  = doc.add(scenedoc::ObjectType::BiconvexLens, gp_Pnt(0, 0, 30.0), group);
    const int src   = doc.add(scenedoc::ObjectType::Source, gp_Pnt(0, 0, -60.0));

    doc.setName(lens, QStringLiteral("Collector"));
    doc.find(lens)->p[0] = 95.0;
    SurfaceOptics optics = doc.find(lens)->optics;
    optics.roughness     = 0.012;
    doc.setOptics(lens, optics);

    SourceSpec spec        = doc.find(src)->source;
    spec.type              = SourceConfig::Type::Lambertian;
    spec.power             = 12.5;
    spec.spectrum.kind     = SpectrumConfig::Kind::D65;
    doc.setSourceSpec(src, spec);
    doc.setVisible(lens, false);

    QStringList warnings;
    const scenedoc::SceneDocument back =
        scenedoc::SceneDocument::fromJson(doc.toJson(), &warnings);

    CHECK(warnings.isEmpty());
    CHECK(back.count() == doc.count());

    const scenedoc::SceneObject* rl = back.find(lens);
    CHECK(rl != nullptr);
    CHECK(rl->name == QStringLiteral("Collector"));
    CHECK(rl->parent == group);
    CHECK(!rl->visible);
    CHECK(rl->opticsEdited);
    CHECK_NEAR(rl->p[0], 95.0, 1e-9);
    CHECK_NEAR(rl->optics.roughness, 0.012, 1e-12);

    const scenedoc::SceneObject* rs = back.find(src);
    CHECK(rs != nullptr);
    CHECK(rs->source.type == SourceConfig::Type::Lambertian);
    CHECK(rs->source.spectrum.kind == SpectrumConfig::Kind::D65);
    CHECK_NEAR(rs->source.power, 12.5, 1e-12);

    // And it still compiles to the same geometry.
    CHECK(back.compile().setup->surfaces.size() == doc.compile().setup->surfaces.size());
}

TEST(scenedoc, a_saved_tutorial_rebuilds_its_parts_on_reload) {
    const auto scene = GeometryProvider::Scene::Cassegrain;
    scenedoc::SceneDocument doc;
    doc.loadTutorial(scene, GeometryProvider::defaultParams(scene));

    int part = 0;
    for (const scenedoc::SceneObject& o : doc.objects())
        if (o.type == scenedoc::ObjectType::TutorialPart) { part = o.id; break; }
    SurfaceOptics optics = doc.find(part)->optics;
    optics.reflectivity  = 0.77;
    doc.setOptics(part, optics);

    QStringList warnings;
    const scenedoc::SceneDocument back =
        scenedoc::SceneDocument::fromJson(doc.toJson(), &warnings);

    CHECK(warnings.isEmpty());
    CHECK(back.linkedToTutorial());
    CHECK(back.tutorialScene() == scene);
    // The B-Rep was never written; the parts come back because the tutorial is
    // rebuilt and matched by index.
    const scenedoc::SceneObject* rp = back.find(part);
    CHECK(rp != nullptr);
    CHECK(!rp->baked.IsNull());
    CHECK_NEAR(rp->optics.reflectivity, 0.77, 1e-12);
    CHECK(back.compile().setup->surfaces.size() ==
          GeometryProvider::buildScene(scene).size());
}

TEST(scenedoc, the_configuration_carries_the_document_beside_it) {
    scenedoc::SceneDocument doc;
    doc.add(scenedoc::ObjectType::ParabolicMirror, gp_Pnt(0, 0, 0));
    doc.add(scenedoc::ObjectType::Source, gp_Pnt(0, 0, 50.0));

    SimConfig cfg;
    cfg.rays  = 4321;
    cfg.power = 7.5;

    const QString json = configio::toJson(cfg, &doc);

    SimConfig               loadedCfg;
    scenedoc::SceneDocument loadedDoc;
    QString                 err;
    CHECK(configio::fromJson(json, loadedCfg, &loadedDoc, &err));
    CHECK(loadedCfg.rays == 4321);
    CHECK(loadedDoc.count() == 2);
    CHECK(loadedDoc.sourceCount() == 1);

    // A file written before there were documents leaves the caller's alone
    // rather than clearing it: the old format is a bare configuration, and a
    // scene the user has composed is not something opening one should discard.
    scenedoc::SceneDocument keep = doc;
    CHECK(configio::fromJson(configio::toJson(cfg), loadedCfg, &keep, &err));
    CHECK(keep.count() == 2);
}

TEST(scenedoc, an_assembled_scene_traces_through_the_import_path) {
    // The point of compiling to a SceneSetup: everything downstream -- the
    // mesher, the hierarchy, the trace, the analysis -- is the code every
    // built-in scene already goes through.
    scenedoc::SceneDocument doc;
    const int lens = doc.add(scenedoc::ObjectType::BallLens, gp_Pnt(0, 0, 0));
    doc.find(lens)->p[0] = 20.0;
    const int det = doc.add(scenedoc::ObjectType::Detector, gp_Pnt(0, 0, 90.0));
    doc.find(det)->p[0] = 200.0;
    doc.add(scenedoc::ObjectType::Source, gp_Pnt(0, 0, -120.0));

    const auto compiled = doc.compile();
    CHECK(compiled.warnings.isEmpty());

    SimConfig cfg;
    cfg.imported = compiled.setup;
    cfg.source   = SourceConfig::Type::Point;
    cfg.halfAngleDeg = 12.0;
    cfg.rays     = 3000;
    cfg.threads  = 1;
    CHECK(cfg.tracesImport());

    const SimulationResult res = Simulation::run(cfg);
    CHECK(res.raysEmitted > 0);
    // Light aimed at a ball lens from in front of it has to arrive somewhere.
    CHECK(res.raysHitDetector > 0);
}
