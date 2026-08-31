// Multi-object editing: writing one field to many objects. Suite "multiedit".
//
// A property panel showing four objects reports *which* box the user moved,
// as a mask, and multiedit::apply*Fields copies exactly the fields that mask
// names. The failure this guards against is silent and destructive: if a copy
// writes a field the mask did not name, four objects with four different
// positions are flattened onto the primary's the moment somebody nudges a
// rotation, and nothing on screen says it happened.
//
// So every test here has the same shape. Build a destination and a reference
// with *every* field different, apply a mask naming one field, and assert two
// things: the named field took the reference's value, and every other field
// still holds the destination's. The second assertion is the one that matters.
#include <cstdint>

namespace {

// A destination with a recognisable value in every placement slot.
struct Placement {
    gp_Pnt pos{1.0, 2.0, 3.0};
    double rot[3] = {10.0, 20.0, 30.0};
    double scale  = 1.5;
};

// The reference. Nothing it carries coincides with the destination, so any
// field that changes at all can only have come from here.
Placement referencePlacement() {
    Placement r;
    r.pos = gp_Pnt(-7.0, -8.0, -9.0);
    r.rot[0] = 70.0; r.rot[1] = 80.0; r.rot[2] = 90.0;
    r.scale  = 4.25;
    return r;
}

SourceSpec destinationSource() {
    SourceSpec s;
    s.type                = SourceConfig::Type::Lambertian;
    s.shape               = SourceConfig::Shape::PointLike;
    s.halfAngleDeg        = 12.0;
    s.sizeA               = 1.0;
    s.sizeB               = 2.0;
    s.beamRadius          = 3.0;
    s.spectrum.kind       = SpectrumConfig::Kind::Monochromatic;
    s.spectrum.wavelengthNm = 450.0;
    s.spectrum.cct        = 3000.0;
    s.power               = 5.0;
    s.polarisationState   = 0;
    s.rayFileScale        = 1.0;
    s.rayFileWavelengths  = true;
    return s;
}

SourceSpec referenceSource() {
    SourceSpec s;
    s.type                = SourceConfig::Type::Collimated;
    s.shape               = SourceConfig::Shape::Rect;
    s.halfAngleDeg        = 44.0;
    s.sizeA               = 11.0;
    s.sizeB               = 22.0;
    s.beamRadius          = 33.0;
    s.spectrum.kind       = SpectrumConfig::Kind::Blackbody;
    s.spectrum.wavelengthNm = 650.0;
    s.spectrum.cct        = 6500.0;
    s.power               = 55.0;
    s.polarisationState   = 2;
    s.rayFileScale        = 9.0;
    s.rayFileWavelengths  = false;
    return s;
}

SurfaceOptics destinationOptics() {
    SurfaceOptics o;
    o.reflectivity      = 0.10;
    o.transmissivity    = 0.20;
    o.fresnel           = false;
    o.mediumPriority    = 1;
    o.index             = 1.50;
    o.absorption        = 0.001;
    o.material.set      = false;
    o.material.nd       = 1.50;
    o.coating.model     = coating::Model::None;
    o.coating.residual  = 0.01;
    o.coating.highReflector = false;
    o.bsdf.model        = bsdf::Model::Lambertian;
    o.bsdf.alpha        = 0.05;
    o.bsdf.fraction     = 0.10;
    o.bsdf.abgA         = 1.0;
    o.bsdf.abgB         = 2.0;
    o.bsdf.abgG         = 3.0;
    o.scatter           = 0.15;
    o.roughness         = 0.002;
    o.setAppearanceColour(0.1, 0.2, 0.3);
    o.volume.coefficient = 0.4;
    o.volume.anisotropy  = 0.5;
    o.detNX             = 32;
    o.detNY             = 32;
    o.detAcceptanceDeg  = 90.0;
    o.detRejectMode     = SurfaceOptics::RejectMode::Absorb;
    return o;
}

SurfaceOptics referenceOptics() {
    SurfaceOptics o;
    o.reflectivity      = 0.90;
    o.transmissivity    = 0.80;
    o.fresnel           = true;
    o.mediumPriority    = 7;
    o.index             = 1.77;
    o.absorption        = 0.099;
    o.material.set      = true;
    o.material.nd       = 1.77;
    o.coating.model     = coating::Model::Ideal;
    o.coating.residual  = 0.995;
    o.coating.highReflector = true;
    o.bsdf.model        = bsdf::Model::Microfacet;
    o.bsdf.alpha        = 0.55;
    o.bsdf.fraction     = 0.60;
    o.bsdf.abgA         = 10.0;
    o.bsdf.abgB         = 20.0;
    o.bsdf.abgG         = 30.0;
    o.scatter           = 0.75;
    o.roughness         = 0.088;
    o.setAppearanceColour(0.7, 0.8, 0.9);
    o.volume.coefficient = 0.44;
    o.volume.anisotropy  = 0.55;
    o.detNX             = 256;
    o.detNY             = 512;
    o.detAcceptanceDeg  = 15.0;
    o.detRejectMode     = SurfaceOptics::RejectMode::Pass;
    return o;
}

// Everything in `o` except the four receiver rows, which have their own rule.
// Used to assert "the rest of the surface is untouched" in one place rather
// than repeating twenty comparisons per test.
bool sameNonReceiverOptics(const SurfaceOptics& a, const SurfaceOptics& b) {
    return a.reflectivity == b.reflectivity && a.transmissivity == b.transmissivity &&
           a.fresnel == b.fresnel && a.mediumPriority == b.mediumPriority &&
           a.index == b.index && a.absorption == b.absorption &&
           a.material.set == b.material.set && a.material.nd == b.material.nd &&
           a.coating.model == b.coating.model &&
           a.coating.residual == b.coating.residual &&
           a.coating.highReflector == b.coating.highReflector &&
           a.bsdf.model == b.bsdf.model && a.bsdf.alpha == b.bsdf.alpha &&
           a.bsdf.fraction == b.bsdf.fraction && a.bsdf.abgA == b.bsdf.abgA &&
           a.bsdf.abgB == b.bsdf.abgB && a.bsdf.abgG == b.bsdf.abgG &&
           a.scatter == b.scatter && a.roughness == b.roughness &&
           a.appearanceRgb[0] == b.appearanceRgb[0] &&
           a.appearanceRgb[1] == b.appearanceRgb[1] &&
           a.appearanceRgb[2] == b.appearanceRgb[2] &&
           a.volume.coefficient == b.volume.coefficient &&
           a.volume.anisotropy == b.volume.anisotropy;
}

bool sameSource(const SourceSpec& a, const SourceSpec& b) {
    return a.type == b.type && a.shape == b.shape &&
           a.halfAngleDeg == b.halfAngleDeg && a.sizeA == b.sizeA &&
           a.sizeB == b.sizeB && a.beamRadius == b.beamRadius &&
           a.spectrum.kind == b.spectrum.kind &&
           a.spectrum.wavelengthNm == b.spectrum.wavelengthNm &&
           a.spectrum.cct == b.spectrum.cct && a.power == b.power &&
           a.polarisationState == b.polarisationState &&
           a.rayFile == b.rayFile && a.rayFileScale == b.rayFileScale &&
           a.rayFileWavelengths == b.rayFileWavelengths;
}

} // namespace

