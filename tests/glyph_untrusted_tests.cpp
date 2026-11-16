// stxlib::glyphs against font tables that are wrong on purpose. An
// application may hand a face any bytes as a font, so every table is
// untrusted input: this test states what each reader promises of such
// input (its contract) and tries it on tables written to be wrong, and on
// a bounded number of altered ones.
//
// The contracts.
//   C1  glyph_of gives 0 or a glyph the font has (less than glyph_count),
//       for any character, whatever the character map says.
//   C2  next_stretch, asked from a character, gives a stretch that does
//       not end before it begins and does not end before that character,
//       or says there is none. Going through a map (from 0, then from one
//       past each stretch's end) therefore ends, and takes no more calls
//       than the map has entries, and two more. glyph_in_stretch gives 0
//       or a glyph the font has, and 0 for a character outside the stretch.
//   C3  For a character map as the format requires it (entries in order,
//       not overlapping), going through it meets exactly the characters
//       glyph_of has a glyph for, each with that glyph.
//   C4  advance_of gives 0 for a glyph the font has not or a table that
//       does not reach it, else a number from 0 to 65535.
//   C5  load_glyph and load_scaled_glyph either refuse, or give an outline
//       within the limits whose tags are as many as its points and whose
//       contours' last points go up and end at the last point.
//       to_segments of such an outline refuses or gives segments whose
//       points are as many as their kinds need; measure of those refuses
//       or gives a box within the limits.
//   C6  A Face and its Typeface, opened on such a font, fit, measure and
//       draw: each call refuses or succeeds, a run's mask is as large as
//       its box says, and nothing is read or written outside what is held.
//   C7  The layout readers give places inside their table, classes and
//       indices no larger than the format can hold, and no more scripts,
//       features, lookups or subtables than the table has room for.
// None of these may read outside the bytes, wrap a whole number or loop
// without end: the builds under AddressSanitizer, UBSan and the integer
// sanitizers are what check that, on every case here.
//
// What is tried.
//   U1  character maps of format 4 written wrong: no segments, segments
//       out of order, overlapping, ending before they begin, a list of
//       glyphs placed outside the table or at an odd place, glyphs the
//       font has not, no last segment, and every prefix;
//   U2  of format 6: no characters (first 0 and first 65535), a table that
//       runs past the last character, glyphs the font has not, prefixes;
//   U3  of format 12: no groups, more groups than there is table, groups
//       out of order, overlapping, ending before they begin, glyph numbers
//       that would wrap, a group to the last 32-bit number, prefixes;
//   U4  the bounded search over character maps;
//   U5  advances (hmtx);
//   U6  outlines (glyf and loca), through the reader, the segments, the
//       rasterizer's measure and a Face;
//   U7  the layout tables.
// Whole fonts cut short and altered anywhere (the table directory, head,
// maxp, hhea, OS/2, CFF and the rest), through the reader and through a
// Face, are font_file_tests.cpp's P8, with its own stated bound.
//
// The bound of the search. Every altered table comes from a fixed seed
// (xorshift64, the seeds below), so the cases are the same on every run
// and host. For each of three well-formed character maps: every prefix,
// every 16-bit field set to each of 0, 1, 0x7FFF, 0x8000, 0xFFFE and
// 0xFFFF, and 20,000 copies with one to four bytes changed. For glyf with
// loca, and for hmtx: every byte set to each of 0x00, 0xFF, 0x80, and
// 30,000 copies with one to four bytes changed (15,000 through a Face).
// For the layout tables: every 16-bit field set to each of the six values,
// and 30,000 copies with one to four bytes changed. The counts are printed
// and checked. It is a search of those cases and no proof of the rest.

#include "algorithms/glyphs/layout_tables.hpp"
#include "stxlib/font_file.hpp"
#include "stxlib/glyph_face.hpp"
#include "stxlib/glyph_mask.hpp"

#include "font_builder.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace glyphs = stxlib::glyphs;
namespace detail = stxlib::glyphs::detail;
using stxlib::i32;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

using font_builder::append;
using font_builder::Bytes;
using font_builder::cmap_table;
using font_builder::composite_glyph;
using font_builder::font_of;
using font_builder::glyph_tables;
using font_builder::head_table;
using font_builder::hhea_table;
using font_builder::map_format_12;
using font_builder::map_format_4;
using font_builder::map_format_6;
using font_builder::maxp_table;
using font_builder::Metric;
using font_builder::put16;
using font_builder::put32;
using font_builder::simple_glyph;
using font_builder::view_of;

std::uint64_t random_state = 0;
void seed(const std::uint64_t value) { random_state = value; }
[[nodiscard]] std::uint64_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

// ---- character maps ------------------------------------------------------------------------------

// A font that is nothing but a character map of the format given.
[[nodiscard]] glyphs::Font font_of_map(const Bytes& map, const u32 format, const u32 glyph_count) {
    glyphs::Font font{};
    font.bytes = stxlib::View<u8>(map.data(), map.size());
    font.character_map = glyphs::FontTable{.offset = 0, .length = static_cast<u32>(map.size())};
    font.character_map_format = format;
    font.glyph_count = glyph_count;
    return font;
}

// How many entries a map says it has (segments, groups), whether or not
// it has room for them.
[[nodiscard]] usize entries_of(const Bytes& map, const u32 format) {
    usize entries = 1;
    if (format == 4 && map.size() >= 8) entries = ((static_cast<usize>(map[6]) << 8) | map[7]) / 2;
    if (format == 12 && map.size() >= 16) {
        entries = (static_cast<usize>(map[12]) << 24) | (static_cast<usize>(map[13]) << 16) | (static_cast<usize>(map[14]) << 8) | map[15];
        // No more than the bytes could hold.
        const usize room = map.size() / 12 + 1;
        entries = entries < room ? entries : room;
    }
    return entries;
}

