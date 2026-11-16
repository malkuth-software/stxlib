// stxlib::glyphs: chords into the cells (src/algorithms/glyphs/mask.cpp
// states the method and what is exact; this is the arithmetic of chords).
//
// add_any_chord is the arithmetic in full: a chord is cut at every row and
// every cell it crosses, with loops and decisions. It is the reference, and
// it draws the chords that have no form of their own.
//
// Most chords of text are short, and which rows and cells one touches
// cannot be told from the one before it: deciding that per chord, inside
// the loop over chords, is what costs. So the chords of an outline are
// first written to a block (put_chord), each to the list of its form, with
// no decision:
//     in one cell;
//     across two or three cells of one row;
//     down two, three, four or five rows of one column;
//     over two, three or four rows and two columns;
//     any other.
// Then each list is drawn by its own loop (add_block). A form's loop does
// the same fixed work for every chord in it: all but the last have no
// decision in them at all, and give every cell the integers add_any_chord
// gives (tests/glyph_mask_tests.cpp, M11, compares them cell for cell).

#ifndef STXLIB_ALGORITHMS_GLYPHS_CHORDS_HPP
#define STXLIB_ALGORITHMS_GLYPHS_CHORDS_HPP

#include "algorithms/glyphs/reciprocal.hpp"
#include "stxlib/types.hpp"

