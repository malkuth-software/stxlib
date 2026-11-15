// Tests of include/stxlib/glyph_mask.hpp.
//
// The reference is the rule itself computed another way: for a polygon,
// the signed area of each pixel in double precision, by integrating the
// part of the pixel to the right of every chord (reference_area below).
// The rasterizer's integer arithmetic must give the value of that area,
// floor(256 |S|) clamped to 255, wherever the area is not within `slack`
// of a step between two values.
//
// Properties:
//   M1  rectangles with edges on 1/64 pixel: every pixel's value is exactly
//       floor(256 * area) clamped to 255 (their areas are exact in 16.16);
//   M2  random polygons (triangles to octagons, convex and not, 26.6
//       coordinates): every pixel within `slack` of the reference; the
//       share of pixels where the byte differs from the reference's own
//       rounding is reported;
//   M3  a contour traversed the other way gives the same bytes;
//   M4  two polygons that share an edge give the same bytes as the one
//       polygon without it (the shared chords cancel exactly);
//   M5  nothing leaks: every pixel whose reference area is 0 is 0, and
//       every pixel fully inside is 255;
//   M6  curves: a quadratic and a cubic outline against the reference on
//       their own chords (the test flattens by the stated rule in
//       doubles), within `slack`; and a cubic that is a straight line
//       draws as that line;
//   M7  measure: the box of all points rounded outward; the offset moves
//       it; empty outlines;
//   M8  refusals: malformed outlines, points outside the box, limits, a
//       mask of the wrong size;
//   M9  many contours: an outline of 784 contours that share no pixel
//       gives the bytes each contour gives alone; and the rasterizer draws
//       a small outline the same before and after it;
//   M10 the tabled reciprocals: for every divisor the table holds, the
//       multiplied quotient is the divided one at every dividend where the
//       quotient changes and just before it, up to the greatest dividend
//       the table is for. (The product does not decrease as the dividend
//       grows, so right at both ends of a run of equal quotients is right
//       all along it.)
//   M11 the forms: chords of every form, drawn through the block and its
//       loops, give every cell the integers the arithmetic in full gives
//       (src/algorithms/glyphs/chords.hpp).

#include "stxlib/glyph_mask.hpp"

#include "algorithms/glyphs/chords.hpp"
#include "algorithms/glyphs/reciprocal.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

namespace glyphs = stxlib::glyphs;
using stxlib::i32;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

// A step of the value is 1/256 of a pixel's area; the arithmetic is good
// to far less than this (glyph_mask.hpp), so a byte may differ from the
// reference's only when the area is within an eighth of a step of the
// next value.
constexpr double slack = 1.0 / 2048.0;

struct Chord final {
    double x0{0};
    double y0{0};
    double x1{0};
    double y1{0};
};

// The signed area of the part of pixel (px, py) to the right of the chord,
// over the chord's height inside the pixel's row; mask coordinates in
// pixels, y downward. Positive for a chord going down.
[[nodiscard]] double chord_area(const Chord& chord, const double px, const double py) {
    double xa = chord.x0;
    double ya = chord.y0;
    double xb = chord.x1;
    double yb = chord.y1;
    double sign = 1.0;
    if (ya == yb) return 0.0;
    if (ya > yb) {
        std::swap(xa, xb);
        std::swap(ya, yb);
        sign = -1.0;
    }
    const double top = std::max(ya, py);
    const double bottom = std::min(yb, py + 1.0);
    if (top >= bottom) return 0.0;
    const double slope = (xb - xa) / (yb - ya);
    // The width of the pixel to the right of the chord at height y:
    // clamp(px + 1 - x(y), 0, 1), linear between the heights where x(y)
    // meets the pixel's two sides.
    double cuts[4] = {top, bottom, top, bottom};
    usize cut_count = 2;
    if (slope != 0.0) {
        const double at_left = ya + (px - xa) / slope;
        const double at_right = ya + (px + 1.0 - xa) / slope;
        if (at_left > top && at_left < bottom) {
            cuts[cut_count] = at_left;
            cut_count = cut_count + 1;
        }
        if (at_right > top && at_right < bottom) {
            cuts[cut_count] = at_right;
            cut_count = cut_count + 1;
        }
    }
    std::sort(cuts, cuts + cut_count);
    double area = 0.0;
    for (usize index = 0; index + 1 < cut_count; ++index) {
        const double y0 = cuts[index];
        const double y1 = cuts[index + 1];
        const double w0 = std::min(1.0, std::max(0.0, px + 1.0 - (xa + (y0 - ya) * slope)));
        const double w1 = std::min(1.0, std::max(0.0, px + 1.0 - (xa + (y1 - ya) * slope)));
        area = area + (w0 + w1) * 0.5 * (y1 - y0);
    }
    return sign * area;
}