// C1 and C2 on a map; `checked` counts what was asked. False at the first
// promise broken.
[[nodiscard]] bool map_keeps_its_promises(const glyphs::Font& font, const usize entries, const bool every_character, std::uint64_t& checked) {
    // C1: characters all through the first planes, or a spread of them,
    // and the ones a 32-bit number makes awkward.
    const u32 awkward[] = {0, 1, 0x7F, 0xFF, 0x100, 0xFFFE, 0xFFFF, 0x10000, 0x10FFFF, 0x110000, 0x7FFFFFFF, 0x80000000U, 0xFFFFFFF0U, 0xFFFFFFFEU, 0xFFFFFFFFU};
    for (const u32 character : awkward) {
        if (glyphs::glyph_of(font, character) >= font.glyph_count) return false;
        checked = checked + 1;
    }
    const u32 step = every_character ? 1 : 251;
    for (u32 character = 0; character < 0x20000; character = character + step) {
        if (glyphs::glyph_of(font, character) >= font.glyph_count) return false;
        checked = checked + 1;
    }
    // C2: going through the map.
    u32 from = 0;
    usize calls = 0;
    bool more = true;
    while (more) {
        glyphs::MappedStretch stretch{};
        calls = calls + 1;
        if (calls > entries + 2) return false;
        if (!glyphs::next_stretch(font, from, stretch)) break;
        if (stretch.last < stretch.first || stretch.last < from) return false;
        const u32 begin = stretch.first > from ? stretch.first : from;
        const u32 inside[] = {begin, stretch.last, begin + (stretch.last - begin) / 2, begin + (stretch.last - begin) / 3};
        for (const u32 character : inside) {
            if (glyphs::glyph_in_stretch(font, stretch, character) >= font.glyph_count) return false;
            checked = checked + 1;
        }
        if (stretch.first > 0 && glyphs::glyph_in_stretch(font, stretch, stretch.first - 1) != 0) return false;
        if (stretch.last < 0xFFFFFFFFU && glyphs::glyph_in_stretch(font, stretch, stretch.last + 1) != 0) return false;
        more = stretch.last < 0xFFFFFFFFU;
        if (more) from = stretch.last + 1;
    }
    // Asked from the awkward characters too.
    for (const u32 character : awkward) {
        glyphs::MappedStretch stretch{};
        if (glyphs::next_stretch(font, character, stretch) && (stretch.last < stretch.first || stretch.last < character)) return false;
        checked = checked + 1;
    }
    return true;
}

// C3: for a map in order, going through it is glyph_of.
[[nodiscard]] bool walk_is_glyph_of(const glyphs::Font& font, const u32 through) {
    u32 from = 0;
    u32 next_to_check = 0;
    bool same = true;
    for (u32 step = 0; step < 100000 && same; ++step) {
        glyphs::MappedStretch stretch{};
        if (!glyphs::next_stretch(font, from, stretch)) break;
        const u32 begin = stretch.first > from ? stretch.first : from;
        for (u32 between = next_to_check; between < begin && between <= through && same; ++between) same = glyphs::glyph_of(font, between) == 0;
        for (u32 character = begin; character <= stretch.last && character <= through && same; ++character) {
            same = glyphs::glyph_in_stretch(font, stretch, character) == glyphs::glyph_of(font, character);
        }
        if (stretch.last >= through) {
            next_to_check = through + 1;
            break;
        }
        next_to_check = stretch.last + 1;
        from = stretch.last + 1;
    }
    for (u32 after = next_to_check; after <= through && same; ++after) same = glyphs::glyph_of(font, after) == 0;
    return same;
}

// A format 4 map from its columns, whatever they say.
[[nodiscard]] Bytes raw_format_4(const std::vector<u32>& ends, const std::vector<u32>& starts, const std::vector<u32>& deltas,
                                 const std::vector<u32>& offsets, const std::vector<u32>& listed, const u32 said_segments) {
    Bytes map{};
    put16(map, 4);
    put16(map, 0);
    put16(map, 0);
    put16(map, static_cast<i32>(said_segments * 2));
    put16(map, 0);
    put16(map, 0);
    put16(map, 0);
    for (const u32 value : ends) put16(map, static_cast<i32>(value));
    put16(map, 0);
    for (const u32 value : starts) put16(map, static_cast<i32>(value));
    for (const u32 value : deltas) put16(map, static_cast<i32>(value));
    for (const u32 value : offsets) put16(map, static_cast<i32>(value));
    for (const u32 value : listed) put16(map, static_cast<i32>(value));
    map[2] = static_cast<u8>(map.size() >> 8);
    map[3] = static_cast<u8>(map.size() & 0xFFU);
    return map;
}

struct Group final {
    u32 first{0};
    u32 last{0};
    u32 glyph{0};
};

// A format 12 map from its groups, whatever they say.
[[nodiscard]] Bytes raw_format_12(const std::vector<Group>& groups, const u32 said_groups) {
    Bytes map{};
    put16(map, 12);
    put16(map, 0);
    put32(map, static_cast<u32>(16 + groups.size() * 12));
    put32(map, 0);
    put32(map, said_groups);
    for (const Group& group : groups) {
        put32(map, group.first);
        put32(map, group.last);
        put32(map, group.glyph);
    }
    return map;
}

[[nodiscard]] Bytes raw_format_6(const u32 first, const u32 said_count, const std::vector<u32>& listed) {
    Bytes map{};
    put16(map, 6);
    put16(map, static_cast<i32>(10 + listed.size() * 2));
    put16(map, 0);
    put16(map, static_cast<i32>(first));
    put16(map, static_cast<i32>(said_count));
    for (const u32 glyph : listed) put16(map, static_cast<i32>(glyph));
    return map;
}

struct WrongMap final {
    const char* what{""};
    u32 format{0};
    Bytes map{};
};

