// stxlib::glyphs: the auto-hinter's segments (hinter.hpp). After Skrifa
// 0.50.0, outline/autohint/topo/segments.rs and topo/mod.rs (themselves
// after FreeType's aflatin.c and afcjk.c): the runs of points along each
// direction of an axis, and which run is across the stem from which.

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs::detail {

namespace {

[[nodiscard]] constexpr i32 lesser(const i32 a, const i32 b) noexcept {
    const i32 result = a < b ? a : b;
    return result;
}

[[nodiscard]] constexpr i32 greater(const i32 a, const i32 b) noexcept {
    const i32 result = a > b ? a : b;
    return result;
}

[[nodiscard]] constexpr Direction normalized(const Direction direction) noexcept {
    if (direction == Direction::left) return Direction::right;
    if (direction == Direction::down) return Direction::up;
    return direction;
}

// What a segment being followed has spanned so far.
struct Reach final {
    i32 least_position{highest_score};
    i32 greatest_position{lowest_score};
    i32 least{highest_score};
    i32 greatest{lowest_score};
    bool least_on{false};
    bool greatest_on{false};
    i32 least_on_curve{highest_score};
    i32 greatest_on_curve{lowest_score};
};

void apply_reach(const Reach& reach, HintSegment& segment, const i32 flat_threshold) noexcept {
    segment.position = (reach.least_position + reach.greatest_position) >> 1;
    segment.delta = (reach.greatest_position - reach.least_position) >> 1;
    // A segment that ends on a control point and whose points on the curve
    // span little is round.
    if ((!reach.least_on || !reach.greatest_on) && (reach.greatest_on_curve - reach.least_on_curve) < flat_threshold) {
        segment.flags = segment.flags | topo_round;
    }
    segment.least = reach.least;
    segment.greatest = reach.greatest;
    segment.height = reach.greatest - reach.least;
}

void assign_positions(HintOutline& outline, const u32 dimension) noexcept {
    HintPoint* const points = outline.points.mutable_view().data();
    const usize count = outline.points.count();
    for (usize index = 0; index < count; ++index) {
        points[index].u = dimension == axis_horizontal ? points[index].fx : points[index].fy;
        points[index].v = dimension == axis_horizontal ? points[index].fy : points[index].fx;
    }
}

[[nodiscard]] bool build_segments(HintOutline& outline, HintAxis& axis, bool& no_memory) noexcept {
    const i32 flat_threshold = outline.units_per_em / 14;
    axis.segments.clear();
    const Direction major = normalized(axis.major_direction);
    Direction segment_direction = major;
    const HintPoint* const points = outline.points.view().data();
    for (usize contour_index = 0; contour_index < outline.contours.count(); ++contour_index) {
        const HintContour contour = outline.contours[contour_index];
        const bool single = contour.first == contour.last;
        u32 point_index = contour.first;
        u32 last_index = previous_in(contour, point_index);
        Reach reach{};
        Reach previous_reach{};
        usize previous_segment = no_index;
        usize segment_index = 0;
        // A contour that begins inside a segment is begun where that
        // segment begins.
        if (same_axis(points[point_index].out_direction, major) && same_axis(points[last_index].out_direction, major)) {
            last_index = point_index;
            const u32 length = contour.last - contour.first + 1;
            for (u32 turn = 0; turn < length; ++turn) {
                point_index = previous_in(contour, point_index);
                if (!same_axis(points[point_index].out_direction, major)) {
                    point_index = next_in(contour, point_index);
                    break;
                }
                if (point_index == last_index) break;
            }
        }
        last_index = point_index;
        bool on_edge = false;
        bool passed = false;
        // Twice round the contour at most.
        const usize turns = 2 * static_cast<usize>(contour.last - contour.first + 1) + 2;
        for (usize turn = 0; turn < turns; ++turn) {
            if (on_edge) {
                const HintPoint& point = points[point_index];
                reach.least_position = lesser(reach.least_position, point.u);
                reach.greatest_position = greater(reach.greatest_position, point.u);
                if (point.v < reach.least) {
                    reach.least = point.v;
                    reach.least_on = point.tag == PointTag::on;
                }
                if (point.v > reach.greatest) {
                    reach.greatest = point.v;
                    reach.greatest_on = point.tag == PointTag::on;
                }
                if (point.tag == PointTag::on) {
                    reach.least_on_curve = lesser(reach.least_on_curve, point.v);
                    reach.greatest_on_curve = greater(reach.greatest_on_curve, point.v);
                }
                if (point.out_direction != segment_direction || point_index == last_index) {
                    // The segment ends here. If the one before it ended
                    // where this one began, the two are one stem's side
                    // with a bump in it, and are made one.
                    if (previous_segment != no_index && axis.segments[segment_index].first != axis.segments[previous_segment].last) {
                        previous_segment = no_index;
                    }
                    if (previous_segment != no_index) {
                        HintSegment& before = axis.segments[previous_segment];
                        if (points[before.last].in_direction == point.in_direction) {
                            // The same direction: one segment of both.
                            reach.least_position = lesser(previous_reach.least_position, reach.least_position);
                            reach.greatest_position = greater(previous_reach.greatest_position, reach.greatest_position);
                            if (previous_reach.least < reach.least) {
                                reach.least = previous_reach.least;
                                reach.least_on = previous_reach.least_on;
                            }
                            if (previous_reach.greatest > reach.greatest) {
                                reach.greatest = previous_reach.greatest;
                                reach.greatest_on = previous_reach.greatest_on;
                            }
                            reach.least_on_curve = lesser(previous_reach.least_on_curve, reach.least_on_curve);
                            reach.greatest_on_curve = greater(previous_reach.greatest_on_curve, reach.greatest_on_curve);
                            before.last = point_index;
                            apply_reach(reach, before, flat_threshold);
                        } else {
                            // Other directions: the longer of the two
                            // stays, widened by the other.
                            const i32 before_span = previous_reach.greatest - previous_reach.least;
                            const i32 this_span = reach.greatest - reach.least;
                            if ((before_span < 0 ? -before_span : before_span) > (this_span < 0 ? -this_span : this_span)) {
                                previous_reach.least_position = lesser(previous_reach.least_position, reach.least_position);
                                previous_reach.greatest_position = greater(previous_reach.greatest_position, reach.greatest_position);
                                before.last = point_index;
                                before.position = (previous_reach.least_position + previous_reach.greatest_position) >> 1;
                                before.delta = (previous_reach.greatest_position - previous_reach.least_position) >> 1;
                            } else {
                                reach.least_position = lesser(reach.least_position, previous_reach.least_position);
                                reach.greatest_position = greater(reach.greatest_position, previous_reach.greatest_position);
                                HintSegment segment = axis.segments[segment_index];
                                segment.last = point_index;
                                apply_reach(reach, segment, flat_threshold);
                                axis.segments[previous_segment] = segment;
                                previous_reach = reach;
                            }
                        }
                        axis.segments.remove_last();
                    } else {
                        HintSegment& segment = axis.segments[segment_index];
                        segment.last = point_index;
                        apply_reach(reach, segment, flat_threshold);
                        previous_segment = segment_index;
                        previous_reach = reach;
                    }
                    on_edge = false;
                }
            }
            if (point_index == last_index) {
                if (passed) break;
                passed = true;
            }
            const HintPoint& point = points[point_index];
            if (!on_edge && (same_axis(point.out_direction, major) || single)) {
                if (axis.segments.count() > 1000) {
                    axis.segments.clear();
                    return false;
                }
                segment_index = axis.segments.count();
                segment_direction = point.out_direction;
                HintSegment segment{};
                segment.direction = segment_direction;
                segment.first = point_index;
                segment.last = point_index;
                segment.score = highest_score;
                const bool on = point.tag == PointTag::on;
                reach.least_position = point.u;
                reach.greatest_position = point.u;
                reach.least = point.v;
                reach.greatest = point.v;
                reach.least_on = on;
                reach.greatest_on = on;
                reach.least_on_curve = on ? point.v : highest_score;
                reach.greatest_on_curve = on ? point.v : lowest_score;
                on_edge = true;
                if (single) {
                    segment.position = reach.least_position;
                    if (!on) segment.flags = segment.flags | topo_round;
                    segment.least = point.v;
                    segment.greatest = point.v;
                    segment.height = 0;
                    on_edge = false;
                }
                if (axis.segments.push_copy(segment).failed()) {
                    no_memory = true;
                    axis.segments.clear();
                    return false;
                }
            }
            point_index = next_in(contour, point_index);
        }
    }
    return true;
}

// A segment whose ends go on beyond it along the axis is counted taller by
// half of each going-on.
void adjust_heights(const HintOutline& outline, HintAxis& axis) noexcept {
    const HintPoint* const points = outline.points.view().data();
    for (usize index = 0; index < axis.segments.count(); ++index) {
        HintSegment& segment = axis.segments[index];
        const HintPoint& first = points[segment.first];
        const HintPoint& last = points[segment.last];
        const HintPoint& before = points[first.previous];
        const HintPoint& after = points[last.next];
        if (first.v < last.v) {
            if (before.v < first.v) segment.height = segment.height + ((first.v - before.v) >> 1);
            if (after.v > last.v) segment.height = segment.height + ((after.v - last.v) >> 1);
        } else {
            if (before.v > first.v) segment.height = segment.height + ((before.v - first.v) >> 1);
            if (after.v < last.v) segment.height = segment.height + ((last.v - after.v) >> 1);
        }
    }
}

void link_default(const HintOutline& outline, HintAxis& axis, const i32 widest) noexcept {
    const i32 length_threshold = greater(for_em(outline.units_per_em, 8), 1);
    const i32 length_score = for_em(outline.units_per_em, 6000);
    const i32 distance_score = 3000;
    const usize count = axis.segments.count();
    HintSegment* const segments = axis.segments.mutable_view().data();
    for (usize first = 0; first < count; ++first) {
        if (segments[first].direction != axis.major_direction) continue;
        const i32 position = segments[first].position;
        for (usize second = 0; second < count; ++second) {
            const HintSegment one = segments[first];
            const HintSegment other = segments[second];
            if (!opposite(one.direction, other.direction) || other.position <= position) continue;
            const i32 length = lesser(one.greatest, other.greatest) - greater(one.least, other.least);
            if (length < length_threshold) continue;
            const i32 distance = other.position - position;
            i32 demerit = distance;
            if (widest != 0) {
                // A distance beyond the widest standard width counts
                // against the pair, by its square.
                const i32 over = (distance << 10) / widest - (1 << 10);
                demerit = over > 10000 ? highest_score : (over > 0 ? over * over / distance_score : 0);
            }
            const i32 score = demerit + length_score / length;
            if (score < one.score) {
                segments[first].score = score;
                segments[first].link = static_cast<u32>(second);
            }
            if (score < other.score) {
                segments[second].score = score;
                segments[second].link = static_cast<u32>(first);
            }
        }
    }
    // A link that is not returned is a serif.
    for (usize index = 0; index < count; ++index) {
        const u32 link = segments[index].link;
        if (link == no_index) continue;
        const u32 back = segments[link].link;
        if (back != index) {
            segments[index].link = no_index;
            segments[index].serif = back;
        }
    }
}

void link_cjk(const HintOutline& outline, HintAxis& axis, const i32 scale) noexcept {
    const i32 length_threshold = for_em(outline.units_per_em, 8);
    // Three pixels, in font units.
    const i64 three = static_cast<i64>(64 * 3) * 65536;
    const i32 distance_threshold = scale > 0 ? static_cast<i32>((three + scale / 2) / scale) : 0x7FFFFFFF;
    const usize count = axis.segments.count();
    HintSegment* const segments = axis.segments.mutable_view().data();
    for (usize first = 0; first < count; ++first) {
        if (segments[first].direction != axis.major_direction) continue;
        const i32 position = segments[first].position;
        for (usize second = 0; second < count; ++second) {
            const HintSegment one = segments[first];
            const HintSegment other = segments[second];
            if (first == second || !opposite(one.direction, other.direction)) continue;
            const i32 distance = other.position - position;
            if (distance < 0) continue;
            const i32 length = lesser(one.greatest, other.greatest) - greater(one.least, other.least);
            if (length < length_threshold) continue;
            if ((distance * 8 < one.score * 9) && (distance * 8 < one.score * 7 || one.length < length)) {
                segments[first].score = distance;
                segments[first].length = length;
                segments[first].link = static_cast<u32>(second);
            }
            if ((distance * 8 < other.score * 9) && (distance * 8 < other.score * 7 || other.length < length)) {
                segments[second].score = distance;
                segments[second].length = length;
                segments[second].link = static_cast<u32>(first);
            }
        }
    }
    // Stems that overlap: the longer one keeps its pair, or neither does.
    for (usize first = 0; first < count; ++first) {
        const HintSegment one = segments[first];
        if (one.score >= distance_threshold || one.link == no_index) continue;
        const u32 one_link = one.link;
        const HintSegment across = segments[one_link];
        if (across.link != first || across.position <= one.position) continue;
        for (usize second = 0; second < count; ++second) {
            const HintSegment other = segments[second];
            if (other.position > one.position || first == second || other.link == no_index) continue;
            const HintSegment other_across = segments[other.link];
            if (other_across.link != second || other_across.position < across.position) continue;
            if (one.position == other.position && across.position == other_across.position) continue;
            if (other.score <= one.score || one.score * 4 <= other.score) continue;
            if (one.length >= other.length * 3) {
                const u32 other_link = other.link;
                for (usize index = 0; index < count; ++index) {
                    const u32 link = segments[index].link;
                    if (link == second) {
                        segments[index].link = no_index;
                        segments[index].serif = one_link;
                    } else if (link == other_link) {
                        segments[index].link = no_index;
                        segments[index].serif = static_cast<u32>(first);
                    }
                }
            } else {
                segments[first].link = no_index;
                segments[one_link].link = no_index;
                break;
            }
        }
    }
    for (usize index = 0; index < count; ++index) {
        const HintSegment one = segments[index];
        if (one.link == no_index) continue;
        const HintSegment other = segments[one.link];
        if (other.link != index) {
            segments[index].link = no_index;
            if (other.score < distance_threshold || one.score < other.score * 4) segments[index].serif = other.link;
        }
    }
}

} // namespace

void reset_axis(HintAxis& axis, const u32 dimension, const Winding winding) noexcept {
    axis.dimension = dimension;
    if (winding == Winding::clockwise) {
        axis.major_direction = dimension == axis_horizontal ? Direction::down : Direction::right;
    } else {
        axis.major_direction = dimension == axis_horizontal ? Direction::up : Direction::left;
    }
    axis.segments.clear();
    axis.edges.clear();
}

bool compute_segments(HintOutline& outline, HintAxis& axis, bool& no_memory) noexcept {
    assign_positions(outline, axis.dimension);
    if (!build_segments(outline, axis, no_memory)) return false;
    adjust_heights(outline, axis);
    return true;
}

void link_segments(const HintOutline& outline, HintAxis& axis, const i32 scale, const bool cjk, const i32 widest) noexcept {
    if (cjk) {
        link_cjk(outline, axis, scale);
    } else {
        link_default(outline, axis, widest);
    }
}

} // namespace stxlib::glyphs::detail
