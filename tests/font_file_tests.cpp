// stxlib::glyphs, the font reader (include/stxlib/font_file.hpp), against
// fonts this test builds in memory, byte by byte, so that every expected
// number is the test's own.
//
//   P1  a TrueType font: the face's numbers, advances, simple glyphs with
//       every way a flag and a coordinate are stored, composite glyphs by
//       offset, by scale, by two-by-two, by matched points, nested, with a
//       scaled offset; the outline placed by the left side bearing;
//   P2  the face's height by each of its three sources;
//   P3  character maps of formats 0, 4, 6 and 12, and which one is taken;
//       going through a map a stretch at a time meets what glyph_of has;
//   P4  a CFF font, plain and with font dictionaries: lines, curves, flex,
//       stems and masks, local and global subroutines, the point that
//       repeats a contour's start left out, fractions dropped;
//   P5  scaled loading: parts and offsets rounded on their own;
//   P6  to_segments: contours that begin on and off the curve, runs of
//       control points, cubic curves, and what it refuses;
//   P7  refusals and limits: not a font, tables cut short, a glyph that
//       contains itself, points, contours and steps beyond the limit;
//   P8  robustness: every prefix of each font and many copies with bytes
//       changed are opened, mapped and loaded; each either succeeds or is
//       refused (the sanitizer builds are what make this a memory check).
//       The same fonts go through a Face, which is how an application's own
//       font is met: opened, warmed, fitted in each of the three fits (so
//       the store, the hinter and the rasterizer read them), measured,
//       drawn (upright, turned 17 degrees and a quarter turn) and asked
//       for ink boxes; each call refuses or succeeds, and a run's mask is
//       as large as its box says.
//
// The bound of P8. Four fonts built here (two TrueType, one with character
// maps of formats 4 and 12; two CFF, by dictionary and not). For each:
// every prefix, which is every table cut short and every table gone;
// every byte in turn set to each of 0x00, 0xFF, 0x7F, 0x80 and 0x01, which
// reaches every field of the table directory, head, maxp, hhea, hmtx, OS/2,
// cmap, loca, glyf and CFF; and 60,000 copies with one to four bytes
// changed, from a fixed seed (xorshift64, below), so the cases are the same
// on every run and host. Every prefix, every single-byte change and every
// fourth of the copies also goes through a Face. The counts are printed and
// checked. It is a search of those cases and no proof of the rest.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_face.hpp"

#include "font_builder.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace glyphs = stxlib::glyphs;
using stxlib::i32;
using stxlib::i64;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

using font_builder::Bytes;
using font_builder::put8;
using font_builder::put16;
using font_builder::put32;
using font_builder::append;
using font_builder::NamedTable;
using font_builder::font_of;
using font_builder::head_table;
using font_builder::maxp_table;
using font_builder::hhea_table;
using font_builder::os2_table;
using font_builder::Spot;
using font_builder::simple_glyph;
using font_builder::Part;
using font_builder::composite_glyph;
using font_builder::Metric;
using font_builder::glyph_tables;
using font_builder::view_of;
using font_builder::Segment4;
using font_builder::map_format_4;
using font_builder::Group12;
using font_builder::map_format_12;
using font_builder::map_format_6;
using font_builder::map_format_0;
using font_builder::MapRecord;
using font_builder::cmap_table;

struct Expected final {
    i32 x{0};
    i32 y{0};
    glyphs::PointTag tag{glyphs::PointTag::on};
};

[[nodiscard]] bool outline_is(const glyphs::GlyphOutline& outline, const std::vector<Expected>& points, const std::vector<u32>& ends) {
    if (outline.points.count() != points.size() || outline.tags.count() != points.size() || outline.contour_ends.count() != ends.size()) {
        std::fprintf(stderr, "  outline has %zu points and %zu contours, expected %zu and %zu\n", outline.points.count(),
                     outline.contour_ends.count(), points.size(), ends.size());
        return false;
    }
    for (usize index = 0; index < points.size(); ++index) {
        if (outline.points[index].x != points[index].x || outline.points[index].y != points[index].y || outline.tags[index] != points[index].tag) {
            std::fprintf(stderr, "  point %zu is (%d, %d, %d), expected (%d, %d, %d)\n", index, outline.points[index].x, outline.points[index].y,
                         static_cast<int>(outline.tags[index]), points[index].x, points[index].y, static_cast<int>(points[index].tag));
            return false;
        }
    }
    for (usize index = 0; index < ends.size(); ++index) {
        if (outline.contour_ends[index] != ends[index]) return false;
    }
    return true;
}


constexpr glyphs::PointTag on = glyphs::PointTag::on;
constexpr glyphs::PointTag quadratic = glyphs::PointTag::quadratic;
constexpr glyphs::PointTag cubic = glyphs::PointTag::cubic;


// ---- the TrueType font ---------------------------------------------------------------------

struct Built final {
    Bytes bytes{};
};

