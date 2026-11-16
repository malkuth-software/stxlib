#pragma once

// stxlib::glyphs: a font file, read. STXlib's own reader of TrueType and
// OpenType font files (the owner, 2026-11-15: stx_gtraster), for text. It
// finds a glyph for a character, a glyph's advance, and a glyph's outline
// in the font's own units. It runs no font program: a TrueType glyph's
// instructions are skipped, and a CFF glyph's charstring is a list of
// drawing operators with fixed limits.
//
// What is read: the table directory (and a collection's, for one face of
// it); head, maxp, hhea, hmtx and OS/2 for the face's numbers; cmap formats
// 0, 4, 6 and 12 for characters; loca and glyf, composite glyphs included,
// for TrueType outlines; CFF for PostScript outlines.
//
// The numbers are the ones FreeType 2.14.2 gives for the same file, so
// that what is built on them (hinting, layout) can be held to it: the
// face's ascender, descender and height by its rule (OS/2's typographic
// values when the font says to use them, else hhea's, else OS/2's again),
// the character map by its choice (a map of all of Unicode before one of
// its first 65,536 characters, the later table entry before the earlier),
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
    // Operators and operands of one CFF glyph, subroutines counted.
    u32 charstring_steps{1U << 20};
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
    // The chosen character map: its bytes, and its format.
    FontTable character_map{};
    u32 character_map_format{0};
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
    u32 advance_count{0};
    // CFF: the charstrings, the global subroutines, and where each glyph's
    // font dictionary is found (src/algorithms/glyphs/font_cff.cpp).
    FontTable charstrings{};
    FontTable global_subroutines{};
    FontTable local_subroutines{};
    FontTable font_dictionaries{};
    FontTable dictionary_select{};
};

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
[[nodiscard]] Status open_font(View<u8> bytes, u32 face, Font& font) noexcept;

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
// stretch.first is past `from`); false when the map has none. One search
// of the map. For going through what a font maps without asking for every
// character there is.
[[nodiscard]] bool next_stretch(const Font& font, u32 from, MappedStretch& stretch) noexcept;

// The glyph of a character of a stretch next_stretch gave for this font
// (glyph_of's answer, without the search), or 0.
[[nodiscard]] u32 glyph_in_stretch(const Font& font, const MappedStretch& stretch, u32 character) noexcept;

// Whether the font's table of substitutions lists the feature `tag` (four
// letters as one number, the first highest: 'smcp' is 0x736D6370).
[[nodiscard]] bool has_feature(const Font& font, u32 tag) noexcept;

// A glyph's advance in font units, or 0 for a glyph the font has not.
[[nodiscard]] i32 advance_of(const Font& font, u32 glyph) noexcept;

// Whether this build checks, at run time, that what stxlib::glyphs holds
// agrees with itself at the end of every call (a checked test build:
// src/algorithms/glyphs/glyph_checks.hpp).
[[nodiscard]] bool glyph_checks_enabled() noexcept;

// A glyph's outline in font units. An empty glyph (a space) succeeds with
// no points.
[[nodiscard]] Status load_glyph(const Font& font, u32 glyph, const FontLimits& limits, GlyphOutline& outline) noexcept;

// A glyph's outline in 26.6 pixels, without hinting, at 16.16 scales
// (scale_of). For a TrueType glyph put together from others this is not
// load_glyph's outline scaled: each part's points and each part's offset
// are scaled and rounded on their own, as TrueType has it, and a point can
// fall 1/64 of a pixel from where scaling the whole would put it.
[[nodiscard]] Status load_scaled_glyph(const Font& font, u32 glyph, i64 x_scale, i64 y_scale, const FontLimits& limits,
                                       GlyphOutline& outline) noexcept;

// The scale that takes font units to 26.6 pixels at a size given in 26.6
// pixels to the em, as 16.16: size * 65536 / units per em, rounded.
[[nodiscard]] i64 scale_of(const Font& font, i64 size) noexcept;

// Every point times a 16.16 scale, rounded to the nearest unit, halves away
// from zero: font units to 26.6 pixels.
void scale_outline(GlyphOutline& outline, i64 x_scale, i64 y_scale) noexcept;

// The outline as segments, for measure and render (glyph_mask.hpp). Two
// quadratic control points in a row have a point of the curve between
// them, their mean with each coordinate rounded down; a contour that
// begins on a control point begins at its last point if that is on the
// curve, else between the two.
[[nodiscard]] Status to_segments(const GlyphOutline& outline, Vector<Segment>& segments, Vector<Point>& points) noexcept;

} // namespace stxlib::glyphs
