// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "TraceScene.h"

#include <gp_Trsf.hxx>
#include <gp_Mat.hxx>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#endif

namespace {

// How many levels of the build may hand a subtree to another thread, and how
// big a subtree has to be to be worth the hand-off. Three levels is at most
// seven extra threads, which is the right order for a desktop machine and small
// enough that the bookkeeping never dominates.
//
// The threshold is 50 000 rather than a few thousand, and that is measured
// rather than guessed. Across the whole built-in library the largest single
// hierarchy is 22 000 triangles -- the ball lens and the integrating sphere --
// and the microlens array, which looks like the big one, is instanced: one
// lenslet is tessellated once and placed twenty-five times, so its hierarchy is
// small. At that size the build is about a millisecond, which is less than the
// thread spawns cost, and geometry latency is dominated by OCCT's tessellation
// rather than by the hierarchy at all: parallel and serial both measure
// 0.11 s on those scenes, repeatably.
//
// It is kept because the case the threshold is set for is real and is the one
// the app is sold on -- an imported CAD assembly of several hundred thousand
// triangles, where the build genuinely is the wait between opening a file and
// seeing it. Below the threshold the serial path runs, which is what the
// numbers above say it should.
constexpr int    kBuildSpawnDepth  = 3;
constexpr int    kBuildParallelMin = 50000;

constexpr int    kLeafSize    = 4;    // triangle count below which splitting stops paying off
constexpr int    kSahBins     = 16;   // buckets used by the SAH split search
constexpr int    kMaxBvhDepth = 60;   // traversal stack is sized from this
constexpr double kBoundsPad   = 1e-7; // absolute slack so grazing hits are not culled


// One entry of a traversal stack: which node, and how far along the ray its box
// begins.
//
// Storing the entry distance is what lets a node be tested when it is *pushed*
// rather than only when it is popped. Every node on the stack used to be a node
// that would be popped and slab-tested even after `best` had tightened past it;
// now a node whose box starts beyond the closest hit found since is skipped for
// the cost of one comparison. A well-established 10 to 20 per cent on the
// incoherent workloads a light guide produces.
struct StackEntry {
    int    node;
    double entry;
};

// Where along the ray a box begins, or false when the ray never enters it
// inside (tMin, tLimit). A template because the node type is private to
// TraceScene and this is used from inside it.
template <class NodeT>
inline bool slabEntry(const NodeT& n, const Vec3& o, const Vec3& invD,
                      double tMin, double tLimit, double& entry) {
    double t0 = tMin, t1 = tLimit;
    for (int a = 0; a < 3; ++a) {
        double lo = (n.bmin[a] - o[a]) * invD[a];
        double hi = (n.bmax[a] - o[a]) * invD[a];
        if (lo > hi) { const double tmp = lo; lo = hi; hi = tmp; }
        if (lo > t0) t0 = lo;
        if (hi < t1) t1 = hi;
        if (t0 > t1) return false;
    }
    entry = t0;
    return true;
}

inline double surfaceArea(const double bmin[3], const double bmax[3]) {
    const double dx = std::max(0.0, bmax[0] - bmin[0]);
    const double dy = std::max(0.0, bmax[1] - bmin[1]);
    const double dz = std::max(0.0, bmax[2] - bmin[2]);
    return 2.0 * (dx * dy + dy * dz + dz * dx);
}

} // namespace

bool intersectTriangle(const Vec3& o, const Vec3& d,
                       const Vec3& v0, const Vec3& e1, const Vec3& e2,
                       double& t, double& u, double& v) {
    const Vec3   p   = d.cross(e2);
    const double det = e1.dot(p);
    if (std::fabs(det) < 1e-12) return false;      // ray parallel to the triangle
    const double inv = 1.0 / det;

    const Vec3   s  = o - v0;
    const double uu = s.dot(p) * inv;
    constexpr double eps = 1e-9;
    if (uu < -eps || uu > 1.0 + eps) return false;

    const Vec3   q  = s.cross(e1);
    const double vv = d.dot(q) * inv;
    if (vv < -eps || uu + vv > 1.0 + eps) return false;

    const double tt = e2.dot(q) * inv;
    if (tt < 0.0) return false;

    t = tt; u = uu; v = vv;
    return true;
}

// ---- the wide leaf path ----------------------------------------------------

namespace simd {

// Probed here rather than in the kernel's own translation unit: that one is
// compiled for AVX2, and the check that decides whether AVX2 may be executed
// cannot live somewhere the compiler is free to use it.
bool avx2Available() {
    static const bool ok = [] {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        int info[4] = {0, 0, 0, 0};
        __cpuid(info, 0);
        if (info[0] < 7) return false;

        __cpuid(info, 1);
        const bool osxsave = (info[2] & (1 << 27)) != 0;
        const bool avx     = (info[2] & (1 << 28)) != 0;
        if (!osxsave || !avx) return false;
        // The OS has to have enabled saving the upper half of the register
        // file, or the instructions exist and the state does not survive a
        // context switch.
        const unsigned long long xcr0 = _xgetbv(0);
        if ((xcr0 & 0x6) != 0x6) return false;

        __cpuidex(info, 7, 0);
        return (info[1] & (1 << 5)) != 0;      // AVX2
#elif defined(__AVX2__)
        return true;
#else
        return false;
#endif
    }();
    return ok;
}

} // namespace simd

void TraceScene::clear() {
    m_tris.clear();
    m_surfs.clear();
    m_surfLabels.clear();
    m_nodes.clear();
    m_packed.clear();
    m_order.clear();
    m_centroids.clear();
    m_shade.clear();
    m_instances.clear();
    m_blas.clear();
    m_tlasOrder.clear();
    m_tlasNodes.clear();
    m_dets.clear();
    m_detOfSurf.clear();
    m_detOffset.clear();
    m_detCells = 0;
    m_boundsCenter = Vec3();
    m_boundsRadius = 0.0;
    m_det = DetectorInfo{};
    m_maxDepth = 0;
}