namespace stxlib::glyphs::detail {

// Mask coordinates: 16.16 fixed point.
inline constexpr u32 fraction_bits = 16;
inline constexpr i64 one = static_cast<i64>(1) << fraction_bits;
// Chords come in 1/256 of a pixel.
inline constexpr u32 frame_bits = 8;
inline constexpr i64 frame_one = static_cast<i64>(1) << frame_bits;
inline constexpr i64 frame_fraction = frame_one - 1;
inline constexpr i64 frame_to_fixed = static_cast<i64>(1) << (fraction_bits - frame_bits);
// The same two units, for stating bounds.
inline constexpr u64 frame_unit = static_cast<u64>(1) << frame_bits;
inline constexpr u64 fixed_unit = static_cast<u64>(1) << fraction_bits;
// A row has the mask's width in cells and two more: a chord on the right
// edge is in the cell after the last, and passes (nothing) to its right.
inline constexpr usize spare_cells = 2;

struct Fixed final {
    i64 x{0};
    i64 y{0};
};

// The working rows: `stride` cells a row.
struct Cells final {
    i64* cells{nullptr};
    usize stride{0};
};

[[nodiscard]] constexpr i64 magnitude(const i64 value) noexcept {
    const i64 result = value < 0 ? -value : value;
    return result;
}

// ---- any chord -----------------------------------------------------------------------------

// A piece inside one cell, `at`: from fraction `enters` to fraction
// `leaves` (each 0 to one), of signed height `cover`.
inline void add_to_cell(i64* const at, const i64 enters, const i64 leaves, const i64 cover) noexcept {
    const i64 doubled_left = (enters + leaves) * cover;
    at[0] = at[0] + (cover * 2 * one - doubled_left);
    at[1] = at[1] + doubled_left;
}

// What the pieces of one chord share: its height and width in 1/256 of a
// pixel, the reciprocal of the width (or 0), and its sign.
struct Slope final {
    i64 rise{0};
    i64 run{0};
    u64 run_reciprocal{0};
    i64 sign{0};
};

// A piece inside one row, whose cells begin at `row`: between x `from` and
// x `to` (16.16), of height `height` (16.16, at most one). Which way it
// runs does not matter: it is taken from its left end to its right.
inline void add_piece(i64* const row, const i64 from, const i64 to, const i64 height, const Slope& slope) noexcept {
    const i64 low = from < to ? from : to;
    const i64 high = from < to ? to : from;
    const i64 first_cell = low >> fraction_bits;
    const i64 last_cell = high >> fraction_bits;
    const i64 low_fraction = low - first_cell * one;
    const i64 high_fraction = high - last_cell * one;
    if (first_cell == last_cell) {
        add_to_cell(row + first_cell, low_fraction, high_fraction, slope.sign * height);
        return;
    }
    // The piece crosses cells (so the chord's width is not 0). The height
    // it has to the left of each cell edge is a running value, so the
    // cells' heights add up to the piece's.
    const i64 first_product = (one - low_fraction) * slope.rise;
    i64 exact = quotient(first_product, slope.run, slope.run_reciprocal);
    i64 remainder = first_product - exact * slope.run;
    i64 reached = exact < height ? exact : height;
    add_to_cell(row + first_cell, low_fraction, one, slope.sign * reached);
    if (first_cell + 1 < last_cell) {
        // Whole cells: the same height each, the remainder carried.
        const i64 whole_product = one * slope.rise;
        const i64 share = quotient(whole_product, slope.run, slope.run_reciprocal);
        const i64 share_remainder = whole_product - share * slope.run;
        for (i64 cell = first_cell + 1; cell < last_cell; ++cell) {
            exact = exact + share;
            remainder = remainder + share_remainder;
            if (remainder >= slope.run) {
                remainder = remainder - slope.run;
                exact = exact + 1;
            }
            const i64 next = exact < height ? exact : height;
            add_to_cell(row + cell, 0, one, slope.sign * (next - reached));
            reached = next;
        }
    }
    add_to_cell(row + last_cell, 0, high_fraction, slope.sign * (height - reached));
}

// Any chord, from its upper end to its lower (1/256 of a pixel, x from the
// mask's left edge, y downward from its top edge), counted with `sign`.
// Requirement: upper.y < lower.y, and both ends in the mask's box.
inline void add_any_chord(const Cells& cells, const Fixed upper, const Fixed lower, const i64 sign) noexcept {
    const i64 dx = lower.x - upper.x;
    const i64 rise = lower.y - upper.y;
    const i64 run = magnitude(dx);
    const i64 toward = dx < 0 ? -1 : 1;
    const i64 first_row = upper.y >> frame_bits;
    const i64 last_row = (lower.y - 1) >> frame_bits;
    // Short both ways: its divisions are multiplications.
    const bool brief = (rise | run) < brief_extent;
    const u64 rise_reciprocal = reciprocals.of[brief ? static_cast<usize>(rise) : 0];
    const u64 run_reciprocal = reciprocals.of[brief ? static_cast<usize>(run) : 0];
    // Where the chord crosses the lower boundary of each of its rows but
    // the last: `crossed` to the side it runs to, from its upper end, in
    // 1/65536. The exact x is rounded down, so a chord running left has
    // its distance rounded up. From row to row the distance grows by the
    // same amount, the remainder carried.
    i64 first_height = rise;
    i64 last_height = rise;
    i64 crossed = 0;
    i64 remainder = 0;
    i64 step = 0;
    i64 step_remainder = 0;
    if (last_row > first_row) {
        first_height = (first_row + 1) * frame_one - upper.y;
        last_height = lower.y - last_row * frame_one;
        const i64 first_product = first_height * run * frame_one + (dx < 0 ? rise - 1 : 0);
        crossed = quotient(first_product, rise, rise_reciprocal);
        remainder = first_product - crossed * rise;
        const i64 row_product = run * one;
        step = quotient(row_product, rise, rise_reciprocal);
        step_remainder = row_product - step * rise;
    }
    const i64 upper_x = upper.x * frame_to_fixed;
    const i64 lower_x = lower.x * frame_to_fixed;
    i64* row = cells.cells + static_cast<usize>(first_row) * cells.stride;
    i64 from = upper_x;
    const i64 cell = upper.x >> frame_bits;
    if (cell == (lower.x >> frame_bits)) {
        // The chord stays in one column of cells: every piece is inside
        // one cell, the same one of each row.
        i64* at = row + cell;
        const i64 doubled_edge = cell * 2 * one;
        i64 cover = sign * first_height * frame_to_fixed;
        for (i64 index = first_row; index < last_row; ++index) {
            const i64 to = upper_x + toward * crossed;
            const i64 doubled_left = (from + to - doubled_edge) * cover;
            at[0] = at[0] + (cover * 2 * one - doubled_left);
            at[1] = at[1] + doubled_left;
            at = at + cells.stride;
            from = to;
            cover = sign * one;
            crossed = crossed + step;
            remainder = remainder + step_remainder;
            if (remainder >= rise) {
                remainder = remainder - rise;
                crossed = crossed + 1;
            }
        }
        const i64 last_cover = sign * last_height * frame_to_fixed;
        const i64 doubled_left = (from + lower_x - doubled_edge) * last_cover;
        at[0] = at[0] + (last_cover * 2 * one - doubled_left);
        at[1] = at[1] + doubled_left;
        return;
    }
    const Slope slope{.rise = rise, .run = run, .run_reciprocal = run_reciprocal, .sign = sign};
    i64 height = first_height * frame_to_fixed;
    for (i64 index = first_row; index < last_row; ++index) {
        const i64 to = upper_x + toward * crossed;
        add_piece(row, from, to, height, slope);
        row = row + cells.stride;
        from = to;
        height = one;
        crossed = crossed + step;
        remainder = remainder + step_remainder;
        if (remainder >= rise) {
            remainder = remainder - rise;
            crossed = crossed + 1;
        }
    }
    add_piece(row, from, lower_x, last_height * frame_to_fixed, slope);
}

// Any chord, but only the rows of a band: rows `first` to `first + count`
// of the mask, into cells whose first row is row `first`. Each row gets
// the piece add_any_chord gives it: the same crossings (the exact x
// rounded down, found for the band's first row by a division and carried
// from there) and the same heights.
inline void add_chord_in_rows(const Cells& cells, const Fixed upper, const Fixed lower, const i64 sign, const i64 first,
                              const i64 count) noexcept {
    const i64 dx = lower.x - upper.x;
    const i64 rise = lower.y - upper.y;
    const i64 run = magnitude(dx);
    const i64 toward = dx < 0 ? -1 : 1;
    const i64 first_row = upper.y >> frame_bits;
    const i64 last_row = (lower.y - 1) >> frame_bits;
    const i64 from_row = first_row > first ? first_row : first;
    const i64 to_row = last_row < first + count - 1 ? last_row : first + count - 1;
    if (from_row > to_row) return;
    const Slope slope{.rise = rise, .run = run, .run_reciprocal = 0, .sign = sign};
    const i64 upper_x = upper.x * frame_to_fixed;
    const i64 lower_x = lower.x * frame_to_fixed;
    const i64 bias = dx < 0 ? rise - 1 : 0;
    // The crossing of the lower boundary of from_row, and of the boundary
    // above it when the chord begins higher.
    const i64 row_product = run * one;
    const i64 step = row_product / rise;
    const i64 step_remainder = row_product - step * rise;
    const i64 product = ((from_row + 1) * frame_one - upper.y) * run * frame_one + bias;
    i64 crossed = product / rise;
    i64 remainder = product - crossed * rise;
    i64 from = upper_x;
    if (from_row > first_row) from = upper_x + toward * ((product - row_product) / rise);
    i64* row = cells.cells + static_cast<usize>(from_row - first) * cells.stride;
    for (i64 index = from_row; index <= to_row; ++index) {
        const i64 top = index * frame_one > upper.y ? index * frame_one : upper.y;
        const i64 bottom = (index + 1) * frame_one < lower.y ? (index + 1) * frame_one : lower.y;
        const i64 to = index == last_row ? lower_x : upper_x + toward * crossed;
        add_piece(row, from, to, (bottom - top) * frame_to_fixed, slope);
        row = row + cells.stride;
        from = to;
        crossed = crossed + step;
        remainder = remainder + step_remainder;
        if (remainder >= rise) {
            remainder = remainder - rise;
            crossed = crossed + 1;
        }
    }
}

// ---- the block -----------------------------------------------------------------------------

// The forms. A chord's form is read from a table by the rows it touches
// (one to five, or more) and the columns between its ends (one to three,
// or more).
inline constexpr usize form_in_a_cell = 0;
inline constexpr usize form_across_two_cells = 1;
inline constexpr usize form_down_two_rows = 2;
inline constexpr usize form_two_rows_two_columns = 3;
inline constexpr usize form_down_three_rows = 4;
inline constexpr usize form_down_four_rows = 5;
inline constexpr usize form_down_five_rows = 6;
inline constexpr usize form_three_rows_two_columns = 7;
inline constexpr usize form_four_rows_two_columns = 8;
inline constexpr usize form_across_three_cells = 9;
inline constexpr usize form_other = 10;
inline constexpr usize form_count = 11;
// A chord along a row covers nothing: its "form" has a list one chord
// long that is never drawn.
inline constexpr usize form_level = form_count;

inline constexpr usize form_rows = 6;
inline constexpr usize form_columns = 4;
inline constexpr u8 form_table[form_rows * form_columns] = {
    form_in_a_cell,       form_across_two_cells,       form_across_three_cells, form_other,
    form_down_two_rows,   form_two_rows_two_columns,   form_other,              form_other,
    form_down_three_rows, form_three_rows_two_columns, form_other,              form_other,
    form_down_four_rows,  form_four_rows_two_columns,  form_other,              form_other,
    form_down_five_rows,  form_other,                  form_other,              form_other,
    form_other,           form_other,                  form_other,              form_other,
};

// Chords in hand: a list for each form, and one more for what is not kept.
// A chord is four words (begin x, begin y, end x, end y), as it came. All
// in `words`, block_words of them, which the caller owns; nothing in it
// needs to be cleared.
inline constexpr usize block_chords = 128;
inline constexpr usize chord_words = 4;
inline constexpr usize list_words = block_chords * chord_words;
inline constexpr usize block_words = form_count * list_words + chord_words;

struct Block final {
    i32* words{nullptr};
    // Chords kept, in all; and in each list (the last is not kept).
    usize count{0};
    usize listed[form_count + 1]{};
};

// One more chord, between two points (1/256 of a pixel, x from the mask's
// left edge, y downward from its top edge, both in the mask's box).
// Requirement: block.count < block_chords.
//
// The chord goes to the end of the list of its form. Nothing is decided.
inline void put_chord(Block& block, const Fixed begin, const Fixed end) noexcept {
    const i64 low_y = begin.y < end.y ? begin.y : end.y;
    const i64 high_y = begin.y < end.y ? end.y : begin.y;
    const bool level = begin.y == end.y;
    // Rows and columns beyond the first, no more than the table has (a
    // level chord's are not used, and read as the most).
    const u64 rows = static_cast<u64>(((high_y - 1) >> frame_bits) - (low_y >> frame_bits));
    const u64 columns = static_cast<u64>(magnitude((end.x >> frame_bits) - (begin.x >> frame_bits)));
    const usize row = static_cast<usize>(rows < form_rows - 1 ? rows : form_rows - 1);
    const usize column = static_cast<usize>(columns < form_columns - 1 ? columns : form_columns - 1);
    const usize shaped = form_table[row * form_columns + column];
    const usize form = level ? form_level : shaped;
    const usize kept = level ? 0 : 1;
    const usize listed = block.listed[form];
    i32* const at = block.words + form * list_words + listed * chord_words;
    at[0] = static_cast<i32>(begin.x);
    at[1] = static_cast<i32>(begin.y);
    at[2] = static_cast<i32>(end.x);
    at[3] = static_cast<i32>(end.y);
    block.listed[form] = listed + kept;
    block.count = block.count + kept;
}

// Chords in one cell. The piece is the chord, whichever way it runs.
inline void add_chords_in_a_cell(const Cells& cells, const i32* const list, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        const i32* const chord = list + index * chord_words;
        const i64 begin_x = chord[0];
        const i64 begin_y = chord[1];
        const i64 end_x = chord[2];
        const i64 end_y = chord[3];
        const i64 upper_y = begin_y < end_y ? begin_y : end_y;
        const i64 cover = (end_y - begin_y) * frame_to_fixed;
        const i64 fractions = ((begin_x & frame_fraction) + (end_x & frame_fraction)) * frame_to_fixed;
        const i64 area = fractions * cover;
        i64* const at = cells.cells + static_cast<usize>(upper_y >> frame_bits) * cells.stride + static_cast<usize>(begin_x >> frame_bits);
        at[0] = at[0] + (cover * 2 * one - area);
        at[1] = at[1] + area;
    }
}