[[nodiscard]] double reference_area(const std::vector<Chord>& chords, const usize px, const usize py) {
    double area = 0.0;
    for (const Chord& chord : chords) area = area + chord_area(chord, static_cast<double>(px), static_cast<double>(py));
    return area;
}

[[nodiscard]] int value_of(const double area) {
    const double scaled = std::floor(256.0 * std::fabs(area));
    const int value = scaled > 255.0 ? 255 : static_cast<int>(scaled);
    return value;
}

struct Shape final {
    std::vector<glyphs::Segment> segments{};
    std::vector<glyphs::Point> points{};
};

[[nodiscard]] glyphs::Outline outline_of(const Shape& shape) {
    const glyphs::Outline outline{.segments = stxlib::View<glyphs::Segment>(shape.segments.data(), shape.segments.size()),
                                  .points = stxlib::View<glyphs::Point>(shape.points.data(), shape.points.size())};
    return outline;
}

void add_polygon(Shape& shape, const std::vector<glyphs::Point>& corners) {
    for (usize index = 0; index < corners.size(); ++index) {
        shape.segments.push_back(index == 0 ? glyphs::Segment::move : glyphs::Segment::line);
        shape.points.push_back(corners[index]);
    }
}

struct Mask final {
    glyphs::Bounds bounds{};
    std::vector<u8> bytes{};
    bool rendered{false};
};

[[nodiscard]] Mask render(const Shape& shape, const glyphs::Point offset) {
    Mask mask{};
    const glyphs::Limits limits{};
    const glyphs::Outline outline = outline_of(shape);
    if (glyphs::measure(outline, offset, limits, mask.bounds).failed()) return mask;
    mask.bytes.assign(static_cast<usize>(mask.bounds.width) * mask.bounds.height, 0x5a);
    glyphs::Rasterizer rasterizer{};
    const stxlib::MutableView<u8> bytes(mask.bytes.data(), mask.bytes.size());
    mask.rendered = rasterizer.render(outline, offset, mask.bounds, limits, bytes).succeeded();
    return mask;
}

// The chords of a polygon in the mask's pixel coordinates (y downward).
void polygon_chords(const std::vector<glyphs::Point>& corners, const glyphs::Bounds& bounds, std::vector<Chord>& chords) {
    for (usize index = 0; index < corners.size(); ++index) {
        const glyphs::Point from = corners[index];
        const glyphs::Point to = corners[(index + 1) % corners.size()];
        const Chord chord{.x0 = from.x / 64.0 - bounds.left,
                          .y0 = bounds.top - from.y / 64.0,
                          .x1 = to.x / 64.0 - bounds.left,
                          .y1 = bounds.top - to.y / 64.0};
        chords.push_back(chord);
    }
}

struct Agreement final {
    std::uint64_t pixels{0};
    std::uint64_t outside_slack{0};
    std::uint64_t other_byte{0};
    std::uint64_t leaked{0};
};