// Each wrong map keeps C1 and C2, and so does every prefix of it.
void try_wrong_maps(const std::vector<WrongMap>& maps, std::uint64_t& tried, std::uint64_t& checked) {
    const u32 glyph_count = 8;
    for (const WrongMap& wrong : maps) {
        const glyphs::Font font = font_of_map(wrong.map, wrong.format, glyph_count);
        const bool kept = map_keeps_its_promises(font, entries_of(wrong.map, wrong.format), true, checked);
        if (!kept) std::fprintf(stderr, "  a promise broken by: %s\n", wrong.what);
        CHECK(kept);
        tried = tried + 1;
        bool prefixes_kept = true;
        for (usize length = 0; length < wrong.map.size() && prefixes_kept; ++length) {
            const Bytes prefix(wrong.map.begin(), wrong.map.begin() + static_cast<std::ptrdiff_t>(length));
            const glyphs::Font cut = font_of_map(prefix, wrong.format, glyph_count);
            prefixes_kept = map_keeps_its_promises(cut, entries_of(prefix, wrong.format), false, checked);
            tried = tried + 1;
        }
        if (!prefixes_kept) std::fprintf(stderr, "  a promise broken by a prefix of: %s\n", wrong.what);
        CHECK(prefixes_kept);
    }
}

void wrong_maps() {
    std::uint64_t tried = 0;
    std::uint64_t checked = 0;
    std::vector<WrongMap> maps{};
    // U1: format 4.
    maps.push_back({"4: no segments", 4, raw_format_4({}, {}, {}, {}, {}, 0)});
    maps.push_back({"4: more segments said than written", 4, raw_format_4({0x41, 0xFFFF}, {0x41, 0xFFFF}, {0, 1}, {0, 0}, {}, 400)});
    maps.push_back({"4: ends out of order", 4, raw_format_4({0x50, 0x30, 0x90, 0xFFFF}, {0x48, 0x28, 0x80, 0xFFFF}, {0xFFB9, 0xFFDA, 0xFF83, 1}, {0, 0, 0, 0}, {}, 4)});
    maps.push_back({"4: overlapping segments", 4, raw_format_4({0x60, 0x80, 0xFFFF}, {0x20, 0x40, 0xFFFF}, {0xFFE1, 0xFFC3, 1}, {0, 0, 0}, {}, 3)});
    maps.push_back({"4: a segment that ends before it begins", 4, raw_format_4({0x40, 0x90, 0xFFFF}, {0x60, 0x80, 0xFFFF}, {0, 0xFF83, 1}, {0, 0, 0}, {}, 3)});
    maps.push_back({"4: every segment ends before it begins", 4, raw_format_4({0x10, 0x20, 0x30}, {0x1000, 0x2000, 0x3000}, {0, 0, 0}, {0, 0, 0}, {}, 3)});
    maps.push_back({"4: a list of glyphs far outside the table", 4, raw_format_4({0x42, 0xFFFF}, {0x41, 0xFFFF}, {0, 1}, {0xFFFE, 0}, {3, 4}, 2)});
    maps.push_back({"4: a list of glyphs just past the table", 4, raw_format_4({0x42, 0xFFFF}, {0x41, 0xFFFF}, {0, 1}, {8, 0}, {3}, 2)});
    maps.push_back({"4: a list of glyphs at an odd place", 4, raw_format_4({0x42, 0xFFFF}, {0x41, 0xFFFF}, {0, 1}, {3, 0}, {3, 4, 5}, 2)});
    maps.push_back({"4: the list offset that means none, with a list", 4, raw_format_4({0x42, 0xFFFF}, {0x41, 0xFFFF}, {0, 1}, {0xFFFF, 0}, {3, 4}, 2)});
    maps.push_back({"4: listed glyphs the font has not", 4, raw_format_4({0x43, 0xFFFF}, {0x41, 0xFFFF}, {0, 1}, {4, 0}, {7, 8, 0xFFFF}, 2)});
    maps.push_back({"4: a delta to glyphs the font has not", 4, raw_format_4({0x7E, 0xFFFF}, {0x20, 0xFFFF}, {0x9000, 1}, {0, 0}, {}, 2)});
    maps.push_back({"4: all 65,536 characters in one segment", 4, raw_format_4({0xFFFF}, {0}, {0}, {0}, {}, 1)});
    maps.push_back({"4: all characters, listed, with a short list", 4, raw_format_4({0xFFFF}, {0}, {0}, {2}, {1, 2, 3}, 1)});
    maps.push_back({"4: no last segment", 4, raw_format_4({0x41, 0x61}, {0x41, 0x61}, {0xFFC0, 0xFFA1}, {0, 0}, {}, 2)});
    maps.push_back({"4: the same segment many times", 4, raw_format_4({0x41, 0x41, 0x41, 0x41, 0x41}, {0x41, 0x41, 0x41, 0x41, 0x41}, {1, 2, 3, 4, 5}, {0, 0, 0, 0, 0}, {}, 5)});
    // U2: format 6.
    maps.push_back({"6: no characters, from 0", 6, raw_format_6(0, 0, {})});
    maps.push_back({"6: no characters, from 65535", 6, raw_format_6(0xFFFF, 0, {})});
    maps.push_back({"6: one character, the last", 6, raw_format_6(0xFFFF, 1, {3})});
    maps.push_back({"6: a table that runs past the last character", 6, raw_format_6(0xFFF0, 0x20, std::vector<u32>(0x20, 5))});
    maps.push_back({"6: more characters said than written", 6, raw_format_6(0x41, 0xFFFF, {1, 2, 3})});
    maps.push_back({"6: glyphs the font has not", 6, raw_format_6(0x41, 4, {7, 8, 0xFFFF, 0})});
    // U3: format 12.
    maps.push_back({"12: no groups", 12, raw_format_12({}, 0)});
    maps.push_back({"12: more groups said than there is table", 12, raw_format_12({{0x41, 0x42, 1}}, 0xFFFFFFFFU)});
    maps.push_back({"12: one more group said than written", 12, raw_format_12({{0x41, 0x42, 1}}, 2)});
    maps.push_back({"12: groups out of order", 12, raw_format_12({{0x500, 0x510, 1}, {0x41, 0x45, 2}, {0x1F600, 0x1F603, 3}, {0x100, 0x101, 4}}, 4)});
    maps.push_back({"12: overlapping groups", 12, raw_format_12({{0x41, 0x60, 1}, {0x50, 0x70, 2}, {0x50, 0x52, 3}}, 3)});
    maps.push_back({"12: a group that ends before it begins", 12, raw_format_12({{0x60, 0x41, 1}, {0x100, 0x105, 2}}, 2)});
    maps.push_back({"12: a group from near the last number round to 16", 12, raw_format_12({{0xFFFFFFF0U, 0x10, 1}}, 1)});
    maps.push_back({"12: glyph numbers that would wrap", 12, raw_format_12({{0x41, 0x80, 0xFFFFFFF0U}, {0x100, 0x200, 0xFFFFFFFFU}}, 2)});
    maps.push_back({"12: a group to the last 32-bit number", 12, raw_format_12({{0x41, 0x42, 1}, {0xFFFFFF00U, 0xFFFFFFFFU, 2}}, 2)});
    maps.push_back({"12: every number in one group", 12, raw_format_12({{0, 0xFFFFFFFFU, 0}}, 1)});
    maps.push_back({"12: every number in one group, from glyph 1", 12, raw_format_12({{0, 0xFFFFFFFFU, 1}}, 1)});
    maps.push_back({"12: the same group many times", 12, raw_format_12({{0x41, 0x43, 1}, {0x41, 0x43, 2}, {0x41, 0x43, 3}, {0x41, 0x43, 4}}, 4)});
    try_wrong_maps(maps, tried, checked);
    // A few by hand, beyond "nothing broke".
    const Bytes no_characters = raw_format_6(0, 0, {});
    const glyphs::Font none = font_of_map(no_characters, 6, 8);
    glyphs::MappedStretch stretch{};
    CHECK(!glyphs::next_stretch(none, 0, stretch) && glyphs::glyph_of(none, 0) == 0);
    const Bytes last_character = raw_format_6(0xFFFF, 1, {3});
    const glyphs::Font last = font_of_map(last_character, 6, 8);
    CHECK(glyphs::next_stretch(last, 0, stretch) && stretch.first == 0xFFFF && stretch.last == 0xFFFF && glyphs::glyph_of(last, 0xFFFF) == 3);
    const Bytes list_outside = raw_format_4({0x42, 0xFFFF}, {0x41, 0xFFFF}, {0, 1}, {0xFFFE, 0}, {3, 4}, 2);
    const glyphs::Font beyond = font_of_map(list_outside, 4, 8);
    CHECK(glyphs::glyph_of(beyond, 0x41) == 0 && glyphs::glyph_of(beyond, 0x42) == 0);
    const Bytes wrapping_glyphs = raw_format_12({{0x41, 0x80, 0xFFFFFFF0U}}, 1);
    const glyphs::Font wrapping = font_of_map(wrapping_glyphs, 12, 8);
    CHECK(glyphs::glyph_of(wrapping, 0x41) == 0 && glyphs::glyph_of(wrapping, 0x51) == 0 && glyphs::glyph_of(wrapping, 0x80) == 0);
    // A group that ends before it begins holds nothing; the one after it
    // is still found by its characters.
    const Bytes backward_group = raw_format_12({{0x60, 0x41, 1}, {0x100, 0x105, 2}}, 2);
    const glyphs::Font backward = font_of_map(backward_group, 12, 8);
    CHECK(glyphs::glyph_of(backward, 0x50) == 0 && glyphs::glyph_of(backward, 0x102) == 4);
    std::printf("wrong maps: %llu maps and prefixes, %llu answers checked\n", static_cast<unsigned long long>(tried), static_cast<unsigned long long>(checked));
    CHECK(tried > 1000);
}

