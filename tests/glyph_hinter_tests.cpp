// stxlib::glyphs, the hinter (include/stxlib/glyph_hinter.hpp), against a
// font this test builds in memory: Latin letters drawn from straight
// stems, so that every height the hinter should find is the test's own.
//
// The letters, in units of an em of 1000: capitals 700 high, small letters
// 500, ascenders 750, descenders 200 below the baseline; upright stems 80
// wide and bars 70 thick. O and 0 are round and go 10 beyond; every other
// top and bottom is flat.
//
//   H1  what is refused: a glyph the font has not, a size of nothing, a
//       font that was not the one prepared, a limit;
//   H2  light mode at 16 pixels, by hand: x as scaled, and the baseline,
//       the x-height, the height of capitals, ascenders and descenders
//       each on the whole pixel nearest;
//   H3  light mode at every size from 8 to 40 pixels: x as scaled, flat
//       tops and bottoms on whole pixels and in order, the advance a whole
//       number of pixels;
//   H4  monochrome mode at the same sizes: stems and bars whole pixels
//       wide and at whole pixels, both stems of a letter the same, the
//       digits all one advance; and the size at which the face is very
//       light, where a bar keeps its thickness;
//   H5  the same call twice gives the same outline; a hinter prepared
//       again and one released and prepared give it too;
//   H6  a font with none of the letters that blue zones are measured from
//       is left as scaled; a glyph of no points succeeds;
//   H7  robustness: every prefix of the font and many copies with bytes
//       changed are prepared and hinted; each either succeeds or is
//       refused (the sanitizer builds are what make this a memory check).
//
// The hinter is held to FreeType 2.14.2 on the bundled fonts by the seat's
// comparison (PROVENANCE.md); nothing here needs FreeType.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_hinter.hpp"

#include "font_builder.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace glyphs = stxlib::glyphs;
using stxlib::i32;
using stxlib::i64;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

using font_builder::Bytes;
using font_builder::cmap_table;
using font_builder::font_of;
using font_builder::glyph_tables;
using font_builder::head_table;
using font_builder::hhea_table;
using font_builder::map_format_4;
using font_builder::maxp_table;
using font_builder::Metric;
using font_builder::os2_table;
using font_builder::simple_glyph;
using font_builder::Spot;
using font_builder::view_of;

// The glyphs of the font, by number.
constexpr u32 glyph_zero = 1;
constexpr u32 glyph_one = 2;
constexpr u32 glyph_H = 3;
constexpr u32 glyph_O = 4;
constexpr u32 glyph_h = 5;
constexpr u32 glyph_i = 6;
constexpr u32 glyph_m = 7;
constexpr u32 glyph_n = 8;
constexpr u32 glyph_o = 9;
constexpr u32 glyph_p = 10;
constexpr u32 glyph_count = 11;

// A round ring: on the curve at the middle of each side, off it at each
// corner; the outer contour clockwise and the inner one against it.
[[nodiscard]] Bytes round_ring(const i32 left, const i32 right, const i32 bottom, const i32 top, const i32 stem, const i32 bar) {
    const i32 x_middle = (left + right) / 2;
    const i32 y_middle = (bottom + top) / 2;
    const Bytes result = simple_glyph({{{left, y_middle, true},
                                        {left, top, false},
                                        {x_middle, top, true},
                                        {right, top, false},
                                        {right, y_middle, true},
                                        {right, bottom, false},
                                        {x_middle, bottom, true},
                                        {left, bottom, false}},
                                       {{left + stem, y_middle, true},
                                        {left + stem, bottom + bar, false},
                                        {x_middle, bottom + bar, true},
                                        {right - stem, bottom + bar, false},
                                        {right - stem, y_middle, true},
                                        {right - stem, top - bar, false},
                                        {x_middle, top - bar, true},
                                        {left + stem, top - bar, false}}});
    return result;
}

