// stxlib::markdown: UTF-8 at a position, the Unicode classes the emphasis
// rules ask about, and the scan to a line's end (characters.hpp).

#include "algorithms/markdown/characters.hpp"

namespace stxlib::markdown::detail {

namespace {

struct CodePointRange final {
    u32 first{0};
    u32 last{0};
};

// Generated from the Unicode Character Database (tools/
// gen_markdown_tables.py): whitespace_ranges and punctuation_ranges,
// sorted, disjoint, inclusive, all above U+007F.
#include "algorithms/markdown/unicode_classes.inc"

[[nodiscard]] bool is_continuation(const char byte) noexcept {
    const bool result = (static_cast<u8>(byte) & 0xc0) == 0x80;
    return result;
}

// Whether `code_point` lies in one of `count` sorted inclusive ranges.
[[nodiscard]] bool in_ranges(const CodePointRange* const ranges, const usize count, const u32 code_point) noexcept {
    usize low = 0;
    usize high = count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (code_point < ranges[middle].first) {
            high = middle;
        } else if (code_point > ranges[middle].last) {
            low = middle + 1;
        } else {
            return true;
        }
    }
    return false;
}

inline constexpr u64 low_bits = 0x0101010101010101ULL;
inline constexpr u64 high_bits = 0x8080808080808080ULL;

// Non-zero if a byte of `word` is zero (the exact form: no false positive).
[[nodiscard]] constexpr u64 zero_bytes(const u64 word) noexcept {
    const u64 result = ~(((word & ~high_bits) + ~high_bits) | word | ~high_bits);
    return result;
}

} // namespace

Decoded decode_at(const char* const text, const u32 size, const u32 offset) noexcept {
    const u32 lead = static_cast<u8>(text[offset]);
    if (lead < 0x80) {
        const Decoded ascii{.code_point = lead, .length = 1};
        return ascii;
    }
    const Decoded ill_formed{.code_point = not_a_character, .length = 1};
    const u32 room = size - offset;
    if (lead >= 0xc2 && lead <= 0xdf) {
        if (room < 2 || !is_continuation(text[offset + 1])) return ill_formed;
        const u32 second = static_cast<u8>(text[offset + 1]);
        const Decoded two{.code_point = ((lead & 0x1f) << 6) | (second & 0x3f), .length = 2};
        return two;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
        if (room < 3 || !is_continuation(text[offset + 1]) || !is_continuation(text[offset + 2])) return ill_formed;
        const u32 second = static_cast<u8>(text[offset + 1]);
        const u32 third = static_cast<u8>(text[offset + 2]);
        const u32 value = ((lead & 0x0f) << 12) | ((second & 0x3f) << 6) | (third & 0x3f);
        // Not the shortest form, or a surrogate.
        if (value < 0x800 || (value >= 0xd800 && value <= 0xdfff)) return ill_formed;
        const Decoded three{.code_point = value, .length = 3};
        return three;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
        if (room < 4 || !is_continuation(text[offset + 1]) || !is_continuation(text[offset + 2]) ||
            !is_continuation(text[offset + 3])) {
            return ill_formed;
        }
        const u32 second = static_cast<u8>(text[offset + 1]);
        const u32 third = static_cast<u8>(text[offset + 2]);
        const u32 fourth = static_cast<u8>(text[offset + 3]);
        const u32 value = ((lead & 0x07) << 18) | ((second & 0x3f) << 12) | ((third & 0x3f) << 6) | (fourth & 0x3f);
        if (value < 0x10000 || value > 0x10ffff) return ill_formed;
        const Decoded four{.code_point = value, .length = 4};
        return four;
    }
    return ill_formed;
}

Decoded decode_before(const char* const text, const u32 floor, const u32 offset) noexcept {
    // A sequence is at most four bytes: look back over up to three
    // continuation bytes for its lead, and accept it only if it decodes to
    // exactly the bytes up to `offset`.
    u32 start = offset - 1;
    u32 steps = 0;
    while (start > floor && steps < 3 && is_continuation(text[start])) {
        start = start - 1;
        steps = steps + 1;
    }
    const Decoded forward = decode_at(text, offset, start);
    if (forward.code_point != not_a_character && start + forward.length == offset) return forward;
    const u32 last = static_cast<u8>(text[offset - 1]);
    const Decoded single{.code_point = last < 0x80 ? last : not_a_character, .length = 1};
    return single;
}

u32 encode_utf8(const u32 code_point, char* const out) noexcept {
    if (code_point < 0x80) {
        out[0] = static_cast<char>(code_point);
        return 1;
    }
    if (code_point < 0x800) {
        out[0] = static_cast<char>(0xc0 | (code_point >> 6));
        out[1] = static_cast<char>(0x80 | (code_point & 0x3f));
        return 2;
    }
    if (code_point < 0x10000) {
        out[0] = static_cast<char>(0xe0 | (code_point >> 12));
        out[1] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
        out[2] = static_cast<char>(0x80 | (code_point & 0x3f));
        return 3;
    }
    out[0] = static_cast<char>(0xf0 | (code_point >> 18));
    out[1] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
    out[2] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
    out[3] = static_cast<char>(0x80 | (code_point & 0x3f));
    return 4;
}

bool is_unicode_whitespace(const u32 code_point) noexcept {
    if (code_point < 0x80) {
        const bool ascii = code_point == ' ' || code_point == '\t' || code_point == '\n' || code_point == '\f' ||
                           code_point == '\r';
        return ascii;
    }
    const bool result = in_ranges(whitespace_ranges, sizeof(whitespace_ranges) / sizeof(whitespace_ranges[0]), code_point);
    return result;
}

bool is_unicode_punctuation(const u32 code_point) noexcept {
    if (code_point < 0x80) {
        const bool ascii = (byte_classes.of[code_point] & byte_class::punctuation) != 0;
        return ascii;
    }
    const bool result =
        in_ranges(punctuation_ranges, sizeof(punctuation_ranges) / sizeof(punctuation_ranges[0]), code_point);
    return result;
}

u32 scan_to_newline(const char* const text, const u32 size, const u32 offset) noexcept {
    u32 cursor = offset;
    // Eight bytes at a time: a word holds LF or CR if xor with either,
    // repeated, has a zero byte.
    while (size - cursor >= 8) {
        u64 word = 0;
        __builtin_memcpy(&word, text + cursor, 8);
        const u64 found = zero_bytes(word ^ (low_bits * 0x0a)) | zero_bytes(word ^ (low_bits * 0x0d));
        if (found != 0) break;
        cursor = cursor + 8;
    }
    while (cursor < size && !is_newline(text[cursor])) cursor = cursor + 1;
    return cursor;
}

} // namespace stxlib::markdown::detail