// ---- the empty mask --------------------------------------------------------
//
// The degenerate case, and the one that states the contract most plainly: a
// panel that reports no field must write no field.

TEST(multiedit, an_empty_mask_writes_nothing_anywhere) {
    Placement d;
    const Placement r = referencePlacement();
    multiedit::applyPlacementFields(d.pos, d.rot, d.scale, r.pos, r.rot, r.scale, 0u);
    CHECK(d.pos.X() == 1.0 && d.pos.Y() == 2.0 && d.pos.Z() == 3.0);
    CHECK(d.rot[0] == 10.0 && d.rot[1] == 20.0 && d.rot[2] == 30.0);
    CHECK(d.scale == 1.5);

    double dp[4] = {1.0, 2.0, 3.0, 4.0};
    const double rp[4] = {9.0, 9.0, 9.0, 9.0};
    multiedit::applyParamFields(dp, rp, 4, 0u);
    CHECK(dp[0] == 1.0 && dp[1] == 2.0 && dp[2] == 3.0 && dp[3] == 4.0);

    SourceSpec ds = destinationSource();
    multiedit::applySourceFields(ds, referenceSource(), 0u);
    CHECK(sameSource(ds, destinationSource()));

    SurfaceOptics dopt = destinationOptics();
    multiedit::applyOpticsFields(dopt, referenceOptics(), 0u);
    CHECK(sameNonReceiverOptics(dopt, destinationOptics()));
    CHECK(dopt.detNX == 32 && dopt.detNY == 32);
    CHECK(dopt.detAcceptanceDeg == 90.0);
}

// ---- placement -------------------------------------------------------------