// Chords across two or three cells of one row. The height to the left of a
// cell edge is the distance from the chord's left end to that edge times
// its slope, rounded down (it cannot pass the chord's height: the right
// end is at or beyond the last edge).
static_assert(reciprocals_exact(2 * fixed_unit * frame_unit, 3 * frame_unit));

inline void add_chords_across_two_cells(const Cells& cells, const i32* const list, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        const i32* const chord = list + index * chord_words;
        const i64 begin_x = chord[0];
        const i64 begin_y = chord[1];
        const i64 end_x = chord[2];
        const i64 end_y = chord[3];
        const i64 upper_y = begin_y < end_y ? begin_y : end_y;
        const i64 sign = begin_y < end_y ? 1 : -1;
        const i64 rise = magnitude(end_y - begin_y);
        const i64 low = begin_x < end_x ? begin_x : end_x;
        const i64 high = begin_x < end_x ? end_x : begin_x;
        const i64 low_fraction = (low & frame_fraction) * frame_to_fixed;
        const i64 high_fraction = (high & frame_fraction) * frame_to_fixed;
        const u64 product = static_cast<u64>((one - low_fraction) * rise);
        const i64 left_height = static_cast<i64>((product * reciprocals.of[high - low]) >> reciprocal_bits);
        const i64 left_cover = sign * left_height;
        const i64 right_cover = sign * (rise * frame_to_fixed - left_height);
        const i64 left_area = (low_fraction + one) * left_cover;
        const i64 right_area = high_fraction * right_cover;
        i64* const at = cells.cells + static_cast<usize>(upper_y >> frame_bits) * cells.stride + static_cast<usize>(low >> frame_bits);
        at[0] = at[0] + (left_cover * 2 * one - left_area);
        at[1] = at[1] + (left_area + right_cover * 2 * one - right_area);
        at[2] = at[2] + right_area;
    }
}

