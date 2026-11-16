// stxlib::glyphs: the auto-hinter's look at an outline, before any size is
// known (hinter.hpp). After Skrifa 0.50.0, outline/autohint/outline.rs
// (itself after FreeType's afhints.c): the points linked round their
// contours, the directions the outline runs in, and the points that carry
// no weight of their own.

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs::detail {

namespace {

[[nodiscard]] constexpr i32 size_of(const i32 value) noexcept {
    const i32 result = value < 0 ? -value : value;
    return result;
}

// A stand-in for the length of a step: the greater arm and three eighths
// of the lesser.
[[nodiscard]] constexpr i32 length_of(const i32 x, const i32 y) noexcept {
    const i32 ax = size_of(x);
    const i32 ay = size_of(y);
    const i32 result = ax > ay ? ax + ((3 * ay) >> 3) : ay + ((3 * ax) >> 3);
    return result;
}

// Whether the outline goes nearly straight on through a corner.
[[nodiscard]] constexpr bool corner_is_flat(const i32 in_x, const i32 in_y, const i32 out_x, const i32 out_y) noexcept {
    const i32 d_in = length_of(in_x, in_y);
    const i32 d_out = length_of(out_x, out_y);
    const i32 d_through = length_of(in_x + out_x, in_y + out_y);
    const bool result = (d_in + d_out - d_through) < (d_through >> 4);
    return result;
}

void link_points(HintOutline& outline) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    for (usize index = 0; index < outline.contours.count(); ++index) {
        const HintContour contour = outline.contours[index];
        u32 previous = contour.last;
        for (u32 point = contour.first; point <= contour.last; ++point) {
            points[point].previous = previous;
            points[point].next = point + 1;
            previous = point;
        }
        points[contour.last].next = contour.first;
    }
}

void mark_near_points(HintOutline& outline, const i32 near_limit) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    for (usize index = 0; index < outline.contours.count(); ++index) {
        const HintContour contour = outline.contours[index];
        u32 previous = contour.last;
        for (u32 point = contour.first; point <= contour.last; ++point) {
            const i32 out_x = points[point].fx - points[previous].fx;
            const i32 out_y = points[point].fy - points[previous].fy;
            if (size_of(out_x) + size_of(out_y) < near_limit) points[previous].marks = points[previous].marks | mark_near;
            previous = point;
        }
    }
}

void compute_directions(HintOutline& outline, const i32 near_limit) noexcept {
    const i32 near_limit2 = 2 * near_limit - 1;
    HintPoint* const points = outline.points.mutable_view().data();
    for (usize index = 0; index < outline.contours.count(); ++index) {
        const HintContour contour = outline.contours[index];
        // Back from the contour's first point to where a step of some
        // length ends: the first point that counts.
        u32 first = contour.first;
        u32 at = first;
        u32 previous = previous_in(contour, first);
        HintPoint point = points[first];
        while (previous != first) {
            const HintPoint before = points[previous];
            const i32 out_x = point.fx - before.fx;
            const i32 out_y = point.fy - before.fy;
            if (size_of(out_x) + size_of(out_y) >= near_limit2) break;
            point = before;
            at = previous;
            previous = previous_in(contour, previous);
        }
        first = at;
        points[first].u = static_cast<i32>(first);
        points[first].v = static_cast<i32>(first);
        u32 next = first;
        at = first;
        i32 out_x = 0;
        i32 out_y = 0;
        // Round the contour once: steps are added up until they amount to
        // something, and the points passed on the way take the direction
        // of the whole.
        const u32 length = contour.last - contour.first + 1;
        for (u32 turn = 0; turn < length; ++turn) {
            const u32 from = next;
            next = next_in(contour, from);
            out_x = out_x + (points[next].fx - points[from].fx);
            out_y = out_y + (points[next].fy - points[from].fy);
            if (size_of(out_x) + size_of(out_y) < near_limit) {
                points[next].marks = points[next].marks | mark_weak;
                if (next == first) break;
                continue;
            }
            const Direction out_direction = direction_of(out_x, out_y);
            points[next].in_direction = out_direction;
            points[next].v = static_cast<i32>(at);
            points[at].u = static_cast<i32>(next);
            points[at].out_direction = out_direction;
            u32 between = next_in(contour, at);
            while (between != next) {
                points[between].in_direction = out_direction;
                points[between].out_direction = out_direction;
                between = next_in(contour, between);
            }
            at = next;
            points[at].u = static_cast<i32>(first);
            points[first].v = static_cast<i32>(at);
            out_x = 0;
            out_y = 0;
            if (next == first) break;
        }
    }
}

