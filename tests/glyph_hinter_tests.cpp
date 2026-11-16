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
//   H8  accents: the dot of i, a bar over a capital and a dot under o are
//       a pixel clear of the letter at every size; a tilde over n is too,
//       or a quarter of a pixel nearer with its middle on the grid, and
//       stays a pixel of wave deep; and at 8 pixels the tilde is where the
//       rule puts it, by hand;
//   H5  the same call twice gives the same outline; a hinter prepared
//       again and one released and prepared give it too;
//   H6  a font with none of the letters that blue zones are measured from
//       is left as scaled; a glyph of no points succeeds;
//   H9  the glyph store: a glyph is read from the font once (after that a
//       font with its bytes taken away serves as well), what it holds is
//       what load_glyph gives, a refused glyph stays refused, and a glyph
//       hinted from the store is the glyph hinted from the font, at every
//       size and in both modes;
//   H10 the store's size: glyphs named in advance are kept while there is
//       room and never given up; of the rest, one that goes unused while
//       a half fills twice is read again and one in use is not; a glyph
//       too large for a half is served and not kept; by count of readings
//       worked out by hand, every outline checked against the reader;
//   F1  a face (glyph_face.hpp): what it refuses; opening reads no glyph
//       and fits none; a fitted outline is the hinter's, cut into
//       segments, and is fitted once however often it is asked for;
//   F2  a glyph's mask from the face is the rasterizer's mask of the
//       hinter's outline, at whole and part pixels, in every fit; rows
//       run from the top (the dot of i is above its stem);
//   F3  a run: one glyph is its mask in its place; several are each
//       glyph's mask put together by the rule (the test's own
//       arithmetic), at positions to 1/64 of a pixel; a glyph over itself
//       is p + p (255 - p) / 255; the box measured is the box drawn;
//   F4  a run's window, a glyph the font has not, and a run too large;
//   F10 fitted text at an angle: the matrix of an angle in whole numbers
//       (quarter turns exact, known values, the same sweep on every host),
//       no turn changes no byte, a quarter turn is the upright mask turned
//       pixel for pixel, a run's places through the matrix, a slight turn
//       changes the drawing slightly, the way back, a turned box;
//   F5  turned text: the matrix of no change changes nothing, a quarter
//       turn turns the box, and a matrix with a hinted fit is refused;
//   F6  the fitted outlines' size: one unused while a half fills twice is
//       fitted again and one in use is not, by count; warming; and
//       drawing a text again reads nothing, fits nothing and holds no
//       more bytes;
//   F7  thicker strokes: a bar and a ring by hand (the left and bottom
//       stay, a pixel more to the right and up, the ink is the area, the
//       counter stays clear); in a run, in every fit; out of range refused;
//   F8  a typeface for the program: faces share what is read; four threads
//       drawing at once from one typeface draw what one draws alone, each
//       glyph read once; byte limits hold and the peak is kept;
//   F9  the font's numbers an application's own font code used to give:
//       heights of small letters and capitals, whether a feature is
//       listed (and a table cut short), a glyph's box at a size;
//   H7  robustness: every prefix of the font and many copies with bytes
//       changed are prepared and hinted; each either succeeds or is
//       refused (the sanitizer builds are what make this a memory check).
//
// The hinter is held to FreeType 2.14.2 on the bundled fonts by the seat's
// comparison (PROVENANCE.md); nothing here needs FreeType.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_face.hpp"
#include "stxlib/glyph_hinter.hpp"
#include "stxlib/glyph_mask.hpp"
#include "stxlib/glyph_store.hpp"
#include "stxlib/shared.hpp"
#include "stxlib/threads.hpp"

#include "font_builder.hpp"
#include "test_support.hpp"

#include <cmath>
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
constexpr u32 glyph_A_acute = 11;
constexpr u32 glyph_n_tilde = 12;
constexpr u32 glyph_o_dot_below = 13;
constexpr u32 glyph_count = 14;

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
    // A capital with an accent over it: H's outline (it stands for the
    // letter) and a bar 40 above it.
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
                                         {130, 0, true}},
                                        {{200, 740, true}, {200, 800, true}, {300, 800, true}, {300, 740, true}}}));
    // n with a tilde 60 above it: a band 100 high that rises, dips and
    // rises, its turns round (a point on the curve level between two
    // control points).
    glyph_bytes.push_back(simple_glyph({{{50, 0, true},
                                         {50, 500, true},
                                         {400, 500, true},
                                         {400, 0, true},
                                         {320, 0, true},
                                         {320, 430, true},
                                         {130, 430, true},
                                         {130, 0, true}},
                                        {{100, 560, true},
                                         {100, 600, true},
                                         {140, 660, false},
                                         {180, 660, true},
                                         {220, 660, false},
                                         {280, 610, false},
                                         {320, 610, true},
                                         {360, 610, false},
                                         {400, 640, true},
                                         {400, 600, true},
                                         {360, 570, false},
                                         {320, 570, true},
                                         {280, 570, false},
                                         {220, 620, false},
                                         {180, 620, true},
                                         {140, 620, false}}}));
    // o with a dot 80 below it.
    glyph_bytes.push_back(simple_glyph({{{50, 0, true}, {50, 500, true}, {400, 500, true}, {400, 0, true}},
                                        {{130, 70, true}, {320, 70, true}, {320, 430, true}, {130, 430, true}},
                                        {{185, -160, true}, {185, -80, true}, {265, -80, true}, {265, -160, true}}}));
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