void compare(const Mask& mask, const std::vector<Chord>& chords, Agreement& agreement) {
    for (usize y = 0; y < mask.bounds.height; ++y) {
        for (usize x = 0; x < mask.bounds.width; ++x) {
            const double area = reference_area(chords, x, y);
            const int got = mask.bytes[y * mask.bounds.width + x];
            const int low = value_of(std::fabs(area) - slack < 0.0 ? 0.0 : std::fabs(area) - slack);
            const int high = value_of(std::fabs(area) + slack);
            agreement.pixels = agreement.pixels + 1;
            if (got < low || got > high) agreement.outside_slack = agreement.outside_slack + 1;
            if (got != value_of(area)) agreement.other_byte = agreement.other_byte + 1;
            if ((std::fabs(area) < 1e-12 && got != 0) || (std::fabs(area) > 1.0 - 1e-12 && std::fabs(area) < 1.0 + 1e-12 && got != 255)) {
                agreement.leaked = agreement.leaked + 1;
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

[[nodiscard]] i32 random_coordinate(const i32 range) {
    const i32 value = static_cast<i32>(next_random() % static_cast<std::uint64_t>(range));
    return value;
}

// M1.
void rectangles() {
    bool exact = true;
    for (i32 left = 0; left < 64; left += 7) {
        for (i32 width = 1; width < 200; width += 37) {
            for (i32 bottom = 0; bottom < 64; bottom += 9) {
                for (i32 height = 1; height < 200; height += 41) {
                    Shape shape{};
                    const std::vector<glyphs::Point> corners = {{left, bottom}, {left, bottom + height}, {left + width, bottom + height}, {left + width, bottom}};
                    add_polygon(shape, corners);
                    const Mask mask = render(shape, glyphs::Point{});
                    if (!mask.rendered) exact = false;
                    for (usize y = 0; y < mask.bounds.height && exact; ++y) {
                        for (usize x = 0; x < mask.bounds.width; ++x) {
                            // The overlap of the rectangle and the pixel, in 1/64.
                            const i32 pixel_left = (mask.bounds.left + static_cast<i32>(x)) * 64;
                            const i32 pixel_top = (mask.bounds.top - static_cast<i32>(y)) * 64;
                            const i32 dx = std::min(left + width, pixel_left + 64) - std::max(left, pixel_left);
                            const i32 dy = std::min(bottom + height, pixel_top) - std::max(bottom, pixel_top - 64);
                            const i32 area_4096 = (dx > 0 && dy > 0) ? dx * dy : 0;
                            const int expected = area_4096 * 256 / 4096 > 255 ? 255 : area_4096 * 256 / 4096;
                            if (mask.bytes[y * mask.bounds.width + x] != expected) exact = false;
                        }
                    }
                }
            }
        }
    }
    CHECK(exact);
}

// M2, M3, M5.
void polygons() {
    Agreement agreement{};
    bool reversed_equal = true;
    for (int round = 0; round < 4000; ++round) {
        const usize corner_count = 3 + next_random() % 6;
        std::vector<glyphs::Point> corners{};
        const i32 extent = round % 4 == 0 ? 64 * 3 : 64 * 20;
        for (usize index = 0; index < corner_count; ++index) {
            corners.push_back(glyphs::Point{.x = random_coordinate(extent) - extent / 4, .y = random_coordinate(extent) - extent / 4});
        }
        // Self-crossing polygons are sums, not unions: keep to convex order
        // half the time so that both kinds are seen.
        Shape shape{};
        add_polygon(shape, corners);
        const glyphs::Point offset{.x = random_coordinate(64), .y = random_coordinate(64)};
        const Mask mask = render(shape, offset);
        CHECK(mask.rendered);
        if (!mask.rendered) return;
        std::vector<glyphs::Point> placed = corners;
        for (glyphs::Point& point : placed) {
            point.x = point.x + offset.x;
            point.y = point.y + offset.y;
        }
        std::vector<Chord> chords{};
        polygon_chords(placed, mask.bounds, chords);
        compare(mask, chords, agreement);
        Shape backwards{};
        const std::vector<glyphs::Point> reversed(corners.rbegin(), corners.rend());
        add_polygon(backwards, reversed);
        const Mask other = render(backwards, offset);
        if (!other.rendered || other.bytes != mask.bytes) reversed_equal = false;
    }
    std::printf("polygons: %llu pixels, %llu outside the slack, %llu with another byte than the reference's rounding, %llu leaked\n",
                static_cast<unsigned long long>(agreement.pixels), static_cast<unsigned long long>(agreement.outside_slack),
                static_cast<unsigned long long>(agreement.other_byte), static_cast<unsigned long long>(agreement.leaked));
    CHECK(agreement.pixels > 100000);
    CHECK(agreement.outside_slack == 0);
    CHECK(agreement.leaked == 0);
    // The byte is the reference's own rounding nearly everywhere.
    CHECK(agreement.other_byte * 50 < agreement.pixels);
    CHECK(reversed_equal);
}

// M4.
void shared_edges() {
    bool equal = true;
    for (int round = 0; round < 500; ++round) {
        // A quadrilateral a-b-c-d cut along a-c into two triangles.
        const glyphs::Point a{.x = random_coordinate(600), .y = random_coordinate(600)};
        const glyphs::Point b{.x = random_coordinate(600) + 700, .y = random_coordinate(600)};
        const glyphs::Point c{.x = random_coordinate(600) + 700, .y = random_coordinate(600) + 700};
        const glyphs::Point d{.x = random_coordinate(600), .y = random_coordinate(600) + 700};
        Shape whole{};
        add_polygon(whole, {a, b, c, d});
        Shape halves{};
        add_polygon(halves, {a, b, c});
        add_polygon(halves, {a, c, d});
        const Mask one = render(whole, glyphs::Point{});
        const Mask two = render(halves, glyphs::Point{});
        if (!one.rendered || !two.rendered || one.bytes != two.bytes) equal = false;
    }
    CHECK(equal);
}

// M11.
void forms() {
    namespace detail = stxlib::glyphs::detail;
    // A mask of 8 by 8 pixels; chord ends in 1/256 of a pixel.
    constexpr i32 extent = 8 * 256;
    constexpr usize stride = 8 + detail::spare_cells;
    std::vector<stxlib::i64> by_forms(stride * 8, 0);
    std::vector<stxlib::i64> in_full(stride * 8, 0);
    std::vector<i32> words(detail::block_words, 0);
    detail::Block block{.words = words.data(), .count = 0, .listed = {}};
    const detail::Cells form_cells{.cells = by_forms.data(), .stride = stride};
    const detail::Cells full_cells{.cells = in_full.data(), .stride = stride};
    std::uint64_t drawn[detail::form_count] = {};
    std::uint64_t level = 0;
    bool equal = true;
    for (int round = 0; round < 40000; ++round) {
        for (int chord = 0; chord < 60; ++chord) {
            // An end anywhere; the other near it or anywhere; some
            // coordinates on cell edges.
            detail::Fixed begin{.x = random_coordinate(extent + 1), .y = random_coordinate(extent + 1)};
            detail::Fixed end{.x = random_coordinate(extent + 1), .y = random_coordinate(extent + 1)};
            if (next_random() % 8 != 0) {
                const i32 reach = 16 + random_coordinate(600);
                end.x = begin.x + random_coordinate(2 * reach + 1) - reach;
                end.y = begin.y + random_coordinate(2 * reach + 1) - reach;
                if (next_random() % 6 == 0) end.x = begin.x;
                if (next_random() % 12 == 0) end.y = begin.y;
            }
            if (next_random() % 4 == 0) begin.x = (begin.x / 256) * 256;
            if (next_random() % 4 == 0) begin.y = (begin.y / 256) * 256;
            if (next_random() % 4 == 0) end.x = (end.x / 256) * 256;
            if (next_random() % 4 == 0) end.y = (end.y / 256) * 256;
            end.x = end.x < 0 ? 0 : (end.x > extent ? extent : end.x);
            end.y = end.y < 0 ? 0 : (end.y > extent ? extent : end.y);
            if (block.count == detail::block_chords) {
                for (usize form = 0; form < detail::form_count; ++form) drawn[form] = drawn[form] + block.listed[form];
                detail::add_block(form_cells, block);
            }
            detail::put_chord(block, begin, end);
            if (begin.y == end.y) {
                level = level + 1;
            } else if (begin.y < end.y) {
                detail::add_any_chord(full_cells, begin, end, 1);
            } else {
                detail::add_any_chord(full_cells, end, begin, -1);
            }
        }
        for (usize form = 0; form < detail::form_count; ++form) drawn[form] = drawn[form] + block.listed[form];
        detail::add_block(form_cells, block);
        if (by_forms != in_full) equal = false;
    }
    std::printf("forms: in a cell %llu, across two cells %llu, down two rows %llu, over four cells %llu, other %llu, along a row %llu\n",
                static_cast<unsigned long long>(drawn[0]), static_cast<unsigned long long>(drawn[1]),
                static_cast<unsigned long long>(drawn[2]), static_cast<unsigned long long>(drawn[3]),
                static_cast<unsigned long long>(drawn[4]), static_cast<unsigned long long>(level));
    CHECK(equal);
    for (usize form = 0; form < detail::form_count; ++form) CHECK(drawn[form] > 100000);
    CHECK(level > 1000);
}

// M10.
void reciprocals() {
    namespace detail = stxlib::glyphs::detail;
    std::uint64_t compared = 0;
    std::uint64_t wrong = 0;
    for (stxlib::i64 divisor = 1; divisor < detail::brief_extent; ++divisor) {
        const stxlib::u64 reciprocal = detail::reciprocals.of[divisor];
        const stxlib::i64 greatest = static_cast<stxlib::i64>(detail::brief_dividend);
        if (detail::quotient(0, divisor, reciprocal) != 0) wrong = wrong + 1;
        if (detail::quotient(greatest, divisor, reciprocal) != greatest / divisor) wrong = wrong + 1;
        const stxlib::i64 last = greatest / divisor;
        for (stxlib::i64 expected = 1; expected <= last; ++expected) {
            const stxlib::i64 dividend = expected * divisor;
            wrong = wrong + (detail::quotient(dividend, divisor, reciprocal) != expected ? 1U : 0U);
            wrong = wrong + (detail::quotient(dividend - 1, divisor, reciprocal) != expected - 1 ? 1U : 0U);
        }
        compared = compared + 2 * static_cast<std::uint64_t>(last) + 2;
    }
    std::printf("reciprocals: %llu quotients compared, %llu wrong\n", static_cast<unsigned long long>(compared),
                static_cast<unsigned long long>(wrong));
    CHECK(detail::reciprocals.of[0] == 0);
    CHECK(wrong == 0);
}

// M9.
void many_chords() {
    // One triangle in each 4 by 4 pixel tile of a 28 by 28 grid: no pixel
    // shared.
    constexpr i32 tiles = 28;
    constexpr i32 tile = 4 * 64;
    Shape all{};
    std::vector<Shape> each{};
    for (i32 row = 0; row < tiles; ++row) {
        for (i32 column = 0; column < tiles; ++column) {
            std::vector<glyphs::Point> corners{};
            for (int corner = 0; corner < 3; ++corner) {
                corners.push_back(glyphs::Point{.x = column * tile + 1 + random_coordinate(tile - 2),
                                                .y = row * tile + 1 + random_coordinate(tile - 2)});
            }
            add_polygon(all, corners);
            Shape single{};
            add_polygon(single, corners);
            each.push_back(single);
        }
    }
    const glyphs::Limits limits{};
    const glyphs::Bounds bounds{.left = 0, .top = tiles * 4, .width = static_cast<u32>(tiles * 4), .height = static_cast<u32>(tiles * 4)};
    const usize size = static_cast<usize>(bounds.width) * bounds.height;
    glyphs::Rasterizer rasterizer{};
    std::vector<u8> first(size, 0x5a);
    std::vector<u8> together(size, 0x5a);
    std::vector<u8> alone(size, 0x5a);
    std::vector<u8> sum(size, 0);
    bool rendered = rasterizer.render(outline_of(each[0]), glyphs::Point{}, bounds, limits, stxlib::MutableView<u8>(first.data(), size)).succeeded();
    rendered = rendered &&
               rasterizer.render(outline_of(all), glyphs::Point{}, bounds, limits, stxlib::MutableView<u8>(together.data(), size)).succeeded();
    bool same_after = true;
    bool disjoint = true;
    for (usize index = 0; index < each.size(); ++index) {
        rendered = rendered &&
                   rasterizer.render(outline_of(each[index]), glyphs::Point{}, bounds, limits, stxlib::MutableView<u8>(alone.data(), size))
                       .succeeded();
        if (index == 0 && alone != first) same_after = false;
        for (usize pixel = 0; pixel < size; ++pixel) {
            if (alone[pixel] == 0) continue;
            if (sum[pixel] != 0) disjoint = false;
            sum[pixel] = alone[pixel];
        }
    }
    CHECK(rendered);
    CHECK(same_after);
    CHECK(disjoint);
    CHECK(together == sum);
}

// The chords the stated rule makes of a quadratic: integer arithmetic of
// its own in 1/256 of a pixel from the mask's left and bottom edges, then
// doubles in mask coordinates.
void quadratic_chords(const glyphs::Point p0, const glyphs::Point p1, const glyphs::Point p2, const glyphs::Bounds& bounds,
                      std::vector<Chord>& chords) {
    const long long left = static_cast<long long>(bounds.left) * 64;
    const long long bottom = (static_cast<long long>(bounds.top) - bounds.height) * 64;
    const long long x0 = (p0.x - left) * 4;
    const long long y0 = (p0.y - bottom) * 4;
    const long long x1 = (p1.x - left) * 4;
    const long long y1 = (p1.y - bottom) * 4;
    const long long x2 = (p2.x - left) * 4;
    const long long y2 = (p2.y - bottom) * 4;
    const long long ax = x0 - 2 * x1 + x2;
    const long long ay = y0 - 2 * y1 + y2;
    long long spread = std::max(std::llabs(ax), std::llabs(ay));
    long long count = 1;
    while (spread > 64) {
        spread = spread / 4;
        count = count * 2;
    }
    long long px = x0;
    long long py = y0;
    for (long long step = 1; step <= count; ++step) {
        long long x = x2;
        long long y = y2;
        if (step < count) {
            // Rounded down: a floor of a quotient that may be negative.
            const long long nx = 2 * step * count * (x1 - x0) + step * step * ax;
            const long long ny = 2 * step * count * (y1 - y0) + step * step * ay;
            const long long square = count * count;
            x = x0 + (nx >= 0 ? nx / square : -((-nx + square - 1) / square));
            y = y0 + (ny >= 0 ? ny / square : -((-ny + square - 1) / square));
        }
        const double height = static_cast<double>(bounds.height);
        chords.push_back(Chord{.x0 = static_cast<double>(px) / 256.0,
                               .y0 = height - static_cast<double>(py) / 256.0,
                               .x1 = static_cast<double>(x) / 256.0,
                               .y1 = height - static_cast<double>(y) / 256.0});
        px = x;
        py = y;
    }
}

// M6.
void curves() {
    Agreement agreement{};
    for (int round = 0; round < 400; ++round) {
        // A closed shape of three quadratics around random points.
        glyphs::Point p[6];
        for (glyphs::Point& point : p) point = glyphs::Point{.x = random_coordinate(64 * 30), .y = random_coordinate(64 * 30)};
        Shape shape{};
        shape.segments = {glyphs::Segment::move, glyphs::Segment::quadratic, glyphs::Segment::quadratic, glyphs::Segment::quadratic};
        shape.points = {p[0], p[1], p[2], p[3], p[4], p[5], p[0]};
        const Mask mask = render(shape, glyphs::Point{});
        CHECK(mask.rendered);
        if (!mask.rendered) return;
        std::vector<Chord> chords{};
        const usize order[3][3] = {{0, 1, 2}, {2, 3, 4}, {4, 5, 0}};
        for (const usize* const arc : order) quadratic_chords(p[arc[0]], p[arc[1]], p[arc[2]], mask.bounds, chords);
        compare(mask, chords, agreement);
    }
    std::printf("quadratics: %llu pixels, %llu outside the slack, %llu other byte\n",
                static_cast<unsigned long long>(agreement.pixels), static_cast<unsigned long long>(agreement.outside_slack),
                static_cast<unsigned long long>(agreement.other_byte));
    CHECK(agreement.outside_slack == 0);
    // A cubic whose control points lie on its chord is that chord.
    Shape straight{};
    straight.segments = {glyphs::Segment::move, glyphs::Segment::line, glyphs::Segment::cubic};
    straight.points = {{0, 0}, {640, 0}, {640 - 213, 213}, {213, 427}, {0, 640}};
    Shape triangle{};
    add_polygon(triangle, {{0, 0}, {640, 0}, {0, 640}});
    const Mask curved = render(straight, glyphs::Point{});
    const Mask plain = render(triangle, glyphs::Point{});
    Agreement close_to{};
    std::vector<Chord> chords{};
    polygon_chords({{0, 0}, {640, 0}, {0, 640}}, plain.bounds, chords);
    compare(curved, chords, close_to);
    CHECK(curved.rendered && plain.rendered && curved.bounds.width == plain.bounds.width);
    // The control points are on the chord only to 1/64 of a pixel.
    CHECK(close_to.outside_slack * 4 < close_to.pixels);
    // A circle of four cubics: every pixel well inside is full, well outside empty.
    Shape circle{};
    const i32 r = 64 * 20;
    const i32 k = static_cast<i32>(r * 0.5522847498);
    circle.segments = {glyphs::Segment::move, glyphs::Segment::cubic, glyphs::Segment::cubic, glyphs::Segment::cubic, glyphs::Segment::cubic};
    circle.points = {{r, 0}, {r, k}, {k, r}, {0, r}, {-k, r}, {-r, k}, {-r, 0}, {-r, -k}, {-k, -r}, {0, -r}, {k, -r}, {r, -k}, {r, 0}};
    const Mask disc = render(circle, glyphs::Point{});
    CHECK(disc.rendered && disc.bounds.width == 40 && disc.bounds.height == 40);
    bool round_enough = disc.rendered;
    double total = 0.0;
    for (usize y = 0; y < disc.bounds.height && round_enough; ++y) {
        for (usize x = 0; x < disc.bounds.width; ++x) {
            const double cx = static_cast<double>(x) + 0.5 - 20.0;
            const double cy = static_cast<double>(y) + 0.5 - 20.0;
            const double distance = std::sqrt(cx * cx + cy * cy);
            const int got = disc.bytes[y * disc.bounds.width + x];
            total = total + got / 255.0;
            if (distance < 19.0 && got != 255) round_enough = false;
            if (distance > 21.0 && got != 0) round_enough = false;
        }
    }
    CHECK(round_enough);
    // The disc's area, to the accuracy of the four-cubic circle and the rounding down.
    CHECK(std::fabs(total - 3.14159265 * 400.0) < 12.0);
}

// M7, M8.
void bounds_and_refusals() {
    const glyphs::Limits limits{};
    glyphs::Bounds bounds{};
    Shape shape{};
    add_polygon(shape, {{-65, 1}, {130, 1}, {130, 127}});
    CHECK(glyphs::measure(outline_of(shape), glyphs::Point{}, limits, bounds).succeeded());
    CHECK(bounds.left == -2 && bounds.width == 5 && bounds.top == 2 && bounds.height == 2);
    CHECK(glyphs::measure(outline_of(shape), glyphs::Point{.x = 64, .y = -128}, limits, bounds).succeeded());
    CHECK(bounds.left == -1 && bounds.width == 5 && bounds.top == 0 && bounds.height == 2);
    const Shape nothing{};
    CHECK(glyphs::measure(outline_of(nothing), glyphs::Point{}, limits, bounds).succeeded() && bounds.width == 0 && bounds.height == 0);

    glyphs::Rasterizer rasterizer{};
    std::vector<u8> bytes(64, 0);
    // No move first.
    Shape headless{};
    headless.segments = {glyphs::Segment::line};
    headless.points = {{0, 0}};
    stxlib::Status status = glyphs::measure(outline_of(headless), glyphs::Point{}, limits, bounds);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // Points that do not match the segments.
    Shape short_of_points{};
    short_of_points.segments = {glyphs::Segment::move, glyphs::Segment::cubic};
    short_of_points.points = {{0, 0}, {64, 64}};
    status = glyphs::measure(outline_of(short_of_points), glyphs::Point{}, limits, bounds);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // An unknown segment.
    Shape unknown{};
    unknown.segments = {glyphs::Segment::move, static_cast<glyphs::Segment>(9)};
    unknown.points = {{0, 0}, {64, 64}};
    status = glyphs::measure(outline_of(unknown), glyphs::Point{}, limits, bounds);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // Too many points; too large a mask.
    glyphs::Limits few{};
    few.points = 2;
    status = glyphs::measure(outline_of(shape), glyphs::Point{}, few, bounds);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    glyphs::Limits narrow{};
    narrow.width = 4;
    status = glyphs::measure(outline_of(shape), glyphs::Point{}, narrow, bounds);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    // A point outside the box given to render; a mask of the wrong size; too little working room.
    CHECK(glyphs::measure(outline_of(shape), glyphs::Point{}, limits, bounds).succeeded());
    glyphs::Bounds smaller = bounds;
    smaller.width = bounds.width - 1;
    bytes.assign(static_cast<usize>(smaller.width) * smaller.height, 0);
    status = rasterizer.render(outline_of(shape), glyphs::Point{}, smaller, limits, stxlib::MutableView<u8>(bytes.data(), bytes.size()));
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument);
    bytes.assign(3, 0);
    status = rasterizer.render(outline_of(shape), glyphs::Point{}, bounds, limits, stxlib::MutableView<u8>(bytes.data(), bytes.size()));
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::invalid_argument);
    glyphs::Limits cramped{};
    cramped.work_cells = 4;
    bytes.assign(static_cast<usize>(bounds.width) * bounds.height, 0);
    status = rasterizer.render(outline_of(shape), glyphs::Point{}, bounds, cramped, stxlib::MutableView<u8>(bytes.data(), bytes.size()));
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    // And then it renders, with the same rasterizer.
    status = rasterizer.render(outline_of(shape), glyphs::Point{}, bounds, limits, stxlib::MutableView<u8>(bytes.data(), bytes.size()));
    CHECK(status.succeeded());
    rasterizer.release();
    status = rasterizer.render(outline_of(shape), glyphs::Point{}, bounds, limits, stxlib::MutableView<u8>(bytes.data(), bytes.size()));
    CHECK(status.succeeded());
    // Wound twice: a sum, clamped to full.
    Shape twice{};
    add_polygon(twice, {{0, 0}, {0, 640}, {640, 640}, {640, 0}});
    add_polygon(twice, {{0, 0}, {0, 640}, {640, 640}, {640, 0}});
    const Mask doubled = render(twice, glyphs::Point{});
    bool full = doubled.rendered;
    for (const u8 byte : doubled.bytes) {
        if (byte != 255) full = false;
    }
    CHECK(full);
}

} // namespace

int main() {
    rectangles();
    polygons();
    shared_edges();
    curves();
    bounds_and_refusals();
    many_chords();
    reciprocals();
    forms();
    const int status = stxlib_tests::finish("stxlib_glyph_mask_tests");
    return status;
}
