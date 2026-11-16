// stxlib::glyphs: the auto-hinter's fitting of edges (hinter.hpp). After
// Skrifa 0.50.0, outline/autohint/hint/edges.rs (itself after FreeType's
// aflatin.c and afcjk.c): each edge's position at the size.

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs::detail {

namespace {

[[nodiscard]] constexpr i32 size_of(const i32 value) noexcept {
    const i32 result = value < 0 ? -value : value;
    return result;
}

// a * b / c, rounded to the nearest, halves away from zero.
[[nodiscard]] i32 times_over(const i64 a, const i64 b, const i64 c) noexcept {
    const i64 a_size = a < 0 ? -a : a;
    const i64 b_size = b < 0 ? -b : b;
    const i64 c_size = c < 0 ? -c : c;
    const i64 quotient = c_size > 0 ? (a_size * b_size + c_size / 2) / c_size : 0x7FFFFFFF;
    const bool negative = ((a < 0) != (b < 0)) != (c < 0);
    return static_cast<i32>(negative ? -quotient : quotient);
}

// The standard width nearest a width, if it is near enough to stand for
// it.
[[nodiscard]] i32 snap_width(const Vector<ScaledWidth>& widths, const i32 width) noexcept {
    i32 best_distance = 64 + 32 + 2;
    i32 reference = width;
    for (usize index = 0; index < widths.count(); ++index) {
        const i32 distance = size_of(width - widths[index].scaled);
        if (distance < best_distance) {
            best_distance = distance;
            reference = widths[index].scaled;
        }
    }
    const i32 scaled = pixel_round(reference);
    if (width >= reference) {
        const i32 result = width < scaled + 48 ? reference : width;
        return result;
    }
    const i32 result = width > scaled - 48 ? reference : width;
    return result;
}

// The width a stem is given at the size.
[[nodiscard]] i32 stem_width(const ScaledAxisMetrics& metrics, const bool cjk, const HintScale& scale, const i32 width, const i32 base_delta,
                             const u32 base_flags, const u32 stem_flags) noexcept {
    // Stems are left as they are unless the mode asks, and for a very
    // light face.
    if ((scale.flags & scale_stem_adjust) == 0 || (!cjk && metrics.width_metrics.extra_light)) return width;
    const bool vertical = metrics.dimension == axis_vertical;
    const i32 sign = width < 0 ? -1 : 1;
    i32 distance = size_of(width);
    const bool snapped = vertical ? (scale.flags & scale_vertical_snap) != 0 : (scale.flags & scale_horizontal_snap) != 0;
    if (!snapped) {
        if (!cjk) {
            if ((stem_flags & topo_serif) != 0 && vertical && distance < 3 * 64) return distance * sign;
            if ((base_flags & topo_round) != 0) {
                if (distance < 80) distance = 64;
            } else if (distance < 56) {
                distance = 56;
            }
        }
        if (!metrics.widths.empty()) {
            const i32 least = metrics.widths[0].scaled;
            if (size_of(distance - least) < 40) {
                distance = least > 48 ? least : 48;
                return distance * sign;
            }
            if (!cjk) {
                if (distance < 3 * 64) {
                    const i32 part = distance & 63;
                    distance = distance & -64;
                    if (part < 10) {
                        distance = distance + part;
                    } else if (part < 32) {
                        distance = distance + 10;
                    } else if (part < 54) {
                        distance = distance + 54;
                    } else {
                        distance = distance + part;
                    }
                } else {
                    i32 allowed = 0;
                    if ((width > 0 && base_delta > 0) || (width < 0 && base_delta < 0)) {
                        if (scale.pixels < 10) {
                            allowed = base_delta;
                        } else if (scale.pixels < 30) {
                            allowed = (base_delta * (30 - scale.pixels)) / 20;
                        }
                    }
                    distance = (distance - size_of(allowed) + 32) & ~63;
                }
            }
        }
        if (cjk) {
            if (distance < 54) {
                distance = distance + (54 - distance) / 2;
            } else if (distance < 3 * 64) {
                const i32 part = distance & 63;
                distance = distance & -64;
                if (part < 10) {
                    distance = distance + part;
                } else if (part < 22) {
                    distance = distance + 10;
                } else if (part < 42) {
                    distance = distance + part;
                } else if (part < 54) {
                    distance = distance + 54;
                } else {
                    distance = distance + part;
                }
            }
        }
    } else {
        const i32 original = distance;
        distance = snap_width(metrics.widths, distance);
        if (vertical) {
            distance = distance >= 64 ? (distance + 16) & ~63 : 64;
        } else if ((scale.flags & scale_mono) != 0) {
            distance = distance < 64 ? 64 : (distance + 32) & ~63;
        } else if (distance < 48) {
            distance = (distance + 64) >> 1;
        } else if (distance < 128) {
            distance = (distance + 22) & ~63;
            if (!cjk && size_of(distance - original) >= 16) {
                distance = original;
                if (distance < 48) distance = (distance + 64) >> 1;
            }
        } else {
            distance = (distance + 32) & ~63;
        }
    }
    return distance * sign;
}

// A stem's other side, at the fitted width from the side that is placed.
void align_linked_edge(HintAxis& axis, const ScaledAxisMetrics& metrics, const bool cjk, const HintScale& scale, const usize base,
                       const usize stem) noexcept {
    HintEdge* const edges = axis.edges.mutable_view().data();
    const i32 width = edges[stem].original - edges[base].original;
    const i32 base_delta = edges[base].position - edges[base].original;
    const i32 fitted = stem_width(metrics, cjk, scale, width, base_delta, edges[base].flags, edges[stem].flags);
    edges[stem].position = edges[base].position + fitted;
}

void align_serif_edge(HintAxis& axis, const usize base, const usize serif) noexcept {
    HintEdge* const edges = axis.edges.mutable_view().data();
    edges[serif].position = edges[base].position + (edges[serif].original - edges[base].original);
}

// Edges caught by blue zones go where the zones are. The first edge so
// placed is the anchor, or none.
[[nodiscard]] usize align_edges_to_blues(HintAxis& axis, const ScaledAxisMetrics& metrics, const bool cjk, const HintScale& scale) noexcept {
    usize anchor = no_index;
    if (!cjk && axis.dimension != axis_vertical) return anchor;
    const usize count = axis.edges.count();
    for (usize index = 0; index < count; ++index) {
        HintEdge* const edges = axis.edges.mutable_view().data();
        if ((edges[index].flags & topo_done) != 0) continue;
        const usize linked = edges[index].link;
        // Both sides of a stem in zones: a neutral zone gives way.
        if (edges[index].has_blue && linked != no_index && edges[linked].has_blue) {
            usize skip = no_index;
            if ((edges[linked].flags & topo_neutral) != 0) {
                skip = linked;
            } else if ((edges[index].flags & topo_neutral) != 0) {
                skip = index;
            }
            if (skip != no_index) {
                edges[skip].has_blue = false;
                edges[skip].flags = edges[skip].flags & ~topo_neutral;
            }
        }
        usize first = no_index;
        usize second = no_index;
        ScaledWidth blue{};
        if (edges[index].has_blue) {
            blue = edges[index].blue;
            first = index;
            second = linked;
        } else if (linked != no_index && edges[linked].has_blue) {
            blue = edges[linked].blue;
            first = linked;
            second = index;
        } else {
            continue;
        }
        edges[first].position = blue.fitted;
        edges[first].flags = edges[first].flags | topo_done;
        if (second != no_index && !edges[second].has_blue) {
            edges[second].flags = edges[second].flags | topo_done;
            align_linked_edge(axis, metrics, cjk, scale, first, second);
        }
        if (anchor == no_index) anchor = index;
    }
    return anchor;
}

// An edge that hinting has put on the wrong side of its neighbour, and
// whose stem is not right beside that neighbour, goes where the
// neighbour is.
void adjust_link(HintAxis& axis, const usize index, const bool next, const bool top_to_bottom) noexcept {
    HintEdge* const edges = axis.edges.mutable_view().data();
    const usize count = axis.edges.count();
    usize other = no_index;
    usize before = no_index;
    if (next) {
        if (index + 1 >= count || (edges[index + 1].flags & topo_done) == 0 || index == 0) return;
        other = index + 1;
        before = index - 1;
    } else {
        if (index == 0) return;
        other = index - 1;
        before = index - 1;
    }
    const i32 first = edges[index].position;
    const i32 second = edges[other].position;
    // Looking back, an edge should be past the one before it; looking
    // forward, short of the one after: the other way round when hinting
    // runs from the top down.
    const bool out_of_order = (next != top_to_bottom) ? first > second : first < second;
    if (!out_of_order) return;
    const usize linked = edges[index].link;
    if (linked == no_index) return;
    if (size_of(edges[linked].position - edges[before].position) > 16) edges[index].position = edges[other].position;
}

// A CJK stem: its two sides at the fitted width, moved a little so that a
// side is on a pixel edge. The move is returned.
[[nodiscard]] i32 hint_cjk_stem(HintAxis& axis, const ScaledAxisMetrics& metrics, const HintScale& scale, const usize first_index,
                                const usize second_index, const i32 anchor) noexcept {
    HintEdge* const edges = axis.edges.mutable_view().data();
    const HintEdge first = edges[first_index];
    const HintEdge second = edges[second_index];
    const bool adjust = (scale.flags & scale_stem_adjust) != 0;
    i32 threshold_delta = 0;
    if (!adjust) {
        const i32 gap = axis.dimension == axis_vertical ? 9 : 15;
        threshold_delta = ((first.flags & topo_round) != 0 && (second.flags & topo_round) != 0) ? gap : gap / 3;
    }
    const i32 threshold = 64 - threshold_delta;
    const i32 original_length = second.original - first.original;
    const i32 length = stem_width(metrics, true, scale, original_length, 0, first.flags, second.flags);
    const i32 centre = (first.original + second.original) / 2 + anchor;
    const i32 low = centre - length / 2;
    const i32 high = low + length;
    i32 below_low = low - pixel_floor(low);
    i32 below_high = high - pixel_floor(high);
    i32 delta = 0;
    bool decided = below_low == 0 || below_high == 0;
    if (!decided) {
        i32 above_low = 64 - below_low;
        i32 above_high = 64 - below_high;
        if (length <= threshold) {
            if (below_high < length) delta = above_low <= below_high ? above_low : -below_high;
            decided = true;
        } else if (threshold < 64 && (below_low >= threshold || above_low >= threshold || below_high >= threshold || above_high >= threshold)) {
            decided = true;
        } else {
            i32 offset = length & 63;
            if (offset < 32) {
                if (above_low <= offset || below_high <= offset) decided = true;
            } else {
                offset = 64 - threshold;
            }
            if (!decided) {
                below_low = threshold - above_low;
                above_low = above_low - offset;
                above_high = threshold - below_high;
                below_high = below_high - offset;
                if (below_low <= above_low) above_low = -below_low;
                if (below_high <= above_high) above_high = -below_high;
                delta = size_of(above_low) <= size_of(above_high) ? above_low : above_high;
            }
        }
    }
    if (!adjust) delta = delta < -14 ? -14 : (delta > 14 ? 14 : delta);
    const i32 placed = low + delta;
    if (first.original < second.original) {
        edges[first_index].position = placed;
        edges[second_index].position = placed + length;
    } else {
        edges[second_index].position = placed;
        edges[first_index].position = placed + length;
    }
    return delta;
}

struct Stems final {
    usize serifs{0};
    usize anchor{no_index};
};

// Stems: an edge and the edge across from it, placed together.
[[nodiscard]] Stems align_stem_edges(HintAxis& axis, const ScaledAxisMetrics& metrics, const bool cjk, const HintScale& scale,
                                     const bool top_to_bottom, const usize first_anchor) noexcept {
    Stems result{.serifs = 0, .anchor = first_anchor};
    bool have_last_stem = false;
    i32 last_stem = 0;
    i32 delta = 0;
    const usize count = axis.edges.count();
    for (usize index = 0; index < count; ++index) {
        HintEdge* const edges = axis.edges.mutable_view().data();
        const HintEdge edge = edges[index];
        if ((edge.flags & topo_done) != 0) continue;
        const usize linked = edge.link;
        if (linked == no_index) {
            result.serifs = result.serifs + 1;
            continue;
        }
        if (cjk && have_last_stem && (edge.position < last_stem + 64 || edges[linked].position < last_stem + 64)) {
            // Too near the last stem: placed with the rest.
            result.serifs = result.serifs + 1;
            continue;
        }
        if (edges[linked].has_blue) {
            // The other side is on a zone: this side follows it.
            align_linked_edge(axis, metrics, cjk, scale, linked, index);
            edges[index].flags = edges[index].flags | topo_done;
            continue;
        }
        if (!cjk) {
            if (result.anchor != no_index) {
                const HintEdge anchor = edges[result.anchor];
                const HintEdge other = edges[linked];
                const i32 original_position = anchor.position + (edge.original - anchor.original);
                const i32 original_length = other.original - edge.original;
                const i32 original_centre = original_position + (original_length >> 1);
                const i32 length = stem_width(metrics, cjk, scale, original_length, 0, edge.flags, other.flags);
                if ((other.flags & topo_done) != 0) {
                    edges[index].position = other.position - length;
                } else if (length < 96) {
                    // A thin stem is centred on a pixel or on a pixel edge,
                    // whichever is nearer.
                    const i32 rounded = pixel_round(original_centre);
                    const i32 up = length <= 64 ? 32 : 38;
                    const i32 down = length <= 64 ? 32 : 26;
                    const i32 first_error = size_of(original_centre - (rounded - up));
                    const i32 second_error = size_of(original_centre - (rounded + down));
                    const i32 centre = first_error < second_error ? rounded - up : rounded + down;
                    edges[index].position = centre - length / 2;
                    edges[linked].position = centre + length / 2;
                } else {
                    // A thick stem has one side on a pixel edge: the side
                    // that leaves its centre nearest where it was.
                    const i32 first_position = pixel_round(original_position);
                    const i32 first_error = size_of(first_position + (length >> 1) - original_centre);
                    const i32 second_position = pixel_round(original_position + original_length) - length;
                    const i32 second_error = size_of(second_position + (length >> 1) - original_centre);
                    const i32 position = first_error < second_error ? first_position : second_position;
                    edges[index].position = position;
                    edges[linked].position = position + length;
                }
                edges[index].flags = edges[index].flags | topo_done;
                edges[linked].flags = edges[linked].flags | topo_done;
                if (index > 0) adjust_link(axis, index, false, top_to_bottom);
            } else {
                // The first stem: it becomes the anchor.
                const HintEdge other = edges[linked];
                const i32 original_length = other.original - edge.original;
                const i32 length = stem_width(metrics, cjk, scale, original_length, 0, edge.flags, other.flags);
                if (length < 96) {
                    const i32 up = length <= 64 ? 32 : 38;
                    const i32 down = length <= 64 ? 32 : 26;
                    const i32 original_centre = edge.original + (original_length >> 1);
                    i32 centre = pixel_round(original_centre);
                    const i32 first_error = size_of(original_centre - (centre - up));
                    const i32 second_error = size_of(original_centre - (centre + down));
                    centre = first_error < second_error ? centre - up : centre + down;
                    edges[index].position = centre - length / 2;
                    edges[linked].position = edges[index].position + length;
                } else {
                    edges[index].position = pixel_round(edge.original);
                }
                edges[index].flags = edges[index].flags | topo_done;
                align_linked_edge(axis, metrics, cjk, scale, index, linked);
                result.anchor = index;
            }
        } else {
            if (linked < index) {
                have_last_stem = true;
                last_stem = edge.position;
                edges[index].flags = edges[index].flags | topo_done;
                align_linked_edge(axis, metrics, cjk, scale, linked, index);
                continue;
            }
            // Along x the first stem's move is given to those after it.
            if (axis.dimension != axis_vertical && result.anchor == no_index) {
                delta = hint_cjk_stem(axis, metrics, scale, index, linked, delta);
            } else {
                static_cast<void>(hint_cjk_stem(axis, metrics, scale, index, linked, delta));
            }
            result.anchor = index;
            edges[index].flags = edges[index].flags | topo_done;
            edges[linked].flags = edges[linked].flags | topo_done;
            have_last_stem = true;
            last_stem = edges[linked].position;
        }
    }
    return result;
}

// The three stems of an m are kept evenly apart.
void hint_lowercase_m(HintAxis& axis, const bool cjk) noexcept {
    HintEdge* const edges = axis.edges.mutable_view().data();
    const usize count = axis.edges.count();
    const usize first = count == 6 ? 0 : 1;
    const usize second = count == 6 ? 2 : 5;
    const usize third = count == 6 ? 4 : 9;
    const i32 first_gap = edges[second].original - edges[first].original;
    const i32 second_gap = edges[third].original - edges[second].original;
    if (cjk) {
        if (edges[first].link != first + 1 || edges[second].link != second + 1 || edges[third].link != third + 1) return;
    }
    if (size_of(first_gap - second_gap) >= 8) return;
    const i32 delta = edges[third].position - (2 * edges[second].position - edges[first].position);
    const usize linked = edges[third].link;
    edges[third].position = edges[third].position - delta;
    edges[third].flags = edges[third].flags | topo_done;
    if (linked != no_index) {
        edges[linked].position = edges[linked].position - delta;
        edges[linked].flags = edges[linked].flags | topo_done;
    }
    if (count == 12) {
        edges[8].position = edges[8].position - delta;
        edges[11].position = edges[11].position - delta;
    }
}

// The nearest placed edge before an edge, and the nearest after.
struct Bounds2 final {
    usize before{no_index};
    usize after{no_index};
};

[[nodiscard]] Bounds2 placed_neighbours(const HintAxis& axis, const usize index) noexcept {
    Bounds2 result{};
    const usize count = axis.edges.count();
    for (usize at = index; at > 0; --at) {
        if ((axis.edges[at - 1].flags & topo_done) != 0) {
            result.before = at - 1;
            break;
        }
    }
    for (usize at = index + 1; at < count; ++at) {
        if ((axis.edges[at].flags & topo_done) != 0) {
            result.after = at;
            break;
        }
    }
    return result;
}

void align_remaining_edges(HintAxis& axis, const bool cjk, const bool top_to_bottom, const usize serif_count, const usize first_anchor) noexcept {
    const usize count = axis.edges.count();
    usize anchor = first_anchor;
    if (!cjk) {
        for (usize index = 0; index < count; ++index) {
            HintEdge* const edges = axis.edges.mutable_view().data();
            const HintEdge edge = edges[index];
            if ((edge.flags & topo_done) != 0) continue;
            const i32 serif_distance = edge.serif != no_index ? size_of(edges[edge.serif].original - edge.original) : 1000;
            if (serif_distance < 64 + 16) {
                // A serif near its stem keeps its distance from it.
                align_serif_edge(axis, edge.serif, index);
            } else if (anchor != no_index) {
                const Bounds2 placed = placed_neighbours(axis, index);
                if (placed.before != no_index && placed.after != no_index) {
                    // Between two placed edges: in proportion.
                    const HintEdge before = edges[placed.before];
                    const HintEdge after = edges[placed.after];
                    edges[index].position = after.original == before.original
                                                ? before.position
                                                : before.position + times_over(static_cast<i64>(edge.original) - before.original,
                                                                               static_cast<i64>(after.position) - before.position,
                                                                               static_cast<i64>(after.original) - before.original);
                } else {
                    // From the anchor, to the nearest half pixel.
                    edges[index].position = edges[anchor].position + ((edge.original - edges[anchor].original + 16) & ~31);
                }
            } else {
                anchor = index;
                edges[index].position = pixel_round(edge.original);
            }
            edges[index].flags = edges[index].flags | topo_done;
            adjust_link(axis, index, false, top_to_bottom);
            adjust_link(axis, index, true, top_to_bottom);
        }
        return;
    }
    usize serifs = serif_count;
    for (usize index = 0; index < count; ++index) {
        HintEdge* const edges = axis.edges.mutable_view().data();
        if ((edges[index].flags & topo_done) != 0) continue;
        if (edges[index].serif != no_index) {
            edges[index].flags = edges[index].flags | topo_done;
            align_serif_edge(axis, edges[index].serif, index);
            serifs = serifs > 0 ? serifs - 1 : 0;
        }
    }
    if (serifs == 0) return;
    for (usize index = 0; index < count; ++index) {
        HintEdge* const edges = axis.edges.mutable_view().data();
        const HintEdge edge = edges[index];
        if ((edge.flags & topo_done) != 0) continue;
        const Bounds2 placed = placed_neighbours(axis, index);
        if (placed.before != no_index && placed.after == no_index) {
            align_serif_edge(axis, placed.before, index);
        } else if (placed.before == no_index && placed.after != no_index) {
            align_serif_edge(axis, placed.after, index);
        } else if (placed.before != no_index && placed.after != no_index) {
            const HintEdge before = edges[placed.before];
            const HintEdge after = edges[placed.after];
            edges[index].position = after.font_position == before.font_position
                                        ? before.position
                                        : before.position + times_over(static_cast<i64>(edge.font_position) - before.font_position,
                                                                       static_cast<i64>(after.position) - before.position,
                                                                       static_cast<i64>(after.font_position) - before.font_position);
        }
    }
}

} // namespace

void hint_edges(HintAxis& axis, const ScaledAxisMetrics& metrics, const bool cjk, const HintScale& scale, const bool top_to_bottom) noexcept {
    const bool downward = axis.dimension == axis_vertical ? top_to_bottom : false;
    const usize blue_anchor = align_edges_to_blues(axis, metrics, cjk, scale);
    const Stems stems = align_stem_edges(axis, metrics, cjk, scale, downward, blue_anchor);
    const usize count = axis.edges.count();
    if (axis.dimension == axis_horizontal && (count == 6 || count == 12)) hint_lowercase_m(axis, cjk);
    if (stems.serifs > 0 || stems.anchor == no_index) align_remaining_edges(axis, cjk, downward, stems.serifs, stems.anchor);
}

} // namespace stxlib::glyphs::detail