inline void add_chords_across_three_cells(const Cells& cells, const i32* const list, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        const i32* const chord = list + index * chord_words;
        const i64 begin_x = chord[0];
        const i64 begin_y = chord[1];
        const i64 end_x = chord[2];
        const i64 end_y = chord[3];
        const i64 upper_y = begin_y < end_y ? begin_y : end_y;
        const i64 sign = begin_y < end_y ? 1 : -1;
        const i64 rise = magnitude(end_y - begin_y);
        const i64 low = begin_x < end_x ? begin_x : end_x;
        const i64 high = begin_x < end_x ? end_x : begin_x;
        const i64 low_fraction = (low & frame_fraction) * frame_to_fixed;
        const i64 high_fraction = (high & frame_fraction) * frame_to_fixed;
        const u64 reciprocal = reciprocals.of[high - low];
        const u64 first_product = static_cast<u64>((one - low_fraction) * rise);
        const u64 second_product = static_cast<u64>((2 * one - low_fraction) * rise);
        const i64 first_reached = static_cast<i64>((first_product * reciprocal) >> reciprocal_bits);
        const i64 second_reached = static_cast<i64>((second_product * reciprocal) >> reciprocal_bits);
        const i64 left_cover = sign * first_reached;
        const i64 middle_cover = sign * (second_reached - first_reached);
        const i64 right_cover = sign * (rise * frame_to_fixed - second_reached);
        const i64 left_area = (low_fraction + one) * left_cover;
        const i64 middle_area = one * middle_cover;
        const i64 right_area = high_fraction * right_cover;
        i64* const at = cells.cells + static_cast<usize>(upper_y >> frame_bits) * cells.stride + static_cast<usize>(low >> frame_bits);
        at[0] = at[0] + (left_cover * 2 * one - left_area);
        at[1] = at[1] + (left_area + middle_cover * 2 * one - middle_area);
        at[2] = at[2] + (middle_area + right_cover * 2 * one - right_area);
        at[3] = at[3] + right_area;
    }
}