[[nodiscard]] std::vector<Bytes> letters() {
    std::vector<Bytes> glyph_bytes{};
    // 0: nothing.
    glyph_bytes.push_back(Bytes{});
    // 0 and 1.
    glyph_bytes.push_back(round_ring(60, 440, -10, 710, 80, 70));
    glyph_bytes.push_back(simple_glyph({{{210, 0, true}, {210, 700, true}, {290, 700, true}, {290, 0, true}}}));
    // H: two stems and a bar.
    glyph_bytes.push_back(simple_glyph({{{50, 0, true},
                                         {50, 700, true},
                                         {130, 700, true},
                                         {130, 390, true},
                                         {370, 390, true},
                                         {370, 700, true},
                                         {450, 700, true},
                                         {450, 0, true},
                                         {370, 0, true},
                                         {370, 320, true},
                                         {130, 320, true},
                                         {130, 0, true}}}));
    // O.
    glyph_bytes.push_back(round_ring(50, 450, -10, 710, 80, 70));
    // h: a tall stem, an arch of straight lines and a short stem.
    glyph_bytes.push_back(simple_glyph({{{50, 0, true},
                                         {50, 750, true},
                                         {130, 750, true},
                                         {130, 500, true},
                                         {400, 500, true},
                                         {400, 0, true},
                                         {320, 0, true},
                                         {320, 430, true},
                                         {130, 430, true},
                                         {130, 0, true}}}));
    // i: a stem and its dot.
    glyph_bytes.push_back(simple_glyph({{{50, 0, true}, {50, 500, true}, {130, 500, true}, {130, 0, true}},
                                        {{50, 600, true}, {50, 680, true}, {130, 680, true}, {130, 600, true}}}));
    // m: three stems.
    glyph_bytes.push_back(simple_glyph({{{50, 0, true},
                                         {50, 500, true},
                                         {670, 500, true},
                                         {670, 0, true},
                                         {590, 0, true},
                                         {590, 430, true},
                                         {400, 430, true},
                                         {400, 0, true},
                                         {320, 0, true},
                                         {320, 430, true},
                                         {130, 430, true},
                                         {130, 0, true}}}));
    // n: two.
    glyph_bytes.push_back(simple_glyph({{{50, 0, true},
                                         {50, 500, true},
                                         {400, 500, true},
                                         {400, 0, true},
                                         {320, 0, true},
                                         {320, 430, true},
                                         {130, 430, true},
                                         {130, 0, true}}}));
    // o: a ring of straight lines, so that the x-height is flat.
    glyph_bytes.push_back(simple_glyph({{{50, 0, true}, {50, 500, true}, {400, 500, true}, {400, 0, true}},
                                        {{130, 70, true}, {320, 70, true}, {320, 430, true}, {130, 430, true}}}));
    // p: o with its left stem carried below the baseline.
    glyph_bytes.push_back(simple_glyph({{{50, -200, true}, {50, 500, true}, {400, 500, true}, {400, 0, true}, {130, 0, true}, {130, -200, true}},
                                        {{130, 70, true}, {320, 70, true}, {320, 430, true}, {130, 430, true}}}));
    return glyph_bytes;
}

[[nodiscard]] Bytes font_with(const std::vector<Bytes>& glyph_bytes, const std::vector<Metric>& metrics, const Bytes& map) {
    Bytes glyf{};
    Bytes loca{};
    Bytes hmtx{};
    glyph_tables(glyph_bytes, metrics, metrics.size(), true, glyf, loca, hmtx);
    const Bytes font = font_of(0x00010000U, {{"OS/2", os2_table(3, 0, 750, -200, 50, 950, 250)},
                                             {"cmap", cmap_table({{3, 1, map}})},
                                             {"glyf", glyf},
                                             {"head", head_table(1000, true)},
                                             {"hhea", hhea_table(800, -220, 50, static_cast<u32>(metrics.size()))},
                                             {"hmtx", hmtx},
                                             {"loca", loca},
                                             {"maxp", maxp_table(static_cast<u32>(glyph_bytes.size()))}});
    return font;
}

