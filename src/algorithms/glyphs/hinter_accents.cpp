// stxlib::glyphs: accents kept clear of their letters (hinter.hpp). After
// FreeType 2.14.2, src/autofit/afadjust.c and the part of aflatin.c that
// uses it (by Craig White): at small sizes the hinter would otherwise put
// an accent against the letter under it, or flatten a tilde into a bar.
//
// A glyph is treated by the character it is the glyph of
// (hinter_adjust.inc). Three things are done, each at its place in the
// hinting along y:
// - before the segments are found, a tilde is made tall enough to keep
//   its wave, put on the grid, and left out of the hinting;
// - when the edges are known, those well above (or below) the letter are
//   kept off the blue zones, so that an accent is not taken for a top;
// - after the points have moved, a contour that is an accent and is
//   nearer than a pixel to the rest is moved a pixel away.
//
// This follows FreeType's source and not Skrifa's (Skrifa 0.50.0 has not
// this step): the owner, 2026-11-15. Where FreeType does something other
// than what it says it means, it is done FreeType's way and said so here,
// since the purpose is the outline FreeType gives.

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs::detail {

namespace {

#include "algorithms/glyphs/hinter_adjust.inc"

// A contour of one or two points has no reach: these stand for it.
inline constexpr i64 least_of_none = 0x7FFFFFFF;
inline constexpr i64 greatest_of_none = -0x7FFFFFFF - 1;

[[nodiscard]] constexpr i64 round_to_pixel(const i64 value) noexcept {
    const i64 result = (value + 32) & ~static_cast<i64>(63);
    return result;
}

[[nodiscard]] constexpr bool has_reach(const ContourReach& reach) noexcept {
    const bool result = reach.least < reach.greatest;
    return result;
}

// The least and greatest y of every contour, as the points now are.
[[nodiscard]] bool compute_reaches(const HintOutline& outline, Vector<ContourReach>& reaches) noexcept {
    reaches.clear();
    const HintPoint* const points = outline.points.view().data();
    for (usize index = 0; index < outline.contours.count(); ++index) {
        const HintContour contour = outline.contours[index];
        ContourReach reach{.least = least_of_none, .greatest = greatest_of_none};
        if (contour.last - contour.first >= 2) {
            for (u32 point = contour.first; point <= contour.last; ++point) {
                const i64 y = points[point].y;
                reach.least = y < reach.least ? y : reach.least;
                reach.greatest = y > reach.greatest ? y : reach.greatest;
            }
        }
        if (reaches.push_copy(reach).failed()) return false;
    }
    return true;
}

// The contour that reaches highest; of two that reach as high, the
// shorter. Contours inside it are inside it, so it is the accent's own.
[[nodiscard]] usize highest_contour(const View<ContourReach> reaches) noexcept {
    usize highest = 0;
    i64 highest_least = least_of_none;
    i64 highest_greatest = greatest_of_none;
    for (usize index = 0; index < reaches.count(); ++index) {
        const ContourReach reach = reaches[index];
        if (reach.greatest > highest_greatest || (reach.greatest == highest_greatest && reach.least > highest_least)) {
            highest_least = reach.least;
            highest_greatest = reach.greatest;
            highest = index;
        }
    }
    return highest;
}

// The contour that reaches highest of those that begin below the bottom
// of the highest one.
[[nodiscard]] usize second_highest_contour(const View<ContourReach> reaches) noexcept {
    usize second = 0;
    if (reaches.count() >= 3) {
        const usize highest = highest_contour(reaches);
        const i64 highest_least = reaches[highest].least;
        i64 second_greatest = greatest_of_none;
        for (usize index = 0; index < reaches.count(); ++index) {
            if (index == highest) continue;
            const ContourReach reach = reaches[index];
            if (reach.greatest > second_greatest && reach.least < highest_least) {
                second_greatest = reach.greatest;
                second = index;
            }
        }
    }
    return second;
}

[[nodiscard]] usize lowest_contour(const View<ContourReach> reaches) noexcept {
    usize lowest = 0;
    i64 lowest_least = least_of_none;
    i64 lowest_greatest = greatest_of_none;
    for (usize index = 0; index < reaches.count(); ++index) {
        const ContourReach reach = reaches[index];
        if (reach.least < lowest_least || (reach.least == lowest_least && reach.greatest < lowest_greatest)) {
            lowest_least = reach.least;
            lowest_greatest = reach.greatest;
            lowest = index;
        }
    }
    return lowest;
}

[[nodiscard]] usize second_lowest_contour(const View<ContourReach> reaches) noexcept {
    usize second = 0;
    if (reaches.count() >= 3) {
        const usize lowest = lowest_contour(reaches);
        const i64 lowest_greatest = reaches[lowest].greatest;
        i64 second_least = least_of_none;
        for (usize index = 0; index < reaches.count(); ++index) {
            if (index == lowest) continue;
            const ContourReach reach = reaches[index];
            if (reach.least < second_least && reach.greatest > lowest_greatest) {
                second_least = reach.least;
                second = index;
            }
        }
    }
    return second;
}

void move_contour(HintOutline& outline, const usize contour_index, const i64 delta) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    const HintContour contour = outline.contours[contour_index];
    for (u32 point = contour.first; point <= contour.last; ++point) points[point].y = static_cast<i32>(points[point].y + delta);
}

// Every contour that begins above `limit` moves up by `delta`. The
// reaches are those taken before anything moved.
void move_contours_up(HintOutline& outline, const View<ContourReach> reaches, const i64 limit, const i64 delta) noexcept {
    for (usize index = 0; index < reaches.count(); ++index) {
        if (has_reach(reaches[index]) && reaches[index].least > limit) move_contour(outline, index, delta);
    }
}

void move_contours_down(HintOutline& outline, const View<ContourReach> reaches, const i64 limit, const i64 delta) noexcept {
    for (usize index = 0; index < reaches.count(); ++index) {
        if (has_reach(reaches[index]) && reaches[index].greatest < limit) move_contour(outline, index, -delta);
    }
}

// The contour is left out of the hinting along y: its points are not
// looked at for segments, and those on the curve stay where they are.
void set_aside(HintOutline& outline, const usize contour_index) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    const HintContour contour = outline.contours[contour_index];
    for (u32 index = contour.first; index <= contour.last; ++index) {
        points[index].marks = points[index].marks | mark_ignore;
        if (points[index].tag == PointTag::on) points[index].marks = points[index].marks | mark_touched_y;
    }
}

