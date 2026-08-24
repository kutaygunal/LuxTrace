#pragma once
#include "Mesh.h"
#include "SurfaceOptics.h"
#include "Vec3.h"
#include <cmath>
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
    Vec3 n;            // unit face normal, pointing out of the solid
    int  surf = 0;     // index into TraceScene::surfaces()
};

// Per-vertex normals of one triangle, kept out of SceneTri so the intersection
// loop keeps reading 64-byte triangles: shading normals are touched once per
// *hit*, the triangle itself once per *test*, and there are orders of magnitude
// more tests than hits.
struct TriShading {
    Vec3 n0, n1, n2;
};

// Receiver frame of a detector surface. Stored as an explicit frame -- origin
// plus two in-plane axes plus a normal -- rather than as "assume +Z", so a
// receiver can be tilted, off-axis, or one of several in the same scene.
struct DetectorInfo {
    bool   valid = false;
    Vec3   center;              // true centre of the receiver rectangle
    Vec3   u{1, 0, 0};          // unit, spans the `w` extent
    Vec3   v{0, 1, 0};          // unit, spans the `h` extent
    Vec3   n{0, 0, 1};          // unit surface normal
    double w = 0.0, h = 0.0;
    int    nx = 1, ny = 1;
    // cos of the acceptance half-angle about `n`. -1 accepts every direction,
    // which is what a bare energy-collecting receiver does.
    double cosAcceptance = -1.0;
    int    surf = -1;           // index into TraceScene::surfaces()

    // Grid coordinates of a point on the receiver, in bins. Returns false when
    // the point falls outside the rectangle.
    bool binOf(const Vec3& p, int& bx, int& by) const {
        if (w <= 0.0 || h <= 0.0) return false;
        const Vec3 r = p - center;
        const double lu = r.dot(u) + 0.5 * w;
        const double lv = r.dot(v) + 0.5 * h;
        if (lu < 0.0 || lv < 0.0) return false;
        bx = int(lu / w * double(nx));
        by = int(lv / h * double(ny));
        return bx >= 0 && bx < nx && by >= 0 && by < ny;
    }

    // Whether a ray arriving along `d` is inside the acceptance cone.
    bool accepts(const Vec3& d) const {
        if (cosAcceptance <= -1.0) return true;
        return std::fabs(d.dot(n)) >= cosAcceptance;
    }
};

// A rigid placement of a bottom-level hierarchy in the world.
//
// Twenty-five identical lenslets used to be twenty-five tessellations and one
// hierarchy over all of their triangles. As instances they are one mesh, one
// hierarchy and twenty-five transforms -- which is what lets an array carry a
// mesh as fine as a single lens instead of a hand-coarsened one, and what makes
// an imported CAD assembly with repeated parts affordable at all.
//
// The transform is rigid by construction (rotation and translation only), so a
// distance in instance space is a distance in world space and `t` needs no
// rescaling on the way back out.
struct Instance {
    Vec3 r0, r1, r2;      // rows of the rotation, world <- local
    Vec3 origin;          // translation, world
    Vec3 i0, i1, i2;      // rows of the inverse rotation, local <- world
    double bmin[3] = {0, 0, 0};   // world-space bounds of the placed part
    double bmax[3] = {0, 0, 0};
    int  blas = 0;        // index into the bottom-level list
    int  surf = 0;        // index into TraceScene::surfaces()

    Vec3 toLocal(const Vec3& p) const {
        const Vec3 q = p - origin;
        return Vec3(i0.dot(q), i1.dot(q), i2.dot(q));
    }
    Vec3 dirToLocal(const Vec3& d) const {
        return Vec3(i0.dot(d), i1.dot(d), i2.dot(d));
    }
    Vec3 dirToWorld(const Vec3& d) const {
        return Vec3(r0.dot(d), r1.dot(d), r2.dot(d));
    }
};

struct RayHit {
    int    tri = -1;
    // Which placement was hit, or -1 for geometry that is not instanced. The
    // triangle index is into that instance's own primitive list.
    int    inst = -1;
    double t   = 0.0;
    // Barycentric coordinates of the hit within the triangle. Moller-Trumbore
    // produces them anyway; keeping them is what lets the shading normal be
    // interpolated instead of the facet normal being used raw.
    double u   = 0.0;
    double v   = 0.0;
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