// The font of the letters: 0 1 H O h i m n o p.
[[nodiscard]] Bytes letter_font() {
    const std::vector<Metric> metrics = {{500, 0}, {500, 60}, {500, 210}, {500, 50}, {500, 50}, {450, 50},
                                         {180, 50}, {720, 50}, {450, 50}, {450, 50}, {450, 50}};
    const Bytes map = map_format_4({{'0', '1', static_cast<i32>(glyph_zero) - '0', {}},
                                    {'H', 'H', static_cast<i32>(glyph_H) - 'H', {}},
                                    {'O', 'O', static_cast<i32>(glyph_O) - 'O', {}},
                                    {'h', 'i', static_cast<i32>(glyph_h) - 'h', {}},
                                    {'m', 'p', static_cast<i32>(glyph_m) - 'm', {}},
                                    {0xFFFF, 0xFFFF, 1, {}}});
    const Bytes font = font_with(letters(), metrics, map);
    return font;
}

// a * b / 65536 rounded, halves away from zero: the test's own.
[[nodiscard]] i32 times(const i64 a, const i64 b) {
    const i64 product = a * b;
    const i64 magnitude = product < 0 ? -product : product;
    const i64 rounded = (magnitude + 0x8000) / 0x10000;
    const i32 result = static_cast<i32>(product < 0 ? -rounded : rounded);
    return result;
}

[[nodiscard]] bool whole(const i32 value) {
    const bool result = (value & 63) == 0;
    return result;
}

[[nodiscard]] bool same_outline(const glyphs::GlyphOutline& a, const glyphs::GlyphOutline& b) {
    if (a.points.count() != b.points.count() || a.tags.count() != b.tags.count() || a.contour_ends.count() != b.contour_ends.count()) return false;
    for (usize index = 0; index < a.points.count(); ++index) {
        if (a.points[index].x != b.points[index].x || a.points[index].y != b.points[index].y || a.tags[index] != b.tags[index]) return false;
    }
    for (usize index = 0; index < a.contour_ends.count(); ++index) {
        if (a.contour_ends[index] != b.contour_ends[index]) return false;
    }
    return true;
}

// A font opened and a hinter prepared for it.
struct Opened final {
    Bytes bytes{};
    glyphs::Font font{};
    glyphs::Hinter hinter{};
    glyphs::FontLimits limits{};
    bool ready{false};
};

void open_letters(Opened& opened) {
    opened.bytes = letter_font();
    const bool font_opened = glyphs::open_font(view_of(opened.bytes), 0, opened.font).succeeded();
    CHECK(font_opened);
    CHECK(opened.font.glyph_count == glyph_count && opened.font.units_per_em == 1000);
    CHECK(glyphs::glyph_of(opened.font, 'H') == glyph_H && glyphs::glyph_of(opened.font, 'p') == glyph_p && glyphs::glyph_of(opened.font, '0') == glyph_zero);
    opened.ready = font_opened && opened.hinter.prepare(opened.font).succeeded();
    CHECK(opened.ready);
}