std::uint64_t TraceScene::surfaceIdentity(int i) const {
    if (i < 0 || i >= int(m_surfs.size())) return 0;
    // FNV-1a over the label, then the role bits folded in. Both halves matter:
    // the label says which surface this was meant to be, the role says the slot
    // still holds a surface of that character.
    std::uint64_t h = 1469598103934665603ull;
    const QByteArray bytes = surfaceLabel(i).toUtf8();
    for (char c : bytes) {
        h ^= std::uint64_t(std::uint8_t(c));
        h *= 1099511628211ull;
    }
    const SceneSurface& s = m_surfs[std::size_t(i)];
    std::uint64_t role = 0;
    if (s.isDetector)     role |= 1u;
    if (s.index > 0.0)    role |= 2u;
    if (s.material.isMetal()) role |= 4u;
    h ^= role + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

int TraceScene::resolveSurface(const QString& label, std::uint64_t identity, int hint) const {
    const int n = int(m_surfs.size());
    if (!label.isEmpty()) {
        int found = -1, matches = 0;
        for (int i = 0; i < n; ++i) {
            if (m_surfLabels[std::size_t(i)] != label) continue;
            ++matches;
            // An exact identity match settles an ambiguous label immediately.
            if (surfaceIdentity(i) == identity) return i;
            if (found < 0) found = i;
        }
        // One surface carries the name: that is the surface, whatever the
        // identity says about its role. A user who turned a mirror into a
        // window still means that mirror.
        if (matches == 1) return found;
        // Named, and either ambiguous or absent. Falling back to the stored
        // index here would be the precise failure this function exists to
        // prevent -- landing the edit on whatever now occupies the slot -- so
        // an override that names a surface the scene does not have resolves to
        // nothing and gets reported.
        return -1;
    }
    // No label to go on: an override from before overrides were named. The
    // stored index is a hint and nothing more, so it is taken only when the
    // surface sitting there is still the same kind of surface.
    if (hint >= 0 && hint < n && (identity == 0 || surfaceIdentity(hint) == identity))
        return hint;
    return -1;
}

void TraceScene::build(const MeshList& meshes) {
    clear();

    std::size_t total = 0;
    bool anyShading = false;
    for (const auto& m : meshes) {
        if (m.placements.size() > 1) continue;   // instanced: its own bottom level
        total += m.tris.size();
        if (!m.vnorm.empty()) anyShading = true;
    }
    for (const auto& m : meshes)
        if (m.placements.size() > 1 && !m.vnorm.empty()) anyShading = true;
    m_tris.reserve(total);
    m_surfs.reserve(meshes.size());
    if (anyShading) m_shade.reserve(total);

    // Gathered as the meshes are walked and turned into bottom levels at the
    // end, because building one takes over the member vectors the static
    // geometry is also accumulating into.
    struct PendingBlas {
        std::vector<SceneTri>   tris;
        std::vector<TriShading> shade;
        int                     surf = 0;
        const MeshSurface*      mesh = nullptr;
    };
    std::vector<PendingBlas> pending;

    for (const auto& m : meshes) {
        const int surfIdx = int(m_surfs.size());
        // MeshSurface derives from SurfaceOptics, so the whole optical
        // description slices across in one copy.
        m_surfs.push_back(static_cast<const SceneSurface&>(m));
        m_surfLabels.push_back(m.label);

        // A part placed more than once is tessellated once and traced through
        // its placements, so its triangles never enter the scene-wide list.
        if (m.placements.size() > 1) {
            PendingBlas pb;
            pb.surf = surfIdx;
            pb.mesh = &m;
            pb.tris.reserve(m.tris.size());
            if (anyShading) pb.shade.reserve(m.tris.size());
            for (const auto& tr : m.tris) {
                const Vec3 a(m.verts[std::size_t(tr.v0)]);
                const Vec3 b(m.verts[std::size_t(tr.v1)]);
                const Vec3 c(m.verts[std::size_t(tr.v2)]);
                SceneTri st;
                st.v0   = a;
                st.e1   = b - a;
                st.e2   = c - a;
                st.n    = Vec3(tr.normal);
                st.surf = surfIdx;
                pb.tris.push_back(st);
                if (anyShading) {
                    TriShading sh;
                    if (m.vnorm.size() == m.verts.size()) {
                        sh.n0 = Vec3(m.vnorm[std::size_t(tr.v0)]);
                        sh.n1 = Vec3(m.vnorm[std::size_t(tr.v1)]);
                        sh.n2 = Vec3(m.vnorm[std::size_t(tr.v2)]);
                    } else {
                        sh.n0 = sh.n1 = sh.n2 = st.n;
                    }
                    pb.shade.push_back(sh);
                }
            }
            pending.push_back(std::move(pb));
            continue;
        }

        for (const auto& tr : m.tris) {
            const Vec3 a(m.verts[std::size_t(tr.v0)]);
            const Vec3 b(m.verts[std::size_t(tr.v1)]);
            const Vec3 c(m.verts[std::size_t(tr.v2)]);
            SceneTri st;
            st.v0   = a;
            st.e1   = b - a;
            st.e2   = c - a;
            st.n    = Vec3(tr.normal);
            st.surf = surfIdx;
            m_tris.push_back(st);

            if (anyShading) {
                TriShading sh;
                if (m.vnorm.size() == m.verts.size()) {
                    sh.n0 = Vec3(m.vnorm[std::size_t(tr.v0)]);
                    sh.n1 = Vec3(m.vnorm[std::size_t(tr.v1)]);
                    sh.n2 = Vec3(m.vnorm[std::size_t(tr.v2)]);
                } else {
                    sh.n0 = sh.n1 = sh.n2 = st.n;
                }
                m_shade.push_back(sh);
            }
        }

        if (m.isDetector) {
            DetectorInfo d;
            d.valid  = true;
            d.center = Vec3(m.detCenter);
            d.u      = Vec3(m.detU);
            d.v      = Vec3(m.detV);
            d.n      = Vec3(m.detNormal);
            d.w      = m.detW;
            d.h      = m.detH;
            d.nx     = m.detNX > 0 ? m.detNX : 1;
            d.ny     = m.detNY > 0 ? m.detNY : 1;
            d.surf   = surfIdx;
            d.cosAcceptance = (m.detAcceptanceDeg >= 180.0)
                                  ? -1.0
                                  : std::cos(std::max(0.0, m.detAcceptanceDeg) * 3.14159265358979323846 / 180.0);
            d.rejectPasses = (m.detRejectMode == SurfaceOptics::RejectMode::Pass);
            m_dets.push_back(d);
            if (!m_det.valid) m_det = d;
        }
    }

    // Bottom levels first: each takes over the member vectors in turn, so the
    // static geometry's own hierarchy has to be the last one built.
    if (!pending.empty()) {
        std::vector<SceneTri>   staticTris  = std::move(m_tris);
        std::vector<TriShading> staticShade = std::move(m_shade);
        m_tris.clear();
        m_shade.clear();

        m_blas.reserve(pending.size());
        for (auto& pb : pending) {
            const int blasIndex = int(m_blas.size());
            m_tris  = std::move(pb.tris);
            m_shade = std::move(pb.shade);
            m_blas.emplace_back();
            buildBlasFromMembers(m_blas.back());

            // One placement each, with its bounds in world space.
            const Blas& bl = m_blas[std::size_t(blasIndex)];
            for (const auto& t : pb.mesh->placements) {
                Instance inst;
                inst.blas = blasIndex;
                inst.surf = pb.surf;
                const gp_Mat r = t.VectorialPart();
                inst.r0 = Vec3(r.Value(1, 1), r.Value(1, 2), r.Value(1, 3));
                inst.r1 = Vec3(r.Value(2, 1), r.Value(2, 2), r.Value(2, 3));
                inst.r2 = Vec3(r.Value(3, 1), r.Value(3, 2), r.Value(3, 3));
                const gp_XYZ tr = t.TranslationPart();
                inst.origin = Vec3(tr.X(), tr.Y(), tr.Z());
                // Rigid, so the inverse rotation is the transpose.
                inst.i0 = Vec3(inst.r0.x, inst.r1.x, inst.r2.x);
                inst.i1 = Vec3(inst.r0.y, inst.r1.y, inst.r2.y);
                inst.i2 = Vec3(inst.r0.z, inst.r1.z, inst.r2.z);

                for (int a = 0; a < 3; ++a) { inst.bmin[a] = DBL_MAX; inst.bmax[a] = -DBL_MAX; }
                for (const auto& st : bl.tris) {
                    const Vec3 corners[3] = {st.v0, st.v0 + st.e1, st.v0 + st.e2};
                    for (const Vec3& c : corners) {
                        const Vec3 w = inst.dirToWorld(c) + inst.origin;
                        for (int a = 0; a < 3; ++a) {
                            inst.bmin[a] = std::min(inst.bmin[a], w[a]);
                            inst.bmax[a] = std::max(inst.bmax[a], w[a]);
                        }
                    }
                }
                for (int a = 0; a < 3; ++a) { inst.bmin[a] -= kBoundsPad; inst.bmax[a] += kBoundsPad; }
                m_instances.push_back(inst);
            }
        }

        m_tris  = std::move(staticTris);
        m_shade = std::move(staticShade);

        m_tlasOrder.resize(m_instances.size());
        for (std::size_t i = 0; i < m_instances.size(); ++i) m_tlasOrder[i] = int(i);
        m_tlasNodes.reserve(2 * m_instances.size() + 1);
        if (!m_instances.empty()) buildTlasNode(0, int(m_instances.size()), 0);
    }

    m_detOfSurf.assign(m_surfs.size(), -1);
    m_detOffset.assign(m_dets.size(), 0);
    for (std::size_t i = 0; i < m_dets.size(); ++i) {
        const int si = m_dets[i].surf;
        if (si >= 0 && si < int(m_detOfSurf.size())) m_detOfSurf[std::size_t(si)] = int(i);
        m_detOffset[i] = m_detCells;
        m_detCells += std::size_t(m_dets[i].nx) * std::size_t(m_dets[i].ny);
    }

    if (m_tris.empty()) return;

    m_order.resize(m_tris.size());
    m_centroids.resize(m_tris.size());
    for (std::size_t i = 0; i < m_tris.size(); ++i) {
        m_order[i] = int(i);
        const SceneTri& t = m_tris[i];
        // (v0 + v1 + v2) / 3, expressed through the stored edges.
        m_centroids[i] = t.v0 + (t.e1 + t.e2) * (1.0 / 3.0);
    }

    m_nodes.reserve(2 * (m_tris.size() / kLeafSize + 1));
    buildNode(0, int(m_tris.size()), 0);
    packLeaves(m_nodes, m_order, m_tris, m_packed);

    // The scene's extent, over the static hierarchy and every placement, since
    // emission aiming has to be able to promise that a direction outside it hits
    // nothing at all.
    double lo[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, hi[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    bool any = false;
    if (!m_nodes.empty()) {
        const BvhNode& root = m_nodes[0];
        for (int a = 0; a < 3; ++a) { lo[a] = root.bmin[a]; hi[a] = root.bmax[a]; }
        any = true;
    }
    for (const auto& inst : m_instances) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], inst.bmin[a]);
            hi[a] = std::max(hi[a], inst.bmax[a]);
        }
        any = true;
    }
    if (any) {
        double c[3], half = 0.0;
        for (int a = 0; a < 3; ++a) {
            c[a] = 0.5 * (lo[a] + hi[a]);
            const double h = 0.5 * (hi[a] - lo[a]);
            half += h * h;
        }
        m_boundsCenter = Vec3(c[0], c[1], c[2]);
        // The sphere circumscribing that box. Larger than the tightest sphere
        // over the triangles, and conservative in the direction that matters:
        // no direction that could hit anything is left outside it.
        m_boundsRadius = std::sqrt(half);
    }
}

