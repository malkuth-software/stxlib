#pragma once

// stxlib::glyphs: the hinter. STXlib's own auto-hinter (the owner,
// 2026-11-15: stx_gtraster), for text. It moves the points of a glyph's
// outline at a size so that stems and the tops and bottoms of letters fall
// on whole pixels, working from the outline alone: it runs no font
// program.
//
// What it does, in order:
// 1. Each glyph has a style: the script of the characters that map to it
//    (Latin, Greek, Cyrillic, Hebrew, Han and the rest of a fixed table).
// 2. A style has measures, taken once from the font: the widths of the
//    stems of a standard character of the script, and the script's blue
//    zones: heights that many glyphs share (the baseline, the x-height,
//    the height of capitals), each with the little further that round
//    tops and bottoms go.
// 3. At a size, the glyph's outline is searched for segments (runs of
//    points along an axis), segments at one position are gathered into
//    edges, and each edge is given a position: on a blue zone if one
//    catches it, else as one side of a stem beside its other side, else
//    between its neighbours.
// 4. The points of the edges go where their edges went, and every other
//    point is carried between the points that moved.
// 5. An accent is kept clear of its letter: a contour that is the accent
//    of the glyph's character and has come nearer than a pixel to the rest
//    is moved a pixel away, and a tilde is kept tall enough to be a tilde.
//
// HintMode::light moves points along y only: the heights of a line of text
// are made crisp and its widths are left as designed. HintMode::monochrome
// moves them along both axes and makes stems whole pixels wide, for a
// mask of one bit a pixel.
//
// The result for the same font, glyph, size and mode is the same on every
// host: the arithmetic is in integers.
//
// A conversion to house style of Skrifa 0.50.0's auto-hinter, which is
// itself after FreeType's, and for the accents (step 5) of FreeType
// 2.14.2's own (PROVENANCE.md). It is held to FreeType 2.14.2's
// auto-hinter on the fonts GUI.Forms bundles.
//
// Where the glyph comes from. A glyph's outline is read from the font
// once, into a GlyphStore (glyph_store.hpp), and hinted from there at
// every size it is wanted at (hint_stored). The hinter reads the font
// itself only for what it measures a style by, once for a style.
//
// Bounds. prepare is linear in the characters of the table of scripts that
// the font maps (and at most all of them, about 116,000). A style's measures are
// taken the first time a glyph of it is hinted: a few tens of glyphs
// loaded. hint_glyph is linear in the glyph's points, times the number of
// its edges where points are carried between edges. No recursion. The
// hinter's arithmetic is 32-bit, made for coordinates of 2^19 units and
// scales of 8 pixels to the unit: a glyph with a coordinate or advance
// beyond 2^19 units, or any glyph at a greater scale, is given unhinted
// (scaled as Fit::none, its advance to a whole pixel), and such a glyph is
// not measured for a style.
//
// Failure. invalid_argument: a glyph the font has not, a font other than
// the one prepared, or what load_glyph refuses. limit_exceeded: a
// FontLimits bound. out_of_memory. Nothing throws.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_store.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/unique.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::glyphs {

namespace detail {
// The hinter's working memory (src/algorithms/glyphs/hinter.hpp).
struct HintWork;
} // namespace detail

// How the glyph will be drawn.
enum class HintMode : u8 {
    // Grey coverage, hinted along y only.
    light = 0,
    // One bit a pixel: both axes, stems of whole pixels.
    monochrome = 1,
};

// A height many glyphs of a script share, in font units: where flat tops
// (or bottoms) sit, and where round ones go a little beyond.
struct UnscaledBlue final {
    i32 position{0};
    i32 overshoot{0};
    i32 ascender{0};
    i32 descender{0};
    u32 zones{0};
};

struct WidthMetrics final {
    i32 edge_distance_threshold{0};
    i32 standard_width{0};
    bool extra_light{false};
};

struct UnscaledAxisMetrics final {
    u32 dimension{0};
    Vector<i32> widths{};
    WidthMetrics width_metrics{};
    Vector<UnscaledBlue> blues{};
};

// A style's measures in font units: for each axis, standard stem widths
// and blue zones.
struct StyleMetrics final {
    u32 style{0};
    bool digits_same_width{true};
    UnscaledAxisMetrics axes[2]{};
};

class Hinter final {
public:
    Hinter() noexcept;
    Hinter(Hinter&&) noexcept;
    Hinter& operator=(Hinter&&) noexcept;
    Hinter(const Hinter&) = delete;
    Hinter& operator=(const Hinter&) = delete;
    ~Hinter() noexcept;

    // Gives every glyph of `font` its style. To be done once for a font,
    // before any glyph of it is hinted; doing it again begins afresh.
    [[nodiscard]] OperationStatus prepare(const Font& font) noexcept;

    // The outline of `glyph` hinted at `size` (26.6 pixels to the em), in
    // 26.6 pixels, and its advance (26.6, a whole number of pixels).
    // `font` is the font prepared.
    // This reads the glyph from the font each time it is called: it is for
    // a glyph now and then. Text is hinted from a GlyphStore, by
    // hint_stored.
    [[nodiscard]] OperationStatus hint_glyph(const Font& font, u32 glyph, i64 size, HintMode mode,
                                             const FontLimits& limits, GlyphOutline& outline, i32& advance) noexcept;

    // The same for a glyph a GlyphStore holds (glyph_store.hpp): `stored`
    // is glyph `glyph` of `font` as the store gave it. Nothing of the
    // glyph is read from the font, and once the hinter has met the glyph's
    // style and `outline` has room, nothing is allocated.
    [[nodiscard]] OperationStatus hint_stored(const Font& font, u32 glyph, const StoredGlyph& stored, i64 size,
                                              HintMode mode, const FontLimits& limits, GlyphOutline& outline,
                                              i32& advance) noexcept;

    // The bytes held, as allocated now: each glyph's style, the measures
    // of the styles met, and the working memory.
    [[nodiscard]] usize bytes_held() const noexcept;

    // Frees what was kept.
    void release() noexcept;

private:
    // The style of each glyph, what is done for its accents, and the
    // measures of the styles met so far.
    Vector<u16> styles_{};
    Vector<u16> adjustments_{};
    Vector<StyleMetrics> metrics_{};
    // What it works in, kept from glyph to glyph.
    Unique<detail::HintWork> work_{};
};

} // namespace stxlib::glyphs