// The font of the letters: 0 1 H O h i m n o p, and A with acute, n with
// tilde and o with dot below (U+00C1, U+00F1, U+1ECD).
[[nodiscard]] Bytes letter_font() {
    const std::vector<Metric> metrics = {{500, 0}, {500, 60}, {500, 210}, {500, 50}, {500, 50}, {450, 50},
                                         {180, 50}, {720, 50}, {450, 50}, {450, 50}, {450, 50}, {500, 50},
                                         {450, 50}, {450, 50}};
    const Bytes map = map_format_4({{'0', '1', static_cast<i32>(glyph_zero) - '0', {}},
                                    {'H', 'H', static_cast<i32>(glyph_H) - 'H', {}},
                                    {'O', 'O', static_cast<i32>(glyph_O) - 'O', {}},
                                    {'h', 'i', static_cast<i32>(glyph_h) - 'h', {}},
                                    {'m', 'p', static_cast<i32>(glyph_m) - 'm', {}},
                                    {0xC1, 0xC1, static_cast<i32>(glyph_A_acute) - 0xC1, {}},
                                    {0xF1, 0xF1, static_cast<i32>(glyph_n_tilde) - 0xF1, {}},
                                    {0x1ECD, 0x1ECD, static_cast<i32>(glyph_o_dot_below) - 0x1ECD, {}},
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

// The least and the greatest hinted y of contour `contour`.
struct Reach final {
    i32 least{0};
    i32 greatest{0};
};

[[nodiscard]] Reach reach_of(const glyphs::GlyphOutline& outline, const usize contour) {
    const usize first = contour == 0 ? 0 : static_cast<usize>(outline.contour_ends[contour - 1]) + 1;
    const usize last = outline.contour_ends[contour];
    Reach reach{.least = outline.points[first].y, .greatest = outline.points[first].y};
    for (usize index = first; index <= last; ++index) {
        const i32 y = outline.points[index].y;
        reach.least = y < reach.least ? y : reach.least;
        reach.greatest = y > reach.greatest ? y : reach.greatest;
    }
    return reach;
}

void accents() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    CHECK(glyphs::glyph_of(opened.font, 0xC1) == glyph_A_acute && glyphs::glyph_of(opened.font, 0xF1) == glyph_n_tilde &&
          glyphs::glyph_of(opened.font, 0x1ECD) == glyph_o_dot_below);
    struct Accented final {
        u32 glyph{0};
        // The letter's contour and the accent's, and whether the accent
        // is over the letter.
        usize letter{0};
        usize accent{0};
        bool over{true};
    };
    const Accented accented[] = {
        {glyph_i, 0, 1, true}, {glyph_A_acute, 0, 1, true}, {glyph_n_tilde, 0, 1, true}, {glyph_o_dot_below, 0, 2, false}};
    const glyphs::HintMode modes[] = {glyphs::HintMode::light, glyphs::HintMode::monochrome};
    for (const glyphs::HintMode mode : modes) {
        for (const Accented& entry : accented) {
            int clear = 0;
            for (i64 pixels = 8; pixels <= 40; ++pixels) {
                Hinted hinted{};
                hint(opened, entry.glyph, pixels * 64, mode, hinted);
                if (!hinted.done || hinted.outline.contour_ends.count() <= entry.accent) continue;
                const Reach letter = reach_of(hinted.outline, entry.letter);
                const Reach accent = reach_of(hinted.outline, entry.accent);
                const i32 gap = entry.over ? accent.least - letter.greatest : letter.least - accent.greatest;
                // A pixel clear; or, if the accent already ran into the
                // letter by more than rounding, left as it was. A tilde
                // may be a quarter of a pixel nearer: one under three
                // pixels tall has its middle on the grid, and is not moved
                // when the move would only take it off again.
                const bool is_tilde = entry.glyph == glyph_n_tilde;
                const bool good = gap >= (is_tilde ? 48 : 64) || gap < -2;
                if (!good) std::fprintf(stderr, "  mode %d glyph %u at %lld pixels: gap %d\n", static_cast<int>(mode), entry.glyph, static_cast<long long>(pixels), gap);
                CHECK(good);
                if (gap >= 64) clear = clear + 1;
                if (entry.glyph == glyph_n_tilde) {
                    // The tilde's wave is a pixel deep at least.
                    const bool deep = accent.greatest - accent.least >= 64;
                    if (!deep) std::fprintf(stderr, "  mode %d: the tilde at %lld pixels is %d high\n", static_cast<int>(mode), static_cast<long long>(pixels), accent.greatest - accent.least);
                    CHECK(deep);
                }
            }
            std::printf("accents: mode %d glyph %u clear by a pixel at %d of 33 sizes\n", static_cast<int>(mode), entry.glyph, clear);
            CHECK(entry.glyph == glyph_n_tilde ? clear >= 20 : clear == 33);
        }
    }
    // The tilde at 8 pixels, by hand. The scale is 33554; n's top, 500, is
    // 256, four pixels. The tilde's bottom, 560, is 287 and its top, 660,
    // is 338: 51 high. Its inner turns are the top of its lower side, 620
    // (317), 21 under its top, and the bottom of its upper side, 610
    // (312), 25 over its bottom; the nearer, 21, and a pixel make 85, so
    // the tilde is made 85 high from its bottom up: 287 to 372. It is put
    // on the grid: its bottom to the nearest pixel, 256, and then, being
    // under three pixels, down by half of what its height is over one
    // pixel, 10: 246 to 331. That is 10 inside the letter, so it is moved
    // up 74, to a pixel above the letter: 320 to 405.
    Hinted tilde{};
    hint(opened, glyph_n_tilde, 8 * 64, glyphs::HintMode::light, tilde);
    if (tilde.done) {
        const Reach letter = reach_of(tilde.outline, 0);
        const Reach accent = reach_of(tilde.outline, 1);
        if (letter.greatest != 256 || accent.least != 320 || accent.greatest != 405) {
            std::fprintf(stderr, "  tilde at 8 pixels: letter to %d, tilde %d..%d\n", letter.greatest, accent.least, accent.greatest);
        }
        CHECK(letter.least == 0 && letter.greatest == 256);
        CHECK(accent.least == 320 && accent.greatest == 405);
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

void stored() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::GlyphStore store{};
    glyphs::StoredGlyph glyph{};
    // A store not opened holds no font's glyphs.
    const stxlib::Status unopened = store.glyph(opened.font, glyph_H, glyph);
    CHECK(unopened.failed() && (*unopened.error()).kind() == stxlib::ErrorKind::invalid_argument);
    CHECK(store.open(opened.font, opened.limits).succeeded());
    CHECK(store.glyphs_read() == 0);
    const stxlib::Status beyond = store.glyph(opened.font, glyph_count, glyph);
    CHECK(beyond.failed() && (*beyond.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // What it holds is what the reader gives.
    for (u32 index = 0; index < glyph_count; ++index) {
        glyphs::GlyphOutline loaded{};
        const bool read = glyphs::load_glyph(opened.font, index, opened.limits, loaded).succeeded() && store.glyph(opened.font, index, glyph).succeeded();
        CHECK(read);
        if (!read) continue;
        bool same = glyph.points.count() == loaded.points.count() && glyph.tags.count() == loaded.tags.count() &&
                    glyph.contour_ends.count() == loaded.contour_ends.count() && glyph.advance == glyphs::advance_of(opened.font, index);
        for (usize point = 0; same && point < loaded.points.count(); ++point) {
            same = glyph.points[point].x == loaded.points[point].x && glyph.points[point].y == loaded.points[point].y &&
                   glyph.tags[point] == loaded.tags[point];
        }
        for (usize contour = 0; same && contour < loaded.contour_ends.count(); ++contour) same = glyph.contour_ends[contour] == loaded.contour_ends[contour];
        CHECK(same);
    }
    CHECK(store.glyphs_read() == glyph_count);
    // Read once: a font with no bytes behind it serves from here on, and
    // nothing more is counted as read.
    glyphs::Font hollow = opened.font;
    hollow.bytes = stxlib::View<u8>();
    hollow.glyf = glyphs::FontTable{};
    hollow.loca = glyphs::FontTable{};
    hollow.hmtx = glyphs::FontTable{};
    for (u32 index = 0; index < glyph_count; ++index) CHECK(store.glyph(hollow, index, glyph).succeeded());
    CHECK(store.glyphs_read() == glyph_count);
    // Hinted from the store, a glyph is the glyph hinted from the font.
    const glyphs::HintMode modes[] = {glyphs::HintMode::light, glyphs::HintMode::monochrome};
    glyphs::GlyphOutline from_store{};
    glyphs::GlyphOutline from_font{};
    for (const glyphs::HintMode mode : modes) {
        for (i64 size = 8 * 64; size <= 40 * 64; size = size + 53) {
            for (u32 index = 0; index < glyph_count; ++index) {
                i32 store_advance = 0;
                i32 font_advance = 0;
                const bool done = store.glyph(opened.font, index, glyph).succeeded() &&
                                  opened.hinter.hint_stored(opened.font, index, glyph, size, mode, opened.limits, from_store, store_advance).succeeded() &&
                                  opened.hinter.hint_glyph(opened.font, index, size, mode, opened.limits, from_font, font_advance).succeeded();
                CHECK(done && same_outline(from_store, from_font) && store_advance == font_advance);
            }
        }
    }
    // A glyph the reader refuses is refused each time, and read once: H
    // under a limit of eleven points.
    glyphs::FontLimits few{};
    few.points = 11;
    glyphs::GlyphStore strict{};
    CHECK(strict.open(opened.font, few).succeeded());
    const stxlib::Status first = strict.glyph(opened.font, glyph_H, glyph);
    const stxlib::Status second = strict.glyph(hollow, glyph_H, glyph);
    CHECK(first.failed() && (*first.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    CHECK(second.failed() && (*second.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    CHECK(strict.glyphs_read() == 1 && glyph.points.empty());
    // An outline larger than the hinter's limit is not taken from a store.
    CHECK(store.glyph(opened.font, glyph_H, glyph).succeeded());
    i32 advance = 0;
    const stxlib::Status over = opened.hinter.hint_stored(opened.font, glyph_H, glyph, 1024, glyphs::HintMode::light, few, from_store, advance);
    CHECK(over.failed() && (*over.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // Released, it holds nothing.
    store.release();
    CHECK(store.glyphs_read() == 0 && store.glyph(opened.font, glyph_H, glyph).failed());
}

// What the store gives for `glyph` is what the reader gives.
[[nodiscard]] bool serves(const Opened& opened, glyphs::GlyphStore& store, const u32 glyph) {
    glyphs::GlyphOutline loaded{};
    glyphs::StoredGlyph stored{};
    if (glyphs::load_glyph(opened.font, glyph, opened.limits, loaded).failed() || store.glyph(opened.font, glyph, stored).failed()) return false;
    bool same = stored.points.count() == loaded.points.count() && stored.tags.count() == loaded.tags.count() &&
                stored.contour_ends.count() == loaded.contour_ends.count() && stored.advance == glyphs::advance_of(opened.font, glyph);
    for (usize point = 0; same && point < loaded.points.count(); ++point) {
        same = stored.points[point].x == loaded.points[point].x && stored.points[point].y == loaded.points[point].y &&
               stored.tags[point] == loaded.tags[point];
    }
    for (usize contour = 0; same && contour < loaded.contour_ends.count(); ++contour) same = stored.contour_ends[contour] == loaded.contour_ends[contour];
    return same;
}

void store_size() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    // A glyph takes 9 bytes a point, 4 a contour and 24: 1 takes 64; n 100;
    // i and o 104; h 118; p 122; H and m 136; O and 0 176. A part holds
    // half its storage: room for 450 bytes kept, and two halves of 410.
    const glyphs::StoreLimits small{.kept_bytes = 900, .recent_bytes = 1640};
    glyphs::GlyphStore store{};
    CHECK(store.open(opened.font, opened.limits, small).succeeded());
    // H, O and m, 448 bytes, fill the kept part; n has no room there and
    // goes with the recent ones; the font has no z.
    const u32 likely[] = {'H', 'O', 'm', 'n', 'z'};
    usize kept = 0;
    CHECK(store.keep(opened.font, stxlib::View<u32>(likely), kept).succeeded() && kept == 3);
    CHECK(store.glyphs_read() == 4);
    // Named again, nothing changes.
    CHECK(store.keep(opened.font, stxlib::View<u32>(likely), kept).succeeded() && kept == 3 && store.glyphs_read() == 4);
    // The newer half: n, i, o are 308 bytes; p (122) does not fit, so the
    // (empty) older half is given up and p begins a new one.
    CHECK(serves(opened, store, glyph_i) && serves(opened, store, glyph_o) && serves(opened, store, glyph_p));
    CHECK(store.glyphs_read() == 7);
    // n is in the older half: it is moved to the newer, not read.
    CHECK(serves(opened, store, glyph_n) && store.glyphs_read() == 7);
    // h and 1 fill the newer half: p, n, h, 1 are 404 bytes.
    CHECK(serves(opened, store, glyph_h) && serves(opened, store, glyph_one) && store.glyphs_read() == 9);
    // 0 (176) does not fit: the older half (i and o; n has left it) is
    // given up.
    CHECK(serves(opened, store, glyph_zero) && store.glyphs_read() == 10);
    // n, in use all along, is still held; i is read again.
    CHECK(serves(opened, store, glyph_n) && store.glyphs_read() == 10);
    CHECK(serves(opened, store, glyph_i) && store.glyphs_read() == 11);
    // The kept glyphs were never given up.
    CHECK(serves(opened, store, glyph_H) && serves(opened, store, glyph_O) && serves(opened, store, glyph_m) && store.glyphs_read() == 11);
    // Through all of that the store agrees with itself.
    CHECK(store.whole());
    // Round and round: every glyph, many times, is always the reader's.
    bool all = true;
    for (u32 round = 0; round < 50; ++round) {
        for (u32 glyph = 0; glyph < glyph_count; ++glyph) all = all && serves(opened, store, (glyph * 7 + round) % glyph_count);
    }
    CHECK(all && store.whole());
    // The storage is never more than its limits, the index and the room a
    // glyph is read into.
    CHECK(store.bytes_held() <= 900 + 1640 + 4 * glyph_count + 600);
    // Halves of 110 bytes: H (136) is too large for one. It is served, and
    // read each time; i (104) is held.
    const glyphs::StoreLimits tiny{.kept_bytes = 0, .recent_bytes = 440};
    glyphs::GlyphStore narrow{};
    CHECK(narrow.open(opened.font, opened.limits, tiny).succeeded());
    CHECK(serves(opened, narrow, glyph_H) && serves(opened, narrow, glyph_H) && narrow.glyphs_read() == 2);
    CHECK(serves(opened, narrow, glyph_i) && serves(opened, narrow, glyph_i) && narrow.glyphs_read() == 3);
    // Nothing kept at all still serves.
    const glyphs::StoreLimits none{.kept_bytes = 0, .recent_bytes = 0};
    glyphs::GlyphStore empty{};
    CHECK(empty.open(opened.font, opened.limits, none).succeeded());
    CHECK(serves(opened, empty, glyph_m) && serves(opened, empty, glyph_one) && empty.glyphs_read() == 2);
}

// The mask of `glyph` by the long way: the hinter's outline from the font,
// cut into segments, measured and drawn by the rasterizer.
[[nodiscard]] bool long_way(Opened& opened, const u32 glyph, const i64 size, const glyphs::Fit fit, const glyphs::Point shift,
                            glyphs::Bounds& bounds, std::vector<u8>& pixels) {
    glyphs::GlyphOutline outline{};
    i32 advance = 0;
    if (fit == glyphs::Fit::none) {
        const i64 scale = glyphs::scale_of(opened.font, size);
        if (glyphs::load_glyph(opened.font, glyph, opened.limits, outline).failed()) return false;
        glyphs::scale_outline(outline, scale, scale);
    } else {
        const glyphs::HintMode mode = fit == glyphs::Fit::monochrome ? glyphs::HintMode::monochrome : glyphs::HintMode::light;
        if (opened.hinter.hint_glyph(opened.font, glyph, size, mode, opened.limits, outline, advance).failed()) return false;
    }
    stxlib::Vector<glyphs::Segment> kinds{};
    stxlib::Vector<glyphs::Point> points{};
    if (glyphs::to_segments(outline, kinds, points).failed()) return false;
    const glyphs::Outline cut{.segments = kinds.view(), .points = points.view()};
    const glyphs::Limits limits{};
    if (glyphs::measure(cut, shift, limits, bounds).failed()) return false;
    pixels.assign(static_cast<usize>(bounds.width) * bounds.height, 0);
    if (pixels.empty()) return true;
    glyphs::Rasterizer rasterizer{};
    const stxlib::MutableView<u8> target(pixels.data(), pixels.size());
    const stxlib::Status drawn = fit == glyphs::Fit::monochrome ? rasterizer.render_monochrome(cut, shift, bounds, limits, target)
                                                               : rasterizer.render(cut, shift, bounds, limits, target);
    return drawn.succeeded();
}

[[nodiscard]] bool same_bounds(const glyphs::Bounds& a, const glyphs::Bounds& b) {
    const bool result = a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
    return result;
}

void face_basics() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::Face face{};
    glyphs::FittedGlyph fitted{};
    // A face not opened.
    CHECK(face.font() == nullptr && face.bytes_held() == 0);
    const stxlib::Status unopened = face.fitted(glyph_H, 1024, glyphs::Fit::light, fitted);
    CHECK(unopened.failed() && (*unopened.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // Not a font.
    const u8 nothing[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(face.open(stxlib::View<u8>(nothing), 0).failed() && face.font() == nullptr);
    CHECK(face.open(view_of(opened.bytes), 0).succeeded());
    CHECK(face.font() != nullptr && (*face.font()).glyph_count == glyph_count);
    // Opening reads no glyph and fits none.
    CHECK((*face.typeface()).glyphs_read() == 0 && face.glyphs_fitted() == 0);
    const stxlib::Status beyond = face.fitted(glyph_count, 1024, glyphs::Fit::light, fitted);
    const stxlib::Status no_size = face.fitted(glyph_H, 0, glyphs::Fit::light, fitted);
    const stxlib::Status huge = face.fitted(glyph_H, (static_cast<i64>(1) << 24) + 1, glyphs::Fit::light, fitted);
    CHECK(beyond.failed() && no_size.failed() && huge.failed());
    CHECK((*face.typeface()).glyphs_read() == 0 && face.glyphs_fitted() == 0);
    // A fitted outline is the hinter's, cut into segments; its box is the
    // box of its points; asked for again it is not fitted again.
    const glyphs::Fit fits[] = {glyphs::Fit::none, glyphs::Fit::light, glyphs::Fit::monochrome};
    usize expected_fittings = 0;
    for (const glyphs::Fit fit : fits) {
        for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
            glyphs::GlyphOutline outline{};
            i32 advance = 0;
            const i64 size = 13 * 64 + 7;
            bool had = true;
            if (fit == glyphs::Fit::none) {
                const i64 scale = glyphs::scale_of(opened.font, size);
                had = glyphs::load_glyph(opened.font, glyph, opened.limits, outline).succeeded();
                glyphs::scale_outline(outline, scale, scale);
                advance = times(glyphs::advance_of(opened.font, glyph), scale);
            } else {
                const glyphs::HintMode mode = fit == glyphs::Fit::monochrome ? glyphs::HintMode::monochrome : glyphs::HintMode::light;
                had = opened.hinter.hint_glyph(opened.font, glyph, size, mode, opened.limits, outline, advance).succeeded();
            }
            stxlib::Vector<glyphs::Segment> kinds{};
            stxlib::Vector<glyphs::Point> points{};
            had = had && glyphs::to_segments(outline, kinds, points).succeeded() && face.fitted(glyph, size, fit, fitted).succeeded();
            CHECK(had);
            if (!had) continue;
            expected_fittings = expected_fittings + 1;
            bool same = fitted.outline.segments.count() == kinds.count() && fitted.outline.points.count() == points.count() && fitted.advance == advance;
            for (usize index = 0; same && index < kinds.count(); ++index) same = fitted.outline.segments[index] == kinds[index];
            bool boxed = true;
            for (usize index = 0; same && index < points.count(); ++index) {
                same = fitted.outline.points[index].x == points[index].x && fitted.outline.points[index].y == points[index].y;
                boxed = boxed && points[index].x >= fitted.left && points[index].x <= fitted.right && points[index].y >= fitted.bottom &&
                        points[index].y <= fitted.top;
            }
            CHECK(same && boxed);
            CHECK(face.fitted(glyph, size, fit, fitted).succeeded() && face.glyphs_fitted() == expected_fittings);
        }
    }
    // Each glyph was read from the font once for all three fits.
    CHECK((*face.typeface()).glyphs_read() == glyph_count);
    face.release();
    CHECK(face.font() == nullptr && face.bytes_held() == 0 && face.typeface().empty());
}

void face_masks() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::Face face{};
    CHECK(face.open(view_of(opened.bytes), 0).succeeded());
    const glyphs::Fit fits[] = {glyphs::Fit::none, glyphs::Fit::light, glyphs::Fit::monochrome};
    const glyphs::Point shifts[] = {{0, 0}, {17, 0}, {63, -1}, {5, -40}, {-130, 77}};
    const i64 sizes[] = {9 * 64, 16 * 64, 23 * 64 + 31};
    stxlib::Vector<u8> pixels{};
    std::vector<u8> expected{};
    bool all = true;
    bool mono_is_two_valued = true;
    for (const glyphs::Fit fit : fits) {
        for (const i64 size : sizes) {
            for (const glyphs::Point shift : shifts) {
                for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
                    glyphs::Bounds bounds{};
                    glyphs::Bounds expected_bounds{};
                    glyphs::DrawStyle style{};
                    style.size = size;
                    style.fit = fit;
                    const bool had = face.mask(glyph, style, shift, bounds, pixels).succeeded() &&
                                     long_way(opened, glyph, size, fit, shift, expected_bounds, expected);
                    bool same = had && same_bounds(bounds, expected_bounds) && pixels.count() == expected.size();
                    for (usize index = 0; same && index < expected.size(); ++index) {
                        same = pixels[index] == expected[index];
                        if (fit == glyphs::Fit::monochrome && pixels[index] != 0 && pixels[index] != 255) mono_is_two_valued = false;
                    }
                    if (!same) std::fprintf(stderr, "  mask: fit %d size %lld glyph %u differs\n", static_cast<int>(fit), static_cast<long long>(size), glyph);
                    all = all && same;
                }
            }
        }
    }
    CHECK(all);
    CHECK(mono_is_two_valued);
    // Rows run from the top: i's first rows of ink are its dot, which is
    // shorter than the stem under it.
    glyphs::Bounds bounds{};
    glyphs::DrawStyle large{};
    large.size = 32 * 64;
    CHECK(face.mask(glyph_i, large, glyphs::Point{0, 0}, bounds, pixels).succeeded());
    usize first_block = 0;
    usize gap = 0;
    usize second_block = 0;
    for (usize row = 0; row < bounds.height; ++row) {
        bool ink = false;
        for (usize x = 0; x < bounds.width; ++x) ink = ink || pixels[row * bounds.width + x] > 128;
        if (ink && gap == 0) first_block = first_block + 1;
        if (!ink && first_block > 0 && second_block == 0) gap = gap + 1;
        if (ink && gap > 0) second_block = second_block + 1;
    }
    if (!(first_block >= 2 && gap >= 1 && second_block > 3 * first_block)) {
        std::fprintf(stderr, "  i at 32 pixels: %zu rows, then %zu clear, then %zu\n", first_block, gap, second_block);
    }
    CHECK(first_block >= 2 && gap >= 1 && second_block > 3 * first_block);
}

// A run by the test's own arithmetic: each glyph's mask from the face,
// placed by its pixel and put together by the rule.
struct OwnRun final {
    std::vector<u8> pixels{};
    i64 left{0};
    i64 top{0};
    i64 width{0};
    i64 height{0};
};

[[nodiscard]] bool own_run(glyphs::Face& face, const std::vector<glyphs::RunGlyph>& run, const glyphs::DrawStyle& style, OwnRun& own) {
    const glyphs::Fit fit = style.fit;
    struct Piece final {
        std::vector<u8> pixels{};
        i64 left{0};
        i64 top{0};
        i64 width{0};
        i64 height{0};
    };
    std::vector<Piece> pieces{};
    bool any = false;
    i64 left = 0;
    i64 top = 0;
    i64 right = 0;
    i64 bottom = 0;
    for (const glyphs::RunGlyph& glyph : run) {
        // Floor division by 64, the test's own.
        const i64 pixel_x = glyph.x >= 0 ? glyph.x / 64 : -((-static_cast<i64>(glyph.x) + 63) / 64);
        const i64 pixel_y = glyph.y >= 0 ? glyph.y / 64 : -((-static_cast<i64>(glyph.y) + 63) / 64);
        const glyphs::Point shift{static_cast<i32>(glyph.x - pixel_x * 64), static_cast<i32>(-(glyph.y - pixel_y * 64))};
        glyphs::Bounds bounds{};
        stxlib::Vector<u8> pixels{};
        if (face.mask(glyph.glyph, style, shift, bounds, pixels).failed()) return false;
        if (bounds.width == 0 || bounds.height == 0) continue;
        Piece piece{};
        piece.left = pixel_x + bounds.left;
        piece.top = pixel_y - bounds.top;
        piece.width = bounds.width;
        piece.height = bounds.height;
        piece.pixels.assign(pixels.view().data(), pixels.view().data() + pixels.count());
        left = (!any || piece.left < left) ? piece.left : left;
        top = (!any || piece.top < top) ? piece.top : top;
        right = (!any || piece.left + piece.width > right) ? piece.left + piece.width : right;
        bottom = (!any || piece.top + piece.height > bottom) ? piece.top + piece.height : bottom;
        any = true;
        pieces.push_back(piece);
    }
    own = OwnRun{};
    if (!any) return true;
    own.left = left;
    own.top = top;
    own.width = right - left;
    own.height = bottom - top;
    own.pixels.assign(static_cast<usize>(own.width * own.height), 0);
    for (const Piece& piece : pieces) {
        for (i64 row = 0; row < piece.height; ++row) {
            for (i64 column = 0; column < piece.width; ++column) {
                u8& target = own.pixels[static_cast<usize>((piece.top - top + row) * own.width + piece.left - left + column)];
                const unsigned fresh = piece.pixels[static_cast<usize>(row * piece.width + column)];
                const unsigned before = target;
                const unsigned together = fit == glyphs::Fit::monochrome ? (fresh > before ? fresh : before) : fresh + before * (255U - fresh) / 255U;
                target = static_cast<u8>(together);
            }
        }
    }
    return true;
}

[[nodiscard]] bool run_is(const glyphs::RunCoverage& coverage, const OwnRun& own) {
    bool same = coverage.left == own.left && coverage.top == own.top && coverage.width == own.width && coverage.height == own.height &&
                coverage.pixels.count() == own.pixels.size();
    for (usize index = 0; same && index < own.pixels.size(); ++index) same = coverage.pixels[index] == own.pixels[index];
    return same;
}

void face_runs() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::Face face{};
    CHECK(face.open(view_of(opened.bytes), 0).succeeded());
    glyphs::RunCoverage coverage{};
    glyphs::RunBox box{};
    const glyphs::Fit fits[] = {glyphs::Fit::none, glyphs::Fit::light, glyphs::Fit::monochrome};
    // A run of no glyphs.
    glyphs::DrawStyle style{};
    style.size = 16 * 64;
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(), style, coverage).succeeded() && coverage.pixels.empty() && coverage.width == 0);
    // One glyph, on a pixel and off it, here and far from the origin.
    const glyphs::RunGlyph places[] = {{glyph_H, 0, 0}, {glyph_p, 64 * 5, 64 * 3}, {glyph_O, 37, 0}, {glyph_n, -1000, 129}, {glyph_i, 64 * 40 + 63, -64 * 7 - 1}};
    for (const glyphs::Fit fit : fits) {
        style.fit = fit;
        for (const glyphs::RunGlyph& place : places) {
            const std::vector<glyphs::RunGlyph> run = {place};
            OwnRun own{};
            const bool had = own_run(face, run, style, own) &&
                             face.draw_run(stxlib::View<glyphs::RunGlyph>(run.data(), run.size()), style, coverage).succeeded();
            CHECK(had && run_is(coverage, own) && coverage.missing_glyphs == 0 && !coverage.oversized);
        }
    }
    // A line of text: glyphs side by side at parts of a pixel, some
    // touching, some far apart, one below the line, one twice in a place.
    const std::vector<glyphs::RunGlyph> line = {{glyph_H, 0, 0},           {glyph_o, 512 + 13, 0},   {glyph_n, 973 + 13, 0}, {glyph_i, 1434 + 40, 0},
                                                {glyph_n_tilde, 1618, 0},   {glyph_p, 2079 + 7, 31},  {glyph_m, 2540, -64},   {glyph_m, 2540, -64},
                                                {glyph_O, 2600, 5},         {glyph_zero, 9000, 700},  {0, 3000, 0},           {glyph_A_acute, -700, -33}};
    for (const glyphs::Fit fit : fits) {
        for (i64 size = 9 * 64; size <= 33 * 64; size = size + 8 * 64 + 21) {
            style.fit = fit;
            style.size = size;
            OwnRun own{};
            const stxlib::View<glyphs::RunGlyph> view(line.data(), line.size());
            const bool had = own_run(face, line, style, own) && face.draw_run(view, style, coverage).succeeded() &&
                             face.measure_run(view, style, box).succeeded();
            CHECK(had && run_is(coverage, own));
            // The box measured is the box drawn.
            CHECK(box.left == coverage.left && box.top == coverage.top && box.right - box.left == static_cast<i32>(coverage.width) &&
                  box.bottom - box.top == static_cast<i32>(coverage.height) && box.missing_glyphs == 0 && !box.oversized);
        }
    }
    // A glyph over itself: every pixel p becomes p + p (255 - p) / 255.
    style.fit = glyphs::Fit::light;
    style.size = 20 * 64;
    const glyphs::RunGlyph once[] = {{glyph_O, 10, 0}};
    const glyphs::RunGlyph twice[] = {{glyph_O, 10, 0}, {glyph_O, 10, 0}};
    glyphs::RunCoverage single{};
    const bool drawn = face.draw_run(stxlib::View<glyphs::RunGlyph>(once), style, single).succeeded() &&
                       face.draw_run(stxlib::View<glyphs::RunGlyph>(twice), style, coverage).succeeded();
    bool doubled = drawn && single.pixels.count() == coverage.pixels.count() && !single.pixels.empty();
    bool some_grey = false;
    for (usize index = 0; doubled && index < single.pixels.count(); ++index) {
        const unsigned p = single.pixels[index];
        doubled = coverage.pixels[index] == p + p * (255U - p) / 255U;
        some_grey = some_grey || (p > 0 && p < 255);
    }
    CHECK(doubled && some_grey);
}

void face_windows() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::Face face{};
    CHECK(face.open(view_of(opened.bytes), 0).succeeded());
    glyphs::RunCoverage coverage{};
    glyphs::DrawStyle style{};
    style.size = 16 * 64;
    // H at 16 pixels is 8 wide and 11 tall, from its origin up. Three of
    // them 100 pixels apart; a window that shows the middle one only.
    const glyphs::RunGlyph three[] = {{glyph_H, 0, 0}, {glyph_H, 6400, 0}, {glyph_H, 12800, 0}};
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(three), style, coverage).succeeded());
    CHECK(coverage.width > 200 && coverage.outside_glyphs == 0);
    const u32 whole_height = coverage.height;
    style.windowed = true;
    style.window_left = 90;
    style.window_right = 120;
    style.window_top = -20;
    style.window_bottom = 5;
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(three), style, coverage).succeeded());
    CHECK(coverage.outside_glyphs == 2 && coverage.width < 12 && coverage.left >= 100 && coverage.height == whole_height);
    // A window nothing is in.
    style.window_left = 300;
    style.window_right = 400;
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(three), style, coverage).succeeded());
    CHECK(coverage.outside_glyphs == 3 && coverage.pixels.empty() && coverage.width == 0);
    style.windowed = false;
    // A glyph the font has not is counted and draws nothing; the rest is
    // drawn.
    const glyphs::RunGlyph with_missing[] = {{glyph_H, 0, 0}, {glyph_count + 5, 640, 0}, {glyph_H, 1280, 0}};
    const glyphs::RunGlyph without[] = {{glyph_H, 0, 0}, {glyph_H, 1280, 0}};
    glyphs::RunCoverage plain{};
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(with_missing), style, coverage).succeeded() &&
          face.draw_run(stxlib::View<glyphs::RunGlyph>(without), style, plain).succeeded());
    bool same = coverage.missing_glyphs == 1 && plain.missing_glyphs == 0 && coverage.pixels.count() == plain.pixels.count() && coverage.width == plain.width;
    for (usize index = 0; same && index < plain.pixels.count(); ++index) same = coverage.pixels[index] == plain.pixels[index];
    CHECK(same);
    // Two glyphs further apart than a mask may be wide: nothing is drawn
    // and the run says so.
    const glyphs::RunGlyph far[] = {{glyph_H, 0, 0}, {glyph_H, 64 * 20000, 0}};
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(far), style, coverage).succeeded());
    CHECK(coverage.oversized && coverage.pixels.empty());
}

