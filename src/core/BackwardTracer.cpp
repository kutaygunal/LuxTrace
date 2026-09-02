#include "BackwardTracer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <thread>

#include "Bsdf.h"
#include "Material.h"
#include "Optics.h"
#include "Polarisation.h"
#include "Spectrum.h"
#include "ThreadPool.h"

namespace backward {
namespace {

using optics::kPi;
using optics::kTwoPi;
using optics::uniform01;
using optics::orthonormalBasis;
using optics::cosineHemisphere;
using optics::reflect;
using optics::refract;
using optics::fresnelReflectance;

constexpr double kEpsRel = 1e-9;

// Millimetres to metres, squared.
//
// The geometry is in millimetres and every radiance in the integrator is formed
// out of it -- either as a flux over an emitter's own area or as a flux over the
// square of a distance -- so each one carries exactly one reciprocal square
// millimetre and no more. Candela per square *metre* is what a luminance is
// quoted in, so the whole answer is scaled once at the end rather than each term
// being converted where it is built, which is where a factor of a million goes
// missing.
constexpr double kPerMm2ToPerM2 = 1.0e6;

// How many media a path can be inside at once. The same eight the forward
// tracer carries, for the same reason: cemented doublets, immersed optics
// and clad guides need two or three, and eight is past anything real.
constexpr int kMediumDepth = 8;

// The refractive solids a path is currently inside, innermost last. Its own
// copy rather than the forward tracer's, which lives inside that
// translation unit -- the two do the same bookkeeping over the same rules,
// stated once each rather than shared through a header nobody else wants.
struct MediumStack {
    int e[kMediumDepth] = {};
    int n = 0;

    bool contains(int idx) const {
        for (int i = 0; i < n; ++i) if (e[i] == idx) return true;
        return false;
    }
    void push(int idx) { if (n < kMediumDepth) e[n++] = idx; }
    // Removes the innermost entry matching `idx`. A stack that never saw it
    // is left alone: leaving a body one was never recorded as entering is a
    // modelling artefact, not a reason to corrupt the rest of the stack.
    void pop(int idx) {
        for (int i = n - 1; i >= 0; --i) {
            if (e[i] != idx) continue;
            for (int j = i + 1; j < n; ++j) e[j - 1] = e[j];
            --n;
            return;
        }
    }
    int top() const { return n > 0 ? e[n - 1] : -1; }
};

double surfaceEps(const Vec3& p) {
    const double m = std::max(std::max(std::fabs(p.x), std::fabs(p.y)), std::fabs(p.z));
    return kEpsRel * (m > 1.0 ? m : 1.0);
}

// ---------------------------------------------------------------------------
// the emitters, as things a path can be connected to
// ---------------------------------------------------------------------------
//
// A forward tracer never has to answer "how bright is this source, seen from
// there" -- it starts at the source and the question does not arise. A camera
// asks it at every vertex, so each emitter needs three things it never needed
// before: a radiance (or an intensity, for one with no area), a way to draw a
// point on it with a known density, and a way to be hit directly by a ray that
// happens to be pointing at it.

// One emitter, resolved out of a SourceConfig once per render.
struct Emitter {
    enum class Kind { PointLike, Disc, Rect, Sphere, Collimated };

    Kind   kind = Kind::PointLike;
    Vec3   origin;
    Vec3   axis{0, 0, 1};
    Vec3   t, b;                 // an orthonormal frame about `axis`
    double power = 0.0;          // in the run's unit
    double sizeA = 0.0, sizeB = 0.0;
    double beamRadius = 0.0;
    bool   cosineLaw = true;     // Lambertian, as opposed to uniform in solid angle
    double cosHalfAngle = -1.0;
    double area = 0.0;
    // Radiance for the ones that have an area; axial intensity for the ones
    // that do not.
    double radiance = 0.0;
    double intensity0 = 0.0;
    SampledSpectrum spectrum;
    FluxUnit unit = FluxUnit::Watt;

