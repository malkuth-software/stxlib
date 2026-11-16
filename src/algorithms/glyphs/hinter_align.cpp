// stxlib::glyphs: the auto-hinter's moving of points (hinter.hpp). After
// Skrifa 0.50.0, outline/autohint/hint/outline.rs (itself after FreeType's
// afhints.c): the points of the edges go where the edges went, the other
// points that count go between the edges either side of them, and the
// rest are carried between the points that moved.

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs::detail {

namespace {

void store(HintPoint& point, const u32 dimension, const i32 value) noexcept {
    if (dimension == axis_horizontal) {
        point.x = value;
        point.marks = point.marks | mark_touched_x;
    } else {
        point.y = value;
        point.marks = point.marks | mark_touched_y;
    }
}

// Points [first, last] of a contour move as its one moved point did.
void shift(HintPoint* const points, const usize first, const usize last, const usize reference) noexcept {
    const i32 delta = points[reference].u - points[reference].v;
    if (delta == 0) return;
    for (usize index = first; index < reference; ++index) points[index].u = points[index].v + delta;
    for (usize index = reference + 1; index <= last; ++index) points[index].u = points[index].v + delta;
}

// Points [first, last] go between two moved points: beyond either, as it
// moved; between them, in proportion.
void interpolate(HintPoint* const points, const usize first, const usize last, const usize reference1, const usize reference2) noexcept {
    if (first > last) return;
    const bool swapped = points[reference1].v > points[reference2].v;
    const HintPoint& low = swapped ? points[reference2] : points[reference1];
    const HintPoint& high = swapped ? points[reference1] : points[reference2];
    const i32 u1 = low.u;
    const i32 v1 = low.v;
    const i32 u2 = high.u;
    const i32 v2 = high.v;
    const i32 d1 = u1 - v1;
    const i32 d2 = u2 - v2;
    const bool level = u1 == u2 || v1 == v2;
    const i32 scale = level ? 0 : divide_fixed(static_cast<i64>(u2) - u1, static_cast<i64>(v2) - v1);
    for (usize index = first; index <= last; ++index) {
        const i32 v = points[index].v;
        if (v <= v1) {
            points[index].u = v + d1;
        } else if (v >= v2) {
            points[index].u = v + d2;
        } else {
            points[index].u = level ? u1 : u1 + times_fixed(static_cast<i64>(v) - v1, scale);
        }
    }
}

} // namespace

void align_edge_points(HintOutline& outline, const HintAxis& axis, const bool cjk, const HintScale& scale) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    const usize point_count = outline.points.count();
    // The default scripts put the points on the edge; the others move them
    // by what the edge moved, unless the mode snaps.
    const bool snap = !cjk || (axis.dimension == axis_horizontal && (scale.flags & scale_horizontal_snap) != 0) ||
                      (axis.dimension == axis_vertical && (scale.flags & scale_vertical_snap) != 0);
    for (usize index = 0; index < axis.segments.count(); ++index) {
        const HintSegment& segment = axis.segments[index];
        if (segment.edge == no_index) continue;
        const HintEdge& edge = axis.edges[segment.edge];
        const i32 delta = edge.position - edge.original;
        usize at = segment.first;
        for (usize turn = 0; turn <= point_count; ++turn) {
            HintPoint& point = points[at];
            if (axis.dimension == axis_horizontal) {
                point.x = snap ? edge.position : point.x + delta;
                point.marks = point.marks | mark_touched_x;
            } else {
                point.y = snap ? edge.position : point.y + delta;
                point.marks = point.marks | mark_touched_y;
            }
            if (at == segment.last) break;
            at = point.next;
        }
    }
}

