#pragma once
#include "Mesh.h"
#include "SurfaceOptics.h"
#include "Vec3.h"
#include <cstddef>
#include <vector>

// Optical behaviour of one surface, flattened out of MeshSurface. It carries no
// extra state of its own -- the flattening is about locality, not about
// dropping properties -- so it is exactly SurfaceOptics.
using SceneSurface = SurfaceOptics;

// A triangle with its edges and unit normal precomputed. Moller-Trumbore only
// ever needs (v0, e1, e2), so storing them avoids recomputing two subtractions
// per triangle per ray.
struct SceneTri {
    Vec3 v0, e1, e2;   // e1 = v1 - v0, e2 = v2 - v0
    Vec3 n;            // unit face normal
    int  surf = 0;     // index into TraceScene::surfaces()
};

// Receiver frame of the detector surface (axis-aligned, facing +Z).
struct DetectorInfo {
    bool   valid = false;
    Vec3   center;              // true centre of the receiver rectangle
    double w = 0.0, h = 0.0;
    int    nx = 1, ny = 1;
};

struct RayHit {
    int    tri = -1;
    double t   = 0.0;
};

// Moller-Trumbore ray/triangle intersection. Two-sided on purpose: backfaces
// must hit, otherwise a ray travelling inside a refractive solid never finds
// the wall it is about to leave through.
// `d` must be normalised. Returns true and fills t/u/v on a hit with t >= 0.
bool intersectTriangle(const Vec3& o, const Vec3& d,
                       const Vec3& v0, const Vec3& e1, const Vec3& e2,
                       double& t, double& u, double& v);

// A ray-trace-ready scene: triangles flattened out of the OCCT meshes plus a
// bounding-volume hierarchy over them. Building it is done once per geometry;
// tracing only ever reads it, so it is safe to share across threads.
class TraceScene {
public:
    void build(const MeshList& meshes);
    void clear();

    // Nearest hit with t > tMin, found through the BVH.
    bool nearestHit(const Vec3& o, const Vec3& d, RayHit& hit, double tMin = 1e-6) const;
    // Reference implementation over every triangle; the tests use it to prove
    // the BVH does not change any result.
    bool nearestHitBruteForce(const Vec3& o, const Vec3& d, RayHit& hit, double tMin = 1e-6) const;

    const std::vector<SceneTri>&     triangles() const { return m_tris; }
    const std::vector<SceneSurface>& surfaces()  const { return m_surfs; }
    const SceneSurface& surfaceOf(const SceneTri& t) const { return m_surfs[std::size_t(t.surf)]; }
    const DetectorInfo& detector() const { return m_det; }

    bool        empty()      const { return m_tris.empty(); }
    std::size_t nodeCount()  const { return m_nodes.size(); }
    int         bvhDepth()   const { return m_maxDepth; }

private:
    // count == 0 marks an interior node; its children are `left` and `right`.
    // For a leaf, `left` is the first index into m_order.
    struct BvhNode {
        double bmin[3] = {0, 0, 0};
        double bmax[3] = {0, 0, 0};
        int    left  = 0;
        int    right = 0;
        int    count = 0;
        int    axis  = 0;
    };

    int  buildNode(int start, int count, int depth);
    void boundsOf(int start, int count, double bmin[3], double bmax[3]) const;

    std::vector<SceneTri>     m_tris;
    std::vector<SceneSurface> m_surfs;
    std::vector<BvhNode>      m_nodes;
    std::vector<int>          m_order;     // triangle indices, permuted by the build
    std::vector<Vec3>         m_centroids; // parallel to m_tris
    DetectorInfo              m_det;
    int                       m_maxDepth = 0;
};
