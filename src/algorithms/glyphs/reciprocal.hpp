// stxlib::glyphs: rounded-down quotients by small divisors without a
// division (src/algorithms/glyphs/chords.hpp uses them for the chords of
// text, which are short).
//
// of[d] = ceil(2^36 / d) for the divisors 1 to 2047, and of[0] = 0, which
// stands for "divide".
//
// Why multiplying gives the quotient. of[d] * d = 2^36 + e with 0 <= e < d,
// so n * of[d] / 2^36 = n / d + n * e / (d * 2^36). A quotient n / d falls
// short of the next integer by at least 1 / d, so its floor is unchanged
// when n * e < 2^36. How large n may be therefore depends on how large d
// may be: reciprocals_exact(n, d) says whether every dividend up to n is
// divided exactly by every divisor up to d, and each use states its own
// two bounds in a static_assert.

#ifndef STXLIB_ALGORITHMS_GLYPHS_RECIPROCAL_HPP
#define STXLIB_ALGORITHMS_GLYPHS_RECIPROCAL_HPP

#include "stxlib/types.hpp"

namespace stxlib::glyphs::detail {

inline constexpr usize reciprocal_count = 2048;
inline constexpr u32 reciprocal_bits = 36;

struct Reciprocals final {
    u64 of[reciprocal_count]{};
    constexpr Reciprocals() noexcept {
        const u64 unit = static_cast<u64>(1) << reciprocal_bits;
        for (u64 divisor = 1; divisor < reciprocal_count; ++divisor) {
            of[divisor] = (unit + divisor - 1) / divisor;
        }
    }
};
inline constexpr Reciprocals reciprocals{};

// Whether every dividend from 0 to `greatest_dividend` is divided exactly,
// by multiplying, by every divisor from 1 to `greatest_divisor`.
[[nodiscard]] constexpr bool reciprocals_exact(const u64 greatest_dividend, const u64 greatest_divisor) noexcept {
    if (greatest_divisor >= reciprocal_count) return false;
    const u64 unit = static_cast<u64>(1) << reciprocal_bits;
    bool exact = true;
    for (u64 divisor = 1; divisor <= greatest_divisor; ++divisor) {
        const u64 excess = reciprocals.of[divisor] * divisor - unit;
        if (excess >= divisor || greatest_dividend * excess >= unit) exact = false;
        // The product itself stays inside 64 bits.
        if (reciprocals.of[divisor] > (~static_cast<u64>(0)) / greatest_dividend) exact = false;
    }
    return exact;
}

// A chord shorter than four pixels both ways (in 1/256 of a pixel): its
// divisors are below brief_extent and its dividends no more than
// brief_dividend.
inline constexpr i64 brief_extent = 1024;
inline constexpr u64 brief_dividend = (static_cast<u64>(1) << 26) + 1024;
static_assert(reciprocals_exact(brief_dividend, static_cast<u64>(brief_extent) - 1));

// dividend / divisor rounded down. Requirement: dividend >= 0, divisor > 0,
// and `reciprocal` either 0 or the table's entry for the divisor, with the
// dividend and the divisor inside bounds reciprocals_exact holds for.
[[nodiscard]] constexpr i64 quotient(const i64 dividend, const i64 divisor, const u64 reciprocal) noexcept {
    if (reciprocal != 0) {
        const i64 multiplied = static_cast<i64>((static_cast<u64>(dividend) * reciprocal) >> reciprocal_bits);
        return multiplied;
    }
    const i64 divided = dividend / divisor;
    return divided;
}

} // namespace stxlib::glyphs::detail

#endif