std::size_t TraceScene::instancedTriangles() const {
    std::size_t n = 0;
    for (const auto& b : m_blas) n += b.tris.size();
    return n;
}

void TraceScene::buildBlasFromMembers(Blas& out) {
    m_nodes.clear();
    m_order.resize(m_tris.size());
    m_centroids.resize(m_tris.size());
    for (std::size_t i = 0; i < m_tris.size(); ++i) {
        m_order[i] = int(i);
        const SceneTri& t = m_tris[i];
        m_centroids[i] = t.v0 + (t.e1 + t.e2) * (1.0 / 3.0);
    }
    const int savedDepth = m_maxDepth;
    m_maxDepth = 0;
    m_nodes.reserve(2 * (m_tris.size() / kLeafSize + 1));
    if (!m_tris.empty()) buildNode(0, int(m_tris.size()), 0);

    out.tris     = std::move(m_tris);
    out.shade    = std::move(m_shade);
    out.order    = std::move(m_order);
    out.nodes    = std::move(m_nodes);
    out.maxDepth = m_maxDepth;
    packLeaves(out.nodes, out.order, out.tris, out.packed);

    m_tris.clear();
    m_shade.clear();
    m_order.clear();
    m_nodes.clear();
    m_centroids.clear();
    m_maxDepth = std::max(savedDepth, out.maxDepth);
}

