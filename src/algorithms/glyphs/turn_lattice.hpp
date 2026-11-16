// stxlib::glyphs: a glyph's mask as a smooth surface, and that surface
// turned (include/stxlib/glyph_face.hpp, "Turned text"). The surface is
// CONV's: the interpolant of stx_conv (the owner's library; its line cell,
// src/detail/surface_kernel.hpp compile_cell, for a cell away from any
// end and without the bubble term), converted here to whole numbers. It
// passes through every pixel of the mask and puts no ripple between them
// that the pixels do not have. It is made once for a glyph at a size, and
// serves every angle and every part of a pixel after that.
//
// This is the definition. Everything is whole-number arithmetic, exact
// until the two places that say they round: the same bytes on every host.
//
// 1. THE CELL. A line of values y has a cell between each two neighbours
//    y[i] and y[i+1], a function of the six values y[i-2] .. y[i+3]
//    alone, here a .. f (the cell lies between c and d). It is a quintic
//    given by six controls: the first is c, the sixth is d, and each of
//    the five steps between them is a current. Results are in 14400ths of
//    the values' unit, in which every division below is exact.
//    - The slopes and curvatures at c and d, times twelve:
//        lm = a - 8b + 8d - e      lq = 16d - e - 30c + 16b - a
//        rm = b - 8c + 8e - f      rq = 16e - f - 30d + 16c - b
//    - The raw currents: r0 = 240 lm, r1 = 240 lm + 60 lq,
//      r3 = 240 rm - 60 rq, r4 = 240 rm, and r2 what is left of
//      delta = 14400 (d - c).
//    - Room. With x+ for x where it is above 0 (else 0) and x- for -x
//      where it is below 0 (else 0), and p = 14400 (c - b),
//      n = 14400 (e - d): up[j] is the least of p+, r0+ .. rj+ added to the
//      least of n+, r4+ .. rj+; down[j] the same with - for +. A current
//      has room only in the direction its neighbours on both sides go.
//      base_up = delta+ + min(sum of down, 4 delta+) and
//      base_down = delta- + min(sum of up, 4 delta-).
//    - Each raw current is held within [-(down[j] + base_down),
//      up[j] + base_up]. What the five then sum to is brought back to
//      delta by taking the same amount t from each (or giving it to each),
//      no current past its bound: t is the level at which the amounts,
//      each the lesser of t and the current's distance from its bound, sum
//      to what is over. (The largest of m / 5, (m - c1) / 4,
//      (m - c1 - c2) / 3, (m - c1 - c2 - c3) / 2 and m - c1 - c2 - c3 - c4,
//      for those distances in rising order c1 .. c5 and m what is over.)
//      The distances always suffice, so the five currents sum to delta
//      exactly and a cell's sixth control is the next cell's first.
//
// 2. THE LATTICE of a mask of w by h pixels. The mask stands in nothing:
//    every pixel beyond it is 0. Cells along a row lie between the pixel
//    k - 1 and the pixel k for k = 0 .. w, and down a column the same for
//    0 .. h: the mask and half a pixel round it. Beyond that every cell is
//    nothing, so this is all of the surface.
//    - Along each row of the mask, rule 1 on its pixels (levels 0 to 255)
//      gives 5 (w + 1) + 1 values: each cell's first five controls, and
//      a last 0 (the sixth control of the last cell).
//    - Down each of those 5 (w + 1) + 1 columns, rule 1 on the values the
//      rows gave (rows beyond the mask being 0) gives 5 (h + 1) + 1
//      controls, now in 14400ths of 14400ths of a level.
//    - A control is kept as a whole number of 32nds of a level: divided by
//      6480000 and ROUNDED to the nearest, halves away from zero, and held
//      within -32768 to 32767 (within about 1024 levels either way; on the
//      bundled fonts controls run from about -110 to 350 levels).
//    Controls run below nothing and above full between pixels where the
//    pixels' own slopes call for it; a turned pixel is held to 0 .. 255.
//
// 3. THE TURN. For a turn of cosine c and sine s (16.16, counter-clockwise
//    as the text is seen) and a shift (26.6, y upward), the turned mask's
//    box is every pixel whose centre is inside the box of the lattice's
//    rectangle (the upright mask's box and half a pixel round it) taken
//    through the turn and the shift: a corner (x, y) in 64ths goes to
//    (x c - y s, x s + y c) / 65536 plus the shift, exactly, and a centre
//    on the box's edge is not inside. For the pixel of that box in row r
//    and column k, with x6 and y6 its centre less the shift in 64ths:
//        u = ((x6 c + y6 s) >> 6) - (65536 L - 32768) + 128
//        v = (65536 T + 32768) - ((y6 c - x6 s) >> 6) + 128
//    (L and T the upright mask's left and top; >> rounds down). u and v
//    are where the centre is in the lattice, in 65536ths of a cell from
//    its first row and column of controls, the 128 so that the 256th of a
//    cell it falls in is the nearest. A pixel whose u is not in
//    [0, 65536 (w + 1)) or whose v is not in [0, 65536 (h + 1)) is 0.
//    Otherwise the cell is (u >> 16, v >> 16) and the place within it
//    p = (u >> 8) & 255 and q = (v >> 8) & 255, and with C the cell's six
//    by six controls and W the weights of rule 4:
//        row[a] = (sum over b of W[p][b] C[a][b] + 8192) >> 14
//        level  = (sum over a of W[q][a] row[a] + 262144) >> 19
//    held to 0 .. 255. At no turn and a shift of whole pixels u and v fall
//    on controls that are the mask's own pixels, and the turned mask is
//    the upright one, box and bytes.
//
// 4. THE WEIGHTS. W[p][k] is the quintic's k-th weight at p / 256 of the
//    way along a cell, in 16384ths:
//    binomial(5, k) p^k (256 - p)^(5 - k) / 2^26 rounded to the nearest
//    (halves up), and the largest of the six (the first of them, if two
//    are equal) then changed by what makes the six sum to 16384 exactly.
//
// Bounds on the arithmetic. Rule 1 with values within 2^31 either way
// keeps every step within 2^53 (delta is at most 14400 * 2^32, a bound at
// most seven times that, five of them summed). The rows' results are
// within 29 times 255 levels, 2^27 in their unit, so the columns' are
// far inside it. In rule 3 the weights are not negative and sum to 16384,
// so a row's sum is within 16384 * 32768 = 2^29 and so is the level's.
//
// What bounds the work: a lattice is (5 w + 6)(5 h + 6) controls of two
// bytes, about fifty times the mask's bytes, and is made with
// (h + 6)(5 w + 6) whole numbers of working storage; a mask of more than
// lattice_side pixels a side is not given one (the face draws such a
// glyph by its outline).

