// stxlib::glyphs: a font file, read (include/stxlib/font_file.hpp). The
// table directory, the face's numbers, the character map, advances, the
// scale and the outline as segments. After ttf-parser 0.25.1 (lib.rs,
// tables/head.rs, hhea.rs, maxp.rs, hmtx.rs, os2.rs, cmap), in integers
// and with every range checked where it is read.

#include "stxlib/font_file.hpp"

#include "algorithms/glyphs/font_reader.hpp"

namespace stxlib::glyphs {

namespace detail {

Status refuse_font(const ErrorKind kind, const Operation operation) noexcept {
    const Status result = Status::failure(ErrorRecord::make(kind, operation));
    return result;
}

} // namespace detail

namespace {

using detail::has;
using detail::part;
using detail::read_i16;
using detail::read_u16;
using detail::read_u32;
using detail::refuse_font;
using detail::Span;
using detail::span_of;
using detail::table_of;

inline constexpr u32 magic_truetype = 0x00010000U;
inline constexpr u32 magic_true = 0x74727565U;
inline constexpr u32 magic_opentype = 0x4F54544FU;
inline constexpr u32 magic_collection = 0x74746366U;

[[nodiscard]] constexpr u32 tag(const char a, const char b, const char c, const char d) noexcept {
    const u32 result = (static_cast<u32>(static_cast<u8>(a)) << 24) | (static_cast<u32>(static_cast<u8>(b)) << 16) |
                       (static_cast<u32>(static_cast<u8>(c)) << 8) | static_cast<u32>(static_cast<u8>(d));
    return result;
}

// The tables looked for, in one pass over the directory.
struct Tables final {
    FontTable head{};
    FontTable maxp{};
    FontTable hhea{};
    FontTable hmtx{};
    FontTable os2{};
    FontTable cmap{};
    FontTable loca{};
    FontTable glyf{};
    FontTable cff{};
};

// The character maps this reads, and which is taken: a map of all of
// Unicode before one of its first 65,536 characters, Windows' before
// Unicode's of the same reach, the first of a kind in the table.
inline constexpr u32 platform_unicode = 0;
inline constexpr u32 platform_windows = 3;
struct Preference final {
    u32 platform{0};
    u32 encoding{0};
};
inline constexpr Preference preferences[] = {
    {platform_windows, 10}, {platform_unicode, 6}, {platform_unicode, 4}, {platform_windows, 1},
    {platform_unicode, 3},  {platform_unicode, 2}, {platform_unicode, 1}, {platform_unicode, 0},
};

// Whether a character map's header is inside its bytes; its length is
// then known to glyph_of, which checks each entry it reads.
[[nodiscard]] bool map_readable(const Span& map, const u32 format) noexcept {
    if (format == 0) return has(map, 6, 256);
    if (format == 4) {
        if (!has(map, 0, 14)) return false;
        const usize segments = read_u16(map, 6) / 2;
        const bool result = segments >= 1 && has(map, 14, segments * 8 + 2);
        return result;
    }
    if (format == 6) {
        if (!has(map, 0, 10)) return false;
        const bool result = has(map, 10, static_cast<usize>(read_u16(map, 8)) * 2);
        return result;
    }
    if (format == 12) {
        if (!has(map, 0, 16)) return false;
        const usize groups = read_u32(map, 12);
        const bool result = groups <= (map.size - 16) / 12;
        return result;
    }
    return false;
}

void choose_character_map(const Span& whole, const FontTable& table, Font& font) noexcept {
    const Span cmap = table_of(font, table);
    if (!has(cmap, 0, 4)) return;
    const usize records = read_u16(cmap, 2);
    if (!has(cmap, 4, records * 8)) return;
    for (const Preference& wanted : preferences) {
        for (usize index = 0; index < records; ++index) {
            const usize record = 4 + index * 8;
            if (read_u16(cmap, record) != wanted.platform || read_u16(cmap, record + 2) != wanted.encoding) continue;
            const usize offset = read_u32(cmap, record + 4);
            if (!has(cmap, offset, 2)) continue;
            const Span map = part(cmap, offset, cmap.size - offset);
            const u32 format = read_u16(map, 0);
            if (!map_readable(map, format)) continue;
            font.character_map = FontTable{.offset = static_cast<u32>(table.offset + offset), .length = static_cast<u32>(map.size)};
            font.character_map_format = format;
            return;
        }
    }
    static_cast<void>(whole);
}

// Format 4: segments of the first 65,536 characters, by a search for the
// first segment that ends at or after the character.
[[nodiscard]] u32 glyph_in_segments(const Span& map, const u32 character) noexcept {
    if (character > 0xFFFFU) return 0;
    const usize segments = read_u16(map, 6) / 2;
    const usize ends = 14;
    const usize starts = ends + segments * 2 + 2;
    const usize deltas = starts + segments * 2;
    const usize offsets = deltas + segments * 2;
    usize low = 0;
    usize high = segments;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (read_u16(map, ends + middle * 2) < character) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == segments) return 0;
    const u32 start = read_u16(map, starts + low * 2);
    if (start > character) return 0;
    const u32 delta = read_u16(map, deltas + low * 2);
    const u32 range_offset = read_u16(map, offsets + low * 2);
    if (range_offset == 0) {
        const u32 result = (character + delta) & 0xFFFFU;
        return result;
    }
    if (range_offset == 0xFFFFU) return 0;
    // The offset is from its own place in the table.
    const usize place = offsets + low * 2 + range_offset + static_cast<usize>(character - start) * 2;
    if (!has(map, place, 2)) return 0;
    const u32 listed = read_u16(map, place);
    if (listed == 0) return 0;
    const u32 result = (listed + delta) & 0xFFFFU;
    return result;
}

// Format 12: groups of consecutive characters, by a search.
[[nodiscard]] u32 glyph_in_groups(const Span& map, const u32 character) noexcept {
    usize low = 0;
    usize high = read_u32(map, 12);
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const usize group = 16 + middle * 12;
        const u32 first = read_u32(map, group);
        const u32 last = read_u32(map, group + 4);
        if (last < character) {
            low = middle + 1;
        } else if (first > character) {
            high = middle;
        } else {
            const u64 glyph = static_cast<u64>(read_u32(map, group + 8)) + (character - first);
            const u32 result = glyph <= 0xFFFFU ? static_cast<u32>(glyph) : 0;
            return result;
        }
    }
    return 0;
}

} // namespace