// The contours at or above the bottom of `limit_contour` are set aside.
void set_aside_top(HintOutline& outline, const View<ContourReach> reaches, const usize limit_contour) noexcept {
    const i64 limit = reaches[limit_contour].least;
    for (usize index = 0; index < reaches.count(); ++index) {
        if (has_reach(reaches[index]) && reaches[index].least >= limit) set_aside(outline, index);
    }
}

// The contours wholly at or below the bottom of `limit_contour` are set
// aside. (FreeType measures from the contour's bottom here too, so a
// tilde under a letter is itself not set aside.)
void set_aside_bottom(HintOutline& outline, const View<ContourReach> reaches, const usize limit_contour) noexcept {
    const i64 limit = reaches[limit_contour].least;
    for (usize index = 0; index < reaches.count(); ++index) {
        if (has_reach(reaches[index]) && reaches[index].greatest <= limit) set_aside(outline, index);
    }
}

// How far the inner turn of a tilde is from the tilde's far side: over
// the points on the curve that lie level between two control points and
// are neither the tilde's top nor its bottom, the least distance to the
// side they turn away from; 0 if there is none. Turns nearer than an
// eighth of the tilde's height to that side are not turns.
[[nodiscard]] i64 inner_turn(const HintOutline& outline, const usize contour_index, const ContourReach reach) noexcept {
    const HintPoint* const points = outline.points.view().data();
    const HintContour contour = outline.contours[contour_index];
    const i64 threshold = (reach.greatest - reach.least) / 8;
    const u32 length = contour.last - contour.first + 1;
    bool taken = false;
    i64 least = 0;
    for (u32 index = contour.first; index <= contour.last; ++index) {
        const HintPoint& point = points[index];
        const HintPoint& before = points[previous_in(contour, index)];
        const HintPoint& after = points[next_in(contour, index)];
        const bool candidate = point.tag == PointTag::on && before.y == point.y && after.y == point.y && point.y != reach.least &&
                               point.y != reach.greatest && before.tag != PointTag::on && after.tag != PointTag::on;
        if (!candidate) continue;
        // The points on the curve either side of it.
        u32 before_on = previous_in(contour, index);
        for (u32 turn = 0; turn < length && points[before_on].tag != PointTag::on; ++turn) before_on = previous_in(contour, before_on);
        u32 after_on = next_in(contour, index);
        for (u32 turn = 0; turn < length && points[after_on].tag != PointTag::on; ++turn) after_on = next_in(contour, after_on);
        i64 measured = 0;
        if (points[after_on].y > point.y && points[before_on].y > point.y) {
            measured = point.y - reach.least;
        } else if (points[after_on].y < point.y && points[before_on].y < point.y) {
            measured = reach.greatest - point.y;
        } else {
            continue;
        }
        if (measured < threshold) continue;
        if (!taken || measured < least) {
            taken = true;
            least = measured;
        }
    }
    return least;
}

