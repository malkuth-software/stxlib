// stxlib::glyphs: shaping a run: from characters with a script and a
// direction to glyphs with their places. After harfrust 0.16.0,
// src/shape.rs, src/normalize.rs, src/fallback.rs and
// src/ot/shaper/hebrew.rs (HarfBuzz's hb-ot-shape, -normalize and
// -fallback).
//
// The sequence. Each character's properties are looked up and clusters
// formed; a run set against its script's direction is turned; characters
// are mirrored for right-to-left text; each is given a glyph, composed or
// decomposed as the font allows (a base and marks become one character
// when the font has it, and are taken apart when it has only the parts);
// the plan's substitutions are applied; glyphs get their advances and the
// plan's positions, or, for a font with none, marks are placed by their
// boxes; characters that are not to show become invisible; and a run that
// was turned is turned back.
//
// Which scripts get more than the font's own lookups. Hebrew: the
// presentation forms, when the font cannot place marks. No other: the
// shapers HarfBuzz has for Arabic, the Indic scripts, Khmer, Myanmar,
// Thai, Hangul and the "universal" one are not converted, and text of
// those scripts gets its font's default lookups only.
//
// Not converted: vertical text, Apple's layout tables, tracking.

#ifndef STXLIB_ALGORITHMS_GLYPHS_SHAPE_TEXT_HPP
#define STXLIB_ALGORITHMS_GLYPHS_SHAPE_TEXT_HPP

#include "algorithms/glyphs/shape_apply.hpp"
#include "algorithms/glyphs/shape_buffer.hpp"
#include "algorithms/glyphs/shape_plan.hpp"
#include "stxlib/font_file.hpp"
#include "stxlib/types.hpp"

namespace stxlib::glyphs::detail {

// A glyph's box as fonts give it: where its ink begins, its top, and its
// width and (downward, so negative) height.
struct GlyphBox final {
    i32 x_bearing{0};
    i32 y_bearing{0};
    i32 width{0};
    i32 height{0};
};

// A font at the size text is shaped for.
struct ShapeFont final {
    const Font* font{nullptr};
    ShapeScale scale{};
    // The 16.16 scale advances are taken with; 0: font units.
    i64 advance_scale{0};
    // A glyph's box at that size; false when it has none. Asked only when
    // marks are placed without the font's help.
    using Boxes = bool (*)(void* context, u32 glyph, GlyphBox& box) noexcept;
    Boxes boxes{nullptr};
    void* context{nullptr};
};

// The font at a size in 26.6 pixels, scaled as HarfBuzz scales a FreeType
// font of that size; size 0 is font units.
[[nodiscard]] ShapeFont shape_font_of(const Font& font, i64 size) noexcept;

// A glyph's advance at the font's size.
[[nodiscard]] i32 shape_advance(const ShapeFont& font, u32 glyph) noexcept;

// The run in the buffer, whose direction and script are the plan's, is
// shaped: its items become glyphs, with positions. False when the buffer
// gave up (too long a result, too many operations, no memory): what it
// holds is then not to be used.
[[nodiscard]] bool shape_run(const ShapePlan& plan, const ShapeFont& font, ShapeBuffer& buffer, ApplyWork& work) noexcept;

} // namespace stxlib::glyphs::detail

#endif
