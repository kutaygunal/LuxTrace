#pragma once

// Optical behaviour of one surface, shared verbatim by the three
// representations a surface passes through: OpticalSurface (B-Rep + optics),
// MeshSurface (tessellated) and SceneSurface (flattened for the BVH).
//
// The three used to declare the same four fields each and copy them across by
// hand twice. Inheriting instead means a new optical property is declared once
// and reaches the tracer without touching either conversion loop.
struct SurfaceOptics {
    // Fixed split, used when `fresnel` is off. Whatever is neither reflected
    // nor transmitted is absorbed at the surface.
    double reflectivity   = 0.0;
    double transmissivity = 0.0;

    // Refractive index at the d line (587.6 nm). 0 == opaque.
    double index = 0.0;

    // Cauchy dispersion coefficient B, in um^2: n(lambda) = A + B / lambda^2,
    // with A fixed so that n(587.6 nm) == index. 0 == non-dispersive.
    // BK7-like crown glass is about 0.00420 um^2 (Abbe number ~64).
    double dispersionB = 0.0;

    // Beer-Lambert bulk attenuation of the medium *behind* this surface, 1/mm.
    // Only consulted while a ray is travelling inside the solid.
    double absorption = 0.0;

    // Fraction of the reflected (and of the transmitted) energy that leaves
    // cosine-weighted about the surface normal instead of specularly.
    // 0 is a polished surface, 1 a perfect Lambertian diffuser.
    double scatter = 0.0;

    // RMS surface slope error, radians. The interaction normal is jittered by a
    // Gaussian of this width before the laws are applied, so a mirror spreads a
    // reflected ray by twice it -- which is what a real polish tolerance does.
    double roughness = 0.0;

    // Angle-dependent reflectance from the Fresnel equations instead of the
    // fixed reflectivity/transmissivity split. Only meaningful when index > 0;
    // an opaque mirror keeps its fixed reflectivity either way.
    bool fresnel = false;

    bool isDetector = false;
};
