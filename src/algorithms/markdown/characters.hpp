#pragma once

// stxlib::markdown: bytes and code points.
//
// One table classifies every byte (ASCII classes; bytes of 128 and above
// have no class). Unicode whitespace and punctuation, which only the
// emphasis rules ask about, are decided from generated ranges
// (unicode_classes.inc, tools/gen_markdown_tables.py).
//
// UTF-8 is decoded leniently and never trusted: an ill-formed sequence
// decodes as one byte with the value not_a_character, which is in no class,
// so it can never be whitespace, punctuation or markup.

#include "stxlib/types.hpp"

namespace stxlib::markdown::detail {

namespace byte_class {
inline constexpr u8 blank = 0x01;       // space, tab
inline constexpr u8 newline = 0x02;     // LF, CR
inline constexpr u8 whitespace = 0x04;  // space, tab, VT, FF (not the line endings)
inline constexpr u8 punctuation = 0x08; // ASCII punctuation: ! to /, : to @, [ to `, { to ~
inline constexpr u8 digit = 0x10;
inline constexpr u8 alpha = 0x20;
inline constexpr u8 hex = 0x40;
inline constexpr u8 control = 0x80; // 0 to 31, and 127
} // namespace byte_class

struct ByteClasses final {
    u8 of[256]{};
};

[[nodiscard]] constexpr ByteClasses make_byte_classes() noexcept {
    ByteClasses table{};
    for (u32 value = 0; value < 256; ++value) {
        u8 classes = 0;
        if (value == ' ' || value == '\t') classes = classes | byte_class::blank;
        if (value == '\n' || value == '\r') classes = classes | byte_class::newline;
        if (value == ' ' || value == '\t' || value == '\v' || value == '\f') classes = classes | byte_class::whitespace;
        if ((value >= 33 && value <= 47) || (value >= 58 && value <= 64) || (value >= 91 && value <= 96) ||
            (value >= 123 && value <= 126)) {
            classes = classes | byte_class::punctuation;
        }
        if (value >= '0' && value <= '9') classes = classes | byte_class::digit | byte_class::hex;
        if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')) classes = classes | byte_class::alpha;
        if ((value >= 'A' && value <= 'F') || (value >= 'a' && value <= 'f')) classes = classes | byte_class::hex;
        if (value <= 31 || value == 127) classes = classes | byte_class::control;
        table.of[value] = classes;
    }
    return table;
}

inline constexpr ByteClasses byte_classes = make_byte_classes();

[[nodiscard]] constexpr bool has_class(const char byte, const u8 classes) noexcept {
    const bool result = (byte_classes.of[static_cast<u8>(byte)] & classes) != 0;
    return result;
}

[[nodiscard]] constexpr bool is_blank(const char byte) noexcept { return has_class(byte, byte_class::blank); }
[[nodiscard]] constexpr bool is_newline(const char byte) noexcept { return has_class(byte, byte_class::newline); }
[[nodiscard]] constexpr bool is_whitespace(const char byte) noexcept { return has_class(byte, byte_class::whitespace); }
[[nodiscard]] constexpr bool is_ascii_punctuation(const char byte) noexcept {
    return has_class(byte, byte_class::punctuation);
}
[[nodiscard]] constexpr bool is_digit(const char byte) noexcept { return has_class(byte, byte_class::digit); }
[[nodiscard]] constexpr bool is_alpha(const char byte) noexcept { return has_class(byte, byte_class::alpha); }
[[nodiscard]] constexpr bool is_hex_digit(const char byte) noexcept { return has_class(byte, byte_class::hex); }
[[nodiscard]] constexpr bool is_alphanumeric(const char byte) noexcept {
    return has_class(byte, byte_class::alpha | byte_class::digit);
}
[[nodiscard]] constexpr bool is_control(const char byte) noexcept { return has_class(byte, byte_class::control); }
[[nodiscard]] constexpr bool is_ascii(const char byte) noexcept { return static_cast<u8>(byte) < 128; }

// ASCII letters only: A to Z become a to z.
[[nodiscard]] constexpr char ascii_lower(const char byte) noexcept {
    const char result = (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte + ('a' - 'A')) : byte;
    return result;
}

// ---- UTF-8 ---------------------------------------------------------------------------

inline constexpr u32 not_a_character = 0xffffffff;

struct Decoded final {
    // The scalar value, or not_a_character for one byte that begins no
    // well-formed sequence.
    u32 code_point{not_a_character};
    // Bytes taken: 1 to 4.
    u32 length{1};
};

// The code point that starts at text[offset]. Requirement: offset < size.
[[nodiscard]] Decoded decode_at(const char* text, u32 size, u32 offset) noexcept;

// The code point that ends just before text[offset] and starts at or after
// `floor`. Requirement: floor < offset <= size.
[[nodiscard]] Decoded decode_before(const char* text, u32 floor, u32 offset) noexcept;

// Appends the UTF-8 form of a scalar value to out[0, 4) and returns its
// length. Requirement: code_point is a scalar value.
[[nodiscard]] u32 encode_utf8(u32 code_point, char* out) noexcept;

// Unicode whitespace as CommonMark defines it: general category Zs, tab,
// line feed, form feed, carriage return.
[[nodiscard]] bool is_unicode_whitespace(u32 code_point) noexcept;

// Unicode punctuation as CommonMark 0.31.2 defines it: general categories
// P and S.
[[nodiscard]] bool is_unicode_punctuation(u32 code_point) noexcept;

// ---- scanning ------------------------------------------------------------------------

// The first offset at or after `offset` whose byte is LF or CR, or `size`.
[[nodiscard]] u32 scan_to_newline(const char* text, u32 size, u32 offset) noexcept;

} // namespace stxlib::markdown::detail