TEST(multiedit, placement_writes_only_the_named_axis) {
    Placement d;
    const Placement r = referencePlacement();
    multiedit::applyPlacementFields(d.pos, d.rot, d.scale, r.pos, r.rot, r.scale,
                                    multiedit::place::PosX);
    // The one field the mask named.
    CHECK(d.pos.X() == -7.0);
    // And the six it did not. This is the assertion that catches a stamp of the
    // primary's whole placement onto the rest of the selection.
    CHECK(d.pos.Y() == 2.0);
    CHECK(d.pos.Z() == 3.0);
    CHECK(d.rot[0] == 10.0 && d.rot[1] == 20.0 && d.rot[2] == 30.0);
    CHECK(d.scale == 1.5);
}

TEST(multiedit, placement_rotation_does_not_disturb_position_or_scale) {
    Placement d;
    const Placement r = referencePlacement();
    multiedit::applyPlacementFields(d.pos, d.rot, d.scale, r.pos, r.rot, r.scale,
                                    multiedit::place::RotY);
    CHECK(d.rot[1] == 80.0);
    CHECK(d.rot[0] == 10.0 && d.rot[2] == 30.0);
    CHECK(d.pos.X() == 1.0 && d.pos.Y() == 2.0 && d.pos.Z() == 3.0);
    CHECK(d.scale == 1.5);
}

TEST(multiedit, placement_scale_does_not_disturb_position_or_rotation) {
    Placement d;
    const Placement r = referencePlacement();
    multiedit::applyPlacementFields(d.pos, d.rot, d.scale, r.pos, r.rot, r.scale,
                                    multiedit::place::Scale);
    CHECK(d.scale == 4.25);
    CHECK(d.pos.X() == 1.0 && d.pos.Y() == 2.0 && d.pos.Z() == 3.0);
    CHECK(d.rot[0] == 10.0 && d.rot[1] == 20.0 && d.rot[2] == 30.0);
}

// The complement of the empty-mask test: every flag together reproduces the
// reference exactly, so no field is unreachable.
TEST(multiedit, the_full_placement_mask_reproduces_the_reference) {
    namespace F = multiedit::place;
    Placement d;
    const Placement r = referencePlacement();
    multiedit::applyPlacementFields(
        d.pos, d.rot, d.scale, r.pos, r.rot, r.scale,
        F::PosX | F::PosY | F::PosZ | F::RotX | F::RotY | F::RotZ | F::Scale);
    CHECK(d.pos.X() == r.pos.X() && d.pos.Y() == r.pos.Y() && d.pos.Z() == r.pos.Z());
    CHECK(d.rot[0] == r.rot[0] && d.rot[1] == r.rot[1] && d.rot[2] == r.rot[2]);
    CHECK(d.scale == r.scale);
}

// ---- geometry parameters ---------------------------------------------------

TEST(multiedit, params_write_only_the_named_slot) {
    double dp[8] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    const double rp[8] = {90.0, 91.0, 92.0, 93.0, 94.0, 95.0, 96.0, 97.0};
    multiedit::applyParamFields(dp, rp, 8, multiedit::param::slot(2));
    CHECK(dp[2] == 92.0);
    CHECK(dp[0] == 1.0 && dp[1] == 2.0 && dp[3] == 4.0 && dp[4] == 5.0);
    CHECK(dp[5] == 6.0 && dp[6] == 7.0 && dp[7] == 8.0);
}

TEST(multiedit, param_slot_zero_is_the_first_flag) {
    // slot(0) and the named Slot0 constant have to be the same bit, or the
    // first dimension is edited by a mask nobody raises.
    CHECK(multiedit::param::slot(0) == multiedit::param::Slot0);
    double dp[2] = {1.0, 2.0};
    const double rp[2] = {8.0, 9.0};
    multiedit::applyParamFields(dp, rp, 2, multiedit::param::Slot0);
    CHECK(dp[0] == 8.0);
    CHECK(dp[1] == 2.0);
}

// A mask is 32 bits, so slot 32 and up have no flag that could name them. They
// must be left alone rather than shifted past the width of the type.
TEST(multiedit, params_leave_slots_past_the_mask_width_alone) {
    double dp[40];
    double rp[40];
    for (int k = 0; k < 40; ++k) { dp[k] = double(k); rp[k] = 1000.0 + double(k); }
    multiedit::applyParamFields(dp, rp, 40, 0xFFFFFFFFu);
    for (int k = 0; k < 32; ++k) CHECK(dp[k] == 1000.0 + double(k));
    for (int k = 32; k < 40; ++k) CHECK(dp[k] == double(k));
}

TEST(multiedit, params_tolerate_a_null_array) {
    const double rp[2] = {1.0, 2.0};
    multiedit::applyParamFields(nullptr, rp, 2, 0xFFFFFFFFu);
    double dp[2] = {3.0, 4.0};
    multiedit::applyParamFields(dp, nullptr, 2, 0xFFFFFFFFu);
    CHECK(dp[0] == 3.0 && dp[1] == 4.0);
}