void face_turned() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::Face face{};
    CHECK(face.open(view_of(opened.bytes), 0).succeeded());
    glyphs::DrawStyle style{};
    style.size = 20 * 64;
    style.fit = glyphs::Fit::none;
    const glyphs::RunGlyph run[] = {{glyph_h, 33, 0}, {glyph_p, 700, 10}};
    glyphs::RunCoverage plain{};
    glyphs::RunCoverage turned{};
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, plain).succeeded());
    // The matrix of no change.
    style.transformed = true;
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, turned).succeeded());
    bool same = plain.pixels.count() == turned.pixels.count() && plain.left == turned.left && plain.top == turned.top && plain.width == turned.width;
    for (usize index = 0; same && index < plain.pixels.count(); ++index) same = plain.pixels[index] == turned.pixels[index];
    CHECK(same && !plain.pixels.empty());
    // A quarter turn (x' = -y, y' = x): h, 7 units wide and 15 tall at
    // this size (from 1 to 8 across and 0 to 15 up), lies on its side:
    // 15 wide, to the left of its origin, and 7 tall above it.
    const glyphs::RunGlyph one[] = {{glyph_h, 0, 0}};
    style.xx = 0;
    style.xy = -0x10000;
    style.yx = 0x10000;
    style.yy = 0;
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(one), style, turned).succeeded());
    if (!(turned.width == 15 && turned.height == 7 && turned.left == -15 && turned.top == -8)) {
        std::fprintf(stderr, "  h on its side: %u by %u at %d, %d\n", turned.width, turned.height, turned.left, turned.top);
    }
    CHECK(turned.width == 15 && turned.height == 7 && turned.left == -15 && turned.top == -8);
}