    bool hasArea() const {
        return kind == Kind::Disc || kind == Kind::Rect || kind == Kind::Sphere;
    }
};

// The emitted radiance of an area source, along a direction whose cosine to the
// local emitting normal is `cosL`.
//
// A Lambertian emitter of total flux Phi over area A into a cone of half-angle
// theta has Phi = L A pi sin^2(theta), so L = Phi / (A pi sin^2 theta) -- which
// is the M / pi of the closed form when the cone is the whole hemisphere. A
// source that is uniform in solid angle instead has Phi = L A 2 pi (1 - cos
// theta), and the distinction is not cosmetic: the two differ by a factor of
// two at a full hemisphere.
double areaRadiance(const Emitter& e, double cosL) {
    if (cosL <= 0.0) return 0.0;
    if (e.cosHalfAngle > -1.0 && cosL < e.cosHalfAngle) return 0.0;   // outside the cone
    return e.radiance;
}

// Intensity of a point-like emitter along a direction at `cosA` to its axis.
double pointIntensity(const Emitter& e, double cosA) {
    if (e.cosHalfAngle > -1.0 && cosA < e.cosHalfAngle) return 0.0;
    if (e.cosineLaw) return cosA > 0.0 ? e.intensity0 * cosA : 0.0;
    return e.intensity0;
}

Emitter resolve(const SourceConfig& src, FluxUnit unit) {
    Emitter e;
    e.origin = Vec3(src.origin);
    e.axis   = Vec3(src.axis);
    if (!e.axis.normalize()) e.axis = Vec3(0, 0, 1);
    orthonormalBasis(e.axis, e.t, e.b);
    e.power      = src.power;
    e.sizeA      = src.sizeA;
    e.sizeB      = src.sizeB;
    e.beamRadius = src.beamRadius;
    e.cosineLaw  = (src.type == SourceConfig::Type::Lambertian);
    e.unit       = unit;
    e.spectrum.build(src.spectrum);

    double half = src.halfAngleDeg;
    if (src.type == SourceConfig::Type::Lambertian) half = std::min(half, 90.0);
    half = std::clamp(half, 0.0, 180.0);
    e.cosHalfAngle = std::cos(half * kPi / 180.0);
    const double sin2 = std::max(1e-12, 1.0 - e.cosHalfAngle * e.cosHalfAngle);

    if (src.type == SourceConfig::Type::Collimated) {
        e.kind = Emitter::Kind::Collimated;
        e.area = kPi * std::max(1e-12, src.beamRadius * src.beamRadius);
        return e;
    }
    switch (src.shape) {
    case SourceConfig::Shape::Disc:
        e.kind = Emitter::Kind::Disc;
        e.area = kPi * src.sizeA * src.sizeA;
        break;
    case SourceConfig::Shape::Rect:
        e.kind = Emitter::Kind::Rect;
        e.area = src.sizeA * src.sizeB;
        break;
    case SourceConfig::Shape::Sphere:
        e.kind = Emitter::Kind::Sphere;
        e.area = 4.0 * kPi * src.sizeA * src.sizeA;
        break;
    default:
        e.kind = Emitter::Kind::PointLike;
        e.area = 0.0;
        break;
    }

    if (e.hasArea() && e.area > 1e-15) {
        e.radiance = e.cosineLaw ? e.power / (e.area * kPi * sin2)
                                 : e.power / (e.area * kTwoPi * (1.0 - e.cosHalfAngle));
    } else {
        e.kind = Emitter::Kind::PointLike;
        e.intensity0 = e.cosineLaw ? e.power / (kPi * sin2)
                                   : e.power / (kTwoPi * std::max(1e-12,
                                                                  1.0 - e.cosHalfAngle));
    }
    return e;
}

// A point on the emitter, uniform per unit area, with the outward normal there.
void sampleArea(const Emitter& e, std::uint64_t& rng, Vec3& q, Vec3& nq, double& pdfArea) {
    const double u1 = uniform01(rng), u2 = uniform01(rng);
    switch (e.kind) {
    case Emitter::Kind::Disc: {
        const double r = e.sizeA * std::sqrt(u1);      // area-uniform, not radius-uniform
        const double a = kTwoPi * u2;
        q  = e.origin + e.t * (r * std::cos(a)) + e.b * (r * std::sin(a));
        nq = e.axis;
        break;
    }
    case Emitter::Kind::Rect:
        q  = e.origin + e.t * (e.sizeA * (u1 - 0.5)) + e.b * (e.sizeB * (u2 - 0.5));
        nq = e.axis;
        break;
    case Emitter::Kind::Sphere: {
        const double cz = 1.0 - 2.0 * u1;
        const double sz = std::sqrt(std::max(0.0, 1.0 - cz * cz));
        const double a  = kTwoPi * u2;
        nq = Vec3(sz * std::cos(a), sz * std::sin(a), cz);
        q  = e.origin + nq * e.sizeA;
        break;
    }
    default:
        q  = e.origin;
        nq = e.axis;
        break;
    }
    pdfArea = e.area > 1e-15 ? 1.0 / e.area : 0.0;
}

// Does this ray see the emitter itself? Only ever asked of a camera ray or of
// one that left a specular surface, because those are the paths a connection
// cannot stand in for.
bool hitEmitter(const Emitter& e, const Vec3& o, const Vec3& d, double tMax,
                double& tOut, double& radianceOut) {
    tOut = 0.0;
    radianceOut = 0.0;
    switch (e.kind) {
    case Emitter::Kind::Disc:
    case Emitter::Kind::Rect: {
        const double denom = d.dot(e.axis);
        if (std::fabs(denom) < 1e-12) return false;
        const double t = (e.origin - o).dot(e.axis) / denom;
        if (t <= 1e-6 || t >= tMax) return false;
        const Vec3 p = o + d * t - e.origin;
        if (e.kind == Emitter::Kind::Disc) {
            if (p.lengthSquared() > e.sizeA * e.sizeA) return false;
        } else {
            if (std::fabs(p.dot(e.t)) > 0.5 * e.sizeA) return false;
            if (std::fabs(p.dot(e.b)) > 0.5 * e.sizeB) return false;
        }
        // Seen from the emitting side only: the back of a one-sided emitter is
        // dark, which is what makes a luminaire have a back.
        radianceOut = areaRadiance(e, -d.dot(e.axis));
        tOut = t;
        return radianceOut > 0.0;
    }
    case Emitter::Kind::Sphere: {
        const Vec3 rel = o - e.origin;
        const double b = rel.dot(d);
        const double c = rel.lengthSquared() - e.sizeA * e.sizeA;
        const double disc = b * b - c;
        if (disc < 0.0) return false;
        const double root = std::sqrt(disc);
        double t = -b - root;
        if (t <= 1e-6) t = -b + root;
        if (t <= 1e-6 || t >= tMax) return false;
        const Vec3 nq = ((o + d * t) - e.origin).normalized();
        radianceOut = areaRadiance(e, -d.dot(nq));
        tOut = t;
        return radianceOut > 0.0;
    }
    default:
        // A point has no area and a collimated beam has no angular extent:
        // neither is an object a ray can land on, and pretending otherwise
        // would put an infinity in the image.
        return false;
    }
}

// ---------------------------------------------------------------------------
// the surface, as something a connection can be evaluated against
// ---------------------------------------------------------------------------

// The reflectance a surface shows at this angle -- the share of the incident
// energy that leaves on the reflected side, before the lobe redistributes it.
double reflectanceAt(const SceneSurface& surf, const PhysicsOptions& phys,
                     double n1, double n2, double cosI, double lambda) {
    if (surf.index > 0.0) {
        if (!(phys.fresnel && surf.fresnel)) return surf.reflectivity;
        double R = fresnelReflectance(n1, n2, cosI);
        if (phys.coatings && surf.coating.active()) {
            double as = 0.0, ap = 0.0, phase = 0.0;
            polarisation::fresnelAmplitudes(n1, n2, cosI, as, ap, phase);
            double rs = 0.0, rp = 0.0;
            surf.coating.reflectanceSP(n1, n2, cosI, lambda, as * as, ap * ap, rs, rp);
            R = 0.5 * (rs + rp);
        }
        return R;
    }
    if (phys.fresnel && surf.material.isMetal())
        return metalReflectance(n1, surf.material.indexAt(lambda),
                                surf.material.extinctionAt(lambda), cosI);
    return surf.reflectivity;
}

// The BRDF for a connection leaving along `w`, per steradian.
//
// bsdf::Surface::value is written in the offset of the outgoing direction from
// the specular one, in direction cosines, which is exactly what a connection
// has to hand: the lobe is a redistribution of the reflected energy, so the
// reflectance multiplies it rather than sitting beside it. A Lambertian lobe
// returns fraction / pi and this collapses to the rho / pi every textbook
// writes -- which is what the furnace test checks to the last digit.
double brdf(const bsdf::Surface& lobe, double reflectance,
            const Vec3& spec, const Vec3& w) {
    if (lobe.isSpecular()) return 0.0;              // no direction to connect along
    const double c = std::clamp(spec.dot(w), -1.0, 1.0);
    // The direction-cosine offset between the two directions.
    const double dbeta = std::sqrt(std::max(0.0, 2.0 * (1.0 - c)));
    return reflectance * lobe.value(std::min(dbeta, 2.0));
}

// ---------------------------------------------------------------------------
// the camera
// ---------------------------------------------------------------------------

struct CameraFrame {
    Vec3   eye, forward, right, up;
    double focal = 50.0, halfW = 18.0, halfH = 12.0;
    double apertureRadius = 0.0, focusDistance = 0.0;
};

CameraFrame frameOf(const CameraConfig& c) {
    CameraFrame f;
    f.eye = c.eye;
    f.forward = c.target - c.eye;
    if (!f.forward.normalize()) f.forward = Vec3(0, 0, 1);
    Vec3 up = c.up;
    if (!up.normalize()) up = Vec3(0, 0, 1);
    f.right = f.forward.cross(up);
    if (!f.right.normalize()) {
        // The camera is looking straight along `up`; any perpendicular will do,
        // and picking one silently beats refusing to render.
        orthonormalBasis(f.forward, f.right, up);
    }
    f.up = f.right.cross(f.forward);
    f.up.normalize();

    f.focal = c.focalLengthMm;
    f.halfW = 0.5 * c.sensorWidthMm;
    f.halfH = 0.5 * c.sensorHeightMm();
    f.apertureRadius = c.fNumber > 0.0 ? 0.5 * c.focalLengthMm / c.fNumber : 0.0;
    f.focusDistance = c.focusDistanceMm > 0.0
                          ? c.focusDistanceMm
                          : std::max(1e-6, (c.target - c.eye).length());
    return f;
}

// The ray for one sample of one pixel. Stratified inside the pixel, so the
// samples of a pixel cover it evenly rather than clumping.
void cameraRay(const CameraFrame& f, const CameraConfig& c, int px, int py,
               int sample, std::uint64_t& rng, Vec3& o, Vec3& d) {
    const int side = std::max(1, int(std::sqrt(double(c.samplesPerPixel)) + 0.5));
    const int sx = sample % side, sy = (sample / side) % side;
    const double jx = (double(sx) + uniform01(rng)) / double(side);
    const double jy = (double(sy) + uniform01(rng)) / double(side);

    // Image row 0 is the top of the picture, so v runs down the sensor.
    const double u = ((double(px) + jx) / double(c.width)  - 0.5) * 2.0 * f.halfW;
    const double v = (0.5 - (double(py) + jy) / double(c.height)) * 2.0 * f.halfH;

    Vec3 dir = f.forward * f.focal + f.right * u + f.up * v;
    dir.normalize();
    o = f.eye;
    d = dir;

    if (f.apertureRadius <= 0.0) return;            // pinhole

    // Thin lens: the sample moves on the aperture and the ray is re-aimed at
    // the point of the focal plane the pinhole ray would have reached, so
    // everything at the focus distance stays sharp and everything else does not.
    const double cosF = dir.dot(f.forward);
    if (std::fabs(cosF) < 1e-9) return;
    const Vec3 focusPoint = o + dir * (f.focusDistance / cosF);
    const double a = kTwoPi * uniform01(rng);
    const double r = f.apertureRadius * std::sqrt(uniform01(rng));
    o = f.eye + f.right * (r * std::cos(a)) + f.up * (r * std::sin(a));
    d = focusPoint - o;
    d.normalize();
}

// ---------------------------------------------------------------------------
// the path
// ---------------------------------------------------------------------------

struct Walk {
    const TraceScene*                scene   = nullptr;
    const std::vector<SceneSurface>* surfs   = nullptr;
    const std::vector<Emitter>*      lights  = nullptr;
    PhysicsOptions                   physics;
    double  lambda = 587.6;
    double  photoWeight = 1.0;      // V(lambda) weighting, mean one over the SPD
    int     maxDepth = 24;
    int     rouletteDepth = 4;
    double* truncated = nullptr;
    // The surround, already in cd/m^2 and so kept apart from the emitter
    // total, which is in the source unit per square millimetre until the
    // very end.
    double  envSky = 0.0, envFloor = 0.0;
    Vec3    envUp{0.0, 0.0, 1.0};
    // A map, and the frame it is oriented in. The frame is built once per
    // run because a lookup happens per bounce per pixel and an orthonormal
    // basis is not free.
    const envmap::EnvironmentMap* envMap = nullptr;
    double  envMapScale = 1.0;
    Vec3    envE1{1.0, 0.0, 0.0}, envE2{0.0, 1.0, 0.0};
};

// The surround along `d`. A sky above the horizon and a darker floor below,
// blended over a few degrees so a mirror does not show a hard line where a
// room has a soft one.
double environmentAlong(const Walk& wk, const Vec3& d) {
    if (wk.envMap && !wk.envMap->empty())
        return wk.envMapScale * wk.envMap->along(d, wk.envUp, wk.envE1, wk.envE2);
    if (wk.envSky <= 0.0 && wk.envFloor <= 0.0) return 0.0;
    const double c = std::clamp(d.dot(wk.envUp), -1.0, 1.0);
    const double t = std::clamp(0.5 + 0.5 * c / 0.15, 0.0, 1.0);
    return wk.envFloor + (wk.envSky - wk.envFloor) * t;
}

double mediumIndexOf(const Walk& wk, int medium) {
    if (medium < 0) return 1.0;
    const SceneSurface& s = (*wk.surfs)[std::size_t(medium)];
    const double n = s.indexAt(wk.lambda, wk.physics.dispersion);
    return n > 0.0 ? n : 1.0;
}

// Everything the emitters deliver to `p`, through the lobe at that vertex.
//
// The mirror image of RayTracer's nextEventEstimate: there, a diffuse bounce is
// connected to every receiver; here, to every emitter. Both replace a direction
// nobody would have sampled with the analytic value of the integral over it,
// and both are why a point source is visible at all.
// Is anything solid between here and there?
//
// TraceScene::occluded answers the same question and ignores one surface,
// which is what forward next-event estimation needs: the receiver it is
// connecting to. A camera needs the other rule -- *every* receiver is
// transparent, because a receiver is a measurement surface and not an
// object. A photometer plane hung in front of a wall must not cast a
// shadow on it, and it did when this walked the shared routine.
bool blocked(const Walk& wk, const Vec3& o, const Vec3& d, double tMax,
             int ignoreSurface, double tMin) {
    double travelled = tMin;
    Vec3   from = o;
    for (int guard = 0; guard < 32; ++guard) {
        RayHit h;
        if (!wk.scene->nearestHit(from, d, h, tMin)) return false;
        if (travelled + h.t >= tMax) return false;
        const int idx = wk.scene->surfaceIndexOf(h);
        if (idx != ignoreSurface && !(*wk.surfs)[std::size_t(idx)].isDetector)
            return true;
        travelled += h.t;
        from = from + d * (h.t + tMin);
    }
    // More than thirty receivers stacked along one shadow ray is not a
    // scene, it is a bug; treating it as blocked is the conservative end.
    return true;
}

double connectToLights(const Walk& wk, const Vec3& p, const Vec3& ng,
                       const Vec3& spec, const bsdf::Surface& lobe,
                       double reflectance, int ignoreSurface, std::uint64_t& rng) {
    if (lobe.isSpecular() || reflectance <= 0.0) return 0.0;
    double total = 0.0;
    const double eps = surfaceEps(p);

    for (const Emitter& e : *wk.lights) {
        if (e.power <= 0.0) continue;

        if (e.kind == Emitter::Kind::Collimated) {
            // A collimated beam has no solid angle to sample: it illuminates a
            // point from exactly one direction or not at all. Sampling it would
            // draw from a delta and return either zero or an infinity, so it is
            // resolved rather than sampled.
            const Vec3 w = -e.axis;                  // back towards the emitter
            const double cosS = w.dot(ng);
            if (cosS <= 1e-9) continue;
            const Vec3 rel = p - e.origin;
            const double along = rel.dot(e.axis);
            if (along <= 0.0) continue;              // behind the aperture
            const Vec3 off = rel - e.axis * along;
            if (off.lengthSquared() > e.beamRadius * e.beamRadius) continue;
            if (blocked(wk, p + ng * eps, w, along - eps, ignoreSurface, eps))
                continue;
            const double irradiance = e.power / e.area;   // normal to the beam
            total += brdf(lobe, reflectance, spec, w) * irradiance * cosS;
            continue;
        }

        if (!e.hasArea()) {
            // A point: one direction, one inverse square, no area to sample.
            Vec3 w = e.origin - p;
            const double r2 = w.lengthSquared();
            if (r2 < 1e-18 || !w.normalize()) continue;
            const double cosS = w.dot(ng);
            if (cosS <= 1e-9) continue;
            const double I = pointIntensity(e, (-w).dot(e.axis));
            if (I <= 0.0) continue;
            const double r = std::sqrt(r2);
            if (blocked(wk, p + ng * eps, w, r - eps, ignoreSurface, eps)) continue;
            total += brdf(lobe, reflectance, spec, w) * I * cosS / r2;
            continue;
        }

        // An area emitter, sampled uniformly over its own surface. The solid
        // angle it subtends comes out of the geometry rather than being
        // approximated by a point at its centre, which is what makes a large
        // soft source soft.
        Vec3 q, nq;
        double pdfArea = 0.0;
        sampleArea(e, rng, q, nq, pdfArea);
        if (pdfArea <= 0.0) continue;

        Vec3 w = q - p;
        const double r2 = w.lengthSquared();
        if (r2 < 1e-18 || !w.normalize()) continue;
        const double cosS = w.dot(ng);
        if (cosS <= 1e-9) continue;
        const double cosL = (-w).dot(nq);
        const double L = areaRadiance(e, cosL);
        if (L <= 0.0) continue;
        const double r = std::sqrt(r2);
        if (blocked(wk, p + ng * eps, w, r - eps, ignoreSurface, eps)) continue;

        // dA -> dOmega is cosL / r^2, and the uniform area draw brings its own
        // 1 / pdfArea with it.
        total += brdf(lobe, reflectance, spec, w) * L * cosS * cosL / (r2 * pdfArea);
    }
    return total;
}

// The surround delivered to `p` by connecting to the map, in cd/m^2.
//
// The mirror of connectToLights, for the one emitter that is not in the
// light list: the room itself. It is drawn in proportion to the map's own
// brightness times solid angle, so a window or a sun is found on the first
// try rather than once in a few thousand -- which is the difference between
// a usable environment and a grainy one.
//
// The direction it draws is *not* the direction the path then follows: this
// is a connection, and the path goes on wherever the BSDF sends it. That is
// why the caller must stop adding the surround where such a path escapes,
// exactly as it already stops adding an emitter it has connected to.
double connectToEnvironment(const Walk& wk, const Vec3& p, const Vec3& ng,
                            const Vec3& spec, const bsdf::Surface& lobe,
                            double reflectance, int ignoreSurface,
                            std::uint64_t& rng) {
    if (!wk.envMap || wk.envMap->empty()) return 0.0;
    if (lobe.isSpecular() || reflectance <= 0.0) return 0.0;

    Vec3   w;
    double pdf = 0.0;
    if (!wk.envMap->sample(uniform01(rng), uniform01(rng), wk.envUp, wk.envE1,
                           wk.envE2, w, pdf))
        return 0.0;
    if (!(pdf > 0.0)) return 0.0;

    const double cosS = w.dot(ng);
    if (cosS <= 1e-9) return 0.0;

    const double L = wk.envMapScale * wk.envMap->along(w, wk.envUp, wk.envE1,
                                                       wk.envE2);
    if (L <= 0.0) return 0.0;

    const double eps = surfaceEps(p);
    // Out to the far side of anything: the map is at infinity, so the only
    // question is whether the scene itself is in the way.
    if (blocked(wk, p + ng * eps, w, 1e30, ignoreSurface, eps)) return 0.0;

    return brdf(lobe, reflectance, spec, w) * L * cosS / pdf;
}

// The luminance along one camera ray.
double radianceAlong(const Walk& wk, Vec3 o, Vec3 d, std::uint64_t& rng) {
    const TraceScene& scene = *wk.scene;
    // Two accumulators, because they are in different units until the end:
    // the emitters are in the source unit per square millimetre and the
    // surround was given in cd/m^2 already.
    double radiance   = 0.0;
    double fromEnv    = 0.0;
    double throughput = 1.0;
    int    medium     = -1;
    MediumStack media;
    // Whether the last vertex was specular. Only then may a direct hit on an
    // emitter be added: at a scattering vertex the connection above has already
    // accounted for every emitter, and adding the sampled direction's own hit
    // as well would count the same light twice.
    bool   allowDirect = true;

    for (int depth = 0; depth < wk.maxDepth; ++depth) {
        const double eps = surfaceEps(o);
        RayHit h;
        const bool hit = scene.nearestHit(o, d, h, eps);
        const double tMax = hit ? h.t : 1e30;

        if (allowDirect) {
            for (const Emitter& e : *wk.lights) {
                double t = 0.0, L = 0.0;
                if (hitEmitter(e, o, d, tMax, t, L)) radiance += throughput * L;
            }
        }
        if (!hit) {
            // Out of the scene and into the room.
            //
            // A uniform surround is reached only by sampling, which is why
            // it needs no flag to keep it from being counted twice: nothing
            // ever connects to it. A *map* is connected to at every
            // scattering vertex, so where a path arrives here from one of
            // those the surround has already been paid for and adding it
            // again would double it. `allowDirect` is the same flag that
            // already answers this question for the emitters.
            if (allowDirect || !wk.envMap || wk.envMap->empty())
                fromEnv += throughput * environmentAlong(wk, d);
            break;
        }

        const SceneSurface& surf = (*wk.surfs)[std::size_t(scene.surfaceIndexOf(h))];
        const int  surfIdx = scene.surfaceIndexOf(h);
        const Vec3 triN    = scene.geometricNormal(h);
        const Vec3 p       = o + d * h.t;

        // Beer-Lambert over the leg just travelled, in whichever medium the
        // path was inside. Attenuation is symmetric, so it applies to a camera
        // path exactly as it does to a source path.
        if (wk.physics.absorption && medium >= 0) {
            const double alpha = (*wk.surfs)[std::size_t(medium)].absorption *
                                 wk.physics.absorptionScale;
            if (alpha > 0.0) throughput *= std::exp(-alpha * h.t);
        }
        if (throughput <= 0.0) break;

        // A receiver is a measurement surface, not an object: it does not
        // occlude, absorb or scatter, so a camera looking through one sees what
        // is behind it rather than a black rectangle.
        if (surf.isDetector) {
            o = p + d * eps;
            continue;
        }

        const bool geoLeaving = (d.dot(triN) > 0.0);
        Vec3 ng = geoLeaving ? -triN : triN;
        if (!ng.normalize()) break;
        Vec3 n = scene.shadingNormal(h);
        if (geoLeaving) n = -n;
        if (n.dot(d) > 0.0 || !n.normalize()) n = ng;

        const bsdf::Surface& lobe = surf.bsdf;
        const double cosI = std::clamp(-d.dot(n), 0.0, 1.0);

        double n1 = 1.0, n2 = 1.0;
        int    afterMed = medium;
        MediumStack afterMedia = media;
        const bool refracts = surf.index > 0.0;
        if (refracts) {
            const double nSurf = surf.indexAt(wk.lambda, wk.physics.dispersion);
            const bool onStack = media.contains(surfIdx);
            const bool leaving = onStack || (medium >= 0 && geoLeaving);
            if (leaving) afterMedia.pop(onStack ? surfIdx : medium);
            else         afterMedia.push(surfIdx);
            afterMed = afterMedia.top();
            n1 = (medium >= 0) ? mediumIndexOf(wk, medium) : (leaving ? nSurf : 1.0);
            n2 = mediumIndexOf(wk, afterMed);
        } else {
            n1 = mediumIndexOf(wk, medium);
        }

        const double R = std::clamp(
            reflectanceAt(surf, wk.physics, n1,
                          refracts ? n2 : surf.material.indexAt(wk.lambda), cosI,
                          wk.lambda), 0.0, 1.0);
        const Vec3 specular = reflect(d, n);

        // Everything the emitters deliver here, before a direction is chosen.
        if (!lobe.isSpecular())
            radiance += throughput *
                        connectToLights(wk, p, ng, specular, lobe, R, surfIdx, rng);
        // And the room, which is in cd/m^2 already and so joins the other
        // accumulator rather than this one.
        if (!lobe.isSpecular())
            fromEnv += throughput * connectToEnvironment(wk, p, ng, specular, lobe,
                                                         R, surfIdx, rng);

        // Which way the path goes on. One branch, sampled in proportion to the
        // energy it carries, exactly as the preview backend does it: following
        // both would double the tree at every refractive vertex and a camera
        // path is already long.
        Vec3   outDir;
        double survive = 1.0;
        bool   specularVertex = lobe.isSpecular();
        bool   transmitted = false;

        if (refracts) {
            const double T = 1.0 - R;
            const bool takeRefl = (uniform01(rng) < R) || T <= 0.0;
            if (takeRefl) {
                outDir = specular;
            } else {
                Vec3 tD;
                if (!refract(d, n, n1 / n2, cosI, tD)) {
                    outDir = specular;               // total internal reflection
                } else {
                    if (tD.dot(ng) >= 0.0 &&
                        !refract(d, ng, n1 / n2, std::clamp(-d.dot(ng), 0.0, 1.0), tD)) {
                        if (wk.truncated) *wk.truncated += throughput;
                        break;
                    }
                    outDir = tD;
                    transmitted = true;
                }
            }
            // The weight is not scaled: sampling one branch with the
            // probability of its own share is an unbiased estimator of both.
        } else {
            if (uniform01(rng) >= R) break;          // absorbed
            outDir = specular;
        }

        if (!lobe.isSpecular()) {
            // A redistribution lobe about the side the ray leaves on. The
            // connection above already carried the emitters; this carries
            // everything else the surface can see.
            const Vec3 lobeN = transmitted ? -ng : ng;
            Vec3 sampled;
            double w = 1.0;
            if (lobe.sample(d, lobeN, outDir, rng, sampled, w)) {
                outDir = sampled;
                throughput *= w;
            }
            specularVertex = false;
        }

        if (!outDir.normalize()) {
            if (wk.truncated) *wk.truncated += throughput;
            break;
        }
        if (transmitted) { media = afterMedia; medium = afterMed; }
        allowDirect = specularVertex;

        o = p + ng * std::copysign(surfaceEps(p), outDir.dot(ng));
        d = outDir;

        // Russian roulette on what is left. Below the threshold every path
        // continues; past it a path survives with the probability of its own
        // throughput, and the survivor is scaled up to carry what the killed
        // ones would have.
        if (depth >= wk.rouletteDepth) {
            survive = std::clamp(throughput, 0.02, 1.0);
            if (uniform01(rng) >= survive) break;
            throughput /= survive;
        }
        if (throughput <= 1e-9) break;
    }
    return radiance * wk.photoWeight * kPerMm2ToPerM2 + fromEnv;
}

} // namespace

CameraConfig defaultView(const TraceScene& scene, int width, int height) {
    CameraConfig c;
    c.width  = std::max(1, width);
    c.height = std::max(1, height);

    const Vec3   centre = scene.boundsCenter();
    const double radius = std::max(1.0, scene.boundsRadius());
    c.target = centre;
    // Three-quarter on and a little above: an optic seen straight down its own
    // axis is a circle, and a circle says nothing about what it does.
    Vec3 dir(-0.75, -1.0, 0.45);
    dir.normalize();
    c.eye = centre - dir * (3.0 * radius);
    c.up  = Vec3(0, 0, 1);

    // A focal length that puts the bounding sphere across the frame with a
    // little air around it.
    c.sensorWidthMm = 36.0;
    c.focalLengthMm = 36.0 * (3.0 * radius) / (2.4 * radius);
    c.fNumber = 0.0;                          // a measurement wants everything sharp
    return c;
}

void render(const TraceScene& scene, const std::vector<SceneSurface>& surfsIn,
            const std::vector<SourceConfig>& srcs, const CameraConfig& cam,
            const PhysicsOptions& physics, FluxUnit unit,
            LuminanceImage& out, const RenderControl& ctl) {
    out = LuminanceImage{};
    if (!cam.valid()) return;
    // A room is a light. This used to refuse a scene with no analytic
    // sources, which was right while the only surround was a decorative
    // one -- and is wrong now that an environment map is an emitter in its
    // own right. A daylit interior with no lamps in it is a scene, and
    // returning an empty image for it would be answering a different
    // question. What is still refused is a scene with no light at all,
    // which has no answer to give.
    const bool hasRoom = (cam.environmentMap && !cam.environmentMap->empty()) ||
                         cam.environmentLuminance > 0.0;
    if (srcs.empty() && !hasRoom) return;

    const auto t0 = std::chrono::steady_clock::now();

    // The same resolution the forward tracer makes, through the same function:
    // `scatter` and `roughness` are shorthands a scene is written in, and two
    // integrators that read them differently are not measuring the same scene.
    std::vector<SceneSurface> surfs = surfsIn;
    resolveScattering(surfs, physics);

    std::vector<Emitter> lights;
    lights.reserve(srcs.size());
    double totalPower = 0.0;
    for (const SourceConfig& s : srcs) {
        lights.push_back(resolve(s, unit));
        totalPower += s.power;
    }
    if (totalPower <= 0.0 && !hasRoom) return;

    const CameraFrame frame = frameOf(cam);
    const std::size_t pixels = std::size_t(cam.width) * std::size_t(cam.height);
    out.width  = cam.width;
    out.height = cam.height;
    out.luminance.assign(pixels, 0.0);
    out.stdErr.assign(pixels, 0.0);
    out.samples.assign(pixels, 0u);

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned nThreads = cam.threads > 0 ? cam.threads : hw;

    // Rows are dealt to workers by a fixed stride, so which worker renders
    // which row cannot reach the answer.
    auto lastPartial = std::chrono::steady_clock::now();
    std::vector<double> truncatedPer(nThreads, 0.0);
    std::vector<double> emittedPer(nThreads, 0.0);

    // Sample by sample over the whole frame, not pixel by pixel.
    //
    // Both orders give the same image, and only one of them gives it *while it
    // runs*. Finishing each pixel before starting the next draws a picture from
    // the top down and a viewer who stops it early has three quarters of a
    // scene; a pass over every pixel at once gives a noisy whole picture that
    // sharpens, which is what the forward tracer's partial results already do
    // and what makes a long render worth watching.
    //
    // The running sums are per pixel and each sample is seeded from the pixel
    // index and the sample index alone, so the answer does not depend on the
    // order either -- and the thread-count test says so.
    std::vector<double> sum(pixels, 0.0), sumSq(pixels, 0.0);

    for (int pass = 0; pass < cam.samplesPerPixel; ++pass) {
        if (ctl.cancel && ctl.cancel->load()) break;

        ThreadPool::shared().runParallel(nThreads, [&](unsigned worker) {
            Walk wk;
            wk.scene   = &scene;
            wk.surfs   = &surfs;
            wk.lights  = &lights;
            wk.physics = physics;
            wk.maxDepth      = std::max(1, cam.maxDepth);
            wk.rouletteDepth = std::max(0, cam.rouletteDepth);
            wk.truncated     = &truncatedPer[worker];
            wk.envSky        = std::max(0.0, cam.environmentLuminance);
            wk.envFloor      = wk.envSky * std::clamp(cam.environmentFloor, 0.0, 1.0);
            wk.envUp         = cam.environmentUp.normalized();
            wk.envMap        = cam.environmentMap.get();
            wk.envMapScale   = cam.environmentMapScale;
            orthonormalBasis(wk.envUp, wk.envE1, wk.envE2);

            for (int y = int(worker); y < cam.height; y += int(nThreads)) {
                for (int x = 0; x < cam.width; ++x) {
                    const std::size_t idx = std::size_t(y) * std::size_t(cam.width) +
                                            std::size_t(x);
                    // Seeded from where and which sample, never from who is
                    // rendering it or in what order.
                    std::uint64_t rng = optics::mix64(
                        cam.seed ^ (std::uint64_t(idx) * 0x9e3779b97f4a7c15ull) ^
                        (std::uint64_t(pass) * 0xbf58476d1ce4e5b9ull));

                    // One wavelength per path, drawn from the power-weighted
                    // mixture of the sources. A run in lumens is photometric
                    // already; a run in watts is weighted by V(lambda) and
                    // scaled by 683 on the way out, so the answer is cd/m^2
                    // either way.
                    if (lights.empty()) {
                        // Lit only by the room, which is in cd/m^2 already and
                        // never passes through the photometric weighting. The
                        // wavelength still matters -- it is what the scene's own
                        // dispersion is evaluated at -- so it takes the d line,
                        // the same default a monochromatic run takes.
                        wk.lambda      = 587.6;
                        wk.photoWeight = 1.0;
                    } else {
                        double pick = uniform01(rng) * totalPower, acc = 0.0;
                        std::size_t li = 0;
                        for (; li + 1 < lights.size(); ++li) {
                            acc += srcs[li].power;
                            if (pick < acc) break;
                        }
                        const Emitter& le = lights[li];
                        wk.lambda = le.spectrum.sample(
                            idx * std::size_t(cam.samplesPerPixel) + std::size_t(pass),
                            uniform01(rng));
                        wk.photoWeight = (unit == FluxUnit::Lumen)
                                             ? le.spectrum.photometricWeight(wk.lambda)
                                             : 683.0 * spectrum::photopic(wk.lambda);
                    }

                    Vec3 o, d;
                    cameraRay(frame, cam, x, y, pass, rng, o, d);
                    const double L = radianceAlong(wk, o, d, rng);
                    sum[idx]   += L;
                    sumSq[idx] += L * L;
                    emittedPer[worker] += 1.0;
                }
            }
        });

        const double n = double(pass + 1);
        for (std::size_t i = 0; i < pixels; ++i) {
            const double mean = sum[i] / n;
            const double var  = std::max(0.0, sumSq[i] / n - mean * mean);
            out.luminance[i] = mean;
            out.stdErr[i]    = n > 1.0 ? std::sqrt(var / n) : 0.0;
            out.samples[i]   = std::uint32_t(pass + 1);
        }
        if (ctl.progress) ctl.progress(std::size_t(pass + 1),
                                       std::size_t(cam.samplesPerPixel));
        if (ctl.partial) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPartial)
                        .count() >= ctl.partialIntervalMs ||
                pass + 1 == cam.samplesPerPixel) {
                lastPartial = now;
                ctl.partial(out);
            }
        }
    }

    double truncated = 0.0, emitted = 0.0;
    for (unsigned i = 0; i < nThreads; ++i) {
        truncated += truncatedPer[i];
        emitted   += emittedPer[i];
    }
    out.truncatedFraction = emitted > 0.0 ? truncated / emitted : 0.0;

    double peak = 0.0, mean = 0.0, logSum = 0.0;
    for (double v : out.luminance) {
        peak = std::max(peak, v);
        mean += v;
        // The log mean is the number a glare index is built on, and it is not
        // recoverable from the arithmetic mean. A zero pixel is floored rather
        // than dropped, so a dark scene has a small log mean instead of none.
        logSum += std::log(std::max(v, 1e-6));
    }
    if (!out.luminance.empty()) {
        mean   /= double(out.luminance.size());
        logSum /= double(out.luminance.size());
    }
    out.peak    = peak;
    out.mean    = mean;
    out.logMean = std::exp(logSum);
    out.seconds = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();
}

} // namespace backward