// ---- emission --------------------------------------------------------------

TEST(multiedit, source_writes_only_the_named_field) {
    SourceSpec d = destinationSource();
    multiedit::applySourceFields(d, referenceSource(), multiedit::src::Power);
    CHECK(d.power == 55.0);

    // Everything else still the destination's. Rebuilt rather than compared
    // field by field here, with power put back so the comparison is about the
    // thirteen fields the mask did not name.
    SourceSpec expect = destinationSource();
    expect.power = 55.0;
    CHECK(sameSource(d, expect));
}

TEST(multiedit, source_half_angle_does_not_drag_the_beam_radius_with_it) {
    // The two are alternatives on screen -- one is shown for a cone, the other
    // for a collimated beam -- which is exactly the pairing a whole-form write
    // would silently couple.
    SourceSpec d = destinationSource();
    multiedit::applySourceFields(d, referenceSource(), multiedit::src::HalfAngle);
    CHECK(d.halfAngleDeg == 44.0);
    CHECK(d.beamRadius == 3.0);
    CHECK(d.type == SourceConfig::Type::Lambertian);
}

TEST(multiedit, source_spectrum_kind_does_not_write_the_wavelength_or_cct) {
    SourceSpec d = destinationSource();
    multiedit::applySourceFields(d, referenceSource(), multiedit::src::Spectrum);
    CHECK(d.spectrum.kind == SpectrumConfig::Kind::Blackbody);
    CHECK(d.spectrum.wavelengthNm == 450.0);
    CHECK(d.spectrum.cct == 3000.0);
}

// ---- surface optics --------------------------------------------------------

TEST(multiedit, optics_writes_only_the_named_field) {
    SurfaceOptics d = destinationOptics();
    multiedit::applyOpticsFields(d, referenceOptics(), multiedit::optic::Reflectivity);
    CHECK(d.reflectivity == 0.90);

    SurfaceOptics expect = destinationOptics();
    expect.reflectivity = 0.90;
    CHECK(sameNonReceiverOptics(d, expect));
}

TEST(multiedit, optics_roughness_leaves_the_other_scatter_fields_alone) {
    // Four surfaces with four different roughnesses keeping them while their
    // shared reflectance is set once is the whole point of the feature.
    SurfaceOptics d = destinationOptics();
    multiedit::applyOpticsFields(d, referenceOptics(), multiedit::optic::Roughness);
    CHECK(d.roughness == 0.088);
    CHECK(d.scatter == 0.15);
    CHECK(d.bsdf.model == bsdf::Model::Lambertian);
    CHECK(d.bsdf.alpha == 0.05);
}

TEST(multiedit, optics_appearance_colour_moves_as_one_triple) {
    SurfaceOptics d = destinationOptics();
    multiedit::applyOpticsFields(d, referenceOptics(), multiedit::optic::Appearance);
    CHECK(d.appearanceRgb[0] == 0.7);
    CHECK(d.appearanceRgb[1] == 0.8);
    CHECK(d.appearanceRgb[2] == 0.9);
    CHECK(d.reflectivity == 0.10);   // and nothing physical moved with it
}

// The compound masks. One user action -- picking a preset -- touches several
// fields, so the mask names all of them and nothing beyond them.
TEST(multiedit, the_coating_preset_mask_writes_exactly_its_three_fields) {
    SurfaceOptics d = destinationOptics();
    multiedit::applyOpticsFields(d, referenceOptics(), multiedit::optic::CoatingPreset);
    CHECK(d.coating.model == coating::Model::Ideal);
    CHECK(d.coating.residual == 0.995);
    CHECK(d.coating.highReflector == true);

    SurfaceOptics expect = destinationOptics();
    expect.coating.model         = coating::Model::Ideal;
    expect.coating.residual      = 0.995;
    expect.coating.highReflector = true;
    CHECK(sameNonReceiverOptics(d, expect));
}

TEST(multiedit, the_material_choice_mask_writes_exactly_material_index_absorption) {
    SurfaceOptics d = destinationOptics();
    multiedit::applyOpticsFields(d, referenceOptics(), multiedit::optic::MaterialChoice);
    CHECK(d.material.set == true);
    CHECK(d.material.nd == 1.77);
    CHECK(d.index == 1.77);
    CHECK(d.absorption == 0.099);

    SurfaceOptics expect = destinationOptics();
    expect.material   = referenceOptics().material;
    expect.index      = 1.77;
    expect.absorption = 0.099;
    CHECK(sameNonReceiverOptics(d, expect));
}