// The mask of a glyph, fitted, through a matrix, with a part of a pixel.
struct TurnedMask final {
    glyphs::Bounds bounds{};
    std::vector<u8> pixels{};
    bool had{false};
};

[[nodiscard]] TurnedMask mask_of(glyphs::Face& face, const u32 glyph, const i64 size, const glyphs::Fit fit, const glyphs::TextMatrix* const matrix,
                                 const glyphs::Point shift) {
    TurnedMask result{};
    glyphs::DrawStyle style{};
    style.size = size;
    style.fit = fit;
    if (matrix != nullptr) glyphs::set_matrix(style, *matrix);
    stxlib::Vector<u8> pixels{};
    result.had = face.mask(glyph, style, shift, result.bounds, pixels).succeeded();
    result.pixels.assign(pixels.view().data(), pixels.view().data() + pixels.count());
    return result;
}

// Fitted text at an angle (glyph_face.hpp, "Turned text").
void face_turned_fitted() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::Face face{};
    CHECK(face.open(view_of(opened.bytes), 0).succeeded());
    const i64 quarter = static_cast<i64>(90) << 16;
    // The matrix of an angle: the quarter turns exactly, known values, and
    // minus an angle the transpose.
    const glyphs::TextMatrix none = glyphs::turn_matrix(0);
    const glyphs::TextMatrix left = glyphs::turn_matrix(quarter);
    const glyphs::TextMatrix half = glyphs::turn_matrix(2 * quarter);
    const glyphs::TextMatrix right = glyphs::turn_matrix(3 * quarter);
    CHECK(none.xx == 0x10000 && none.xy == 0 && none.yx == 0 && none.yy == 0x10000);
    CHECK(left.xx == 0 && left.xy == -0x10000 && left.yx == 0x10000 && left.yy == 0);
    CHECK(half.xx == -0x10000 && half.xy == 0 && half.yx == 0 && half.yy == -0x10000);
    CHECK(right.xx == 0 && right.xy == 0x10000 && right.yx == -0x10000 && right.yy == 0);
    const glyphs::TextMatrix whole = glyphs::turn_matrix(4 * quarter);
    const glyphs::TextMatrix back = glyphs::turn_matrix(-quarter);
    CHECK(whole.xx == none.xx && whole.yx == none.yx && back.xx == right.xx && back.xy == right.xy && back.yx == right.yx && back.yy == right.yy);
    // The sine of 30 degrees is a half, of 45 the root of a half (46341 of
    // 65536, rounded), of 60 it is 56756.
    const glyphs::TextMatrix thirty = glyphs::turn_matrix(static_cast<i64>(30) << 16);
    const glyphs::TextMatrix forty_five = glyphs::turn_matrix(static_cast<i64>(45) << 16);
    const glyphs::TextMatrix sixty = glyphs::turn_matrix(static_cast<i64>(60) << 16);
    CHECK(thirty.yx == 32768 && thirty.xx == 56756 && thirty.xy == -32768 && thirty.yy == 56756);
    CHECK(forty_five.yx == 46341 && forty_five.xx == 46341 && sixty.yx == 56756 && sixty.xx == 32768);
    // Every 1009th 65536th of a degree over three turns: each entry within
    // one 65536th of the true value, minus the angle the transpose, and the
    // whole sweep's numbers the same on every host.
    bool near = true;
    bool transposed = true;
    std::uint64_t sweep = 0;
    const i64 full = 4 * quarter;
    for (i64 degrees = -full; degrees < 2 * full; degrees = degrees + 1009) {
        const glyphs::TextMatrix matrix = glyphs::turn_matrix(degrees);
        const glyphs::TextMatrix minus = glyphs::turn_matrix(-degrees);
        const double radians = static_cast<double>(degrees) / 65536.0 * 3.14159265358979323846 / 180.0;
        near = near && std::fabs(static_cast<double>(matrix.yx) - std::sin(radians) * 65536.0) <= 1.0 &&
               std::fabs(static_cast<double>(matrix.xx) - std::cos(radians) * 65536.0) <= 1.0 && matrix.yy == matrix.xx && matrix.xy == -matrix.yx;
        transposed = transposed && minus.xx == matrix.xx && minus.xy == matrix.yx && minus.yx == matrix.xy && minus.yy == matrix.yy;
        const i32 entries[4] = {matrix.xx, matrix.xy, matrix.yx, matrix.yy};
        for (const i32 entry : entries) {
            sweep = sweep ^ static_cast<std::uint32_t>(entry);
            sweep = sweep ^ (sweep << 13);
            sweep = sweep ^ (sweep >> 7);
            sweep = sweep ^ (sweep << 17);
        }
    }
    CHECK(near);
    CHECK(transposed);
    if (sweep != 0xf454c44177900624ULL) std::fprintf(stderr, "  the sweep of angles: %016llx\n", static_cast<unsigned long long>(sweep));
    CHECK(sweep == 0xf454c44177900624ULL);

    // The matrix of no turn changes nothing of fitted text, with any part
    // of a pixel: the bytes are the upright ones.
    const glyphs::Fit fits[] = {glyphs::Fit::light, glyphs::Fit::monochrome, glyphs::Fit::none};
    const u32 letters[] = {glyph_H, glyph_h, glyph_o, glyph_p};
    const glyphs::Point shifts[] = {{0, 0}, {21, -40}, {63, -1}};
    bool unchanged = true;
    for (const glyphs::Fit fit : fits) {
        for (const u32 letter : letters) {
            for (const glyphs::Point shift : shifts) {
                const TurnedMask upright = mask_of(face, letter, 13 * 64, fit, nullptr, shift);
                const TurnedMask through = mask_of(face, letter, 13 * 64, fit, &none, shift);
                unchanged = unchanged && upright.had && through.had && same_bounds(upright.bounds, through.bounds) && upright.pixels == through.pixels &&
                            !upright.pixels.empty();
            }
        }
    }
    CHECK(unchanged);

    // A quarter turn of fitted text is the upright mask turned pixel for
    // pixel: a point (x, y) goes to (-y, x), so the mask's rows become
    // columns, the part of a pixel turned with it.
    bool turned_exactly = true;
    usize differing = 0;
    usize compared = 0;
    for (const glyphs::Fit fit : fits) {
        for (const u32 letter : letters) {
            for (const glyphs::Point shift : shifts) {
                // The upright mask moved by what the turn takes to `shift`.
                const glyphs::Point upright_shift{shift.y, -shift.x};
                const TurnedMask upright = mask_of(face, letter, 13 * 64, fit, nullptr, upright_shift);
                const TurnedMask turned = mask_of(face, letter, 13 * 64, fit, &left, shift);
                const bool shaped = upright.had && turned.had && turned.bounds.width == upright.bounds.height && turned.bounds.height == upright.bounds.width &&
                                    turned.bounds.left == -upright.bounds.top &&
                                    turned.bounds.top == upright.bounds.left + static_cast<i32>(upright.bounds.width);
                turned_exactly = turned_exactly && shaped;
                if (!shaped) continue;
                const usize width = upright.bounds.width;
                const usize height = upright.bounds.height;
                for (usize row = 0; row < height; ++row) {
                    for (usize column = 0; column < width; ++column) {
                        const u8 was = upright.pixels[row * width + column];
                        const u8 is = turned.pixels[(width - 1 - column) * height + row];
                        compared = compared + 1;
                        if (was != is) differing = differing + 1;
                    }
                }
                // A half turn: (x, y) goes to (-x, -y), the mask read from
                // its end. Three quarters: (x, y) goes to (y, -x).
                const TurnedMask upright_half = mask_of(face, letter, 13 * 64, fit, nullptr, glyphs::Point{-shift.x, -shift.y});
                const TurnedMask halved = mask_of(face, letter, 13 * 64, fit, &half, shift);
                bool half_same = upright_half.had && halved.had && halved.bounds.width == upright_half.bounds.width &&
                                 halved.bounds.height == upright_half.bounds.height &&
                                 halved.bounds.left == -(upright_half.bounds.left + static_cast<i32>(upright_half.bounds.width)) &&
                                 halved.bounds.top == static_cast<i32>(upright_half.bounds.height) - upright_half.bounds.top &&
                                 halved.pixels.size() == upright_half.pixels.size();
                for (usize index = 0; half_same && index < halved.pixels.size(); ++index) {
                    half_same = halved.pixels[halved.pixels.size() - 1 - index] == upright_half.pixels[index];
                }
                const TurnedMask upright_right = mask_of(face, letter, 13 * 64, fit, nullptr, glyphs::Point{-shift.y, shift.x});
                const TurnedMask righted = mask_of(face, letter, 13 * 64, fit, &right, shift);
                bool right_same = upright_right.had && righted.had && righted.bounds.width == upright_right.bounds.height &&
                                  righted.bounds.height == upright_right.bounds.width &&
                                  righted.bounds.left == upright_right.bounds.top - static_cast<i32>(upright_right.bounds.height) &&
                                  righted.bounds.top == -upright_right.bounds.left;
                const usize right_width = upright_right.bounds.width;
                const usize right_height = upright_right.bounds.height;
                for (usize row = 0; right_same && row < right_height; ++row) {
                    for (usize column = 0; right_same && column < right_width; ++column) {
                        right_same = righted.pixels[column * right_height + (right_height - 1 - row)] == upright_right.pixels[row * right_width + column];
                    }
                }
                turned_exactly = turned_exactly && half_same && right_same;
            }
        }
    }
    std::printf("quarter turns: %zu pixels compared, %zu differ\n", compared, differing);
    CHECK(turned_exactly && compared > 1000 && differing == 0);
    // In a run too: one glyph a quarter turned is its mask in its place.
    {
        glyphs::DrawStyle quarter_style{};
        quarter_style.size = 13 * 64;
        quarter_style.fit = glyphs::Fit::light;
        glyphs::set_matrix(quarter_style, left);
        const glyphs::RunGlyph one_glyph[] = {{glyph_p, 5 * 64 + 21, 3 * 64 + 40}};
        glyphs::RunCoverage in_run{};
        CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(one_glyph), quarter_style, in_run).succeeded());
        const TurnedMask alone = mask_of(face, glyph_p, 13 * 64, glyphs::Fit::light, &left, glyphs::Point{21, -40});
        bool placed = alone.had && in_run.width == alone.bounds.width && in_run.height == alone.bounds.height && in_run.left == 5 + alone.bounds.left &&
                      in_run.top == 3 - alone.bounds.top && in_run.pixels.count() == alone.pixels.size();
        for (usize index = 0; placed && index < alone.pixels.size(); ++index) placed = in_run.pixels[index] == alone.pixels[index];
        CHECK(placed);
    }

    // A fitted run at an angle: the places taken through the matrix by the
    // face are the caller's own through turn_place.
    const glyphs::TextMatrix slant = glyphs::turn_matrix(static_cast<i64>(17) << 16);
    const glyphs::RunGlyph laid_out[] = {{glyph_H, 0, 0}, {glyph_o, 9 * 64 + 13, 0}, {glyph_p, 18 * 64 + 40, 0}};
    glyphs::RunGlyph placed_by_hand[3] = {};
    for (usize index = 0; index < 3; ++index) {
        const glyphs::Point place = glyphs::turn_place(slant, glyphs::Point{laid_out[index].x, laid_out[index].y});
        placed_by_hand[index] = glyphs::RunGlyph{laid_out[index].glyph, place.x, place.y};
    }
    glyphs::DrawStyle style{};
    style.size = 13 * 64;
    style.fit = glyphs::Fit::light;
    glyphs::set_matrix(style, slant);
    glyphs::RunCoverage by_hand{};
    glyphs::RunCoverage by_face{};
    glyphs::RunBox box{};
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(placed_by_hand), style, by_hand).succeeded());
    style.turn_places = true;
    CHECK(face.draw_run(stxlib::View<glyphs::RunGlyph>(laid_out), style, by_face).succeeded() &&
          face.measure_run(stxlib::View<glyphs::RunGlyph>(laid_out), style, box).succeeded());
    bool same = by_hand.width == by_face.width && by_hand.height == by_face.height && by_hand.left == by_face.left && by_hand.top == by_face.top &&
                by_hand.pixels.count() == by_face.pixels.count() && by_face.missing_glyphs == 0 && by_face.width > 0;
    for (usize index = 0; same && index < by_hand.pixels.count(); ++index) same = by_hand.pixels[index] == by_face.pixels[index];
    CHECK(same);
    // The run measured is the run drawn; turned 17 degrees counter-
    // clockwise, text that ran rightward climbs: it is taller than upright
    // and its last glyph is above its first.
    CHECK(box.right - box.left == static_cast<i32>(by_face.width) && box.bottom - box.top == static_cast<i32>(by_face.height) && box.left == by_face.left &&
          box.top == by_face.top);
    CHECK(placed_by_hand[2].y < placed_by_hand[0].y && placed_by_hand[2].x > placed_by_hand[1].x && by_face.height > 13);

    // Turning a little changes the drawing a little: the same fitted
    // outline at every angle, so no shape is exchanged for another near 0.
    const glyphs::TextMatrix slight = glyphs::turn_matrix(4096);
    const TurnedMask upright_H = mask_of(face, glyph_H, 13 * 64, glyphs::Fit::light, nullptr, glyphs::Point{0, 0});
    const TurnedMask slight_H = mask_of(face, glyph_H, 13 * 64, glyphs::Fit::light, &slight, glyphs::Point{0, 0});
    long long ink_upright = 0;
    long long ink_slight = 0;
    for (const u8 pixel : upright_H.pixels) ink_upright = ink_upright + pixel;
    for (const u8 pixel : slight_H.pixels) ink_slight = ink_slight + pixel;
    const long long ink_change = ink_upright > ink_slight ? ink_upright - ink_slight : ink_slight - ink_upright;
    CHECK(upright_H.had && slight_H.had && ink_upright > 0 && ink_change * 100 < ink_upright);

    // The way back, for a point of the surface: a quarter turn's inverse
    // is the turn the other way, exactly; a turn's is its transpose to
    // within a 65536th; a matrix that flattens has none.
    glyphs::TextMatrix inverse{};
    CHECK(glyphs::invert_matrix(left, inverse) && inverse.xx == right.xx && inverse.xy == right.xy && inverse.yx == right.yx && inverse.yy == right.yy);
    CHECK(glyphs::invert_matrix(slant, inverse) && inverse.xx - slant.xx <= 1 && slant.xx - inverse.xx <= 1 && inverse.xy - slant.yx <= 1 &&
          slant.yx - inverse.xy <= 1);
    const glyphs::TextMatrix flat{0x10000, 0x20000, 0x8000, 0x10000};
    CHECK(!glyphs::invert_matrix(flat, inverse));
    // A place and back: (10, 4) pixels through 17 degrees and its inverse
    // is itself to within a 64th or two.
    const glyphs::Point place{10 * 64, 4 * 64};
    const glyphs::Point there = glyphs::turn_place(slant, place);
    const glyphs::Point again = glyphs::turn_place(glyphs::turn_matrix(-(static_cast<i64>(17) << 16)), there);
    CHECK(again.x - place.x <= 2 && place.x - again.x <= 2 && again.y - place.y <= 2 && place.y - again.y <= 2);
    // On the surface (y downward) a quarter turn counter-clockwise takes
    // what was to the right to above: (64, 0) goes to (0, -64).
    const glyphs::Point up = glyphs::turn_place(left, glyphs::Point{64, 0});
    CHECK(up.x == 0 && up.y == -64);
    // A box 100 by 20 from the origin, turned a quarter: 20 by 100, above
    // the origin.
    const glyphs::PlaceBox turned_box = glyphs::turn_box(left, glyphs::PlaceBox{0, 0, 100 * 64, 20 * 64});
    CHECK(turned_box.left == 0 && turned_box.right == 20 * 64 && turned_box.top == -100 * 64 && turned_box.bottom == 0);
}