void search_maps() {
    struct Seed final {
        u32 format{0};
        Bytes map{};
        std::uint64_t seed{0};
        u32 through{0};
    };
    const std::vector<Seed> seeds = {
        {4, map_format_4({{0x20, 0x7E, 0xFFE1, {}}, {0xA0, 0xA3, 0, {5, 0, 6, 7}}, {0x2010, 0x2015, 0xDFF4, {}}, {0xFFFF, 0xFFFF, 1, {}}}), 0x243F6A8885A308D3ULL, 0x2100},
        {6, map_format_6(0x30, {1, 2, 3, 0, 5, 6, 7, 1, 2, 3}), 0x13198A2E03707344ULL, 0x100},
        {12, map_format_12({{0x20, 0x7E, 1}, {0xA0, 0xFF, 2}, {0x1F600, 0x1F64F, 3}, {0x20000, 0x20003, 4}}), 0xA4093822299F31D0ULL, 0x20010},
    };
    const u32 glyph_count = 120;
    const u32 values[] = {0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF};
    std::uint64_t tried = 0;
    std::uint64_t checked = 0;
    bool kept = true;
    for (const Seed& from : seeds) {
        // The map as it should be keeps C3 as well.
        const glyphs::Font good = font_of_map(from.map, from.format, glyph_count);
        CHECK(map_keeps_its_promises(good, entries_of(from.map, from.format), true, checked) && walk_is_glyph_of(good, from.through));
        // Every prefix.
        for (usize length = 0; length < from.map.size() && kept; ++length) {
            const Bytes prefix(from.map.begin(), from.map.begin() + static_cast<std::ptrdiff_t>(length));
            kept = map_keeps_its_promises(font_of_map(prefix, from.format, glyph_count), entries_of(prefix, from.format), false, checked);
            tried = tried + 1;
        }
        // Every 16-bit field set to each of six values.
        for (usize at = 0; at + 1 < from.map.size() && kept; at = at + 2) {
            for (const u32 value : values) {
                Bytes changed = from.map;
                changed[at] = static_cast<u8>(value >> 8);
                changed[at + 1] = static_cast<u8>(value & 0xFFU);
                kept = kept && map_keeps_its_promises(font_of_map(changed, from.format, glyph_count), entries_of(changed, from.format), false, checked);
                tried = tried + 1;
            }
        }
        // One to four bytes changed, 20,000 times.
        seed(from.seed);
        for (int round = 0; round < 20000 && kept; ++round) {
            Bytes changed = from.map;
            const usize changes = 1 + next_random() % 4;
            for (usize change = 0; change < changes; ++change) changed[next_random() % changed.size()] = static_cast<u8>(next_random() & 0xFFU);
            kept = map_keeps_its_promises(font_of_map(changed, from.format, glyph_count), entries_of(changed, from.format), false, checked);
            tried = tried + 1;
        }
        if (!kept) std::fprintf(stderr, "  a promise broken by an altered map of format %u\n", from.format);
        CHECK(kept);
    }
    std::printf("search of maps: %llu maps, %llu answers checked\n", static_cast<unsigned long long>(tried), static_cast<unsigned long long>(checked));
    CHECK(tried > 60000);
}