// A tilde over a letter is made tall enough that its wave is a pixel deep,
// from its bottom up. Gives how much taller it became. The contours at or
// above its bottom are set aside whether it grew or not.
[[nodiscard]] i64 stretch_top_tilde(HintOutline& outline, const View<ContourReach> reaches, const usize contour_index) noexcept {
    const ContourReach reach = reaches[contour_index];
    // A contour with no reach is no tilde. (FreeType goes on with numbers
    // that mean nothing.)
    if (!has_reach(reach)) return 0;
    const i64 height = reach.greatest - reach.least;
    const i64 target = inner_turn(outline, contour_index, reach) + 64;
    set_aside_top(outline, reaches, contour_index);
    if (height >= target) return 0;
    HintPoint* const points = outline.points.mutable_view().data();
    const HintContour contour = outline.contours[contour_index];
    for (u32 index = contour.first; index <= contour.last; ++index) {
        points[index].y = static_cast<i32>((points[index].y - reach.least) * target / height + reach.least);
    }
    const i64 result = target - height;
    return result;
}

// The same for a tilde under a letter, from its top down.
[[nodiscard]] i64 stretch_bottom_tilde(HintOutline& outline, const View<ContourReach> reaches, const usize contour_index) noexcept {
    const ContourReach reach = reaches[contour_index];
    if (!has_reach(reach)) return 0;
    const i64 height = reach.greatest - reach.least;
    const i64 target = inner_turn(outline, contour_index, reach) + 64;
    set_aside_bottom(outline, reaches, contour_index);
    if (height >= target) return 0;
    HintPoint* const points = outline.points.mutable_view().data();
    const HintContour contour = outline.contours[contour_index];
    for (u32 index = contour.first; index <= contour.last; ++index) {
        points[index].y = static_cast<i32>((points[index].y - reach.greatest) * target / height + reach.greatest);
    }
    const i64 result = target - height;
    return result;
}

// A tilde that was set aside is not fitted by the hinter, so it is put on
// the grid here: its bottom (for one over a letter) on a whole pixel, or,
// if it is under three pixels tall, its middle. Gives how far it moved.
[[nodiscard]] i64 align_tilde(HintOutline& outline, const usize contour_index, const bool top) noexcept {
    const HintPoint* const points = outline.points.view().data();
    const HintContour contour = outline.contours[contour_index];
    i64 least = points[contour.first].y;
    i64 greatest = points[contour.first].y;
    for (u32 index = contour.first; index <= contour.last; ++index) {
        const i64 y = points[index].y;
        least = y < least ? y : least;
        greatest = y > greatest ? y : greatest;
    }
    const i64 height = greatest - least;
    const i64 centring = height < 64 * 3 ? (round_to_pixel(height) - height) / 2 : 0;
    const i64 delta = top ? round_to_pixel(least) - least + centring : round_to_pixel(greatest) - greatest - centring;
    move_contour(outline, contour_index, delta);
    return delta;
}