void face_size() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    // The points of a glyph's segments (a square is a move and three
    // lines: 4; the closing line is not kept): read from the face here,
    // and the halves below are sized from them.
    glyphs::Face probe{};
    CHECK(probe.open(view_of(opened.bytes), 0).succeeded());
    glyphs::FittedGlyph fitted{};
    usize points_of[glyph_count] = {};
    for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
        CHECK(probe.fitted(glyph, 1024, glyphs::Fit::light, fitted).succeeded());
        points_of[glyph] = fitted.outline.points.count();
    }
    CHECK(points_of[glyph_one] == 4 && points_of[glyph_i] == 8 && points_of[glyph_n] == 8 && points_of[glyph_o] == 8 && points_of[glyph_H] == 12);
    // An outline takes 9 bytes a point and 96: i, n and o 168, 1 132, H
    // 204. A half holds a fifth of the limit: halves of 400 bytes, in
    // which two of i, n and o fit and not three.
    glyphs::FaceLimits small{};
    small.fitted_bytes = 2000;
    glyphs::Face face{};
    CHECK(face.open(view_of(opened.bytes), 0, small).succeeded());
    const i64 size = 1024;
    const glyphs::Fit fit = glyphs::Fit::light;
    CHECK(face.fitted(glyph_i, size, fit, fitted).succeeded() && face.fitted(glyph_n, size, fit, fitted).succeeded() && face.glyphs_fitted() == 2);
    // o does not fit beside them: a new half, with i and n in the older.
    CHECK(face.fitted(glyph_o, size, fit, fitted).succeeded() && face.glyphs_fitted() == 3);
    // n is moved to the newer half, not fitted again: o and n fill it.
    CHECK(face.fitted(glyph_n, size, fit, fitted).succeeded() && face.glyphs_fitted() == 3);
    // 1 (132 bytes) does not fit: the older half, with i, is given up, and
    // o and n are the older half now.
    CHECK(face.fitted(glyph_one, size, fit, fitted).succeeded() && face.glyphs_fitted() == 4);
    // n, in use all along, is held; i is fitted again; the same glyph at
    // another size, or in another fit, is another outline.
    CHECK(face.fitted(glyph_n, size, fit, fitted).succeeded() && face.glyphs_fitted() == 4);
    CHECK(face.fitted(glyph_i, size, fit, fitted).succeeded() && face.glyphs_fitted() == 5);
    CHECK(face.fitted(glyph_i, size + 64, fit, fitted).succeeded() && face.glyphs_fitted() == 6);
    CHECK(face.fitted(glyph_i, size + 64, glyphs::Fit::none, fitted).succeeded() && face.glyphs_fitted() == 7);
    // Through all of that each glyph was read from the font once.
    CHECK((*face.typeface()).glyphs_read() == 4);
    // H (204 bytes) is too large for a half of 100: served, fitted each
    // time.
    glyphs::FaceLimits tiny{};
    tiny.fitted_bytes = 500;
    glyphs::Face narrow{};
    CHECK(narrow.open(view_of(opened.bytes), 0, tiny).succeeded());
    CHECK(narrow.fitted(glyph_H, size, fit, fitted).succeeded() && fitted.outline.points.count() == 12 &&
          narrow.fitted(glyph_H, size, fit, fitted).succeeded() && narrow.glyphs_fitted() == 2 && (*narrow.typeface()).glyphs_read() == 1);
    // Warming: the characters' glyphs are read, and fitted if a size is
    // given; the font has no z.
    const u32 likely[] = {'H', 'O', 'n', 'z', 'o'};
    glyphs::Face warm{};
    usize warmed = 0;
    CHECK(warm.open(view_of(opened.bytes), 0).succeeded());
    const glyphs::Typeface& shared = *warm.typeface();
    CHECK(warm.warm(stxlib::View<u32>(likely), 0, fit, warmed).succeeded() && warmed == 4 && shared.glyphs_read() == 4 && warm.glyphs_fitted() == 0);
    CHECK(warm.warm(stxlib::View<u32>(likely), size, fit, warmed).succeeded() && warmed == 4 && shared.glyphs_read() == 4 && warm.glyphs_fitted() == 4);
    // A text drawn again reads nothing, fits nothing and holds no more.
    const glyphs::RunGlyph text[] = {{glyph_H, 0, 0}, {glyph_O, 520, 0}, {glyph_n, 1040, 0}, {glyph_o, 1500, 0}, {glyph_n, 1960, 0}};
    glyphs::DrawStyle style{};
    style.size = size;
    glyphs::RunCoverage coverage{};
    CHECK(warm.draw_run(stxlib::View<glyphs::RunGlyph>(text), style, coverage).succeeded());
    const usize read = shared.glyphs_read();
    const usize fittings = warm.glyphs_fitted();
    const usize bytes = warm.bytes_held();
    const usize shared_bytes = shared.bytes_held();
    CHECK(read == 4 && fittings == 4 && bytes > 0 && shared_bytes > 0);
    for (int again = 0; again < 20; ++again) CHECK(warm.draw_run(stxlib::View<glyphs::RunGlyph>(text), style, coverage).succeeded());
    CHECK(shared.glyphs_read() == read && warm.glyphs_fitted() == fittings && warm.bytes_held() == bytes && shared.bytes_held() == shared_bytes);
    // The most held is at least what is held; reset, it is what is held.
    CHECK(warm.bytes_peak() >= bytes && shared.bytes_peak() >= shared_bytes);
    warm.reset_peak();
    CHECK(warm.bytes_peak() == warm.bytes_held());
    // The fitted outlines' storage stays under its limit however much is
    // fitted: a face of 8,192 bytes, every glyph at 120 sizes.
    glyphs::FaceLimits bounded{};
    bounded.fitted_bytes = 8192;
    glyphs::Face busy{};
    CHECK(busy.open(view_of(opened.bytes), 0, bounded).succeeded());
    usize after_first = 0;
    for (i64 step = 0; step < 120; ++step) {
        for (u32 glyph = 0; glyph < glyph_count; ++glyph) CHECK(busy.fitted(glyph, 600 + step * 13, fit, fitted).succeeded());
        if (step == 30) after_first = busy.bytes_held();
    }
    CHECK(busy.glyphs_fitted() == 120 * glyph_count && after_first > 0 && busy.bytes_held() <= after_first + 8192);
    // Every face here, after everything done to it, agrees with itself.
    CHECK(busy.whole() && warm.whole() && face.whole() && narrow.whole() && probe.whole() && shared.whole());
    glyphs::Face unopened{};
    CHECK(unopened.whole());
}