void refusals() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::GlyphOutline outline{};
    i32 advance = 7;
    const stxlib::Status beyond = opened.hinter.hint_glyph(opened.font, glyph_count, 1024, glyphs::HintMode::light, opened.limits, outline, advance);
    CHECK(beyond.failed() && (*beyond.error()).kind() == stxlib::ErrorKind::invalid_argument);
    CHECK(advance == 0 && outline.points.empty());
    const stxlib::Status nothing = opened.hinter.hint_glyph(opened.font, glyph_H, 0, glyphs::HintMode::light, opened.limits, outline, advance);
    CHECK(nothing.failed() && (*nothing.error()).kind() == stxlib::ErrorKind::invalid_argument);
    const stxlib::Status negative = opened.hinter.hint_glyph(opened.font, glyph_H, -64, glyphs::HintMode::light, opened.limits, outline, advance);
    CHECK(negative.failed() && (*negative.error()).kind() == stxlib::ErrorKind::invalid_argument);
    const stxlib::Status huge = opened.hinter.hint_glyph(opened.font, glyph_H, (static_cast<i64>(1) << 24) + 1, glyphs::HintMode::light, opened.limits, outline, advance);
    CHECK(huge.failed() && (*huge.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // A hinter that was not prepared, and one prepared for another font.
    glyphs::Hinter fresh{};
    const stxlib::Status unprepared = fresh.hint_glyph(opened.font, glyph_H, 1024, glyphs::HintMode::light, opened.limits, outline, advance);
    CHECK(unprepared.failed() && (*unprepared.error()).kind() == stxlib::ErrorKind::invalid_argument);
    glyphs::Font other = opened.font;
    other.glyph_count = glyph_count - 1;
    const stxlib::Status another = opened.hinter.hint_glyph(other, glyph_H, 1024, glyphs::HintMode::light, opened.limits, outline, advance);
    CHECK(another.failed() && (*another.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // A limit: H has twelve points.
    glyphs::FontLimits few{};
    few.points = 11;
    const stxlib::Status limited = opened.hinter.hint_glyph(opened.font, glyph_H, 1024, glyphs::HintMode::light, few, outline, advance);
    CHECK(limited.failed() && (*limited.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    few.points = 12;
    CHECK(opened.hinter.hint_glyph(opened.font, glyph_H, 1024, glyphs::HintMode::light, few, outline, advance).succeeded());
    // After release nothing is prepared.
    opened.hinter.release();
    const stxlib::Status released = opened.hinter.hint_glyph(opened.font, glyph_H, 1024, glyphs::HintMode::light, opened.limits, outline, advance);
    CHECK(released.failed() && (*released.error()).kind() == stxlib::ErrorKind::invalid_argument);
}

// The hinted y of the points of `glyph` whose y in font units is
// `height`: all of them one number, which is given; `found` says whether
// there were any and they agreed.
[[nodiscard]] i32 height_of(const glyphs::GlyphOutline& plain, const glyphs::GlyphOutline& hinted, const i32 height, bool& found) {
    i32 result = 0;
    bool any = false;
    bool agree = true;
    for (usize index = 0; index < plain.points.count(); ++index) {
        if (plain.points[index].y != height) continue;
        if (any && hinted.points[index].y != result) agree = false;
        result = hinted.points[index].y;
        any = true;
    }
    found = any && agree;
    return result;
}

struct Hinted final {
    glyphs::GlyphOutline plain{};
    glyphs::GlyphOutline outline{};
    i32 advance{0};
    bool done{false};
};

// `glyph` in font units and hinted.
void hint(Opened& opened, const u32 glyph, const i64 size, const glyphs::HintMode mode, Hinted& hinted) {
    const bool loaded = glyphs::load_glyph(opened.font, glyph, opened.limits, hinted.plain).succeeded();
    const bool fitted = opened.hinter.hint_glyph(opened.font, glyph, size, mode, opened.limits, hinted.outline, hinted.advance).succeeded();
    hinted.done = loaded && fitted && hinted.plain.points.count() == hinted.outline.points.count();
    CHECK(hinted.done);
}

void light_by_hand() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    // 16 pixels to an em of 1000: 1024 * 65536 / 1000, rounded.
    const i64 size = 1024;
    const i64 scale = glyphs::scale_of(opened.font, size);
    CHECK(scale == 67109);
    struct Level final {
        u32 glyph{0};
        i32 height{0};
        i32 expected{0};
    };
    // 700 is 716.8 and goes to 11 pixels; 500 is 512, 8 pixels, so that
    // the scale along y stays as it is; 750 is 768, 12 pixels; -200 is
    // -204.8 and goes to -3 pixels.
    const Level levels[] = {
        {glyph_H, 700, 704}, {glyph_H, 0, 0},   {glyph_n, 500, 512}, {glyph_n, 0, 0},   {glyph_m, 500, 512}, {glyph_m, 0, 0},
        {glyph_h, 750, 768}, {glyph_h, 0, 0},   {glyph_o, 500, 512}, {glyph_o, 0, 0},   {glyph_p, 500, 512}, {glyph_p, -200, -192},
        {glyph_i, 500, 512}, {glyph_i, 0, 0},   {glyph_one, 0, 0},   {glyph_one, 700, 704},
    };
    for (const Level& level : levels) {
        Hinted hinted{};
        hint(opened, level.glyph, size, glyphs::HintMode::light, hinted);
        if (!hinted.done) continue;
        bool found = false;
        const i32 y = height_of(hinted.plain, hinted.outline, level.height, found);
        if (!found || y != level.expected) std::fprintf(stderr, "  glyph %u height %d: %d (found %d), expected %d\n", level.glyph, level.height, y, found ? 1 : 0, level.expected);
        CHECK(found && y == level.expected);
        // x is the outline's own, scaled.
        bool x_as_scaled = true;
        for (usize index = 0; index < hinted.plain.points.count(); ++index) {
            if (hinted.outline.points[index].x != times(hinted.plain.points[index].x, scale)) x_as_scaled = false;
        }
        CHECK(x_as_scaled);
    }
    // Round tops and bottoms this close to the flat ones go where the flat
    // ones go: O's top with H's, its bottom on the baseline.
    Hinted round{};
    hint(opened, glyph_O, size, glyphs::HintMode::light, round);
    if (round.done) {
        bool top_found = false;
        bool bottom_found = false;
        const i32 top = height_of(round.plain, round.outline, 710, top_found);
        const i32 bottom = height_of(round.plain, round.outline, -10, bottom_found);
        if (top != 704 || bottom != 0) std::fprintf(stderr, "  O: top %d bottom %d\n", top, bottom);
        CHECK(top_found && top == 704);
        CHECK(bottom_found && bottom == 0);
    }
    // Advances: 500 is 512 and 450 is 460.8, 7 pixels; a glyph of no
    // points has its advance and nothing else.
    Hinted wide{};
    hint(opened, glyph_H, size, glyphs::HintMode::light, wide);
    CHECK(wide.advance == 512);
    Hinted narrow{};
    hint(opened, glyph_n, size, glyphs::HintMode::light, narrow);
    CHECK(narrow.advance == 448);
    Hinted empty{};
    hint(opened, 0, size, glyphs::HintMode::light, empty);
    CHECK(empty.done && empty.outline.points.empty() && empty.outline.contour_ends.empty() && empty.advance == 512);
}

struct Flat final {
    u32 glyph{0};
    // Flat heights in font units, lowest first.
    std::vector<i32> heights{};
};

[[nodiscard]] std::vector<Flat> flats() {
    const std::vector<Flat> result = {
        {glyph_one, {0, 700}}, {glyph_H, {0, 700}}, {glyph_h, {0, 500, 750}}, {glyph_i, {0, 500}},
        {glyph_m, {0, 500}},   {glyph_n, {0, 500}}, {glyph_o, {0, 500}},      {glyph_p, {-200, 500}},
    };
    return result;
}

// Flat tops and bottoms on whole pixels and in order.
void check_heights(Opened& opened, const i64 size, const glyphs::HintMode mode) {
    for (const Flat& flat : flats()) {
        Hinted hinted{};
        hint(opened, flat.glyph, size, mode, hinted);
        if (!hinted.done) continue;
        i32 below = 0;
        for (usize index = 0; index < flat.heights.size(); ++index) {
            bool found = false;
            const i32 y = height_of(hinted.plain, hinted.outline, flat.heights[index], found);
            const bool good = found && whole(y) && (index == 0 || y > below);
            if (!good) {
                std::fprintf(stderr, "  size %lld mode %d glyph %u height %d: %d (found %d, below %d)\n", static_cast<long long>(size), static_cast<int>(mode),
                             flat.glyph, flat.heights[index], y, found ? 1 : 0, below);
            }
            CHECK(good);
            below = y;
        }
        CHECK(whole(hinted.advance) && hinted.advance > 0);
    }
}

void light_at_sizes() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    for (i64 pixels = 8; pixels <= 40; ++pixels) {
        const i64 size = pixels * 64;
        check_heights(opened, size, glyphs::HintMode::light);
        // x is never moved, and the advance is the scaled one to a pixel.
        const i64 scale = glyphs::scale_of(opened.font, size);
        for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
            Hinted hinted{};
            hint(opened, glyph, size, glyphs::HintMode::light, hinted);
            if (!hinted.done) continue;
            bool x_as_scaled = true;
            for (usize index = 0; index < hinted.plain.points.count(); ++index) {
                if (hinted.outline.points[index].x != times(hinted.plain.points[index].x, scale)) x_as_scaled = false;
            }
            CHECK(x_as_scaled);
            const i32 scaled_advance = times(glyphs::advance_of(opened.font, glyph), scale);
            CHECK(hinted.advance == ((scaled_advance + 32) & ~63));
        }
    }
    // A size that is not a whole number of pixels.
    check_heights(opened, 13 * 64 + 21, glyphs::HintMode::light);
}

// The hinted x of the points whose x in font units is `position`.
[[nodiscard]] i32 position_of(const glyphs::GlyphOutline& plain, const glyphs::GlyphOutline& hinted, const i32 position, bool& found) {
    i32 result = 0;
    bool any = false;
    bool agree = true;
    for (usize index = 0; index < plain.points.count(); ++index) {
        if (plain.points[index].x != position) continue;
        if (any && hinted.points[index].x != result) agree = false;
        result = hinted.points[index].x;
        any = true;
    }
    found = any && agree;
    return result;
}

void monochrome_at_sizes() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    struct Stems final {
        u32 glyph{0};
        // The sides of the upright stems in font units, left to right.
        std::vector<i32> sides{};
    };
    const std::vector<Stems> letters_with_stems = {
        {glyph_one, {210, 290}},
        {glyph_H, {50, 130, 370, 450}},
        {glyph_h, {50, 130, 320, 400}},
        {glyph_n, {50, 130, 320, 400}},
        {glyph_m, {50, 130, 320, 400, 590, 670}},
        {glyph_o, {50, 130, 320, 400}},
    };
    for (i64 pixels = 8; pixels <= 40; ++pixels) {
        const i64 size = pixels * 64;
        check_heights(opened, size, glyphs::HintMode::monochrome);
        for (const Stems& letter : letters_with_stems) {
            Hinted hinted{};
            hint(opened, letter.glyph, size, glyphs::HintMode::monochrome, hinted);
            if (!hinted.done) continue;
            bool good = true;
            i32 first_width = 0;
            i32 before = 0;
            for (usize index = 0; index < letter.sides.size(); ++index) {
                bool found = false;
                const i32 x = position_of(hinted.plain, hinted.outline, letter.sides[index], found);
                if (!found || !whole(x)) good = false;
                if (index > 0 && x <= before) good = false;
                if (index % 2 == 1) {
                    const i32 width = x - before;
                    if (index == 1) first_width = width;
                    if (width < 64 || width != first_width) good = false;
                }
                before = x;
            }
            if (!good) {
                std::fprintf(stderr, "  size %lld glyph %u: sides", static_cast<long long>(size), letter.glyph);
                for (const i32 side : letter.sides) {
                    bool found = false;
                    const i32 x = position_of(hinted.plain, hinted.outline, side, found);
                    std::fprintf(stderr, " %d->%d%s", side, x, found ? "" : "?");
                }
                std::fprintf(stderr, "\n");
            }
            CHECK(good);
        }
        // H's bar: a whole number of pixels thick, between the baseline
        // and the top. At 8 pixels the bars of this font are under 40/64
        // of a pixel (70 units are 35.8), which makes it a very light
        // face at that size: a bar keeps its thickness, 36, and is put
        // with its middle on the middle of a pixel, 160.
        Hinted bar{};
        hint(opened, glyph_H, size, glyphs::HintMode::monochrome, bar);
        if (bar.done) {
            bool low_found = false;
            bool high_found = false;
            bool top_found = false;
            const i32 low = height_of(bar.plain, bar.outline, 320, low_found);
            const i32 high = height_of(bar.plain, bar.outline, 390, high_found);
            const i32 top = height_of(bar.plain, bar.outline, 700, top_found);
            const bool very_light = pixels == 8;
            const bool fitted = very_light ? (low == 142 && high == 178) : (whole(low) && whole(high) && high - low >= 64);
            const bool good = low_found && high_found && top_found && fitted && low > 0 && high < top;
            if (!good) std::fprintf(stderr, "  size %lld: H's bar %d..%d under %d\n", static_cast<long long>(size), low, high, top);
            CHECK(good);
        }
        // The digits all have one advance.
        Hinted zero{};
        Hinted one{};
        hint(opened, glyph_zero, size, glyphs::HintMode::monochrome, zero);
        hint(opened, glyph_one, size, glyphs::HintMode::monochrome, one);
        CHECK(zero.advance == one.advance && whole(zero.advance) && zero.advance > 0);
    }
}

void repeated() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    const glyphs::HintMode modes[] = {glyphs::HintMode::light, glyphs::HintMode::monochrome};
    const i64 sizes[] = {9 * 64, 12 * 64 + 32, 17 * 64, 33 * 64};
    for (const glyphs::HintMode mode : modes) {
        for (const i64 size : sizes) {
            for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
                Hinted first{};
                Hinted second{};
                hint(opened, glyph, size, mode, first);
                hint(opened, glyph, size, mode, second);
                CHECK(same_outline(first.outline, second.outline) && first.advance == second.advance);
                // A hinter of its own, which has met no other glyph.
                glyphs::Hinter own{};
                glyphs::GlyphOutline outline{};
                i32 advance = 0;
                const bool done = own.prepare(opened.font).succeeded() && own.hint_glyph(opened.font, glyph, size, mode, opened.limits, outline, advance).succeeded();
                CHECK(done && same_outline(first.outline, outline) && first.advance == advance);
                // Released and prepared again.
                own.release();
                const bool again = own.prepare(opened.font).succeeded() && own.hint_glyph(opened.font, glyph, size, mode, opened.limits, outline, advance).succeeded();
                CHECK(again && same_outline(first.outline, outline) && first.advance == advance);
            }
        }
    }
}

