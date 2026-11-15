#pragma once

// The pure core of simple case folding (include/stxlib/case_folding.hpp):
// the table lookup, and decoding and encoding of well-formed UTF-8. Header
// only, so the ESBMC harnesses (proofs/esbmc/case_folding.cpp) and the
// tests read the same code the library runs. Every loop is bounded by its
// input: the lookup halves [low, high) each step (at most 11 steps for
// 1,533 entries), the text loops advance by one sequence each step.

#include "stxlib/types.hpp"

namespace stxlib::case_folding_core {

#include "algorithms/case_folding_table.inc"

inline constexpr u32 highest_scalar = 0x10ffffu;
inline constexpr u32 surrogate_low = 0xd800u;
inline constexpr u32 surrogate_high = 0xdfffu;

// The index of `code_point` among the table's sources, or case_folding_count
// when it is not one.
[[nodiscard]] constexpr usize source_index(const u32 code_point) noexcept {
    usize low = 0;
    usize high = case_folding_count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const u32 candidate = case_folding_sources[middle];
        if (candidate == code_point) return middle;
        if (candidate < code_point) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return case_folding_count;
}

[[nodiscard]] constexpr u32 fold(const u32 code_point) noexcept {
    // ASCII: only A-Z fold (checked by the generator).
    if (code_point < 0x80u) {
        if (code_point >= 0x41u && code_point <= 0x5au) return code_point + 0x20u;
        return code_point;
    }
    if (code_point > case_folding_sources[case_folding_count - 1]) return code_point;
    const usize index = source_index(code_point);
    if (index == case_folding_count) return code_point;
    return case_folding_targets[index];
}

struct Decoded final {
    u32 scalar{0};
    usize length{0};
};

// The scalar value whose sequence starts at bytes[index]. Requirement:
// bytes[0, count) is well-formed UTF-8 and index < count lies on a sequence
// boundary (a TextView's bytes, walked from 0). A byte that cannot start a
// sequence, or a sequence cut off by `count`, which the requirement rules
// out, decodes as that one byte so the walk still advances and stays in
// bounds.
[[nodiscard]] constexpr Decoded decode(const char* const bytes, const usize count, const usize index) noexcept {
    const u32 first = static_cast<u8>(bytes[index]);
    if (first < 0x80u) return Decoded{first, 1};
    usize length = 0;
    u32 scalar = 0;
    if (first >= 0xc2u && first <= 0xdfu) {
        length = 2;
        scalar = first & 0x1fu;
    } else if (first >= 0xe0u && first <= 0xefu) {
        length = 3;
        scalar = first & 0x0fu;
    } else if (first >= 0xf0u && first <= 0xf4u) {
        length = 4;
        scalar = first & 0x07u;
    } else {
        return Decoded{first, 1};
    }
    if (length > count - index) return Decoded{first, 1};
    for (usize offset = 1; offset < length; ++offset) {
        const u32 next = static_cast<u8>(bytes[index + offset]);
        scalar = (scalar << 6) | (next & 0x3fu);
    }
    return Decoded{scalar, length};
}

[[nodiscard]] constexpr usize encoded_length(const u32 scalar) noexcept {
    if (scalar < 0x80u) return 1;
    if (scalar < 0x800u) return 2;
    if (scalar < 0x10000u) return 3;
    return 4;
}

// Writes the UTF-8 form of `scalar` (a scalar value) to output[0, length).
// Requirement: output holds encoded_length(scalar) bytes.
constexpr void encode(const u32 scalar, char* const output) noexcept {
    if (scalar < 0x80u) {
        output[0] = static_cast<char>(scalar);
    } else if (scalar < 0x800u) {
        output[0] = static_cast<char>(0xc0u | (scalar >> 6));
        output[1] = static_cast<char>(0x80u | (scalar & 0x3fu));
    } else if (scalar < 0x10000u) {
        output[0] = static_cast<char>(0xe0u | (scalar >> 12));
        output[1] = static_cast<char>(0x80u | ((scalar >> 6) & 0x3fu));
        output[2] = static_cast<char>(0x80u | (scalar & 0x3fu));
    } else {
        output[0] = static_cast<char>(0xf0u | (scalar >> 18));
        output[1] = static_cast<char>(0x80u | ((scalar >> 12) & 0x3fu));
        output[2] = static_cast<char>(0x80u | ((scalar >> 6) & 0x3fu));
        output[3] = static_cast<char>(0x80u | (scalar & 0x3fu));
    }
}

// The bytes the folding of bytes[0, count) takes (well-formed UTF-8).
[[nodiscard]] constexpr usize folded_bytes(const char* const bytes, const usize count) noexcept {
    usize total = 0;
    usize index = 0;
    while (index < count) {
        const Decoded decoded = decode(bytes, count, index);
        total += encoded_length(fold(decoded.scalar));
        index += decoded.length;
    }
    return total;
}

// Writes the folding of bytes[0, count) to output. Requirement: output holds
// folded_bytes(bytes, count) bytes and does not overlap the input.
constexpr void fold_into(const char* const bytes, const usize count, char* const output) noexcept {
    usize index = 0;
    usize written = 0;
    while (index < count) {
        const Decoded decoded = decode(bytes, count, index);
        const u32 folded = fold(decoded.scalar);
        encode(folded, output + written);
        written += encoded_length(folded);
        index += decoded.length;
    }
}

// -1, 0 or 1: the order of the two texts' foldings, scalar by scalar, a
// proper prefix first.
[[nodiscard]] constexpr int compare(const char* const left, const usize left_count, const char* const right,
                                    const usize right_count) noexcept {
    usize left_index = 0;
    usize right_index = 0;
    while (left_index < left_count && right_index < right_count) {
        const Decoded a = decode(left, left_count, left_index);
        const Decoded b = decode(right, right_count, right_index);
        const u32 folded_a = fold(a.scalar);
        const u32 folded_b = fold(b.scalar);
        if (folded_a != folded_b) return folded_a < folded_b ? -1 : 1;
        left_index += a.length;
        right_index += b.length;
    }
    const bool left_done = left_index >= left_count;
    const bool right_done = right_index >= right_count;
    if (left_done && right_done) return 0;
    return left_done ? -1 : 1;
}

} // namespace stxlib::case_folding_core