// Thicker strokes, by hand. 1 is an upright bar: at 16 pixels, unfitted,
// from 215 to 297 across (26.6) and 0 to 717 up. Thickened by a pixel (64)
// its left and bottom stay and it grows a pixel right and up: 215 to 361,
// 0 to 781.
void face_thicker() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    glyphs::Face face{};
    CHECK(face.open(view_of(opened.bytes), 0).succeeded());
    glyphs::DrawStyle style{};
    style.size = 16 * 64;
    style.fit = glyphs::Fit::none;
    glyphs::Bounds plain{};
    glyphs::Bounds thick{};
    stxlib::Vector<u8> plain_pixels{};
    stxlib::Vector<u8> thick_pixels{};
    CHECK(face.mask(glyph_one, style, glyphs::Point{0, 0}, plain, plain_pixels).succeeded());
    style.thicken = 64;
    CHECK(face.mask(glyph_one, style, glyphs::Point{0, 0}, thick, thick_pixels).succeeded());
    CHECK(plain.left == 3 && plain.width == 2 && plain.top == 12 && plain.height == 12);
    if (!(thick.left == 3 && thick.width == 3 && thick.top == 13 && thick.height == 13)) {
        std::fprintf(stderr, "  thick 1: left %d width %u top %d height %u\n", thick.left, thick.width, thick.top, thick.height);
    }
    CHECK(thick.left == 3 && thick.width == 3 && thick.top == 13 && thick.height == 13);
    // Its ink is its area: 146 by 781 sixty-fourths, 27.84 pixels.
    unsigned long long ink = 0;
    for (usize index = 0; index < thick_pixels.count(); ++index) ink = ink + thick_pixels[index];
    if (!(ink > 27 * 255 + 138 && ink < 28 * 255 + 80)) std::fprintf(stderr, "  thick 1: ink %llu\n", ink);
    CHECK(ink > 27 * 255 + 138 && ink < 28 * 255 + 80);
    // o at 20 pixels is 64 to 512 across and 0 to 640 up with a counter
    // from 166 to 410: thickened by a pixel it is a pixel wider and
    // taller, and its counter, a pixel narrower from the left, is still
    // clear at its middle.
    style.size = 20 * 64;
    style.thicken = 0;
    CHECK(face.mask(glyph_o, style, glyphs::Point{0, 0}, plain, plain_pixels).succeeded());
    style.thicken = 64;
    CHECK(face.mask(glyph_o, style, glyphs::Point{0, 0}, thick, thick_pixels).succeeded());
    CHECK(plain.left == 1 && plain.width == 7 && plain.height == 10);
    CHECK(thick.left == 1 && thick.width == 8 && thick.height == 11 && thick.top == 11);
    // The pixel at column 5 (x from 5 to 6), six rows down: in the counter.
    const usize middle = static_cast<usize>(6) * thick.width + 4;
    CHECK(thick_pixels.count() > middle && thick_pixels[middle] == 0);
    // The stem to its left (x from 1 to 3.59) is solid ink there.
    CHECK(thick_pixels.count() > middle && thick_pixels[static_cast<usize>(6) * thick.width + 1] == 255);
    // No thickening is no change; too much, or less than none, is refused.
    style.thicken = 0;
    glyphs::Bounds again{};
    CHECK(face.mask(glyph_o, style, glyphs::Point{0, 0}, again, thick_pixels).succeeded() && same_bounds(again, plain) &&
          thick_pixels.count() == plain_pixels.count());
    style.thicken = 1025;
    const stxlib::Status much = face.mask(glyph_o, style, glyphs::Point{0, 0}, again, thick_pixels);
    style.thicken = -1;
    const stxlib::Status less = face.mask(glyph_o, style, glyphs::Point{0, 0}, again, thick_pixels);
    CHECK(much.failed() && (*much.error()).kind() == stxlib::ErrorKind::invalid_argument && less.failed());
    // In a run, with hinting, at parts of a pixel: each glyph's thickened
    // mask put together by the rule, and never less ink than without.
    const std::vector<glyphs::RunGlyph> line = {{glyph_H, 3, 0}, {glyph_o, 600 + 13, 0}, {glyph_n_tilde, 1100, 20}, {glyph_O, 1700, 0}, {glyph_p, 2300, 7}};
    const glyphs::Fit fits[] = {glyphs::Fit::none, glyphs::Fit::light, glyphs::Fit::monochrome};
    for (const glyphs::Fit fit : fits) {
        for (i32 thicken = 16; thicken <= 96; thicken = thicken + 40) {
            style = glyphs::DrawStyle{};
            style.size = 18 * 64 + 5;
            style.fit = fit;
            glyphs::RunCoverage thin{};
            glyphs::RunCoverage bold{};
            const stxlib::View<glyphs::RunGlyph> view(line.data(), line.size());
            CHECK(face.draw_run(view, style, thin).succeeded());
            style.thicken = thicken;
            OwnRun own{};
            CHECK(own_run(face, line, style, own) && face.draw_run(view, style, bold).succeeded() && run_is(bold, own));
            unsigned long long thin_ink = 0;
            unsigned long long bold_ink = 0;
            for (usize index = 0; index < thin.pixels.count(); ++index) thin_ink = thin_ink + thin.pixels[index];
            for (usize index = 0; index < bold.pixels.count(); ++index) bold_ink = bold_ink + bold.pixels[index];
            CHECK(bold_ink > thin_ink && bold.width >= thin.width && bold.height >= thin.height);
        }
    }
}