int TraceScene::buildTlasNode(int start, int count, int depth) {
    const int self = int(m_tlasNodes.size());
    m_tlasNodes.emplace_back();

    double bmin[3] = {DBL_MAX, DBL_MAX, DBL_MAX};
    double bmax[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int i = start; i < start + count; ++i) {
        const Instance& in = m_instances[std::size_t(m_tlasOrder[std::size_t(i)])];
        for (int a = 0; a < 3; ++a) {
            bmin[a] = std::min(bmin[a], in.bmin[a]);
            bmax[a] = std::max(bmax[a], in.bmax[a]);
        }
    }
    {
        BvhNode& n = m_tlasNodes[std::size_t(self)];
        for (int a = 0; a < 3; ++a) { n.bmin[a] = bmin[a]; n.bmax[a] = bmax[a]; }
    }

    // A handful of placements is not worth a surface-area search: split at the
    // median of the widest axis and stop at two per leaf.
    if (count <= 2 || depth >= kMaxBvhDepth) {
        BvhNode& n = m_tlasNodes[std::size_t(self)];
        n.left = start; n.right = 0; n.count = count; n.axis = 0;
        return self;
    }

    int axis = 0;
    for (int a = 1; a < 3; ++a)
        if (bmax[a] - bmin[a] > bmax[axis] - bmin[axis]) axis = a;
    const auto mid = m_tlasOrder.begin() + start + count / 2;
    std::nth_element(m_tlasOrder.begin() + start, mid, m_tlasOrder.begin() + start + count,
                     [&](int a, int b) {
                         const Instance& ia = m_instances[std::size_t(a)];
                         const Instance& ib = m_instances[std::size_t(b)];
                         return (ia.bmin[axis] + ia.bmax[axis]) <
                                (ib.bmin[axis] + ib.bmax[axis]);
                     });
    const int leftN = count / 2;
    const int l = buildTlasNode(start, leftN, depth + 1);
    const int r = buildTlasNode(start + leftN, count - leftN, depth + 1);

    BvhNode& n = m_tlasNodes[std::size_t(self)];
    n.left = l; n.right = r; n.count = 0; n.axis = axis;
    return self;
}