// Whether a contour and the rest of the outline share any of the x axis.
[[nodiscard]] bool overlaps_the_rest(const HintOutline& outline, const usize contour_index) noexcept {
    const HintPoint* const points = outline.points.view().data();
    i64 own_least = least_of_none;
    i64 own_greatest = greatest_of_none;
    i64 others_least = least_of_none;
    i64 others_greatest = greatest_of_none;
    for (usize index = 0; index < outline.contours.count(); ++index) {
        const HintContour contour = outline.contours[index];
        if (contour.last - contour.first < 2) continue;
        for (u32 point = contour.first; point <= contour.last; ++point) {
            const i64 x = points[point].x;
            if (index == contour_index) {
                own_least = x < own_least ? x : own_least;
                own_greatest = x > own_greatest ? x : own_greatest;
            } else {
                others_least = x < others_least ? x : others_least;
                others_greatest = x > others_greatest ? x : others_greatest;
            }
        }
    }
    const bool result = (others_least <= own_greatest && own_greatest <= others_greatest) ||
                        (others_least <= own_least && own_least <= others_greatest) ||
                        (own_greatest >= others_greatest && own_least <= others_least);
    return result;
}

// An accent over the letter is moved up until it is a pixel clear. False:
// nothing more is done for the glyph.
[[nodiscard]] bool raise_accent(HintOutline& outline, const View<ContourReach> reaches, const u32 adjustment, const i64 height_limit) noexcept {
    const bool topmost = (adjustment & adjust_up) != 0;
    const bool second = (adjustment & adjust_up_second) != 0;
    const bool topmost_is_tilde = (adjustment & adjust_tilde_top) != 0;
    const bool second_is_tilde = (adjustment & adjust_tilde_top_second) != 0;
    const usize high = second ? second_highest_contour(reaches) : highest_contour(reaches);
    // An accent beside the letter and not over it is left alone.
    if (!overlaps_the_rest(outline, high)) return false;
    const ContourReach reach = reaches[high];
    // A contour with no reach: FreeType's height for it comes to 1, and
    // nothing moves.
    const i64 height = has_reach(reach) ? reach.greatest - reach.least : 1;
    // What is too tall is not an accent.
    if (height > height_limit) return false;
    if (!has_reach(reach)) return true;
    // The nearest of the contours under it, if nearer than a pixel.
    i64 nearest = 64;
    for (usize index = 0; index < reaches.count(); ++index) {
        if (index == high) continue;
        const i64 distance = reach.least - reaches[index].greatest;
        if (distance < 64 && distance < nearest && reaches[index].least < reach.least) nearest = distance;
    }
    const i64 amount = 64 - nearest;
    // Moving a tilde that was put with its middle on the grid would undo
    // that: it is moved a little further to restore it.
    i64 centring = 0;
    if (topmost_is_tilde || second_is_tilde) {
        const usize tilde = (topmost || second_is_tilde) ? high : highest_contour(reaches);
        const i64 tilde_height = reaches[tilde].greatest - reaches[tilde].least;
        i64 position = reach.least + amount;
        if (second && topmost_is_tilde) position = position + height;
        if (position % 64 == 0 && tilde_height < 3 * 64) centring = (round_to_pixel(tilde_height) - tilde_height) / 2;
    }
    const i64 calculated = ((topmost && topmost_is_tilde) || (second && second_is_tilde)) ? amount + centring : amount;
    // Two sixty-fourths either way are rounding.
    if (calculated != 0 && calculated >= -2 && (calculated <= 66 || amount <= 66)) {
        // Everything at about the accent's height or above goes with it:
        // the inside of a ring, the second stroke of a double acute.
        const i64 limit = reach.least - height / 8;
        move_contours_up(outline, reaches, limit, amount);
        if (second && topmost_is_tilde) move_contours_up(outline, reaches, limit + height, centring);
    }
    return true;
}