#ifndef STXLIB_ALGORITHMS_GLYPHS_TURN_LATTICE_HPP
#define STXLIB_ALGORITHMS_GLYPHS_TURN_LATTICE_HPP

#include "stxlib/glyph_mask.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::glyphs::detail {

// A cell's results for one unit of what it is given.
inline constexpr i64 cell_unit = 14400;
// A kept control for one level, and what a column's result is divided by
// to make it.
inline constexpr i64 control_unit = 32;
inline constexpr i64 control_divisor = cell_unit * cell_unit / control_unit;
// The most pixels a side of a mask that is given a lattice.
inline constexpr u32 lattice_side = 256;

[[nodiscard]] constexpr i64 lesser(const i64 a, const i64 b) noexcept {
    const i64 result = b < a ? b : a;
    return result;
}

[[nodiscard]] constexpr i64 greater(const i64 a, const i64 b) noexcept {
    const i64 result = b > a ? b : a;
    return result;
}

// x where it is above 0, and -x where it is below.
[[nodiscard]] constexpr i64 above_zero(const i64 x) noexcept {
    const i64 result = x > 0 ? x : 0;
    return result;
}

[[nodiscard]] constexpr i64 below_zero(const i64 x) noexcept {
    const i64 result = x < 0 ? -x : 0;
    return result;
}

