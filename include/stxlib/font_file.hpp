#pragma once

// stxlib::glyphs: a font file, read. STXlib's own reader of TrueType and
// OpenType font files (the owner, 2026-11-15: stx_gtraster), for text. It
// finds a glyph for a character, a glyph's advance, and a glyph's outline
// in the font's own units. It runs no font program: a TrueType glyph's
// instructions are skipped, and a CFF glyph's charstring is a list of
// drawing operators with fixed limits.
//
// What is read: the table directory (and a collection's, for one face of
// it, and its count of faces); head, maxp, hhea, hmtx and OS/2 for the
// face's numbers; name for its names when they are asked for; cmap formats
// 0, 4, 6 and 12 for characters; loca and glyf, composite glyphs included,
// for TrueType outlines; CFF for PostScript outlines.
//
// The numbers are the ones FreeType 2.14.2 gives for the same file, so
// that what is built on them (hinting, layout) can be held to it: the
// face's ascender, descender and height by its rule (OS/2's typographic
// values when the font says to use them, else hhea's, else OS/2's again),
// the character map by its choice (a map of all of Unicode before one of
// its first 65,536 characters, the later table entry before the earlier;
// HarfBuzz's symbol and Mac Roman maps when there is no Unicode one), a
// format 4 map out of order searched in order, an hmtx that runs past the
// file kept to its end, a loca not well formed read as FreeType reads it,
// and an outline's points, contours and curve tags as it has them for a
// glyph loaded without scaling.
//
// A font is untrusted. Every offset and count in it is checked against the
// bytes before it is used; nothing in the file is believed. A font that is
// not well formed where it is read is refused, there or (for one glyph)
// when that glyph is loaded; a table not needed is not looked at.
//
// Bounds. open_font is linear in the table directory and the character
// map's header. glyph_of is logarithmic in the map. load_glyph is linear
// in the glyph's data and bounded by FontLimits: points, contours, the
// components of a composite glyph and how deep they nest, and the steps of
// a CFF charstring. No recursion.
//
// Failure. invalid_argument: the bytes are not a font this reads, a table
// that is needed is missing or cut short, or a glyph's data is not well
// formed. limit_exceeded: a FontLimits bound. out_of_memory: an outline's
// storage. Nothing throws.

#include "stxlib/glyph_mask.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs {

struct FontLimits final {
    // Of one glyph, composite glyphs counted whole.
    u32 points{16384};
    u32 contours{4096};
    u32 components{512};
    u32 component_depth{8};
    // Operators and operands of one CFF glyph, subroutines counted: far
    // more than a glyph of a real font takes (a few thousand), and the
    // most a hostile one can make each reading of a glyph cost (a store
    // reads a glyph once, and remembers a refusal).
    u32 charstring_steps{1U << 17};
};

// Where a table is in the font's bytes; length 0 when the font has none.
struct FontTable final {
    u32 offset{0};
    u32 length{0};
};

enum class OutlineKind : u8 {
    none = 0,
    truetype = 1,
    cff = 2,
};

// The kinds of character map: of Unicode characters; a symbol font's
// (Windows, encoding 0: a character is asked for, and below 256 asked for
// again at 0xF000 more); Mac Roman (asked for ASCII alone).
inline constexpr u32 character_map_unicode = 0;
inline constexpr u32 character_map_symbol = 1;
inline constexpr u32 character_map_mac_roman = 2;

