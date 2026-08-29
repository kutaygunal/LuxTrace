// Tests for T3-002: measured-data sample caps raised + resampling surfaced.
//
// Included by TestMain.cpp (see ./TestMain.cpp), suite "fidelity".
//
// These pin the structural fix: the three measured-data tables (coating R,
// material n/k, BSDF) no longer truncate a measurement to eight / sixteen
// points at load. Ordinary files — which have up to a few hundred points —
// round-trip exactly as read on the hot-path arrays, and a file that exceeds
// the raised cap is resampled onto it while its exact source is kept off-path
// with `reduced` set, so nothing a vendor published is lost and the reduction
// is visible where the number is read.

namespace {

// Writes a refractiveindex.info "tabulated n" YAML with `n` equally spaced
// points across 0.40-0.90 um, n ramping linearly with the point index. Returns
// the temp path. The values are deterministic so the test can assert exact
// retention of every point the file wrote.
QString writeTabulatedYaml(const QString& fname, int n) {
    const QString path = QDir::temp().filePath(fname);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        CHECK(false);
        return path;
    }
    QTextStream ts(&f);
    ts.setRealNumberPrecision(17);
    ts << "REFERENCES: \"test\"\n";
    ts << "DATA:\n";
    ts << "  - type: tabulated n\n";
    ts << "    data: |\n";
    for (int i = 0; i < n; ++i) {
        const double lamUm = 0.40 + 0.50 * double(i) / double(n - 1);
        const double nv    = 1.50 + 0.010 * double(i) / double(n - 1);
        ts << "        " << lamUm << " " << nv << "\n";
    }
    return path;
}

double expectedWavelengthNm(int i, int n) { return (0.40 + 0.50 * double(i) / double(n - 1)) * 1000.0; }
double expectedIndex    (int i, int n) { return  1.50 + 0.010 * double(i) / double(n - 1); }

} // namespace

TEST(fidelity, measured_data_caps_are_raised) {
    // The three tables that used to truncate to 8 (material + coating) and 16
    // (BSDF) now carry a real vendor table on the fixed hot-path arrays.
    CHECK(OpticalMaterial::kMaxSamples == 256);
    CHECK(coating::Coating::kMaxSamples == 256);
    CHECK(bsdf::Surface::kMaxSamples == 512);
}

TEST(fidelity, a_small_measured_table_round_trips_with_full_fidelity) {
    // A >8-point table (this was the old material cap) must be kept exactly as
    // read: every source point lands on the hot-path arrays, with no resample
    // and no reduction flagged.
    const int n = 24;
    const QString path = writeTabulatedYaml(QStringLiteral("luxtrace_t3_fit24.yml"), n);
    const auto res = materialfile::loadRefractiveIndexYaml(path, QStringLiteral("T3-Fit24"));
    if (!res.ok) std::printf("       %s\n", res.error.toLocal8Bit().constData());
    CHECK(res.ok);

    const OpticalMaterial m = materials::byName(QStringLiteral("T3-Fit24"));
    CHECK(m.valid());
    CHECK(m.model == OpticalMaterial::Model::Table);
    CHECK(m.samples == n);                       // every point present, none discarded
    CHECK(m.originCount == n);                   // provenance: how many were measured
    CHECK(!m.reduced);                           // nothing was thinned down
    CHECK(m.originN.empty());                    // the arrays ARE the full data
    for (int i = 0; i < n; ++i) {
        CHECK_NEAR(m.lambdaNm[i], expectedWavelengthNm(i, n), 1e-9);
        CHECK_NEAR(m.nSample[i],  expectedIndex(i, n),        1e-9);
        CHECK_NEAR(m.kSample[i],  0.0, 1e-12);   // a transparent glass
    }
    // The description that a user reads surfaces the fidelity.
    CHECK(!res.names.isEmpty());

    QFile::remove(path);
    materials::resetLoaded();
    CHECK(materials::indexOf(QStringLiteral("T3-Fit24")) < 0);
}

