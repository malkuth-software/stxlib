// stxlib::glyphs: the coverage mask of an outline (include/stxlib/
// glyph_mask.hpp states the rule; this is its arithmetic).
//
// The method is signed-area accumulation. Inside the mask every chord is
// cut at the rows it crosses, and each row's piece at the cells it crosses.
// A piece inside one cell, from horizontal fraction fa to fb with height c
// (signed), adds to that cell
//     c * 2 * one - (fa + fb) * c
// and to the cell on its right
//     (fa + fb) * c.
// The first is twice the area to the right of the piece inside the cell,
// the two together twice the area of the full cell's width over the height
// c: so a running sum along the row gives, at every pixel, twice its
// signed area (in units of one * one, one being 65536). No pixel is
// visited per edge beyond the cells the edge passes through; the mask is
// produced by one pass that sums, takes the magnitude, shifts and clamps,
// with no decision about edges in it.
//
// Where a chord is cut. A chord's ends are in 1/256 of a pixel. The x at
// which it crosses a row boundary is the exact x rounded down to 1/65536 of
// a pixel. Inside a row, the height a piece has to the left of a cell's
// edge is the distance from the piece's left end to that edge times the
// chord's slope, rounded down to 1/65536, and never more than the piece's
// height.
//
// Exactness of what must be exact. The heights given to the cells of one
// row's piece are differences of that running height, and the last cell
// gets the rest, so they add up to the piece's height exactly; the x at
// which a chord crosses a row boundary is one value, used by the piece
// above and the piece below; products of fractions and heights are not
// rounded at all; and a chord gives the same pieces whichever way it is
// traversed. So every closed contour's heights cancel exactly in every
// row: nothing leaks to the right of a shape.
//
// The chords' arithmetic is in chords.hpp: in full for any chord, and
// again, with no decision in it, for each of the four forms most chords of
// text have (in one cell, across two cells, down two rows, over four
// cells). The outline's chords are written to a block, listed by form, and
// each list is drawn by its own loop. The flattening of curves, the checks
// and the pass that writes the mask are here.
//
// Divisions. Every quotient above is a rounded-down quotient of
// non-negative integers. For a chord shorter than four pixels both ways
// the divisor is below 1024 and the dividend at most 2^26 + 1024, and the
// quotient is taken by multiplying with a tabled reciprocal, which gives
// the same integer (reciprocal.hpp has the proof, its premise checked when
// this file is compiled; tests/glyph_mask_tests.cpp compares the two at
// every dividend where a quotient changes). Longer chords divide. The
// result does not depend on which was used.
//
// Ranges. Mask coordinates are 16.16 fixed point in [0, 16384 * 65536], so
// a difference of two is below 2^31, a product of a difference and a
// height (at most 2^30) below 2^61, and a cell's value for one piece below
// 2^34. With at most 2^16 chords for each of at most 16384 points, every
// sum is far inside 64 bits.

#include "stxlib/glyph_mask.hpp"

#include "algorithms/glyphs/chords.hpp"