Status open_font(const View<u8> bytes, const u32 face, Font& font) noexcept {
    const Operation operation("stxlib.glyphs.open_font");
    font = Font{};
    const Span whole = span_of(bytes);
    if (whole.size > 0xFFFFFFFFU || !has(whole, 0, 12)) return refuse_font(ErrorKind::invalid_argument, operation);
    usize start = 0;
    u32 magic = read_u32(whole, 0);
    if (magic == magic_collection) {
        const u32 faces = read_u32(whole, 8);
        if (face >= faces || !has(whole, 12, static_cast<usize>(faces) * 4)) return refuse_font(ErrorKind::invalid_argument, operation);
        start = read_u32(whole, 12 + static_cast<usize>(face) * 4);
        if (!has(whole, start, 12)) return refuse_font(ErrorKind::invalid_argument, operation);
        magic = read_u32(whole, start);
    } else if (face != 0) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    if (magic != magic_truetype && magic != magic_true && magic != magic_opentype) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    const usize table_count = read_u16(whole, start + 4);
    if (!has(whole, start + 12, table_count * 16)) return refuse_font(ErrorKind::invalid_argument, operation);
    Tables tables{};
    for (usize index = 0; index < table_count; ++index) {
        const usize record = start + 12 + index * 16;
        const u32 name = read_u32(whole, record);
        const FontTable table{.offset = read_u32(whole, record + 8), .length = read_u32(whole, record + 12)};
        // A table whose range is not in the file is as if it were absent.
        if (!has(whole, table.offset, table.length)) continue;
        if (name == tag('h', 'e', 'a', 'd')) tables.head = table;
        if (name == tag('m', 'a', 'x', 'p')) tables.maxp = table;
        if (name == tag('h', 'h', 'e', 'a')) tables.hhea = table;
        if (name == tag('h', 'm', 't', 'x')) tables.hmtx = table;
        if (name == tag('O', 'S', '/', '2')) tables.os2 = table;
        if (name == tag('c', 'm', 'a', 'p')) tables.cmap = table;
        if (name == tag('l', 'o', 'c', 'a')) tables.loca = table;
        if (name == tag('g', 'l', 'y', 'f')) tables.glyf = table;
        if (name == tag('C', 'F', 'F', ' ')) tables.cff = table;
    }
    font.bytes = bytes;
    const Span head = table_of(font, tables.head);
    const Span maxp = table_of(font, tables.maxp);
    const Span hhea = table_of(font, tables.hhea);
    if (!has(head, 0, 54) || !has(maxp, 0, 6) || !has(hhea, 0, 36)) {
        font = Font{};
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    font.head = tables.head;
    font.head_flags = read_u16(head, 16);
    font.units_per_em = read_u16(head, 18);
    const i32 location_format = read_i16(head, 50);
    font.glyph_count = read_u16(maxp, 4);
    if (font.units_per_em < 16 || font.units_per_em > 16384 || font.glyph_count == 0 || (location_format != 0 && location_format != 1)) {
        font = Font{};
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    font.long_offsets = location_format == 1;
    // The face's height: OS/2's typographic values when the font says to
    // use them (a flag of version 4 and later), else hhea's, else, when
    // hhea has none, OS/2's typographic and then its Windows values.
    font.ascender = read_i16(hhea, 4);
    font.descender = read_i16(hhea, 6);
    font.height = font.ascender - font.descender + read_i16(hhea, 8);
    const Span os2 = table_of(font, tables.os2);
    if (has(os2, 0, 78)) {
        const i32 typo_ascender = read_i16(os2, 68);
        const i32 typo_descender = read_i16(os2, 70);
        const i32 typo_gap = read_i16(os2, 72);
        const bool use_typographic = read_u16(os2, 0) >= 4 && (read_u16(os2, 62) & 0x80U) != 0;
        if (use_typographic) {
            font.ascender = typo_ascender;
            font.descender = typo_descender;
            font.height = typo_ascender - typo_descender + typo_gap;
        } else if (font.ascender == 0 && font.descender == 0) {
            if (typo_ascender != 0 || typo_descender != 0) {
                font.ascender = typo_ascender;
                font.descender = typo_descender;
                font.height = typo_ascender - typo_descender + typo_gap;
            } else {
                font.ascender = static_cast<i32>(read_u16(os2, 74));
                font.descender = -static_cast<i32>(read_u16(os2, 76));
                font.height = font.ascender - font.descender;
            }
        }
    }
    // Advances: as many as hhea says, no more than the table holds.
    const Span hmtx = table_of(font, tables.hmtx);
    const usize advances = read_u16(hhea, 34);
    font.hmtx = tables.hmtx;
    font.advance_count = static_cast<u32>(advances < hmtx.size / 4 ? advances : hmtx.size / 4);
    choose_character_map(whole, tables.cmap, font);
    if (tables.glyf.length != 0 && tables.loca.length != 0) {
        font.outlines = OutlineKind::truetype;
        font.loca = tables.loca;
        font.glyf = tables.glyf;
    } else if (tables.cff.length != 0) {
        font.cff = tables.cff;
        const Status opened = detail::open_cff(font);
        if (opened.failed()) {
            font = Font{};
            return opened;
        }
        font.outlines = OutlineKind::cff;
    }
    return Status::success();
}

u32 glyph_of(const Font& font, const u32 character) noexcept {
    const Span map = table_of(font, font.character_map);
    const u32 format = font.character_map_format;
    if (!map_readable(map, format)) return 0;
    u32 glyph = 0;
    if (format == 0) {
        glyph = character < 256 ? detail::read_u8(map, 6 + character) : 0;
    } else if (format == 4) {
        glyph = glyph_in_segments(map, character);
    } else if (format == 6) {
        const u32 first = read_u16(map, 6);
        const u32 count = read_u16(map, 8);
        glyph = character >= first && character - first < count ? read_u16(map, 10 + static_cast<usize>(character - first) * 2) : 0;
    } else {
        glyph = glyph_in_groups(map, character);
    }
    const u32 result = glyph < font.glyph_count ? glyph : 0;
    return result;
}

i32 advance_of(const Font& font, const u32 glyph) noexcept {
    const Span hmtx = table_of(font, font.hmtx);
    const usize count = font.advance_count;
    if (glyph >= font.glyph_count || count == 0 || !has(hmtx, 0, count * 4)) return 0;
    // A glyph past the last advance has the last one.
    const usize index = glyph < count ? glyph : count - 1;
    const i32 result = static_cast<i32>(read_u16(hmtx, index * 4));
    return result;
}

Status load_glyph(const Font& font, const u32 glyph, const FontLimits& limits, GlyphOutline& outline) noexcept {
    const Operation operation("stxlib.glyphs.load_glyph");
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    if (glyph >= font.glyph_count) return refuse_font(ErrorKind::invalid_argument, operation);
    if (font.outlines == OutlineKind::truetype) {
        const Status status = detail::load_truetype_glyph(font, glyph, 0, 0, limits, outline);
        return status;
    }
    if (font.outlines == OutlineKind::cff) {
        const Status status = detail::load_cff_glyph(font, glyph, limits, outline);
        return status;
    }
    return refuse_font(ErrorKind::invalid_argument, operation);
}

Status load_scaled_glyph(const Font& font, const u32 glyph, const i64 x_scale, const i64 y_scale, const FontLimits& limits,
                         GlyphOutline& outline) noexcept {
    const Operation operation("stxlib.glyphs.load_scaled_glyph");
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    if (glyph >= font.glyph_count || x_scale <= 0 || y_scale <= 0) return refuse_font(ErrorKind::invalid_argument, operation);
    if (font.outlines == OutlineKind::truetype) {
        const Status status = detail::load_truetype_glyph(font, glyph, x_scale, y_scale, limits, outline);
        return status;
    }
    if (font.outlines == OutlineKind::cff) {
        // A CFF glyph's points are whole font units before they are scaled.
        const Status status = detail::load_cff_glyph(font, glyph, limits, outline);
        if (status.succeeded()) scale_outline(outline, x_scale, y_scale);
        return status;
    }
    return refuse_font(ErrorKind::invalid_argument, operation);
}

i64 scale_of(const Font& font, const i64 size) noexcept {
    if (font.units_per_em == 0 || size <= 0) return 0;
    const i64 units = font.units_per_em;
    const i64 result = (size * 65536 + units / 2) / units;
    return result;
}

void scale_outline(GlyphOutline& outline, const i64 x_scale, const i64 y_scale) noexcept {
    const usize count = outline.points.count();
    Point* const points = outline.points.mutable_view().data();
    for (usize index = 0; index < count; ++index) {
        points[index].x = static_cast<i32>(detail::multiply_fixed(points[index].x, x_scale));
        points[index].y = static_cast<i32>(detail::multiply_fixed(points[index].y, y_scale));
    }
}

namespace {

// The point between two points, each coordinate rounded down.
[[nodiscard]] Point between(const Point a, const Point b) noexcept {
    const Point result{.x = static_cast<i32>((static_cast<i64>(a.x) + b.x) >> 1), .y = static_cast<i32>((static_cast<i64>(a.y) + b.y) >> 1)};
    return result;
}

// Segments being written.
struct Writer final {
    Vector<Segment>* segments{nullptr};
    Vector<Point>* points{nullptr};
    bool failed{false};
};

void write(Writer& writer, const Segment segment, const Point* const points, const usize count) noexcept {
    if ((*writer.segments).push_copy(segment).failed()) writer.failed = true;
    for (usize index = 0; index < count; ++index) {
        if ((*writer.points).push_copy(points[index]).failed()) writer.failed = true;
    }
}

} // namespace

Status to_segments(const GlyphOutline& outline, Vector<Segment>& segments, Vector<Point>& points) noexcept {
    const Operation operation("stxlib.glyphs.to_segments");
    segments.clear();
    points.clear();
    const usize point_count = outline.points.count();
    if (outline.tags.count() != point_count) return refuse_font(ErrorKind::invalid_argument, operation);
    const View<Point> all = outline.points.view();
    const View<PointTag> tags = outline.tags.view();
    const View<u32> ends = outline.contour_ends.view();
    Writer writer{.segments = &segments, .points = &points, .failed = false};
    usize first = 0;
    for (usize contour = 0; contour < ends.count(); ++contour) {
        const usize last = ends[contour];
        if (last < first || last >= point_count) return refuse_font(ErrorKind::invalid_argument, operation);
        if (tags[first] == PointTag::cubic) return refuse_font(ErrorKind::invalid_argument, operation);
        // Where the contour begins, and the points still to take: [next,
        // stop).
        Point start = all[first];
        usize next = first + 1;
        usize stop = last + 1;
        if (tags[first] == PointTag::quadratic) {
            next = first;
            if (tags[last] == PointTag::on) {
                start = all[last];
                stop = last;
            } else {
                start = between(all[first], all[last]);
            }
        }
        write(writer, Segment::move, &start, 1);
        while (next < stop) {
            const PointTag kind = tags[next];
            if (kind == PointTag::on) {
                write(writer, Segment::line, &all[next], 1);
                next = next + 1;
            } else if (kind == PointTag::quadratic) {
                // The curve ends at the next point if it is on the curve,
                // between this control point and the next if that is
                // another, and at the start when the contour is over.
                Point curve[2] = {all[next], start};
                next = next + 1;
                if (next < stop) {
                    if (tags[next] == PointTag::on) {
                        curve[1] = all[next];
                        next = next + 1;
                    } else if (tags[next] == PointTag::quadratic) {
                        curve[1] = between(curve[0], all[next]);
                    } else {
                        return refuse_font(ErrorKind::invalid_argument, operation);
                    }
                }
                write(writer, Segment::quadratic, curve, 2);
            } else {
                // Two cubic control points, then the end: the next point,
                // or the start when the contour is over.
                if (next + 1 >= stop || tags[next + 1] != PointTag::cubic) return refuse_font(ErrorKind::invalid_argument, operation);
                Point curve[3] = {all[next], all[next + 1], start};
                next = next + 2;
                if (next < stop) {
                    curve[2] = all[next];
                    next = next + 1;
                }
                write(writer, Segment::cubic, curve, 3);
            }
        }
        first = last + 1;
    }
    if (writer.failed) return refuse_font(ErrorKind::out_of_memory, operation);
    return Status::success();
}

} // namespace stxlib::glyphs
