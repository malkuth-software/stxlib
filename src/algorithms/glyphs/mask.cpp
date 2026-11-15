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
// the two together twice the area of the full cell's height c: so a
// running sum along the row gives, at every pixel, twice its signed area
// (in units of one * one, one being 65536). No pixel is visited per edge
// beyond the cells the edge passes through; the mask is produced by one
// pass that sums, takes the magnitude, shifts and clamps, with no decision
// about edges in it.
//
// Exactness of what must be exact. The heights given to the cells of one
// row's piece are differences of a rounded-down running total, so they add
// up to the piece's height exactly; the x at which a chord crosses a row
// boundary is one value, used by the piece above and the piece below; and
// a chord gives the same crossings whichever way it is traversed. So every
// closed contour's heights cancel exactly in every row: nothing leaks to
// the right of a shape, whatever the rounding of areas.
//
// Ranges. Mask coordinates are 16.16 fixed point in [0, 16384 * 65536], so
// a difference of two is below 2^31, a product of a difference and a
// height (at most 2^30) below 2^61, and a cell's value for one piece below
// 2^34. With at most 2^16 chords for each of at most 16384 points, every
// sum is far inside 64 bits.

#include "stxlib/glyph_mask.hpp"

namespace stxlib::glyphs {

namespace {

inline constexpr u32 fraction_bits = 16;
inline constexpr i64 one = static_cast<i64>(1) << fraction_bits;
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

struct Fixed final {
    i64 x{0};
    i64 y{0};
};

// The working rows: `stride` cells a row (the mask's width and two more,
// for a piece on the right edge and what it passes to its right).
struct Work final {
    i64* cells{nullptr};
    usize stride{0};
    // The mask's box in 26.6: x of the left edge, y of the bottom edge.
    i64 left{0};
    i64 bottom{0};
    // The mask's height in 16.16.
    i64 height{0};
};

// The quotient rounded toward minus infinity. Requirement: divisor > 0.
[[nodiscard]] i64 floor_divide(const i64 dividend, const i64 divisor) noexcept {
    i64 quotient = dividend / divisor;
    if (dividend % divisor < 0) quotient = quotient - 1;
    return quotient;
}

[[nodiscard]] i64 magnitude(const i64 value) noexcept {
    const i64 result = value < 0 ? -value : value;
    return result;
}

// A piece inside one cell: from fraction `enters` to fraction `leaves`
// (each 0 to one), of signed height `cover`.
void add_to_cell(const Work& work, const i64 row, const i64 cell, const i64 enters, const i64 leaves,
                 const i64 cover) noexcept {
    const i64 doubled_left = (enters + leaves) * cover;
    i64* const at = work.cells + static_cast<usize>(row) * work.stride + static_cast<usize>(cell);
    at[0] = at[0] + (cover * 2 * one - doubled_left);
    at[1] = at[1] + doubled_left;
}

// A piece inside one row, from x `from` to x `to`, of signed height
// `cover` (its magnitude at most one).
void add_to_row(const Work& work, const i64 row, const i64 from, const i64 to, const i64 cover) noexcept {
    const i64 first_cell = from >> fraction_bits;
    const i64 last_cell = to >> fraction_bits;
    const i64 from_fraction = from - (first_cell << fraction_bits);
    const i64 to_fraction = to - (last_cell << fraction_bits);
    if (first_cell == last_cell) {
        add_to_cell(work, row, first_cell, from_fraction, to_fraction, cover);
        return;
    }
    // The piece crosses cells: each gets the share of the height that its
    // share of the run earns, as differences of a running total rounded
    // down, so the shares add up to the height exactly.
    const i64 sign = cover < 0 ? -1 : 1;
    const i64 height = magnitude(cover);
    const bool rightward = to > from;
    const i64 run = rightward ? to - from : from - to;
    const i64 step = rightward ? 1 : -1;
    const i64 exit_side = rightward ? one : 0;
    const i64 entry_side = rightward ? 0 : one;
    // To the edge of the first cell.
    const i64 first_run = rightward ? one - from_fraction : from_fraction;
    const i64 first_product = height * first_run;
    i64 total = first_product / run;
    i64 remainder = first_product - total * run;
    add_to_cell(work, row, first_cell, from_fraction, exit_side, sign * total);
    i64 cell = first_cell + step;
    if (cell != last_cell) {
        // Whole cells: the same share each, with the remainder carried.
        const i64 whole_product = height * one;
        const i64 share = whole_product / run;
        const i64 share_remainder = whole_product - share * run;
        while (cell != last_cell) {
            i64 piece = share;
            remainder = remainder + share_remainder;
            if (remainder >= run) {
                remainder = remainder - run;
                piece = piece + 1;
            }
            add_to_cell(work, row, cell, entry_side, exit_side, sign * piece);
            total = total + piece;
            cell = cell + step;
        }
    }
    add_to_cell(work, row, last_cell, entry_side, to_fraction, sign * (height - total));
}

// A chord, in mask coordinates (y downward).
void add_chord(const Work& work, const Fixed begin, const Fixed end) noexcept {
    if (begin.y == end.y) return;
    // Downward, with the sign remembered: a chord gives the same pieces
    // whichever way it runs.
    Fixed upper = begin;
    Fixed lower = end;
    i64 sign = 1;
    if (begin.y > end.y) {
        upper = end;
        lower = begin;
        sign = -1;
    }
    const i64 dy = lower.y - upper.y;
    const i64 dx = lower.x - upper.x;
    const i64 first_row = upper.y >> fraction_bits;
    const i64 last_row = (lower.y - 1) >> fraction_bits;
    if (first_row == last_row) {
        add_to_row(work, first_row, upper.x, lower.x, sign * dy);
        return;
    }
    // The first row, down to its lower boundary.
    const i64 first_boundary = (first_row + 1) << fraction_bits;
    const i64 first_height = first_boundary - upper.y;
    const i64 first_product = first_height * dx;
    const i64 first_quotient = floor_divide(first_product, dy);
    i64 remainder = first_product - first_quotient * dy;
    i64 crossing = upper.x + first_quotient;
    add_to_row(work, first_row, upper.x, crossing, sign * first_height);
    // Whole rows: the x advances by the same amount, the remainder carried,
    // so each crossing is the exact x rounded down.
    const i64 row_product = one * dx;
    const i64 row_step = floor_divide(row_product, dy);
    const i64 row_remainder = row_product - row_step * dy;
    for (i64 row = first_row + 1; row < last_row; ++row) {
        i64 next = crossing + row_step;
        remainder = remainder + row_remainder;
        if (remainder >= dy) {
            remainder = remainder - dy;
            next = next + 1;
        }
        add_to_row(work, row, crossing, next, sign * one);
        crossing = next;
    }
    add_to_row(work, last_row, crossing, lower.x, sign * (lower.y - (last_row << fraction_bits)));
}

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

// A chord between two points of the frame, added in mask coordinates (16.16,
// y downward).
void add_frame_chord(const Work& work, const Fixed begin, const Fixed end) noexcept {
    const Fixed from{.x = begin.x << 8, .y = work.height - (begin.y << 8)};
    const Fixed to{.x = end.x << 8, .y = work.height - (end.y << 8)};
    add_chord(work, from, to);
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
void add_quadratic(const Work& work, const Fixed start, const Fixed control, const Fixed end) noexcept {
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
    for (i64 step = 1; step < count; ++step) {
        // P0 + 2 t (P1 - P0) + t^2 (P0 - 2 P1 + P2) at t = step / count:
        // the numerator over count^2, rounded down.
        const Fixed point{.x = start.x + ((2 * step * count * bx + step * step * ax) >> (2 * shift)),
                          .y = start.y + ((2 * step * count * by + step * step * ay) >> (2 * shift))};
        add_frame_chord(work, previous, point);
        previous = point;
    }
    add_frame_chord(work, previous, end);
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
void add_cubic(const Work& work, const Fixed start, const Fixed first, const Fixed second, const Fixed end) noexcept {
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
            add_frame_chord(work, current, a[0]);
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

// How many points a segment takes.
[[nodiscard]] usize points_of(const Segment segment) noexcept {
    if (segment == Segment::quadratic) return 2;
    if (segment == Segment::cubic) return 3;
    return 1;
}

[[nodiscard]] Status refuse(const ErrorKind kind, const Operation operation) noexcept {
    const Status result = Status::failure(ErrorRecord::make(kind, operation));
    return result;
}

// The outline's form: known segments, a move first, points for every
// segment and none left over, no more than the limit.
[[nodiscard]] bool well_formed(const Outline& outline, const Limits& limits, bool& too_many) noexcept {
    too_many = outline.points.count() > limits.points;
    if (too_many) return false;
    const usize segment_count = outline.segments.count();
    const Segment* const segments = outline.segments.data();
    usize needed = 0;
    for (usize index = 0; index < segment_count; ++index) {
        const Segment segment = segments[index];
        if (segment != Segment::move && segment != Segment::line && segment != Segment::quadratic &&
            segment != Segment::cubic) {
            return false;
        }
        if (index == 0 && segment != Segment::move) return false;
        needed = needed + points_of(segment);
    }
    const bool result = needed == outline.points.count();
    return result;
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
    const Point* const points = outline.points.data();
    i64 x_low = static_cast<i64>(points[0].x) + offset.x;
    i64 x_high = x_low;
    i64 y_low = static_cast<i64>(points[0].y) + offset.y;
    i64 y_high = y_low;
    for (usize index = 1; index < count; ++index) {
        const i64 x = static_cast<i64>(points[index].x) + offset.x;
        const i64 y = static_cast<i64>(points[index].y) + offset.y;
        if (x < x_low) x_low = x;
        if (x > x_high) x_high = x;
        if (y < y_low) y_low = y;
        if (y > y_high) y_high = y;
    }
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
    // Every point inside the box, checked once, before any arithmetic that
    // relies on it.
    const i64 box_left = static_cast<i64>(bounds.left) * 64;
    const i64 box_right = box_left + static_cast<i64>(width) * 64;
    const i64 box_top = static_cast<i64>(bounds.top) * 64;
    const i64 box_bottom = box_top - static_cast<i64>(height) * 64;
    const usize point_count = outline.points.count();
    const Point* const points = outline.points.data();
    for (usize index = 0; index < point_count; ++index) {
        const i64 x = static_cast<i64>(points[index].x) + offset.x;
        const i64 y = static_cast<i64>(points[index].y) + offset.y;
        if (x < box_left || x > box_right || y < box_bottom || y > box_top) return refuse(ErrorKind::invalid_argument, operation);
    }
    const usize stride = width + 2;
    const usize cell_count = stride * height;
    if (cell_count > limits.work_cells) return refuse(ErrorKind::limit_exceeded, operation);
    if (cells_.count() < cell_count) {
        if (cells_.resize(cell_count).failed()) return refuse(ErrorKind::out_of_memory, operation);
    }
    i64* const cells = cells_.mutable_view().data();
    for (usize index = 0; index < cell_count; ++index) cells[index] = 0;
    const Work work{.cells = cells,
                    .stride = stride,
                    .left = box_left,
                    .bottom = box_bottom,
                    .height = static_cast<i64>(height) << fraction_bits};

    // The chords.
    const usize segment_count = outline.segments.count();
    const Segment* const segments = outline.segments.data();
    usize next_point = 0;
    Fixed contour_start{};
    Fixed current{};
    bool open = false;
    for (usize index = 0; index < segment_count; ++index) {
        const Segment segment = segments[index];
        if (segment == Segment::move) {
            if (open) add_frame_chord(work, current, contour_start);
            contour_start = to_frame(work, points[next_point], offset);
            current = contour_start;
            open = true;
            next_point = next_point + 1;
        } else if (segment == Segment::line) {
            const Fixed end = to_frame(work, points[next_point], offset);
            add_frame_chord(work, current, end);
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
    if (open) add_frame_chord(work, current, contour_start);

    // The mask: a running sum along each row, its magnitude in 256ths of a
    // pixel, full coverage as 255.
    u8* const out = mask.data();
    for (usize row = 0; row < height; ++row) {
        const i64* const row_cells = cells + row * stride;
        u8* const row_out = out + row * width;
        i64 sum = 0;
        for (usize x = 0; x < width; ++x) {
            sum = sum + row_cells[x];
            const i64 size = sum < 0 ? -sum : sum;
            const i64 value = size >> value_shift;
            row_out[x] = static_cast<u8>(value > 255 ? 255 : value);
        }
    }
    return Status::success();
}

void Rasterizer::release() noexcept { cells_.release(); }

} // namespace stxlib::glyphs
