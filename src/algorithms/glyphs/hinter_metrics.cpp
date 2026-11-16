// stxlib::glyphs: the auto-hinter's measures of a font (hinter.hpp). After
// Skrifa 0.50.0, outline/autohint: style.rs (which style each glyph has),
// metrics/widths.rs (a script's standard stem widths), metrics/blues.rs
// (its blue zones), metrics/scale.rs and metrics/mod.rs (both at a size);
// themselves after FreeType's afglobal.c, aflatin.c and afcjk.c.
//
// Characters go to glyphs by the font's character map alone (Skrifa's
// "nominal" shaping, FreeType built without HarfBuzz): a cluster of more
// than one character stands for no glyph.

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs::detail {

namespace {

#include "algorithms/glyphs/hinter_tables.inc"

inline constexpr usize most_widths = 16;
inline constexpr usize most_blues = 8;
inline constexpr usize most_blue_characters = 51;
inline constexpr i32 least_number = -0x7FFFFFFF - 1;
inline constexpr i32 greatest_number = 0x7FFFFFFF;

[[nodiscard]] constexpr i32 size_of(const i32 value) noexcept {
    const i32 result = value < 0 ? -value : value;
    return result;
}

// The clusters of a run of the table's characters: stretches between
// spaces.
struct Clusters final {
    usize cursor{0};
    usize end{0};
};

// The next cluster: its first character and its length. False at the end.
[[nodiscard]] bool next_cluster(Clusters& clusters, usize& first, usize& count) noexcept {
    while (clusters.cursor < clusters.end && table_characters[clusters.cursor] == 0x20) clusters.cursor = clusters.cursor + 1;
    if (clusters.cursor >= clusters.end) return false;
    first = clusters.cursor;
    while (clusters.cursor < clusters.end && table_characters[clusters.cursor] != 0x20) clusters.cursor = clusters.cursor + 1;
    count = clusters.cursor - first;
    return true;
}

// The glyph a cluster stands for: that of its one character, or none.
[[nodiscard]] u32 glyph_of_cluster(const Font& font, const usize first, const usize count) noexcept {
    const u32 result = count == 1 ? glyph_of(font, table_characters[first]) : 0;
    return result;
}

void sort(i32* const values, const usize count) noexcept {
    for (usize index = 1; index < count; ++index) {
        const i32 value = values[index];
        usize at = index;
        while (at > 0 && values[at - 1] > value) {
            values[at] = values[at - 1];
            at = at - 1;
        }
        values[at] = value;
    }
}

// Widths sorted, and those within `threshold` of one another made one: the
// mean of the cluster.
void sort_and_quantize(Vector<i32>& widths, const i32 threshold) noexcept {
    const usize count = widths.count();
    if (count <= 1) return;
    i32* const table = widths.mutable_view().data();
    sort(table, count);
    usize current = 0;
    i32 current_value = table[0];
    const usize last = count - 1;
    usize index = 1;
    while (index < count) {
        if ((table[index] - current_value) > threshold || index == last) {
            i32 sum = 0;
            if ((table[index] - current_value <= threshold) && index == last) index = index + 1;
            for (usize member = current; member < index; ++member) {
                sum = sum + table[member];
                table[member] = 0;
            }
            table[current] = sum / static_cast<i32>(index);
            if (index < last) {
                current = index + 1;
                current_value = table[current];
            }
        }
        index = index + 1;
    }
    current = 1;
    for (usize member = 1; member < count; ++member) {
        if (table[member] != 0) {
            table[current] = table[member];
            current = current + 1;
        }
    }
    widths.truncate(current);
}

// The glyph's outline in font units, as loaded; false when it cannot be,
// or when it is not within the hinter's bounds (hinter.hpp): such a glyph
// is not measured, as one the font has not.
[[nodiscard]] bool load(const Font& font, const u32 glyph, const FontLimits& limits, GlyphOutline& outline) noexcept {
    const bool result = load_glyph(font, glyph, limits, outline).succeeded() && within_hint_bounds(outline.points.view(), advance_of(font, glyph));
    return result;
}

// A script's standard widths: the stems of one of its standard characters,
// across each axis.
void compute_widths(const Font& font, const TableScript& script, const FontLimits& limits, HintScratch& scratch, StyleMetrics& metrics,
                    bool& no_memory) noexcept {
    const i32 units = static_cast<i32>(font.units_per_em);
    Clusters clusters{.cursor = script.standard_first, .end = script.standard_first + script.standard_count};
    usize first = 0;
    usize count = 0;
    bool filled = false;
    // The first standard character the font has.
    while (next_cluster(clusters, first, count)) {
        const u32 glyph = glyph_of_cluster(font, first, count);
        if (glyph == 0) continue;
        filled = load(font, glyph, limits, scratch.glyph) &&
                 fill_hint_outline(scratch.glyph.points.view(), scratch.glyph.tags.view(), scratch.glyph.contour_ends.view(), units,
                                   advance_of(font, glyph), scratch.outline)
                     .succeeded();
        break;
    }
    if (filled && !scratch.outline.points.empty()) {
        for (u32 dimension = 0; dimension < 2; ++dimension) {
            Vector<i32>& widths = metrics.axes[dimension].widths;
            HintAxis& axis = scratch.axis;
            reset_axis(axis, dimension, scratch.outline.winding);
            static_cast<void>(compute_segments(scratch.outline, axis, no_memory));
            link_segments(scratch.outline, axis, 0, false, 0);
            const usize segment_count = axis.segments.count();
            for (usize index = 0; index < segment_count; ++index) {
                const HintSegment& segment = axis.segments[index];
                if (segment.link == no_index) continue;
                const HintSegment& link = axis.segments[segment.link];
                // Each pair once.
                if (segment.link > index && link.link == index) {
                    if (widths.count() >= most_widths) break;
                    if (widths.push_copy(size_of(segment.position - link.position)).failed()) no_memory = true;
                }
            }
            if (widths.empty()) {
                if (widths.push_copy(0).failed()) no_memory = true;
            }
            sort_and_quantize(widths, units / 100);
        }
    }
    for (u32 dimension = 0; dimension < 2; ++dimension) {
        UnscaledAxisMetrics& axis = metrics.axes[dimension];
        const i32 standard = axis.widths.empty() ? for_em(units, 50) : axis.widths[0];
        axis.width_metrics.edge_distance_threshold = standard / 5;
        axis.width_metrics.standard_width = standard;
        axis.width_metrics.extra_light = false;
    }
}

// A contour of a loaded glyph, and one of its points.
struct Chosen final {
    usize first{0};
    usize count{0};
    usize point{0};
    bool found{false};
};

// A blue zone of the default scripts from one glyph: the highest (or
// lowest) point of the glyph, the flat stretch it is on, and whether that
// stretch is round. Requirement: the glyph is loaded in scratch.
struct Extremum final {
    bool found{false};
    i32 y{0};
    bool round{false};
};

[[nodiscard]] Extremum extremum_of(const GlyphOutline& glyph, const u32 zones, const i32 units, const i32 flat_threshold, i32& ascender,
                                   i32& descender) noexcept {
    Extremum result{};
    const usize point_count = glyph.points.count();
    if (point_count <= 2) return result;
    const Point* const all = glyph.points.view().data();
    const PointTag* const tags = glyph.tags.view().data();
    const bool top_like = (zones & (blue_top | blue_sub_top)) != 0;
    // The last contour that holds a point higher (lower) than any before
    // it, and that point. A contour of one point is passed over.
    Chosen best{};
    Chosen current{};
    bool current_found = false;
    bool have_y = false;
    i32 best_y = 0;
    usize contour_first = 0;
    for (usize contour = 0; contour < glyph.contour_ends.count(); ++contour) {
        const usize contour_last = glyph.contour_ends[contour];
        if (current_found) best = current;
        current = Chosen{.first = contour_first, .count = contour_last - contour_first + 1, .point = 0, .found = true};
        current_found = false;
        if (contour_last != contour_first) {
            for (usize index = contour_first; index <= contour_last; ++index) {
                const i32 y = all[index].y;
                const bool better = !have_y || (top_like ? y > best_y : y < best_y);
                if (better) {
                    best_y = y;
                    have_y = true;
                    current.point = index - contour_first;
                    current_found = true;
                }
                // Every point passed counts toward the zone's reach: the
                // better ones on its own side, the others on the far one.
                if (better == top_like) {
                    ascender = y > ascender ? y : ascender;
                } else {
                    descender = y < descender ? y : descender;
                }
            }
        }
        contour_first = contour_last + 1;
    }
    if (current_found) best = current;
    if (!best.found || !have_y) return result;
    const Point* const contour = all + best.first;
    const PointTag* const contour_tags = tags + best.first;
    const usize length = best.count;
    const usize best_point = best.point;
    const i32 best_x = contour[best_point].x;
    const bool best_on = contour_tags[best_point] == PointTag::on;
    bool have_first_on = best_on;
    bool have_last_on = best_on;
    usize first_on = best_point;
    usize last_on = best_point;
    usize segment_first = best_point;
    usize segment_last = best_point;
    // Back and forward from the point while the outline stays level.
    for (usize step = 0; step < length; ++step) {
        const usize index = (length - 1 - step + best_point) % length;
        const i32 distance = size_of(contour[index].y - best_y);
        if (distance > 5 && size_of(contour[index].x - best_x) <= 20 * distance) break;
        segment_first = index;
        if (contour_tags[index] == PointTag::on) {
            first_on = index;
            have_first_on = true;
            if (!have_last_on) {
                last_on = index;
                have_last_on = true;
            }
        }
    }
    usize next_index = 0;
    for (usize step = 0; step < length; ++step) {
        const usize index = (step + best_point + 1) % length;
        next_index = index;
        const i32 distance = size_of(contour[index].y - best_y);
        if (distance > 5 && size_of(contour[index].x - best_x) <= 20 * distance) break;
        segment_last = index;
        if (contour_tags[index] == PointTag::on) {
            last_on = index;
            have_last_on = true;
            if (!have_first_on) {
                first_on = index;
                have_first_on = true;
            }
        }
    }
    if ((zones & blue_long) != 0) {
        // The zone wants a long level stretch, and the one found is short:
        // look further round the contour for a long one near this height.
        const i32 length_threshold = units / 25;
        const i32 found_length = size_of(contour[segment_last].x - contour[segment_first].x);
        const usize span = segment_first <= segment_last ? segment_last - segment_first : (length - 1) - segment_first + 1 + segment_last;
        if (found_length < length_threshold && span + 2 <= length - 1) {
            const i32 height_threshold = units / 4;
            usize previous = best_point;
            for (usize step = 0; step < length; ++step) {
                const usize index = (length - 1 - step + best_point) % length;
                if (contour[index].x != best_x) {
                    previous = index;
                    break;
                }
            }
            if (previous == best_point) return result;
            const bool left_to_right = contour[previous].x < best_x;
            usize first = segment_last;
            usize last = first;
            bool have_p_first = false;
            bool have_p_last = false;
            usize p_first = 0;
            usize p_last = 0;
            bool hit = false;
            for (usize turn = 0; turn < 2 * length + 4; ++turn) {
                if (!hit) {
                    first = last;
                    have_p_first = contour_tags[first] == PointTag::on;
                    have_p_last = have_p_first;
                    p_first = first;
                    p_last = first;
                    hit = true;
                }
                last = last < length - 1 ? last + 1 : 0;
                if (size_of(best_y - contour[first].y) > height_threshold) {
                    hit = false;
                    if (last == segment_first) break;
                    continue;
                }
                const i32 distance = size_of(contour[last].y - contour[first].y);
                if (distance > 5 && size_of(contour[last].x - contour[first].x) <= 20 * distance) {
                    hit = false;
                    if (last == segment_first) break;
                    continue;
                }
                if (contour_tags[last] == PointTag::on) {
                    p_last = last;
                    have_p_last = true;
                    if (!have_p_first) {
                        p_first = last;
                        have_p_first = true;
                    }
                }
                const i32 first_x = contour[first].x;
                const i32 last_x = contour[last].x;
                if ((first_x < last_x) == left_to_right && size_of(last_x - first_x) >= length_threshold) {
                    // Long enough: on to its end.
                    for (usize further = 0; further < length; ++further) {
                        last = last < length - 1 ? last + 1 : 0;
                        const i32 dy = size_of(contour[last].y - contour[first].y);
                        if (dy > 5 && size_of(contour[next_index].x - contour[first].x) <= 20 * distance) {
                            last = last > 0 ? last - 1 : length - 1;
                            break;
                        }
                        p_last = last;
                        have_p_last = true;
                        if (contour_tags[last] == PointTag::on && !have_p_first) {
                            p_first = last;
                            have_p_first = true;
                        }
                        if (last == segment_first) break;
                    }
                    best_y = contour[first].y;
                    segment_first = first;
                    segment_last = last;
                    first_on = p_first;
                    have_first_on = have_p_first;
                    last_on = p_last;
                    have_last_on = have_p_last;
                    break;
                }
                if (last == segment_first) break;
            }
        }
    }
    bool round = contour_tags[segment_first] != PointTag::on || contour_tags[segment_last] != PointTag::on;
    if (have_first_on && have_last_on && size_of(contour[last_on].x - contour[first_on].x) > flat_threshold) round = false;
    result.found = true;
    result.y = best_y;
    result.round = round;
    return result;
}

void compute_default_blues(const Font& font, const TableScript& script, const FontLimits& limits, HintScratch& scratch,
                           Vector<UnscaledBlue>& blues, bool& no_memory) noexcept {
    const i32 units = static_cast<i32>(font.units_per_em);
    const i32 flat_threshold = units / 14;
    i32 flats[most_blue_characters] = {};
    i32 rounds[most_blue_characters] = {};
    for (usize blue_index = 0; blue_index < script.blue_count; ++blue_index) {
        const TableBlue& table = table_blues[script.blue_first + blue_index];
        const u32 zones = table.zones;
        const bool is_top = (zones & blue_top) != 0;
        const bool top_like = (zones & (blue_top | blue_sub_top)) != 0;
        i32 ascender = least_number;
        i32 descender = greatest_number;
        usize flat_count = 0;
        usize round_count = 0;
        if (blues.count() >= most_blues) continue;
        Clusters clusters{.cursor = table.first, .end = table.first + table.count};
        usize first = 0;
        usize count = 0;
        while (next_cluster(clusters, first, count)) {
            const u32 glyph = glyph_of_cluster(font, first, count);
            if (glyph == 0 || !load(font, glyph, limits, scratch.glyph)) continue;
            const Extremum extremum = extremum_of(scratch.glyph, zones, units, flat_threshold, ascender, descender);
            if (!extremum.found) continue;
            // A round top is no use to a neutral zone.
            if (extremum.round && (zones & blue_neutral) != 0) continue;
            if (extremum.round) {
                if (round_count < most_blue_characters) {
                    rounds[round_count] = extremum.y;
                    round_count = round_count + 1;
                }
            } else if (flat_count < most_blue_characters) {
                flats[flat_count] = extremum.y;
                flat_count = flat_count + 1;
            }
        }
        if (flat_count == 0 && round_count == 0) continue;
        sort(rounds, round_count);
        sort(flats, flat_count);
        // The zone's position is the median flat top, its overshoot the
        // median round one.
        i32 reference = 0;
        i32 shoot = 0;
        if (flat_count == 0) {
            reference = rounds[round_count / 2];
            shoot = reference;
        } else if (round_count == 0) {
            reference = flats[flat_count / 2];
            shoot = reference;
        } else {
            reference = flats[flat_count / 2];
            shoot = rounds[round_count / 2];
        }
        if (shoot != reference) {
            // An overshoot on the wrong side of its position: both go to
            // the middle.
            const bool over = shoot > reference;
            if (top_like != over) {
                reference = (shoot + reference) / 2;
                shoot = reference;
            }
        }
        UnscaledBlue blue{.position = reference,
                          .overshoot = shoot,
                          .ascender = ascender,
                          .descender = descender,
                          .zones = zones & (blue_top | blue_sub_top | blue_neutral)};
        if ((zones & blue_x_height) != 0) blue.zones = blue.zones | blue_adjustment;
        blue.zones = blue.zones | blue_bottom_of(script.blue_first + blue_index);
        if (blues.push_copy(blue).failed()) no_memory = true;
        static_cast<void>(is_top);
    }
    // Zones that overlap are pulled apart: in order of height, a zone's
    // far side is no further than the next zone's near side.
    const usize count = blues.count();
    if (count == 0) return;
    usize order[most_blues] = {};
    for (usize index = 0; index < most_blues; ++index) order[index] = index;
    UnscaledBlue* const values = blues.mutable_view().data();
    for (usize index = 1; index < count; ++index) {
        for (usize at = index; at >= 1; --at) {
            const UnscaledBlue& before = values[order[at - 1]];
            const UnscaledBlue& after = values[order[at]];
            const i32 a = (before.zones & (blue_top | blue_sub_top)) != 0 ? before.position : before.overshoot;
            const i32 b = (after.zones & (blue_top | blue_sub_top)) != 0 ? after.position : after.overshoot;
            if (b >= a) break;
            const usize swapped = order[at];
            order[at] = order[at - 1];
            order[at - 1] = swapped;
        }
    }
    for (usize index = 0; index + 1 < count; ++index) {
        UnscaledBlue& lower = values[order[index]];
        const UnscaledBlue& upper = values[order[index + 1]];
        const bool lower_top = (lower.zones & (blue_top | blue_sub_top)) != 0;
        const i32 a = lower_top ? lower.overshoot : lower.position;
        const i32 b = (upper.zones & (blue_top | blue_sub_top)) != 0 ? upper.overshoot : upper.position;
        if (a > b) {
            if (lower_top) {
                lower.overshoot = b;
            } else {
                lower.position = b;
            }
        }
    }
}

// The CJK scripts: a zone is the extreme of the characters' boxes, "fill"
// characters giving its position and "flat" ones (after a bar in the
// list) its overshoot. Only the zones along y are kept.
void compute_cjk_blues(const Font& font, const TableScript& script, const FontLimits& limits, HintScratch& scratch,
                       Vector<UnscaledBlue>& blues, bool& no_memory) noexcept {
    i32 flats[most_blue_characters] = {};
    i32 fills[most_blue_characters] = {};
    for (usize blue_index = 0; blue_index < script.blue_count; ++blue_index) {
        const TableBlue& table = table_blues[script.blue_first + blue_index];
        const u32 zones = table.zones;
        if ((zones & blue_horizontal) != 0) continue;
        const bool is_top = (zones & blue_top) != 0;
        if (blues.count() >= most_blues) continue;
        usize flat_count = 0;
        usize fill_count = 0;
        bool is_fill = true;
        Clusters clusters{.cursor = table.first, .end = table.first + table.count};
        usize first = 0;
        usize count = 0;
        while (next_cluster(clusters, first, count)) {
            if (count == 1 && table_characters[first] == '|') {
                is_fill = false;
                continue;
            }
            const u32 glyph = glyph_of_cluster(font, first, count);
            if (glyph == 0 || !load(font, glyph, limits, scratch.glyph)) continue;
            const usize point_count = scratch.glyph.points.count();
            if (point_count <= 2) continue;
            i32 best = scratch.glyph.points[0].y;
            for (usize index = 1; index < point_count; ++index) {
                const i32 y = scratch.glyph.points[index].y;
                best = is_top ? (y > best ? y : best) : (y < best ? y : best);
            }
            if (is_fill) {
                if (fill_count < most_blue_characters) {
                    fills[fill_count] = best;
                    fill_count = fill_count + 1;
                }
            } else if (flat_count < most_blue_characters) {
                flats[flat_count] = best;
                flat_count = flat_count + 1;
            }
        }
        if (flat_count == 0 && fill_count == 0) continue;
        sort(fills, fill_count);
        sort(flats, flat_count);
        i32 reference = 0;
        i32 shoot = 0;
        if (flat_count == 0) {
            reference = fills[fill_count / 2];
            shoot = reference;
        } else if (fill_count == 0) {
            reference = flats[flat_count / 2];
            shoot = reference;
        } else {
            reference = fills[fill_count / 2];
            shoot = flats[flat_count / 2];
        }
        if (shoot != reference) {
            const bool under = shoot < reference;
            if (is_top != under) {
                reference = (shoot + reference) / 2;
                shoot = reference;
            }
        }
        const UnscaledBlue blue{.position = reference, .overshoot = shoot, .ascender = 0, .descender = 0, .zones = zones & blue_top};
        if (blues.push_copy(blue).failed()) no_memory = true;
    }
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

[[nodiscard]] bool scale_default_axis(const UnscaledAxisMetrics& unscaled, HintScale& scale, ScaledAxisMetrics& axis) noexcept {
    const u32 dimension = unscaled.dimension;
    axis.dimension = dimension;
    axis.widths.clear();
    axis.blues.clear();
    axis.scale = dimension == axis_horizontal ? scale.x_scale : scale.y_scale;
    axis.delta = dimension == axis_horizontal ? scale.x_delta : scale.y_delta;
    const usize blue_count = unscaled.blues.count();
    // The x-height is made a whole number of pixels by changing the scale
    // along y a little, if that moves nothing by as much as two pixels.
    for (usize index = 0; index < blue_count; ++index) {
        const UnscaledBlue& blue = unscaled.blues[index];
        if ((blue.zones & blue_adjustment) == 0) continue;
        const i32 scaled = times_fixed(axis.scale, blue.overshoot);
        const i32 fitted = static_cast<i32>((static_cast<i64>(scaled) + 40) & ~static_cast<i64>(63));
        if (scaled != fitted && dimension == axis_vertical) {
            const i32 new_scale = times_over(axis.scale, fitted, scaled);
            i32 tallest = scale.units_per_em;
            for (usize other = 0; other < blue_count; ++other) {
                const UnscaledBlue& reach = unscaled.blues[other];
                tallest = reach.ascender > tallest ? reach.ascender : tallest;
                const i64 below = -static_cast<i64>(reach.descender);
                tallest = below > tallest ? static_cast<i32>(below > greatest_number ? greatest_number : below) : tallest;
            }
            const i32 moved = size_of(times_fixed(tallest, static_cast<i64>(new_scale) - axis.scale)) & ~127;
            if (moved == 0) {
                axis.scale = new_scale;
                scale.y_scale = new_scale;
            }
        }
        break;
    }
    axis.width_metrics = unscaled.width_metrics;
    for (usize index = 0; index < unscaled.widths.count(); ++index) {
        const i32 scaled = times_fixed(axis.scale, unscaled.widths[index]);
        if (axis.widths.push_copy(ScaledWidth{.scaled = scaled, .fitted = scaled}).failed()) return false;
    }
    // A standard stem thinner than five eighths of a pixel is extra light.
    axis.width_metrics.extra_light = times_fixed(axis.width_metrics.standard_width, axis.scale) < (32 + 8);
    if (dimension != axis_vertical) return true;
    for (usize index = 0; index < blue_count; ++index) {
        const UnscaledBlue& in_units = unscaled.blues[index];
        const i32 position = times_fixed(axis.scale, in_units.position) + axis.delta;
        const i32 overshoot = times_fixed(axis.scale, in_units.overshoot) + axis.delta;
        ScaledBlue blue{.position = ScaledWidth{.scaled = position, .fitted = position},
                        .overshoot = ScaledWidth{.scaled = overshoot, .fitted = overshoot},
                        .zones = in_units.zones,
                        .active = false};
        // A zone is used while its overshoot is no more than three
        // quarters of a pixel: then the overshoot is nothing, half a pixel
        // or one.
        const i32 distance = times_fixed(static_cast<i64>(in_units.position) - in_units.overshoot, axis.scale);
        if (distance >= -48 && distance <= 48) {
            i32 delta = size_of(distance);
            delta = delta < 32 ? 0 : (delta < 48 ? 32 : 64);
            if (distance < 0) delta = -delta;
            blue.position.fitted = pixel_round(blue.position.scaled);
            blue.overshoot.fitted = blue.position.fitted - delta;
            blue.active = true;
        }
        if (axis.blues.push_copy(blue).failed()) return false;
    }
    // A sub-top zone inside another zone is not used.
    for (usize index = 0; index < axis.blues.count(); ++index) {
        const ScaledBlue blue = axis.blues[index];
        if ((blue.zones & blue_sub_top) == 0 || !blue.active) continue;
        for (usize other = 0; other < axis.blues.count(); ++other) {
            const ScaledBlue& second = axis.blues[other];
            if ((second.zones & blue_sub_top) != 0 || !second.active) continue;
            if (second.position.fitted <= blue.overshoot.fitted && second.overshoot.fitted >= blue.position.fitted) {
                axis.blues[index].active = false;
                break;
            }
        }
    }
    return true;
}

[[nodiscard]] bool scale_cjk_axis(const UnscaledAxisMetrics& unscaled, const HintScale& scale, ScaledAxisMetrics& axis) noexcept {
    const u32 dimension = unscaled.dimension;
    axis.dimension = dimension;
    axis.widths.clear();
    axis.blues.clear();
    axis.scale = dimension == axis_horizontal ? scale.x_scale : scale.y_scale;
    axis.delta = dimension == axis_horizontal ? scale.x_delta : scale.y_delta;
    for (usize index = 0; index < unscaled.blues.count(); ++index) {
        const UnscaledBlue& in_units = unscaled.blues[index];
        const i32 position = times_fixed(in_units.position, axis.scale) + axis.delta;
        const i32 overshoot = times_fixed(in_units.overshoot, axis.scale) + axis.delta;
        ScaledBlue blue{.position = ScaledWidth{.scaled = position, .fitted = position},
                        .overshoot = ScaledWidth{.scaled = overshoot, .fitted = overshoot},
                        .zones = in_units.zones,
                        .active = false};
        const i32 distance = times_fixed(static_cast<i64>(in_units.position) - in_units.overshoot, axis.scale);
        if (distance >= -48 && distance <= 48) {
            blue.position.fitted = pixel_round(blue.position.scaled);
            const i32 first = divide_fixed(blue.position.fitted, axis.scale) - in_units.overshoot;
            i32 second = times_fixed(size_of(first), axis.scale);
            second = second < 32 ? 0 : pixel_round(second);
            if (first < 0) second = -second;
            blue.overshoot.fitted = blue.position.fitted - second;
            blue.active = true;
        }
        if (axis.blues.push_copy(blue).failed()) return false;
    }
    // The widths are not used at a size, but there are as many.
    for (usize index = 0; index < unscaled.widths.count(); ++index) {
        if (axis.widths.push_copy(ScaledWidth{}).failed()) return false;
    }
    axis.width_metrics = unscaled.width_metrics;
    return true;
}

} // namespace

u32 style_count() noexcept {
    const u32 result = static_cast<u32>(sizeof(table_styles) / sizeof(table_styles[0]));
    return result;
}

const TableScript& script_of_style(const u32 style) noexcept {
    const u32 known = style < style_count() ? style : static_cast<u32>(table_han_style);
    return table_scripts[table_styles[known].script];
}

OperationStatus compute_glyph_styles(const Font& font, Vector<u16>& styles) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    styles.clear();
    if (styles.resize(font.glyph_count).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    u16* const all = styles.mutable_view().data();
    for (usize glyph = 0; glyph < font.glyph_count; ++glyph) all[glyph] = static_cast<u16>(style_unassigned);
    // Every character of every range, in order of character: its glyph
    // takes the range's style unless it has one of a lower number.
    const usize range_count = sizeof(table_ranges) / sizeof(table_ranges[0]);
    for (usize index = 0; index < range_count; ++index) {
        const TableRange& range = table_ranges[index];
        const u32 marked = range.style | (range.non_base ? style_non_base : 0U);
        // Only the characters the font maps are gone through: each step
        // goes to the next one the character map has.
        // Only the characters the font maps are gone through: a stretch
        // of the character map at a time.
        u32 character = range.first;
        const u32 most = range.last - range.first + 1;
        for (u32 step = 0; step < most && character <= range.last; ++step) {
            MappedStretch stretch{};
            if (!next_stretch(font, character, stretch) || stretch.first > range.last) break;
            const u32 from = stretch.first > character ? stretch.first : character;
            const u32 to = stretch.last < range.last ? stretch.last : range.last;
            for (u32 mapped = from; mapped <= to; ++mapped) {
                const u32 glyph = glyph_in_stretch(font, stretch, mapped);
                if (glyph == 0) continue;
                const u32 current = all[glyph];
                if ((marked & style_mask) <= (current & style_mask)) all[glyph] = static_cast<u16>((current & ~style_mask) | marked);
            }
            if (to == range.last) break;
            character = to + 1;
        }
    }
    // What is left is given the style of Han.
    for (usize glyph = 0; glyph < font.glyph_count; ++glyph) {
        if ((all[glyph] & style_mask) == style_unassigned) all[glyph] = static_cast<u16>((all[glyph] & ~style_mask) | table_han_style);
    }
    for (u32 digit = '0'; digit <= '9'; ++digit) {
        const u32 glyph = glyph_of(font, digit);
        if (glyph != 0) all[glyph] = static_cast<u16>(all[glyph] | style_digit);
    }
    return OperationStatus::success();
}

OperationStatus compute_style_metrics(const Font& font, const u32 style, const FontLimits& limits, HintScratch& scratch, StyleMetrics& metrics) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    metrics.style = style;
    metrics.digits_same_width = true;
    for (u32 dimension = 0; dimension < 2; ++dimension) {
        metrics.axes[dimension].dimension = dimension;
        metrics.axes[dimension].widths.clear();
        metrics.axes[dimension].blues.clear();
        metrics.axes[dimension].width_metrics = WidthMetrics{};
    }
    const TableScript& script = script_of_style(style);
    bool no_memory = false;
    compute_widths(font, script, limits, scratch, metrics, no_memory);
    if (script.group == group_default) {
        compute_default_blues(font, script, limits, scratch, metrics.axes[1].blues, no_memory);
    } else if (script.group == group_cjk) {
        compute_cjk_blues(font, script, limits, scratch, metrics.axes[1].blues, no_memory);
    }
    bool have_advance = false;
    i32 digit_advance = 0;
    for (u32 digit = '0'; digit <= '9'; ++digit) {
        const u32 glyph = glyph_of(font, digit);
        if (glyph == 0) continue;
        const i32 advance = advance_of(font, glyph);
        if (have_advance && advance != digit_advance) {
            metrics.digits_same_width = false;
            break;
        }
        digit_advance = advance;
        have_advance = true;
    }
    if (no_memory) return refuse_font(ErrorKind::out_of_memory, operation);
    return OperationStatus::success();
}

HintScale make_scale(const Font& font, const i64 size, const HintMode mode, const u32 group) noexcept {
    HintScale scale{};
    // Held to hint_most_scale: a glyph is hinted only at no greater scale
    // (hint_loaded), so the cut never changes one that is.
    const i64 full = scale_of(font, size);
    const i32 value = static_cast<i32>(full < hint_most_scale ? full : hint_most_scale);
    scale.x_scale = value;
    scale.y_scale = value;
    scale.units_per_em = static_cast<i32>(font.units_per_em);
    scale.pixels = static_cast<i32>(size >> 6);
    const bool mono = mode == HintMode::monochrome;
    const bool light = mode == HintMode::light;
    if (mono) scale.flags = scale.flags | scale_horizontal_snap | scale_vertical_snap | scale_mono;
    if (!light) scale.flags = scale.flags | scale_stem_adjust;
    if (group == group_default) {
        // Along x the default scripts are not hinted in light mode, nor
        // when the face is slanted.
        if (light || font.italic) scale.flags = scale.flags | scale_no_horizontal;
    } else {
        scale.flags = scale.flags | scale_no_advance;
    }
    return scale;
}

OperationStatus scale_style_metrics(const StyleMetrics& metrics, const HintScale& scale, ScaledStyleMetrics& scaled) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    scaled.scale = scale;
    const bool is_default = script_of_style(metrics.style).group == group_default;
    bool done = true;
    for (u32 dimension = 0; dimension < 2; ++dimension) {
        if (is_default) {
            done = done && scale_default_axis(metrics.axes[dimension], scaled.scale, scaled.axes[dimension]);
        } else {
            done = done && scale_cjk_axis(metrics.axes[dimension], scaled.scale, scaled.axes[dimension]);
        }
    }
    if (!done) return refuse_font(ErrorKind::out_of_memory, operation);
    return OperationStatus::success();
}

} // namespace stxlib::glyphs::detail
