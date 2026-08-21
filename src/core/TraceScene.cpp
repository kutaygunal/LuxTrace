#include "TraceScene.h"

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
    m_det = DetectorInfo{};
    m_maxDepth = 0;
}

void TraceScene::build(const MeshList& meshes) {
    clear();

    std::size_t total = 0;
    for (const auto& m : meshes) total += m.tris.size();
    m_tris.reserve(total);
    m_surfs.reserve(meshes.size());

    for (const auto& m : meshes) {
        const int surfIdx = int(m_surfs.size());
        // MeshSurface derives from SurfaceOptics, so the whole optical
        // description slices across in one copy.
        m_surfs.push_back(static_cast<const SceneSurface&>(m));

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
        }

        if (m.isDetector && !m_det.valid) {
            m_det.valid  = true;
            m_det.center = Vec3(m.detCenter);
            m_det.w      = m.detW;
            m_det.h      = m.detH;
            m_det.nx     = m.detNX > 0 ? m.detNX : 1;
            m_det.ny     = m.detNY > 0 ? m.detNY : 1;
        }
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

bool TraceScene::nearestHit(const Vec3& o, const Vec3& d, RayHit& hit, double tMin) const {
    hit.tri = -1;
    hit.t   = 0.0;
    if (m_nodes.empty()) return false;

    const Vec3 invD(1.0 / d.x, 1.0 / d.y, 1.0 / d.z);
    double best    = DBL_MAX;
    int    bestTri = -1;

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
                    best = t; bestTri = ti;
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
    hit.tri = bestTri;
    hit.t   = best;
    return true;
}

bool TraceScene::nearestHitBruteForce(const Vec3& o, const Vec3& d, RayHit& hit, double tMin) const {
    hit.tri = -1;
    hit.t   = 0.0;
    double best    = DBL_MAX;
    int    bestTri = -1;
    for (std::size_t i = 0; i < m_tris.size(); ++i) {
        const SceneTri& tr = m_tris[i];
        double t = 0.0, u = 0.0, v = 0.0;
        if (intersectTriangle(o, d, tr.v0, tr.e1, tr.e2, t, u, v) && t > tMin && t < best) {
            best = t; bestTri = int(i);
        }
    }
    if (bestTri < 0) return false;
    hit.tri = bestTri;
    hit.t   = best;
    return true;
}