// A font's own data, as open_font found it. It borrows `bytes`: they must
// stay as they are while the Font is used. A Font is plain numbers: one
// altered by hand reads no byte outside `bytes`, but may not find what it
// looks for.
struct Font final {
    View<u8> bytes{};
    FontTable head{};
    FontTable hmtx{};
    FontTable loca{};
    FontTable glyf{};
    FontTable cff{};
    // The chosen character map: its bytes, its format, its kind
    // (character_map_unicode, _symbol or _mac_roman), and, for format 4,
    // whether its segments are out of order (then searched in order).
    FontTable character_map{};
    u32 character_map_format{0};
    u32 character_map_kind{0};
    bool character_map_unsorted{false};
    OutlineKind outlines{OutlineKind::none};
    u32 glyph_count{0};
    u32 units_per_em{0};
    // head's flags; bit 3: sizes are whole pixels.
    u32 head_flags{0};
    i32 ascender{0};
    i32 descender{0};
    i32 height{0};
    // loca's entries are 32 bits wide, not 16.
    bool long_offsets{false};
    // The face is slanted: OS/2 says oblique or italic, or, with no OS/2,
    // head does.
    bool italic{false};
    // Every glyph has one advance (post says so).
    bool fixed_pitch{false};
    // The height of small letters and of capitals in font units, as OS/2
    // (version 2 and later) gives them; 0 when it does not.
    i32 x_height{0};
    i32 cap_height{0};
    // The layout tables: substitutions (GSUB), positions (GPOS) and glyph
    // definitions (GDEF). has_feature reads the first; the shaper all three.
    FontTable substitutions{};
    FontTable positions{};
    FontTable definitions{};
    // The character map's list of variation sequences (a base character
    // and a variation selector that together name a glyph), and the old
    // kerning table; the shaper reads both. Empty when the font has none.
    FontTable variation_map{};
    FontTable kerning{};
    u32 advance_count{0};
    // CFF: the charstrings, the global subroutines, and where each glyph's
    // font dictionary is found (src/algorithms/glyphs/font_cff.cpp).
    FontTable charstrings{};
    FontTable global_subroutines{};
    FontTable local_subroutines{};
    FontTable font_dictionaries{};
    FontTable dictionary_select{};
    // The name table (font_name, family_name and style_name read it);
    // empty when the font has none or its header and records are not
    // inside it.
    FontTable names{};
    // hhea's own numbers, as the table has them (ascender and descender
    // above are the face's by FreeType's rule).
    i32 hhea_ascender{0};
    i32 hhea_descender{0};
    i32 hhea_line_gap{0};
    // OS/2, as FreeType 2.14.2 reads it: os2_version is 0xFFFF, and the
    // rest 0, when the font has no OS/2 or one too short for its version
    // (78 bytes for version 0, 86 for 1, 96 for 2 to 4, 100 for 5 and
    // later). selection is fsSelection: bit 0 italic, 5 bold, 6 regular,
    // 7 use the typographic metrics, 8 WWS, 9 oblique.
    u32 os2_version{0xFFFF};
    u32 weight_class{0};
    u32 width_class{0};
    u32 selection{0};
    i32 typo_ascender{0};
    i32 typo_descender{0};
    i32 typo_line_gap{0};
    u32 win_ascent{0};
    u32 win_descent{0};
};

// Name identifiers of the name table.
inline constexpr u32 name_family = 1;
inline constexpr u32 name_style = 2;
inline constexpr u32 name_full = 4;
inline constexpr u32 name_postscript = 6;
inline constexpr u32 name_typographic_family = 16;
inline constexpr u32 name_typographic_style = 17;
inline constexpr u32 name_wws_family = 21;
inline constexpr u32 name_wws_style = 22;

// How a point of a GlyphOutline is used (FreeType's tag values).
enum class PointTag : u8 {
    // A control point of a quadratic curve.
    quadratic = 0,
    // A point the outline passes through.
    on = 1,
    // A control point of a cubic curve.
    cubic = 2,
};

// A glyph's outline in the form the font has it: points in contour order,
// each tagged, and the number of the last point of each contour. The
// storage is kept from glyph to glyph.
struct GlyphOutline final {
    Vector<Point> points{};
    Vector<PointTag> tags{};
    Vector<u32> contour_ends{};
};

// Reads face `face` of the font in `bytes` (0 for a font that is not a
// collection).
[[nodiscard]] OperationStatus open_font(View<u8> bytes, u32 face, Font& font) noexcept;

// How many faces the bytes hold: a collection's count (TTC or OTC), 1 for
// one font, 0 for bytes that are neither (a collection of no face, or one
// whose list of faces is not inside the bytes, is neither). Only the
// header is read: a face counted may still be refused by open_font.
[[nodiscard]] u32 face_count(View<u8> bytes) noexcept;

// A name of the font, as UTF-8 in `text` (emptied first; empty when the
// font has no such name). The record is chosen as FreeType 2.14.2 chooses
// it: Windows' (encodings 0, 1 and 10, read as UTF-16) in English (US or
// any other English) or, when there is none in English, the last Windows
// one, unless there is a Macintosh one and the Windows one is not in
// English; then Macintosh's in English, else in Roman; else the Unicode
// platform's (UTF-16). Records whose text is not inside the table, or is
// empty, are passed over. Unlike FreeType, the text is not cut to ASCII:
// UTF-16 is decoded (a lone surrogate is U+FFFD) and Macintosh text is
// read as Mac Roman. It ends at a U+0000, as FreeType's does. Linear in
// the table's records; the text is at most 3 bytes for each byte of the
// record's string. out_of_memory is the only failure.
[[nodiscard]] OperationStatus font_name(const Font& font, u32 name_id, Vector<char>& text) noexcept;