    // Nearest hit with t > tMin, found through the hierarchy.
    //
    // A scene with no instanced parts -- which is most of them -- goes straight
    // to the plain walk, with the branch resolved here in the caller rather than
    // inside it. Putting the two paths in one function measured a third of the
    // trace time on a light guide, whose rays take a hundred hits each: the
    // instanced path's presence alone was enough to cost the common one its
    // registers.
    bool nearestHit(const Vec3& o, const Vec3& d, RayHit& hit, double tMin = 1e-6) const {
        return m_instances.empty() ? nearestHitStatic(o, d, hit, tMin)
                                   : nearestHitInstanced(o, d, hit, tMin);
    }
    // Reference implementation over every triangle; the tests use it to prove
    // the BVH does not change any result.
    bool nearestHitBruteForce(const Vec3& o, const Vec3& d, RayHit& hit, double tMin = 1e-6) const;

    const std::vector<SceneTri>&     triangles() const { return m_tris; }
    const std::vector<SceneSurface>& surfaces()  const { return m_surfs; }
    const SceneSurface& surfaceOf(const SceneTri& t) const { return m_surfs[std::size_t(t.surf)]; }

    // What a hit resolves to, whichever level of the hierarchy found it. The
    // tracer reads these rather than indexing `triangles()` directly, because an
    // instanced hit does not live in that list at all.
    int surfaceIndexOf(const RayHit& hit) const {
        if (hit.inst < 0) return m_tris[std::size_t(hit.tri)].surf;
        return m_instances[std::size_t(hit.inst)].surf;
    }
    const SceneSurface& surfaceOf(const RayHit& hit) const {
        return m_surfs[std::size_t(surfaceIndexOf(hit))];
    }
    // Unit facet normal in world space, pointing out of the solid.
    //
    // Inline on purpose: a light guide takes a hundred hits per ray, and an
    // out-of-line call here measured a quarter of the trace time on exactly
    // those scenes.
    Vec3 geometricNormal(const RayHit& hit) const {
        if (hit.inst < 0) return m_tris[std::size_t(hit.tri)].n;
        const Instance& in = m_instances[std::size_t(hit.inst)];
        return in.dirToWorld(m_blas[std::size_t(in.blas)].tris[std::size_t(hit.tri)].n);
    }

    const std::vector<Instance>& instances() const { return m_instances; }
    // Triangles across every bottom-level part, counted once each rather than
    // once per placement: the number an instanced scene actually stores.
    std::size_t instancedTriangles() const;
    const DetectorInfo& detector() const { return m_det; }
    // Every receiver in the scene, in surface order. detector() is the first.
    const std::vector<DetectorInfo>& detectors() const { return m_dets; }
    // Receiver index of a surface, or -1. A second detector surface used to
    // terminate rays and book their flux against the *first* detector's frame;
    // this is what lets each one bin against itself.
    int detectorOfSurface(int surf) const {
        return (surf >= 0 && surf < int(m_detOfSurf.size())) ? m_detOfSurf[std::size_t(surf)] : -1;
    }
    // Offset of a receiver's first bin in a flat grid holding them all.
    const std::vector<std::size_t>& detectorOffsets() const { return m_detOffset; }
    std::size_t detectorCells() const { return m_detCells; }

    // Interpolated (smooth) normal at a hit, falling back to the facet normal
    // where the mesh carries none. Always unit and always oriented the same way
    // as the facet normal, so "d . n > 0 means leaving" survives the smoothing.
    Vec3 shadingNormal(const RayHit& hit) const {
        const SceneTri*   t  = nullptr;
        const TriShading* sh = nullptr;
        const Instance*   in = nullptr;
        if (hit.inst < 0) {
            t = &m_tris[std::size_t(hit.tri)];
            if (!m_shade.empty()) sh = &m_shade[std::size_t(hit.tri)];
        } else {
            in = &m_instances[std::size_t(hit.inst)];
            const Blas& bl = m_blas[std::size_t(in->blas)];
            t = &bl.tris[std::size_t(hit.tri)];
            if (!bl.shade.empty()) sh = &bl.shade[std::size_t(hit.tri)];
        }
        // Both normals are produced in the part's own frame and rotated
        // together, so the comparison below is made where they are comparable.
        if (!sh) return in ? in->dirToWorld(t->n) : t->n;

        const double w = 1.0 - hit.u - hit.v;
        Vec3 n = sh->n0 * w + sh->n1 * hit.u + sh->n2 * hit.v;
        if (!n.normalize()) return in ? in->dirToWorld(t->n) : t->n;
        // The interpolation is only ever a refinement of the facet normal, never
        // a reversal of it: a mesh whose vertex normals disagree with its
        // winding would otherwise turn an entering ray into a leaving one.
        if (n.dot(t->n) < 0.0) n = t->n;
        return in ? in->dirToWorld(n) : n;
    }

