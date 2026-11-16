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
// Bounds. prepare is linear in the characters of the table of scripts
// (about 116,000 look-ups in the character map). A style's measures are
// taken the first time a glyph of it is hinted: a few tens of glyphs
// loaded. hint_glyph is linear in the glyph's points, times the number of
// its edges where points are carried between edges. No recursion.
//
// Failure. invalid_argument: a glyph the font has not, a font other than
// the one prepared, or what load_glyph refuses. limit_exceeded: a
// FontLimits bound. out_of_memory. Nothing throws.

#include "stxlib/font_file.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::glyphs {

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
    Hinter() noexcept = default;
    Hinter(Hinter&&) noexcept = default;
    Hinter& operator=(Hinter&&) noexcept = default;
    Hinter(const Hinter&) = delete;
    Hinter& operator=(const Hinter&) = delete;

    // Gives every glyph of `font` its style. To be done once for a font,
    // before any glyph of it is hinted; doing it again begins afresh.
    [[nodiscard]] Status prepare(const Font& font) noexcept;

    // The outline of `glyph` hinted at `size` (26.6 pixels to the em), in
    // 26.6 pixels, and its advance (26.6, a whole number of pixels).
    // `font` is the font prepared.
    [[nodiscard]] Status hint_glyph(const Font& font, u32 glyph, i64 size, HintMode mode, const FontLimits& limits, GlyphOutline& outline,
                                    i32& advance) noexcept;

    // Frees what was kept.
    void release() noexcept;

private:
    // The style of each glyph, what is done for its accents, and the
    // measures of the styles met so far.
    Vector<u16> styles_{};
    Vector<u16> adjustments_{};
    Vector<StyleMetrics> metrics_{};
};

} // namespace stxlib::glyphs
