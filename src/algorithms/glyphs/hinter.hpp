// stxlib::glyphs: the auto-hinter's own data (include/stxlib/glyph_hinter.hpp
// states what it does). After Skrifa 0.50.0, outline/autohint: a glyph's
// outline as the hinter looks at it, in font units and then scaled, with
// what it finds out about every point.

#ifndef STXLIB_ALGORITHMS_GLYPHS_HINTER_HPP
#define STXLIB_ALGORITHMS_GLYPHS_HINTER_HPP

#include "algorithms/glyphs/font_reader.hpp"
#include "stxlib/font_file.hpp"
#include "stxlib/glyph_hinter.hpp"

namespace stxlib::glyphs::detail {

// Which way an outline runs between two points, when it runs nearly along
// an axis. Opposites add to nothing; the two of an axis have one size.
enum class Direction : i8 {
    none = 4,
    right = 1,
    left = -1,
    up = 2,
    down = -2,
};

// The direction of a step: along an axis when the step's long arm is more
// than fourteen times its short one.
[[nodiscard]] constexpr Direction direction_of(const i32 dx, const i32 dy) noexcept {
    Direction direction = Direction::none;
    i64 long_arm = 0;
    i64 short_arm = 0;
    if (dy >= dx) {
        if (dy >= -dx) {
            direction = Direction::up;
            long_arm = dy;
            short_arm = dx;
        } else {
            direction = Direction::left;
            long_arm = -static_cast<i64>(dx);
            short_arm = dy;
        }
    } else if (dy >= -dx) {
        direction = Direction::right;
        long_arm = dx;
        short_arm = dy;
    } else {
        direction = Direction::down;
        long_arm = -static_cast<i64>(dy);
        short_arm = dx;
    }
    const i64 short_size = short_arm < 0 ? -short_arm : short_arm;
    const Direction result = long_arm <= 14 * short_size ? Direction::none : direction;
    return result;
}

[[nodiscard]] constexpr bool opposite(const Direction a, const Direction b) noexcept {
    const bool result = static_cast<i32>(a) + static_cast<i32>(b) == 0;
    return result;
}

[[nodiscard]] constexpr bool same_axis(const Direction a, const Direction b) noexcept {
    const i32 first = static_cast<i32>(a) < 0 ? -static_cast<i32>(a) : static_cast<i32>(a);
    const i32 second = static_cast<i32>(b) < 0 ? -static_cast<i32>(b) : static_cast<i32>(b);
    const bool result = first == second;
    return result;
}

// What is known of a point, as bits.
inline constexpr u32 mark_touched_x = 0x08;
inline constexpr u32 mark_touched_y = 0x10;
// The point need not be looked at when edges are found: it is carried
// between its neighbours.
inline constexpr u32 mark_weak = 0x20;
// The point is very near the next one.
inline constexpr u32 mark_near = 0x40;
// The point is left out when segments are found (a tilde that was made
// taller, hinter_accents.cpp).
inline constexpr u32 mark_ignore = 0x80;

struct HintPoint final {
    PointTag tag{PointTag::on};
    u32 marks{0};
    // In font units.
    i32 fx{0};
    i32 fy{0};
    // Scaled, before and after hinting (26.6).
    i32 ox{0};
    i32 oy{0};
    i32 x{0};
    i32 y{0};
    Direction in_direction{Direction::none};
    Direction out_direction{Direction::none};
    // Working numbers: the next and the previous point that count.
    i32 u{0};
    i32 v{0};
    u32 next{0};
    u32 previous{0};
};

struct HintContour final {
    u32 first{0};
    u32 last{0};
};

[[nodiscard]] constexpr u32 next_in(const HintContour& contour, const u32 index) noexcept {
    const u32 result = index >= contour.last ? contour.first : index + 1;
    return result;
}

[[nodiscard]] constexpr u32 previous_in(const HintContour& contour, const u32 index) noexcept {
    const u32 result = index <= contour.first ? contour.last : index - 1;
    return result;
}

// Which way the outline's contours wind, taken together.
enum class Winding : u8 {
    none = 0,
    clockwise = 1,
    counter_clockwise = 2,
};

struct HintOutline final {
    Vector<HintPoint> points{};
    Vector<HintContour> contours{};
    i32 units_per_em{0};
    i32 advance{0};
    Winding winding{Winding::none};
};

// ---- segments and edges ----------------------------------------------------------------------

// What is known of a segment or an edge, as bits.
inline constexpr u32 topo_round = 1;
inline constexpr u32 topo_serif = 2;
inline constexpr u32 topo_done = 4;
inline constexpr u32 topo_neutral = 8;
// An edge that no blue zone may catch (an accent's, hinter_accents.cpp).
inline constexpr u32 topo_no_blue = 16;

inline constexpr u32 no_index = 0xFFFFFFFFU;
inline constexpr i32 highest_score = 32000;
inline constexpr i32 lowest_score = -32000;

// A run of points along one direction of an axis: a stem's side, or part
// of one.
struct HintSegment final {
    u32 flags{0};
    Direction direction{Direction::none};
    // Where it is across the axis (font units), and half its spread.
    i32 position{0};
    i32 delta{0};
    // Its reach along the axis.
    i32 least{0};
    i32 greatest{0};
    i32 height{0};
    i32 score{0};
    i32 length{0};
    // The segment across the stem from it, or the one it is a serif of.
    u32 link{no_index};
    u32 serif{no_index};
    // Its first and last points.
    u32 first{0};
    u32 last{0};
    // Its edge, and the next segment of that edge (a ring).
    u32 edge{no_index};
    u32 edge_next{no_index};
};

// A width as it is at a size: scaled, and fitted to the pixel grid.
struct ScaledWidth final {
    i32 scaled{0};
    i32 fitted{0};
};

// Segments at one position: a stem's side as a whole.
struct HintEdge final {
    // In font units; scaled; and where hinting puts it (26.6).
    i32 font_position{0};
    i32 original{0};
    i32 position{0};
    u32 flags{0};
    Direction direction{Direction::none};
    // The blue zone edge it is caught by, if any.
    bool has_blue{false};
    ScaledWidth blue{};
    u32 link{no_index};
    u32 serif{no_index};
    i32 scale{0};
    // Its first and last segments.
    u32 first{0};
    u32 last{0};
};

// One of the two axes: 0 for positions measured along x (upright stems),
// 1 for positions measured along y.
inline constexpr u32 axis_horizontal = 0;
inline constexpr u32 axis_vertical = 1;

struct HintAxis final {
    u32 dimension{axis_horizontal};
    Direction major_direction{Direction::none};
    Vector<HintSegment> segments{};
    Vector<HintEdge> edges{};
};

// value * units_per_em / 2048: a measure given for an em of 2048.
[[nodiscard]] constexpr i32 for_em(const i32 units_per_em, const i32 value) noexcept {
    const i32 result = static_cast<i32>(static_cast<i64>(value) * units_per_em / 2048);
    return result;
}

// ---- measures of a style, in font units and at a size ----------------------------------------

// a * 65536 / b, rounded to the nearest, halves away from zero; the
// greatest number when b is 0.
[[nodiscard]] constexpr i32 divide_fixed(const i64 a, const i64 b) noexcept {
    const i64 a_size = a < 0 ? -a : a;
    const i64 b_size = b < 0 ? -b : b;
    const i64 quotient = b_size > 0 ? (a_size * 65536 + b_size / 2) / b_size : 0x7FFFFFFF;
    const i64 result = (a < 0) != (b < 0) ? -quotient : quotient;
    return static_cast<i32>(result);
}

[[nodiscard]] constexpr i32 times_fixed(const i64 a, const i64 b) noexcept {
    const i32 result = static_cast<i32>(multiply_fixed(a, b));
    return result;
}

// To the nearest whole pixel, and down to one (26.6).
[[nodiscard]] constexpr i32 pixel_round(const i32 value) noexcept {
    const i32 result = static_cast<i32>((static_cast<i64>(value) + 32) & ~static_cast<i64>(63));
    return result;
}

[[nodiscard]] constexpr i32 pixel_floor(const i32 value) noexcept {
    const i32 result = static_cast<i32>(static_cast<i64>(value) & ~static_cast<i64>(63));
    return result;
}

// What a blue zone is, as bits.
inline constexpr u32 blue_top = 1U << 1;
inline constexpr u32 blue_sub_top = 1U << 2;
inline constexpr u32 blue_neutral = 1U << 3;
inline constexpr u32 blue_adjustment = 1U << 4;
inline constexpr u32 blue_x_height = 1U << 5;
inline constexpr u32 blue_long = 1U << 6;
// The bottom of capitals and of small letters (hinter_accents.cpp).
inline constexpr u32 blue_capital_bottom = 1U << 7;
inline constexpr u32 blue_small_bottom = 1U << 8;
inline constexpr u32 blue_horizontal = 1U << 2;
inline constexpr u32 blue_right = blue_top;

struct ScaledBlue final {
    ScaledWidth position{};
    ScaledWidth overshoot{};
    u32 zones{0};
    bool active{false};
};

struct ScaledAxisMetrics final {
    u32 dimension{axis_horizontal};
    i32 scale{0};
    i32 delta{0};
    Vector<ScaledWidth> widths{};
    WidthMetrics width_metrics{};
    Vector<ScaledBlue> blues{};
};

// What a size and a rendering mode ask of the hinter, as bits.
inline constexpr u32 scale_horizontal_snap = 1U << 0;
inline constexpr u32 scale_vertical_snap = 1U << 1;
inline constexpr u32 scale_stem_adjust = 1U << 2;
inline constexpr u32 scale_mono = 1U << 3;
inline constexpr u32 scale_no_horizontal = 1U << 4;
inline constexpr u32 scale_no_vertical = 1U << 5;
inline constexpr u32 scale_no_advance = 1U << 6;

struct HintScale final {
    u32 flags{0};
    // 16.16, and what is added after scaling (26.6).
    i32 x_scale{0};
    i32 y_scale{0};
    i32 x_delta{0};
    i32 y_delta{0};
    i32 units_per_em{0};
    // Whole pixels to the em.
    i32 pixels{0};
};

// ---- scripts and styles ------------------------------------------------------------------------

// The table of scripts (hinter_tables.inc): for each, the characters whose
// stems give its standard widths and the characters whose tops and bottoms
// give its blue zones; the styles; and the ranges of characters of each.
struct TableBlue final {
    u32 first{0};
    u32 count{0};
    u32 zones{0};
};

inline constexpr u32 group_default = 0;
inline constexpr u32 group_cjk = 1;
inline constexpr u32 group_indic = 2;

struct TableScript final {
    u32 group{group_default};
    bool top_to_bottom{false};
    u32 standard_first{0};
    u32 standard_count{0};
    u32 blue_first{0};
    u32 blue_count{0};
};

struct TableStyle final {
    u32 script{0};
    bool feature{false};
};

struct TableRange final {
    u32 first{0};
    u32 last{0};
    u32 style{0};
    bool non_base{false};
};

// A glyph's style: its number in the low eight bits (all set: none yet),
// and two marks.
inline constexpr u32 style_mask = 0xFF;
inline constexpr u32 style_unassigned = 0xFF;
inline constexpr u32 style_non_base = 0x100;
inline constexpr u32 style_digit = 0x200;

[[nodiscard]] u32 style_count() noexcept;
[[nodiscard]] const TableScript& script_of_style(u32 style) noexcept;

struct ScaledStyleMetrics final {
    HintScale scale{};
    ScaledAxisMetrics axes[2]{};
};

// The least and the greatest y of a contour's points (26.6); a contour of
// one or two points has least_of_none and greatest_of_none.
struct ContourReach final {
    i64 least{0};
    i64 greatest{0};
};

// What a hinter keeps to work in: storage reused from glyph to glyph.
struct HintScratch final {
    GlyphOutline glyph{};
    HintOutline outline{};
    HintAxis axis{};
    Vector<ContourReach> reaches{};
};

// A Hinter's working memory, kept from glyph to glyph: the scratch, and
// the measures of the last style hinted at the last size and mode (a line
// of text is mostly one style at one size).
struct HintWork final {
    HintScratch scratch{};
    ScaledStyleMetrics scaled{};
    bool scaled_held{false};
    u32 scaled_style{0};
    i64 scaled_size{0};
    HintMode scaled_mode{HintMode::light};
};

// ---- accents (hinter_accents.cpp) --------------------------------------------------------------

// What is done for a glyph by the character it is the glyph of
// (hinter_adjust.inc), as bits. "Up": the topmost contour is an accent and
// is kept a pixel above the rest; "up, second": the contour under the
// topmost one is, and both move; "down" likewise below. A tilde is made
// tall enough to stay a tilde. "Off": edges well above (or below) the
// letter are kept off the blue zones.
inline constexpr u32 adjust_up = 0x01;
inline constexpr u32 adjust_down = 0x02;
inline constexpr u32 adjust_up_second = 0x04;
inline constexpr u32 adjust_down_second = 0x08;
inline constexpr u32 adjust_tilde_top = 0x10;
inline constexpr u32 adjust_tilde_bottom = 0x20;
inline constexpr u32 adjust_tilde_top_second = 0x40;
inline constexpr u32 adjust_tilde_bottom_second = 0x80;
inline constexpr u32 adjust_off_capital_top = 0x100;
inline constexpr u32 adjust_off_capital_bottom = 0x200;
inline constexpr u32 adjust_off_small_top = 0x400;
inline constexpr u32 adjust_off_small_bottom = 0x800;
inline constexpr u32 adjust_any_height = 0x1000;

struct TableAdjust final {
    u32 character{0};
    u32 flags{0};
};

// What is done for each glyph of the font: the adjust_ bits.
[[nodiscard]] Status compute_glyph_adjustments(const Font& font, Vector<u16>& adjustments) noexcept;
// blue_capital_bottom, blue_small_bottom or neither, for a blue zone of
// the table of scripts by its number there.
[[nodiscard]] u32 blue_bottom_of(usize table_blue) noexcept;
// Before the segments along y are found: a tilde is made tall enough and
// put on the grid. False: out of memory.
[[nodiscard]] bool stretch_tildes(HintOutline& outline, u32 adjustment, Vector<ContourReach>& reaches) noexcept;
// After the edges along y are found: the edges of an accent are kept off
// the blue zones. Gives the tallest an accent may be and still be moved.
[[nodiscard]] i32 keep_accents_off_blues(HintAxis& axis, const HintScale& scale, View<ScaledBlue> blues, u32 adjustment) noexcept;
// After the points have moved along y: an accent nearer than a pixel to
// its letter is moved away. False: out of memory.
[[nodiscard]] bool separate_accents(HintOutline& outline, u32 adjustment, i32 accent_height_limit, Vector<ContourReach>& reaches) noexcept;

// The style of every glyph of the font, from its character map.
[[nodiscard]] Status compute_glyph_styles(const Font& font, Vector<u16>& styles) noexcept;
// The measures of one style of the font.
[[nodiscard]] Status compute_style_metrics(const Font& font, u32 style, const FontLimits& limits, HintScratch& scratch, StyleMetrics& metrics) noexcept;
// The scale for a size (26.6 pixels to the em) and a mode.
[[nodiscard]] HintScale make_scale(const Font& font, i64 size, HintMode mode, u32 group) noexcept;
// A style's measures at a scale.
[[nodiscard]] Status scale_style_metrics(const StyleMetrics& metrics, const HintScale& scale, ScaledStyleMetrics& scaled) noexcept;

// The edges of an axis from its segments, in order of position.
// `cjk`: the rule of the CJK scripts. False: out of memory.
[[nodiscard]] bool compute_edges(HintAxis& axis, const ScaledAxisMetrics& metrics, bool top_to_bottom, i32 y_scale, bool cjk) noexcept;
// Each edge and the blue zone it is caught by, if one is near enough.
void compute_blue_edges(HintAxis& axis, const HintScale& scale, View<UnscaledBlue> unscaled, View<ScaledBlue> blues, bool cjk) noexcept;

// Every edge of the axis is given its position at the size: on a blue
// zone, as a stem's side at a fitted width from its other side, or
// between its neighbours.
void hint_edges(const HintOutline& outline, HintAxis& axis, const ScaledAxisMetrics& metrics, bool cjk, const HintScale& scale,
                bool top_to_bottom) noexcept;

// The outline's points go where the edges of the axis went: those of the
// edges' segments, then those between edges, then the rest.
void align_edge_points(HintOutline& outline, const HintAxis& axis, bool cjk, const HintScale& scale) noexcept;
void align_strong_points(HintOutline& outline, HintAxis& axis) noexcept;
void align_weak_points(HintOutline& outline, u32 dimension) noexcept;

void reset_axis(HintAxis& axis, u32 dimension, Winding winding) noexcept;
// The segments of the outline along an axis. False: more than the hinter
// takes, and the axis is left with none.
[[nodiscard]] bool compute_segments(HintOutline& outline, HintAxis& axis, bool& no_memory) noexcept;
// Each segment and the one across the stem from it. `cjk` chooses the
// rule; `scale` is the axis's (16.16); `widest` is the widest standard
// width, or 0.
void link_segments(const HintOutline& outline, HintAxis& axis, i32 scale, bool cjk, i32 widest) noexcept;

// The outline of a glyph as loaded (font units), with every point's
// neighbours, directions and marks worked out.
[[nodiscard]] Status fill_hint_outline(View<Point> glyph_points, View<PointTag> glyph_tags, View<u32> contour_ends, i32 units_per_em, i32 advance,
                                       HintOutline& outline) noexcept;

} // namespace stxlib::glyphs::detail

#endif
