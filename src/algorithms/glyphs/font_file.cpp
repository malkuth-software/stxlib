// stxlib::glyphs: a font file, read (include/stxlib/font_file.hpp). The
// table directory, the face's numbers, the character map, advances, the
// scale and the outline as segments. After ttf-parser 0.25.1 (lib.rs,
// tables/head.rs, hhea.rs, maxp.rs, hmtx.rs, os2.rs, cmap), in integers
// and with every range checked where it is read.

#include "stxlib/font_file.hpp"

#include "algorithms/glyphs/font_reader.hpp"
#include "algorithms/glyphs/glyph_checks.hpp"

namespace stxlib::glyphs {

namespace detail {

OperationStatus refuse_font(const ErrorKind kind, const Operation operation) noexcept {
    const OperationStatus result = OperationStatus::failure(ErrorRecord::make(kind, operation));
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
    FontTable post{};
    FontTable gsub{};
    FontTable gpos{};
    FontTable gdef{};
    FontTable kern{};
    FontTable name{};
};

// The character maps this reads, and which is taken: a map of all of
// Unicode before one of its first 65,536 characters, Windows' before
// Unicode's of the same reach, the later of a kind in the table before the
// earlier (as FreeType). With none of them, a symbol map (3, 0), and then
// a Mac Roman one (1, 0) of format 0 or 6, as HarfBuzz falls back.
inline constexpr u32 platform_unicode = 0;
inline constexpr u32 platform_mac = 1;
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

// The last record of the platform and encoding whose map is readable (of
// format 0 or 6 when `old_formats`), taken for the font; false if none is.
[[nodiscard]] bool take_map(const Span& cmap, const FontTable& table, const Preference wanted, const bool old_formats, const u32 kind,
                            Font& font) noexcept {
    const usize records = read_u16(cmap, 2);
    for (usize turn = records; turn > 0; --turn) {
        const usize record = 4 + (turn - 1) * 8;
        if (read_u16(cmap, record) != wanted.platform || read_u16(cmap, record + 2) != wanted.encoding) continue;
        const usize offset = read_u32(cmap, record + 4);
        if (!has(cmap, offset, 2)) continue;
        const Span map = part(cmap, offset, cmap.size - offset);
        const u32 format = read_u16(map, 0);
        if (!map_readable(map, format) || (old_formats && format != 0 && format != 6)) continue;
        font.character_map = FontTable{.offset = static_cast<u32>(table.offset + offset), .length = static_cast<u32>(map.size)};
        font.character_map_format = format;
        font.character_map_kind = kind;
        return true;
    }
    return false;
}

// Whether a format 4 map's segments end in rising order (searched by
// halving), or not (searched in order).
[[nodiscard]] bool segments_rise(const Span& map) noexcept {
    const usize segments = read_u16(map, 6) / 2;
    bool rising = true;
    for (usize index = 1; index < segments && rising; ++index) rising = read_u16(map, 14 + (index - 1) * 2) < read_u16(map, 14 + index * 2);
    return rising;
}

void choose_character_map(const FontTable& table, Font& font) noexcept {
    const Span cmap = table_of(font, table);
    if (!has(cmap, 0, 4)) return;
    const usize records = read_u16(cmap, 2);
    if (!has(cmap, 4, records * 8)) return;
    bool taken = false;
    for (const Preference& wanted : preferences) {
        if (!taken) taken = take_map(cmap, table, wanted, false, character_map_unicode, font);
    }
    if (!taken) taken = take_map(cmap, table, Preference{.platform = platform_windows, .encoding = 0}, false, character_map_symbol, font);
    if (!taken) taken = take_map(cmap, table, Preference{.platform = platform_mac, .encoding = 0}, true, character_map_mac_roman, font);
    if (taken && font.character_map_format == 4) font.character_map_unsorted = !segments_rise(table_of(font, font.character_map));
}

// The list of variation sequences: the character map of platform 0,
// encoding 5, format 14, when its header and its records are inside it.
void choose_variation_map(const FontTable& table, Font& font) noexcept {
    const Span cmap = table_of(font, table);
    if (!has(cmap, 0, 4)) return;
    const usize records = read_u16(cmap, 2);
    if (!has(cmap, 4, records * 8)) return;
    for (usize index = 0; index < records; ++index) {
        const usize record = 4 + index * 8;
        if (read_u16(cmap, record) != platform_unicode || read_u16(cmap, record + 2) != 5) continue;
        const usize offset = read_u32(cmap, record + 4);
        if (!has(cmap, offset, 10) || read_u16(cmap, offset) != 14) continue;
        const Span map = part(cmap, offset, cmap.size - offset);
        const usize selectors = read_u32(map, 6);
        if (selectors > (map.size - 10) / 11) continue;
        font.variation_map = FontTable{.offset = static_cast<u32>(table.offset + offset), .length = static_cast<u32>(map.size)};
        return;
    }
}

// Format 4: segments of the first 65,536 characters, by a search for the
// first segment that ends at or after the character.
// The glyph of a character by segment `segment` of a format 4 map of
// `segments` segments: 0 if the segment begins after it.
[[nodiscard]] u32 glyph_in_segment(const Span& map, const usize segments, const usize segment, const u32 character) noexcept {
    const usize ends = 14;
    const usize starts = ends + segments * 2 + 2;
    const usize deltas = starts + segments * 2;
    const usize offsets = deltas + segments * 2;
    const u32 start = read_u16(map, starts + segment * 2);
    if (start > character) return 0;
    const u32 delta = read_u16(map, deltas + segment * 2);
    const u32 range_offset = read_u16(map, offsets + segment * 2);
    if (range_offset == 0) {
        const u32 result = (character + delta) & 0xFFFFU;
        return result;
    }
    if (range_offset == 0xFFFFU) return 0;
    // The offset is from its own place in the table.
    const usize place = offsets + segment * 2 + range_offset + static_cast<usize>(character - start) * 2;
    if (!has(map, place, 2)) return 0;
    const u32 listed = read_u16(map, place);
    if (listed == 0) return 0;
    const u32 result = (listed + delta) & 0xFFFFU;
    return result;
}

// A map whose segments are not in order (FreeType reads one so): the
// first segment, in the table's order, that holds the character.
[[nodiscard]] u32 glyph_in_unsorted_segments(const Span& map, const u32 character) noexcept {
    if (character > 0xFFFFU) return 0;
    const usize segments = read_u16(map, 6) / 2;
    const usize starts = 14 + segments * 2 + 2;
    for (usize segment = 0; segment < segments; ++segment) {
        if (read_u16(map, starts + segment * 2) <= character && character <= read_u16(map, 14 + segment * 2)) {
            const u32 result = glyph_in_segment(map, segments, segment, character);
            return result;
        }
    }
    return 0;
}

[[nodiscard]] u32 glyph_in_segments(const Span& map, const u32 character) noexcept {
    if (character > 0xFFFFU) return 0;
    const usize segments = read_u16(map, 6) / 2;
    const usize ends = 14;
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
    const u32 result = glyph_in_segment(map, segments, low, character);
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

// OS/2's numbers as FreeType reads them: the table is taken whole for its
// version or not at all (os2_version 0xFFFF).
void read_os2_numbers(const Span& os2, Font& font) noexcept {
    if (!has(os2, 0, 78)) return;
    const u32 version = read_u16(os2, 0);
    usize needed = 78;
    if (version >= 1) needed = 86;
    if (version >= 2) needed = 96;
    if (version >= 5) needed = 100;
    if (!has(os2, 0, needed)) return;
    font.os2_version = version;
    font.weight_class = read_u16(os2, 4);
    font.width_class = read_u16(os2, 6);
    font.selection = read_u16(os2, 62);
    font.typo_ascender = read_i16(os2, 68);
    font.typo_descender = read_i16(os2, 70);
    font.typo_line_gap = read_i16(os2, 72);
    font.win_ascent = read_u16(os2, 74);
    font.win_descent = read_u16(os2, 76);
}

} // namespace

u32 face_count(const View<u8> bytes) noexcept {
    const Span whole = span_of(bytes);
    if (!has(whole, 0, 12)) return 0;
    const u32 magic = read_u32(whole, 0);
    if (magic == magic_truetype || magic == magic_true || magic == magic_opentype) return 1;
    if (magic != magic_collection) return 0;
    const u32 faces = read_u32(whole, 8);
    if (faces == 0 || !has(whole, 12, static_cast<usize>(faces) * 4)) return 0;
    return faces;
}

OperationStatus open_font(const View<u8> bytes, const u32 face, Font& font) noexcept {
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
        // A table whose range is not in the file is as if it were absent;
        // but hmtx that begins in the file and runs past its end is kept
        // to the end, as FreeType keeps it (advance_count takes no more
        // advances than it then holds).
        FontTable kept = table;
        if (name == tag('h', 'm', 't', 'x') && table.offset <= whole.size && !has(whole, table.offset, table.length)) {
            kept.length = static_cast<u32>(whole.size - table.offset);
        }
        if (!has(whole, kept.offset, kept.length)) continue;
        if (name == tag('h', 'e', 'a', 'd')) tables.head = table;
        if (name == tag('m', 'a', 'x', 'p')) tables.maxp = table;
        if (name == tag('h', 'h', 'e', 'a')) tables.hhea = table;
        if (name == tag('h', 'm', 't', 'x')) tables.hmtx = kept;
        if (name == tag('O', 'S', '/', '2')) tables.os2 = table;
        if (name == tag('G', 'S', 'U', 'B')) tables.gsub = table;
        if (name == tag('G', 'P', 'O', 'S')) tables.gpos = table;
        if (name == tag('G', 'D', 'E', 'F')) tables.gdef = table;
        if (name == tag('k', 'e', 'r', 'n')) tables.kern = table;
        if (name == tag('c', 'm', 'a', 'p')) tables.cmap = table;
        if (name == tag('l', 'o', 'c', 'a')) tables.loca = table;
        if (name == tag('g', 'l', 'y', 'f')) tables.glyf = table;
        if (name == tag('C', 'F', 'F', ' ')) tables.cff = table;
        if (name == tag('p', 'o', 's', 't')) tables.post = table;
        if (name == tag('n', 'a', 'm', 'e')) tables.name = table;
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
    font.hhea_ascender = font.ascender;
    font.hhea_descender = font.descender;
    font.hhea_line_gap = read_i16(hhea, 8);
    const Span os2 = table_of(font, tables.os2);
    read_os2_numbers(os2, font);
    // The name table, when its header and its records are inside it.
    const Span names = table_of(font, tables.name);
    if (has(names, 0, 6) && has(names, 6, static_cast<usize>(read_u16(names, 2)) * 12)) font.names = tables.name;
    font.italic = (read_u16(head, 44) & 0x2U) != 0;
    const Span post = table_of(font, tables.post);
    font.fixed_pitch = has(post, 0, 16) && read_u32(post, 12) != 0;
    if (has(os2, 0, 64)) font.italic = (read_u16(os2, 62) & 0x201U) != 0;
    if (has(os2, 0, 90) && read_u16(os2, 0) >= 2) {
        font.x_height = read_i16(os2, 86);
        font.cap_height = read_i16(os2, 88);
    }
    font.substitutions = tables.gsub;
    font.positions = tables.gpos;
    font.definitions = tables.gdef;
    font.kerning = tables.kern;
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
    choose_character_map(tables.cmap, font);
    choose_variation_map(tables.cmap, font);
    if (tables.glyf.length != 0 && tables.loca.length != 0) {
        font.outlines = OutlineKind::truetype;
        font.loca = tables.loca;
        font.glyf = tables.glyf;
    } else if (tables.cff.length != 0) {
        font.cff = tables.cff;
        const OperationStatus opened = detail::open_cff(font);
        if (opened.failed()) {
            font = Font{};
            return opened;
        }
        font.outlines = OutlineKind::cff;
    }
    return OperationStatus::success();
}

namespace {

// The glyph a map gives a code, of whatever kind the map is.
[[nodiscard]] u32 glyph_by_code(const Font& font, const Span& map, const u32 format, const u32 code) noexcept {
    u32 glyph = 0;
    if (format == 0) {
        glyph = code < 256 ? detail::read_u8(map, 6 + code) : 0;
    } else if (format == 4) {
        glyph = font.character_map_unsorted ? glyph_in_unsorted_segments(map, code) : glyph_in_segments(map, code);
    } else if (format == 6) {
        const u32 first = read_u16(map, 6);
        const u32 count = read_u16(map, 8);
        glyph = code >= first && code - first < count ? read_u16(map, 10 + static_cast<usize>(code - first) * 2) : 0;
    } else {
        glyph = glyph_in_groups(map, code);
    }
    return glyph;
}

} // namespace

u32 glyph_of(const Font& font, const u32 character) noexcept {
    const Span map = table_of(font, font.character_map);
    const u32 format = font.character_map_format;
    if (!map_readable(map, format)) return 0;
    u32 glyph = 0;
    if (font.character_map_kind == character_map_symbol) {
        // A symbol map's codes: the character, then, for the first 256,
        // the character in the private area where symbol fonts put them.
        glyph = glyph_by_code(font, map, format, character);
        if (glyph == 0 && character < 0x100) glyph = glyph_by_code(font, map, format, 0xF000 + character);
    } else if (font.character_map_kind == character_map_mac_roman) {
        // Mac Roman's codes are ASCII's below 128; above, not read.
        glyph = character < 0x80 ? glyph_by_code(font, map, format, character) : 0;
    } else {
        glyph = glyph_by_code(font, map, format, character);
    }
    const u32 result = glyph < font.glyph_count ? glyph : 0;
    return result;
}

bool next_stretch(const Font& font, const u32 from, MappedStretch& stretch) noexcept {
    const Span map = table_of(font, font.character_map);
    const u32 format = font.character_map_format;
    stretch = MappedStretch{};
    // Stretches are of Unicode characters: a symbol or Mac Roman map has
    // none.
    if (!map_readable(map, format) || font.character_map_kind != character_map_unicode) return false;
    bool found = false;
    if (format == 0) {
        found = from < 256;
        stretch.last = 255;
    } else if (format == 4) {
        if (from > 0xFFFFU) return false;
        // The first segment that ends at or after `from`.
        const usize segments = read_u16(map, 6) / 2;
        const usize ends = 14;
        const usize starts = ends + segments * 2 + 2;
        usize low = 0;
        usize high = segments;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            if (read_u16(map, ends + middle * 2) < from) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (low < segments) {
            found = true;
            stretch.first = read_u16(map, starts + low * 2);
            stretch.last = read_u16(map, ends + low * 2);
            stretch.place = static_cast<u32>(low);
        }
    } else if (format == 6) {
        // An empty table covers nothing.
        const u32 first = read_u16(map, 6);
        const u32 count = read_u16(map, 8);
        found = count != 0 && from < first + count;
        if (found) {
            stretch.first = first;
            stretch.last = first + count - 1;
        }
    } else {
        // The first group that ends at or after `from`.
        usize low = 0;
        usize high = read_u32(map, 12);
        const usize groups = high;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            if (read_u32(map, 16 + middle * 12 + 4) < from) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (low < groups) {
            found = true;
            stretch.first = read_u32(map, 16 + low * 12);
            stretch.last = read_u32(map, 16 + low * 12 + 4);
            stretch.place = static_cast<u32>(low);
        }
    }
    // A stretch that ends before it begins (a map that is not in order)
    // holds nothing.
    if (!found || stretch.last < stretch.first || stretch.last < from) {
        stretch = MappedStretch{};
        return false;
    }
    return true;
}

u32 glyph_in_stretch(const Font& font, const MappedStretch& stretch, const u32 character) noexcept {
    const Span map = table_of(font, font.character_map);
    const u32 format = font.character_map_format;
    if (!map_readable(map, format) || font.character_map_kind != character_map_unicode) return 0;
    if (character < stretch.first || character > stretch.last) return 0;
    u32 glyph = 0;
    if (format == 0) {
        glyph = character < 256 ? detail::read_u8(map, 6 + character) : 0;
    } else if (format == 4) {
        const usize segments = read_u16(map, 6) / 2;
        if (stretch.place < segments && character <= 0xFFFFU) glyph = glyph_in_segment(map, segments, stretch.place, character);
    } else if (format == 6) {
        const u32 first = read_u16(map, 6);
        const u32 count = read_u16(map, 8);
        glyph = character >= first && character - first < count ? read_u16(map, 10 + static_cast<usize>(character - first) * 2) : 0;
    } else {
        const usize groups = read_u32(map, 12);
        if (stretch.place < groups) {
            const usize group = 16 + static_cast<usize>(stretch.place) * 12;
            const u32 first = read_u32(map, group);
            const u32 last = read_u32(map, group + 4);
            if (character >= first && character <= last) {
                const u64 number = static_cast<u64>(read_u32(map, group + 8)) + (character - first);
                glyph = number <= 0xFFFFU ? static_cast<u32>(number) : 0;
            }
        }
    }
    const u32 result = glyph < font.glyph_count ? glyph : 0;
    return result;
}

bool has_feature(const Font& font, const u32 wanted) noexcept {
    const Span table = table_of(font, font.substitutions);
    // The header: version, then the offsets of the lists of scripts,
    // features and lookups. A feature record is its tag and an offset.
    if (!has(table, 0, 10)) return false;
    const usize list = read_u16(table, 6);
    if (list == 0 || !has(table, list, 2)) return false;
    const usize count = read_u16(table, list);
    if (!has(table, list + 2, count * 6)) return false;
    bool found = false;
    for (usize index = 0; index < count && !found; ++index) found = read_u32(table, list + 2 + index * 6) == wanted;
    return found;
}

u32 variation_glyph(const Font& font, const u32 character, const u32 selector, bool& use_default) noexcept {
    use_default = false;
    const Span map = table_of(font, font.variation_map);
    if (!has(map, 0, 10) || character > 0xFFFFFFU) return 0;
    const usize selectors = read_u32(map, 6);
    if (selectors > (map.size - 10) / 11) return 0;
    // The selectors in order: the selector (24 bits), then the offsets of
    // its two lists, each 0 for none.
    usize low = 0;
    usize high = selectors;
    usize record = 0;
    while (low < high && record == 0) {
        const usize middle = low + (high - low) / 2;
        const u32 listed = detail::read_u24(map, 10 + middle * 11);
        if (listed < selector) {
            low = middle + 1;
        } else if (listed > selector) {
            high = middle;
        } else {
            record = 10 + middle * 11;
        }
    }
    if (record == 0) return 0;
    // The sequences that name the character's own glyph: ranges in order,
    // a first character (24 bits) and how many more follow it.
    const usize defaults = read_u32(map, record + 3);
    if (defaults != 0 && has(map, defaults, 4)) {
        const usize ranges = read_u32(map, defaults);
        if (ranges <= (map.size - defaults - 4) / 4) {
            low = 0;
            high = ranges;
            while (low < high && !use_default) {
                const usize middle = low + (high - low) / 2;
                const u32 first = detail::read_u24(map, defaults + 4 + middle * 4);
                const u32 last = first + detail::read_u8(map, defaults + 4 + middle * 4 + 3);
                if (last < character) {
                    low = middle + 1;
                } else if (first > character) {
                    high = middle;
                } else {
                    use_default = true;
                }
            }
        }
    }
    if (use_default) return 0;
    // The sequences with a glyph of their own: characters in order, each
    // with its glyph.
    const usize others = read_u32(map, record + 7);
    u32 glyph = 0;
    if (others != 0 && has(map, others, 4)) {
        const usize count = read_u32(map, others);
        if (count <= (map.size - others - 4) / 5) {
            low = 0;
            high = count;
            bool found = false;
            while (low < high && !found) {
                const usize middle = low + (high - low) / 2;
                const u32 listed = detail::read_u24(map, others + 4 + middle * 5);
                if (listed < character) {
                    low = middle + 1;
                } else if (listed > character) {
                    high = middle;
                } else {
                    glyph = read_u16(map, others + 4 + middle * 5 + 3);
                    found = true;
                }
            }
        }
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

bool glyph_checks_enabled() noexcept {
#if defined(STXLIB_GLYPHS_CHECKS)
    return true;
#else
    return false;
#endif
}

namespace detail {

bool outline_whole(const GlyphOutline& outline) noexcept {
    const usize count = outline.points.count();
    bool whole = outline.tags.count() == count;
    usize before = 0;
    for (usize contour = 0; whole && contour < outline.contour_ends.count(); ++contour) {
        const usize last = outline.contour_ends[contour];
        whole = last < count && (contour == 0 || last > before);
        before = last;
    }
    if (whole && outline.contour_ends.count() > 0) whole = before + 1 == count;
    if (whole && outline.contour_ends.count() == 0) whole = count == 0;
    return whole;
}

} // namespace detail

OperationStatus load_glyph(const Font& font, const u32 glyph, const FontLimits& limits,
                           GlyphOutline& outline) noexcept {
    const Operation operation("stxlib.glyphs.load_glyph");
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    if (glyph >= font.glyph_count) return refuse_font(ErrorKind::invalid_argument, operation);
    if (font.outlines == OutlineKind::truetype) {
        const OperationStatus status = detail::load_truetype_glyph(font, glyph, 0, 0, limits, outline);
        STXLIB_GLYPHS_CHECK(status.failed() || (detail::outline_whole(outline) && outline.points.count() <= limits.points &&
                                                outline.contour_ends.count() <= limits.contours),
                            "load_glyph: the outline is not whole");
        return status;
    }
    if (font.outlines == OutlineKind::cff) {
        const OperationStatus status = detail::load_cff_glyph(font, glyph, limits, outline);
        STXLIB_GLYPHS_CHECK(status.failed() || (detail::outline_whole(outline) && outline.points.count() <= limits.points &&
                                                outline.contour_ends.count() <= limits.contours),
                            "load_glyph: the outline is not whole");
        return status;
    }
    return refuse_font(ErrorKind::invalid_argument, operation);
}

OperationStatus load_scaled_glyph(const Font& font, const u32 glyph, const i64 x_scale, const i64 y_scale,
                                  const FontLimits& limits, GlyphOutline& outline) noexcept {
    const Operation operation("stxlib.glyphs.load_scaled_glyph");
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    if (glyph >= font.glyph_count || x_scale <= 0 || y_scale <= 0) return refuse_font(ErrorKind::invalid_argument, operation);
    if (font.outlines == OutlineKind::truetype) {
        const OperationStatus status = detail::load_truetype_glyph(font, glyph, x_scale, y_scale, limits, outline);
        STXLIB_GLYPHS_CHECK(status.failed() || detail::outline_whole(outline), "load_scaled_glyph: the outline is not whole");
        return status;
    }
    if (font.outlines == OutlineKind::cff) {
        // A CFF glyph's points are whole font units before they are scaled.
        const OperationStatus status = detail::load_cff_glyph(font, glyph, limits, outline);
        if (status.failed()) return status;
        return scale_outline(outline, x_scale, y_scale);
    }
    return refuse_font(ErrorKind::invalid_argument, operation);
}

i64 scale_of(const Font& font, const i64 size) noexcept {
    if (font.units_per_em == 0 || size <= 0) return 0;
    const i64 units = font.units_per_em;
    const i64 result = (size * 65536 + units / 2) / units;
    return result;
}

OperationStatus scale_outline(GlyphOutline& outline, const i64 x_scale, const i64 y_scale) noexcept {
    const Operation operation("stxlib.glyphs.scale_outline");
    const i64 most_scale = static_cast<i64>(1) << 32;
    if (x_scale <= 0 || y_scale <= 0 || x_scale > most_scale || y_scale > most_scale) {
        return detail::refuse_font(ErrorKind::invalid_argument, operation);
    }
    // A 32-bit coordinate times a scale of no more than 2^32 is inside 64
    // bits; each result is tested before any is written.
    const usize count = outline.points.count();
    Point* const points = outline.points.mutable_view().data();
    bool near = true;
    for (usize index = 0; index < count && near; ++index) {
        const i64 x = detail::multiply_fixed(points[index].x, x_scale);
        const i64 y = detail::multiply_fixed(points[index].y, y_scale);
        near = x >= -detail::farthest_point && x <= detail::farthest_point && y >= -detail::farthest_point && y <= detail::farthest_point;
    }
    if (!near) return detail::refuse_font(ErrorKind::invalid_argument, operation);
    for (usize index = 0; index < count; ++index) {
        points[index].x = static_cast<i32>(detail::multiply_fixed(points[index].x, x_scale));
        points[index].y = static_cast<i32>(detail::multiply_fixed(points[index].y, y_scale));
    }
    return OperationStatus::success();
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

OperationStatus to_segments(const GlyphOutline& outline, Vector<Segment>& segments, Vector<Point>& points) noexcept {
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
    return OperationStatus::success();
}

} // namespace stxlib::glyphs
