#include "Sampling.h"

namespace sampling {

namespace {

// Primitive polynomial degree, its coefficient word, and the initial direction
// numbers, for Sobol dimensions 1..7 (dimension 0 is the van der Corput
// sequence and needs none). Joe & Kuo, "Constructing Sobol sequences with
// better two-dimensional projections".
struct Poly {
    int           degree;
    std::uint32_t a;          // polynomial coefficients, excluding both ends
    std::uint32_t m[5];       // initial values, `degree` of them
};

constexpr Poly kPolys[kMaxDim - 1] = {
    {1, 0, {1, 0, 0, 0, 0}},
    {2, 1, {1, 3, 0, 0, 0}},
    {3, 1, {1, 3, 1, 0, 0}},
    {3, 2, {1, 1, 1, 0, 0}},
    {4, 1, {1, 1, 3, 3, 0}},
    {4, 4, {1, 3, 5, 13, 0}},
    {5, 2, {1, 1, 5, 5, 17}},
};

} // namespace

DirectionNumbers::DirectionNumbers() {
    // Dimension 0: v[k] = 2^(31-k), which makes the sequence the radical
    // inverse in base 2.
    for (int k = 0; k < 32; ++k)
        v[0][k] = 1u << (31 - k);

    for (int d = 1; d < kMaxDim; ++d) {
        const Poly& p = kPolys[d - 1];
        std::uint32_t m[32] = {};
        for (int k = 0; k < p.degree; ++k)
            m[k] = p.m[k];
        // The recurrence: m_k = 2 a_1 m_{k-1} ^ 4 a_2 m_{k-2} ^ ... ^
        //                       2^s m_{k-s} ^ m_{k-s}
        for (int k = p.degree; k < 32; ++k) {
            std::uint32_t value = m[k - p.degree];
            value ^= value << p.degree;
            for (int j = 1; j < p.degree; ++j)
                if ((p.a >> (p.degree - 1 - j)) & 1u)
                    value ^= m[k - j] << j;
            m[k] = value;
        }
        for (int k = 0; k < 32; ++k)
            v[d][k] = m[k] << (31 - k);
    }
}

const DirectionNumbers& directions() {
    // Function-local static: built once, thread-safe initialisation, and never
    // written again, so every worker reads it without synchronisation.
    static const DirectionNumbers d;
    return d;
}

} // namespace sampling
