#pragma once
#include <cstdint>

// The boundary between the application and the device code.
//
// Plain structs and raw pointers, and deliberately no Qt and no OCCT. nvcc
// compiles this translation unit with its own front end before handing the host
// half to cl, and Qt's compiler detection does not survive that trip -- but the
// real reason is simpler: device code has no business knowing what a QString or
// a gp_Pnt is. The host side flattens the scene into these and nothing crosses
// that is not a number.
namespace gpukernel {

struct Tri   { float v0[3], e1[3], e2[3], n[3]; int surf; int pad[3]; };
struct Shade { float n0[3], n1[3], n2[3]; };
struct Node  { float bmin[3], bmax[3]; int left, right, count, axis; };

// One tessellated part, as a slice of the concatenated bottom-level arrays.
// Its node and order indices stay relative to the part, so the same part
// placed twenty-five times is uploaded once.
struct Blas  { int triFirst, nodeFirst, orderFirst, nodeCount; };

// One placement of a part. The transform is rigid -- rotation rows world
// <- local, their inverse local <- world, and a translation -- so a distance
// means the same thing on both sides of it and the traversal's running best
// carries across untouched.
struct Inst  {
    float r0[3], r1[3], r2[3];
    float origin[3];
    float i0[3], i1[3], i2[3];
    float bmin[3], bmax[3];
    int   blas;
    int   surf;                 // index into the scene's surfaces
};

struct Surf {
    float index;            // refractive index, 0 for an opaque surface
    float reflectivity;
    float transmissivity;
    float absorption;       // 1/mm, Beer-Lambert inside this medium
    int   fresnel;
    int   isDetector;
    // A metal's complex index at the run's wavelength. metalK > 0 marks the
    // surface as one whose reflectance comes from the absorbing-medium Fresnel
    // equations rather than from `reflectivity`: aluminium at 45 degrees is not
    // aluminium at normal incidence, and a flat number is out by three points
    // of efficiency on a reflector.
    float metalN;
    float metalK;
    // An ideal coating's residual reflectance at normal incidence, or a
    // negative number for a bare surface. Only the ideal model crosses: a
    // measured table and a characteristic-matrix stack are refused by the host,
    // because approximating them here would be inventing a coating.
    float coatResidual;
    int   coatHighReflector;
    // Where two solids overlap, the higher priority wins: a ray inside both
    // is treated as travelling in the higher-priority medium. Without it the
    // preview always took the innermost, which is the same answer only when
    // nothing in the scene sets a priority.
    int   mediumPriority;

    // How the surface scatters, and how the medium behind it does.
    //
    // Resolved on the host by raytracer::resolveScattering, which is the
    // reference's own function rather than a second copy of it: `scatter`
    // and `roughness` are shorthands a scene is written in, and two
    // backends that read them differently are not tracing the same surface.
    //   0 specular  1 GGX microfacet  2 ABg  3 Lambertian  4 measured table
    int   bsdfModel;
    float bsdfAlpha;              // GGX RMS microfacet slope
    float abgA, abgB, abgG;
    float bsdfFraction;           // share that scatters at all
    // Total integrated scatter. Quadrature over the hemisphere for ABg and
    // for a table, so it is computed once on the host rather than four
    // hundred times per hit on the device.
    float bsdfTis;
    int   tableFirst, tableCount; // slice of Input::tableBeta / tableValue

    // Volume scattering inside this medium. 0 coefficient is a clear one.
    float volCoeff, volG, volAlpha;
    int   volPhase;               // 0 Henyey-Greenstein, 1 Gegenbauer
};

// How many receivers one connection may reach. The reference carries the same
// eight, and a scene with more of them is one nobody has built.
inline constexpr int kMaxReceivers = 8;

struct Det {
    float center[3], u[3], v[3], n[3];
    float w, h;
    int   nx, ny;
    float cosAcceptance;    // -1 accepts everything
    int   rejectPasses;
    // Which surface this receiver is, so a connection to it does not treat
    // it as its own occluder, and where its bins begin in the shared grid.
    int   surf;
    int   cellFirst;
};

struct Src {
    float origin[3], axis[3];
    int   type;             // 0 point, 1 lambertian, 2 collimated
    float cosHalfAngle;
    // Emitting area: 0 point-like, 1 disc, 2 rect, 3 sphere. A source with an
    // area has a real etendue, which is what stops a concentrator reporting a
    // gain it cannot have.
    int   shape;
    float sizeA, sizeB;
    float beamRadius;
    // The refractive solids this emitter sits inside, outermost first. An
    // LED die in its own encapsulant starts its rays already in that solid
    // rather than in vacuum, or the first interface it meets refracts
    // against the wrong pair of indices. Found once per source per run by
    // the reference own containment probe, not by the device.
    int   media[8];
    int   mediaN;
};

// Everything the launch needs. The pointers are host memory; the kernel uploads
// and frees its own copies, so the caller owns nothing on the device.
struct Input {
    const Tri*   tris   = nullptr;  int nTris   = 0;
    const Shade* shade  = nullptr;                    // null: use facet normals
    const Node*  nodes  = nullptr;  int nNodes  = 0;
    const int*   order  = nullptr;  int nOrder  = 0;
    const Surf*  surfs  = nullptr;  int nSurfs  = 0;
    // Every measured BSDF in the scene, concatenated; Surf::tableFirst
    // and tableCount slice it.
    // Owen-scrambled Sobol for the emission stream, over the direction
    // numbers the host builds from sampling::directions(). Null, or zero
    // replicas, traces the emission from the counter-based generator
    // instead -- which is what makes the two comparable.
    const unsigned int* sobolDir = nullptr;   // 8 x 32, row-major by dimension
    int                 replicas = 1;
    int                 lowDiscrepancy = 0;
    const float* tableBeta  = nullptr;
    const float* tableValue = nullptr;  int nTable = 0;
    // The instanced half of the hierarchy. Empty for a scene that has no
    // placements, which is most of them.
    const Tri*   blasTris  = nullptr;  int nBlasTris = 0;
    const Shade* blasShade = nullptr;                    // null: facet normals
    const Node*  blasNodes = nullptr;  int nBlasNodes = 0;
    const int*   blasOrder = nullptr;  int nBlasOrder = 0;
    const Blas*  blas      = nullptr;  int nBlas      = 0;
    const Inst*  insts     = nullptr;  int nInsts     = 0;
    const Node*  tlas      = nullptr;  int nTlas      = 0;
    const int*   tlasOrder = nullptr;  int nTlasOrder = 0;
    // Every receiver in the scene, and which one each surface is. One grid
    // holds them all end to end, the way the reference lays them out, so a
    // second receiver costs one more slice rather than a second run.
    const Det*   dets       = nullptr;  int nDets = 0;
    const int*   detOfSurf  = nullptr;  int nDetOfSurf = 0;
    int          gridCells  = 0;
    Src          src{};
    // Connect every diffuse bounce to every receiver analytically instead
    // of hoping a random walk finds it.
    int          nextEvent  = 0;
    // Emit only into the cone the scene occupies, weighting each ray by the
    // share of the source law that lands there. Exact rather than unbiased: a
    // direction outside that cone cannot reach the geometry at all.
    float        boundsCentre[3] = {0, 0, 0};
    float        boundsRadius    = 0.0f;
    int          aim             = 0;
    // Follow a dim branch with a probability instead of dropping it.
    int          roulette        = 0;

