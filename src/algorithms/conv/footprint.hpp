#pragma once

// Footprint quadrature along one axis (stxlib::conv canon on axis maps and lines): a target
// sample's footprint [k s - 1/2, (k + 1) s - 1/2], s = source / target, cut at source cells,
// three-point Gauss on each piece, exact for the quintic cells.

#include "line.hpp"

#include "stxlib/types.hpp"

namespace stxlib::conv::detail {

// Gauss-Legendre, three points on [-1, 1].
inline constexpr double gauss_x[3]{-0.7745966692414834, 0.0, 0.7745966692414834};
inline constexpr double gauss_w[3]{5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};

// A quadrature node of a footprint: its source cell, phase and weight.
struct FootprintNode final {
    usize cell{0};
    double t{0.0};
    double weight{0.0};
};

// Writes the nodes of target pixel `index`'s footprint [index s - 1/2, (index + 1) s - 1/2]
// (s = source / target), cut at source cells, into nodes (footprint_room); returns their count.
inline usize footprint(const usize index, const usize source, const usize target, FootprintNode* nodes) noexcept {
    const double scale = static_cast<double>(source) / static_cast<double>(target);
    const double left = static_cast<double>(index) * scale - 0.5;
    const double right = static_cast<double>(index + 1) * scale - 0.5;
    const double length = right - left;
    usize count = 0;
    double a = left;
    while (a < right) {
        // floor(a): a >= -1/2, so -1 below zero and truncation above.
        const double whole = a < 0.0 ? -1.0 : static_cast<double>(static_cast<u64>(a));
        double b = whole + 1.0;
        if (b > right) b = right;
        const double half = 0.5 * (b - a);
        const double mid = 0.5 * (a + b);
        for (usize k = 0; k < 3; ++k) {
            FootprintNode node;
            locate(mid + half * gauss_x[k], source - 1, node.cell, node.t);
            node.weight = gauss_w[k] * half / length;
            nodes[count] = node;
            count = count + 1;
        }
        a = b;
    }
    return count;
}

inline usize footprint_room(const usize source, const usize target) noexcept {
    const usize pieces = source / target + 3;
    return 3 * pieces;
}

} // namespace stxlib::conv::detail