bool TraceScene::hitInstances(const Vec3& o, const Vec3& d, RayHit& hit, double tMin,
                              double& best) const {
    if (m_tlasNodes.empty()) return false;
    const Vec3 invD(1.0 / d.x, 1.0 / d.y, 1.0 / d.z);
    bool found = false;

    int stack[kMaxBvhDepth + 2];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const BvhNode& n = m_tlasNodes[std::size_t(stack[--sp])];
        double t0 = tMin, t1 = best;
        bool miss = false;
        for (int a = 0; a < 3; ++a) {
            double loT = (n.bmin[a] - o[a]) * invD[a];
            double hiT = (n.bmax[a] - o[a]) * invD[a];
            if (loT > hiT) std::swap(loT, hiT);
            if (loT > t0) t0 = loT;
            if (hiT < t1) t1 = hiT;
            if (t0 > t1) { miss = true; break; }
        }
        if (miss) continue;

        if (n.count > 0) {
            for (int i = n.left; i < n.left + n.count; ++i) {
                const int ii = m_tlasOrder[std::size_t(i)];
                const Instance& in = m_instances[std::size_t(ii)];
                // The placement's own box first: transforming a ray that misses
                // it would be work spent to find that out.
                double e0 = tMin, e1 = best;
                bool skip = false;
                for (int a = 0; a < 3; ++a) {
                    double loT = (in.bmin[a] - o[a]) * invD[a];
                    double hiT = (in.bmax[a] - o[a]) * invD[a];
                    if (loT > hiT) std::swap(loT, hiT);
                    if (loT > e0) e0 = loT;
                    if (hiT < e1) e1 = hiT;
                    if (e0 > e1) { skip = true; break; }
                }
                if (skip) continue;

                // Into the part's own frame. The transform is rigid, so a
                // distance means the same thing on both sides of it and `best`
                // carries across untouched.
                const Vec3 lo2 = in.toLocal(o);
                const Vec3 ld  = in.dirToLocal(d);
                const Blas& bl = m_blas[std::size_t(in.blas)];
                if (bl.nodes.empty()) continue;

                const Vec3 linv(1.0 / ld.x, 1.0 / ld.y, 1.0 / ld.z);
                int  bstack[kMaxBvhDepth + 2];
                int  bsp = 0;
                bstack[bsp++] = 0;
                while (bsp > 0) {
                    const BvhNode& bn = bl.nodes[std::size_t(bstack[--bsp])];
                    double u0 = tMin, u1 = best;
                    bool bmiss = false;
                    for (int a = 0; a < 3; ++a) {
                        double loT = (bn.bmin[a] - lo2[a]) * linv[a];
                        double hiT = (bn.bmax[a] - lo2[a]) * linv[a];
                        if (loT > hiT) std::swap(loT, hiT);
                        if (loT > u0) u0 = loT;
                        if (hiT < u1) u1 = hiT;
                        if (u0 > u1) { bmiss = true; break; }
                    }
                    if (bmiss) continue;
                    if (bn.count > 0) {
                        for (int k = bn.left; k < bn.left + bn.count; ++k) {
                            const int ti = bl.order[std::size_t(k)];
                            const SceneTri& tr = bl.tris[std::size_t(ti)];
                            double t = 0.0, u = 0.0, v = 0.0;
                            if (intersectTriangle(lo2, ld, tr.v0, tr.e1, tr.e2, t, u, v) &&
                                t > tMin && t < best) {
                                best = t;
                                hit.tri  = ti;
                                hit.inst = ii;
                                hit.t    = t;
                                hit.u    = u;
                                hit.v    = v;
                                found    = true;
                            }
                        }
                    } else if (bsp + 1 < int(sizeof(bstack) / sizeof(bstack[0]))) {
                        if (ld[bn.axis] < 0.0) { bstack[bsp++] = bn.left;  bstack[bsp++] = bn.right; }
                        else                   { bstack[bsp++] = bn.right; bstack[bsp++] = bn.left;  }
                    }
                }
            }
        } else if (sp + 1 < int(sizeof(stack) / sizeof(stack[0]))) {
            stack[sp++] = n.right;
            stack[sp++] = n.left;
        }
    }
    return found;
}

void TraceScene::boundsOf(int start, int count, double bmin[3], double bmax[3]) const {
    for (int a = 0; a < 3; ++a) { bmin[a] = DBL_MAX; bmax[a] = -DBL_MAX; }
    for (int i = start; i < start + count; ++i) {
        const SceneTri& t = m_tris[std::size_t(m_order[std::size_t(i)])];
        const Vec3 v1 = t.v0 + t.e1;
        const Vec3 v2 = t.v0 + t.e2;
        for (int a = 0; a < 3; ++a) {
            bmin[a] = std::min(std::min(bmin[a], t.v0[a]), std::min(v1[a], v2[a]));
            bmax[a] = std::max(std::max(bmax[a], t.v0[a]), std::max(v1[a], v2[a]));
        }
    }
    for (int a = 0; a < 3; ++a) { bmin[a] -= kBoundsPad; bmax[a] += kBoundsPad; }
}

void TraceScene::packLeaves(std::vector<BvhNode>& nodes, const std::vector<int>& order,
                            const std::vector<SceneTri>& tris,
                            std::vector<simd::Tri4>& packed) {
    packed.clear();
    if (!simd::avx2Available()) {
        // No wide path on this machine: mark every leaf scalar and spend
        // nothing on a layout that will not be read.
        for (BvhNode& n : nodes)
            if (n.count > 0) n.right = -1;
        return;
    }

    std::size_t groups = 0;
    for (const BvhNode& n : nodes)
        if (n.count > 0) groups += std::size_t(n.count / 4);
    packed.reserve(groups);

    for (BvhNode& n : nodes) {
        if (n.count <= 0) continue;
        const int whole = n.count / 4;
        if (whole == 0) { n.right = -1; continue; }

        n.right = int(packed.size());
        for (int g = 0; g < whole; ++g) {
            simd::Tri4 rec;
            for (int lane = 0; lane < 4; ++lane) {
                const int ti = order[std::size_t(n.left + g * 4 + lane)];
                const SceneTri& tr = tris[std::size_t(ti)];
                for (int a = 0; a < 3; ++a) {
                    rec.v0[a][lane] = tr.v0[a];
                    rec.e1[a][lane] = tr.e1[a];
                    rec.e2[a][lane] = tr.e2[a];
                }
                rec.tri[lane] = ti;
            }
            packed.push_back(rec);
        }
    }
}

void TraceScene::spliceSubtree(std::vector<BvhNode>& dst, const std::vector<BvhNode>& src) {
    const int base = int(dst.size());
    dst.reserve(dst.size() + src.size());
    for (const BvhNode& n : src) {
        BvhNode copy = n;
        if (copy.count == 0) { copy.left += base; copy.right += base; }
        dst.push_back(copy);
    }
}

int TraceScene::buildNode(int start, int count, int depth) {
    int maxDepth = m_maxDepth;
    const int root = buildSubtree(m_nodes, start, count, depth, maxDepth,
                                  kBuildSpawnDepth);
    m_maxDepth = std::max(m_maxDepth, maxDepth);
    return root;
}