    // The run spectrum. The inverse CDF the reference built, uploaded rather
    // than rebuilt, and every wavelength-dependent surface quantity sampled
    // across the band it covers -- the refractive index, a metal complex
    // index, a measured coating table. Sampling the reference own curves
    // once per run rather than porting four dispersion models is what keeps
    // the two backends from disagreeing about what a glass is.
    const float* invCdf     = nullptr;   // spdBins + 1 entries
    int          spdBins    = 0;
    int          spdMono    = 1;
    int          spdRgb     = 0;
    float        monoLambda = 587.6f;
    float        meanV      = 0.0f;
    int          unitLumen  = 0;
    int          spectral   = 0;
    // [surface][quantity][sample], four quantities, across [specMinNm,
    // specMaxNm].
    const float* specTable   = nullptr;
    int          specSamples = 0;
    float        specMinNm = 380.0f, specMaxNm = 780.0f;

    // Whether to carry a Stokes state per branch, and the state the source
    // emits, normalised to unit intensity. Unpolarised is the default and the
    // only state an ordinary illumination source is in.
    int          polarised = 0;
    float        emittedState[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    // Far-field accumulation, on the fine uniform master grid the reported
    // partition is derived from. 0 theta bins switches it off.
    int                nThetaMaster = 0;
    int                nPhi         = 0;
    long long          rays = 0;
    unsigned long long seed = 0;
    int                wantVariance = 0;
};

// What comes back. `grid` and `gridSq` are caller-allocated, nx*ny long;
// `gridSq` may be null when variance was not asked for.
struct Output {
    double*             grid    = nullptr;
    double*             gridSq  = nullptr;
    // Three colour bands over the first receiver, or null. They sum to the
    // irradiance grid exactly, which is what every band-wise reading of the
    // result depends on.
    double*             bandGrid = nullptr;
    // nThetaMaster * nPhi, or null. Caller-allocated like the others.
    double*             angles    = nullptr;
    double*             anglesSq  = nullptr;
    // detected, absorbed, escaped, truncated, rejected, and -- as a subset of
    // absorbed, not a sixth channel of its own -- the Beer-Lambert share.
    double              totals[6] = {0, 0, 0, 0, 0, 0};
    double              moments[2] = {0, 0};           // sum w, sum w^2 of per-ray detected weight
    // What a BSDF sampling weight created or destroyed. Not a loss channel:
    // its expectation is zero, and the budget only closes with it in.
    double              residualBsdf = 0.0;
    // Medium-tracking anomalies: unmatched exit, medium-stack overflow,
    // guessed incident index. The tracer counting its own recoveries is what
    // lets a run say whether the index pairs behind its answer were the
    // geometry's or a guess.
    unsigned long long  anomalies[3] = {0, 0, 0};
    // Detected and emitted weight per Owen scramble, caller-allocated,
    // `Input::replicas` long. The spread across them is the error bar a
    // low-discrepancy run actually achieved; null asks for neither.
    double*             repDet   = nullptr;
    double*             repEmit  = nullptr;
    // The analytic direct term, entered negative because it reached the
    // receiver without a ray carrying it, and the sampled arrival that was
    // suppressed because it already had. Both are estimator residual, both
    // are zero in expectation, and the budget only closes with them in.
    double              residualNee     = 0.0;
    // What emission aiming did not trace, and what roulette killed or
    // promoted. Both are estimator residual and both average to nothing.
    double              residualAiming   = 0.0;
    double              residualRoulette = 0.0;
    double              residualSkipDet = 0.0;
    unsigned long long  hits = 0;
    // Flux-weighted Stokes sum of what reached the receiver, on a polarised
    // run. Zero and meaningless otherwise; `Input::polarised` says which.
    double              detStokes[4] = {0, 0, 0, 0};
    float               milliseconds = 0.0f;
};

// Is there a device, and what is it called? `name` and `why` are caller
// buffers; one of them is filled.
bool probe(char* name, int nameCap, char* why, int whyCap);

// Runs the trace. False on failure, with `err` filled.
bool run(const Input& in, Output& out, char* err, int errCap);

} // namespace gpukernel