// The same for an accent under the letter.
void lower_accent(HintOutline& outline, const View<ContourReach> reaches, const u32 adjustment, const i64 height_limit) noexcept {
    const bool bottommost = (adjustment & adjust_down) != 0;
    const bool second = (adjustment & adjust_down_second) != 0;
    const bool bottommost_is_tilde = (adjustment & adjust_tilde_bottom) != 0;
    const bool second_is_tilde = (adjustment & adjust_tilde_bottom_second) != 0;
    const usize low = second ? second_lowest_contour(reaches) : lowest_contour(reaches);
    if (!overlaps_the_rest(outline, low)) return;
    const ContourReach reach = reaches[low];
    const i64 height = has_reach(reach) ? reach.greatest - reach.least : 1;
    if (height > height_limit) return;
    if (!has_reach(reach)) return;
    i64 nearest = 64;
    for (usize index = 0; index < reaches.count(); ++index) {
        if (index == low) continue;
        const i64 distance = reaches[index].least - reach.greatest;
        if (distance < 64 && distance < nearest && reaches[index].greatest > reach.greatest) nearest = distance;
    }
    const i64 amount = 64 - nearest;
    i64 centring = 0;
    if (bottommost_is_tilde || second_is_tilde) {
        const usize tilde = (bottommost || second_is_tilde) ? low : lowest_contour(reaches);
        const i64 tilde_height = reaches[tilde].greatest - reaches[tilde].least;
        i64 position = reach.greatest - amount;
        if (second && bottommost_is_tilde) position = position - height;
        if (position % 64 == 0 && tilde_height < 3 * 64) centring = (round_to_pixel(tilde_height) - tilde_height) / 2;
    }
    const i64 calculated = ((bottommost && bottommost_is_tilde) || (second && second_is_tilde)) ? amount + centring : amount;
    if (calculated != 0 && calculated >= -2 && (calculated <= 66 || amount <= 66)) {
        const i64 limit = reach.greatest + height / 8;
        move_contours_down(outline, reaches, limit, amount);
        if (second && bottommost_is_tilde) move_contours_down(outline, reaches, limit - height, centring);
    }
}

// The first zone in use that has all of `wanted`, or no_index.
[[nodiscard]] usize zone_with(const View<ScaledBlue> blues, const u32 wanted) noexcept {
    usize found = no_index;
    for (usize index = 0; index < blues.count(); ++index) {
        if (blues[index].active && (blues[index].zones & wanted) == wanted) {
            found = index;
            break;
        }
    }
    return found;
}

} // namespace

u32 blue_bottom_of(const usize table_blue) noexcept {
    const usize count = sizeof(table_blue_bottoms) / sizeof(table_blue_bottoms[0]);
    const u32 kind = table_blue < count ? table_blue_bottoms[table_blue] : 0U;
    const u32 result = ((kind & 1U) != 0 ? blue_capital_bottom : 0U) | ((kind & 2U) != 0 ? blue_small_bottom : 0U);
    return result;
}

OperationStatus compute_glyph_adjustments(const Font& font, Vector<u16>& adjustments) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    adjustments.clear();
    if (adjustments.resize(font.glyph_count).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    u16* const all = adjustments.mutable_view().data();
    for (usize glyph = 0; glyph < font.glyph_count; ++glyph) all[glyph] = 0;
    // A glyph of several of the table's characters takes the last one's.
    const usize count = sizeof(table_adjust) / sizeof(table_adjust[0]);
    for (usize index = 0; index < count; ++index) {
        const u32 glyph = glyph_of(font, table_adjust[index].character);
        if (glyph == 0 || glyph >= font.glyph_count) continue;
        all[glyph] = static_cast<u16>(table_adjust[index].flags);
    }
    return OperationStatus::success();
}

bool stretch_tildes(HintOutline& outline, const u32 adjustment, Vector<ContourReach>& reaches) noexcept {
    const u32 tildes = adjust_tilde_top | adjust_tilde_bottom | adjust_tilde_top_second | adjust_tilde_bottom_second;
    if ((adjustment & tildes) == 0 || outline.contours.empty()) return true;
    if (!compute_reaches(outline, reaches)) return false;
    const View<ContourReach> view = reaches.view();
    // A tilde with an accent beyond it first: that accent goes with it.
    if ((adjustment & adjust_tilde_top_second) != 0) {
        const usize tilde = second_highest_contour(view);
        const i64 grown = stretch_top_tilde(outline, view, tilde);
        const i64 moved = align_tilde(outline, tilde, true);
        move_contours_up(outline, view, view[tilde].least, grown + moved);
    }
    if ((adjustment & adjust_tilde_bottom_second) != 0) {
        const usize tilde = second_lowest_contour(view);
        const i64 grown = stretch_bottom_tilde(outline, view, tilde);
        const i64 moved = align_tilde(outline, tilde, false);
        move_contours_down(outline, view, view[tilde].greatest, grown - moved);
    }
    if ((adjustment & adjust_tilde_top) != 0) {
        const usize tilde = highest_contour(view);
        static_cast<void>(stretch_top_tilde(outline, view, tilde));
        static_cast<void>(align_tilde(outline, tilde, true));
    }
    if ((adjustment & adjust_tilde_bottom) != 0) {
        const usize tilde = lowest_contour(view);
        static_cast<void>(stretch_bottom_tilde(outline, view, tilde));
        static_cast<void>(align_tilde(outline, tilde, false));
    }
    return true;
}

