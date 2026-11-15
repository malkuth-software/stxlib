#pragma once

// ASCII character classes and case mapping, locale-free (roadmap M2, R2;
// audit B: the C <ctype.h> functions follow the C locale and are undefined
// for a negative char other than EOF).
//
// Each class is the C library's in the "C" locale (ISO C 7.4.1, POSIX
// "POSIX locale" LC_CTYPE), restricted to bytes 0x00..0x7F: a byte at or
// above 0x80 is in no class and is never changed by case mapping. So the
// functions take any char or byte, signed or not, and UTF-8 text passed
// through them byte by byte stays well-formed (a byte of a multi-byte
// sequence is never touched). tests/ascii_search_tests.cpp checks all 256
// byte values: 0x00..0x7F against the host C library in the "C" locale,
// 0x80..0xFF against the rule above.
//
// Header-only and constexpr: these are table-free comparisons.

#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

[[nodiscard]] constexpr u8 ascii_byte(const char character) noexcept { return static_cast<u8>(character); }

[[nodiscard]] constexpr bool is_ascii(const char character) noexcept { return ascii_byte(character) <= 0x7f; }
[[nodiscard]] constexpr bool is_ascii_upper(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte >= 'A' && byte <= 'Z';
    return result;
}
[[nodiscard]] constexpr bool is_ascii_lower(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte >= 'a' && byte <= 'z';
    return result;
}
[[nodiscard]] constexpr bool is_ascii_alpha(const char character) noexcept {
    const bool result = is_ascii_upper(character) || is_ascii_lower(character);
    return result;
}
[[nodiscard]] constexpr bool is_ascii_digit(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte >= '0' && byte <= '9';
    return result;
}
[[nodiscard]] constexpr bool is_ascii_alnum(const char character) noexcept {
    const bool result = is_ascii_alpha(character) || is_ascii_digit(character);
    return result;
}
[[nodiscard]] constexpr bool is_ascii_hex_digit(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool letter = (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
    const bool result = is_ascii_digit(character) || letter;
    return result;
}
// Space, tab, line feed, vertical tab, form feed, carriage return (isspace).
[[nodiscard]] constexpr bool is_ascii_space(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte == ' ' || (byte >= 0x09 && byte <= 0x0d);
    return result;
}
// Space and tab (isblank).
[[nodiscard]] constexpr bool is_ascii_blank(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte == ' ' || byte == 0x09;
    return result;
}
// 0x00..0x1F and 0x7F (iscntrl).
[[nodiscard]] constexpr bool is_ascii_control(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte <= 0x1f || byte == 0x7f;
    return result;
}
// 0x20..0x7E (isprint).
[[nodiscard]] constexpr bool is_ascii_printable(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte >= 0x20 && byte <= 0x7e;
    return result;
}
// 0x21..0x7E (isgraph).
[[nodiscard]] constexpr bool is_ascii_graphic(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    const bool result = byte >= 0x21 && byte <= 0x7e;
    return result;
}
// Graphic and not alphanumeric (ispunct).
[[nodiscard]] constexpr bool is_ascii_punctuation(const char character) noexcept {
    const bool result = is_ascii_graphic(character) && !is_ascii_alnum(character);
    return result;
}

// 'A'..'Z' to 'a'..'z'; every other byte unchanged.
[[nodiscard]] constexpr char ascii_lower(const char character) noexcept {
    if (!is_ascii_upper(character)) return character;
    const char result = static_cast<char>(ascii_byte(character) + 0x20);
    return result;
}
// 'a'..'z' to 'A'..'Z'; every other byte unchanged.
[[nodiscard]] constexpr char ascii_upper(const char character) noexcept {
    if (!is_ascii_lower(character)) return character;
    const char result = static_cast<char>(ascii_byte(character) - 0x20);
    return result;
}

// The value of a digit in bases up to 36 ('0'..'9' then 'a'..'z' or
// 'A'..'Z' for 10..35), or -1 for any other byte.
[[nodiscard]] constexpr i32 ascii_digit_value(const char character) noexcept {
    const u8 byte = ascii_byte(character);
    if (byte >= '0' && byte <= '9') return static_cast<i32>(byte - '0');
    if (byte >= 'a' && byte <= 'z') return static_cast<i32>(byte - 'a') + 10;
    if (byte >= 'A' && byte <= 'Z') return static_cast<i32>(byte - 'A') + 10;
    return -1;
}

// Equality of two texts with ASCII letters compared without case; every
// other byte compared exactly (no Unicode case folding: that is C7).
[[nodiscard]] constexpr bool equals_ignoring_ascii_case(const TextView left, const TextView right) noexcept {
    if (left.count() != right.count()) return false;
    const char* const a = left.data();
    const char* const b = right.data();
    for (usize index = 0; index < left.count(); ++index) {
        if (ascii_lower(a[index]) != ascii_lower(b[index])) return false;
    }
    return true;
}

// Byte order of the two texts after ASCII lower-casing: negative, zero or
// positive, as compare_text (text.hpp) orders the unfolded texts.
[[nodiscard]] constexpr i32 compare_ignoring_ascii_case(const TextView left, const TextView right) noexcept {
    const usize shorter = left.count() < right.count() ? left.count() : right.count();
    const char* const a = left.data();
    const char* const b = right.data();
    for (usize index = 0; index < shorter; ++index) {
        const u8 x = ascii_byte(ascii_lower(a[index]));
        const u8 y = ascii_byte(ascii_lower(b[index]));
        if (x < y) return -1;
        if (x > y) return 1;
    }
    if (left.count() < right.count()) return -1;
    if (left.count() > right.count()) return 1;
    return 0;
}

} // namespace stxlib
