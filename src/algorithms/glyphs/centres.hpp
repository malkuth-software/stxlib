// stxlib::glyphs: the monochrome mask's arithmetic (include/stxlib/
// glyph_mask.hpp states the rule).
//
// The same method as the coverage mask, with crossings in place of areas.
// Through the pixel centres of each row runs a line; where a chord crosses
// it, the chord's sign is added to the first pixel whose centre is at or
// right of the crossing. A running sum along the row is then, at every
// pixel, the number of times the outline winds round its centre. The lines
// are taken an instant below the centres and the centres an instant right
// of where they are, so a centre exactly on the outline is on one side of
// it, the same side whichever way the outline runs.
//
// For drop-outs each stretch between two neighbouring centres also keeps
// the least and the greatest crossing in it. Ink that lies between two
// centres and reaches neither shows as a stretch with crossings in it and
// a winding of zero before and after.
//
// Columns are rows with x and y exchanged: the caller exchanges them.

#ifndef STXLIB_ALGORITHMS_GLYPHS_CENTRES_HPP
#define STXLIB_ALGORITHMS_GLYPHS_CENTRES_HPP

#include "algorithms/glyphs/chords.hpp"

namespace stxlib::glyphs::detail {

// A stretch is three cells: the windings added, the least crossing and the
// greatest (16.16; none: least above greatest).
inline constexpr usize stretch_cells = 3;
inline constexpr i64 no_least = static_cast<i64>(1) << 62;
inline constexpr i64 no_greatest = -1;
inline constexpr i64 half = one / 2;

// The lines of a band: lines `first` to `first + count`, each of `stride`
// stretches (the pixels across, and one more).
struct Lines final {
    i64* cells{nullptr};
    usize stride{0};
    i64 first{0};
    i64 count{0};
};

inline void clear_lines(const Lines& lines) noexcept {
    const usize stretches = static_cast<usize>(lines.count) * lines.stride;
    for (usize index = 0; index < stretches; ++index) {
        i64* const at = lines.cells + index * stretch_cells;
        at[0] = 0;
        at[1] = no_least;
        at[2] = no_greatest;
    }
}

// A chord's crossings of the band's lines. Line k is 256 k + 128 down (in
// 1/256 of a pixel) and an instant more: the chord crosses it when its
// upper end is at or above 256 k + 128 and its lower end below. The x of a
// crossing is the exact x rounded down to 1/65536 of a pixel, as in
// add_any_chord.
inline void add_crossings(const Lines& lines, const Fixed begin, const Fixed end) noexcept {
    if (begin.y == end.y) return;
    const bool downward = begin.y < end.y;
    const Fixed upper{.x = downward ? begin.x : end.x, .y = downward ? begin.y : end.y};
    const Fixed lower{.x = downward ? end.x : begin.x, .y = downward ? end.y : begin.y};
    const i64 sign = downward ? 1 : -1;
    const i64 dx = lower.x - upper.x;
    const i64 rise = lower.y - upper.y;
    const i64 run = magnitude(dx);
    const i64 toward = dx < 0 ? -1 : 1;
    // The first line at or below the upper end, the last above the lower.
    const i64 first_line = (upper.y + frame_one / 2 - 1) >> frame_bits;
    const i64 last_line = (lower.y - frame_one / 2 - 1) >> frame_bits;
    const i64 from_line = first_line > lines.first ? first_line : lines.first;
    const i64 to_line = last_line < lines.first + lines.count - 1 ? last_line : lines.first + lines.count - 1;
    if (from_line > to_line) return;
    const i64 row_product = run * one;
    const i64 step = row_product / rise;
    const i64 step_remainder = row_product - step * rise;
    const i64 product = (from_line * frame_one + frame_one / 2 - upper.y) * run * frame_one + (dx < 0 ? rise - 1 : 0);
    i64 crossed = product / rise;
    i64 remainder = product - crossed * rise;
    const i64 upper_x = upper.x * frame_to_fixed;
    i64* line = lines.cells + static_cast<usize>(from_line - lines.first) * lines.stride * stretch_cells;
    for (i64 index = from_line; index <= to_line; ++index) {
        const i64 x = upper_x + toward * crossed;
        // The first pixel whose centre is at or right of x.
        const i64 stretch = (x + half - 1) >> fraction_bits;
        i64* const at = line + static_cast<usize>(stretch) * stretch_cells;
        at[0] = at[0] + sign;
        at[1] = x < at[1] ? x : at[1];
        at[2] = x > at[2] ? x : at[2];
        line = line + lines.stride * stretch_cells;
        crossed = crossed + step;
        remainder = remainder + step_remainder;
        if (remainder >= rise) {
            remainder = remainder - rise;
            crossed = crossed + 1;
        }
    }
}

// Which of the two pixels beside stretch `stretch` a drop-out in it turns
// on: the one whose centre is nearer the middle of the ink, and when they
// are equally near, the first (`later` false) or the second (`later`
// true); and always a pixel there is (`pixels` of them, at least one).
[[nodiscard]] inline usize drop_out_pixel(const i64 stretch, const i64 least, const i64 greatest, const bool later,
                                          const usize pixels) noexcept {
    // The two centres are equally far from the edge between the pixels, at
    // stretch * one: twice the middle of the ink against twice that edge.
    const i64 doubled_middle = least + greatest;
    const i64 doubled_edge = stretch * 2 * one;
    const bool second = later ? doubled_middle >= doubled_edge : doubled_middle > doubled_edge;
    i64 pixel = second ? stretch : stretch - 1;
    if (pixel < 0) pixel = 0;
    if (pixel > static_cast<i64>(pixels) - 1) pixel = static_cast<i64>(pixels) - 1;
    const usize result = static_cast<usize>(pixel);
    return result;
}

// The rows of a band, written: every pixel 255 when the outline winds
// round its centre and 0 when not, and then the drop-outs along the row.
// `out` is the band's first row; a row is `width` bytes.
inline void write_rows(const Lines& lines, const usize width, u8* const out) noexcept {
    for (usize line = 0; line < static_cast<usize>(lines.count); ++line) {
        const i64* const cells = lines.cells + line * lines.stride * stretch_cells;
        u8* const row = out + line * width;
        i64 winding = 0;
        for (usize stretch = 0; stretch <= width; ++stretch) {
            const i64* const at = cells + stretch * stretch_cells;
            const i64 before = winding;
            winding = winding + at[0];
            if (stretch < width) row[stretch] = winding != 0 ? 255 : 0;
            if (before == 0 && winding == 0 && at[1] < at[2]) {
                row[drop_out_pixel(static_cast<i64>(stretch), at[1], at[2], false, width)] = 255;
            }
        }
    }
}

// The columns of a band, for their drop-outs only: the lines are columns
// `first` to `first + count` and a stretch lies between two rows' centres.
// `out` is the whole mask, `height` rows of `width` bytes.
inline void mark_columns(const Lines& lines, const usize width, const usize height, u8* const out) noexcept {
    for (usize line = 0; line < static_cast<usize>(lines.count); ++line) {
        const i64* const cells = lines.cells + line * lines.stride * stretch_cells;
        const usize column = static_cast<usize>(lines.first) + line;
        i64 winding = 0;
        for (usize stretch = 0; stretch <= height; ++stretch) {
            const i64* const at = cells + stretch * stretch_cells;
            const i64 before = winding;
            winding = winding + at[0];
            if (before == 0 && winding == 0 && at[1] < at[2]) {
                out[drop_out_pixel(static_cast<i64>(stretch), at[1], at[2], true, height) * width + column] = 255;
            }
        }
    }
}

} // namespace stxlib::glyphs::detail

#endif
