#pragma once
#include <cstdint>

// Low-discrepancy sampling for the emission stream.
//
// Independent uniforms scatter samples at random, so the error falls as
// 1/sqrt(N) with a constant set by how unevenly they happened to land. An
// Owen-scrambled Sobol sequence lands them evenly in every projection at once,
// and on the smooth integrands a source distribution actually is, the same error
// bar arrives from several times fewer rays.
//
// It costs nothing in determinism: the sequence is indexed by ray number, which
// is exactly what the counter-based RNG was already doing, so a trace stays
// bit-identical at any thread count.
namespace sampling {

// Owen scrambling by hashing (Burley, JCGT 2020). Reversing the bits, running a
// Laine-Karras permutation and reversing back applies a different random
// permutation at every level of the binary tree of intervals -- which is what
// Owen scrambling is, without building the tree.
inline std::uint32_t reverseBits(std::uint32_t x) {
    x = (x << 16) | (x >> 16);
    x = ((x & 0x00ff00ffu) << 8) | ((x & 0xff00ff00u) >> 8);
    x = ((x & 0x0f0f0f0fu) << 4) | ((x & 0xf0f0f0f0u) >> 4);
    x = ((x & 0x33333333u) << 2) | ((x & 0xccccccccu) >> 2);
    x = ((x & 0x55555555u) << 1) | ((x & 0xaaaaaaaau) >> 1);
    return x;
}

inline std::uint32_t laineKarras(std::uint32_t x, std::uint32_t seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

inline std::uint32_t owenScramble(std::uint32_t x, std::uint32_t seed) {
    return reverseBits(laineKarras(reverseBits(x), seed));
}

inline std::uint32_t hashCombine(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Direction numbers for the first eight Sobol dimensions, from the primitive
// polynomials and initial values of Joe & Kuo. Eight is comfortably more than
// the emission stream needs (two angular, two areal, one spectral).
constexpr int kMaxDim = 8;

// Built once, at first use: 8 dimensions x 32 bits of direction numbers.
struct DirectionNumbers {
    std::uint32_t v[kMaxDim][32] = {};
    DirectionNumbers();
};

const DirectionNumbers& directions();

// The `dim`-th coordinate of Sobol point `index`, Owen-scrambled with `seed`.
// Returns a double in [0, 1).
inline double sobol(std::uint32_t index, int dim, std::uint32_t seed) {
    if (dim < 0 || dim >= kMaxDim) {
        // Past the tabulated dimensions, fall back to a hashed value: still
        // deterministic, just no longer stratified.
        return double(hashCombine(index ^ (std::uint32_t(dim) * 0x9e3779b9u) ^ seed)) *
               (1.0 / 4294967296.0);
    }
    // Shuffling the index first decorrelates the sequences of different rays;
    // scrambling the output decorrelates the dimensions.
    const std::uint32_t shuffled = owenScramble(index, hashCombine(seed ^ 0x51633e2du));
    const std::uint32_t* v = directions().v[dim];
    std::uint32_t x = 0;
    std::uint32_t i = shuffled;
    for (int bit = 0; i != 0; ++bit, i >>= 1)
        if (i & 1u) x ^= v[bit];
    x = owenScramble(x, hashCombine(seed ^ (std::uint32_t(dim) * 0x68bc21ebu)));
    // 24 bits is every bit a float has and more than the sequence resolves.
    return double(x >> 8) * (1.0 / 16777216.0);
}

} // namespace sampling
