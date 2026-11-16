#pragma once

// stxlib::glyphs: the coverage mask of an outline. STXlib's own rasterizer
// (the owner, 2026-11-15: stx_gtraster), for text. It reads an outline of
// straight lines and quadratic and cubic curves and writes one byte per
// pixel. It runs no font program and reads no font file: an outline is
// plain numbers.
//
// The rule. This is the definition, not a description of some other
// rasterizer:
// 1. Coordinates are 26.6 fixed point pixels (1/64 of a pixel), y upward.
// 2. Each curve is replaced by straight chords, in units of 1/256 of a
//    pixel measured from the mask's left and bottom edges (rule 5 gives
//    the mask's place).
//    - A quadratic by 2^k chords at equal steps of its parameter. k is how
//      many times its second difference, the larger of |P0 - 2 P1 + P2| on
//      the two axes, is quartered, rounding down each time, before it is at
//      most a quarter of a pixel. The chords' ends are the curve's points
//      rounded down to 1/256 of a pixel.
//    - A cubic by halving at its parameter's midpoint, each midpoint
//      rounded down to 1/256 of a pixel, until on both axes
//      |2 P3 - 3 P2 + P0| and |P3 - 3 P1 + 2 P0| are at most half a pixel
//      (each control point is then within a sixth of a pixel of where a
//      straight chord would have it), at most 16 times.
//    These are the subdivision rules of FreeType's smooth rasterizer in
//    its own units, taken so that the two draw the same chords for the
//    same curve. Every contour is closed by a chord from its last point to
//    its first.
// 3. S(p), the signed area of the outline inside pixel p, is the sum over
//    every chord of the area of the part of the pixel to the right of the
//    chord, over the chord's height inside the pixel's row: counted
//    positive for a chord going down the mask and negative for one going
//    up. Where contours overlap, the sum counts the overlap as many times
//    as it is wound: it is a sum, not a union.
// 4. The pixel's value is min(255, floor(256 |S(p)|)).
// 5. The mask is the box of every point of the outline, control points
//    included, rounded outward to whole pixels.
// The arithmetic is in integers only, so the mask is the same bytes on
// every host. S(p) is computed in units of 2^-33 of a pixel's area from
// chords whose crossings of pixel edges are found to within 1/65536 of a
// pixel, so it differs from the exact area under the chords by less than
// 2^-14 of a pixel for each chord through the pixel (tests/
// glyph_mask_tests.cpp checks the bound against exact areas).
//
// The monochrome mask (render_monochrome) is its own rule, on the same
// chords and in the same box. It is a scan conversion, not the coverage
// mask cut at a value:
// 6. A pixel is 255 when the outline winds round its centre (the number of
//    chords crossing the row's line of centres to the left of it, those
//    going down the mask less those going up, is not zero), and 0 when
//    not. A centre exactly on the outline is taken an instant right of and
//    below where it is, which puts it on one side.
// 7. Drop-outs. Where the outline's inside crosses a row's line of centres
//    between two neighbouring centres and reaches neither, so that rule 6
//    leaves both pixels off, the pixel whose centre is nearer the middle
//    of that crossing is 255; the left one when they are equally near. The
//    same along every column's line of centres, between the centres of
//    two rows: the nearer pixel, and the lower one when equally near. At
//    the mask's edge it is the one pixel there is.
// Rules 6 and 7 are the scan conversion rules of the OpenType
// specification with its "smart" drop-out control and stubs included (the
// mode the text fonts GUI.Forms bundles ask for), but for one thing: the
// specification turns a pixel on when the outline passes exactly through
// its centre, whichever side the inside is on, and rule 6 puts such a
// centre on one side, so that a shape reaching from one centre to the
// next is one pixel and not two. The x at which a chord crosses a line of
// centres is the exact x rounded down to 1/65536 of a pixel, and the same
// for columns.
//
// The mask's place. measure gives the bounds: the box of every point of
// the outline, control points included, with the offset added, rounded
// outward to whole pixels. `left` is the x of the mask's left edge and
// `top` the y of its top edge, in whole pixels. Row 0 of the mask is its
// top row; a row is `width` bytes; rows follow one another with no padding.
//
// The offset is added to every point before anything else: one outline
// serves every position, to 1/64 of a pixel.
//
// Bounds. An outline has at most Limits::points points (and never more
// than 2^30, whatever the limit); a mask is at most
// Limits::width by Limits::height pixels; the working storage is 8 bytes
// for each of at most Limits::work_cells cells, and 27 kilobytes for the
// chords and segments in hand. A mask whose (width + 2) * height cells are
// within Limits::work_cells is drawn at once, and the work is linear in
// the points, the chords (at most 2^16 a curve) and the mask's area. A
// larger mask is drawn a band of rows at a time, as many rows as the cells
// hold, with the same bytes; the chords are then gone through once for
// each band. The monochrome mask is always drawn so, rows and then
// columns, three cells for each pixel and one more a line. A mask one of
// whose lines does not fit the cells is refused. No recursion.
//
// Failure. invalid_argument: the segments and points do not agree, a
// contour does not begin with a move, a point lies outside the bounds
// given to render, or the mask's storage is not width * height bytes.
// limit_exceeded: a Limits bound. out_of_memory: the working storage. On
// failure the mask's bytes are unspecified. Nothing throws.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs {

