// stxlib::glyphs: the auto-hinter's edges (hinter.hpp). After Skrifa
// 0.50.0, outline/autohint/topo/edges.rs and topo/mod.rs (themselves after
// FreeType's aflatin.c and afcjk.c): segments at one position gathered
// into an edge, the edge across the stem from each, and the blue zone
// that catches each.

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs::detail {

namespace {

[[nodiscard]] constexpr i32 size_of(const i32 value) noexcept {
    const i32 result = value < 0 ? -value : value;
    return result;
}

// A new edge, put where its position has it among the others.
[[nodiscard]] bool insert_edge(HintAxis& axis, const HintEdge& edge, const bool top_to_bottom) noexcept {
    if (axis.edges.push_copy(edge).failed()) return false;
    HintEdge* const edges = axis.edges.mutable_view().data();
    usize index = axis.edges.count() - 1;
    while (index > 0) {
        const i32 before = edges[index - 1].font_position;
        if ((top_to_bottom && before > edge.font_position) || (!top_to_bottom && before < edge.font_position)) break;
        // Edges at one position: the one along the major direction first.
        if (before == edge.font_position && edge.direction == axis.major_direction) break;
        edges[index] = edges[index - 1];
        index = index - 1;
    }
    edges[index] = edge;
    return true;
}

// The segments of an edge are a ring: the new one goes last and points
// back at the first.
void append_segment(HintAxis& axis, const usize segment, const usize edge) noexcept {
    HintEdge& target = axis.edges[edge];
    const u32 first = target.first;
    const u32 last = target.last;
    target.last = static_cast<u32>(segment);
    axis.segments[segment].edge_next = first;
    axis.segments[last].edge_next = static_cast<u32>(segment);
}

void link_segments_to_edges(HintAxis& axis) noexcept {
    HintSegment* const segments = axis.segments.mutable_view().data();
    const usize segment_count = axis.segments.count();
    for (usize edge = 0; edge < axis.edges.count(); ++edge) {
        usize index = axis.edges[edge].first;
        const usize last = axis.edges[edge].last;
        for (usize turn = 0; turn <= segment_count; ++turn) {
            segments[index].edge = static_cast<u32>(edge);
            if (index == last) break;
            index = segments[index].edge_next != no_index ? segments[index].edge_next : last;
        }
    }
}

void compute_edge_properties(HintAxis& axis) noexcept {
    HintEdge* const edges = axis.edges.mutable_view().data();
    const HintSegment* const segments = axis.segments.view().data();
    const usize segment_count = axis.segments.count();
    for (usize edge_index = 0; edge_index < axis.edges.count(); ++edge_index) {
        i32 roundness = 0;
        i32 straightness = 0;
        usize segment_index = edges[edge_index].first;
        const usize last_segment = edges[edge_index].last;
        for (usize turn = 0; turn <= segment_count; ++turn) {
            const HintEdge edge = edges[edge_index];
            const HintSegment& segment = segments[segment_index];
            const u32 next_segment = segment.edge_next;
            if ((segment.flags & topo_round) != 0) {
                roundness = roundness + 1;
            } else {
                straightness = straightness + 1;
            }
            // A serif, if its other segment is in another edge.
            const bool is_serif = segment.serif != no_index && segments[segment.serif].edge != no_index && segments[segment.serif].edge != edge_index;
            if (is_serif || (segment.link != no_index && segments[segment.link].edge != no_index)) {
                const u32 other_edge = is_serif ? edge.serif : edge.link;
                const u32 other_segment = is_serif ? segment.serif : segment.link;
                u32 chosen = other_edge;
                if (other_edge != no_index && other_segment != no_index) {
                    // The nearer of the edge already taken and this
                    // segment's own.
                    const i32 edge_delta = size_of(edge.font_position - edges[other_edge].font_position);
                    const i32 segment_delta = size_of(segment.position - segments[other_segment].position);
                    if (segment_delta < edge_delta) chosen = segments[other_segment].edge;
                } else if (other_segment != no_index) {
                    chosen = segments[other_segment].edge;
                }
                if (is_serif) {
                    edges[edge_index].serif = chosen;
                    if (chosen != no_index) edges[chosen].flags = edges[chosen].flags | topo_serif;
                } else {
                    edges[edge_index].link = chosen;
                }
            }
            if (segment_index == last_segment) break;
            segment_index = next_segment != no_index ? next_segment : last_segment;
        }
        HintEdge& edge = edges[edge_index];
        edge.flags = 0;
        if (roundness > 0 && roundness >= straightness) edge.flags = edge.flags | topo_round;
        // An edge is not both a stem's side and a serif.
        if (edge.serif != no_index && edge.link != no_index) edge.serif = no_index;
    }
}

} // namespace

bool compute_edges(HintAxis& axis, const ScaledAxisMetrics& metrics, const bool top_to_bottom, const i32 y_scale, const bool cjk) noexcept {
    axis.edges.clear();
    const i32 scale = metrics.scale;
    const bool downward = (axis.dimension == axis_horizontal || cjk) ? false : top_to_bottom;
    // Segments shorter than a pixel, or wider than half of one, make no
    // edge of the default scripts.
    const i32 length_threshold = axis.dimension == axis_horizontal ? divide_fixed(64, y_scale) : 0;
    const i32 width_threshold = divide_fixed(32, scale);
    const i32 initial = metrics.width_metrics.edge_distance_threshold;
    const i32 most = 64 / 4;
    i32 distance_threshold = 0;
    if (!cjk) {
        const i32 scaled = times_fixed(initial, scale);
        distance_threshold = divide_fixed(scaled < most ? scaled : most, scale);
    } else {
        distance_threshold = times_fixed(initial, scale) > most ? divide_fixed(most, scale) : initial;
    }
    const usize segment_count = axis.segments.count();
    for (usize segment_index = 0; segment_index < segment_count; ++segment_index) {
        const HintSegment segment = axis.segments[segment_index];
        if (!cjk) {
            if (segment.height < length_threshold || segment.delta > width_threshold || segment.direction == Direction::none) continue;
            // A serif must be half again as long.
            if (segment.serif != no_index && 2 * segment.height < 3 * length_threshold) continue;
        }
        i32 best_distance = 0x7FFFFFFF;
        usize best_edge = no_index;
        for (usize edge_index = 0; edge_index < axis.edges.count(); ++edge_index) {
            const HintEdge& edge = axis.edges[edge_index];
            const i32 distance = size_of(segment.position - edge.font_position);
            if (distance >= distance_threshold || edge.direction != segment.direction || distance >= best_distance) continue;
            if (!cjk) {
                best_edge = edge_index;
                break;
            }
            if (segment.link != no_index) {
                // CJK: the stems of the edge's segments must be as thick
                // as this one's.
                const HintSegment link = axis.segments[segment.link];
                const usize first = edge.first;
                usize other = first;
                i32 second_distance = 0;
                for (usize turn = 0; turn <= segment_count; ++turn) {
                    const HintSegment& of_edge = axis.segments[other];
                    if (of_edge.link != no_index) {
                        second_distance = size_of(link.position - axis.segments[of_edge.link].position);
                        if (second_distance >= distance_threshold) break;
                    }
                    if (of_edge.edge_next == first || of_edge.edge_next == no_index) break;
                    other = of_edge.edge_next;
                }
                if (second_distance >= distance_threshold) continue;
            }
            best_distance = distance;
            best_edge = edge_index;
        }
        if (best_edge != no_index) {
            append_segment(axis, segment_index, best_edge);
        } else {
            HintEdge edge{};
            edge.font_position = segment.position;
            edge.original = times_fixed(segment.position, scale);
            edge.position = edge.original;
            edge.direction = segment.direction;
            edge.first = static_cast<u32>(segment_index);
            edge.last = static_cast<u32>(segment_index);
            if (!insert_edge(axis, edge, downward)) return false;
            axis.segments[segment_index].edge_next = static_cast<u32>(segment_index);
        }
    }
    if (!cjk) {
        // Segments with no direction (single points) join the first edge
        // near enough.
        for (usize segment_index = 0; segment_index < segment_count; ++segment_index) {
            const HintSegment segment = axis.segments[segment_index];
            if (segment.direction != Direction::none) continue;
            for (usize edge_index = 0; edge_index < axis.edges.count(); ++edge_index) {
                if (size_of(segment.position - axis.edges[edge_index].font_position) < distance_threshold) {
                    append_segment(axis, segment_index, edge_index);
                    break;
                }
            }
        }
    }
    link_segments_to_edges(axis);
    compute_edge_properties(axis);
    return true;
}

void compute_blue_edges(HintAxis& axis, const HintScale& scale, const View<UnscaledBlue> unscaled, const View<ScaledBlue> blues, const bool cjk) noexcept {
    if (axis.dimension != axis_vertical && !cjk) return;
    const i32 axis_scale = axis.dimension == axis_horizontal ? scale.x_scale : scale.y_scale;
    // No further than a fortieth of the em, and no further than half a
    // pixel.
    const i32 scaled_limit = times_fixed(scale.units_per_em / 40, axis_scale);
    const i32 farthest = scaled_limit < 64 / 2 ? scaled_limit : 64 / 2;
    const usize blue_count = unscaled.count() < blues.count() ? unscaled.count() : blues.count();
    for (usize edge_index = 0; edge_index < axis.edges.count(); ++edge_index) {
        HintEdge& edge = axis.edges[edge_index];
        bool found = false;
        bool neutral_found = false;
        ScaledWidth best{};
        i32 best_distance = farthest;
        for (usize blue_index = 0; blue_index < blue_count; ++blue_index) {
            const UnscaledBlue& in_units = unscaled[blue_index];
            const ScaledBlue& blue = blues[blue_index];
            if (!blue.active) continue;
            const bool is_top = (blue.zones & (blue_top | blue_sub_top)) != 0;
            const bool is_neutral = (blue.zones & blue_neutral) != 0;
            const bool is_major = edge.direction == axis.major_direction;
            // A top zone catches edges against the major direction, a
            // bottom zone those along it; a neutral zone both.
            if (!((is_top != is_major) || is_neutral)) continue;
            i32 reference = in_units.position;
            ScaledWidth matching = blue.position;
            if (cjk && size_of(edge.font_position - in_units.position) > size_of(edge.font_position - in_units.overshoot)) {
                reference = in_units.overshoot;
                matching = blue.overshoot;
            }
            const i32 distance = times_fixed(size_of(edge.font_position - reference), axis_scale);
            if (distance < best_distance) {
                best_distance = distance;
                best = matching;
                found = true;
                neutral_found = is_neutral;
            }
            if (!cjk && (edge.flags & topo_round) != 0 && distance != 0 && !is_neutral) {
                // A round edge beyond the zone's flat position may sit on
                // its overshoot.
                const bool under = edge.font_position < in_units.position;
                if (is_top != under) {
                    const i32 beyond = times_fixed(size_of(edge.font_position - in_units.overshoot), axis_scale);
                    if (beyond < best_distance) {
                        best_distance = beyond;
                        best = blue.overshoot;
                        found = true;
                        neutral_found = is_neutral;
                    }
                }
            }
        }
        if (found) {
            edge.has_blue = true;
            edge.blue = best;
            if (neutral_found) edge.flags = edge.flags | topo_neutral;
        }
    }
}

} // namespace stxlib::glyphs::detail