// A typeface for the program: faces on several threads draw from one, each
// glyph read from the font once for all of them.
struct ThreadWork final {
    stxlib::Shared<glyphs::Typeface> typeface{};
    u32 number{0};
    unsigned long long sum{0};
    bool good{false};
};

[[nodiscard]] unsigned long long draw_all(glyphs::Face& face, const u32 number, bool& good) {
    unsigned long long sum = 0;
    glyphs::RunCoverage coverage{};
    good = true;
    for (u32 round = 0; round < 40; ++round) {
        glyphs::RunGlyph run[glyph_count] = {};
        for (u32 glyph = 0; glyph < glyph_count; ++glyph) run[glyph] = glyphs::RunGlyph{(glyph + round) % glyph_count, static_cast<i32>(glyph * 700 + round), 0};
        glyphs::DrawStyle style{};
        // Every thread goes through the same sizes, in an order of its own.
        style.size = 600 + static_cast<i64>((round * 7 + number * 11) % 40) * 29;
        style.fit = (round + number) % 3 == 0 ? glyphs::Fit::monochrome : glyphs::Fit::light;
        good = good && face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, coverage).succeeded();
        unsigned long long ink = 0;
        for (usize index = 0; index < coverage.pixels.count(); ++index) ink = ink + coverage.pixels[index];
        sum = sum + ink * static_cast<unsigned long long>(style.size) + coverage.width;
    }
    return sum;
}

void draw_on_thread(const stxlib::StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    ThreadWork& work = *static_cast<ThreadWork*>(context);
    glyphs::Face face{};
    if (face.open(work.typeface).failed()) return;
    bool good = false;
    work.sum = draw_all(face, work.number, good);
    work.good = good;
}

void shared_typeface() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    stxlib::Result<stxlib::Shared<glyphs::Typeface>> made = glyphs::open_typeface(view_of(opened.bytes), 0);
    CHECK(made.succeeded());
    if (made.failed()) return;
    const stxlib::Shared<glyphs::Typeface> typeface = *made.value();
    CHECK((*typeface).font() != nullptr && (*typeface).glyphs_read() == 0);
    // Two faces of it: what one has read, the other finds there.
    glyphs::Face first{};
    glyphs::Face second{};
    CHECK(first.open(typeface).succeeded() && second.open(typeface).succeeded());
    glyphs::FittedGlyph fitted{};
    CHECK(first.fitted(glyph_H, 1024, glyphs::Fit::light, fitted).succeeded() && (*typeface).glyphs_read() == 1);
    const i32 advance = fitted.advance;
    const usize points = fitted.outline.points.count();
    CHECK(second.fitted(glyph_H, 1024, glyphs::Fit::light, fitted).succeeded() && (*typeface).glyphs_read() == 1);
    CHECK(fitted.advance == advance && fitted.outline.points.count() == points && first.glyphs_fitted() == 1 && second.glyphs_fitted() == 1);
    // A face holds its typeface: the caller's own hold can go.
    glyphs::Face lasting{};
    {
        stxlib::Result<stxlib::Shared<glyphs::Typeface>> brief = glyphs::open_typeface(view_of(opened.bytes), 0);
        CHECK(brief.succeeded() && lasting.open(*brief.value()).succeeded());
    }
    CHECK(lasting.fitted(glyph_p, 1024, glyphs::Fit::light, fitted).succeeded() && fitted.outline.points.count() > 0);
    // An empty typeface is no typeface.
    glyphs::Face none{};
    CHECK(none.open(stxlib::Shared<glyphs::Typeface>()).failed());
    // What one thread alone draws.
    unsigned long long alone[4] = {};
    for (u32 number = 0; number < 4; ++number) {
        glyphs::Face face{};
        bool good = false;
        CHECK(face.open(view_of(opened.bytes), 0).succeeded());
        alone[number] = draw_all(face, number, good);
        CHECK(good && alone[number] != 0);
    }
    // Four threads at once, each with a face of the one typeface, draw the
    // same: every glyph was read once for all four.
    stxlib::Result<stxlib::Shared<glyphs::Typeface>> common = glyphs::open_typeface(view_of(opened.bytes), 0);
    CHECK(common.succeeded());
    if (common.failed()) return;
    ThreadWork work[4] = {};
    stxlib::Thread threads[4] = {};
    bool started = true;
    for (u32 number = 0; number < 4; ++number) {
        work[number].typeface = *common.value();
        work[number].number = number;
        stxlib::Result<stxlib::Thread> thread = stxlib::Thread::start(draw_on_thread, &work[number]);
        if (thread.failed()) {
            started = false;
            break;
        }
        threads[number] = static_cast<stxlib::Result<stxlib::Thread>&&>(thread).take_value();
    }
    for (u32 number = 0; number < 4; ++number) static_cast<void>(threads[number].join());
    CHECK(started);
    for (u32 number = 0; number < 4; ++number) CHECK(started && work[number].good && work[number].sum == alone[number]);
    CHECK((*(*common.value())).glyphs_read() == glyph_count);
}

