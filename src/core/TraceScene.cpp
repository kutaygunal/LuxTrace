#include "TraceScene.h"

#include <gp_Trsf.hxx>
#include <gp_Mat.hxx>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {

constexpr int    kLeafSize    = 4;    // triangle count below which splitting stops paying off
constexpr int    kSahBins     = 16;   // buckets used by the SAH split search
constexpr int    kMaxBvhDepth = 60;   // traversal stack is sized from this
constexpr double kBoundsPad   = 1e-7; // absolute slack so grazing hits are not culled


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

void TraceScene::clear() {
    m_tris.clear();
    m_surfs.clear();
    m_nodes.clear();
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

int TraceScene::buildNode(int start, int count, int depth) {
    const int self = int(m_nodes.size());
    m_nodes.emplace_back();
    m_maxDepth = std::max(m_maxDepth, depth);

    double bmin[3], bmax[3];
    boundsOf(start, count, bmin, bmax);

    auto storeBounds = [&]() {
        BvhNode& n = m_nodes[std::size_t(self)];
        for (int a = 0; a < 3; ++a) { n.bmin[a] = bmin[a]; n.bmax[a] = bmax[a]; }
    };
    auto makeLeaf = [&]() {
        storeBounds();
        BvhNode& n = m_nodes[std::size_t(self)];
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

    const int leftChild  = buildNode(start, leftN, depth + 1);
    const int rightChild = buildNode(start + leftN, count - leftN, depth + 1);

    storeBounds();
    BvhNode& n = m_nodes[std::size_t(self)];
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
    int stack[kMaxBvhDepth + 2];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const BvhNode& n = m_nodes[std::size_t(stack[--sp])];

        // Slab test against the node box, clipped to the best hit so far.
        double t0 = tMin, t1 = best;
        bool   miss = false;
        for (int a = 0; a < 3; ++a) {
            double lo = (n.bmin[a] - o[a]) * invD[a];
            double hi = (n.bmax[a] - o[a]) * invD[a];
            if (lo > hi) { const double tmp = lo; lo = hi; hi = tmp; }
            if (lo > t0) t0 = lo;
            if (hi < t1) t1 = hi;
            if (t0 > t1) { miss = true; break; }
        }
        if (miss) continue;

        if (n.count > 0) {
            for (int i = n.left; i < n.left + n.count; ++i) {
                const int ti = m_order[std::size_t(i)];
                const SceneTri& tr = m_tris[std::size_t(ti)];
                double t = 0.0, u = 0.0, v = 0.0;
                if (intersectTriangle(o, d, tr.v0, tr.e1, tr.e2, t, u, v) &&
                    t > tMin && t < best) {
                    best = t; bestTri = ti; bestU = u; bestV = v;
                }
            }
        } else {
            // Push the far child first so the near one is popped and tested
            // first, tightening `best` as early as possible.
            if (d[n.axis] < 0.0) { stack[sp++] = n.left;  stack[sp++] = n.right; }
            else                 { stack[sp++] = n.right; stack[sp++] = n.left;  }
        }
    }

    if (bestTri < 0) return false;
    hit.tri  = bestTri;
    hit.inst = -1;
    hit.t    = best;
    hit.u    = bestU;
    hit.v    = bestV;
    return true;
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

    int stack[kMaxBvhDepth + 2];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const BvhNode& n = m_nodes[std::size_t(stack[--sp])];
        double t0 = tMin, t1 = best;
        bool   miss = false;
        for (int a = 0; a < 3; ++a) {
            double lo = (n.bmin[a] - o[a]) * invD[a];
            double hi = (n.bmax[a] - o[a]) * invD[a];
            if (lo > hi) { const double tmp = lo; lo = hi; hi = tmp; }
            if (lo > t0) t0 = lo;
            if (hi < t1) t1 = hi;
            if (t0 > t1) { miss = true; break; }
        }
        if (miss) continue;

        if (n.count > 0) {
            for (int i = n.left; i < n.left + n.count; ++i) {
                const int ti = m_order[std::size_t(i)];
                const SceneTri& tr = m_tris[std::size_t(ti)];
                double t = 0.0, u = 0.0, v = 0.0;
                if (intersectTriangle(o, d, tr.v0, tr.e1, tr.e2, t, u, v) &&
                    t > tMin && t < best) {
                    best = t; bestTri = ti; bestU = u; bestV = v;
                }
            }
        } else {
            if (d[n.axis] < 0.0) { stack[sp++] = n.left;  stack[sp++] = n.right; }
            else                 { stack[sp++] = n.right; stack[sp++] = n.left;  }
        }
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