// 26.6 fixed point pixels.
struct Point final {
    i32 x{0};
    i32 y{0};
};

enum class Segment : u8 {
    // Begins a contour at one point (and closes the one before it).
    move = 0,
    // A straight line to one point.
    line = 1,
    // A quadratic curve: a control point, then the end point.
    quadratic = 2,
    // A cubic curve: two control points, then the end point.
    cubic = 3,
};

// An outline, borrowed: `points` holds each segment's points in order.
struct Outline final {
    View<Segment> segments{};
    View<Point> points{};
};

struct Bounds final {
    // Whole pixels: the mask covers x in [left, left + width) and y in
    // (top - height, top].
    i32 left{0};
    i32 top{0};
    u32 width{0};
    u32 height{0};
};

struct Limits final {
    u32 points{16384};
    u32 width{16384};
    u32 height{16384};
    usize work_cells{static_cast<usize>(1) << 22};
};

// The bounds of the outline's mask at `offset`. An outline with no point
// has an empty mask (width and height 0).
[[nodiscard]] Status measure(const Outline& outline, Point offset, const Limits& limits, Bounds& bounds) noexcept;

// Renders masks. It owns the working storage, kept from one call to the
// next; it holds nothing else. One thread at a time.
class Rasterizer final {
public:
    Rasterizer() noexcept = default;
    Rasterizer(Rasterizer&&) noexcept = default;
    Rasterizer& operator=(Rasterizer&&) noexcept = default;
    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;

    // Writes the mask of `outline` at `offset` into `mask`, which is
    // bounds.width * bounds.height bytes and is replaced. `bounds` is what
    // measure gave for the same outline, offset and limits, or any box that
    // holds every point. `mask` must not overlap the outline.
    [[nodiscard]] Status render(const Outline& outline, Point offset, const Bounds& bounds, const Limits& limits,
                                MutableView<u8> mask) noexcept;

    // The same for the monochrome mask (rules 6 and 7): every byte is 0 or
    // 255.
    [[nodiscard]] Status render_monochrome(const Outline& outline, Point offset, const Bounds& bounds, const Limits& limits,
                                           MutableView<u8> mask) noexcept;

    // The bytes of the working storage, as allocated now.
    [[nodiscard]] usize bytes_held() const noexcept { return cells_.capacity() * sizeof(i64) + block_.capacity() * sizeof(i32); }

    // Frees the working storage.
    void release() noexcept;

private:
    Vector<i64> cells_{};
    // Chords in hand (src/algorithms/glyphs/chords.hpp) and the lists of
    // segments (src/algorithms/glyphs/mask.cpp).
    Vector<i32> block_{};
};

} // namespace stxlib::glyphs
