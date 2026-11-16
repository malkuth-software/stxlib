// stxlib::glyphs: reading a font's bytes (include/stxlib/font_file.hpp).
// Big-endian numbers out of a range that was checked first: `has` is the
// check, and the reads after it rely on it.

#ifndef STXLIB_ALGORITHMS_GLYPHS_FONT_READER_HPP
#define STXLIB_ALGORITHMS_GLYPHS_FONT_READER_HPP

#include "stxlib/font_file.hpp"

namespace stxlib::glyphs::detail {

// A stretch of the font's bytes.
struct Span final {
    const u8* data{nullptr};
    usize size{0};
};

// Whether `count` bytes at `offset` are inside the span.
[[nodiscard]] constexpr bool has(const Span& span, const usize offset, const usize count) noexcept {
    const bool result = offset <= span.size && count <= span.size - offset;
    return result;
}

// The part of a span from `offset`, `count` bytes long. Requirement:
// has(span, offset, count).
[[nodiscard]] constexpr Span part(const Span& span, const usize offset, const usize count) noexcept {
    const Span result{.data = span.data + offset, .size = count};
    return result;
}

// The whole font, and one of its tables (empty when the table's range is
// not inside the bytes, as for a Font altered by hand).
[[nodiscard]] inline Span span_of(const View<u8> bytes) noexcept {
    const Span result{.data = bytes.data(), .size = bytes.count()};
    return result;
}

[[nodiscard]] inline Span table_of(const Font& font, const FontTable& table) noexcept {
    const Span whole = span_of(font.bytes);
    if (table.length == 0 || !has(whole, table.offset, table.length)) return Span{};
    const Span result = part(whole, table.offset, table.length);
    return result;
}

// Requirement for each: the bytes read are inside the span.
[[nodiscard]] constexpr u32 read_u8(const Span& span, const usize offset) noexcept {
    const u32 result = span.data[offset];
    return result;
}

[[nodiscard]] constexpr u32 read_u16(const Span& span, const usize offset) noexcept {
    const u32 result = (static_cast<u32>(span.data[offset]) << 8) | static_cast<u32>(span.data[offset + 1]);
    return result;
}

[[nodiscard]] constexpr i32 read_i16(const Span& span, const usize offset) noexcept {
    const u32 value = read_u16(span, offset);
    const i32 result = value >= 0x8000U ? static_cast<i32>(value) - 0x10000 : static_cast<i32>(value);
    return result;
}

[[nodiscard]] constexpr i32 read_i8(const Span& span, const usize offset) noexcept {
    const u32 value = span.data[offset];
    const i32 result = value >= 0x80U ? static_cast<i32>(value) - 0x100 : static_cast<i32>(value);
    return result;
}

[[nodiscard]] constexpr u32 read_u24(const Span& span, const usize offset) noexcept {
    const u32 result = (static_cast<u32>(span.data[offset]) << 16) | (static_cast<u32>(span.data[offset + 1]) << 8) |
                       static_cast<u32>(span.data[offset + 2]);
    return result;
}

[[nodiscard]] constexpr u32 read_u32(const Span& span, const usize offset) noexcept {
    const u32 result = (static_cast<u32>(span.data[offset]) << 24) | (static_cast<u32>(span.data[offset + 1]) << 16) |
                       (static_cast<u32>(span.data[offset + 2]) << 8) | static_cast<u32>(span.data[offset + 3]);
    return result;
}

// The same numbers for a place that has not been checked: 0 when the bytes
// are not all inside the span. A table read this way is read as if what
// lies outside it were zeros, which every reader of offsets and counts
// takes as "nothing there".
[[nodiscard]] constexpr u32 peek_u16(const Span& span, const usize offset) noexcept {
    const u32 result = has(span, offset, 2) ? read_u16(span, offset) : 0;
    return result;
}

[[nodiscard]] constexpr i32 peek_i16(const Span& span, const usize offset) noexcept {
    const i32 result = has(span, offset, 2) ? read_i16(span, offset) : 0;
    return result;
}

[[nodiscard]] constexpr u32 peek_u32(const Span& span, const usize offset) noexcept {
    const u32 result = has(span, offset, 4) ? read_u32(span, offset) : 0;
    return result;
}

// No coordinate of an outline the reader gives is farther from the origin
// than this, in font units or, scaled, in 1/64 of a pixel: the one bound
// every outline leaves the reader with (load_glyph, load_scaled_glyph,
// scale_outline). What uses outlines may rely on it.
inline constexpr i64 farthest_point = static_cast<i64>(1) << 30;

// a * b / 65536, rounded to the nearest, halves away from zero: a times a
// 16.16 number.
[[nodiscard]] constexpr i64 multiply_fixed(const i64 a, const i64 b) noexcept {
    const i64 product = a * b;
    const i64 result = (product + 0x8000 + (product < 0 ? -1 : 0)) >> 16;
    return result;
}

[[nodiscard]] OperationStatus refuse_font(ErrorKind kind, Operation operation) noexcept;

// The outlines of the two kinds (font_truetype.cpp, font_cff.cpp).
[[nodiscard]] OperationStatus load_truetype_glyph(const Font& font, u32 glyph, i64 x_scale, i64 y_scale, const FontLimits& limits,
                                         GlyphOutline& outline) noexcept;
[[nodiscard]] OperationStatus open_cff(Font& font) noexcept;
[[nodiscard]] OperationStatus load_cff_glyph(const Font& font, u32 glyph, const FontLimits& limits, GlyphOutline& outline) noexcept;

} // namespace stxlib::glyphs::detail

#endif
