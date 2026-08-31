// Atomic large-receiver accumulation. Suite "atomicgrid".
//
// RayTracer::trace has two receiver-accumulation paths: above 65536
// all-receivers cells, threads accumulate into ONE shared atomic grid (relaxed
// CAS adds per bin) instead of a per-thread copy; below the threshold the
// per-thread bit-exact reduction is used. Every other suite exercises only the
// small-receiver path. This module pins the large one down:
//
//   * equivalence   a large (atomic) grid traces to the same bin totals as a
//                   small (per-thread) grid at a matching resolution,
//   * determinism   the atomic path is deterministic across identical runs
//                   (updates cannot be lost or mis-ordered),
//   * concurrency   concurrent writers into the shared grid lose no updates
//                   (the multi-threaded total equals the single-threaded one).
//
// Detector grid resolution comes from the mesh's detNX/detNY, so a per-thread
// run uses a 128x128 detector (16384 cells < 65536) and an atomic run uses a
// 512x512 detector (262144 cells >= 65536). Because every ray seeds its RNG
// from its global index, the two runs emit the identical ray set for a fixed
// seed, so the only difference between them is the accumulation path. 512 is an
// exact multiple of 128, so the atomic 512x512 grid rebins down to the 128x128
// grid by summing 4x4 fine bins per coarse bin and the two can be compared
// bin-for-bin, not just as a total.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

// A collimated bundle normal to a square detector, lit with a smooth uniform
// disc so the fine grid rebins cleanly onto the coarse one. `bins` is the
// detector's grid resolution -- the only thing that differs between the
// per-thread and the atomic runs.
SimulationResult atomicPathProbe(int bins, int rays, std::uint64_t seed,
                                 unsigned threads) {
    const double detectorSize = 400.0, D = 50.0;
    MeshSurface det = detectorSurface(detectorSize, D, bins);

    SourceConfig src;
    src.type       = SourceConfig::Type::Collimated;
    src.axis       = gp_Dir(0, 0, 1);
    src.origin     = gp_Pnt(0, 0, 0);
    src.beamRadius = 140.0;   // lit disc, safely inside the 400 mm detector
    src.rays       = rays;

    TraceOptions opt;
    opt.threads = threads;
    opt.seed    = seed;

    SimulationResult out;
    MeshList meshes{det};
    RayTracer::trace(meshes, src, out, opt);
    return out;
}

// The atomically-reduced 512x512 grid, summed 4x4 down to one coarse cell.
double rebin4(const std::vector<double>& fine, int cx, int cy) {
    double sum = 0.0;
    for (int dy = 0; dy < 4; ++dy)
        for (int dx = 0; dx < 4; ++dx) {
            const int fx = cx * 4 + dx, fy = cy * 4 + dy;
            sum += fine[std::size_t(fy) * 512u + std::size_t(fx)];
        }
    return sum;
}

} // namespace

// Equivalence: the atomic large-receiver path agrees with the small-receiver
// per-thread path at a matching resolution, within tolerance.
TEST(atomicgrid, atomic_large_grid_matches_per_thread_path_on_bin_totals) {
    const std::uint64_t seed = 54321u;
    const int rays = 250000;
    const unsigned threads = 8;

    // Small grid: 128x128 = 16384 cells (< 65536) -> per-thread reduction.
    const SimulationResult perThread = atomicPathProbe(128, rays, seed, threads);
    // Large grid: 512x512 = 262144 cells (>= 65536) -> shared atomic reduction.
    const SimulationResult atomic = atomicPathProbe(512, rays, seed, threads);

    // Guard the premise: the two runs really do sit on opposite sides of the
    // atomic-receiver threshold, so each path was actually exercised.
    CHECK(perThread.nx * perThread.ny < 65536);
    CHECK(atomic.nx * atomic.ny >= 65536);

    // Total flux on the receiver is a property of the arrivals, not of the
    // binning, so it must agree between the paths to within FP rounding.
    CHECK_NEAR(atomic.fluxDetector, perThread.fluxDetector,
               1e-9 * std::max(1.0, std::fabs(perThread.fluxDetector)));

    // The atomic grid, rebinned 4x4 -> 128x128, must match the per-thread grid
    // bin-for-bin. Both carry the identical set of arrivals and the coarse cell
    // boundaries align, so the limit is reduced FP rounding, not noise.
    const double refMax = *std::max_element(perThread.irradiance.begin(),
                                            perThread.irradiance.end());
    for (int cy = 0; cy < 128; ++cy) {
        for (int cx = 0; cx < 128; ++cx) {
            const double big =
                rebin4(atomic.irradiance, cx, cy);
            const double small =
                perThread.irradiance[std::size_t(cy) * 128u + std::size_t(cx)];
            const double tol = 1e-6 * std::max(refMax, 1e-12);
            CHECK_NEAR(big, small, tol);
        }
    }
}

// Determinism: two identical atomic runs give the same result, in total and
// bin-for-bin -- no update is lost or mis-ordered in a way that changes the
// physics.
TEST(atomicgrid, atomic_large_grid_is_deterministic_across_runs) {
    const std::uint64_t seed = 424242u;
    const int rays = 250000;
    const unsigned threads = 8;

    const SimulationResult a = atomicPathProbe(512, rays, seed, threads);
    const SimulationResult b = atomicPathProbe(512, rays, seed, threads);
    CHECK(a.nx * a.ny >= 65536);

    CHECK_NEAR(a.fluxDetector, b.fluxDetector,
               1e-9 * std::max(1.0, std::fabs(a.fluxDetector)));
    const double refMax = *std::max_element(a.irradiance.begin(),
                                            a.irradiance.end());
    for (std::size_t i = 0; i < a.irradiance.size(); ++i)
        CHECK_NEAR(a.irradiance[i], b.irradiance[i],
                   1e-9 * std::max(refMax, 1e-12));
}

// Concurrency: many threads writing into the one shared grid lose no updates.
// A single-threaded atomic run is the ground-truth total (no concurrency, so
// nothing can be lost); the multi-threaded total must match it.
TEST(atomicgrid, atomic_large_grid_does_not_lose_updates_under_concurrency) {
    const std::uint64_t seed = 777u;
    const int rays = 250000;

    // threads == 1: no concurrent writers, so the total is the reference.
    const SimulationResult single = atomicPathProbe(512, rays, seed, 1u);
    // threads == 8: the whole point of the path -- concurrent atomic writes.
    const SimulationResult multi = atomicPathProbe(512, rays, seed, 8u);
    CHECK(single.nx * single.ny >= 65536);
    CHECK(multi.nx * multi.ny >= 65536);

    // A lost update would make the multi-threaded total measurably smaller;
    // the tolerance is far below a single ray's share, so any genuinely dropped
    // energy fails hard.
    CHECK_NEAR(multi.fluxDetector, single.fluxDetector,
               1e-7 * std::max(1.0, std::fabs(single.fluxDetector)));

    // And the per-bin totals agree too, compared directly.
    const double refMax = *std::max_element(single.irradiance.begin(),
                                            single.irradiance.end());
    for (std::size_t i = 0; i < single.irradiance.size(); ++i)
        CHECK_NEAR(single.irradiance[i], multi.irradiance[i],
                   1e-6 * std::max(refMax, 1e-12));
}