int TraceScene::buildSubtree(std::vector<BvhNode>& out, int start, int count, int depth,
                             int& maxDepth, int threadBudget) {
    const int self = int(out.size());
    out.emplace_back();
    maxDepth = std::max(maxDepth, depth);

    double bmin[3], bmax[3];
    boundsOf(start, count, bmin, bmax);

    auto storeBounds = [&]() {
        BvhNode& n = out[std::size_t(self)];
        for (int a = 0; a < 3; ++a) { n.bmin[a] = bmin[a]; n.bmax[a] = bmax[a]; }
    };
    auto makeLeaf = [&]() {
        storeBounds();
        BvhNode& n = out[std::size_t(self)];
        n.left  = start;
        n.right = 0;
        n.count = count;
        n.axis  = 0;
        return self;
    };

    if (count <= kLeafSize || depth >= kMaxBvhDepth) return makeLeaf();

    // Split along the widest extent of the centroids.
    double cmin[3] = {DBL_MAX, DBL_MAX, DBL_MAX};
    double cmax[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int i = start; i < start + count; ++i) {
        const Vec3& c = m_centroids[std::size_t(m_order[std::size_t(i)])];
        for (int a = 0; a < 3; ++a) {
            cmin[a] = std::min(cmin[a], c[a]);
            cmax[a] = std::max(cmax[a], c[a]);
        }
    }
    int axis = 0;
    for (int a = 1; a < 3; ++a)
        if (cmax[a] - cmin[a] > cmax[axis] - cmin[axis]) axis = a;
    const double extent = cmax[axis] - cmin[axis];
    if (extent < 1e-12) return makeLeaf();   // every centroid coincides

    // Binned surface-area heuristic.
    struct Bin {
        int    n = 0;
        double bmin[3] = {DBL_MAX, DBL_MAX, DBL_MAX};
        double bmax[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
        void add(const SceneTri& t) {
            const Vec3 v1 = t.v0 + t.e1;
            const Vec3 v2 = t.v0 + t.e2;
            for (int a = 0; a < 3; ++a) {
                bmin[a] = std::min(std::min(bmin[a], t.v0[a]), std::min(v1[a], v2[a]));
                bmax[a] = std::max(std::max(bmax[a], t.v0[a]), std::max(v1[a], v2[a]));
            }
            ++n;
        }
    };
    Bin bins[kSahBins];
    const double scale = double(kSahBins) / extent;
    auto binOf = [&](int triIndex) {
        const double c = m_centroids[std::size_t(triIndex)][axis];
        return std::min(kSahBins - 1, std::max(0, int((c - cmin[axis]) * scale)));
    };
    for (int i = start; i < start + count; ++i) {
        const int ti = m_order[std::size_t(i)];
        bins[binOf(ti)].add(m_tris[std::size_t(ti)]);
    }

    // Sweep from both sides for the cost of every candidate split plane.
    double leftArea[kSahBins]   = {};
    double rightArea[kSahBins]  = {};
    int    leftCount[kSahBins]  = {};
    int    rightCount[kSahBins] = {};
    {
        double bmn[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, bmx[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
        int acc = 0;
        for (int i = 0; i < kSahBins; ++i) {
            if (bins[i].n) {
                for (int a = 0; a < 3; ++a) {
                    bmn[a] = std::min(bmn[a], bins[i].bmin[a]);
                    bmx[a] = std::max(bmx[a], bins[i].bmax[a]);
                }
                acc += bins[i].n;
            }
            leftCount[i] = acc;
            leftArea[i]  = acc ? surfaceArea(bmn, bmx) : 0.0;
        }
    }
    {
        double bmn[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, bmx[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
        int acc = 0;
        for (int i = kSahBins - 1; i >= 0; --i) {
            if (bins[i].n) {
                for (int a = 0; a < 3; ++a) {
                    bmn[a] = std::min(bmn[a], bins[i].bmin[a]);
                    bmx[a] = std::max(bmx[a], bins[i].bmax[a]);
                }
                acc += bins[i].n;
            }
            rightCount[i] = acc;
            rightArea[i]  = acc ? surfaceArea(bmn, bmx) : 0.0;
        }
    }

    const double parentArea = std::max(surfaceArea(bmin, bmax), 1e-12);
    const double leafCost   = double(count);
    double bestCost = DBL_MAX;
    int    bestBin  = -1;
    for (int i = 0; i < kSahBins - 1; ++i) {
        if (leftCount[i] == 0 || rightCount[i + 1] == 0) continue;
        const double cost = 0.5 + (leftArea[i] * leftCount[i] +
                                   rightArea[i + 1] * rightCount[i + 1]) / parentArea;
        if (cost < bestCost) { bestCost = cost; bestBin = i; }
    }
    if (bestBin < 0 || bestCost >= leafCost) return makeLeaf();

    const auto mid = std::partition(m_order.begin() + start,
                                    m_order.begin() + start + count,
                                    [&](int ti) { return binOf(ti) <= bestBin; });
    const int leftN = int(mid - (m_order.begin() + start));
    if (leftN == 0 || leftN == count) return makeLeaf();

    int leftChild = 0, rightChild = 0;
    if (threadBudget > 0 && count >= kBuildParallelMin) {
        // The two halves index disjoint ranges of the primitive order and read
        // everything else, so they are independent from here. Each builds into
        // its own node vector and the two are spliced in the order the serial
        // recursion would have produced them, which is what keeps the layout --
        // and therefore the answer -- identical.
        std::vector<BvhNode> leftNodes, rightNodes;
        int leftDepth = depth + 1, rightDepth = depth + 1;

        std::thread worker([&] {
            buildSubtree(rightNodes, start + leftN, count - leftN, depth + 1,
                         rightDepth, threadBudget - 1);
        });
        buildSubtree(leftNodes, start, leftN, depth + 1, leftDepth, threadBudget - 1);
        worker.join();

        leftChild = int(out.size());
        spliceSubtree(out, leftNodes);
        rightChild = int(out.size());
        spliceSubtree(out, rightNodes);
        maxDepth = std::max(maxDepth, std::max(leftDepth, rightDepth));
    } else {
        leftChild  = buildSubtree(out, start, leftN, depth + 1, maxDepth, 0);
        rightChild = buildSubtree(out, start + leftN, count - leftN, depth + 1,
                                  maxDepth, 0);
    }

    storeBounds();
    BvhNode& n = out[std::size_t(self)];
    n.left  = leftChild;
    n.right = rightChild;
    n.count = 0;
    n.axis  = axis;
    return self;
}

bool TraceScene::nearestHitStatic(const Vec3& o, const Vec3& d, RayHit& hit,
                                  double tMin) const {
    hit = RayHit{};
    if (m_nodes.empty()) return false;

    const Vec3 invD(1.0 / d.x, 1.0 / d.y, 1.0 / d.z);
    double best    = DBL_MAX;
    int    bestTri = -1;
    double bestU   = 0.0, bestV = 0.0;

    // Each interior node pushes both children, so the stack never needs more
    // than one entry per level plus the level being expanded.
    StackEntry stack[kMaxBvhDepth + 2];
    int sp = 0;
    {
        // The root box is tested here for the same reason every other node is
        // tested at push time: a ray that misses the whole scene should learn
        // it from one slab test.
        double e = tMin;
        if (slabEntry(m_nodes[0], o, invD, tMin, best, e)) stack[sp++] = {0, e};
    }

    while (sp > 0) {
        const StackEntry top = stack[--sp];
        // The box was tested when it was pushed. `best` may have tightened past
        // it since, and one comparison is cheaper than re-running the slab test
        // to find that out.
        if (top.entry >= best) continue;
        const BvhNode& n = m_nodes[std::size_t(top.node)];

        if (n.count > 0) {
            intersectLeaf(n, m_order, m_tris, m_packed, o, d, tMin,
                          best, bestTri, bestU, bestV);
            continue;
        }

        // Test both children here rather than on the way back out, and push only
        // the ones the ray actually enters. The near one is pushed last so it is
        // popped first and tightens `best` as early as possible.
        double eNear = 0.0, eFar = 0.0;
        const int nearChild = (d[n.axis] < 0.0) ? n.right : n.left;
        const int farChild  = (d[n.axis] < 0.0) ? n.left  : n.right;
        const bool hitNear = slabEntry(m_nodes[std::size_t(nearChild)], o, invD,
                                       tMin, best, eNear);
        const bool hitFar  = slabEntry(m_nodes[std::size_t(farChild)], o, invD,
                                       tMin, best, eFar);
        if (hitFar)  stack[sp++] = {farChild,  eFar};
        if (hitNear) stack[sp++] = {nearChild, eNear};
    }

    if (bestTri < 0) return false;
    hit.tri  = bestTri;
    hit.inst = -1;
    hit.t    = best;
    hit.u    = bestU;
    hit.v    = bestV;
    return true;
}

bool TraceScene::occluded(const Vec3& o, const Vec3& d, double tMax, int ignoreSurface,
                          double tMin) const {
    if (!(tMax > tMin)) return false;
    const Vec3 invD(1.0 / d.x, 1.0 / d.y, 1.0 / d.z);

    // The scene-wide hierarchy. No `best` to tighten and no nearest to keep:
    // the first triangle in the way is the whole answer, so the walk returns
    // from inside the leaf loop rather than running to completion.
    if (!m_nodes.empty()) {
        StackEntry stack[kMaxBvhDepth + 2];
        int sp = 0;
        {
            double e = tMin;
            if (slabEntry(m_nodes[0], o, invD, tMin, tMax, e)) stack[sp++] = {0, e};
        }
        while (sp > 0) {
            const BvhNode& n = m_nodes[std::size_t(stack[--sp].node)];
            if (n.count > 0) {
                for (int i = n.left; i < n.left + n.count; ++i) {
                    const int ti = m_order[std::size_t(i)];
                    const SceneTri& tr = m_tris[std::size_t(ti)];
                    if (tr.surf == ignoreSurface) continue;
                    double t = 0.0, u = 0.0, v = 0.0;
                    if (intersectTriangle(o, d, tr.v0, tr.e1, tr.e2, t, u, v) &&
                        t > tMin && t < tMax)
                        return true;
                }
                continue;
            }
            // No near/far ordering: without a nearest hit to tighten, which
            // child is visited first changes nothing but the order of the
            // answer, and the answer is a bool.
            double e = tMin;
            if (slabEntry(m_nodes[std::size_t(n.left)], o, invD, tMin, tMax, e))
                stack[sp++] = {n.left, e};
            if (slabEntry(m_nodes[std::size_t(n.right)], o, invD, tMin, tMax, e))
                stack[sp++] = {n.right, e};
        }
    }

    // The placements.
    if (m_tlasNodes.empty()) return false;
    StackEntry stack[kMaxBvhDepth + 2];
    int sp = 0;
    {
        double e = tMin;
        if (slabEntry(m_tlasNodes[0], o, invD, tMin, tMax, e)) stack[sp++] = {0, e};
    }
    while (sp > 0) {
        const BvhNode& n = m_tlasNodes[std::size_t(stack[--sp].node)];
        if (n.count == 0) {
            double e = tMin;
            if (slabEntry(m_tlasNodes[std::size_t(n.left)], o, invD, tMin, tMax, e))
                stack[sp++] = {n.left, e};
            if (slabEntry(m_tlasNodes[std::size_t(n.right)], o, invD, tMin, tMax, e))
                stack[sp++] = {n.right, e};
            continue;
        }
        for (int i = n.left; i < n.left + n.count; ++i) {
            const int ii = m_tlasOrder[std::size_t(i)];
            const Instance& in = m_instances[std::size_t(ii)];
            if (in.surf == ignoreSurface) continue;

            double e = tMin;
            struct Box { const double* bmin; const double* bmax; } box{in.bmin, in.bmax};
            struct BoxNode { double bmin[3], bmax[3]; } bn;
            for (int a = 0; a < 3; ++a) { bn.bmin[a] = box.bmin[a]; bn.bmax[a] = box.bmax[a]; }
            if (!slabEntry(bn, o, invD, tMin, tMax, e)) continue;

            const Vec3  lo2 = in.toLocal(o);
            const Vec3  ld  = in.dirToLocal(d);
            const Blas& bl  = m_blas[std::size_t(in.blas)];
            if (bl.nodes.empty()) continue;

            const Vec3 linv(1.0 / ld.x, 1.0 / ld.y, 1.0 / ld.z);
            StackEntry bstack[kMaxBvhDepth + 2];
            int bsp = 0;
            {
                double be = tMin;
                if (slabEntry(bl.nodes[0], lo2, linv, tMin, tMax, be))
                    bstack[bsp++] = {0, be};
            }
            while (bsp > 0) {
                const BvhNode& b = bl.nodes[std::size_t(bstack[--bsp].node)];
                if (b.count > 0) {
                    for (int k = b.left; k < b.left + b.count; ++k) {
                        const int ti = bl.order[std::size_t(k)];
                        const SceneTri& tr = bl.tris[std::size_t(ti)];
                        double t = 0.0, u = 0.0, v = 0.0;
                        if (intersectTriangle(lo2, ld, tr.v0, tr.e1, tr.e2, t, u, v) &&
                            t > tMin && t < tMax)
                            return true;
                    }
                    continue;
                }
                double be = tMin;
                if (bsp + 2 >= int(sizeof(bstack) / sizeof(bstack[0]))) continue;
                if (slabEntry(bl.nodes[std::size_t(b.left)], lo2, linv, tMin, tMax, be))
                    bstack[bsp++] = {b.left, be};
                if (slabEntry(bl.nodes[std::size_t(b.right)], lo2, linv, tMin, tMax, be))
                    bstack[bsp++] = {b.right, be};
            }
        }
    }
    return false;
}

bool TraceScene::nearestHitInstanced(const Vec3& o, const Vec3& d, RayHit& hit,
                                     double tMin) const {
    hit = RayHit{};
    double best = DBL_MAX;

    // The placements first: they tighten `best` for the walk over the rest, and
    // a scene that is entirely instanced has no such walk to do.
    const bool instHit = hitInstances(o, d, hit, tMin, best);
    if (m_nodes.empty()) return instHit;

    const Vec3 invD(1.0 / d.x, 1.0 / d.y, 1.0 / d.z);
    int    bestTri = -1;
    double bestU   = 0.0, bestV = 0.0;

    StackEntry stack[kMaxBvhDepth + 2];
    int sp = 0;
    {
        // The root box is tested here for the same reason every other node is
        // tested at push time: a ray that misses the whole scene should learn
        // it from one slab test.
        double e = tMin;
        if (slabEntry(m_nodes[0], o, invD, tMin, best, e)) stack[sp++] = {0, e};
    }

    while (sp > 0) {
        const StackEntry top = stack[--sp];
        if (top.entry >= best) continue;
        const BvhNode& n = m_nodes[std::size_t(top.node)];

        if (n.count > 0) {
            intersectLeaf(n, m_order, m_tris, m_packed, o, d, tMin,
                          best, bestTri, bestU, bestV);
            continue;
        }

        double eNear = 0.0, eFar = 0.0;
        const int nearChild = (d[n.axis] < 0.0) ? n.right : n.left;
        const int farChild  = (d[n.axis] < 0.0) ? n.left  : n.right;
        const bool hitNear = slabEntry(m_nodes[std::size_t(nearChild)], o, invD,
                                       tMin, best, eNear);
        const bool hitFar  = slabEntry(m_nodes[std::size_t(farChild)], o, invD,
                                       tMin, best, eFar);
        if (hitFar)  stack[sp++] = {farChild,  eFar};
        if (hitNear) stack[sp++] = {nearChild, eNear};
    }

    if (bestTri < 0) return instHit;
    hit.tri  = bestTri;
    hit.inst = -1;
    hit.t    = best;
    hit.u    = bestU;
    hit.v    = bestV;
    return true;
}

bool TraceScene::nearestHitBruteForce(const Vec3& o, const Vec3& d, RayHit& hit, double tMin) const {
    hit = RayHit{};
    double best    = DBL_MAX;
    int    bestTri = -1;
    int    bestInst = -1;
    double bestU   = 0.0, bestV = 0.0;
    for (std::size_t i = 0; i < m_tris.size(); ++i) {
        const SceneTri& tr = m_tris[i];
        double t = 0.0, u = 0.0, v = 0.0;
        if (intersectTriangle(o, d, tr.v0, tr.e1, tr.e2, t, u, v) && t > tMin && t < best) {
            best = t; bestTri = int(i); bestU = u; bestV = v;
        }
    }
    // Every triangle of every placement, so the reference really is exhaustive:
    // a hierarchy is only worth trusting against something that skips nothing.
    for (std::size_t ii = 0; ii < m_instances.size(); ++ii) {
        const Instance& in = m_instances[ii];
        const Blas& bl = m_blas[std::size_t(in.blas)];
        const Vec3 lo = in.toLocal(o);
        const Vec3 ld = in.dirToLocal(d);
        for (std::size_t i = 0; i < bl.tris.size(); ++i) {
            const SceneTri& tr = bl.tris[i];
            double t = 0.0, u = 0.0, v = 0.0;
            if (intersectTriangle(lo, ld, tr.v0, tr.e1, tr.e2, t, u, v) && t > tMin && t < best) {
                best = t; bestTri = int(i); bestInst = int(ii); bestU = u; bestV = v;
            }
        }
    }
    if (bestTri < 0) return false;
    hit.tri  = bestTri;
    hit.inst = bestInst;
    hit.t    = best;
    hit.u    = bestU;
    hit.v    = bestV;
    return true;
}
