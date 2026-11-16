// stxlib::glyphs: what the shaper needs to know of a character. After
// harfrust 0.16.0, src/unicode/mod.rs (itself after HarfBuzz's hb-ucd): the
// general category, the script, the combining class and the order marks are
// put in, the mirror image, what a character is made of and what two make,
// which characters are not to show, and which way a script is written.
//
// The values are harfrust's, which are the Unicode Character Database's
// (version 18.0 in harfrust 0.16.0), written in this library's own tables by
// tools/gen_shape_unicode.py (shape_unicode.inc). Every function here is a
// fixed number of table reads or a search by halving of a list in order;
// none allocates, and any 32-bit number may be asked about.
//
// Not converted: the vertical forms of punctuation (text is set
// horizontally).

#ifndef STXLIB_ALGORITHMS_GLYPHS_SHAPE_UNICODE_HPP
#define STXLIB_ALGORITHMS_GLYPHS_SHAPE_UNICODE_HPP

#include "algorithms/glyphs/shape_buffer.hpp"
#include "stxlib/types.hpp"

namespace stxlib::glyphs::detail {

// The general categories, numbered as harfrust and HarfBuzz number them.
inline constexpr u32 category_control = 0;
inline constexpr u32 category_format = 1;
inline constexpr u32 category_unassigned = 2;
inline constexpr u32 category_private_use = 3;
inline constexpr u32 category_surrogate = 4;
inline constexpr u32 category_lowercase_letter = 5;
inline constexpr u32 category_modifier_letter = 6;
inline constexpr u32 category_other_letter = 7;
inline constexpr u32 category_titlecase_letter = 8;
inline constexpr u32 category_uppercase_letter = 9;
inline constexpr u32 category_spacing_mark = 10;
inline constexpr u32 category_enclosing_mark = 11;
inline constexpr u32 category_non_spacing_mark = 12;
inline constexpr u32 category_decimal_number = 13;
inline constexpr u32 category_letter_number = 14;
inline constexpr u32 category_other_number = 15;
inline constexpr u32 category_connector_punctuation = 16;
inline constexpr u32 category_dash_punctuation = 17;
inline constexpr u32 category_close_punctuation = 18;
inline constexpr u32 category_final_punctuation = 19;
inline constexpr u32 category_initial_punctuation = 20;
inline constexpr u32 category_other_punctuation = 21;
inline constexpr u32 category_open_punctuation = 22;
inline constexpr u32 category_currency_symbol = 23;
inline constexpr u32 category_modifier_symbol = 24;
inline constexpr u32 category_math_symbol = 25;
inline constexpr u32 category_other_symbol = 26;
inline constexpr u32 category_line_separator = 27;
inline constexpr u32 category_paragraph_separator = 28;
inline constexpr u32 category_space_separator = 29;

[[nodiscard]] constexpr bool category_is_mark(const u32 category) noexcept {
    const bool result = category >= category_spacing_mark && category <= category_non_spacing_mark;
    return result;
}

[[nodiscard]] constexpr bool category_is_letter(const u32 category) noexcept {
    const bool result = category >= category_lowercase_letter && category <= category_uppercase_letter;
    return result;
}

// A script is its ISO 15924 tag, four letters as one number. These three
// are no script of their own: shared by scripts, taken from the character
// before, and not known.
inline constexpr u32 script_common = 0x5A797979;
inline constexpr u32 script_inherited = 0x5A696E68;
inline constexpr u32 script_unknown = 0x5A7A7A7A;

// How wide a space is when the font has no glyph for it (harfrust's
// space_fallback): a part of an em, or one of the named widths.
inline constexpr u32 space_none = 0;
inline constexpr u32 space_em = 1;
inline constexpr u32 space_em_2 = 2;
inline constexpr u32 space_em_3 = 3;
inline constexpr u32 space_em_4 = 4;
inline constexpr u32 space_em_5 = 5;
inline constexpr u32 space_em_6 = 6;
inline constexpr u32 space_em_16 = 16;
inline constexpr u32 space_4_em_18 = 17;
inline constexpr u32 space_plain = 18;
inline constexpr u32 space_figure = 19;
inline constexpr u32 space_punctuation = 20;
inline constexpr u32 space_narrow = 21;

// Of a character; a number above 0x10FFFF is unassigned, of no known
// script, class 0.
[[nodiscard]] u32 general_category(u32 character) noexcept;
[[nodiscard]] u32 script_of(u32 character) noexcept;
[[nodiscard]] u32 combining_class(u32 character) noexcept;
// The class marks are put in order by: the combining class, with the
// changes harfrust makes so that marks fonts expect in another order come
// in that order.
[[nodiscard]] u32 modified_combining_class(u32 character) noexcept;
[[nodiscard]] bool is_extended_pictographic(u32 character) noexcept;
// The character's mirror image, 0 when it has none.
[[nodiscard]] u32 mirror_of(u32 character) noexcept;

// One step of canonical decomposition: the one or two characters this one
// is made of (`second` 0: one). False: it is made of no others.
[[nodiscard]] bool decompose(u32 character, u32& first, u32& second) noexcept;
// The character two characters make, when they make one.
[[nodiscard]] bool compose(u32 first, u32 second, u32& made) noexcept;

// Not to show when the font has no glyph for it (Default_Ignorable, less
// the Hangul fillers and the shorthand format characters, which fonts
// draw).
[[nodiscard]] bool is_default_ignorable(u32 character) noexcept;
[[nodiscard]] bool is_variation_selector(u32 character) noexcept;
[[nodiscard]] u32 space_fallback(u32 character) noexcept;

// Which way a script is written: left_to_right, right_to_left, or unset
// for a script written either way.
[[nodiscard]] ShapeDirection way_of_script(u32 script) noexcept;

// An item's unicode_props from its character: the general category, and
// for what is not to show, for joiners and for marks, their bits; what the
// run has of them is noted in `scratch`.
void init_unicode_props(ShapeGlyph& item, u32& scratch) noexcept;

// The buffer's script, if unset, becomes that of its first character that
// has one of its own; its direction, if unset, that script's, or left to
// right.
void guess_segment_properties(ShapeBuffer& buffer) noexcept;

// The sum the tables were written with (the test computes it again from
// what these functions give).
[[nodiscard]] u64 unicode_table_sum() noexcept;

} // namespace stxlib::glyphs::detail

#endif