[[nodiscard]] Bytes truetype_font(const Bytes& os2, const Bytes& hhea, const Bytes& cmap, const bool long_offsets,
                                  const std::vector<NamedTable>& extra = {}) {
    std::vector<Bytes> glyph_bytes{};
    // 0: nothing.
    glyph_bytes.push_back(Bytes{});
    // 1: a square; its bearing is its box's left edge.
    glyph_bytes.push_back(simple_glyph({{{0, 0, true}, {0, 101, true}, {101, 101, true}, {101, 0, true}}}));
    // 2: curves, two contours, a contour that begins off the curve, steps
    // that are short, long and nothing, and flags that repeat. Its bearing
    // is five to the left of its box's left edge.
    glyph_bytes.push_back(simple_glyph({{{20, 0, false}, {300, 0, true}, {300, 400, false}, {-25, 400, false}, {-25, 75, true}},
                                        {{50, 50, true}, {50, 51, true}, {50, 52, true}, {50, 53, true}, {600, -700, true}}}));
    // 3: the square twice: moved by words, and halved and moved by bytes.
    glyph_bytes.push_back(composite_glyph(0, {{0x0003, 1, 200, 50, {}}, {0x000A, 1, 10, -5, {0x2000}}}));
    // 4: the square, and the square with its first point on the first
    // square's third.
    glyph_bytes.push_back(composite_glyph(0, {{0x0002, 1, 0, 0, {}}, {0x0000, 1, 2, 0, {}}}));
    // 5: glyph 3 moved, and glyph 2 by two scales.
    glyph_bytes.push_back(composite_glyph(0, {{0x0003, 3, -300, 7, {}}, {0x0042, 2, 0, 0, {0x4000, -0x2000}}}));
    // 6: itself.
    glyph_bytes.push_back(composite_glyph(0, {{0x0002, 6, 1, 1, {}}}));
    // 7: the square by a two-by-two, its offset in the part's own measure.
    glyph_bytes.push_back(composite_glyph(0, {{0x0883, 1, 100, 60, {0x2000, 0x1000, -0x1000, 0x2000}}}));
    const std::vector<Metric> metrics = {{500, 0}, {600, 0}, {700, -30}, {800, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
    Bytes glyf{};
    Bytes loca{};
    Bytes hmtx{};
    glyph_tables(glyph_bytes, metrics, 4, long_offsets, glyf, loca, hmtx);
    Bytes adjusted = hhea;
    adjusted[35] = 4;
    std::vector<NamedTable> tables = {{"OS/2", os2},
                                      {"cmap", cmap},
                                      {"glyf", glyf},
                                      {"head", head_table(1000, long_offsets)},
                                      {"hhea", adjusted},
                                      {"hmtx", hmtx},
                                      {"loca", loca},
                                      {"maxp", maxp_table(8)}};
    for (const NamedTable& table : extra) tables.push_back(table);
    const Bytes font = font_of(0x00010000U, tables);
    return font;
}

[[nodiscard]] Bytes plain_cmap() {
    const Bytes result = cmap_table({{3, 1, map_format_4({{'A', 'C', 1 - 'A', {}}, {'x', 'y', 0, {5, 2}}, {0xFFFF, 0xFFFF, 1, {}}})}});
    return result;
}

// a * b / 65536 rounded, halves away from zero: the test's own.
[[nodiscard]] i32 times(const i64 a, const i64 b) {
    const i64 product = a * b;
    const i64 magnitude = product < 0 ? -product : product;
    const i64 rounded = (magnitude + 0x8000) / 0x10000;
    const i32 result = static_cast<i32>(product < 0 ? -rounded : rounded);
    return result;
}

void truetype() {
    const Bytes os2 = os2_table(3, 0, 700, -200, 90, 950, 250);
    for (int form = 0; form < 2; ++form) {
        const Bytes bytes = truetype_font(os2, hhea_table(800, -220, 50, 4), plain_cmap(), form == 1);
        glyphs::Font font{};
        CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
        CHECK(font.glyph_count == 8 && font.units_per_em == 1000 && font.outlines == glyphs::OutlineKind::truetype);
        CHECK(font.ascender == 800 && font.descender == -220 && font.height == 1070);
        CHECK(glyphs::advance_of(font, 0) == 500 && glyphs::advance_of(font, 2) == 700 && glyphs::advance_of(font, 3) == 800);
        // Past the last advance: the last one. Past the last glyph: none.
        CHECK(glyphs::advance_of(font, 6) == 800 && glyphs::advance_of(font, 8) == 0);
        CHECK(glyphs::glyph_of(font, 'A') == 1 && glyphs::glyph_of(font, 'C') == 3 && glyphs::glyph_of(font, 'D') == 0);
        CHECK(glyphs::glyph_of(font, 'x') == 5 && glyphs::glyph_of(font, 'y') == 2 && glyphs::glyph_of(font, 'z') == 0);
        CHECK(glyphs::glyph_of(font, 0xFFFF) == 0 && glyphs::glyph_of(font, 0x10000 + 'A') == 0);
        const glyphs::FontLimits limits{};
        glyphs::GlyphOutline outline{};
        CHECK(glyphs::load_glyph(font, 0, limits, outline).succeeded() && outline.points.empty() && outline.contour_ends.empty());
        CHECK(glyphs::load_glyph(font, 1, limits, outline).succeeded());
        CHECK(outline_is(outline, {{0, 0, on}, {0, 101, on}, {101, 101, on}, {101, 0, on}}, {3}));
        // Glyph 2: its box's left edge is at -25 and its bearing is -30, so
        // it moves left by five, and the edge is at the bearing.
        CHECK(glyphs::load_glyph(font, 2, limits, outline).succeeded());
        CHECK(outline_is(outline,
                         {{15, 0, quadratic}, {295, 0, on}, {295, 400, quadratic}, {-30, 400, quadratic}, {-30, 75, on},
                          {45, 50, on}, {45, 51, on}, {45, 52, on}, {45, 53, on}, {595, -700, on}},
                         {4, 9}));
        // Glyph 3: moved by (200, 50); halved (halves away from zero) and
        // moved by (10, -5).
        CHECK(glyphs::load_glyph(font, 3, limits, outline).succeeded());
        CHECK(outline_is(outline,
                         {{200, 50, on}, {200, 151, on}, {301, 151, on}, {301, 50, on},
                          {10, -5, on}, {10, 46, on}, {61, 46, on}, {61, -5, on}},
                         {3, 7}));
        // Glyph 4: the second square's first point on (101, 101).
        CHECK(glyphs::load_glyph(font, 4, limits, outline).succeeded());
        CHECK(outline_is(outline,
                         {{0, 0, on}, {0, 101, on}, {101, 101, on}, {101, 0, on},
                          {101, 101, on}, {101, 202, on}, {202, 202, on}, {202, 101, on}},
                         {3, 7}));
        // Glyph 5: glyph 3 (already put together) moved by (-300, 7), then
        // glyph 2 as its own file has it (no bearing of its own applies
        // inside another glyph), x by one and y by minus a half.
        CHECK(glyphs::load_glyph(font, 5, limits, outline).succeeded());
        CHECK(outline.points.count() == 18 && outline.contour_ends.count() == 4);
        CHECK(outline.points[0].x == -100 && outline.points[0].y == 57 && outline.points[4].x == -290 && outline.points[4].y == 2);
        CHECK(outline.points[8].x == 20 && outline.points[8].y == 0 && outline.points[12].x == -25 && outline.points[12].y == times(75, -0x8000));
        CHECK(outline.points[17].x == 600 && outline.points[17].y == 350);
        // Glyph 7: by the two-by-two, and the offset by the lengths of its
        // rows (the greater number and three eighths of the lesser).
        CHECK(glyphs::load_glyph(font, 7, limits, outline).succeeded());
        const i64 xx = 0x2000 * 4;
        const i64 yx = 0x1000 * 4;
        const i64 xy = -0x1000 * 4;
        const i64 yy = 0x2000 * 4;
        const i32 dx = times(100, xx + ((3 * (-xy)) >> 3));
        const i32 dy = times(60, yy + ((3 * yx) >> 3));
        CHECK(outline_is(outline,
                         {{dx, dy, on},
                          {times(0, xx) + times(101, xy) + dx, times(0, yx) + times(101, yy) + dy, on},
                          {times(101, xx) + times(101, xy) + dx, times(101, yx) + times(101, yy) + dy, on},
                          {times(101, xx) + dx, times(101, yx) + dy, on}},
                         {3}));
        // Glyph 6 contains itself: it ends at the depth allowed.
        const stxlib::OperationStatus itself = glyphs::load_glyph(font, 6, limits, outline);
        CHECK(itself.failed() && (*itself.error()).kind() == stxlib::ErrorKind::limit_exceeded);
        // Scaled: at 16 pixels to the em, each part and offset on its own.
        const i64 scale = glyphs::scale_of(font, 16 * 64);
        CHECK(scale == (16 * 64 * 65536 + 500) / 1000);
        CHECK(glyphs::load_scaled_glyph(font, 3, scale, scale, limits, outline).succeeded());
        CHECK(outline.points[2].x == times(101, scale) + times(200, scale) && outline.points[2].y == times(101, scale) + times(50, scale));
        CHECK(outline.points[6].x == times(times(101, scale), 0x8000) + times(10, scale));
        CHECK(outline.points[6].y == times(times(101, scale), 0x8000) + times(-5, scale));
        CHECK(glyphs::load_scaled_glyph(font, 2, scale, scale, limits, outline).succeeded());
        // The move by the bearing is scaled on its own too.
        CHECK(outline.points.count() == 10 && outline.points[1].x == times(300, scale) - times(5, scale));
        // Limits.
        glyphs::FontLimits few{};
        few.points = 7;
        stxlib::OperationStatus status = glyphs::load_glyph(font, 3, few, outline);
        CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
        few = glyphs::FontLimits{};
        few.contours = 1;
        status = glyphs::load_glyph(font, 3, few, outline);
        CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
        few = glyphs::FontLimits{};
        few.components = 1;
        status = glyphs::load_glyph(font, 3, few, outline);
        CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
        status = glyphs::load_glyph(font, 8, limits, outline);
        CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument);
    }
}

void heights() {
    const Bytes cmap = plain_cmap();
    glyphs::Font font{};
    // The font says to use the typographic values (version 4, bit 7).
    Bytes bytes = truetype_font(os2_table(4, 0x80, 700, -200, 90, 950, 250), hhea_table(800, -220, 50, 4), cmap, false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.ascender == 700 && font.descender == -200 && font.height == 990);
    // The bit before version 4 means nothing.
    bytes = truetype_font(os2_table(3, 0x80, 700, -200, 90, 950, 250), hhea_table(800, -220, 50, 4), cmap, false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.ascender == 800 && font.height == 1070);
    // hhea has none: the typographic values.
    bytes = truetype_font(os2_table(3, 0, 700, -200, 90, 950, 250), hhea_table(0, 0, 50, 4), cmap, false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.ascender == 700 && font.descender == -200 && font.height == 990);
    // Nor has that: the Windows values, the descent counted downward.
    bytes = truetype_font(os2_table(3, 0, 0, 0, 90, 950, 250), hhea_table(0, 0, 50, 4), cmap, false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.ascender == 950 && font.descender == -250 && font.height == 1200);
}

// Going through the map a stretch at a time meets exactly the characters
// glyph_of has a glyph for, each with that glyph, in order.
[[nodiscard]] bool walk_is_glyph_of(const glyphs::Font& font) {
    u32 character = 0;
    u32 next_to_check = 0;
    bool same = true;
    for (u32 step = 0; step < 200000 && same; ++step) {
        glyphs::MappedStretch stretch{};
        if (!glyphs::next_stretch(font, character, stretch)) break;
        if (stretch.last < stretch.first || stretch.last < character) return false;
        const u32 from = stretch.first > character ? stretch.first : character;
        // Nothing between the stretches has a glyph.
        for (u32 between = next_to_check; between < from && same; ++between) same = glyphs::glyph_of(font, between) == 0;
        for (u32 mapped = from; mapped <= stretch.last && same; ++mapped) {
            same = glyphs::glyph_in_stretch(font, stretch, mapped) == glyphs::glyph_of(font, mapped);
            if (mapped == 0xFFFFFFFFU) break;
        }
        // A character outside the stretch has no glyph by it.
        same = same && (stretch.first == 0 || glyphs::glyph_in_stretch(font, stretch, stretch.first - 1) == 0);
        next_to_check = stretch.last + 1;
        if (stretch.last >= 0x10FFFF) break;
        character = stretch.last + 1;
    }
    for (u32 after = next_to_check; after <= 0x30000 && same; ++after) same = glyphs::glyph_of(font, after) == 0;
    return same;
}

void character_maps() {
    const Bytes os2 = os2_table(3, 0, 700, -200, 90, 950, 250);
    const Bytes hhea = hhea_table(800, -220, 50, 4);
    const Bytes four = map_format_4({{'A', 'C', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}});
    const Bytes twelve = map_format_12({{'A', 'B', 3}, {0x1F600, 0x1F602, 5}, {0x20000, 0x20000, 70000}});
    glyphs::Font font{};
    // All of Unicode is taken before the first 65,536 characters, wherever
    // each is in the table.
    Bytes bytes = truetype_font(os2, hhea, cmap_table({{3, 1, four}, {3, 10, twelve}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_format == 12);
    CHECK(walk_is_glyph_of(font));
    CHECK(glyphs::glyph_of(font, 'A') == 3 && glyphs::glyph_of(font, 'B') == 4 && glyphs::glyph_of(font, 'C') == 0);
    CHECK(glyphs::glyph_of(font, 0x1F600) == 5 && glyphs::glyph_of(font, 0x1F602) == 7 && glyphs::glyph_of(font, 0x1F603) == 0);
    // A glyph the font has not is no glyph.
    CHECK(glyphs::glyph_of(font, 0x20000) == 0 && glyphs::glyph_of(font, 0x10FFFF) == 0 && glyphs::glyph_of(font, 0xFFFFFFFFU) == 0);
    bytes = truetype_font(os2, hhea, cmap_table({{0, 4, twelve}, {3, 1, four}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_format == 12);
    CHECK(walk_is_glyph_of(font));
    // Windows' before Unicode's of the same reach; a map for another
    // platform is not taken.
    bytes = truetype_font(os2, hhea, cmap_table({{0, 3, map_format_6('a', {2, 3})}, {1, 0, map_format_0('A', 7)}, {3, 1, four}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_format == 4 && glyphs::glyph_of(font, 'B') == 2);
    CHECK(walk_is_glyph_of(font));
    bytes = truetype_font(os2, hhea, cmap_table({{1, 0, map_format_0('A', 7)}, {0, 3, map_format_6('a', {2, 3})}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_format == 6);
    CHECK(walk_is_glyph_of(font));
    CHECK(glyphs::glyph_of(font, 'a') == 2 && glyphs::glyph_of(font, 'b') == 3 && glyphs::glyph_of(font, 'c') == 0 && glyphs::glyph_of(font, 'A') == 0);
    bytes = truetype_font(os2, hhea, cmap_table({{0, 3, map_format_0('Q', 6)}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_format == 0);
    CHECK(walk_is_glyph_of(font));
    CHECK(glyphs::glyph_of(font, 'Q') == 6 && glyphs::glyph_of(font, 'R') == 0 && glyphs::glyph_of(font, 256 + 'Q') == 0);
    // No map it reads: every character is no glyph, and the font opens.
    bytes = truetype_font(os2, hhea, cmap_table({{1, 1, map_format_0('A', 7)}, {3, 4, map_format_0('A', 7)}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && glyphs::glyph_of(font, 'A') == 0);
    CHECK(walk_is_glyph_of(font));
}

// ---- the CFF font --------------------------------------------------------------------------

void number(Bytes& code, const i32 value) {
    if (value >= -107 && value <= 107) {
        put8(code, static_cast<u32>(value + 139));
    } else if (value >= 108 && value <= 1131) {
        put8(code, static_cast<u32>(247 + (value - 108) / 256));
        put8(code, static_cast<u32>((value - 108) % 256));
    } else if (value >= -1131 && value <= -108) {
        put8(code, static_cast<u32>(251 + (-value - 108) / 256));
        put8(code, static_cast<u32>((-value - 108) % 256));
    } else {
        put8(code, 28);
        put16(code, value);
    }
}

// A charstring from numbers and operators: an operator is 1000 + its byte
// (2000 + the second byte of a two-byte one), a 16.16 number 100000 + its
// index in `fixed`.
[[nodiscard]] Bytes charstring(const std::vector<i32>& items, const std::vector<u32>& fixed = {}) {
    Bytes code{};
    for (const i32 item : items) {
        if (item >= 100000) {
            put8(code, 255);
            put32(code, fixed[static_cast<usize>(item - 100000)]);
        } else if (item >= 2000) {
            put8(code, 12);
            put8(code, static_cast<u32>(item - 2000));
        } else if (item >= 1000) {
            put8(code, static_cast<u32>(item - 1000));
        } else {
            number(code, item);
        }
    }
    return code;
}

[[nodiscard]] Bytes index_of(const std::vector<Bytes>& objects) {
    Bytes index{};
    put16(index, static_cast<i32>(objects.size()));
    if (objects.empty()) return index;
    put8(index, 2);
    usize offset = 1;
    put16(index, static_cast<i32>(offset));
    for (const Bytes& object : objects) {
        offset = offset + object.size();
        put16(index, static_cast<i32>(offset));
    }
    for (const Bytes& object : objects) append(index, object);
    return index;
}

// A dictionary operand five bytes long, so that a dictionary's length does
// not depend on the offsets in it.
void operand(Bytes& dictionary, const usize value) {
    put8(dictionary, 29);
    put32(dictionary, static_cast<u32>(value));
}

constexpr i32 rmoveto = 1021;
constexpr i32 hmoveto = 1022;
constexpr i32 rlineto = 1005;
constexpr i32 hlineto = 1006;
constexpr i32 vlineto = 1007;
constexpr i32 rrcurveto = 1008;
constexpr i32 callsubr = 1010;
constexpr i32 return_ = 1011;
constexpr i32 endchar = 1014;
constexpr i32 hstemhm = 1018;
constexpr i32 hintmask = 1019;
constexpr i32 callgsubr = 1029;
constexpr i32 vhcurveto = 1030;
constexpr i32 hvcurveto = 1031;
constexpr i32 hflex = 2034;

[[nodiscard]] std::vector<Bytes> cff_glyphs() {
    std::vector<Bytes> list{};
    // 0: nothing.
    list.push_back(charstring({endchar}));
    // 1: a box left open: its last side is the closing of the contour.
    list.push_back(charstring({10, 20, rmoveto, 100, hlineto, 100, vlineto, -100, hlineto, endchar}));
    // 2: the same box drawn shut: the point back on the start is left out.
    list.push_back(charstring({10, 20, rmoveto, 100, 100, -100, -100, hlineto, endchar}));
    // 3: a curve, then a line back to the start (left out), then a second
    // contour begun by a move; a width first.
    list.push_back(charstring({450, 0, 0, rmoveto, 10, 30, 30, 10, 20, 0, rrcurveto, -60, -40, rlineto, 5, hmoveto, 7, 7, rlineto, endchar}));
    // 4: a curve that ends on the start: its two control points stay.
    list.push_back(charstring({0, 0, rmoveto, 50, 0, rlineto, 0, 50, -50, 0, 0, -50, rrcurveto, endchar}));
    // 5: a width, stems, a mask over three stems, then a local and a
    // global subroutine that each draw a side.
    Bytes masked = charstring({50, 0, 10, 20, 5, hstemhm, 0, 20, hintmask});
    put8(masked, 0xE0);
    append(masked, charstring({5, 5, rmoveto, -107, callsubr, -107, callgsubr, endchar}));
    list.push_back(masked);
    // 6: a flex along a row.
    list.push_back(charstring({0, 0, rmoveto, 10, 10, 5, 10, 10, 10, 10, hflex, endchar}));
    // 7: curves that begin along one axis and end along the other.
    list.push_back(charstring({0, 0, rmoveto, 10, 20, 30, 40, hvcurveto, 5, 6, 7, 8, 9, vhcurveto, endchar}));
    // 8: fractions: one and a half, and minus one and a half.
    list.push_back(charstring({100000, 100001, rmoveto, 10, 10, rlineto, endchar}, {0x00018000U, 0xFFFE8000U}));
    // 9: a line that goes nowhere is left out; a move with nothing drawn
    // from it leaves nothing.
    list.push_back(charstring({3, 4, rmoveto, 0, 0, rlineto, 6, 0, rlineto, 0, 0, rlineto, 9, 9, rmoveto, endchar}));
    // 10: an operator that is not one, which clears the stack.
    list.push_back(charstring({0, 0, rmoveto, 1000, endchar}));
    // 11: a subroutine that is not there.
    list.push_back(charstring({0, 0, rmoveto, 5, callsubr, endchar}));
    // 12: the accented-character form of endchar.
    list.push_back(charstring({0, 0, 65, 66, endchar}));
    return list;
}

[[nodiscard]] Bytes cff_table(const bool by_dictionary, const std::vector<Bytes>& glyph_list) {
    const Bytes header = {1, 0, 4, 1};
    const Bytes names = index_of({Bytes{'T'}});
    const Bytes strings = index_of({});
    const Bytes global = index_of({charstring({20, vlineto, return_})});
    const Bytes charstrings = index_of(glyph_list);
    const Bytes local_a = index_of({charstring({20, hlineto, return_})});
    const Bytes local_b = index_of({charstring({40, hlineto, return_})});
    // A private dictionary: its subroutines are right after it.
    Bytes private_dictionary{};
    operand(private_dictionary, 6);
    put8(private_dictionary, 19);
    // The top dictionary's operands are all five bytes long, so its length
    // is known before the offsets it holds are.
    const usize top_length = by_dictionary ? 3 + 2 + 6 + 7 + 7 : 6 + 11;
    const usize top_index_length = 2 + 1 + 8 + top_length;
    const usize charstrings_at = header.size() + names.size() + top_index_length + strings.size() + global.size();
    const usize private_at = charstrings_at + charstrings.size();
    const usize second_private_at = private_at + private_dictionary.size() + local_a.size();
    Bytes top{};
    Bytes tail{};
    if (by_dictionary) {
        // Two font dictionaries, each with a private dictionary; glyphs 0
        // to 4 use the first and the rest the second.
        Bytes first_dictionary{};
        operand(first_dictionary, private_dictionary.size());
        operand(first_dictionary, private_at);
        put8(first_dictionary, 18);
        Bytes second_dictionary{};
        operand(second_dictionary, private_dictionary.size());
        operand(second_dictionary, second_private_at);
        put8(second_dictionary, 18);
        const Bytes dictionaries = index_of({first_dictionary, second_dictionary});
        const usize dictionaries_at = second_private_at + private_dictionary.size() + local_b.size();
        const usize select_at = dictionaries_at + dictionaries.size();
        number(top, 0);
        number(top, 0);
        number(top, 0);
        put8(top, 12);
        put8(top, 30);
        operand(top, charstrings_at);
        put8(top, 17);
        operand(top, dictionaries_at);
        put8(top, 12);
        put8(top, 36);
        operand(top, select_at);
        put8(top, 12);
        put8(top, 37);
        append(tail, private_dictionary);
        append(tail, local_a);
        append(tail, private_dictionary);
        append(tail, local_b);
        append(tail, dictionaries);
        // Ranges: from glyph 0 the first dictionary, from glyph 5 the
        // second, and the glyph after the last.
        put8(tail, 3);
        put16(tail, 2);
        put16(tail, 0);
        put8(tail, 0);
        put16(tail, 5);
        put8(tail, 1);
        put16(tail, static_cast<i32>(glyph_list.size()));
    } else {
        operand(top, charstrings_at);
        put8(top, 17);
        operand(top, private_dictionary.size());
        operand(top, private_at);
        put8(top, 18);
        append(tail, private_dictionary);
        append(tail, local_a);
    }
    Bytes table = header;
    append(table, names);
    put16(table, 1);
    put8(table, 4);
    put32(table, 1);
    put32(table, static_cast<u32>(1 + top.size()));
    append(table, top);
    append(table, strings);
    append(table, global);
    append(table, charstrings);
    append(table, tail);
    CHECK(top.size() == top_length);
    return table;
}

[[nodiscard]] Bytes cff_font_of(const std::vector<Bytes>& glyph_list, const bool by_dictionary = false) {
    const usize glyph_count = glyph_list.size();
    Bytes hmtx{};
    for (usize index = 0; index < glyph_count; ++index) {
        put16(hmtx, static_cast<i32>(400 + index));
        put16(hmtx, 0);
    }
    Bytes hhea = hhea_table(880, -120, 0, static_cast<u32>(glyph_count));
    const Bytes font = font_of(0x4F54544FU, {{"CFF ", cff_table(by_dictionary, glyph_list)},
                                             {"OS/2", os2_table(3, 0, 700, -200, 90, 950, 250)},
                                             {"cmap", plain_cmap()},
                                             {"head", head_table(1000, false)},
                                             {"hhea", hhea},
                                             {"hmtx", hmtx},
                                             {"maxp", maxp_table(static_cast<u32>(glyph_count))}});
    return font;
}

[[nodiscard]] Bytes cff_font(const bool by_dictionary) {
    const Bytes font = cff_font_of(cff_glyphs(), by_dictionary);
    return font;
}

void cff() {
    for (int form = 0; form < 2; ++form) {
        const Bytes bytes = cff_font(form == 1);
        glyphs::Font font{};
        CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
        CHECK(font.outlines == glyphs::OutlineKind::cff && font.glyph_count == 13 && glyphs::advance_of(font, 3) == 403);
        const glyphs::FontLimits limits{};
        glyphs::GlyphOutline outline{};
        CHECK(glyphs::load_glyph(font, 0, limits, outline).succeeded() && outline.points.empty());
        CHECK(glyphs::load_glyph(font, 1, limits, outline).succeeded());
        CHECK(outline_is(outline, {{10, 20, on}, {110, 20, on}, {110, 120, on}, {10, 120, on}}, {3}));
        CHECK(glyphs::load_glyph(font, 2, limits, outline).succeeded());
        CHECK(outline_is(outline, {{10, 20, on}, {110, 20, on}, {110, 120, on}, {10, 120, on}}, {3}));
        CHECK(glyphs::load_glyph(font, 3, limits, outline).succeeded());
        CHECK(outline_is(outline, {{0, 0, on}, {10, 30, cubic}, {40, 40, cubic}, {60, 40, on}, {5, 0, on}, {12, 7, on}}, {3, 5}));
        CHECK(glyphs::load_glyph(font, 4, limits, outline).succeeded());
        CHECK(outline_is(outline, {{0, 0, on}, {50, 0, on}, {50, 50, cubic}, {0, 50, cubic}}, {3}));
        // Glyph 5's local subroutine is its own font dictionary's: a side
        // of 20, or of 40 where the second dictionary is used.
        const i32 side = form == 1 ? 40 : 20;
        CHECK(glyphs::load_glyph(font, 5, limits, outline).succeeded());
        CHECK(outline_is(outline, {{5, 5, on}, {5 + side, 5, on}, {5 + side, 25, on}}, {2}));
        CHECK(glyphs::load_glyph(font, 6, limits, outline).succeeded());
        CHECK(outline_is(outline, {{0, 0, on}, {10, 0, cubic}, {20, 5, cubic}, {30, 5, on}, {40, 5, cubic}, {50, 0, cubic}, {60, 0, on}}, {6}));
        CHECK(glyphs::load_glyph(font, 7, limits, outline).succeeded());
        CHECK(outline_is(outline, {{0, 0, on}, {10, 0, cubic}, {30, 30, cubic}, {30, 70, on}, {30, 75, cubic}, {36, 82, cubic}, {44, 91, on}}, {6}));
        // One and a half is one; minus one and a half is minus two.
        CHECK(glyphs::load_glyph(font, 8, limits, outline).succeeded());
        CHECK(outline_is(outline, {{1, -2, on}, {11, 8, on}}, {1}));
        CHECK(glyphs::load_glyph(font, 9, limits, outline).succeeded());
        CHECK(outline_is(outline, {{3, 4, on}, {9, 4, on}}, {1}));
        // An operator that is not one clears the stack (as read-fonts does):
        // a move with nothing drawn from it leaves nothing.
        CHECK(glyphs::load_glyph(font, 10, limits, outline).succeeded() && outline.points.empty());
        for (u32 glyph = 11; glyph < 13; ++glyph) {
            const stxlib::OperationStatus status = glyphs::load_glyph(font, glyph, limits, outline);
            CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument);
        }
        glyphs::FontLimits few{};
        few.charstring_steps = 5;
        stxlib::OperationStatus status = glyphs::load_glyph(font, 1, few, outline);
        CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
        few = glyphs::FontLimits{};
        few.points = 3;
        status = glyphs::load_glyph(font, 1, few, outline);
        CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
        // Scaled: whole font units, then the scale.
        const i64 scale = glyphs::scale_of(font, 13 * 64);
        CHECK(glyphs::load_scaled_glyph(font, 1, scale, scale, limits, outline).succeeded());
        CHECK(outline.points.count() == 4 && outline.points[2].x == times(110, scale) && outline.points[2].y == times(120, scale));
    }
}

// ---- the audit of 2026-11-16 (A-stxlib-glyphs-01): contracts ------------------------------

// What every opened font promises of its character map, whatever its bytes:
// glyph_of answers a glyph the font has, and a walk of the stretches ends,
// with each stretch in order and each of its glyphs one the font has.
[[nodiscard]] bool map_keeps_promises(const glyphs::Font& font) {
    for (u32 character = 0; character < 0x400; ++character) {
        if (glyphs::glyph_of(font, character) >= font.glyph_count && glyphs::glyph_of(font, character) != 0) return false;
    }
    const u32 far[] = {0xF041U, 0xFFFFU, 0x10000U, 0x1F600U, 0x10FFFFU, 0x110000U, 0xFFFFFFFFU};
    for (const u32 character : far) {
        if (glyphs::glyph_of(font, character) >= font.glyph_count && glyphs::glyph_of(font, character) != 0) return false;
    }
    u32 character = 0;
    for (u32 step = 0; step < 70000; ++step) {
        glyphs::MappedStretch stretch{};
        if (!glyphs::next_stretch(font, character, stretch)) return true;
        if (stretch.last < stretch.first || stretch.last < character) return false;
        const u32 glyph = glyphs::glyph_in_stretch(font, stretch, stretch.last);
        if (glyph != 0 && glyph >= font.glyph_count) return false;
        if (stretch.last >= 0x10FFFF) return true;
        character = stretch.last + 1;
    }
    // More stretches than a map of this size can hold: a walk that does not end.
    return false;
}

// The bounded search over malformed character maps. For each of an empty
// and a small map of formats 4, 6 and 12 (and an unsorted format 4), alone
// in the cmap table: every prefix of the map (the map's own length field
// left as it is), and every byte of the map set in turn to each of 0x00,
// 0x01, 0x7F, 0x80, 0xFE and 0xFF. Each font either is refused or opens
// and keeps map_keeps_promises. Returns the count of fonts tried.
[[nodiscard]] usize malformed_maps(const Bytes& os2, const Bytes& hhea) {
    std::vector<Bytes> maps{};
    maps.push_back(map_format_4({}));
    maps.push_back(map_format_4({{0xFFFF, 0xFFFF, 1, {}}}));
    maps.push_back(map_format_4({{'A', 'C', 1 - 'A', {}}, {'x', 'y', 0, {5, 2}}, {0xFFFF, 0xFFFF, 1, {}}}));
    maps.push_back(map_format_4({{'x', 'y', 0, {5, 2}}, {'A', 'C', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}}));
    maps.push_back(map_format_6('a', {}));
    maps.push_back(map_format_6('a', {2, 3, 7}));
    maps.push_back(map_format_12({}));
    maps.push_back(map_format_12({{'A', 'B', 3}, {0x1F600, 0x1F602, 5}}));
    const u8 values[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
    usize tried = 0;
    bool kept = true;
    for (const Bytes& map : maps) {
        for (usize length = 0; length <= map.size() && kept; ++length) {
            const Bytes cut(map.begin(), map.begin() + static_cast<std::ptrdiff_t>(length));
            const Bytes bytes = truetype_font(os2, hhea, cmap_table({{3, 1, cut}}), false);
            glyphs::Font font{};
            if (glyphs::open_font(view_of(bytes), 0, font).succeeded()) kept = map_keeps_promises(font);
            tried = tried + 1;
        }
        for (usize place = 0; place < map.size() && kept; ++place) {
            for (const u8 value : values) {
                Bytes changed = map;
                changed[place] = value;
                const Bytes bytes = truetype_font(os2, hhea, cmap_table({{3, 1, changed}, {3, 0, changed}}), false);
                glyphs::Font font{};
                if (glyphs::open_font(view_of(bytes), 0, font).succeeded()) kept = kept && map_keeps_promises(font);
                tried = tried + 1;
            }
        }
    }
    CHECK(kept);
    return tried;
}

void audit_contracts() {
    const Bytes os2 = os2_table(3, 0, 700, -200, 90, 950, 250);
    const Bytes hhea = hhea_table(800, -220, 50, 4);
    glyphs::Font font{};
    // 01-F4: format 4 segments out of order are read in order, as FreeType does.
    Bytes bytes = truetype_font(os2, hhea, cmap_table({{3, 1, map_format_4({{'x', 'y', 0, {5, 2}}, {'A', 'C', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}})}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_format == 4 && font.character_map_unsorted);
    CHECK(glyphs::glyph_of(font, 'A') == 1 && glyphs::glyph_of(font, 'C') == 3 && glyphs::glyph_of(font, 'x') == 5 &&
          glyphs::glyph_of(font, 'y') == 2 && glyphs::glyph_of(font, 'D') == 0 && glyphs::glyph_of(font, 'w') == 0);
    bytes = truetype_font(os2, hhea, cmap_table({{3, 1, map_format_4({{'A', 'C', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}})}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && !font.character_map_unsorted);
    // Empty maps of each format: the font opens and no character has a glyph.
    const Bytes empty_maps[] = {map_format_4({}), map_format_6('a', {}), map_format_12({})};
    for (const Bytes& empty : empty_maps) {
        bytes = truetype_font(os2, hhea, cmap_table({{3, 1, empty}}), false);
        CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && glyphs::glyph_of(font, 'a') == 0 && walk_is_glyph_of(font));
    }
    // 01-F11: of two maps of one kind, the later record's, as FreeType takes.
    bytes = truetype_font(os2, hhea, cmap_table({{3, 1, map_format_6('A', {2})}, {3, 1, map_format_6('A', {3})}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && glyphs::glyph_of(font, 'A') == 3);
    // 01-F6: a symbol map, by the character and in the private area; Mac
    // Roman's first 128 codes. Neither has stretches of Unicode.
    bytes = truetype_font(os2, hhea, cmap_table({{3, 0, map_format_4({{0xF041, 0xF042, 1 - 0xF041, {}}, {0xFFFF, 0xFFFF, 1, {}}})}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_kind == glyphs::character_map_symbol);
    CHECK(glyphs::glyph_of(font, 'A') == 1 && glyphs::glyph_of(font, 'B') == 2 && glyphs::glyph_of(font, 0xF041) == 1 &&
          glyphs::glyph_of(font, 'C') == 0 && glyphs::glyph_of(font, 0x10041) == 0);
    glyphs::MappedStretch stretch{};
    CHECK(!glyphs::next_stretch(font, 0, stretch));
    bytes = truetype_font(os2, hhea, cmap_table({{1, 0, map_format_0('A', 7)}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_kind == glyphs::character_map_mac_roman);
    CHECK(glyphs::glyph_of(font, 'A') == 7 && glyphs::glyph_of(font, 'B') == 0 && glyphs::glyph_of(font, 0xC4) == 0);
    // A Unicode map is taken before either.
    bytes = truetype_font(os2, hhea, cmap_table({{1, 0, map_format_0('A', 7)}, {3, 0, map_format_6(0xF041, {6})}, {3, 1, map_format_6('A', {2})}}), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.character_map_kind == glyphs::character_map_unicode &&
          glyphs::glyph_of(font, 'A') == 2);
    const usize maps_tried = malformed_maps(os2, hhea);
    std::printf("malformed character maps: %zu fonts\n", maps_tried);

    // 01-F3: loca past glyf, as FreeType reads it. Glyph 1 ends past glyf
    // and is not the last: empty. Glyph 2 ends before it begins (so at
    // glyf's end) and begins past glyf: empty. Glyph 3, the last, ends past
    // glyf and is cut there: the square.
    const Bytes square = simple_glyph({{{0, 0, true}, {0, 101, true}, {101, 101, true}, {101, 0, true}}});
    Bytes loca{};
    const i32 half = static_cast<i32>(square.size() / 2);
    for (const i32 entry : {0, 0, half + 50, 0, half + 500}) put16(loca, entry);
    Bytes hmtx{};
    for (i32 glyph = 0; glyph < 4; ++glyph) {
        put16(hmtx, 500 + glyph);
        put16(hmtx, 0);
    }
    bytes = font_of(0x00010000U, {{"OS/2", os2},
                                  {"cmap", plain_cmap()},
                                  {"glyf", square},
                                  {"head", head_table(1000, false)},
                                  {"hhea", hhea},
                                  {"hmtx", hmtx},
                                  {"loca", loca},
                                  {"maxp", maxp_table(4)}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
    const glyphs::FontLimits limits{};
    glyphs::GlyphOutline outline{};
    CHECK(glyphs::load_glyph(font, 1, limits, outline).succeeded() && outline.points.empty());
    CHECK(glyphs::load_glyph(font, 2, limits, outline).succeeded() && outline.points.empty());
    CHECK(glyphs::load_glyph(font, 3, limits, outline).succeeded() && outline.points.count() == 4);

    // 01-F5: an hmtx that runs past the file's end is read to the end: the
    // whole advances there are kept, and a glyph past them has the last.
    std::vector<NamedTable> tables = {{"OS/2", os2},
                                      {"cmap", plain_cmap()},
                                      {"glyf", square},
                                      {"head", head_table(1000, false)},
                                      {"hhea", hhea},
                                      {"loca", Bytes{0, 0, 0, 0, 0, 0, 0, 0, 0, static_cast<u8>(half)}},
                                      {"maxp", maxp_table(4)},
                                      {"hmtx", hmtx}};
    bytes = font_of(0x00010000U, tables);
    bytes.resize(bytes.size() - 2);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && glyphs::advance_of(font, 0) == 500 &&
          glyphs::advance_of(font, 2) == 502 && glyphs::advance_of(font, 3) == 502 && font.advance_count == 3);

    // 01-F1: a scaled outline beyond the coordinate bound is refused, not
    // narrowed, and the outline is left as it was.
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    CHECK(outline.points.push_copy(glyphs::Point{.x = 100000, .y = -100000}).succeeded() &&
          outline.tags.push_copy(on).succeeded() && outline.contour_ends.push_copy(0U).succeeded());
    stxlib::OperationStatus status = glyphs::scale_outline(outline, static_cast<i64>(1) << 32, static_cast<i64>(1) << 32);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument && outline.points[0].x == 100000);
    status = glyphs::scale_outline(outline, 0, 65536);
    CHECK(status.failed() && outline.points[0].x == 100000);
    status = glyphs::scale_outline(outline, 65536 * 2, 65536 * 3);
    CHECK(status.succeeded() && outline.points[0].x == 200000 && outline.points[0].y == -300000);
}

// ---- the face's names, OS/2, line metrics and collections (font tables for GUI.Forms) -------

struct NameRecord final {
    u32 platform{0};
    u32 encoding{0};
    u32 language{0};
    u32 id{0};
    Bytes text{};
};

// A name table of format 0 (or 1 with `tags` language tags, each pointing
// at the first record's text) holding the records' text in order.
[[nodiscard]] Bytes name_table(const std::vector<NameRecord>& records, const u32 format = 0, const u32 tags = 0) {
    Bytes table{};
    put16(table, static_cast<i32>(format));
    put16(table, static_cast<i32>(records.size()));
    const usize storage = 6 + records.size() * 12 + (format == 1 ? 2 + tags * 4 : 0);
    put16(table, static_cast<i32>(storage));
    usize offset = 0;
    for (const NameRecord& record : records) {
        put16(table, static_cast<i32>(record.platform));
        put16(table, static_cast<i32>(record.encoding));
        put16(table, static_cast<i32>(record.language));
        put16(table, static_cast<i32>(record.id));
        put16(table, static_cast<i32>(record.text.size()));
        put16(table, static_cast<i32>(offset));
        offset = offset + record.text.size();
    }
    if (format == 1) {
        put16(table, static_cast<i32>(tags));
        for (u32 tag = 0; tag < tags; ++tag) {
            put16(table, records.empty() ? 0 : static_cast<i32>(records[0].text.size()));
            put16(table, 0);
        }
    }
    for (const NameRecord& record : records) append(table, record.text);
    return table;
}

[[nodiscard]] Bytes utf16_of(const std::vector<u32>& units) {
    Bytes bytes{};
    for (const u32 unit : units) put16(bytes, static_cast<i32>(unit));
    return bytes;
}

[[nodiscard]] Bytes ascii_utf16(const std::string& text) {
    Bytes bytes{};
    for (const char letter : text) put16(bytes, static_cast<i32>(static_cast<u8>(letter)));
    return bytes;
}

[[nodiscard]] Bytes ascii_bytes(const std::string& text) {
    Bytes bytes{};
    for (const char letter : text) put8(bytes, static_cast<u8>(letter));
    return bytes;
}

[[nodiscard]] std::string text_of(const stxlib::Vector<char>& text) {
    std::string result{};
    for (usize index = 0; index < text.count(); ++index) result.push_back(text[index]);
    return result;
}

// Whether the bytes are well-formed UTF-8 of scalar values, with no U+0000.
[[nodiscard]] bool well_formed_utf8(const stxlib::Vector<char>& text) {
    usize index = 0;
    while (index < text.count()) {
        const u32 lead = static_cast<u8>(text[index]);
        usize length = 0;
        u32 value = 0;
        if (lead >= 0x01 && lead < 0x80) {
            length = 1;
            value = lead;
        } else if (lead >= 0xC2 && lead < 0xE0) {
            length = 2;
            value = lead & 0x1FU;
        } else if (lead >= 0xE0 && lead < 0xF0) {
            length = 3;
            value = lead & 0x0FU;
        } else if (lead >= 0xF0 && lead < 0xF5) {
            length = 4;
            value = lead & 0x07U;
        } else {
            return false;
        }
        if (index + length > text.count()) return false;
        for (usize more = 1; more < length; ++more) {
            const u32 next = static_cast<u8>(text[index + more]);
            if ((next & 0xC0U) != 0x80U) return false;
            value = (value << 6) | (next & 0x3FU);
        }
        const u32 least = length == 1 ? 0 : (length == 2 ? 0x80 : (length == 3 ? 0x800 : 0x10000));
        if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;
        index = index + length;
    }
    return true;
}

[[nodiscard]] std::string name_of(const glyphs::Font& font, const u32 id) {
    stxlib::Vector<char> text{};
    const stxlib::OperationStatus status = glyphs::font_name(font, id, text);
    CHECK(status.succeeded());
    return text_of(text);
}

// Whether the font's family name is asked without failing and is
// well-formed UTF-8.
[[nodiscard]] bool name_well_formed(const glyphs::Font& font) {
    stxlib::Vector<char> text{};
    const bool result = glyphs::family_name(font, text).succeeded() && well_formed_utf8(text);
    return result;
}

// A collection of the fonts given: each font's table offsets moved by where
// it is put.
[[nodiscard]] Bytes collection_of(const std::vector<Bytes>& fonts) {
    Bytes bytes{};
    put32(bytes, 0x74746366U);
    put32(bytes, 0x00010000U);
    put32(bytes, static_cast<u32>(fonts.size()));
    usize place = 12 + fonts.size() * 4;
    std::vector<usize> places{};
    for (const Bytes& font : fonts) {
        places.push_back(place);
        put32(bytes, static_cast<u32>(place));
        place = place + font.size();
    }
    for (usize index = 0; index < fonts.size(); ++index) {
        Bytes font = fonts[index];
        const usize tables = (static_cast<usize>(font[4]) << 8) | font[5];
        for (usize table = 0; table < tables; ++table) {
            const usize record = 12 + table * 16 + 8;
            const u32 offset = (static_cast<u32>(font[record]) << 24) | (static_cast<u32>(font[record + 1]) << 16) |
                               (static_cast<u32>(font[record + 2]) << 8) | static_cast<u32>(font[record + 3]);
            const u32 moved = offset + static_cast<u32>(places[index]);
            font[record] = static_cast<u8>(moved >> 24);
            font[record + 1] = static_cast<u8>(moved >> 16);
            font[record + 2] = static_cast<u8>(moved >> 8);
            font[record + 3] = static_cast<u8>(moved);
        }
        append(bytes, font);
    }
    return bytes;
}

// An OS/2 table of `size` bytes and `version`, with a weight, a width and
// fsSelection.
[[nodiscard]] Bytes os2_of(const u32 version, const usize size, const u32 weight, const u32 width, const u32 selection) {
    Bytes os2 = os2_table(version, selection, 700, -200, 90, 950, 250);
    os2.resize(size, 0);
    os2[4] = static_cast<u8>(weight >> 8);
    os2[5] = static_cast<u8>(weight);
    os2[6] = static_cast<u8>(width >> 8);
    os2[7] = static_cast<u8>(width);
    return os2;
}

void font_tables() {
    const Bytes hhea = hhea_table(800, -220, 50, 4);
    const Bytes os2 = os2_of(3, 96, 700, 5, 0x0021);
    glyphs::Font font{};
    // OS/2 and hhea, as the tables have them.
    Bytes bytes = truetype_font(os2, hhea, plain_cmap(), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
    CHECK(font.os2_version == 3 && font.weight_class == 700 && font.width_class == 5 && font.selection == 0x0021);
    CHECK(font.typo_ascender == 700 && font.typo_descender == -200 && font.typo_line_gap == 90 && font.win_ascent == 950 &&
          font.win_descent == 250);
    CHECK(font.hhea_ascender == 800 && font.hhea_descender == -220 && font.hhea_line_gap == 50);
    // An OS/2 too short for its version is none, as FreeType has it.
    bytes = truetype_font(os2_of(3, 78, 700, 5, 0x21), hhea, plain_cmap(), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.os2_version == 0xFFFFU && font.weight_class == 0);
    bytes = truetype_font(os2_of(0, 78, 400, 5, 0x40), hhea, plain_cmap(), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.os2_version == 0 && font.weight_class == 400 && font.selection == 0x40);
    bytes = truetype_font(os2_of(5, 96, 400, 5, 0x40), hhea, plain_cmap(), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.os2_version == 0xFFFFU);
    bytes = truetype_font(os2_of(5, 100, 300, 5, 0x40), hhea, plain_cmap(), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.os2_version == 5 && font.weight_class == 300);
    bytes = truetype_font(Bytes{}, hhea, plain_cmap(), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.os2_version == 0xFFFFU && font.selection == 0);

    // Names: Windows' English before Macintosh's; text decoded.
    const NameRecord windows_family{3, 1, 0x409, 1, utf16_of({'F', 'a', 'm', 0xE9, 0xD83D, 0xDE00, 0x4E2D})};
    const NameRecord mac_family{1, 0, 0, 1, ascii_bytes("MacFam\x8E")};
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({mac_family, windows_family})}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.names.length != 0);
    CHECK(name_of(font, 1) == "Fam\xC3\xA9\xF0\x9F\x98\x80\xE4\xB8\xAD" && name_of(font, 2).empty());
    // A Windows name not in English gives way to a Macintosh one (read as Mac Roman).
    const NameRecord german_family{3, 1, 0x407, 1, ascii_utf16("Familie")};
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({german_family, mac_family})}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && name_of(font, 1) == "MacFam\xC3\xA9");
    // Alone, it is taken; English of another country is English.
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({german_family})}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && name_of(font, 1) == "Familie");
    const NameRecord british_family{3, 1, 0x809, 1, ascii_utf16("British")};
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({british_family, german_family, mac_family})}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && name_of(font, 1) == "British");
    // The Unicode platform's last; a lone surrogate is U+FFFD; text ends at U+0000.
    const NameRecord unicode_family{0, 3, 0, 1, utf16_of({'U', 0xDC00, 'x', 0, 'y'})};
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({unicode_family})}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && name_of(font, 1) == "U\xEF\xBF\xBDx");
    // A record whose text is outside the table, or empty, is passed over.
    Bytes cut = name_table({windows_family, german_family});
    cut[6 + 10] = 0x7F;
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", cut}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && name_of(font, 1) == "Familie");
    const NameRecord empty_family{3, 1, 0x409, 1, Bytes{}};
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({german_family, empty_family})}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && name_of(font, 1) == "Familie");
    // Format 1: a record of a language tag that does not exist is passed over.
    const NameRecord tagged{3, 1, 0x8001, 1, ascii_utf16("Tagged")};
    const NameRecord tagged_ok{3, 1, 0x8000, 2, ascii_utf16("Ok")};
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({tagged, tagged_ok}, 1, 1)}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && name_of(font, 1).empty() && name_of(font, 2) == "Ok");
    // Family and style by FreeType's rule.
    const std::vector<NameRecord> all_names = {{3, 1, 0x409, 1, ascii_utf16("Plain")},       {3, 1, 0x409, 2, ascii_utf16("Bold")},
                                               {3, 1, 0x409, 16, ascii_utf16("Typographic")}, {3, 1, 0x409, 17, ascii_utf16("Heavy")},
                                               {3, 1, 0x409, 21, ascii_utf16("Wws")},         {3, 1, 0x409, 22, ascii_utf16("WwsStyle")}};
    stxlib::Vector<char> text{};
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table(all_names)}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
    CHECK(glyphs::family_name(font, text).succeeded() && text_of(text) == "Wws");
    CHECK(glyphs::style_name(font, text).succeeded() && text_of(text) == "WwsStyle");
    bytes = truetype_font(os2_of(3, 96, 700, 5, 0x0120), hhea, plain_cmap(), false, {{"name", name_table(all_names)}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
    CHECK(glyphs::family_name(font, text).succeeded() && text_of(text) == "Typographic");
    CHECK(glyphs::style_name(font, text).succeeded() && text_of(text) == "Heavy");
    bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({all_names[0], all_names[1]})}});
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
    CHECK(glyphs::family_name(font, text).succeeded() && text_of(text) == "Plain");
    CHECK(glyphs::style_name(font, text).succeeded() && text_of(text) == "Bold");
    // No name table: every name is empty.
    bytes = truetype_font(os2, hhea, plain_cmap(), false);
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded() && font.names.length == 0);
    CHECK(glyphs::family_name(font, text).succeeded() && text.empty());

    // Collections: the count, and face N.
    const Bytes first = truetype_font(os2, hhea, plain_cmap(), false, {{"name", name_table({{3, 1, 0x409, 1, ascii_utf16("One")}})}});
    const Bytes second = truetype_font(os2_of(3, 96, 900, 5, 0x21), hhea, plain_cmap(), true,
                                       {{"name", name_table({{3, 1, 0x409, 1, ascii_utf16("Two")}})}});
    const Bytes both = collection_of({first, second});
    CHECK(glyphs::face_count(view_of(both)) == 2 && glyphs::face_count(view_of(first)) == 1);
    CHECK(glyphs::open_font(view_of(both), 0, font).succeeded() && name_of(font, 1) == "One" && font.weight_class == 700);
    CHECK(glyphs::open_font(view_of(both), 1, font).succeeded() && name_of(font, 1) == "Two" && font.weight_class == 900 && font.long_offsets);
    CHECK(glyphs::open_font(view_of(both), 2, font).failed() && glyphs::open_font(view_of(first), 1, font).failed());
    Bytes none_inside = both;
    none_inside[11] = 0;
    CHECK(glyphs::face_count(view_of(none_inside)) == 0 && glyphs::open_font(view_of(none_inside), 0, font).failed());
    Bytes too_many = both;
    too_many[10] = 0x7F;
    CHECK(glyphs::face_count(view_of(too_many)) == 0);
    const Bytes zeros(12, 0);
    CHECK(glyphs::face_count(view_of(zeros)) == 0 && glyphs::face_count(stxlib::View<u8>()) == 0);

    // The bounded search: a name table of four records (Windows, Macintosh,
    // Unicode; format 0 and 1) and the two-face collection, every prefix and
    // every byte set in turn to 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF. Each
    // font opens or is refused; each name asked is well-formed UTF-8 no
    // longer than three bytes a byte of the table; face_count is 0 or the
    // header's count, a face past it is refused, and a face opened has a
    // well-formed family name.
    const u8 values[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
    const std::vector<Bytes> name_tables = {name_table({windows_family, mac_family, unicode_family, german_family}),
                                            name_table({tagged, tagged_ok, windows_family}, 1, 2)};
    usize tried = 0;
    bool kept = true;
    for (const Bytes& names : name_tables) {
        std::vector<Bytes> variants{};
        for (usize length = 0; length <= names.size(); ++length) variants.push_back(Bytes(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(length)));
        for (usize place = 0; place < names.size(); ++place) {
            for (const u8 value : values) {
                Bytes changed = names;
                changed[place] = value;
                variants.push_back(changed);
            }
        }
        for (const Bytes& variant : variants) {
            bytes = truetype_font(os2, hhea, plain_cmap(), false, {{"name", variant}});
            tried = tried + 1;
            if (glyphs::open_font(view_of(bytes), 0, font).failed()) continue;
            const u32 ids[] = {1, 2, 4, 16, 0xFFFF};
            for (const u32 id : ids) {
                const stxlib::OperationStatus status = glyphs::font_name(font, id, text);
                kept = kept && status.succeeded() && well_formed_utf8(text) && text.count() <= variant.size() * 3;
            }
            kept = kept && glyphs::family_name(font, text).succeeded() && well_formed_utf8(text);
        }
    }
    for (usize length = 0; length <= both.size() && kept; length = length + (length < 64 ? 1 : 97)) {
        const Bytes prefix(both.begin(), both.begin() + static_cast<std::ptrdiff_t>(length));
        const u32 count = glyphs::face_count(view_of(prefix));
        kept = kept && glyphs::open_font(view_of(prefix), count, font).failed();
        for (u32 face = 0; face < count; ++face) {
            if (glyphs::open_font(view_of(prefix), face, font).succeeded()) kept = kept && name_well_formed(font);
        }
        tried = tried + 1;
    }
    for (usize place = 0; place < 64 && kept; ++place) {
        for (const u8 value : values) {
            Bytes changed = both;
            changed[place] = value;
            const u32 count = glyphs::face_count(view_of(changed));
            const u32 header = (static_cast<u32>(changed[8]) << 24) | (static_cast<u32>(changed[9]) << 16) | (static_cast<u32>(changed[10]) << 8) | changed[11];
            kept = kept && (count == 0 || count == header || count == 1);
            kept = kept && glyphs::open_font(view_of(changed), count, font).failed();
            for (u32 face = 0; face < count && face < 4; ++face) {
                if (glyphs::open_font(view_of(changed), face, font).succeeded()) kept = kept && name_well_formed(font);
            }
            tried = tried + 1;
        }
    }
    CHECK(kept);
    std::printf("font tables: %zu fonts and collections tried\n", tried);
}

// 01-F2: CFF operators FreeType and read-fonts draw. Each glyph is one
// charstring in a font of its own; `points` the outline's points expected.
[[nodiscard]] bool cff_draws(const Bytes& code, const std::vector<Expected>& points, const std::vector<u32>& ends) {
    const Bytes font_bytes = cff_font_of({code});
    glyphs::Font font{};
    if (!glyphs::open_font(view_of(font_bytes), 0, font).succeeded()) return false;
    glyphs::GlyphOutline outline{};
    if (!glyphs::load_glyph(font, 0, glyphs::FontLimits{}, outline).succeeded()) return false;
    const bool same = points.empty() ? outline.points.empty() : outline_is(outline, points, ends);
    return same;
}

[[nodiscard]] bool cff_refuses(const Bytes& code) {
    const Bytes font_bytes = cff_font_of({code});
    glyphs::Font font{};
    if (!glyphs::open_font(view_of(font_bytes), 0, font).succeeded()) return false;
    glyphs::GlyphOutline outline{};
    const stxlib::OperationStatus status = glyphs::load_glyph(font, 0, glyphs::FontLimits{}, outline);
    const bool refused = status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument;
    return refused;
}

void cff_operators() {
    constexpr i32 dotsection = 2000;
    constexpr i32 op_and = 2003;
    constexpr i32 op_or = 2004;
    constexpr i32 op_not = 2005;
    constexpr i32 op_abs = 2009;
    constexpr i32 op_add = 2010;
    constexpr i32 op_sub = 2011;
    constexpr i32 op_div = 2012;
    constexpr i32 op_neg = 2014;
    constexpr i32 op_eq = 2015;
    constexpr i32 op_drop = 2018;
    constexpr i32 op_put = 2020;
    constexpr i32 op_get = 2021;
    constexpr i32 op_ifelse = 2022;
    constexpr i32 op_mul = 2024;
    constexpr i32 op_sqrt = 2026;
    constexpr i32 op_dup = 2027;
    constexpr i32 op_exch = 2028;
    constexpr i32 op_index = 2029;
    constexpr i32 op_roll = 2030;
    constexpr i32 seac_ = 2006;
    const std::vector<Expected> line = {{0, 0, on}, {10, 20, on}};
    // dotsection and the reserved operators do nothing.
    CHECK(cff_draws(charstring({0, 0, rmoveto, dotsection, 10, 20, rlineto, endchar}), line, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 1013, 10, 20, rlineto, endchar}), line, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 2007, 10, 20, rlineto, endchar}), line, {1}));
    // An unknown operator clears the stack, as read-fonts does.
    CHECK(cff_draws(charstring({0, 0, rmoveto, 5, 1000, 10, 20, rlineto, endchar}), line, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 5, 2040, 10, 20, rlineto, endchar}), line, {1}));
    // Arithmetic: 4 + 6, 30 - 10; 7 * 3 - 1; 40 / 2; 400 sqrt; neg neg; abs.
    CHECK(cff_draws(charstring({0, 0, rmoveto, 4, 6, op_add, 30, 10, op_sub, rlineto, endchar}), {{0, 0, on}, {10, 20, on}}, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 5, 2, op_mul, 3, 7, op_mul, op_sub, 40, 2, op_div, rlineto, endchar}),
                    {{0, 0, on}, {-11, 20, on}}, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 100, op_sqrt, -20, op_abs, rlineto, endchar}), line, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, -10, op_neg, 20, op_neg, op_neg, rlineto, endchar}), line, {1}));
    // Stack operators: dup, exch, drop, index, roll.
    CHECK(cff_draws(charstring({0, 0, rmoveto, 10, op_dup, op_add, 10, op_exch, op_drop, 20, rlineto, endchar}), line, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 10, 20, 1, op_index, op_drop, rlineto, endchar}), line, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 20, 10, 2, 1, op_roll, rlineto, endchar}), line, {1}));
    // Logic: and, or, not, eq; ifelse picks the first when the third is not above the fourth.
    CHECK(cff_draws(charstring({0, 0, rmoveto, 1, 1, op_and, 10, op_mul, 0, 0, op_or, op_not, 20, op_mul, rlineto, endchar}), line, {1}));
    CHECK(cff_draws(charstring({0, 0, rmoveto, 3, 3, op_eq, 10, op_mul, 10, 30, 1, 2, op_ifelse, 2, op_mul, rlineto, endchar}), line, {1}));
    // Storage: put, then get.
    CHECK(cff_draws(charstring({0, 0, rmoveto, 20, 3, op_put, 10, 3, op_get, rlineto, endchar}), line, {1}));
    // An endchar with four operands is the accented form, which is not read.
    CHECK(cff_refuses(charstring({0, 0, 65, 66, endchar})));
    CHECK(cff_refuses(charstring({0, 0, 0, 65, 66, seac_, endchar})));
}