TEST(fidelity, an_oversized_measured_table_keeps_its_source_and_flags_reduction) {
    // A file with more points than even the raised cap is resampled onto the
    // hot-path arrays so they stay fixed-size, but its exact source is kept
    // off-path and the reduction is made visible rather than silent.
    const int n = 400;                           // > 256
    const QString path = writeTabulatedYaml(QStringLiteral("luxtrace_t3_big400.yml"), n);
    const auto res = materialfile::loadRefractiveIndexYaml(path, QStringLiteral("T3-Big400"));
    if (!res.ok) std::printf("       %s\n", res.error.toLocal8Bit().constData());
    CHECK(res.ok);

    const OpticalMaterial m = materials::byName(QStringLiteral("T3-Big400"));
    CHECK(m.valid());
    CHECK(m.model == OpticalMaterial::Model::Table);
    CHECK(m.originCount == n);
    CHECK(m.reduced);                            // the fixed array could not carry it all
    CHECK(m.samples == OpticalMaterial::kMaxSamples);
    // The exact source round-trips so no measured point is lost.
    CHECK(int(m.originLambdaNm.size()) == n);
    CHECK(int(m.originN.size()) == n);
    CHECK(int(m.originK.size()) == n);
    for (int i = 0; i < n; ++i) {
        CHECK_NEAR(m.originLambdaNm[std::size_t(i)], expectedWavelengthNm(i, n), 1e-9);
        CHECK_NEAR(m.originN[std::size_t(i)],         expectedIndex(i, n),        1e-9);
    }
    // The resampled hot-path table still spans the file's own range.
    CHECK_NEAR(m.lambdaNm[0], expectedWavelengthNm(0, n), 1e-6);
    CHECK_NEAR(m.lambdaNm[m.samples - 1], expectedWavelengthNm(n - 1, n), 1e-6);
    // And it still indexes the band sensibly.
    CHECK(m.indexAt(500.0) > 1.49 && m.indexAt(500.0) < 1.53);

    QFile::remove(path);
    materials::resetLoaded();
}

TEST(fidelity, bsdf_table_cap_is_raised_and_retains_points) {
    // The BSDF table was capped at 16. With the raised cap a measured scatter
    // spec of a few dozen points sits on the hot-path arrays exactly as set, and
    // the off-path provenance carries how many were measured.
    bsdf::Surface b;
    b.model       = bsdf::Model::Table;
    const int n   = 48;                          // > 16
    b.samples     = n;
    b.originCount = n;
    b.originBeta.reserve(std::size_t(n));
    b.originValue.reserve(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        const double beta = std::pow(10.0, -4.0 + 6.0 * double(i) / double(n - 1));
        b.tableBeta[i] = beta;
        b.tableValue[i] = 100.0 / std::pow(1.0 + 1e4 * beta, 1.8);
        b.originBeta.push_back(beta);
        b.originValue.push_back(b.tableValue[i]);
    }
    CHECK(b.samples == n);
    CHECK(b.originCount == n);
    CHECK(!b.reduced);
    // The lobe evaluates inside its own range and clamps past it (no extrapolation).
    CHECK(b.value(b.tableBeta[0]) > 0.0);
    CHECK_NEAR(b.value(b.tableBeta[n - 1] * 10.0), b.tableValue[n - 1], 1e-12);
    const double tis = b.totalIntegratedScatter();
    CHECK(tis >= 0.0 && tis <= 1.0);
}

TEST(fidelity, a_coating_table_round_trips_through_the_config) {
    // A >8-point measured coating table survives a save/load of a configuration,
    // so a measured R(lambda) coating is not truncated when it is persisted.
    coating::Coating c;
    c.model = coating::Model::Table;
    const int n = 30;                            // > 8
    c.samples = n;
    c.originCount = n;
    for (int i = 0; i < n; ++i) {
        c.lambdaNm[i]   = 300.0 + 10.0 * double(i);
        c.reflectance[i] = 0.02 + 0.002 * double(i);
        c.originLambdaNm.push_back(c.lambdaNm[i]);
        c.originReflectance.push_back(c.reflectance[i]);
    }

    SurfaceOverride ov;
    ov.label      = QStringLiteral("Lens_A");
    ov.optics.index = 1.5;
    ov.optics.coating = c;

    SimConfig cfg;
    cfg.scene = GeometryProvider::Scene::Lens;
    cfg.surfaceOverrides.push_back(ov);

    SimConfig out;
    CHECK(configio::fromJson(configio::toJson(cfg), out));
    CHECK(out.surfaceOverrides.size() == 1);
    const coating::Coating& c2 = out.surfaceOverrides[0].optics.coating;
    CHECK(c2.model == coating::Model::Table);
    CHECK(c2.samples == n);                      // all 30 points came back
    for (int i = 0; i < n; ++i) {
        CHECK_NEAR(c2.lambdaNm[i],   c.lambdaNm[i],   1e-9);
        CHECK_NEAR(c2.reflectance[i], c.reflectance[i], 1e-9);
    }
}

TEST(fidelity, material_table_fidelity_appears_in_the_description) {
    // The material description -- what the picker tooltip and the inspector
    // show -- states the fidelity of a measured table, so a reduced curve is
    // legible where the number is read rather than silently thinned.
    const int n = 24;
    const QString path = writeTabulatedYaml(QStringLiteral("luxtrace_t3_desc.yml"), n);
    const auto res = materialfile::loadRefractiveIndexYaml(path, QStringLiteral("T3-Desc"));
    CHECK(res.ok);
    const int idx = materials::indexOf(QStringLiteral("T3-Desc"));
    CHECK(idx >= 0);
    const QString desc = materials::description(idx);
    CHECK(desc.contains(QStringLiteral("24 measured points")));
    CHECK(desc.contains(QStringLiteral("full fidelity")));

    QFile::remove(path);
    materials::resetLoaded();
}
