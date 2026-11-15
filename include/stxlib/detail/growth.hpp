#pragma once

// The growth policy of the owned vocabulary's growable storage (Vector,
// Utf8String), alone in a header that includes only types.hpp, so that a
// model checker can take it without the rest of the library
// (proofs/esbmc/vocabulary_growth.cpp).

#include "stxlib/types.hpp"

namespace stxlib::detail {

// The capacity a vector moves to when it holds `current` and needs room for
// `wanted` elements of `element_size` bytes; 0 when wanted * element_size is
// not representable in Unsigned. Otherwise the result is at least wanted, at
// least current, and at most the representable maximum: current when it
// already suffices; else the larger of current and 4, doubled until it
// reaches wanted, or exactly wanted once a doubling would leave the
// representable range. A template so that the same code can be checked
// exhaustively over 8-bit operands (tests/owned_vocabulary_tests.cpp) and by
// a model checker over 64-bit ones (proofs/esbmc/vocabulary_growth.cpp).
// Requirement: element_size > 0 and current * element_size representable.
template <typename Unsigned>
[[nodiscard]] constexpr Unsigned grown_capacity(const Unsigned current, const Unsigned wanted,
                                                const Unsigned element_size) noexcept {
    const Unsigned all_ones = static_cast<Unsigned>(~static_cast<Unsigned>(0));
    const Unsigned maximum = static_cast<Unsigned>(all_ones / element_size);
    if (wanted > maximum) return static_cast<Unsigned>(0);
    if (wanted <= current) return current;
    const Unsigned minimum = static_cast<Unsigned>(4);
    Unsigned next = current < minimum ? minimum : current;
    if (next > maximum) next = maximum;
    const Unsigned half_maximum = static_cast<Unsigned>(maximum / static_cast<Unsigned>(2));
    while (next < wanted) {
        if (next > half_maximum) {
            next = wanted;
            break;
        }
        next = static_cast<Unsigned>(next * static_cast<Unsigned>(2));
    }
    return next;
}

} // namespace stxlib::detail
