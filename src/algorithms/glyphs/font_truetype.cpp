// stxlib::glyphs: TrueType outlines (include/stxlib/font_file.hpp). A
// glyph's points in font units, composite glyphs put together. After
// ttf-parser 0.25.1 (tables/loca.rs, tables/glyf.rs: the glyph's range, the
// passes over flags and coordinates) and, for composite glyphs in whole
// numbers, Skrifa 0.50.0 (outline/glyf/mod.rs, load_composite): a
// component's two-by-two numbers are 2.14 taken as 16.16, a point is
// multiplied and rounded to the nearest unit, an offset is added or two
// points are made to meet.
//
// The outline is placed as TrueType places it: moved along x so that the
// left edge of the glyph's box is at its left side bearing.
//
// No instructions are run: they are stepped over. A composite glyph is
// put together with a stack of fixed depth, not by a function calling
// itself.

#include "algorithms/glyphs/font_reader.hpp"

namespace stxlib::glyphs::detail {

namespace {

inline constexpr u32 flag_on_curve = 0x01;
inline constexpr u32 flag_x_short = 0x02;
inline constexpr u32 flag_y_short = 0x04;
inline constexpr u32 flag_repeat = 0x08;
inline constexpr u32 flag_x_same_or_positive = 0x10;
inline constexpr u32 flag_y_same_or_positive = 0x20;

inline constexpr u32 arguments_are_words = 0x0001;
inline constexpr u32 arguments_are_offsets = 0x0002;
inline constexpr u32 has_scale = 0x0008;
inline constexpr u32 more_components = 0x0020;
inline constexpr u32 has_two_scales = 0x0040;
inline constexpr u32 has_two_by_two = 0x0080;
inline constexpr u32 scaled_offset = 0x0800;
inline constexpr u32 unscaled_offset = 0x1000;

inline constexpr usize deepest = 16;
// No coordinate of a glyph is farther from the origin than this, in font
// units or, scaled, in 1/64 of a pixel: one that is, in a font not well
// formed or at a scale beyond any use, is refused before any arithmetic
// could leave 32 bits.
inline constexpr i64 farthest = farthest_point;

[[nodiscard]] constexpr bool near_enough(const i64 x, const i64 y) noexcept {
    const bool result = x >= -farthest && x <= farthest && y >= -farthest && y <= farthest;
    return result;
}

// A glyph's bytes in glyf: none for a glyph with no outline (a space), or
// one the tables do not place.
[[nodiscard]] Span glyph_data(const Font& font, const u32 glyph) noexcept {
    const Span loca = table_of(font, font.loca);
    const Span glyf = table_of(font, font.glyf);
    usize begin = 0;
    usize end = 0;
    if (font.long_offsets) {
        if (!has(loca, static_cast<usize>(glyph) * 4, 8)) return Span{};
        begin = read_u32(loca, static_cast<usize>(glyph) * 4);
        end = read_u32(loca, static_cast<usize>(glyph) * 4 + 4);
    } else {
        if (!has(loca, static_cast<usize>(glyph) * 2, 4)) return Span{};
        begin = static_cast<usize>(read_u16(loca, static_cast<usize>(glyph) * 2)) * 2;
        end = static_cast<usize>(read_u16(loca, static_cast<usize>(glyph) * 2 + 2)) * 2;
    }
    // As FreeType reads a loca not well formed: a glyph that begins past
    // glyf has nothing; the last glyph's end past glyf is glyf's end (any
    // other glyph's is nothing); an end before the beginning is glyf's end.
    // The glyph's own reading is bounded by what it is given.
    const usize entry = font.long_offsets ? 4 : 2;
    const bool last_glyph = loca.size / entry >= 2 && static_cast<usize>(glyph) == loca.size / entry - 2;
    if (begin > glyf.size) return Span{};
    if (end > glyf.size) {
        if (!last_glyph) return Span{};
        end = glyf.size;
    }
    if (end < begin) end = glyf.size;
    if (begin == end || !has(glyf, begin, end - begin)) return Span{};
    const Span result = part(glyf, begin, end - begin);
    return result;
}

// A glyph's left side bearing in hmtx: beside its advance, or, past the
// last advance, in the list of bearings that follows; 0 when the table
// does not reach it.
[[nodiscard]] i32 bearing_of(const Font& font, const u32 glyph) noexcept {
    const Span hmtx = table_of(font, font.hmtx);
    const usize advances = font.advance_count;
    if (glyph < advances) {
        if (!has(hmtx, static_cast<usize>(glyph) * 4 + 2, 2)) return 0;
        const i32 result = read_i16(hmtx, static_cast<usize>(glyph) * 4 + 2);
        return result;
    }
    const usize at = advances * 4 + (static_cast<usize>(glyph) - advances) * 2;
    if (!has(hmtx, at, 2)) return 0;
    const i32 result = read_i16(hmtx, at);
    return result;
}

// A composite glyph being put together: where its next component is read,
// and the component whose glyph is being loaded.
struct Level final {
    Span data{};
    usize cursor{0};
    // The first point of this glyph, and of the component being loaded.
    usize base{0};
    usize component_base{0};
    u32 flags{0};
    i32 first_argument{0};
    i32 second_argument{0};
    i64 xx{0x10000};
    i64 xy{0};
    i64 yx{0};
    i64 yy{0x10000};
    bool waiting{false};
};

struct Loading final {
    GlyphOutline* outline{nullptr};
    const FontLimits* limits{nullptr};
    u32 components{0};
    // The scales (16.16) when the glyph is loaded scaled: each simple
    // glyph's points and each offset are scaled on their own, and what is
    // put together is in 26.6 pixels.
    bool scaled{false};
    i64 x_scale{0};
    i64 y_scale{0};
};

enum class Loaded : u8 {
    done = 0,
    invalid = 1,
    too_large = 2,
    no_memory = 3,
};

// A simple glyph's points, appended. `data` is the glyph's bytes after its
// ten-byte header.
[[nodiscard]] Loaded append_simple(Loading& loading, const Span& data, const usize contour_count) noexcept {
    GlyphOutline& outline = *loading.outline;
    const FontLimits& limits = *loading.limits;
    if (!has(data, 0, contour_count * 2 + 2)) return Loaded::invalid;
    const usize base = outline.points.count();
    if (outline.contour_ends.count() + contour_count > limits.contours) return Loaded::too_large;
    // The last point of each contour, each after the one before.
    usize point_count = 0;
    for (usize contour = 0; contour < contour_count; ++contour) {
        const usize end = read_u16(data, contour * 2);
        if (contour != 0 && end + 1 <= point_count) return Loaded::invalid;
        point_count = end + 1;
        if (outline.contour_ends.push_copy(static_cast<u32>(base + end)).failed()) return Loaded::no_memory;
    }
    if (base + point_count > limits.points) return Loaded::too_large;
    const usize instructions = read_u16(data, contour_count * 2);
    const usize flags_at = contour_count * 2 + 2 + instructions;
    if (!has(data, flags_at, 0)) return Loaded::invalid;
    // One pass over the flags for the lengths of the two runs of
    // coordinates, so that each run is checked once.
    usize cursor = flags_at;
    usize x_bytes = 0;
    usize y_bytes = 0;
    usize flagged = 0;
    while (flagged < point_count) {
        if (!has(data, cursor, 1)) return Loaded::invalid;
        const u32 flags = read_u8(data, cursor);
        cursor = cursor + 1;
        usize times = 1;
        if ((flags & flag_repeat) != 0) {
            if (!has(data, cursor, 1)) return Loaded::invalid;
            times = static_cast<usize>(read_u8(data, cursor)) + 1;
            cursor = cursor + 1;
        }
        if (times > point_count - flagged) return Loaded::invalid;
        const usize x_each = (flags & flag_x_short) != 0 ? 1 : ((flags & flag_x_same_or_positive) != 0 ? 0 : 2);
        const usize y_each = (flags & flag_y_short) != 0 ? 1 : ((flags & flag_y_same_or_positive) != 0 ? 0 : 2);
        x_bytes = x_bytes + x_each * times;
        y_bytes = y_bytes + y_each * times;
        flagged = flagged + times;
    }
    const usize x_at = cursor;
    const usize y_at = x_at + x_bytes;
    if (!has(data, x_at, x_bytes) || !has(data, y_at, y_bytes)) return Loaded::invalid;
    if (outline.points.resize(base + point_count).failed() || outline.tags.resize(base + point_count).failed()) return Loaded::no_memory;
    Point* const points = outline.points.mutable_view().data() + base;
    PointTag* const tags = outline.tags.mutable_view().data() + base;
    // The second pass: flags again, with the coordinates beside them.
    usize flag_cursor = flags_at;
    usize x_cursor = x_at;
    usize y_cursor = y_at;
    usize repeats = 0;
    u32 flags = 0;
    i32 x = 0;
    i32 y = 0;
    for (usize index = 0; index < point_count; ++index) {
        if (repeats == 0) {
            flags = read_u8(data, flag_cursor);
            flag_cursor = flag_cursor + 1;
            if ((flags & flag_repeat) != 0) {
                repeats = read_u8(data, flag_cursor);
                flag_cursor = flag_cursor + 1;
            }
        } else {
            repeats = repeats - 1;
        }
        if ((flags & flag_x_short) != 0) {
            const i32 step = static_cast<i32>(read_u8(data, x_cursor));
            x = x + ((flags & flag_x_same_or_positive) != 0 ? step : -step);
            x_cursor = x_cursor + 1;
        } else if ((flags & flag_x_same_or_positive) == 0) {
            x = x + read_i16(data, x_cursor);
            x_cursor = x_cursor + 2;
        }
        if ((flags & flag_y_short) != 0) {
            const i32 step = static_cast<i32>(read_u8(data, y_cursor));
            y = y + ((flags & flag_y_same_or_positive) != 0 ? step : -step);
            y_cursor = y_cursor + 1;
        } else if ((flags & flag_y_same_or_positive) == 0) {
            y = y + read_i16(data, y_cursor);
            y_cursor = y_cursor + 2;
        }
        points[index] = Point{.x = x, .y = y};
        tags[index] = (flags & flag_on_curve) != 0 ? PointTag::on : PointTag::quadratic;
    }
    // Coordinates are sums of 16-bit steps, so they are inside 32 bits; a
    // glyph is refused when one is far outside any font's em.
    bool all_near = true;
    for (usize index = 0; index < point_count; ++index) all_near = all_near && near_enough(points[index].x, points[index].y);
    if (!all_near) return Loaded::invalid;
    if (loading.scaled) {
        for (usize index = 0; index < point_count; ++index) {
            const i64 scaled_x = multiply_fixed(points[index].x, loading.x_scale);
            const i64 scaled_y = multiply_fixed(points[index].y, loading.y_scale);
            if (!near_enough(scaled_x, scaled_y)) return Loaded::invalid;
            points[index] = Point{.x = static_cast<i32>(scaled_x), .y = static_cast<i32>(scaled_y)};
        }
    }
    return Loaded::done;
}

// The component a level was waiting for is loaded: its points, from
// component_base on, are multiplied and moved into place.
[[nodiscard]] Loaded place_component(Loading& loading, const Level& level) noexcept {
    GlyphOutline& outline = *loading.outline;
    const usize count = outline.points.count();
    Point* const points = outline.points.mutable_view().data();
    const bool multiplied = (level.flags & (has_scale | has_two_scales | has_two_by_two)) != 0;
    if (multiplied) {
        for (usize index = level.component_base; index < count; ++index) {
            const i64 x = points[index].x;
            const i64 y = points[index].y;
            const i64 new_x = multiply_fixed(x, level.xx) + multiply_fixed(y, level.xy);
            const i64 new_y = multiply_fixed(x, level.yx) + multiply_fixed(y, level.yy);
            if (!near_enough(new_x, new_y)) return Loaded::invalid;
            points[index].x = static_cast<i32>(new_x);
            points[index].y = static_cast<i32>(new_y);
        }
    }
    i64 dx = 0;
    i64 dy = 0;
    if ((level.flags & arguments_are_offsets) != 0) {
        dx = level.first_argument;
        dy = level.second_argument;
        if (multiplied && (level.flags & (scaled_offset | unscaled_offset)) == scaled_offset) {
            // The offset is in the component's own measure: the greater of
            // the two numbers and three eighths of the lesser stands for
            // their length.
            const i64 xx = level.xx < 0 ? -level.xx : level.xx;
            const i64 xy = level.xy < 0 ? -level.xy : level.xy;
            const i64 yy = level.yy < 0 ? -level.yy : level.yy;
            const i64 yx = level.yx < 0 ? -level.yx : level.yx;
            const i64 x_length = xx > xy ? xx + ((3 * xy) >> 3) : xy + ((3 * xx) >> 3);
            const i64 y_length = yy > yx ? yy + ((3 * yx) >> 3) : yx + ((3 * yy) >> 3);
            dx = multiply_fixed(dx, x_length);
            dy = multiply_fixed(dy, y_length);
        }
        if (loading.scaled) {
            dx = multiply_fixed(dx, loading.x_scale);
            dy = multiply_fixed(dy, loading.y_scale);
        }
    } else {
        // A point of the component meets a point of the glyph so far.
        const usize anchor = level.base + static_cast<usize>(level.first_argument);
        const usize moved = level.component_base + static_cast<usize>(level.second_argument);
        if (anchor >= level.component_base || moved >= count) return Loaded::invalid;
        dx = static_cast<i64>(points[anchor].x) - points[moved].x;
        dy = static_cast<i64>(points[anchor].y) - points[moved].y;
    }
    if (dx != 0 || dy != 0) {
        for (usize index = level.component_base; index < count; ++index) {
            const i64 new_x = points[index].x + dx;
            const i64 new_y = points[index].y + dy;
            if (!near_enough(new_x, new_y)) return Loaded::invalid;
            points[index].x = static_cast<i32>(new_x);
            points[index].y = static_cast<i32>(new_y);
        }
    }
    return Loaded::done;
}

// The next component of a composite glyph, read into its level.
// `more` says whether one was read.
[[nodiscard]] Loaded read_component(Level& level, u32& glyph, bool& more) noexcept {
    more = false;
    const Span& data = level.data;
    if (!has(data, level.cursor, 4)) return Loaded::invalid;
    const u32 flags = read_u16(data, level.cursor);
    glyph = read_u16(data, level.cursor + 2);
    usize cursor = level.cursor + 4;
    const usize argument_bytes = (flags & arguments_are_words) != 0 ? 4 : 2;
    const usize scale_bytes = (flags & has_two_by_two) != 0 ? 8 : ((flags & has_two_scales) != 0 ? 4 : ((flags & has_scale) != 0 ? 2 : 0));
    if (!has(data, cursor, argument_bytes + scale_bytes)) return Loaded::invalid;
    const bool offsets = (flags & arguments_are_offsets) != 0;
    if ((flags & arguments_are_words) != 0) {
        level.first_argument = offsets ? read_i16(data, cursor) : static_cast<i32>(read_u16(data, cursor));
        level.second_argument = offsets ? read_i16(data, cursor + 2) : static_cast<i32>(read_u16(data, cursor + 2));
    } else {
        level.first_argument = offsets ? read_i8(data, cursor) : static_cast<i32>(read_u8(data, cursor));
        level.second_argument = offsets ? read_i8(data, cursor + 1) : static_cast<i32>(read_u8(data, cursor + 1));
    }
    cursor = cursor + argument_bytes;
    // 2.14 numbers, as 16.16.
    level.xx = 0x10000;
    level.xy = 0;
    level.yx = 0;
    level.yy = 0x10000;
    if ((flags & has_two_by_two) != 0) {
        level.xx = static_cast<i64>(read_i16(data, cursor)) * 4;
        level.yx = static_cast<i64>(read_i16(data, cursor + 2)) * 4;
        level.xy = static_cast<i64>(read_i16(data, cursor + 4)) * 4;
        level.yy = static_cast<i64>(read_i16(data, cursor + 6)) * 4;
    } else if ((flags & has_two_scales) != 0) {
        level.xx = static_cast<i64>(read_i16(data, cursor)) * 4;
        level.yy = static_cast<i64>(read_i16(data, cursor + 2)) * 4;
    } else if ((flags & has_scale) != 0) {
        level.xx = static_cast<i64>(read_i16(data, cursor)) * 4;
        level.yy = level.xx;
    }
    level.cursor = cursor + scale_bytes;
    level.flags = flags;
    more = true;
    return Loaded::done;
}

} // namespace

OperationStatus load_truetype_glyph(const Font& font, const u32 glyph, const i64 x_scale, const i64 y_scale,
                                    const FontLimits& limits, GlyphOutline& outline) noexcept {
    const Operation operation("stxlib.glyphs.load_glyph");
    // Scales are 16.16 and no greater than 16,384 pixels to the em of the
    // smallest em there is; 0 for both: font units.
    const bool scaled = x_scale != 0 || y_scale != 0;
    if (x_scale < 0 || y_scale < 0 || x_scale > (static_cast<i64>(1) << 32) || y_scale > (static_cast<i64>(1) << 32)) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    Loading loading{.outline = &outline, .limits = &limits, .components = 0, .scaled = scaled, .x_scale = x_scale, .y_scale = y_scale};
    Level levels[deepest] = {};
    const usize allowed = limits.component_depth < deepest ? limits.component_depth : deepest;
    usize depth = 0;
    // The glyph to begin, when there is one; else the level on top goes on.
    u32 entering = glyph;
    bool enter = true;
    Loaded result = Loaded::done;
    // Every turn begins a glyph, or reads a component, or finishes a level:
    // no more turns than three for each component allowed.
    const usize turns = static_cast<usize>(limits.components) * 3 + 8;
    for (usize turn = 0; turn < turns && result == Loaded::done; ++turn) {
        if (enter) {
            enter = false;
            const Span data = glyph_data(font, entering);
            if (data.size != 0) {
                if (!has(data, 0, 10)) {
                    result = Loaded::invalid;
                    break;
                }
                const i32 contours = read_i16(data, 0);
                const Span body = part(data, 10, data.size - 10);
                if (contours > 0) {
                    result = append_simple(loading, body, static_cast<usize>(contours));
                } else if (contours < 0) {
                    if (depth >= allowed) {
                        result = Loaded::too_large;
                        break;
                    }
                    levels[depth] = Level{};
                    levels[depth].data = body;
                    levels[depth].base = outline.points.count();
                    depth = depth + 1;
                    continue;
                }
            }
            if (result != Loaded::done) break;
        }
        if (depth == 0) break;
        Level& level = levels[depth - 1];
        if (level.waiting) {
            level.waiting = false;
            if (outline.points.count() != level.component_base) result = place_component(loading, level);
            if (result != Loaded::done) break;
            if ((level.flags & more_components) == 0) {
                depth = depth - 1;
                continue;
            }
        }
        bool more = false;
        u32 component = 0;
        result = read_component(level, component, more);
        if (result != Loaded::done) break;
        if (component >= font.glyph_count) {
            result = Loaded::invalid;
            break;
        }
        loading.components = loading.components + 1;
        if (loading.components > limits.components) {
            result = Loaded::too_large;
            break;
        }
        level.component_base = outline.points.count();
        level.waiting = true;
        entering = component;
        enter = true;
    }
    if (result == Loaded::done && (depth != 0 || enter)) result = Loaded::too_large;
    if (result == Loaded::done) {
        // The glyph's origin is where its left side bearing puts it: the
        // outline moves left by what its box's left edge is beyond the
        // bearing (TrueType's first phantom point).
        const Span data = glyph_data(font, glyph);
        const i64 in_units = has(data, 0, 10) ? static_cast<i64>(read_i16(data, 2)) - bearing_of(font, glyph) : 0;
        const i64 shift = scaled ? multiply_fixed(in_units, x_scale) : in_units;
        if (shift != 0) {
            const usize count = outline.points.count();
            Point* const points = outline.points.mutable_view().data();
            for (usize index = 0; index < count; ++index) {
                const i64 x = points[index].x - shift;
                if (!near_enough(x, 0)) return refuse_font(ErrorKind::invalid_argument, operation);
                points[index].x = static_cast<i32>(x);
            }
        }
    }
    if (result == Loaded::invalid) return refuse_font(ErrorKind::invalid_argument, operation);
    if (result == Loaded::too_large) return refuse_font(ErrorKind::limit_exceeded, operation);
    if (result == Loaded::no_memory) return refuse_font(ErrorKind::out_of_memory, operation);
    return OperationStatus::success();
}

} // namespace stxlib::glyphs::detail