void segments() {
    glyphs::GlyphOutline outline{};
    stxlib::Vector<glyphs::Segment> kinds{};
    stxlib::Vector<glyphs::Point> points{};
    struct Fill final {
        static void outline_of(glyphs::GlyphOutline& target, const std::vector<Expected>& spots, const std::vector<u32>& ends) {
            target.points.clear();
            target.tags.clear();
            target.contour_ends.clear();
            for (const Expected& spot : spots) {
                static_cast<void>(target.points.push_copy(glyphs::Point{.x = spot.x, .y = spot.y}));
                static_cast<void>(target.tags.push_copy(spot.tag));
            }
            for (const u32 end : ends) static_cast<void>(target.contour_ends.push_copy(end));
        }
    };
    using glyphs::Segment;
    // On, control, on, on: a move, a curve, a line; then a contour of a
    // run of control points that begins on one: it begins between its last
    // and its first.
    Fill::outline_of(outline, {{0, 0, on}, {5, 9, quadratic}, {10, 0, on}, {20, 0, on}, {1, 1, quadratic}, {4, 6, quadratic}, {8, 3, quadratic}}, {3, 6});
    CHECK(glyphs::to_segments(outline, kinds, points).succeeded());
    CHECK(kinds.count() == 7 && kinds[0] == Segment::move && kinds[1] == Segment::quadratic && kinds[2] == Segment::line && kinds[3] == Segment::move);
    CHECK(kinds[4] == Segment::quadratic && kinds[5] == Segment::quadratic && kinds[6] == Segment::quadratic);
    // 1 + 2 + 1 points, then 1 + 2 + 2 + 2: the start is between (1, 1) and
    // (8, 3), each coordinate rounded down.
    CHECK(points.count() == 11 && points[4].x == 4 && points[4].y == 2);
    CHECK(points[5].x == 1 && points[5].y == 1 && points[6].x == 2 && points[6].y == 3);
    CHECK(points[7].x == 4 && points[7].y == 6 && points[8].x == 6 && points[8].y == 4);
    CHECK(points[9].x == 8 && points[9].y == 3 && points[10].x == 4 && points[10].y == 2);
    // A contour that begins on a control point and ends on the curve
    // begins at its end.
    Fill::outline_of(outline, {{3, 3, quadratic}, {9, 0, on}, {0, 0, on}}, {2});
    CHECK(glyphs::to_segments(outline, kinds, points).succeeded());
    CHECK(kinds.count() == 2 && kinds[0] == Segment::move && kinds[1] == Segment::quadratic);
    CHECK(points.count() == 3 && points[0].x == 0 && points[0].y == 0 && points[1].x == 3 && points[2].x == 9);
    // Rounded down below zero too.
    Fill::outline_of(outline, {{-3, -3, quadratic}, {-8, -4, quadratic}}, {1});
    CHECK(glyphs::to_segments(outline, kinds, points).succeeded() && points.count() == 5 && points[0].x == -6 && points[0].y == -4);
    // Cubic curves: to the next point, and to the start at the contour's end.
    Fill::outline_of(outline, {{0, 0, on}, {1, 5, cubic}, {4, 5, cubic}, {5, 0, on}, {4, -5, cubic}, {1, -5, cubic}}, {5});
    CHECK(glyphs::to_segments(outline, kinds, points).succeeded());
    CHECK(kinds.count() == 3 && kinds[1] == Segment::cubic && kinds[2] == Segment::cubic && points.count() == 7);
    CHECK(points[6].x == 0 && points[6].y == 0);
    // Through to the mask: the box of 101 units at 64 units to the pixel.
    Fill::outline_of(outline, {{0, 0, on}, {0, 128, on}, {128, 128, on}, {128, 0, on}}, {3});
    CHECK(glyphs::to_segments(outline, kinds, points).succeeded());
    const glyphs::Outline drawn{.segments = kinds.view(), .points = points.view()};
    glyphs::Bounds bounds{};
    CHECK(glyphs::measure(drawn, glyphs::Point{}, glyphs::Limits{}, bounds).succeeded() && bounds.width == 2 && bounds.height == 2);
    // Refused: one cubic control point alone, a contour that begins on a
    // cubic control point, ends out of order, tags that do not match.
    Fill::outline_of(outline, {{0, 0, on}, {1, 5, cubic}, {5, 0, on}}, {2});
    CHECK(glyphs::to_segments(outline, kinds, points).failed());
    Fill::outline_of(outline, {{1, 5, cubic}, {4, 5, cubic}, {5, 0, on}}, {2});
    CHECK(glyphs::to_segments(outline, kinds, points).failed());
    Fill::outline_of(outline, {{0, 0, on}, {1, 5, on}, {5, 0, on}}, {1, 0});
    CHECK(glyphs::to_segments(outline, kinds, points).failed());
    Fill::outline_of(outline, {{0, 0, on}, {1, 5, on}, {5, 0, on}}, {3});
    CHECK(glyphs::to_segments(outline, kinds, points).failed());
}