// A chord from its upper end to its lower, and the sign of the way it ran.
struct Ordered final {
    i64 upper_x{0};
    i64 upper_y{0};
    i64 lower_x{0};
    i64 lower_y{0};
    i64 sign{0};
};

[[nodiscard]] inline Ordered ordered(const i32* const chord) noexcept {
    const bool downward = chord[1] < chord[3];
    const Ordered result{.upper_x = downward ? chord[0] : chord[2],
                         .upper_y = downward ? chord[1] : chord[3],
                         .lower_x = downward ? chord[2] : chord[0],
                         .lower_y = downward ? chord[3] : chord[1],
                         .sign = downward ? 1 : -1};
    return result;
}

// Where a chord crosses the lower boundary of each of its rows but the
// last: `crossed`, the distance from its upper end, to the side it runs
// to, in 1/65536. The exact x is rounded down, so a chord running left has
// its distance rounded up. From row to row the distance grows by the same
// amount, the remainder carried: the same values add_any_chord has.
struct Crossings final {
    i64 crossed{0};
    i64 remainder{0};
    i64 step{0};
    i64 step_remainder{0};
    i64 rise{0};
};

[[nodiscard]] inline Crossings crossings_of(const i64 first_height, const i64 rise, const i64 dx) noexcept {
    const i64 run = magnitude(dx);
    const u64 reciprocal = reciprocals.of[rise];
    const i64 first_product = first_height * run * frame_one + (dx < 0 ? rise - 1 : 0);
    const i64 crossed = static_cast<i64>((static_cast<u64>(first_product) * reciprocal) >> reciprocal_bits);
    const i64 row_product = run * one;
    const i64 step = static_cast<i64>((static_cast<u64>(row_product) * reciprocal) >> reciprocal_bits);
    const Crossings result{.crossed = crossed,
                           .remainder = first_product - crossed * rise,
                           .step = step,
                           .step_remainder = row_product - step * rise,
                           .rise = rise};
    return result;
}