constexpr void in_order(i64& a, i64& b) noexcept {
    const i64 low = lesser(a, b);
    const i64 high = greater(a, b);
    a = low;
    b = high;
}

// The level t of rule 1: the amounts, each the lesser of t and a
// distance, sum to `over`. Every number is a multiple of 60, so the
// divisions are exact.
[[nodiscard]] constexpr i64 level_of(const i64* const distance, const i64 over) noexcept {
    i64 smallest = distance[0];
    for (usize j = 1; j < 5; ++j) smallest = lesser(smallest, distance[j]);
    // No distance is reached: the same from each.
    if (over <= 5 * smallest) {
        const i64 even = over / 5;
        return even;
    }
    i64 sorted[5] = {distance[0], distance[1], distance[2], distance[3], distance[4]};
    in_order(sorted[0], sorted[1]);
    in_order(sorted[3], sorted[4]);
    in_order(sorted[2], sorted[4]);
    in_order(sorted[2], sorted[3]);
    in_order(sorted[1], sorted[4]);
    in_order(sorted[0], sorted[3]);
    in_order(sorted[0], sorted[2]);
    in_order(sorted[1], sorted[3]);
    in_order(sorted[1], sorted[2]);
    i64 result = over / 5;
    i64 sum = sorted[0];
    result = greater(result, (over - sum) / 4);
    sum = sum + sorted[1];
    result = greater(result, (over - sum) / 3);
    sum = sum + sorted[2];
    result = greater(result, (over - sum) / 2);
    sum = sum + sorted[3];
    result = greater(result, over - sum);
    return result;
}

// Rule 1: the first five controls of the cell between c and d, in
// 14400ths of the values' unit. The values are within 2^31 either way.
constexpr void lattice_cell(const i64 a, const i64 b, const i64 c, const i64 d, const i64 e, const i64 f, i64* const controls) noexcept {
    const i64 left_m = a - 8 * b + 8 * d - e;
    const i64 left_q = 16 * d - e - 30 * c + 16 * b - a;
    const i64 right_m = b - 8 * c + 8 * e - f;
    const i64 right_q = 16 * e - f - 30 * d + 16 * c - b;
    const i64 delta = cell_unit * (d - c);
    i64 raw[5] = {240 * left_m, 240 * left_m + 60 * left_q, 0, 240 * right_m - 60 * right_q, 240 * right_m};
    raw[2] = delta - raw[0] - raw[1] - raw[3] - raw[4];
    // Room, from the neighbours on each side inward.
    const i64 previous = cell_unit * (c - b);
    const i64 next = cell_unit * (e - d);
    i64 room_up[5] = {0, 0, 0, 0, 0};
    i64 room_down[5] = {0, 0, 0, 0, 0};
    i64 head_up = above_zero(previous);
    i64 head_down = below_zero(previous);
    for (usize j = 0; j < 5; ++j) {
        head_up = lesser(head_up, above_zero(raw[j]));
        head_down = lesser(head_down, below_zero(raw[j]));
        room_up[j] = head_up;
        room_down[j] = head_down;
    }
    i64 tail_up = above_zero(next);
    i64 tail_down = below_zero(next);
    i64 total_up = 0;
    i64 total_down = 0;
    for (usize step = 0; step < 5; ++step) {
        const usize j = 4 - step;
        tail_up = lesser(tail_up, above_zero(raw[j]));
        tail_down = lesser(tail_down, below_zero(raw[j]));
        room_up[j] = room_up[j] + tail_up;
        room_down[j] = room_down[j] + tail_down;
        total_up = total_up + room_up[j];
        total_down = total_down + room_down[j];
    }
    const i64 plus = above_zero(delta);
    const i64 minus = below_zero(delta);
    const i64 base_up = plus + lesser(total_down, 4 * plus);
    const i64 base_down = minus + lesser(total_up, 4 * minus);
    // Held within its bounds, and how far each then is from either.
    i64 held[5] = {0, 0, 0, 0, 0};
    i64 to_low[5] = {0, 0, 0, 0, 0};
    i64 to_high[5] = {0, 0, 0, 0, 0};
    i64 sum = 0;
    for (usize j = 0; j < 5; ++j) {
        const i64 high = room_up[j] + base_up;
        const i64 low = -(room_down[j] + base_down);
        held[j] = lesser(high, greater(low, raw[j]));
        to_low[j] = held[j] - low;
        to_high[j] = high - held[j];
        sum = sum + held[j];
    }
    // The sum brought back to delta.
    const i64 over = sum - delta;
    const bool lowering = over > 0;
    const i64* const distance = lowering ? to_low : to_high;
    const i64 level = level_of(distance, lowering ? over : -over);
    i64 control = cell_unit * c;
    for (usize j = 0; j < 5; ++j) {
        controls[j] = control;
        const i64 amount = lesser(level, distance[j]);
        control = control + (lowering ? held[j] - amount : held[j] + amount);
    }
}