// ---- the receiver asymmetry ------------------------------------------------
//
// The one place a named field is refused. A bin grid on a mirror is not a
// harmless spare number: isDetector decides whether the mesher lays a grid out
// at all, so those fields would sit in the document contradicting it.

TEST(multiedit, receiver_fields_are_refused_on_a_surface_that_is_not_one) {
    namespace F = multiedit::optic;
    SurfaceOptics d = destinationOptics();
    d.isDetector = false;
    SurfaceOptics r = referenceOptics();
    r.isDetector = true;

    multiedit::applyOpticsFields(
        d, r, F::DetNX | F::DetNY | F::DetAcceptance | F::DetReject);

    CHECK(d.detNX == 32);
    CHECK(d.detNY == 32);
    CHECK(d.detAcceptanceDeg == 90.0);
    CHECK(d.detRejectMode == SurfaceOptics::RejectMode::Absorb);
    // And the refusal did not quietly make it one.
    CHECK(d.isDetector == false);
}

TEST(multiedit, receiver_fields_are_written_on_a_receiver) {
    namespace F = multiedit::optic;
    SurfaceOptics d = destinationOptics();
    d.isDetector = true;
    SurfaceOptics r = referenceOptics();
    r.isDetector = true;

    multiedit::applyOpticsFields(
        d, r, F::DetNX | F::DetNY | F::DetAcceptance | F::DetReject);

    CHECK(d.detNX == 256);
    CHECK(d.detNY == 512);
    CHECK(d.detAcceptanceDeg == 15.0);
    CHECK(d.detRejectMode == SurfaceOptics::RejectMode::Pass);
    // The receiver mask is still only the receiver rows.
    CHECK(sameNonReceiverOptics(d, destinationOptics()));
}

// ---- the flags themselves --------------------------------------------------
//
// Two flags sharing a bit is the failure mode that makes every test above pass
// while the feature is broken: the mask would name one field and write two.

TEST(multiedit, every_optic_flag_occupies_its_own_bit) {
    namespace F = multiedit::optic;
    const std::uint32_t all[] = {
        F::Reflectivity, F::Transmissivity, F::Fresnel, F::MediumPriority,
        F::Material, F::Index, F::Absorption, F::CoatingModel, F::CoatingResidual,
        F::CoatingHigh, F::BsdfModel, F::BsdfAlpha, F::BsdfFraction, F::AbgA,
        F::AbgB, F::AbgG, F::Scatter, F::Roughness, F::Appearance,
        F::VolumeCoefficient, F::VolumeAnisotropy, F::DetNX, F::DetNY,
        F::DetAcceptance, F::DetReject};
    const int n = int(sizeof(all) / sizeof(all[0]));
    std::uint32_t seen = 0;
    for (int i = 0; i < n; ++i) {
        CHECK(all[i] != 0u);
        CHECK((seen & all[i]) == 0u);   // no bit used twice
        seen |= all[i];
    }
}

TEST(multiedit, every_source_flag_occupies_its_own_bit) {
    namespace F = multiedit::src;
    const std::uint32_t all[] = {
        F::Type, F::Shape, F::HalfAngle, F::SizeA, F::SizeB, F::BeamRadius,
        F::Spectrum, F::Wavelength, F::Cct, F::Power, F::Polarisation,
        F::RayFile, F::RayScale, F::RayLambda};
    const int n = int(sizeof(all) / sizeof(all[0]));
    std::uint32_t seen = 0;
    for (int i = 0; i < n; ++i) {
        CHECK(all[i] != 0u);
        CHECK((seen & all[i]) == 0u);
        seen |= all[i];
    }
}

TEST(multiedit, every_placement_flag_occupies_its_own_bit) {
    namespace F = multiedit::place;
    const std::uint32_t all[] = {F::PosX, F::PosY, F::PosZ,
                                 F::RotX, F::RotY, F::RotZ, F::Scale};
    std::uint32_t seen = 0;
    for (int i = 0; i < 7; ++i) {
        CHECK(all[i] != 0u);
        CHECK((seen & all[i]) == 0u);
        seen |= all[i];
    }
}

// The compound masks have to be exactly the union of the flags they stand for.
// A stray bit here writes a field the user never touched.
TEST(multiedit, the_compound_masks_are_unions_of_their_own_flags) {
    namespace F = multiedit::optic;
    CHECK(F::CoatingPreset ==
          (F::CoatingModel | F::CoatingResidual | F::CoatingHigh));
    CHECK(F::MaterialChoice == (F::Material | F::Index | F::Absorption));
}