inline void next_crossing(Crossings& crossings) noexcept {
    const i64 remainder = crossings.remainder + crossings.step_remainder;
    const i64 carried = remainder >= crossings.rise ? 1 : 0;
    crossings.crossed = crossings.crossed + crossings.step + carried;
    crossings.remainder = remainder - carried * crossings.rise;
}

// Chords down `Rows` rows of one column: one piece in each row, all in the
// same cell of their rows. The number of rows is the loop's, not the
// chord's.
template <usize Rows>
inline void add_chords_down_rows(const Cells& cells, const i32* const list, const usize count) noexcept {
    // The chord is narrower than a cell and no taller than its rows.
    static_assert(Rows >= 2 && reciprocals_exact(frame_unit * fixed_unit + static_cast<u64>(Rows) * frame_unit,
                                                  static_cast<u64>(Rows) * frame_unit));
    for (usize index = 0; index < count; ++index) {
        const Ordered chord = ordered(list + index * chord_words);
        const i64 rise = chord.lower_y - chord.upper_y;
        const i64 dx = chord.lower_x - chord.upper_x;
        const i64 first_height = frame_one - (chord.upper_y & frame_fraction);
        const i64 upper_fraction = (chord.upper_x & frame_fraction) * frame_to_fixed;
        const i64 lower_fraction = (chord.lower_x & frame_fraction) * frame_to_fixed;
        Crossings crossings = crossings_of(first_height, rise, dx);
        i64* at = cells.cells + static_cast<usize>(chord.upper_y >> frame_bits) * cells.stride + static_cast<usize>(chord.upper_x >> frame_bits);
        i64 from = upper_fraction;
        i64 cover = chord.sign * first_height * frame_to_fixed;
        for (usize row = 0; row + 1 < Rows; ++row) {
            const i64 to = upper_fraction + (dx < 0 ? -crossings.crossed : crossings.crossed);
            const i64 area = (from + to) * cover;
            at[0] = at[0] + (cover * 2 * one - area);
            at[1] = at[1] + area;
            at = at + cells.stride;
            from = to;
            cover = chord.sign * one;
            next_crossing(crossings);
        }
        const i64 last_height = rise - first_height - static_cast<i64>(Rows - 2) * frame_one;
        const i64 last_cover = chord.sign * last_height * frame_to_fixed;
        const i64 area = (from + lower_fraction) * last_cover;
        at[0] = at[0] + (last_cover * 2 * one - area);
        at[1] = at[1] + area;
    }
}

// One row's piece of a chord over two columns, in the two cells at `at`
// and what the second passes to its right. `from` and `to` are measured
// from the left edge of the first cell (0 to 2 * one, 16.16).
//
// The piece is a part in the left cell and a part in the right one, and a
// part it does not have has height 0. The height to the left of the edge
// between the cells is the distance from the piece's left end to that edge
// times the chord's slope, rounded down and no more than the piece's
// height. When the piece begins beyond the edge the distance is 0; when it
// stops short of the edge the product is no less than its height, because
// a piece is never wider than the slope allows by as much as 1/65536. So
// one expression gives every case its two heights, with nothing decided.
inline void add_piece_in_two_cells(i64* const at, const i64 from, const i64 to, const i64 height, const i64 rise,
                                   const u64 run_reciprocal, const i64 sign) noexcept {
    const i64 low = from < to ? from : to;
    const i64 high = from < to ? to : from;
    const i64 low_left = low < one ? low : one;
    const i64 high_left = high < one ? high : one;
    const i64 low_right = low - low_left;
    const i64 high_right = high - high_left;
    const u64 product = static_cast<u64>((one - low_left) * rise);
    const i64 used = static_cast<i64>((product * run_reciprocal) >> reciprocal_bits);
    const i64 left_height = used < height ? used : height;
    const i64 left_cover = sign * left_height;
    const i64 right_cover = sign * (height - left_height);
    const i64 left_area = (low_left + high_left) * left_cover;
    const i64 right_area = (low_right + high_right) * right_cover;
    at[0] = at[0] + (left_cover * 2 * one - left_area);
    at[1] = at[1] + (left_area + right_cover * 2 * one - right_area);
    at[2] = at[2] + right_area;
}