// A font whose only characters are A and B, each a square: no blue zone
// can be measured, and its glyphs are left as scaled.
void without_blue_zones() {
    std::vector<Bytes> glyph_bytes{};
    glyph_bytes.push_back(Bytes{});
    glyph_bytes.push_back(simple_glyph({{{40, 0, true}, {40, 613, true}, {453, 613, true}, {453, 0, true}}}));
    glyph_bytes.push_back(simple_glyph({{{40, 0, true}, {40, 313, false}, {253, 313, true}, {253, 0, true}}}));
    const std::vector<Metric> metrics = {{500, 0}, {517, 40}, {333, 40}};
    const Bytes map = map_format_4({{'A', 'B', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}});
    const Bytes bytes = font_with(glyph_bytes, metrics, map);
    glyphs::Font font{};
    glyphs::Hinter hinter{};
    const bool ready = glyphs::open_font(view_of(bytes), 0, font).succeeded() && hinter.prepare(font).succeeded();
    CHECK(ready);
    if (!ready) return;
    const glyphs::FontLimits limits{};
    const glyphs::HintMode modes[] = {glyphs::HintMode::light, glyphs::HintMode::monochrome};
    for (const glyphs::HintMode mode : modes) {
        for (i64 size = 7 * 64; size <= 30 * 64; size = size + 37) {
            const i64 scale = glyphs::scale_of(font, size);
            for (u32 glyph = 0; glyph < 3; ++glyph) {
                glyphs::GlyphOutline plain{};
                glyphs::GlyphOutline outline{};
                i32 advance = 0;
                const bool done = glyphs::load_scaled_glyph(font, glyph, scale, scale, limits, plain).succeeded() &&
                                  hinter.hint_glyph(font, glyph, size, mode, limits, outline, advance).succeeded();
                CHECK(done && same_outline(plain, outline));
                const i32 scaled_advance = times(glyphs::advance_of(font, glyph), scale);
                CHECK(advance == ((scaled_advance + 32) & ~63));
            }
        }
    }
}

