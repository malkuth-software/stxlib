#pragma once

// Fonts built in memory, byte by byte, for the tests of stxlib::glyphs
// (font_file_tests.cpp, glyph_hinter_tests.cpp): the tables of a TrueType
// font, simple and composite glyphs with every way a flag and a coordinate
// are stored, and character maps of formats 0, 4, 6 and 12.

#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

#include <string>
#include <vector>

namespace font_builder {

using stxlib::i32;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

using Bytes = std::vector<u8>;

inline void put8(Bytes& bytes, const u32 value) { bytes.push_back(static_cast<u8>(value & 0xFFU)); }
inline void put16(Bytes& bytes, const i32 value) {
    put8(bytes, static_cast<u32>(value) >> 8);
    put8(bytes, static_cast<u32>(value));
}
inline void put32(Bytes& bytes, const u32 value) {
    put16(bytes, static_cast<i32>(value >> 16));
    put16(bytes, static_cast<i32>(value & 0xFFFFU));
}
inline void append(Bytes& bytes, const Bytes& more) { bytes.insert(bytes.end(), more.begin(), more.end()); }

struct NamedTable final {
    std::string name{};
    Bytes bytes{};
};

// A font file of the tables given.
[[nodiscard]] inline Bytes font_of(const u32 magic, const std::vector<NamedTable>& tables) {
    Bytes font{};
    put32(font, magic);
    put16(font, static_cast<i32>(tables.size()));
    put16(font, 0);
    put16(font, 0);
    put16(font, 0);
    usize offset = 12 + tables.size() * 16;
    for (const NamedTable& table : tables) {
        for (const char letter : table.name) put8(font, static_cast<u8>(letter));
        put32(font, 0);
        put32(font, static_cast<u32>(offset));
        put32(font, static_cast<u32>(table.bytes.size()));
        offset = offset + ((table.bytes.size() + 3) / 4) * 4;
    }
    for (const NamedTable& table : tables) {
        append(font, table.bytes);
        while (font.size() % 4 != 0) put8(font, 0);
    }
    return font;
}

[[nodiscard]] inline Bytes head_table(const u32 units, const bool long_offsets) {
    Bytes head(54, 0);
    head[16] = 0;
    head[17] = 0;
    head[18] = static_cast<u8>(units >> 8);
    head[19] = static_cast<u8>(units & 0xFFU);
    head[51] = long_offsets ? 1 : 0;
    return head;
}

[[nodiscard]] inline Bytes maxp_table(const u32 glyph_count) {
    Bytes maxp{};
    put32(maxp, 0x00005000U);
    put16(maxp, static_cast<i32>(glyph_count));
    return maxp;
}

[[nodiscard]] inline Bytes hhea_table(const i32 ascender, const i32 descender, const i32 gap, const u32 advances) {
    Bytes hhea(36, 0);
    Bytes numbers{};
    put16(numbers, ascender);
    put16(numbers, descender);
    put16(numbers, gap);
    for (usize index = 0; index < 6; ++index) hhea[4 + index] = numbers[index];
    hhea[34] = static_cast<u8>(advances >> 8);
    hhea[35] = static_cast<u8>(advances & 0xFFU);
    return hhea;
}

[[nodiscard]] inline Bytes os2_table(const u32 version, const u32 selection, const i32 typo_ascender, const i32 typo_descender, const i32 typo_gap,
                              const u32 win_ascent, const u32 win_descent) {
    Bytes os2(78, 0);
    Bytes numbers{};
    put16(numbers, typo_ascender);
    put16(numbers, typo_descender);
    put16(numbers, typo_gap);
    put16(numbers, static_cast<i32>(win_ascent));
    put16(numbers, static_cast<i32>(win_descent));
    os2[0] = static_cast<u8>(version >> 8);
    os2[1] = static_cast<u8>(version & 0xFFU);
    os2[62] = static_cast<u8>(selection >> 8);
    os2[63] = static_cast<u8>(selection & 0xFFU);
    for (usize index = 0; index < 10; ++index) os2[68 + index] = numbers[index];
    return os2;
}

struct Spot final {
    i32 x{0};
    i32 y{0};
    bool on{true};
};

// A simple glyph: its header, contour ends, two bytes of instructions to be
// stepped over, flags with repeats, and coordinates each in the shortest
// form.
[[nodiscard]] inline Bytes simple_glyph(const std::vector<std::vector<Spot>>& contours) {
    std::vector<Spot> all{};
    for (const std::vector<Spot>& contour : contours) all.insert(all.end(), contour.begin(), contour.end());
    i32 x_low = all[0].x;
    i32 x_high = all[0].x;
    i32 y_low = all[0].y;
    i32 y_high = all[0].y;
    for (const Spot& spot : all) {
        x_low = spot.x < x_low ? spot.x : x_low;
        x_high = spot.x > x_high ? spot.x : x_high;
        y_low = spot.y < y_low ? spot.y : y_low;
        y_high = spot.y > y_high ? spot.y : y_high;
    }
    Bytes glyph{};
    put16(glyph, static_cast<i32>(contours.size()));
    put16(glyph, x_low);
    put16(glyph, y_low);
    put16(glyph, x_high);
    put16(glyph, y_high);
    usize end = 0;
    for (const std::vector<Spot>& contour : contours) {
        end = end + contour.size();
        put16(glyph, static_cast<i32>(end - 1));
    }
    put16(glyph, 2);
    put8(glyph, 0xB0);
    put8(glyph, 0x00);
    std::vector<u32> flags{};
    Bytes xs{};
    Bytes ys{};
    i32 x = 0;
    i32 y = 0;
    for (const Spot& spot : all) {
        u32 flag = spot.on ? 0x01U : 0U;
        const i32 dx = spot.x - x;
        const i32 dy = spot.y - y;
        if (dx == 0) {
            flag = flag | 0x10U;
        } else if (dx > -256 && dx < 256) {
            flag = flag | 0x02U | (dx > 0 ? 0x10U : 0U);
            put8(xs, static_cast<u32>(dx < 0 ? -dx : dx));
        } else {
            put16(xs, dx);
        }
        if (dy == 0) {
            flag = flag | 0x20U;
        } else if (dy > -256 && dy < 256) {
            flag = flag | 0x04U | (dy > 0 ? 0x20U : 0U);
            put8(ys, static_cast<u32>(dy < 0 ? -dy : dy));
        } else {
            put16(ys, dy);
        }
        flags.push_back(flag);
        x = spot.x;
        y = spot.y;
    }
    for (usize index = 0; index < flags.size();) {
        usize run = 1;
        while (index + run < flags.size() && flags[index + run] == flags[index] && run < 256) run = run + 1;
        if (run > 1) {
            put8(glyph, flags[index] | 0x08U);
            put8(glyph, static_cast<u32>(run - 1));
        } else {
            put8(glyph, flags[index]);
        }
        index = index + run;
    }
    append(glyph, xs);
    append(glyph, ys);
    return glyph;
}

struct Part final {
    u32 flags{0};
    u32 glyph{0};
    i32 first{0};
    i32 second{0};
    std::vector<i32> scale{};
};

// A composite glyph of the parts given; `more` is set here.
[[nodiscard]] inline Bytes composite_glyph(const i32 x_low, const std::vector<Part>& parts) {
    Bytes glyph{};
    put16(glyph, -1);
    put16(glyph, x_low);
    put16(glyph, 0);
    put16(glyph, 0);
    put16(glyph, 0);
    for (usize index = 0; index < parts.size(); ++index) {
        const Part& part = parts[index];
        const u32 flags = part.flags | (index + 1 < parts.size() ? 0x20U : 0U);
        put16(glyph, static_cast<i32>(flags));
        put16(glyph, static_cast<i32>(part.glyph));
        if ((flags & 0x01U) != 0) {
            put16(glyph, part.first);
            put16(glyph, part.second);
        } else {
            put8(glyph, static_cast<u32>(part.first));
            put8(glyph, static_cast<u32>(part.second));
        }
        for (const i32 number : part.scale) put16(glyph, number);
    }
    return glyph;
}

struct Metric final {
    u32 advance{0};
    i32 bearing{0};
};

// glyf, loca and hmtx of the glyphs given; `advances` of them have an
// advance of their own.
inline void glyph_tables(const std::vector<Bytes>& glyph_bytes, const std::vector<Metric>& metrics, const usize advances, const bool long_offsets,
                  Bytes& glyf, Bytes& loca, Bytes& hmtx) {
    for (const Bytes& glyph : glyph_bytes) {
        if (long_offsets) {
            put32(loca, static_cast<u32>(glyf.size()));
        } else {
            put16(loca, static_cast<i32>(glyf.size() / 2));
        }
        append(glyf, glyph);
        while (glyf.size() % 2 != 0) put8(glyf, 0);
    }
    if (long_offsets) {
        put32(loca, static_cast<u32>(glyf.size()));
    } else {
        put16(loca, static_cast<i32>(glyf.size() / 2));
    }
    for (usize index = 0; index < metrics.size(); ++index) {
        if (index < advances) put16(hmtx, static_cast<i32>(metrics[index].advance));
        put16(hmtx, metrics[index].bearing);
    }
}

[[nodiscard]] inline stxlib::View<u8> view_of(const Bytes& bytes) {
    const stxlib::View<u8> result(bytes.data(), bytes.size());
    return result;
}

// ---- character maps ------------------------------------------------------------------------

struct Segment4 final {
    u32 first{0};
    u32 last{0};
    i32 delta{0};
    std::vector<u32> listed{};
};

[[nodiscard]] inline Bytes map_format_4(const std::vector<Segment4>& segments) {
    Bytes map{};
    const usize count = segments.size();
    put16(map, 4);
    put16(map, 0);
    put16(map, 0);
    put16(map, static_cast<i32>(count * 2));
    put16(map, 0);
    put16(map, 0);
    put16(map, 0);
    for (const Segment4& segment : segments) put16(map, static_cast<i32>(segment.last));
    put16(map, 0);
    for (const Segment4& segment : segments) put16(map, static_cast<i32>(segment.first));
    for (const Segment4& segment : segments) put16(map, segment.delta);
    // Listed glyphs follow the offsets; an offset is from its own place.
    usize listed_before = 0;
    for (usize index = 0; index < count; ++index) {
        if (segments[index].listed.empty()) {
            put16(map, 0);
        } else {
            put16(map, static_cast<i32>((count - index) * 2 + listed_before * 2));
            listed_before = listed_before + segments[index].listed.size();
        }
    }
    for (const Segment4& segment : segments) {
        for (const u32 glyph : segment.listed) put16(map, static_cast<i32>(glyph));
    }
    const usize length = map.size();
    map[2] = static_cast<u8>(length >> 8);
    map[3] = static_cast<u8>(length & 0xFFU);
    return map;
}

struct Group12 final {
    u32 first{0};
    u32 last{0};
    u32 glyph{0};
};

[[nodiscard]] inline Bytes map_format_12(const std::vector<Group12>& groups) {
    Bytes map{};
    put16(map, 12);
    put16(map, 0);
    put32(map, static_cast<u32>(16 + groups.size() * 12));
    put32(map, 0);
    put32(map, static_cast<u32>(groups.size()));
    for (const Group12& group : groups) {
        put32(map, group.first);
        put32(map, group.last);
        put32(map, group.glyph);
    }
    return map;
}

[[nodiscard]] inline Bytes map_format_6(const u32 first, const std::vector<u32>& listed) {
    Bytes map{};
    put16(map, 6);
    put16(map, static_cast<i32>(10 + listed.size() * 2));
    put16(map, 0);
    put16(map, static_cast<i32>(first));
    put16(map, static_cast<i32>(listed.size()));
    for (const u32 glyph : listed) put16(map, static_cast<i32>(glyph));
    return map;
}

[[nodiscard]] inline Bytes map_format_0(const u32 character, const u32 glyph) {
    Bytes map{};
    put16(map, 0);
    put16(map, 262);
    put16(map, 0);
    for (u32 index = 0; index < 256; ++index) put8(map, index == character ? glyph : 0);
    return map;
}

struct MapRecord final {
    u32 platform{0};
    u32 encoding{0};
    Bytes map{};
};

[[nodiscard]] inline Bytes cmap_table(const std::vector<MapRecord>& records) {
    Bytes cmap{};
    put16(cmap, 0);
    put16(cmap, static_cast<i32>(records.size()));
    usize offset = 4 + records.size() * 8;
    for (const MapRecord& record : records) {
        put16(cmap, static_cast<i32>(record.platform));
        put16(cmap, static_cast<i32>(record.encoding));
        put32(cmap, static_cast<u32>(offset));
        offset = offset + record.map.size();
    }
    for (const MapRecord& record : records) append(cmap, record.map);
    return cmap;
}

} // namespace font_builder