// Chords over `Rows` rows and two columns: one piece in each row, each in
// two cells.
template <usize Rows>
inline void add_chords_over_two_columns(const Cells& cells, const i32* const list, const usize count) noexcept {
    // The chord is narrower than two cells and no taller than its rows;
    // within a row a distance is at most a cell.
    static_assert(Rows >= 2 && reciprocals_exact(2 * frame_unit * fixed_unit + static_cast<u64>(Rows) * frame_unit,
                                                  static_cast<u64>(Rows) * frame_unit));
    static_assert(reciprocals_exact(fixed_unit * static_cast<u64>(Rows) * frame_unit, 2 * frame_unit));
    for (usize index = 0; index < count; ++index) {
        const Ordered chord = ordered(list + index * chord_words);
        const i64 rise = chord.lower_y - chord.upper_y;
        const i64 dx = chord.lower_x - chord.upper_x;
        const i64 first_height = frame_one - (chord.upper_y & frame_fraction);
        const i64 left_cell = (chord.upper_x < chord.lower_x ? chord.upper_x : chord.lower_x) >> frame_bits;
        const i64 upper = (chord.upper_x - left_cell * frame_one) * frame_to_fixed;
        const i64 lower = (chord.lower_x - left_cell * frame_one) * frame_to_fixed;
        const u64 run_reciprocal = reciprocals.of[magnitude(dx)];
        Crossings crossings = crossings_of(first_height, rise, dx);
        i64* at = cells.cells + static_cast<usize>(chord.upper_y >> frame_bits) * cells.stride + static_cast<usize>(left_cell);
        i64 from = upper;
        i64 height = first_height * frame_to_fixed;
        for (usize row = 0; row + 1 < Rows; ++row) {
            const i64 to = upper + (dx < 0 ? -crossings.crossed : crossings.crossed);
            add_piece_in_two_cells(at, from, to, height, rise, run_reciprocal, chord.sign);
            at = at + cells.stride;
            from = to;
            height = one;
            next_crossing(crossings);
        }
        const i64 last_height = rise - first_height - static_cast<i64>(Rows - 2) * frame_one;
        add_piece_in_two_cells(at, from, lower, last_height * frame_to_fixed, rise, run_reciprocal, chord.sign);
    }
}

// The rest, each in full.
inline void add_other_chords(const Cells& cells, const i32* const list, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        const Ordered chord = ordered(list + index * chord_words);
        const Fixed upper{.x = chord.upper_x, .y = chord.upper_y};
        const Fixed lower{.x = chord.lower_x, .y = chord.lower_y};
        add_any_chord(cells, upper, lower, chord.sign);
    }
}

// Every chord of the block into the cells; the block is then empty.
inline void add_block(const Cells& cells, Block& block) noexcept {
    const i32* const words = block.words;
    add_chords_in_a_cell(cells, words + form_in_a_cell * list_words, block.listed[form_in_a_cell]);
    add_chords_across_two_cells(cells, words + form_across_two_cells * list_words, block.listed[form_across_two_cells]);
    add_chords_down_rows<2>(cells, words + form_down_two_rows * list_words, block.listed[form_down_two_rows]);
    add_chords_over_two_columns<2>(cells, words + form_two_rows_two_columns * list_words, block.listed[form_two_rows_two_columns]);
    add_chords_down_rows<3>(cells, words + form_down_three_rows * list_words, block.listed[form_down_three_rows]);
    add_chords_down_rows<4>(cells, words + form_down_four_rows * list_words, block.listed[form_down_four_rows]);
    add_chords_down_rows<5>(cells, words + form_down_five_rows * list_words, block.listed[form_down_five_rows]);
    add_chords_over_two_columns<3>(cells, words + form_three_rows_two_columns * list_words, block.listed[form_three_rows_two_columns]);
    add_chords_over_two_columns<4>(cells, words + form_four_rows_two_columns * list_words, block.listed[form_four_rows_two_columns]);
    add_chords_across_three_cells(cells, words + form_across_three_cells * list_words, block.listed[form_across_three_cells]);
    add_other_chords(cells, words + form_other * list_words, block.listed[form_other]);
    block.count = 0;
    for (usize form = 0; form <= form_count; ++form) block.listed[form] = 0;
}

} // namespace stxlib::glyphs::detail

#endif