// ---- a whole font, for advances and outlines ----------------------------------------------------

// Simple glyphs with curves and long and short steps, and glyphs put
// together from them: by offset, by scale, by two-by-two, by matched
// points, one inside another, and one that names itself.
[[nodiscard]] Bytes outline_font() {
    std::vector<Bytes> glyph_bytes{};
    glyph_bytes.push_back(Bytes{});
    glyph_bytes.push_back(simple_glyph({{{0, 0, true}, {0, 101, true}, {101, 101, true}, {101, 0, true}}}));
    glyph_bytes.push_back(simple_glyph({{{20, 0, false}, {300, 0, true}, {300, 400, false}, {-25, 400, false}, {-25, 75, true}},
                                        {{50, 50, true}, {50, 51, true}, {50, 52, true}, {50, 53, true}, {600, -700, true}}}));
    glyph_bytes.push_back(composite_glyph(0, {{0x0003, 1, 200, 50, {}}, {0x000A, 1, 10, -5, {0x2000}}}));
    glyph_bytes.push_back(composite_glyph(0, {{0x0002, 1, 0, 0, {}}, {0x0000, 1, 2, 0, {}}}));
    glyph_bytes.push_back(composite_glyph(0, {{0x0003, 3, -300, 7, {}}, {0x0042, 2, 0, 0, {0x4000, -0x2000}}}));
    glyph_bytes.push_back(composite_glyph(0, {{0x0002, 6, 1, 1, {}}}));
    glyph_bytes.push_back(composite_glyph(0, {{0x0883, 1, 100, 60, {0x2000, 0x1000, -0x1000, 0x2000}}}));
    const std::vector<Metric> metrics = {{500, 0}, {600, 0}, {700, -30}, {800, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
    Bytes glyf{};
    Bytes loca{};
    Bytes hmtx{};
    glyph_tables(glyph_bytes, metrics, 4, true, glyf, loca, hmtx);
    const Bytes map = map_format_4({{'A', 'G', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}});
    const Bytes font = font_of(0x00010000U, {{"cmap", cmap_table({{3, 1, map}})},
                                             {"glyf", glyf},
                                             {"head", head_table(1000, true)},
                                             {"hhea", hhea_table(800, -220, 50, 4)},
                                             {"hmtx", hmtx},
                                             {"loca", loca},
                                             {"maxp", maxp_table(8)}});
    return font;
}

// C5 for one glyph of a font. False at the first promise broken.
[[nodiscard]] bool outline_keeps_its_promises(const glyphs::Font& font, const u32 glyph, const glyphs::FontLimits& limits, std::uint64_t& loaded) {
    glyphs::GlyphOutline outline{};
    stxlib::Vector<glyphs::Segment> kinds{};
    stxlib::Vector<glyphs::Point> points{};
    for (int scaled = 0; scaled < 2; ++scaled) {
        const stxlib::Status status =
            scaled == 0 ? glyphs::load_glyph(font, glyph, limits, outline) : glyphs::load_scaled_glyph(font, glyph, 70001, 65000, limits, outline);
        if (status.failed()) continue;
        loaded = loaded + 1;
        const usize count = outline.points.count();
        if (outline.tags.count() != count || count > limits.points || outline.contour_ends.count() > limits.contours) return false;
        usize before = 0;
        for (usize contour = 0; contour < outline.contour_ends.count(); ++contour) {
            const usize last = outline.contour_ends[contour];
            if (last >= count || (contour > 0 && last <= before)) return false;
            before = last;
        }
        if (outline.contour_ends.count() > 0 && before + 1 != count) return false;
        if (outline.contour_ends.count() == 0 && count != 0) return false;
        if (glyphs::to_segments(outline, kinds, points).failed()) continue;
        usize needed = 0;
        for (usize index = 0; index < kinds.count(); ++index) {
            const glyphs::Segment kind = kinds[index];
            needed = needed + (kind == glyphs::Segment::quadratic ? 2 : (kind == glyphs::Segment::cubic ? 3 : 1));
        }
        if (needed != points.count() || (kinds.count() > 0 && kinds[0] != glyphs::Segment::move)) return false;
        const glyphs::Outline cut{.segments = kinds.view(), .points = points.view()};
        const glyphs::Limits mask_limits{};
        glyphs::Bounds bounds{};
        if (glyphs::measure(cut, glyphs::Point{0, 0}, mask_limits, bounds).succeeded()) {
            if (bounds.width > mask_limits.width || bounds.height > mask_limits.height) return false;
        }
    }
    return true;
}

// C6: a face on the bytes, used as a consumer uses one.
[[nodiscard]] bool face_keeps_its_promises(const Bytes& bytes, std::uint64_t& drawn) {
    glyphs::Face face{};
    if (face.open(view_of(bytes), 0).failed()) return true;
    const glyphs::Font& font = *face.font();
    glyphs::RunGlyph run[10] = {};
    for (u32 index = 0; index < 10; ++index) run[index] = glyphs::RunGlyph{index, static_cast<i32>(index * 640 + index * 7), static_cast<i32>(index)};
    glyphs::RunCoverage coverage{};
    glyphs::RunBox box{};
    const glyphs::Fit fits[] = {glyphs::Fit::light, glyphs::Fit::none, glyphs::Fit::monochrome};
    for (const glyphs::Fit fit : fits) {
        glyphs::DrawStyle style{};
        style.size = 12 * 64 + 3;
        style.fit = fit;
        style.thicken = fit == glyphs::Fit::none ? 40 : 0;
        const bool measured = face.measure_run(stxlib::View<glyphs::RunGlyph>(run), style, box).succeeded();
        if (face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, coverage).succeeded()) {
            drawn = drawn + 1;
            if (coverage.pixels.count() != static_cast<usize>(coverage.width) * coverage.height) return false;
            if (measured && !coverage.oversized &&
                (box.right - box.left != static_cast<i32>(coverage.width) || box.bottom - box.top != static_cast<i32>(coverage.height))) {
                return false;
            }
        }
    }
    glyphs::InkBox ink{};
    for (u32 glyph = 0; glyph < font.glyph_count && glyph < 10; ++glyph) {
        if ((*face.typeface()).ink_box(glyph, 900, ink).succeeded() && (ink.right < ink.left || ink.top < ink.bottom)) return false;
    }
    return true;
}

void advances_and_outlines() {
    const Bytes good = outline_font();
    glyphs::Font opened{};
    CHECK(glyphs::open_font(view_of(good), 0, opened).succeeded() && opened.glyph_count == 8);
    if (opened.glyph_count != 8) return;
    const glyphs::FontLimits limits{};
    std::uint64_t loaded = 0;
    std::uint64_t tried = 0;
    std::uint64_t drawn = 0;
    // The font as it should be.
    bool kept = true;
    for (u32 glyph = 0; glyph < 9; ++glyph) kept = kept && outline_keeps_its_promises(opened, glyph, limits, loaded);
    CHECK(kept && loaded >= 12);
    // C4 by hand: advances 500, 600, 700, 800, the last one for the rest,
    // nothing for a glyph the font has not; and for tables cut short.
    CHECK(glyphs::advance_of(opened, 0) == 500 && glyphs::advance_of(opened, 3) == 800 && glyphs::advance_of(opened, 7) == 800 &&
          glyphs::advance_of(opened, 8) == 0 && glyphs::advance_of(opened, 0xFFFFFFFFU) == 0);
    glyphs::Font short_advances = opened;
    short_advances.hmtx.length = 6;
    CHECK(glyphs::advance_of(short_advances, 0) == 0 && glyphs::advance_of(short_advances, 3) == 0);
    short_advances.hmtx.length = 0;
    CHECK(glyphs::advance_of(short_advances, 0) == 0);
    short_advances = opened;
    short_advances.advance_count = 0;
    CHECK(glyphs::advance_of(short_advances, 2) == 0);
    short_advances = opened;
    short_advances.advance_count = 0xFFFFFFFFU;
    CHECK(glyphs::advance_of(short_advances, 2) == 0 || glyphs::advance_of(short_advances, 2) == 700);
    // U5 and U6: the tables altered under a font already opened (its
    // numbers stay; what it reads changes), so that the readers meet the
    // wrong bytes themselves.
    const glyphs::FontTable tables[] = {opened.glyf, opened.loca, opened.hmtx};
    const u8 values[] = {0x00, 0xFF, 0x80};
    for (const glyphs::FontTable& table : tables) {
        for (usize at = 0; at < table.length && kept; ++at) {
            for (const u8 value : values) {
                Bytes changed = good;
                changed[table.offset + at] = value;
                glyphs::Font font = opened;
                font.bytes = stxlib::View<u8>(changed.data(), changed.size());
                for (u32 glyph = 0; glyph < 9 && kept; ++glyph) {
                    kept = outline_keeps_its_promises(font, glyph, limits, loaded) && glyphs::advance_of(font, glyph) <= 0xFFFF && glyphs::advance_of(font, glyph) >= 0;
                }
                tried = tried + 1;
            }
        }
    }
    CHECK(kept);
    seed(0x082EFA98EC4E6C89ULL);
    const usize first = opened.glyf.offset < opened.loca.offset ? opened.glyf.offset : opened.loca.offset;
    for (int round = 0; round < 30000 && kept; ++round) {
        Bytes changed = good;
        const usize changes = 1 + next_random() % 4;
        for (usize change = 0; change < changes; ++change) {
            const glyphs::FontTable& table = tables[next_random() % 3];
            changed[table.offset + next_random() % table.length] = static_cast<u8>(next_random() & 0xFFU);
        }
        glyphs::Font font = opened;
        font.bytes = stxlib::View<u8>(changed.data(), changed.size());
        for (u32 glyph = 0; glyph < 9 && kept; ++glyph) {
            kept = outline_keeps_its_promises(font, glyph, limits, loaded) && glyphs::advance_of(font, glyph) <= 0xFFFF && glyphs::advance_of(font, glyph) >= 0;
        }
        // Every other one through a face as well, opened on the bytes.
        if (round % 2 == 0) kept = kept && face_keeps_its_promises(changed, drawn);
        tried = tried + 1;
    }
    static_cast<void>(first);
    CHECK(kept);
    // Small limits: what is over them is refused, never cut to fit.
    glyphs::FontLimits few{};
    few.points = 6;
    few.contours = 1;
    few.components = 1;
    few.component_depth = 1;
    kept = true;
    for (u32 glyph = 0; glyph < 9; ++glyph) kept = kept && outline_keeps_its_promises(opened, glyph, few, loaded);
    CHECK(kept);
    std::printf("search of advances and outlines: %llu fonts, %llu outlines loaded, %llu runs drawn\n", static_cast<unsigned long long>(tried),
                static_cast<unsigned long long>(loaded), static_cast<unsigned long long>(drawn));
    CHECK(tried > 30000 && loaded > 100000 && drawn > 10000);
}

// ---- the layout tables ------------------------------------------------------------------------------

// A table of glyph definitions and one of substitutions, each of every
// kind of thing the readers read, as it should be.
[[nodiscard]] Bytes definitions_bytes() {
    Bytes table{};
    put16(table, 1);
    put16(table, 2);
    put16(table, 14);
    put16(table, 0);
    put16(table, 0);
    put16(table, 36);
    put16(table, 52);
    // Glyph classes (format 2, three ranges), mark classes (format 1), and
    // two mark sets (a coverage of each format).
    put16(table, 2);
    put16(table, 3);
    for (const u32 value : {5U, 6U, 1U, 7U, 7U, 2U, 8U, 9U, 3U}) put16(table, static_cast<i32>(value));
    put16(table, 1);
    put16(table, 8);
    put16(table, 2);
    put16(table, 1);
    put16(table, 2);
    put16(table, 0xEEEE);
    put16(table, 0xEEEE);
    put16(table, 0xEEEE);
    put16(table, 1);
    put16(table, 2);
    put32(table, 12);
    put32(table, 18);
    put16(table, 1);
    put16(table, 1);
    put16(table, 8);
    put16(table, 2);
    put16(table, 1);
    put16(table, 8);
    put16(table, 9);
    put16(table, 0);
    return table;
}

[[nodiscard]] Bytes substitutions_bytes() {
    Bytes dflt{};
    for (const u32 value : {4U, 0U, 0U, 0xFFFFU, 2U, 0U, 1U}) put16(dflt, static_cast<i32>(value));
    Bytes latn{};
    put16(latn, 10);
    put16(latn, 1);
    put32(latn, 0x54524B20U);
    put16(latn, 18);
    for (const u32 value : {0U, 2U, 1U, 1U, 0U, 0xFFFFU, 3U, 0U, 1U, 2U}) put16(latn, static_cast<i32>(value));
    Bytes scripts{};
    put16(scripts, 2);
    put32(scripts, 0x44464C54U);
    put16(scripts, 14);
    put32(scripts, 0x6C61746EU);
    put16(scripts, static_cast<i32>(14 + dflt.size()));
    append(scripts, dflt);
    append(scripts, latn);
    Bytes features{};
    put16(features, 3);
    put32(features, 0x6B65726EU);
    put16(features, 20);
    put32(features, 0x6C696761U);
    put16(features, 26);
    put32(features, 0x6C6F636CU);
    put16(features, 34);
    for (const u32 value : {0U, 1U, 1U, 0U, 2U, 0U, 2U, 0U, 0U}) put16(features, static_cast<i32>(value));
    Bytes first{};
    for (const u32 value : {4U, 8U, 1U, 8U, 1U, 2U, 3U, 7U}) put16(first, static_cast<i32>(value));
    Bytes second{};
    for (const u32 value : {1U, 0x0210U, 2U, 12U, 20U, 5U, 2U, 1U, 3U, 9U, 4U, 1U, 1U, 5U}) put16(second, static_cast<i32>(value));
    Bytes third{};
    for (const u32 value : {7U, 0U, 2U, 10U, 18U, 1U, 6U}) put16(third, static_cast<i32>(value));
    put32(third, 16);
    put16(third, 1);
    put16(third, 6);
    put32(third, 10);
    put16(third, 1);
    put16(third, 1);
    put16(third, 6);
    Bytes lookups{};
    put16(lookups, 3);
    put16(lookups, 8);
    put16(lookups, static_cast<i32>(8 + first.size()));
    put16(lookups, static_cast<i32>(8 + first.size() + second.size()));
    append(lookups, first);
    append(lookups, second);
    append(lookups, third);
    Bytes table{};
    put16(table, 1);
    put16(table, 0);
    put16(table, 10);
    put16(table, static_cast<i32>(10 + scripts.size()));
    put16(table, static_cast<i32>(10 + scripts.size() + features.size()));
    append(table, scripts);
    append(table, features);
    append(table, lookups);
    return table;
}

// C7 on the two tables. False at the first promise broken.
[[nodiscard]] bool layout_keeps_its_promises(const Bytes& definitions, const Bytes& layout_table, std::uint64_t& checked) {
    Bytes both = definitions;
    append(both, layout_table);
    glyphs::Font font{};
    font.bytes = stxlib::View<u8>(both.data(), both.size());
    font.glyph_count = 65535;
    font.definitions = glyphs::FontTable{.offset = 0, .length = static_cast<u32>(definitions.size())};
    font.substitutions = glyphs::FontTable{.offset = static_cast<u32>(definitions.size()), .length = static_cast<u32>(layout_table.size())};
    font.positions = font.substitutions;
    const detail::GlyphDefinitions gdef = detail::open_definitions(font);
    for (u32 glyph = 0; glyph < 12; ++glyph) {
        if (detail::glyph_class(gdef, glyph) > 0xFFFF || detail::mark_attachment_class(gdef, glyph) > 0xFFFF) return false;
        for (u32 set = 0; set < 3; ++set) static_cast<void>(detail::in_mark_set(gdef, set, glyph));
        checked = checked + 1;
    }
    static_cast<void>(glyphs::has_feature(font, 0x6C696761U));
    for (int pass = 0; pass < 2; ++pass) {
        const detail::LayoutTable layout = detail::open_layout(font, pass == 0);
        const usize size = layout.table.size;
        // No more of anything than the table has room for.
        if (static_cast<usize>(detail::script_count(layout)) * 6 > size || static_cast<usize>(detail::feature_count(layout)) * 6 > size ||
            static_cast<usize>(detail::lookup_count(layout)) * 2 > size) {
            return false;
        }
        const u32 tags[] = {0x44464C54U, 0x6C61746EU, 0, 0xFFFFFFFFU};
        for (const u32 script_tag : tags) {
            usize script = 0;
            if (!detail::find_script(layout, script_tag, script)) continue;
            if (script >= size) return false;
            const u32 languages[] = {0, 0x54524B20U, 0xFFFFFFFFU};
            for (const u32 language : languages) {
                const detail::LanguageSystem system = detail::language_system(layout, script, language);
                if (!system.found) continue;
                if (system.features + static_cast<usize>(system.feature_count) * 2 > size || system.required_feature > 0xFFFF) return false;
                for (u32 index = 0; index < system.feature_count; ++index) {
                    if (detail::language_feature(layout, system, index) > 0xFFFF) return false;
                }
                if (detail::language_feature(layout, system, system.feature_count) != detail::no_feature) return false;
                checked = checked + 1;
            }
        }
        for (u32 index = 0; index < detail::feature_count(layout); ++index) {
            detail::LayoutFeature feature{};
            if (!detail::feature_at(layout, index, feature)) continue;
            if (feature.lookups + static_cast<usize>(feature.lookup_count) * 2 > size) return false;
            for (u32 lookup = 0; lookup < feature.lookup_count; ++lookup) {
                if (detail::feature_lookup(layout, feature, lookup) > 0xFFFF) return false;
            }
            checked = checked + 1;
        }
        for (u32 index = 0; index < detail::lookup_count(layout); ++index) {
            detail::LayoutLookup lookup{};
            if (!detail::lookup_at(layout, index, lookup)) continue;
            if (lookup.place >= size || lookup.place + 6 + static_cast<usize>(lookup.subtable_count) * 2 > size) return false;
            const u32 extension = pass == 0 ? detail::substitution_extension : detail::position_extension;
            if (lookup.kind == extension) return false;
            for (u32 number = 0; number < lookup.subtable_count; ++number) {
                usize subtable = 0;
                if (!detail::subtable_at(layout, lookup, number, subtable)) continue;
                if (subtable + 2 > size) return false;
                for (u32 glyph = 0; glyph < 6; ++glyph) {
                    const u32 covered = detail::coverage_index(layout.table, subtable, glyph);
                    if (covered != detail::not_covered && covered > 0x1FFFE) return false;
                    if (detail::class_of(layout.table, subtable, glyph) > 0xFFFF) return false;
                }
                checked = checked + 1;
            }
        }
    }
    return true;
}

void layout_tables() {
    const Bytes definitions = definitions_bytes();
    const Bytes layout_table = substitutions_bytes();
    std::uint64_t checked = 0;
    std::uint64_t tried = 0;
    // As they should be, the tables are read as the layout test has them;
    // here only that the promises hold, and that something was read.
    CHECK(layout_keeps_its_promises(definitions, layout_table, checked) && checked > 20);
    bool kept = true;
    const u32 values[] = {0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF};
    for (int which = 0; which < 2 && kept; ++which) {
        const Bytes& source = which == 0 ? definitions : layout_table;
        for (usize length = 0; length < source.size() && kept; ++length) {
            const Bytes prefix(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(length));
            kept = which == 0 ? layout_keeps_its_promises(prefix, layout_table, checked) : layout_keeps_its_promises(definitions, prefix, checked);
            tried = tried + 1;
        }
        for (usize at = 0; at + 1 < source.size() && kept; at = at + 2) {
            for (const u32 value : values) {
                Bytes changed = source;
                changed[at] = static_cast<u8>(value >> 8);
                changed[at + 1] = static_cast<u8>(value & 0xFFU);
                kept = kept && (which == 0 ? layout_keeps_its_promises(changed, layout_table, checked) : layout_keeps_its_promises(definitions, changed, checked));
                tried = tried + 1;
            }
        }
    }
    CHECK(kept);
    seed(0x452821E638D01377ULL);
    for (int round = 0; round < 30000 && kept; ++round) {
        Bytes changed_definitions = definitions;
        Bytes changed_layout = layout_table;
        const usize changes = 1 + next_random() % 4;
        for (usize change = 0; change < changes; ++change) {
            Bytes& target = next_random() % 3 == 0 ? changed_definitions : changed_layout;
            target[next_random() % target.size()] = static_cast<u8>(next_random() & 0xFFU);
        }
        kept = layout_keeps_its_promises(changed_definitions, changed_layout, checked);
        tried = tried + 1;
    }
    CHECK(kept);
    std::printf("search of layout tables: %llu tables, %llu answers checked\n", static_cast<unsigned long long>(tried), static_cast<unsigned long long>(checked));
    CHECK(tried > 30000);
}

} // namespace

int main() {
    wrong_maps();
    search_maps();
    advances_and_outlines();
    layout_tables();
    // The checked program (CMakeLists.txt, stxlib_glyphs_checked) is this
    // test on glyph code that checks every structure whole at every exit.
    std::printf("glyph checks: %s\n", glyphs::glyph_checks_enabled() ? "on" : "off");
#if defined(STXLIB_GLYPHS_CHECKS_EXPECTED)
    CHECK(glyphs::glyph_checks_enabled());
#endif
    const int status = stxlib_tests::finish("stxlib_glyph_untrusted_tests");
    return status;
}