// The family and style names, by FreeType 2.14.2's rule (its
// family_name and style_name): when OS/2's fsSelection has bit 8, the
// typographic name (16, 17) and then the plain one (1, 2); otherwise the
// WWS name (21, 22), then the typographic one, then the plain one.
[[nodiscard]] OperationStatus family_name(const Font& font, Vector<char>& text) noexcept;
[[nodiscard]] OperationStatus style_name(const Font& font, Vector<char>& text) noexcept;

// The glyph for a character, or 0 when the font has none.
[[nodiscard]] u32 glyph_of(const Font& font, u32 character) noexcept;

// A stretch of characters in a row that the character map has an entry
// for: `first` to `last`, and the entry's own number in the map.
struct MappedStretch final {
    u32 first{0};
    u32 last{0};
    u32 place{0};
};

// The stretch that holds `from`, or the first one after it (then
// stretch.first is past `from`); false when the map has none, or is not of
// Unicode characters. One search of the map (by halving: a format 4 map
// out of order may have stretches passed over). For going through what a font maps without asking for every
// character there is.
[[nodiscard]] bool next_stretch(const Font& font, u32 from, MappedStretch& stretch) noexcept;

// The glyph of a character of a stretch next_stretch gave for this font
// (glyph_of's answer, without the search), or 0.
[[nodiscard]] u32 glyph_in_stretch(const Font& font, const MappedStretch& stretch, u32 character) noexcept;

// Whether the font's table of substitutions lists the feature `tag` (four
// letters as one number, the first highest: 'smcp' is 0x736D6370).
[[nodiscard]] bool has_feature(const Font& font, u32 tag) noexcept;

// The glyph of a character followed by a variation selector: 0 when the
// font lists no such sequence; `use_default` set when the font says the
// sequence is the character's own glyph (glyph_of gives it).
[[nodiscard]] u32 variation_glyph(const Font& font, u32 character, u32 selector, bool& use_default) noexcept;

// A glyph's advance in font units, or 0 for a glyph the font has not.
[[nodiscard]] i32 advance_of(const Font& font, u32 glyph) noexcept;

// Whether this build checks, at run time, that what stxlib::glyphs holds
// agrees with itself at the end of every call (a checked test build:
// src/algorithms/glyphs/glyph_checks.hpp).
[[nodiscard]] bool glyph_checks_enabled() noexcept;

// A glyph's outline in font units. An empty glyph (a space) succeeds with
// no points.
[[nodiscard]] OperationStatus load_glyph(const Font& font, u32 glyph, const FontLimits& limits,
                                         GlyphOutline& outline) noexcept;

// A glyph's outline in 26.6 pixels, without hinting, at 16.16 scales
// (scale_of). For a TrueType glyph put together from others this is not
// load_glyph's outline scaled: each part's points and each part's offset
// are scaled and rounded on their own, as TrueType has it, and a point can
// fall 1/64 of a pixel from where scaling the whole would put it.
[[nodiscard]] OperationStatus load_scaled_glyph(const Font& font, u32 glyph, i64 x_scale, i64 y_scale,
                                                const FontLimits& limits, GlyphOutline& outline) noexcept;

// The scale that takes font units to 26.6 pixels at a size given in 26.6
// pixels to the em, as 16.16: size * 65536 / units per em, rounded.
[[nodiscard]] i64 scale_of(const Font& font, i64 size) noexcept;

// Every point times a 16.16 scale, rounded to the nearest unit, halves away
// from zero: font units to 26.6 pixels. invalid_argument, the outline as it
// was, for a scale not above 0 or above 2^32, or a point that would come
// farther than 2^30 from the origin either way (the bound every outline the
// reader gives keeps, in font units and scaled).
[[nodiscard]] OperationStatus scale_outline(GlyphOutline& outline, i64 x_scale, i64 y_scale) noexcept;

// The outline as segments, for measure and render (glyph_mask.hpp). Two
// quadratic control points in a row have a point of the curve between
// them, their mean with each coordinate rounded down; a contour that
// begins on a control point begins at its last point if that is on the
// curve, else between the two.
[[nodiscard]] OperationStatus to_segments(const GlyphOutline& outline, Vector<Segment>& segments,
                                          Vector<Point>& points) noexcept;

} // namespace stxlib::glyphs