// Rule 4.
inline constexpr usize weight_places = 256;

struct PlaceWeights final {
    i16 of[weight_places][6]{};
    constexpr PlaceWeights() noexcept {
        const i64 binomial[6] = {1, 5, 10, 10, 5, 1};
        for (usize place = 0; place < weight_places; ++place) {
            const i64 p = static_cast<i64>(place);
            const i64 q = static_cast<i64>(weight_places) - p;
            i64 sum = 0;
            usize largest = 0;
            for (usize k = 0; k < 6; ++k) {
                i64 product = binomial[k];
                for (usize power = 0; power < 5; ++power) product = product * (power < k ? p : q);
                const i64 weight = (product + (static_cast<i64>(1) << 25)) >> 26;
                of[place][k] = static_cast<i16>(weight);
                sum = sum + weight;
                largest = of[place][k] > of[place][largest] ? k : largest;
            }
            of[place][largest] = static_cast<i16>(of[place][largest] + (16384 - sum));
        }
    }
};
inline constexpr PlaceWeights place_weights{};

// How many controls a mask's lattice is, along a row and in all.
[[nodiscard]] constexpr usize lattice_stride(const u32 width) noexcept {
    const usize result = 5 * (static_cast<usize>(width) + 1) + 1;
    return result;
}

[[nodiscard]] constexpr usize lattice_controls(const u32 width, const u32 height) noexcept {
    const usize result = lattice_stride(width) * lattice_stride(height);
    return result;
}

// A lattice as it is turned: its controls borrowed, and the upright mask's
// box.
struct TurnLattice final {
    const i16* controls{nullptr};
    Bounds upright{};
};

// Rule 2: the lattice of the mask `pixels` (width * height bytes, the top
// row first; each side from 1 to lattice_side) into `controls`, which is
// resized to lattice_controls(width, height); `rows` is working storage.
// False: out of memory.
[[nodiscard]] bool build_lattice(const u8* pixels, u32 width, u32 height, Vector<i64>& rows, Vector<i16>& controls) noexcept;

// Whether a matrix is a turn and nothing else: xx = yy, xy = -yx, and the
// two squared summing to 1 within what rounding each to a 65536th leaves.
[[nodiscard]] bool is_turn(i32 xx, i32 xy, i32 yx, i32 yy) noexcept;

// Rule 3's box (the upright box not empty). False when it is larger than
// the limits allow or further out than a mask can be.
[[nodiscard]] bool turned_bounds(const Bounds& upright, i32 cosine, i32 sine, Point shift, const Limits& limits, Bounds& turned) noexcept;

// Rule 3: the turned mask into `pixels` (turned.width * turned.height
// bytes, the top row first), every byte written. `turned` is what
// turned_bounds gave for the same lattice, turn and shift, or any box of
// pixels: what of it lies off the lattice is 0.
void turn_lattice(const TurnLattice& lattice, i32 cosine, i32 sine, Point shift, const Bounds& turned, u8* pixels) noexcept;

} // namespace stxlib::glyphs::detail

#endif