    // Bounding sphere of everything in the scene, from the BVH root. A ray that
    // leaves the emitter outside the cone this sphere subtends provably hits
    // nothing at all, which is what makes emission aiming exact rather than
    // merely unbiased.
    const Vec3& boundsCenter() const { return m_boundsCenter; }
    double      boundsRadius() const { return m_boundsRadius; }

    bool        empty()      const { return m_tris.empty() && m_instances.empty(); }
    std::size_t nodeCount()  const { return m_nodes.size(); }
    int         bvhDepth()   const { return m_maxDepth; }

private:
    // A node of the hierarchy. count == 0 marks an interior node, whose children
    // are `left` and `right`; for a leaf, `left` is the first index into
    // m_order.
    //
    // The bounds stay in double, and that is a measured choice rather than an
    // oversight. Two cheaper-looking layouts were built and benchmarked against
    // this one across the whole scene library:
    //
    //   float bounds rounded outward, halving the node to 40 bytes -- slower on
    //   every scene, by a quarter on a light guide and by a fifth on the 81 000
    //   triangle microlens array. None of these trees is large enough for the
    //   memory to be the limit, and six float-to-double conversions per node
    //   test add latency to a loop whose bounds were already in cache.
    //
    //   a four-wide node with the four slab tests laid out as lanes -- better on
    //   the largest scenes and a quarter worse on a light guide, whose rays
    //   cross the whole solid and give the hierarchy nothing to cull. Making
    //   that pay wants a float SIMD slab test, which trades away the property
    //   that a grazing ray can never be rounded out of a box it really enters.
    //
    // Both are kept in this comment rather than in the code because a
    // performance claim that does not survive its own benchmark is not an
    // optimisation.
    struct BvhNode {
        double bmin[3] = {0, 0, 0};
        double bmax[3] = {0, 0, 0};
        int    left  = 0;
        int    right = 0;
        int    count = 0;
        int    axis  = 0;
    };

    // One instanced part: its own triangles, shading normals and hierarchy.
    struct Blas {
        std::vector<SceneTri>   tris;
        std::vector<TriShading> shade;
        std::vector<int>        order;
        std::vector<BvhNode>    nodes;
        int                     maxDepth = 0;
    };

    int  buildNode(int start, int count, int depth);
    void boundsOf(int start, int count, double bmin[3], double bmax[3]) const;

    // Builds a hierarchy over the triangles currently in the member vectors and
    // moves the result into `out`, leaving the members clear. Building each
    // bottom level through the same code as the top one is what keeps a single
    // surface-area heuristic in the codebase.
    void buildBlasFromMembers(Blas& out);
    // A median-split hierarchy over the placements' world boxes. There are far
    // fewer of these than there are triangles, so the surface-area heuristic
    // would be effort spent where it cannot be repaid.
    int  buildTlasNode(int start, int count, int depth);
    // Nearest hit among the placements, tightening `best` as it goes.
    bool hitInstances(const Vec3& o, const Vec3& d, RayHit& hit, double tMin,
                      double& best) const;
    // The walk over the scene-wide hierarchy, and the walk that also visits the
    // placements. Split so neither pays for the other.
    bool nearestHitStatic(const Vec3& o, const Vec3& d, RayHit& hit, double tMin) const;
    bool nearestHitInstanced(const Vec3& o, const Vec3& d, RayHit& hit, double tMin) const;

    std::vector<SceneTri>     m_tris;
    std::vector<SceneSurface> m_surfs;
    std::vector<BvhNode>      m_nodes;
    std::vector<int>          m_order;     // triangle indices, permuted by the build
    std::vector<Vec3>         m_centroids; // parallel to m_tris
    std::vector<TriShading>   m_shade;     // parallel to m_tris, or empty
    std::vector<Instance>     m_instances;
    std::vector<Blas>         m_blas;
    std::vector<int>          m_tlasOrder;    // instance indices, permuted by the build
    std::vector<BvhNode>      m_tlasNodes;
    std::vector<DetectorInfo> m_dets;
    std::vector<int>          m_detOfSurf;   // parallel to m_surfs, -1 for a non-receiver
    std::vector<std::size_t>  m_detOffset;   // parallel to m_dets
    std::size_t               m_detCells = 0;
    Vec3                      m_boundsCenter;
    double                    m_boundsRadius = 0.0;
    DetectorInfo              m_det;
    int                       m_maxDepth = 0;
};