// What Pocket Library's FontSet gave, from the one interface: the font's
// numbers, whether it has a feature, a glyph's box, and a glyph's mask
// moved and thickened.
void font_numbers() {
    Opened opened{};
    open_letters(opened);
    if (!opened.ready) return;
    // The font of the letters says nothing of small letters' height and
    // has no substitutions.
    CHECK(opened.font.x_height == 0 && opened.font.cap_height == 0 && !glyphs::has_feature(opened.font, 0x736D6370U));
    // With an OS/2 of version 2 that says so, and a table of substitutions
    // that lists 'liga' and 'smcp'.
    Bytes os2 = os2_table(2, 0, 750, -200, 50, 950, 250);
    os2.resize(96, 0);
    os2[86] = 0x01;
    os2[87] = 0xF4;
    os2[88] = 0x02;
    os2[89] = 0xBC;
    Bytes gsub{};
    font_builder::put32(gsub, 0x00010000U);
    font_builder::put16(gsub, 0);
    font_builder::put16(gsub, 10);
    font_builder::put16(gsub, 0);
    font_builder::put16(gsub, 2);
    font_builder::put32(gsub, 0x6C696761U);
    font_builder::put16(gsub, 0);
    font_builder::put32(gsub, 0x736D6370U);
    font_builder::put16(gsub, 0);
    Bytes glyf{};
    Bytes loca{};
    Bytes hmtx{};
    const std::vector<Bytes> glyph_bytes = {Bytes{}, simple_glyph({{{0, 0, true}, {0, 100, true}, {100, 100, true}, {100, 0, true}}})};
    const std::vector<Metric> metrics = {{500, 0}, {500, 0}};
    glyph_tables(glyph_bytes, metrics, 2, true, glyf, loca, hmtx);
    const Bytes bytes = font_of(0x00010000U, {{"GSUB", gsub},
                                              {"OS/2", os2},
                                              {"cmap", cmap_table({{3, 1, map_format_4({{'A', 'A', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}})}})},
                                              {"glyf", glyf},
                                              {"head", head_table(1000, true)},
                                              {"hhea", hhea_table(800, -220, 50, 2)},
                                              {"hmtx", hmtx},
                                              {"loca", loca},
                                              {"maxp", maxp_table(2)}});
    glyphs::Font font{};
    CHECK(glyphs::open_font(view_of(bytes), 0, font).succeeded());
    CHECK(font.x_height == 500 && font.cap_height == 700);
    CHECK(glyphs::has_feature(font, 0x6C696761U) && glyphs::has_feature(font, 0x736D6370U) && !glyphs::has_feature(font, 0x6F6E756DU));
    // A table of substitutions cut short lists nothing, and nothing is
    // read past it.
    for (usize length = 0; length < gsub.size(); ++length) {
        const Bytes cut(gsub.begin(), gsub.begin() + static_cast<std::ptrdiff_t>(length));
        const Bytes cut_font = font_of(0x00010000U, {{"GSUB", cut},
                                                     {"cmap", cmap_table({{3, 1, map_format_4({{'A', 'A', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}})}})},
                                                     {"glyf", glyf},
                                                     {"head", head_table(1000, true)},
                                                     {"hhea", hhea_table(800, -220, 50, 2)},
                                                     {"hmtx", hmtx},
                                                     {"loca", loca},
                                                     {"maxp", maxp_table(2)}});
        glyphs::Font other{};
        CHECK(glyphs::open_font(view_of(cut_font), 0, other).succeeded() && !glyphs::has_feature(other, 0x736D6370U));
    }
    // A glyph put together from another: the square, and the square again
    // 33 across and 17 up. Its parts and their offsets are scaled each on
    // their own, as TrueType has it: at 13 pixels and a bit the unfitted
    // outline and the ink box are load_scaled_glyph's, point for point.
    {
        const std::vector<Bytes> parts = {Bytes{}, simple_glyph({{{0, 0, true}, {0, 101, true}, {101, 101, true}, {101, 0, true}}}),
                                          font_builder::composite_glyph(0, {{0x0003, 1, 0, 0, {}}, {0x0003, 1, 33, 17, {}}})};
        const std::vector<Metric> part_metrics = {{500, 0}, {500, 0}, {500, 0}};
        Bytes part_glyf{};
        Bytes part_loca{};
        Bytes part_hmtx{};
        glyph_tables(parts, part_metrics, 3, true, part_glyf, part_loca, part_hmtx);
        const Bytes composite_font = font_of(0x00010000U, {{"cmap", cmap_table({{3, 1, map_format_4({{'A', 'B', 1 - 'A', {}}, {0xFFFF, 0xFFFF, 1, {}}})}})},
                                                           {"glyf", part_glyf},
                                                           {"head", head_table(1000, true)},
                                                           {"hhea", hhea_table(800, -220, 50, 3)},
                                                           {"hmtx", part_hmtx},
                                                           {"loca", part_loca},
                                                           {"maxp", maxp_table(3)}});
        stxlib::Result<stxlib::Shared<glyphs::Typeface>> made = glyphs::open_typeface(view_of(composite_font), 0);
        CHECK(made.succeeded());
        if (made.succeeded()) {
            glyphs::Typeface& typeface = *(*made.value());
            const glyphs::Font& parts_font = *typeface.font();
            bool all = true;
            bool some_differ_from_whole = false;
            for (i64 size = 500; size < 1500; size = size + 7) {
                const i64 scale = glyphs::scale_of(parts_font, size);
                glyphs::GlyphOutline expected{};
                glyphs::GlyphOutline whole{};
                glyphs::GlyphOutline got{};
                i32 advance = 0;
                glyphs::InkBox box{};
                const glyphs::FontLimits limits{};
                all = all && glyphs::load_scaled_glyph(parts_font, 2, scale, scale, limits, expected).succeeded() &&
                      glyphs::load_glyph(parts_font, 2, limits, whole).succeeded() &&
                      typeface.fit(2, size, glyphs::Fit::none, got, advance).succeeded() && typeface.ink_box(2, size, box).succeeded() &&
                      same_outline(expected, got) && expected.points.count() == 8;
                if (!all) break;
                glyphs::scale_outline(whole, scale, scale);
                some_differ_from_whole = some_differ_from_whole || !same_outline(whole, expected);
                i32 left = expected.points[0].x;
                i32 right = left;
                i32 bottom = expected.points[0].y;
                i32 top = bottom;
                for (usize index = 0; index < expected.points.count(); ++index) {
                    left = expected.points[index].x < left ? expected.points[index].x : left;
                    right = expected.points[index].x > right ? expected.points[index].x : right;
                    bottom = expected.points[index].y < bottom ? expected.points[index].y : bottom;
                    top = expected.points[index].y > top ? expected.points[index].y : top;
                }
                all = all && box.left == left && box.right == right && box.bottom == bottom && box.top == top;
            }
            CHECK(all);
            // The test means something: scaling the whole glyph at once
            // is not the same at some of these sizes.
            CHECK(some_differ_from_whole);
            glyphs::InkBox box{};
            CHECK(typeface.ink_box(0, 1024, box).succeeded() && box.left == 0 && box.right == 0 && box.top == 0 && box.bottom == 0);
            CHECK(typeface.ink_box(3, 1024, box).failed() && typeface.ink_box(1, 0, box).failed());
        }
    }
    // A glyph's box at a size, unfitted: the square at 16 pixels is 0 to
    // 102 (26.6) each way.
    glyphs::Face face{};
    glyphs::FittedGlyph fitted{};
    CHECK(face.open(view_of(bytes), 0).succeeded() && face.fitted(1, 1024, glyphs::Fit::none, fitted).succeeded());
    CHECK(fitted.left == 0 && fitted.bottom == 0 && fitted.right == 102 && fitted.top == 102 && fitted.advance == 512);
    glyphs::InkBox square{};
    CHECK((*face.typeface()).ink_box(1, 1024, square).succeeded() && square.left == 0 && square.bottom == 0 && square.right == 102 && square.top == 102);
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
    glyphs::GlyphStore store{};
    const bool store_open = store.open(font, limits).succeeded();
    const u32 count = font.glyph_count < 16 ? font.glyph_count : 16;
    for (u32 glyph = 0; glyph < count; ++glyph) {
        i32 advance = 0;
        if (hinter.hint_glyph(font, glyph, 11 * 64, glyphs::HintMode::light, limits, outline, advance).succeeded()) hinted = hinted + 1;
        if (hinter.hint_glyph(font, glyph, 23 * 64 + 5, glyphs::HintMode::monochrome, limits, outline, advance).succeeded()) hinted = hinted + 1;
        glyphs::StoredGlyph stored{};
        if (store_open && store.glyph(font, glyph, stored).succeeded()) {
            if (hinter.hint_stored(font, glyph, stored, 17 * 64, glyphs::HintMode::monochrome, limits, outline, advance).succeeded()) hinted = hinted + 1;
        }
    }
    // The same font through a face: a run of its first glyphs, three ways.
    glyphs::Face face{};
    if (face.open(view_of(bytes), 0).failed()) return;
    glyphs::RunGlyph run[16] = {};
    for (u32 glyph = 0; glyph < 16; ++glyph) run[glyph] = glyphs::RunGlyph{glyph, static_cast<i32>(glyph * 700 + glyph), static_cast<i32>(glyph * 3)};
    glyphs::RunCoverage coverage{};
    glyphs::DrawStyle style{};
    style.size = 14 * 64 + 9;
    static_cast<void>(face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, coverage));
    style.fit = glyphs::Fit::monochrome;
    static_cast<void>(face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, coverage));
    style.fit = glyphs::Fit::none;
    style.transformed = true;
    style.xy = 0x4000;
    style.thicken = 37;
    static_cast<void>(face.draw_run(stxlib::View<glyphs::RunGlyph>(run), style, coverage));
    static_cast<void>(glyphs::has_feature(font, 0x6C696761U));
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
    accents();
    stored();
    store_size();
    face_basics();
    face_masks();
    face_runs();
    face_windows();
    face_turned();
    face_turned_fitted();
    face_size();
    face_thicker();
    shared_typeface();
    font_numbers();
    repeated();
    without_blue_zones();
    robustness();
    // The checked program (CMakeLists.txt, stxlib_glyphs_checked) is this
    // test on glyph code that checks every structure whole at every exit.
    std::printf("glyph checks: %s\n", glyphs::glyph_checks_enabled() ? "on" : "off");
#if defined(STXLIB_GLYPHS_CHECKS_EXPECTED)
    CHECK(glyphs::glyph_checks_enabled());
#endif
    const int status = stxlib_tests::finish("stxlib_glyph_hinter_tests");
    return status;
}
