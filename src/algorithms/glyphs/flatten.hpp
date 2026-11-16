// stxlib::glyphs: an outline's chords (src/algorithms/glyphs/mask.cpp states
// the method; chords.hpp is what the chords go to).
//
// Flattening is arranged as the chords are. The segments of an outline are
// taken a block at a time and listed by kind, the quadratics again by the
// number of chords they become, and each list has its own loop: every item
// of a list is the same kind of thing and becomes the same number of
// chords, so nothing is decided per item. add_quadratic is the rule for a
// quadratic in full; the lists of one, two and four chords are the same
// expression at fixed steps (tests/glyph_mask_tests.cpp, M12, compares the
// chords).

#ifndef STXLIB_ALGORITHMS_GLYPHS_FLATTEN_HPP
#define STXLIB_ALGORITHMS_GLYPHS_FLATTEN_HPP

#include "algorithms/glyphs/chords.hpp"
#include "stxlib/glyph_mask.hpp"

namespace stxlib::glyphs::detail {

// The lists count points by a segment's number.
static_assert(static_cast<u32>(Segment::move) == 0 && static_cast<u32>(Segment::line) == 1 &&
              static_cast<u32>(Segment::quadratic) == 2 && static_cast<u32>(Segment::cubic) == 3);

// A quadratic becomes at most 2^16 chords (the second difference of the
// largest curve a mask can hold vanishes sooner), and a cubic is halved at
// most 16 times.
inline constexpr u32 longest_subdivision = 16;
inline constexpr usize cubic_depth = 16;
// The flattening's two measures, in 1/256 of a pixel.
inline constexpr i64 quarter_pixel = 64;
inline constexpr i64 half_pixel = 128;

// Segments are taken a block at a time, and listed: by kind (move, line,
// quadratic, cubic), and the quadratics again by the number of chords
// they become (one, two, four, more). A list holds the number of each
// segment's first point.
inline constexpr usize segment_block = 128;
inline constexpr usize kind_count = 4;
inline constexpr usize quadratic_kinds = 4;
inline constexpr usize list_count = kind_count + quadratic_kinds;
inline constexpr usize lists_words = list_count * segment_block;

// The working rows, the chords in hand, the lists of segments, and the
// frame points are in.
struct Work final {
    Cells cells{};
    Block block{};
    i32* lists{nullptr};
    // What is added to a point (26.6) to measure it from the mask's left
    // and bottom edges: the offset less the box's corner.
    i64 shift_x{0};
    i64 shift_y{0};
    // The mask's height in 1/256 of a pixel.
    i64 height{0};
};

// Curves are flattened in the frame FreeType's smooth rasterizer uses, so
// that the chords are the same ones: 24.8 fixed point (1/256 of a pixel),
// x from the mask's left edge and y upward from its bottom edge. A point of
// the outline is exact there (26.6 times four).
[[nodiscard]] inline Fixed to_frame(const Work& work, const Point point) noexcept {
    const Fixed result{.x = (static_cast<i64>(point.x) + work.shift_x) * 4, .y = (static_cast<i64>(point.y) + work.shift_y) * 4};
    return result;
}

// Room in the block for `count` more chords (no more than block_chords):
// the chords in hand are drawn first if there is not.
inline void make_room(Work& work, const usize count) noexcept {
    if (work.block.count + count > block_chords) add_block(work.cells, work.block);
}

// How many of `left` items, each of `each` chords, the block has room for:
// at least one (the chords in hand are drawn first if need be).
[[nodiscard]] inline usize run_of(Work& work, const usize left, const usize each) noexcept {
    make_room(work, each);
    const usize room = (block_chords - work.block.count) / each;
    const usize result = left < room ? left : room;
    return result;
}

// A chord between two points of the frame, to the block, in mask
// coordinates (y downward). Requirement: room for it.
inline void put_frame_chord(Work& work, const Fixed begin, const Fixed end) noexcept {
    const Fixed from{.x = begin.x, .y = work.height - begin.y};
    const Fixed to{.x = end.x, .y = work.height - end.y};
    put_chord(work.block, from, to);
}

[[nodiscard]] inline i64 larger(const i64 a, const i64 b) noexcept {
    const i64 result = a > b ? a : b;
    return result;
}

// A quadratic: 2^k chords at equal steps of the parameter. k is the number
// of times the second difference (the larger of |P0 - 2 P1 + P2| on the two
// axes, in 1/256 of a pixel) is quartered, rounding down, before it is at
// most 64 (a quarter of a pixel). The chords' ends are the curve's points
// rounded down to 1/256 of a pixel.
inline void add_quadratic(Work& work, const Fixed start, const Fixed control, const Fixed end) noexcept {
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
inline void halve(i64& p0, i64& p1, i64& p2, i64& p3, i64& p4, i64& p5, i64& p6) noexcept {
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
inline void add_cubic(Work& work, const Fixed start, const Fixed first, const Fixed second, const Fixed end) noexcept {
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

// The loops below each take one list: every item in it is the same kind of
// thing and becomes the same number of chords, so nothing is decided per
// item, and room in the block is made for a run of items, not for each.

// Lines: one chord, from the point before the segment's to its own.
inline void add_lines(Work& work, const Point* const points, const i32* const list, const usize count) noexcept {
    usize index = 0;
    while (index < count) {
        const usize stop = index + run_of(work, count - index, 1);
        for (; index < stop; ++index) {
            const usize point = static_cast<usize>(list[index]);
            put_frame_chord(work, to_frame(work, points[point - 1]), to_frame(work, points[point]));
        }
    }
}

// The larger second difference of a quadratic, in 1/256 of a pixel: what
// decides how many chords it becomes (add_quadratic).
[[nodiscard]] inline i64 spread_of(const Point start, const Point control, const Point end) noexcept {
    const i64 x = (static_cast<i64>(start.x) - 2 * static_cast<i64>(control.x) + static_cast<i64>(end.x)) * 4;
    const i64 y = (static_cast<i64>(start.y) - 2 * static_cast<i64>(control.y) + static_cast<i64>(end.y)) * 4;
    const i64 result = larger(magnitude(x), magnitude(y));
    return result;
}

// Quadratics, to the list of the chords they become: one (the spread at
// most 64), two (at most 259: quartered once, rounding down, it is at most
// 64), four (at most 1039), more. Each is written at the end of every
// list, and the one list it belongs to grows: the four counts are then
// four sums, none waiting on a store.
struct Sorted final {
    usize of_one{0};
    usize of_two{0};
    usize of_four{0};
    usize of_more{0};
};

[[nodiscard]] inline Sorted sort_quadratics(const Point* const points, const i32* const list, const usize count, i32* const lists) noexcept {
    usize of_one = 0;
    usize of_two = 0;
    usize of_four = 0;
    usize of_more = 0;
    for (usize index = 0; index < count; ++index) {
        const i32 number = list[index];
        const usize point = static_cast<usize>(number);
        const i64 spread = spread_of(points[point - 1], points[point], points[point + 1]);
        lists[of_one] = number;
        lists[segment_block + of_two] = number;
        lists[2 * segment_block + of_four] = number;
        lists[3 * segment_block + of_more] = number;
        const usize beyond_one = spread > quarter_pixel ? 1U : 0U;
        const usize beyond_two = spread > 4 * quarter_pixel + 3 ? 1U : 0U;
        const usize beyond_four = spread > 16 * quarter_pixel + 15 ? 1U : 0U;
        of_one = of_one + (1U - beyond_one);
        of_two = of_two + (beyond_one - beyond_two);
        of_four = of_four + (beyond_two - beyond_four);
        of_more = of_more + beyond_four;
    }
    const Sorted result{.of_one = of_one, .of_two = of_two, .of_four = of_four, .of_more = of_more};
    return result;
}

// Quadratics of one chord.
inline void add_quadratics_of_one(Work& work, const Point* const points, const i32* const list, const usize count) noexcept {
    usize index = 0;
    while (index < count) {
        const usize stop = index + run_of(work, count - index, 1);
        for (; index < stop; ++index) {
            const usize point = static_cast<usize>(list[index]);
            put_frame_chord(work, to_frame(work, points[point - 1]), to_frame(work, points[point + 1]));
        }
    }
}

// Quadratics of two chords: the curve's point at the middle of its
// parameter, by add_quadratic's expression at step 1 of 2.
inline void add_quadratics_of_two(Work& work, const Point* const points, const i32* const list, const usize count) noexcept {
    usize index = 0;
    while (index < count) {
        const usize stop = index + run_of(work, count - index, 2);
        for (; index < stop; ++index) {
            const usize point = static_cast<usize>(list[index]);
            const Fixed start = to_frame(work, points[point - 1]);
            const Fixed control = to_frame(work, points[point]);
            const Fixed end = to_frame(work, points[point + 1]);
            const i64 bx = control.x - start.x;
            const i64 by = control.y - start.y;
            const i64 ax = end.x - control.x - bx;
            const i64 ay = end.y - control.y - by;
            const Fixed middle{.x = start.x + ((4 * bx + ax) >> 2), .y = start.y + ((4 * by + ay) >> 2)};
            put_frame_chord(work, start, middle);
            put_frame_chord(work, middle, end);
        }
    }
}

// Quadratics of four chords: the curve's points at steps 1, 2 and 3 of 4.
inline void add_quadratics_of_four(Work& work, const Point* const points, const i32* const list, const usize count) noexcept {
    usize index = 0;
    while (index < count) {
        const usize stop = index + run_of(work, count - index, 4);
        for (; index < stop; ++index) {
            const usize point = static_cast<usize>(list[index]);
            const Fixed start = to_frame(work, points[point - 1]);
            const Fixed control = to_frame(work, points[point]);
            const Fixed end = to_frame(work, points[point + 1]);
            const i64 bx = control.x - start.x;
            const i64 by = control.y - start.y;
            const i64 ax = end.x - control.x - bx;
            const i64 ay = end.y - control.y - by;
            const Fixed first{.x = start.x + ((8 * bx + ax) >> 4), .y = start.y + ((8 * by + ay) >> 4)};
            const Fixed second{.x = start.x + ((16 * bx + 4 * ax) >> 4), .y = start.y + ((16 * by + 4 * ay) >> 4)};
            const Fixed third{.x = start.x + ((24 * bx + 9 * ax) >> 4), .y = start.y + ((24 * by + 9 * ay) >> 4)};
            put_frame_chord(work, start, first);
            put_frame_chord(work, first, second);
            put_frame_chord(work, second, third);
            put_frame_chord(work, third, end);
        }
    }
}

// Every chord of the outline into the cells. Requirement: the outline is
// well formed and every point lies in the mask's box.
inline void accumulate(Work& work, const Outline& outline) noexcept {
    const usize segment_count = outline.segments.count();
    const Segment* const segments = outline.segments.data();
    const Point* const points = outline.points.data();
    i32* const lists = work.lists;
    // The next segment's first point, and the open contour's.
    usize point = 0;
    usize contour = 0;
    for (usize first = 0; first < segment_count; first = first + segment_block) {
        const usize count = segment_count - first < segment_block ? segment_count - first : segment_block;
        // By kind. A segment's number is the points it takes, but for a
        // move, which takes one. Each segment is written at the end of
        // every list, and the one list it belongs to grows.
        usize moves = 0;
        usize lines = 0;
        usize quadratic_count = 0;
        usize cubics = 0;
        for (usize index = 0; index < count; ++index) {
            const usize kind = static_cast<usize>(segments[first + index]);
            const i32 number = static_cast<i32>(point);
            lists[moves] = number;
            lists[segment_block + lines] = number;
            lists[2 * segment_block + quadratic_count] = number;
            lists[3 * segment_block + cubics] = number;
            moves = moves + (kind == 0 ? 1U : 0U);
            lines = lines + (kind == 1 ? 1U : 0U);
            quadratic_count = quadratic_count + (kind == 2 ? 1U : 0U);
            cubics = cubics + (kind == 3 ? 1U : 0U);
            point = point + kind + (kind == 0 ? 1U : 0U);
        }
        // Moves: each closes the contour before it, from that contour's
        // last point to its first. The outline's first move closes nothing.
        for (usize index = first == 0 ? 1 : 0; index < moves; ++index) {
            const usize moved = static_cast<usize>(lists[index]);
            make_room(work, 1);
            put_frame_chord(work, to_frame(work, points[moved - 1]), to_frame(work, points[contour]));
            contour = moved;
        }
        add_lines(work, points, lists + segment_block, lines);
        i32* const quadratics = lists + kind_count * segment_block;
        const Sorted sorted = sort_quadratics(points, lists + 2 * segment_block, quadratic_count, quadratics);
        add_quadratics_of_one(work, points, quadratics, sorted.of_one);
        add_quadratics_of_two(work, points, quadratics + segment_block, sorted.of_two);
        add_quadratics_of_four(work, points, quadratics + 2 * segment_block, sorted.of_four);
        for (usize index = 0; index < sorted.of_more; ++index) {
            const usize curve = static_cast<usize>(quadratics[3 * segment_block + index]);
            add_quadratic(work, to_frame(work, points[curve - 1]), to_frame(work, points[curve]), to_frame(work, points[curve + 1]));
        }
        for (usize index = 0; index < cubics; ++index) {
            const usize curve = static_cast<usize>(lists[3 * segment_block + index]);
            add_cubic(work, to_frame(work, points[curve - 1]), to_frame(work, points[curve]), to_frame(work, points[curve + 1]),
                      to_frame(work, points[curve + 2]));
        }
    }
    if (segment_count != 0) {
        make_room(work, 1);
        put_frame_chord(work, to_frame(work, points[point - 1]), to_frame(work, points[contour]));
    }
    add_block(work.cells, work.block);
}

} // namespace stxlib::glyphs::detail

#endif
