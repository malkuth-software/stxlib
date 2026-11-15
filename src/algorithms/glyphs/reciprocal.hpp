// stxlib::glyphs: rounded-down quotients by small divisors without a
// division (src/algorithms/glyphs/mask.cpp uses them for the chords of
// text, which are short).
//
// of[d] = ceil(2^36 / d) for the divisors 1 to 1023, and of[0] = 0, which
// stands for "divide".
//
// Why multiplying gives the quotient. of[d] * d = 2^36 + e with 0 <= e < d,
// so n * of[d] / 2^36 = n / d + n * e / (d * 2^36). A quotient n / d falls
// short of the next integer by at least 1 / d, so its floor is unchanged
// when n * e < 2^36. e is at most 1022 and (2^26 + 1024) * 1022 < 2^36: it
// holds for every dividend up to brief_dividend. reciprocals_hold checks
// exactly this, for every divisor, when the file is compiled.

#ifndef STXLIB_ALGORITHMS_GLYPHS_RECIPROCAL_HPP
#define STXLIB_ALGORITHMS_GLYPHS_RECIPROCAL_HPP

#include "stxlib/types.hpp"

namespace stxlib::glyphs::detail {

// Four pixels in 1/256 of a pixel: the divisors the table holds are below.
inline constexpr i64 brief_extent = 1024;
inline constexpr u32 reciprocal_bits = 36;
// The greatest dividend the table is exact for.
inline constexpr u64 brief_dividend = (static_cast<u64>(1) << 26) + 1024;

struct Reciprocals final {
    u64 of[brief_extent]{};
    constexpr Reciprocals() noexcept {
        const u64 unit = static_cast<u64>(1) << reciprocal_bits;
        for (u64 divisor = 1; divisor < static_cast<u64>(brief_extent); ++divisor) {
            of[divisor] = (unit + divisor - 1) / divisor;
        }
    }
};
inline constexpr Reciprocals reciprocals{};

[[nodiscard]] constexpr bool reciprocals_hold() noexcept {
    const u64 unit = static_cast<u64>(1) << reciprocal_bits;
    bool hold = true;
    for (u64 divisor = 1; divisor < static_cast<u64>(brief_extent); ++divisor) {
        const u64 excess = reciprocals.of[divisor] * divisor - unit;
        if (excess >= divisor || brief_dividend * excess >= unit) hold = false;
        // The product itself stays inside 64 bits.
        if (reciprocals.of[divisor] > (~static_cast<u64>(0)) / brief_dividend) hold = false;
    }
    return hold;
}
static_assert(reciprocals_hold());

// dividend / divisor rounded down. Requirement: dividend >= 0, divisor > 0,
// and `reciprocal` either 0 or the table's entry for the divisor with the
// dividend no more than brief_dividend.
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