void refusals() {
    glyphs::Font font{};
    const Bytes nothing{};
    CHECK(glyphs::open_font(view_of(nothing), 0, font).failed());
    const Bytes text = {'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't', ' ', 'a', 't', ' ', 'a', 'l', 'l'};
    CHECK(glyphs::open_font(view_of(text), 0, font).failed());
    const Bytes os2 = os2_table(3, 0, 700, -200, 90, 950, 250);
    Bytes bytes = truetype_font(os2, hhea_table(800, -220, 50, 4), plain_cmap(), false);
    // A face other than the first of a font that is not a collection.
    CHECK(glyphs::open_font(view_of(bytes), 1, font).failed());
    // A collection of one face: the same font found through it.
    Bytes collection{};
    put32(collection, 0x74746366U);
    put32(collection, 0x00010000U);
    put32(collection, 1);
    put32(collection, 16);
    // The table offsets are from the file's beginning: move them by 16.
    Bytes moved = bytes;
    const usize table_count = (static_cast<usize>(moved[4]) << 8) | moved[5];
    for (usize index = 0; index < table_count; ++index) {
        const usize at = 12 + index * 16 + 8;
        const u32 offset = (static_cast<u32>(moved[at]) << 24) | (static_cast<u32>(moved[at + 1]) << 16) | (static_cast<u32>(moved[at + 2]) << 8) | moved[at + 3];
        const u32 shifted = offset + 16;
        moved[at] = static_cast<u8>(shifted >> 24);
        moved[at + 1] = static_cast<u8>((shifted >> 16) & 0xFFU);
        moved[at + 2] = static_cast<u8>((shifted >> 8) & 0xFFU);
        moved[at + 3] = static_cast<u8>(shifted & 0xFFU);
    }
    append(collection, moved);
    CHECK(glyphs::open_font(view_of(collection), 0, font).succeeded() && font.glyph_count == 8 && glyphs::glyph_of(font, 'B') == 2);
    glyphs::GlyphOutline outline{};
    CHECK(glyphs::load_glyph(font, 1, glyphs::FontLimits{}, outline).succeeded() && outline.points.count() == 4);
    CHECK(glyphs::open_font(view_of(collection), 1, font).failed());
    // An em of nothing.
    Bytes bad = bytes;
    const stxlib::View<u8> whole = view_of(bytes);
    glyphs::Font good{};
    CHECK(glyphs::open_font(whole, 0, good).succeeded());
    bad[good.head.offset + 18] = 0;
    bad[good.head.offset + 19] = 0;
    CHECK(glyphs::open_font(view_of(bad), 0, font).failed());
}

std::uint64_t random_state = 0x2545f4914f6cdd1dULL;
[[nodiscard]] std::uint64_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

// Everything this reader does with a font, whatever it is.
void use(const Bytes& bytes, std::uint64_t& opened, std::uint64_t& loaded) {
    glyphs::Font font{};
    if (glyphs::open_font(view_of(bytes), 0, font).failed()) return;
    opened = opened + 1;
    static_cast<void>(glyphs::glyph_of(font, 'A'));
    static_cast<void>(glyphs::glyph_of(font, 'y'));
    static_cast<void>(glyphs::glyph_of(font, 0x1F600));
    // The map gone through a stretch at a time, a bounded way in.
    u32 character = 0;
    for (u32 step = 0; step < 64; ++step) {
        glyphs::MappedStretch stretch{};
        if (!glyphs::next_stretch(font, character, stretch)) break;
        static_cast<void>(glyphs::glyph_in_stretch(font, stretch, stretch.first));
        static_cast<void>(glyphs::glyph_in_stretch(font, stretch, stretch.last));
        if (stretch.last >= 0x10FFFF) break;
        character = stretch.last + 1;
    }
    glyphs::FontLimits limits{};
    limits.charstring_steps = 2000;
    limits.components = 40;
    glyphs::GlyphOutline outline{};
    stxlib::Vector<glyphs::Segment> kinds{};
    stxlib::Vector<glyphs::Point> points{};
    const u32 count = font.glyph_count < 40 ? font.glyph_count : 40;
    for (u32 glyph = 0; glyph < count; ++glyph) {
        static_cast<void>(glyphs::advance_of(font, glyph));
        if (glyphs::load_glyph(font, glyph, limits, outline).succeeded()) {
            loaded = loaded + 1;
            static_cast<void>(glyphs::to_segments(outline, kinds, points));
        }
        static_cast<void>(glyphs::load_scaled_glyph(font, glyph, 70000, 70000, limits, outline));
    }
}

// The font as an application's own, through a Face. False at the first
// promise broken; whether a call refuses or succeeds is the font's affair.
[[nodiscard]] bool face_use(const Bytes& bytes, std::uint64_t& faces, std::uint64_t& drawn) {
    glyphs::FaceLimits limits{};
    glyphs::TypefaceLimits typeface_limits{};
    typeface_limits.font.charstring_steps = 2000;
    typeface_limits.font.components = 40;
    glyphs::Face face{};
    if (face.open(view_of(bytes), 0, limits, typeface_limits).failed()) return true;
    faces = faces + 1;
    const glyphs::Font& font = *face.font();
    static_cast<void>(glyphs::has_feature(font, 0x6C696761U));
    const u32 letters[] = {'A', 'B', 'C', 'x', 'y', 0x1F600, 0x10FFFF};
    usize warmed = 0;
    static_cast<void>(face.warm(stxlib::View<u32>(letters), 13 * 64, glyphs::Fit::light, warmed));
    if (warmed > 7) return false;
    // A run of the font's first glyphs and of two it has not.
    glyphs::RunGlyph run[16] = {};
    for (u32 index = 0; index < 16; ++index) {
        const u32 glyph = index < 14 ? index : font.glyph_count + index;
        run[index] = glyphs::RunGlyph{glyph, static_cast<i32>(index * 700 + index * 5), static_cast<i32>(index * 3)};
    }
    glyphs::RunCoverage coverage{};
    glyphs::RunBox box{};
    glyphs::FittedGlyph fitted{};
    const glyphs::Fit fits[] = {glyphs::Fit::light, glyphs::Fit::none, glyphs::Fit::monochrome};
    for (const glyphs::Fit fit : fits) {
        glyphs::DrawStyle style{};
        style.size = 11 * 64 + 5;
        style.fit = fit;
        style.thicken = fit == glyphs::Fit::none ? 48 : 0;
        const bool measured = face.measure_run(stxlib::View<glyphs::RunGlyph>(run), style, box).succeeded();
        if (measured && box.missing_glyphs < 2) return false;
        if (face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, coverage).succeeded()) {
            drawn = drawn + 1;
            if (coverage.pixels.count() != static_cast<usize>(coverage.width) * coverage.height) return false;
            if (measured && !coverage.oversized &&
                (box.right - box.left != static_cast<i32>(coverage.width) || box.bottom - box.top != static_cast<i32>(coverage.height))) {
                return false;
            }
        }
        for (u32 glyph = 0; glyph < font.glyph_count && glyph < 14; ++glyph) {
            if (face.fitted(glyph, 20 * 64, fit, fitted).succeeded() && (fitted.right < fitted.left || fitted.top < fitted.bottom)) return false;
        }
    }
    // Turned: fitted at 17 degrees with the places through the matrix, and
    // by a quarter turn.
    const stxlib::i64 angles[] = {static_cast<stxlib::i64>(17) << 16, static_cast<stxlib::i64>(90) << 16};
    for (const stxlib::i64 angle : angles) {
        glyphs::DrawStyle style{};
        style.size = 11 * 64 + 5;
        style.fit = glyphs::Fit::light;
        style.turn_places = true;
        glyphs::set_matrix(style, glyphs::turn_matrix(angle));
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
    for (u32 glyph = 0; glyph < font.glyph_count && glyph < 14; ++glyph) {
        if ((*face.typeface()).ink_box(glyph, 900, ink).succeeded() && (ink.right < ink.left || ink.top < ink.bottom)) return false;
    }
    const bool whole = face.whole();
    return whole;
}

void robustness() {
    const Bytes os2 = os2_table(3, 0, 700, -200, 90, 950, 250);
    const std::vector<Bytes> fonts = {
        truetype_font(os2, hhea_table(800, -220, 50, 4),
                      cmap_table({{3, 1, map_format_4({{'A', 'C', 1 - 'A', {}}, {'x', 'y', 0, {5, 2}}, {0xFFFF, 0xFFFF, 1, {}}})},
                                  {3, 10, map_format_12({{'A', 'B', 3}, {0x1F600, 0x1F602, 5}})}}),
                      true),
        truetype_font(os2, hhea_table(800, -220, 50, 4), plain_cmap(), false),
        cff_font(false),
        cff_font(true),
    };
    std::uint64_t tried = 0;
    std::uint64_t opened = 0;
    std::uint64_t loaded = 0;
    std::uint64_t through_a_face = 0;
    std::uint64_t faces = 0;
    std::uint64_t drawn = 0;
    bool kept = true;
    for (const Bytes& font : fonts) {
        // The font as it should be draws.
        const std::uint64_t drawn_before = drawn;
        kept = kept && face_use(font, faces, drawn) && drawn == drawn_before + 5;
        // Every prefix.
        for (usize length = 0; length <= font.size(); ++length) {
            const Bytes prefix(font.begin(), font.begin() + static_cast<std::ptrdiff_t>(length));
            use(prefix, opened, loaded);
            kept = kept && face_use(prefix, faces, drawn);
            through_a_face = through_a_face + 1;
            tried = tried + 1;
        }
        // One to four bytes changed, many times; and every byte in turn
        // set to each of a few values.
        for (int round = 0; round < 60000; ++round) {
            Bytes changed = font;
            const usize changes = 1 + next_random() % 4;
            for (usize change = 0; change < changes; ++change) changed[next_random() % changed.size()] = static_cast<u8>(next_random() & 0xFFU);
            use(changed, opened, loaded);
            if (round % 4 == 0) {
                kept = kept && face_use(changed, faces, drawn);
                through_a_face = through_a_face + 1;
            }
            tried = tried + 1;
        }
        const u8 values[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
        for (usize at = 0; at < font.size(); ++at) {
            for (const u8 value : values) {
                Bytes changed = font;
                changed[at] = value;
                use(changed, opened, loaded);
                kept = kept && face_use(changed, faces, drawn);
                through_a_face = through_a_face + 1;
                tried = tried + 1;
            }
        }
    }
    std::printf("robustness: %llu fonts tried, %llu opened, %llu glyphs loaded\n", static_cast<unsigned long long>(tried),
                static_cast<unsigned long long>(opened), static_cast<unsigned long long>(loaded));
    CHECK(opened > 1000 && loaded > 1000);
    std::printf("robustness through a face: %llu fonts, %llu faces opened, %llu runs drawn\n", static_cast<unsigned long long>(through_a_face),
                static_cast<unsigned long long>(faces), static_cast<unsigned long long>(drawn));
    CHECK(kept);
    CHECK(through_a_face > 60000 && faces > 1000 && drawn > 1000);
}

} // namespace

int main() {
    truetype();
    heights();
    character_maps();
    cff();
    audit_contracts();
    font_tables();
    cff_operators();
    segments();
    refusals();
    robustness();
    // The checked program (CMakeLists.txt, stxlib_glyphs_checked) is this
    // test on glyph code that checks every structure whole at every exit.
    std::printf("glyph checks: %s\n", glyphs::glyph_checks_enabled() ? "on" : "off");
#if defined(STXLIB_GLYPHS_CHECKS_EXPECTED)
    CHECK(glyphs::glyph_checks_enabled());
#endif
    const int status = stxlib_tests::finish("stxlib_font_file_tests");
    return status;
}