void align_strong_points(HintOutline& outline, HintAxis& axis) noexcept {
    const usize edge_count = axis.edges.count();
    if (edge_count == 0) return;
    const u32 dimension = axis.dimension;
    const u32 touched = dimension == axis_horizontal ? mark_touched_x : mark_touched_y;
    HintPoint* const points = outline.points.mutable_view().data();
    HintEdge* const edges = axis.edges.mutable_view().data();
    const usize point_count = outline.points.count();
    for (usize index = 0; index < point_count; ++index) {
        HintPoint& point = points[index];
        if ((point.marks & (touched | mark_weak)) != 0) continue;
        const i32 u = dimension == axis_vertical ? point.fy : point.fx;
        const i32 ou = dimension == axis_vertical ? point.oy : point.ox;
        // Before the first edge or after the last: as that edge moved.
        const HintEdge& first = edges[0];
        if (first.font_position - u >= 0) {
            store(point, dimension, first.position - (first.original - ou));
            continue;
        }
        const HintEdge& last = edges[edge_count - 1];
        if (u - last.font_position >= 0) {
            store(point, dimension, last.position + (ou - last.original));
            continue;
        }
        // The first edge at or beyond the point: by looking along a few
        // edges, by halving among many.
        usize least = 0;
        bool on_edge = false;
        if (edge_count <= 8) {
            for (usize edge = 0; edge < edge_count; ++edge) {
                if (edges[edge].font_position >= u) {
                    least = edge;
                    on_edge = edges[edge].font_position == u;
                    break;
                }
            }
        } else {
            usize most = edge_count;
            while (least < most) {
                const usize middle = (least + most) >> 1;
                const i32 position = edges[middle].font_position;
                if (u < position) {
                    most = middle;
                } else if (u > position) {
                    least = middle + 1;
                } else {
                    least = middle;
                    on_edge = true;
                    break;
                }
            }
        }
        if (on_edge) {
            store(point, dimension, edges[least].position);
            continue;
        }
        if (least == 0 || least >= edge_count) continue;
        // Between two edges: in proportion. The proportion is worked out
        // once for the edge before.
        HintEdge& before = edges[least - 1];
        if (before.scale == 0) {
            const HintEdge& after = edges[least];
            before.scale = divide_fixed(static_cast<i64>(after.position) - before.position,
                                        static_cast<i64>(after.font_position) - before.font_position);
        }
        store(point, dimension, before.position + times_fixed(static_cast<i64>(u) - before.font_position, before.scale));
    }
}

void align_weak_points(HintOutline& outline, const u32 dimension) noexcept {
    HintPoint* const all = outline.points.mutable_view().data();
    const usize point_count = outline.points.count();
    const u32 touched = dimension == axis_horizontal ? mark_touched_x : mark_touched_y;
    for (usize index = 0; index < point_count; ++index) {
        all[index].u = dimension == axis_horizontal ? all[index].x : all[index].y;
        all[index].v = dimension == axis_horizontal ? all[index].ox : all[index].oy;
    }
    for (usize contour_index = 0; contour_index < outline.contours.count(); ++contour_index) {
        const HintContour contour = outline.contours[contour_index];
        HintPoint* const points = all + contour.first;
        const usize last = contour.last - contour.first;
        usize first_touched = no_index;
        for (usize index = 0; index <= last; ++index) {
            if ((points[index].marks & touched) != 0) {
                first_touched = index;
                break;
            }
        }
        if (first_touched == no_index) continue;
        usize at = first_touched;
        usize last_touched = first_touched;
        // Each stretch of untouched points between two touched ones.
        for (usize turn = 0; turn <= last + 1; ++turn) {
            while (at < last && (points[at + 1].marks & touched) != 0) at = at + 1;
            last_touched = at;
            at = at + 1;
            while (at <= last && (points[at].marks & touched) == 0) at = at + 1;
            if (at > last) break;
            interpolate(points, last_touched + 1, at - 1, last_touched, at);
        }
        if (last_touched == first_touched) {
            shift(points, 0, last, first_touched);
        } else {
            // Round the end of the contour to its beginning.
            if (last_touched < last) interpolate(points, last_touched + 1, last, last_touched, first_touched);
            if (first_touched > 0) interpolate(points, 0, first_touched - 1, last_touched, first_touched);
        }
    }
    for (usize index = 0; index < point_count; ++index) {
        if (dimension == axis_horizontal) {
            all[index].x = all[index].u;
        } else {
            all[index].y = all[index].u;
        }
    }
}

} // namespace stxlib::glyphs::detail