namespace stxlib::glyphs {

namespace {

using detail::add_block;
using detail::Block;
using detail::block_chords;
using detail::block_words;
using detail::Cells;
using detail::Fixed;
using detail::fraction_bits;
using detail::frame_one;
using detail::magnitude;
using detail::put_chord;
using detail::spare_cells;

// well_formed counts points by a segment's number.
static_assert(static_cast<u32>(Segment::move) == 0 && static_cast<u32>(Segment::line) == 1 &&
              static_cast<u32>(Segment::quadratic) == 2 && static_cast<u32>(Segment::cubic) == 3);

// A pixel's value is its doubled area (units of one * one) shifted by this.
inline constexpr u32 value_shift = 2 * fraction_bits + 1 - 8;
// A quadratic becomes at most 2^16 chords (the second difference of the
// largest curve a mask can hold vanishes sooner), and a cubic is halved at
// most 16 times.
inline constexpr u32 longest_subdivision = 16;
inline constexpr usize cubic_depth = 16;
// The flattening's two measures, in 1/256 of a pixel.
inline constexpr i64 quarter_pixel = 64;
inline constexpr i64 half_pixel = 128;

// The working rows, the chords in hand, and the frame points are in.
struct Work final {
    Cells cells{};
    Block block{};
    // The mask's box in 26.6: x of the left edge, y of the bottom edge.
    i64 left{0};
    i64 bottom{0};
    // The mask's height in 1/256 of a pixel.
    i64 height{0};
};

// Curves are flattened in the frame FreeType's smooth rasterizer uses, so
// that the chords are the same ones: 24.8 fixed point (1/256 of a pixel),
// x from the mask's left edge and y upward from its bottom edge. A point of
// the outline is exact there (26.6 times four).
[[nodiscard]] Fixed to_frame(const Work& work, const Point point, const Point offset) noexcept {
    const i64 x = static_cast<i64>(point.x) + offset.x - work.left;
    const i64 y = static_cast<i64>(point.y) + offset.y - work.bottom;
    const Fixed result{.x = x * 4, .y = y * 4};
    return result;
}

// Room in the block for `count` more chords (no more than block_chords):
// the chords in hand are drawn first if there is not.
void make_room(Work& work, const usize count) noexcept {
    if (work.block.count + count > block_chords) add_block(work.cells, work.block);
}

// A chord between two points of the frame, to the block, in mask
// coordinates (y downward). Requirement: room for it.
void put_frame_chord(Work& work, const Fixed begin, const Fixed end) noexcept {
    const Fixed from{.x = begin.x, .y = work.height - begin.y};
    const Fixed to{.x = end.x, .y = work.height - end.y};
    put_chord(work.block, from, to);
}

[[nodiscard]] i64 larger(const i64 a, const i64 b) noexcept {
    const i64 result = a > b ? a : b;
    return result;
}

// A quadratic: 2^k chords at equal steps of the parameter. k is the number
// of times the second difference (the larger of |P0 - 2 P1 + P2| on the two
// axes, in 1/256 of a pixel) is quartered, rounding down, before it is at
// most 64 (a quarter of a pixel). The chords' ends are the curve's points
// rounded down to 1/256 of a pixel.
void add_quadratic(Work& work, const Fixed start, const Fixed control, const Fixed end) noexcept {
    const i64 bx = control.x - start.x;
    const i64 by = control.y - start.y;
    const i64 ax = end.x - control.x - bx;
    const i64 ay = end.y - control.y - by;
    i64 spread = larger(magnitude(ax), magnitude(ay));
    u32 shift = 0;
    while (spread > quarter_pixel && shift < longest_subdivision) {
        spread = spread >> 2;
        shift = shift + 1;
    }
    const i64 count = static_cast<i64>(1) << shift;
    Fixed previous = start;
    i64 step = 1;
    while (step < count) {
        // Room is made for a run of chords, not for each.
        const i64 stop = count - step > static_cast<i64>(block_chords) ? step + static_cast<i64>(block_chords) : count;
        make_room(work, static_cast<usize>(stop - step));
        for (; step < stop; ++step) {
            // P0 + 2 t (P1 - P0) + t^2 (P0 - 2 P1 + P2) at t = step / count:
            // the numerator over count^2, rounded down.
            const Fixed point{.x = start.x + ((2 * step * count * bx + step * step * ax) >> (2 * shift)),
                              .y = start.y + ((2 * step * count * by + step * step * ay) >> (2 * shift))};
            put_frame_chord(work, previous, point);
            previous = point;
        }
    }
    make_room(work, 1);
    put_frame_chord(work, previous, end);
}

// One coordinate of a cubic arc, stored end first in arc[0..3], halved at
// its parameter's midpoint, each sum rounded down: the second half stays in
// arc[0..3] and the first half goes to arc[3..6] (they share arc[3], the
// midpoint).
void halve(i64& p0, i64& p1, i64& p2, i64& p3, i64& p4, i64& p5, i64& p6) noexcept {
    p6 = p3;
    i64 a = p0 + p1;
    const i64 b = p1 + p2;
    i64 c = p2 + p3;
    p5 = c >> 1;
    c = c + b;
    p4 = c >> 2;
    p1 = a >> 1;
    a = a + b;
    p2 = a >> 2;
    p3 = (a + c) >> 3;
}

// A cubic is halved until, on both axes, |2 P3 - 3 P2 + P0| and
// |P3 - 3 P1 + 2 P0| are at most half a pixel (each control point then lies
// within a sixth of a pixel of where a straight chord would have it), and
// is then drawn as its chord. The arcs still to draw are kept on a fixed
// stack, the nearest to the start on top.
void add_cubic(Work& work, const Fixed start, const Fixed first, const Fixed second, const Fixed end) noexcept {
    Fixed stack[cubic_depth * 3 + 7];
    stack[0] = end;
    stack[1] = second;
    stack[2] = first;
    stack[3] = start;
    usize arc = 0;
    Fixed current = start;
    for (;;) {
        Fixed* const a = stack + arc;
        const bool flat = magnitude(2 * a[0].x - 3 * a[1].x + a[3].x) <= half_pixel &&
                          magnitude(2 * a[0].y - 3 * a[1].y + a[3].y) <= half_pixel &&
                          magnitude(a[0].x - 3 * a[2].x + 2 * a[3].x) <= half_pixel &&
                          magnitude(a[0].y - 3 * a[2].y + 2 * a[3].y) <= half_pixel;
        if (flat || arc >= cubic_depth * 3) {
            make_room(work, 1);
            put_frame_chord(work, current, a[0]);
            current = a[0];
            if (arc == 0) return;
            arc = arc - 3;
            continue;
        }
        halve(a[0].x, a[1].x, a[2].x, a[3].x, a[4].x, a[5].x, a[6].x);
        halve(a[0].y, a[1].y, a[2].y, a[3].y, a[4].y, a[5].y, a[6].y);
        arc = arc + 3;
    }
}

[[nodiscard]] Status refuse(const ErrorKind kind, const Operation operation) noexcept {
    const Status result = Status::failure(ErrorRecord::make(kind, operation));
    return result;
}

// The outline's form: known segments, a move first, points for every
// segment and none left over, no more than the limit. One pass with no
// decision in it: a segment's number is the points it takes, but for a
// move, which takes one.
[[nodiscard]] bool well_formed(const Outline& outline, const Limits& limits, bool& too_many) noexcept {
    too_many = outline.points.count() > limits.points;
    if (too_many) return false;
    const usize segment_count = outline.segments.count();
    const Segment* const segments = outline.segments.data();
    usize needed = 0;
    usize moves = 0;
    u32 every = 0;
    for (usize index = 0; index < segment_count; ++index) {
        const u32 number = static_cast<u32>(segments[index]);
        every = every | number;
        needed = needed + number;
        moves = moves + (number == 0 ? 1U : 0U);
    }
    // The known numbers are 0 to 3: every one of them is, if no bit above
    // those is set in any.
    if (every > static_cast<u32>(Segment::cubic)) return false;
    needed = needed + moves;
    if (segment_count != 0 && segments[0] != Segment::move) return false;
    const bool result = needed == outline.points.count();
    return result;
}

// The least and greatest coordinates of the points. Requirement: count > 0.
struct Extent final {
    i32 x_low{0};
    i32 x_high{0};
    i32 y_low{0};
    i32 y_high{0};
};

[[nodiscard]] Extent extent_of(const Point* const points, const usize count) noexcept {
    i32 x_low = points[0].x;
    i32 x_high = x_low;
    i32 y_low = points[0].y;
    i32 y_high = y_low;
    for (usize index = 1; index < count; ++index) {
        const i32 x = points[index].x;
        const i32 y = points[index].y;
        x_low = x < x_low ? x : x_low;
        x_high = x > x_high ? x : x_high;
        y_low = y < y_low ? y : y_low;
        y_high = y > y_high ? y : y_high;
    }
    const Extent result{.x_low = x_low, .x_high = x_high, .y_low = y_low, .y_high = y_high};
    return result;
}

// Every chord of the outline into the cells. Requirement: the outline is
// well formed and every point lies in the mask's box.
void accumulate(Work& work, const Outline& outline, const Point offset) noexcept {
    const usize segment_count = outline.segments.count();
    const Segment* const segments = outline.segments.data();
    const Point* const points = outline.points.data();
    usize next_point = 0;
    Fixed contour_start{};
    Fixed current{};
    bool open = false;
    for (usize index = 0; index < segment_count; ++index) {
        const Segment segment = segments[index];
        if (segment == Segment::move) {
            make_room(work, 1);
            if (open) put_frame_chord(work, current, contour_start);
            contour_start = to_frame(work, points[next_point], offset);
            current = contour_start;
            open = true;
            next_point = next_point + 1;
        } else if (segment == Segment::line) {
            const Fixed end = to_frame(work, points[next_point], offset);
            make_room(work, 1);
            put_frame_chord(work, current, end);
            current = end;
            next_point = next_point + 1;
        } else if (segment == Segment::quadratic) {
            const Fixed control = to_frame(work, points[next_point], offset);
            const Fixed end = to_frame(work, points[next_point + 1], offset);
            add_quadratic(work, current, control, end);
            current = end;
            next_point = next_point + 2;
        } else {
            const Fixed first = to_frame(work, points[next_point], offset);
            const Fixed second = to_frame(work, points[next_point + 1], offset);
            const Fixed end = to_frame(work, points[next_point + 2], offset);
            add_cubic(work, current, first, second, end);
            current = end;
            next_point = next_point + 3;
        }
    }
    make_room(work, 1);
    if (open) put_frame_chord(work, current, contour_start);
    add_block(work.cells, work.block);
}

// The mask from the cells: along each row a running sum, its magnitude in
// 256ths of a pixel, full coverage as 255.
void write_mask(const i64* const cells, const usize stride, const usize width, const usize height, u8* const out) noexcept {
    for (usize row = 0; row < height; ++row) {
        const i64* const row_cells = cells + row * stride;
        u8* const row_out = out + row * width;
        i64 sum = 0;
        for (usize x = 0; x < width; ++x) {
            sum = sum + row_cells[x];
            const i64 value = magnitude(sum) >> value_shift;
            row_out[x] = static_cast<u8>(value > 255 ? 255 : value);
        }
    }
}

} // namespace

Status measure(const Outline& outline, const Point offset, const Limits& limits, Bounds& bounds) noexcept {
    const Operation operation("stxlib.glyphs.measure");
    bool too_many = false;
    if (!well_formed(outline, limits, too_many)) {
        const Status refused = refuse(too_many ? ErrorKind::limit_exceeded : ErrorKind::invalid_argument, operation);
        return refused;
    }
    bounds = Bounds{};
    const usize count = outline.points.count();
    if (count == 0) return Status::success();
    const Extent extent = extent_of(outline.points.data(), count);
    const i64 x_low = static_cast<i64>(extent.x_low) + offset.x;
    const i64 x_high = static_cast<i64>(extent.x_high) + offset.x;
    const i64 y_low = static_cast<i64>(extent.y_low) + offset.y;
    const i64 y_high = static_cast<i64>(extent.y_high) + offset.y;
    // Outward to whole pixels.
    const i64 left = x_low >> 6;
    const i64 right = (x_high + 63) >> 6;
    const i64 bottom = y_low >> 6;
    const i64 top = (y_high + 63) >> 6;
    const i64 width = right - left;
    const i64 height = top - bottom;
    if (width > limits.width || height > limits.height) return refuse(ErrorKind::limit_exceeded, operation);
    // i32 sums of two i32 values fit 33 bits; a box that does not fit 32 is
    // far outside any mask.
    if (left < -0x40000000LL || top > 0x40000000LL || left > 0x40000000LL || top < -0x40000000LL) {
        return refuse(ErrorKind::limit_exceeded, operation);
    }
    bounds = Bounds{.left = static_cast<i32>(left),
                    .top = static_cast<i32>(top),
                    .width = static_cast<u32>(width),
                    .height = static_cast<u32>(height)};
    return Status::success();
}

Status Rasterizer::render(const Outline& outline, const Point offset, const Bounds& bounds, const Limits& limits,
                          const MutableView<u8> mask) noexcept {
    const Operation operation("stxlib.glyphs.render");
    bool too_many = false;
    if (!well_formed(outline, limits, too_many)) {
        const Status refused = refuse(too_many ? ErrorKind::limit_exceeded : ErrorKind::invalid_argument, operation);
        return refused;
    }
    if (bounds.width > limits.width || bounds.height > limits.height) return refuse(ErrorKind::limit_exceeded, operation);
    const usize width = bounds.width;
    const usize height = bounds.height;
    if (mask.count() != width * height) return refuse(ErrorKind::invalid_argument, operation);
    if (width == 0 || height == 0) {
        // Nothing to write; an outline with points cannot fit such a box
        // unless it is a single dot or a line along a pixel edge, which
        // covers nothing.
        return Status::success();
    }
    u8* const out = mask.data();
    const usize point_count = outline.points.count();
    if (point_count == 0) {
        for (usize index = 0; index < width * height; ++index) out[index] = 0;
        return Status::success();
    }
    // Every point inside the box, checked once, before any arithmetic that
    // relies on it.
    const i64 box_left = static_cast<i64>(bounds.left) * 64;
    const i64 box_right = box_left + static_cast<i64>(width) * 64;
    const i64 box_top = static_cast<i64>(bounds.top) * 64;
    const i64 box_bottom = box_top - static_cast<i64>(height) * 64;
    const Extent extent = extent_of(outline.points.data(), point_count);
    if (static_cast<i64>(extent.x_low) + offset.x < box_left || static_cast<i64>(extent.x_high) + offset.x > box_right ||
        static_cast<i64>(extent.y_low) + offset.y < box_bottom || static_cast<i64>(extent.y_high) + offset.y > box_top) {
        return refuse(ErrorKind::invalid_argument, operation);
    }
    const usize stride = width + spare_cells;
    const usize cell_count = stride * height;
    if (cell_count > limits.work_cells) return refuse(ErrorKind::limit_exceeded, operation);
    if (cells_.count() < cell_count) {
        if (cells_.resize(cell_count).failed()) return refuse(ErrorKind::out_of_memory, operation);
    }
    if (block_.count() < block_words) {
        if (block_.resize(block_words).failed()) return refuse(ErrorKind::out_of_memory, operation);
    }
    i64* const cells = cells_.mutable_view().data();
    for (usize index = 0; index < cell_count; ++index) cells[index] = 0;
    Work work{.cells = Cells{.cells = cells, .stride = stride},
              .block = Block{.words = block_.mutable_view().data(), .count = 0, .listed = {}},
              .left = box_left,
              .bottom = box_bottom,
              .height = static_cast<i64>(height) * frame_one};
    accumulate(work, outline, offset);
    write_mask(cells, stride, width, height, out);
    return Status::success();
}

void Rasterizer::release() noexcept {
    cells_.release();
    block_.release();
}

} // namespace stxlib::glyphs