std::uint64_t random_state = 0x9e3779b97f4a7c15ULL;
[[nodiscard]] std::uint64_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

// Everything the hinter does with a font, whatever it is.
void use(const Bytes& bytes, std::uint64_t& prepared, std::uint64_t& hinted) {
    glyphs::Font font{};
    if (glyphs::open_font(view_of(bytes), 0, font).failed()) return;
    glyphs::Hinter hinter{};
    if (hinter.prepare(font).failed()) return;
    prepared = prepared + 1;
    glyphs::FontLimits limits{};
    limits.components = 40;
    glyphs::GlyphOutline outline{};
    const u32 count = font.glyph_count < 16 ? font.glyph_count : 16;
    for (u32 glyph = 0; glyph < count; ++glyph) {
        i32 advance = 0;
        if (hinter.hint_glyph(font, glyph, 11 * 64, glyphs::HintMode::light, limits, outline, advance).succeeded()) hinted = hinted + 1;
        if (hinter.hint_glyph(font, glyph, 23 * 64 + 5, glyphs::HintMode::monochrome, limits, outline, advance).succeeded()) hinted = hinted + 1;
    }
}

void robustness() {
    const Bytes font = letter_font();
    std::uint64_t tried = 0;
    std::uint64_t prepared = 0;
    std::uint64_t hinted = 0;
    // Prefixes: preparing a font looks up every character of the table of
    // scripts, so the lengths are taken a few bytes apart.
    for (usize length = 0; length <= font.size(); length = length + 5) {
        const Bytes prefix(font.begin(), font.begin() + static_cast<std::ptrdiff_t>(length));
        use(prefix, prepared, hinted);
        tried = tried + 1;
    }
    // One to four bytes changed, many times.
    for (int round = 0; round < 1500; ++round) {
        Bytes changed = font;
        const usize changes = 1 + next_random() % 4;
        for (usize change = 0; change < changes; ++change) changed[next_random() % changed.size()] = static_cast<u8>(next_random() & 0xFFU);
        use(changed, prepared, hinted);
        tried = tried + 1;
    }
    // Every byte of the glyphs in turn set to each of two values: the
    // outlines are what the hinter reads most closely.
    const u8 values[] = {0xFF, 0x80};
    for (usize at = 0; at < font.size(); at = at + 3) {
        for (const u8 value : values) {
            Bytes changed = font;
            changed[at] = value;
            use(changed, prepared, hinted);
            tried = tried + 1;
        }
    }
    std::printf("robustness: %llu fonts tried, %llu prepared, %llu glyphs hinted\n", static_cast<unsigned long long>(tried),
                static_cast<unsigned long long>(prepared), static_cast<unsigned long long>(hinted));
    CHECK(prepared > 500 && hinted > 5000);
}

} // namespace

// With "--write-font <path>" the font of the letters is written out, to be
// looked at with another program, and nothing is tested.
int main(const int argument_count, const char* const* const arguments) {
    if (argument_count == 3 && std::strcmp(arguments[1], "--write-font") == 0) {
        const Bytes font = letter_font();
        std::FILE* const file = std::fopen(arguments[2], "wb");
        if (file == nullptr) return 2;
        const usize written = std::fwrite(font.data(), 1, font.size(), file);
        const int closed = std::fclose(file);
        const int status = (written == font.size() && closed == 0) ? 0 : 2;
        return status;
    }
    refusals();
    light_by_hand();
    light_at_sizes();
    monochrome_at_sizes();
    repeated();
    without_blue_zones();
    robustness();
    const int status = stxlib_tests::finish("stxlib_glyph_hinter_tests");
    return status;
}