// A point with no direction either side, between two steps that go the
// same way in both coordinates, carries no weight.
void simplify_topology(HintOutline& outline) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    const usize count = outline.points.count();
    for (usize index = 0; index < count; ++index) {
        const HintPoint point = points[index];
        if ((point.marks & mark_weak) != 0) continue;
        if (point.in_direction != Direction::none || point.out_direction != Direction::none) continue;
        const usize u = static_cast<usize>(point.u);
        const usize v = static_cast<usize>(point.v);
        const i32 in_x = point.fx - points[v].fx;
        const i32 in_y = point.fy - points[v].fy;
        const i32 out_x = points[u].fx - point.fx;
        const i32 out_y = points[u].fy - point.fy;
        if ((in_x ^ out_x) >= 0 && (in_y ^ out_y) >= 0) {
            points[index].marks = points[index].marks | mark_weak;
            points[v].u = static_cast<i32>(u);
            points[u].v = static_cast<i32>(v);
        }
    }
}

void check_remaining_weak_points(HintOutline& outline) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    const usize count = outline.points.count();
    for (usize index = 0; index < count; ++index) {
        const HintPoint point = points[index];
        if ((point.marks & mark_weak) != 0) continue;
        bool weak = false;
        if (point.tag != PointTag::on) {
            weak = true;
        } else if (point.out_direction == point.in_direction) {
            if (point.out_direction != Direction::none) {
                weak = true;
            } else {
                const usize u = static_cast<usize>(point.u);
                const usize v = static_cast<usize>(point.v);
                if (corner_is_flat(point.fx - points[v].fx, point.fy - points[v].fy, points[u].fx - point.fx, points[u].fy - point.fy)) {
                    weak = true;
                    points[v].u = static_cast<i32>(u);
                    points[u].v = static_cast<i32>(v);
                }
            }
        } else if (opposite(point.in_direction, point.out_direction)) {
            weak = true;
        }
        if (weak) points[index].marks = points[index].marks | mark_weak;
    }
}

void compute_winding(HintOutline& outline) noexcept {
    outline.winding = Winding::none;
    const HintPoint* const points = outline.points.view().data();
    i64 area = 0;
    for (usize index = 0; index < outline.contours.count(); ++index) {
        const HintContour contour = outline.contours[index];
        i64 previous_x = points[contour.last].fx;
        i64 previous_y = points[contour.last].fy;
        for (u32 point = contour.first; point <= contour.last; ++point) {
            const i64 x = points[point].fx;
            const i64 y = points[point].fy;
            area = area + (y - previous_y) * (x + previous_x);
            previous_x = x;
            previous_y = y;
        }
    }
    if (area < 0) outline.winding = Winding::counter_clockwise;
    if (area > 0) outline.winding = Winding::clockwise;
}

} // namespace

Status fill_hint_outline(const GlyphOutline& glyph, const i32 units_per_em, const i32 advance, HintOutline& outline) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    outline.points.clear();
    outline.contours.clear();
    outline.units_per_em = units_per_em;
    outline.advance = advance;
    outline.winding = Winding::none;
    const usize count = glyph.points.count();
    if (glyph.tags.count() != count) return refuse_font(ErrorKind::invalid_argument, operation);
    if (outline.points.resize(count).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    HintPoint* const points = outline.points.mutable_view().data();
    for (usize index = 0; index < count; ++index) {
        points[index] = HintPoint{};
        points[index].tag = glyph.tags[index];
        points[index].fx = glyph.points[index].x;
        points[index].fy = glyph.points[index].y;
    }
    usize first = 0;
    for (usize index = 0; index < glyph.contour_ends.count(); ++index) {
        const usize last = glyph.contour_ends[index];
        if (last < first || last >= count) return refuse_font(ErrorKind::invalid_argument, operation);
        const HintContour contour{.first = static_cast<u32>(first), .last = static_cast<u32>(last)};
        if (outline.contours.push_copy(contour).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        first = last + 1;
    }
    if (first != count) return refuse_font(ErrorKind::invalid_argument, operation);
    // Steps shorter than this many units (twenty at 2048 to the em) are
    // too short to have a direction.
    const i32 near_limit = 20 * units_per_em / 2048;
    link_points(outline);
    mark_near_points(outline, near_limit);
    compute_directions(outline, near_limit);
    simplify_topology(outline);
    check_remaining_weak_points(outline);
    compute_winding(outline);
    return Status::success();
}

} // namespace stxlib::glyphs::detail