i32 keep_accents_off_blues(HintAxis& axis, const HintScale& scale, const View<ScaledBlue> blues, const u32 adjustment) noexcept {
    if (adjustment == 0) return 0;
    // The letter under the accent: a capital between the first top zone
    // and the zone of capitals' bottoms, a small letter between the
    // x-height and the zone of small letters' bottoms.
    const usize capital_top = zone_with(blues, blue_top);
    const usize capital_bottom = zone_with(blues, blue_capital_bottom);
    const usize small_top = zone_with(blues, blue_adjustment);
    const usize small_bottom = zone_with(blues, blue_small_bottom);
    const bool capitals = capital_top != no_index && capital_bottom != no_index;
    const bool smalls = small_top != no_index && small_bottom != no_index;
    const i32 capital_height = capitals ? blues[capital_top].overshoot.scaled - blues[capital_bottom].overshoot.scaled : 0;
    const i32 small_height = smalls ? blues[small_top].overshoot.scaled - blues[small_bottom].overshoot.scaled : 0;
    // An accent is no taller than two thirds of a small letter, or half a
    // capital, or four tenths of the em. (For a character marked "any
    // height" FreeType leaves this at nothing, which moves no accent at
    // all; it is left so.)
    i32 limit = 0;
    if ((adjustment & adjust_any_height) == 0) {
        if (smalls) {
            limit = 2 * small_height / 3;
        } else if (capitals) {
            limit = capital_height / 2;
        } else {
            limit = times_fixed(scale.units_per_em * 4 / 10, scale.y_scale);
        }
    }
    // Edges more than a seventh of the letter's height (and a quarter of a
    // pixel) beyond the letter are an accent's.
    HintEdge* const edges = axis.edges.mutable_view().data();
    const usize count = axis.edges.count();
    if (capitals) {
        if ((adjustment & adjust_off_capital_top) != 0) {
            const i32 above = blues[capital_top].overshoot.scaled + capital_height / 7 + 16;
            for (usize index = 0; index < count; ++index) {
                if (edges[index].position > above) edges[index].flags = edges[index].flags | topo_no_blue;
            }
        }
        if ((adjustment & adjust_off_capital_bottom) != 0) {
            const i32 below = blues[capital_bottom].overshoot.scaled - capital_height / 7 - 16;
            for (usize index = 0; index < count; ++index) {
                if (edges[index].position < below) edges[index].flags = edges[index].flags | topo_no_blue;
            }
        }
    }
    if (smalls) {
        if ((adjustment & adjust_off_small_top) != 0) {
            const i32 above = blues[small_top].overshoot.scaled + small_height / 7 + 16;
            for (usize index = 0; index < count; ++index) {
                if (edges[index].position > above) edges[index].flags = edges[index].flags | topo_no_blue;
            }
        }
        if ((adjustment & adjust_off_small_bottom) != 0) {
            const i32 below = blues[small_bottom].overshoot.scaled - small_height / 7 - 16;
            for (usize index = 0; index < count; ++index) {
                if (edges[index].position < below) edges[index].flags = edges[index].flags | topo_no_blue;
            }
        }
    }
    return limit;
}

bool separate_accents(HintOutline& outline, const u32 adjustment, const i32 accent_height_limit, Vector<ContourReach>& reaches) noexcept {
    const usize contours = outline.contours.count();
    const bool above = ((adjustment & adjust_up) != 0 && contours >= 2) || ((adjustment & adjust_up_second) != 0 && contours >= 3);
    const bool below = ((adjustment & adjust_down) != 0 && contours >= 2) || ((adjustment & adjust_down_second) != 0 && contours >= 3);
    if (!above && !below) return true;
    // Where the contours are now that they are hinted.
    if (!compute_reaches(outline, reaches)) return false;
    const View<ContourReach> view = reaches.view();
    const bool go_on = above ? raise_accent(outline, view, adjustment, accent_height_limit) : true;
    if (go_on && below) lower_accent(outline, view, adjustment, accent_height_limit);
    return true;
}

} // namespace stxlib::glyphs::detail
