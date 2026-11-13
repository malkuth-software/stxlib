#pragma once

// UTF-8 well-formedness, as the Unicode Standard's Table 3-7 ("Well-Formed
// UTF-8 Byte Sequences", Unicode 15.1, section 3.9) defines it:
//
//   first byte   second   third    fourth
//   00..7F
//   C2..DF       80..BF
//   E0           A0..BF   80..BF
//   E1..EC       80..BF   80..BF
//   ED           80..9F   80..BF
//   EE..EF       80..BF   80..BF
//   F0           90..BF   80..BF   80..BF
//   F1..F3       80..BF   80..BF   80..BF
//   F4           80..8F   80..BF   80..BF
//
// The one implementation behind TextView's compile-time literal check,
// validate_utf8 and every cut of text to a capacity. It is constexpr so a
// literal is checked by the compiler, and header-only so both uses are the
// same code. tests/utf8_exhaustive.cpp checks it against an independent
// decoder over every byte string up to four bytes (proofs/REPORT.md).
//
// Every loop below advances by at least one byte per step and stops at the
// count, so it runs at most `count` times.

#include "stxlib/types.hpp"

namespace stxlib::detail {

// The shape a first byte allows: the sequence length (0 for a byte that
// cannot start a sequence) and the range of the second byte.
struct Utf8Lead final {
    u8 length{0};
    u8 second_low{0};
    u8 second_high{0};
};

[[nodiscard]] constexpr Utf8Lead utf8_lead(const u8 byte) noexcept {
    if (byte <= 0x7f) return Utf8Lead{1, 0, 0};
    if (byte < 0xc2) return Utf8Lead{0, 0, 0};
    if (byte <= 0xdf) return Utf8Lead{2, 0x80, 0xbf};
    if (byte == 0xe0) return Utf8Lead{3, 0xa0, 0xbf};
    if (byte <= 0xec) return Utf8Lead{3, 0x80, 0xbf};
    if (byte == 0xed) return Utf8Lead{3, 0x80, 0x9f};
    if (byte <= 0xef) return Utf8Lead{3, 0x80, 0xbf};
    if (byte == 0xf0) return Utf8Lead{4, 0x90, 0xbf};
    if (byte <= 0xf3) return Utf8Lead{4, 0x80, 0xbf};
    if (byte == 0xf4) return Utf8Lead{4, 0x80, 0x8f};
    return Utf8Lead{0, 0, 0};
}

// The length of the well-formed sequence starting at bytes[index], or 0 when
// the bytes there are ill-formed or the sequence is cut off by `count`.
// Requirement (by every caller below): index < count.
[[nodiscard]] constexpr usize utf8_sequence_length(const char* const bytes, const usize count,
                                                   const usize index) noexcept {
    const u8 first = static_cast<u8>(bytes[index]);
    const Utf8Lead shape = utf8_lead(first);
    if (shape.length == 0) return 0;
    const usize available = count - index;
    if (shape.length > available) return 0;
    if (shape.length == 1) return 1;
    const u8 second = static_cast<u8>(bytes[index + 1]);
    if (second < shape.second_low || second > shape.second_high) return 0;
    for (usize offset = 2; offset < shape.length; ++offset) {
        const u8 next = static_cast<u8>(bytes[index + offset]);
        if (next < 0x80 || next > 0xbf) return 0;
    }
    const usize result = shape.length;
    return result;
}

// The length of the longest prefix of bytes[0, count) that is well-formed
// and ends at a sequence boundary at or before `limit`. With limit >= count
// it is the well-formed prefix; with a smaller limit it is the cut of text
// to a capacity that never splits a sequence.
[[nodiscard]] constexpr usize utf8_prefix(const char* const bytes, const usize count, const usize limit) noexcept {
    usize index = 0;
    while (index < count) {
        const usize length = utf8_sequence_length(bytes, count, index);
        if (length == 0) break;
        if (length > limit - index) break;
        index += length;
        if (index == limit) break;
    }
    return index;
}

[[nodiscard]] constexpr bool utf8_well_formed(const char* const bytes, const usize count) noexcept {
    const usize prefix = utf8_prefix(bytes, count, count);
    const bool result = prefix == count;
    return result;
}

// Not constexpr: calling it during constant evaluation stops compilation,
// which is how TextView rejects a literal that is not well-formed UTF-8.
void literal_is_not_utf8() noexcept;

} // namespace stxlib::detail
