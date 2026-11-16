// stxlib::glyphs: a digest of a set of glyphs, to say fast that a glyph is
// not in the set. After harfrust 0.16.0, src/set_digest.rs (HarfBuzz's
// hb_set_digest_t): three masks of 64 bits, each a glyph number's six bits
// from a different place. A glyph in the set has its bit in all three; a
// glyph with a bit missing in any is not in the set. Nothing is ever said
// to be absent that is present, so a lookup skipped by its digest could not
// have applied.

#ifndef STXLIB_ALGORITHMS_GLYPHS_SHAPE_DIGEST_HPP
#define STXLIB_ALGORITHMS_GLYPHS_SHAPE_DIGEST_HPP

#include "stxlib/types.hpp"

namespace stxlib::glyphs::detail {

struct GlyphDigest final {
    u64 masks[3]{0, 0, 0};
};

inline constexpr u32 digest_shifts[3] = {4, 0, 6};
inline constexpr u64 digest_all = ~static_cast<u64>(0);

constexpr void digest_add(GlyphDigest& digest, const u32 glyph) noexcept {
    for (usize index = 0; index < 3; ++index) digest.masks[index] = digest.masks[index] | (static_cast<u64>(1) << ((glyph >> digest_shifts[index]) & 63U));
}

// Every glyph from `first` to `last` (first <= last).
constexpr void digest_add_range(GlyphDigest& digest, const u32 first, const u32 last) noexcept {
    for (usize index = 0; index < 3; ++index) {
        const u32 low = first >> digest_shifts[index];
        const u32 high = last >> digest_shifts[index];
        if (high - low >= 63U) {
            digest.masks[index] = digest_all;
        } else {
            // The bits from low's to high's, going round past 63.
            const u32 from = low & 63U;
            const u32 to = high & 63U;
            const u64 below_from = (static_cast<u64>(1) << from) - 1;
            const u64 through_to = to == 63U ? digest_all : (static_cast<u64>(1) << (to + 1)) - 1;
            const u64 bits = from <= to ? (through_to & ~below_from) : ~(below_from & ~through_to);
            digest.masks[index] = digest.masks[index] | bits;
        }
    }
}

constexpr void digest_fill(GlyphDigest& digest) noexcept {
    for (usize index = 0; index < 3; ++index) digest.masks[index] = digest_all;
}

constexpr void digest_join(GlyphDigest& digest, const GlyphDigest& other) noexcept {
    for (usize index = 0; index < 3; ++index) digest.masks[index] = digest.masks[index] | other.masks[index];
}

[[nodiscard]] constexpr bool digest_may_have(const GlyphDigest& digest, const u32 glyph) noexcept {
    bool result = true;
    for (usize index = 0; index < 3; ++index) result = result && ((digest.masks[index] >> ((glyph >> digest_shifts[index]) & 63U)) & 1U) != 0;
    return result;
}

[[nodiscard]] constexpr bool digest_may_intersect(const GlyphDigest& first, const GlyphDigest& second) noexcept {
    bool result = true;
    for (usize index = 0; index < 3; ++index) result = result && (first.masks[index] & second.masks[index]) != 0;
    return result;
}

} // namespace stxlib::glyphs::detail

#endif
