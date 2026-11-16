#pragma once

// stxlib::glyphs: faces to draw text from. Two objects.
//
// - Typeface: one face of one font as the PROGRAM holds it. It reads the
//   font (font_file.hpp), keeps the glyphs in use (glyph_store.hpp) and
//   what the hinter measures the font by (glyph_hinter.hpp). There is one
//   for a face however many windows or threads draw with it: what one of
//   them has caused to be read, the others find already there. It may be
//   used from any threads at once.
// - Face: what ONE THREAD draws with. It keeps the outlines it has fitted
//   to a size, and its working storage, and draws (glyph_mask.hpp). Any
//   number of Faces share a Typeface.
// A caller that has glyph numbers and positions (from shaping) needs
// nothing else of stxlib::glyphs.
//
// What is kept, and why at this level. Text is drawn with each glyph at a
// position of its own to 1/64 of a pixel, so a glyph's mask is different
// almost every time it is drawn and is not worth keeping. What is the same
// at every position is the glyph's outline fitted to the size: a Face
// keeps that, by glyph, size and fit. Under it the Typeface keeps the
// outline in font units, which serves every size and every Face. A mask is
// made from the fitted outline each time, which is the cheap part.
//
// - fitted: a glyph's outline at a size, as segments ready to draw, with
//   its advance and its box. Held in two halves, newer and older, exactly
//   as the glyph store holds its recent glyphs: one found in the older
//   half is moved to the newer, and when the newer is full the older is
//   given up.
// - mask: one glyph's mask, the outline moved by a part of a pixel first.
// - measure_run and draw_run: a run of glyphs, each at its own position,
//   into one mask. Where glyphs overlap, a pixel that has c and gets p
//   becomes p + c * (255 - p) / 255, rounded down; in the monochrome fit,
//   the larger of the two.
//
// The fit (Fit). light: grey coverage, the outline hinted along y only;
// this is the fit for text. monochrome: one bit a pixel, hinted along both
// axes, drawn by the monochrome rule. none: the outline scaled and no
// more, for measuring and for text that is to match an unhinted layout
// point for point. The unfitted outline is the one a TrueType or CFF scaler
// gives without hinting, point for point: a TrueType glyph's parts and
// their offsets are each scaled and rounded on their own (load_scaled_glyph),
// so such a glyph is read from the font at the size and not scaled from
// the store.
//
// A glyph's ink box at a size (Typeface::ink_box) is the box of that
// unfitted outline's points, control points included: what a shaper that
// asks FreeType for a glyph's extents without hinting is given.
//
// Turned text (DrawStyle's matrix; turn_matrix makes one for an angle).
// A glyph with a matrix is drawn one of three ways, settled by the style:
// - BY QUARTERS. At a quarter turn, a half turn and three quarters
//   (matrices of 0, 65536 and -65536 only) a glyph is the upright glyph's
//   mask turned pixel for pixel: the same bytes, edges on pixel boundaries
//   and all. (Thickened text excepted, which goes one of the ways below.)
// - BY THE MASK'S SURFACE. Fit::light at any other turn, at sizes up to
//   FaceLimits::lattice_size. The glyph is fitted and drawn upright, at no
//   part of a pixel, exactly as text at rest is. That mask is made a
//   smooth surface through its pixels (CONV, the interpolant of the
//   owner's stx_conv, which puts no ripple between pixels that they do
//   not have), and the turned mask is the surface read at each turned
//   pixel's centre. What is turned is the letter as it is seen upright,
//   fitting and all. The surface is made once for a glyph at a size and
//   kept, as a lattice of whole numbers about fifty times the mask's
//   bytes (FaceLimits::lattice_bytes); every angle and every part of a
//   pixel is read from the one lattice. The rules, which are the
//   definition, are at the head of src/algorithms/glyphs/turn_lattice.hpp.
// - BY THE OUTLINE. Anything else with a matrix: Fit::none, Fit::monochrome,
//   a matrix that is not a turn, a larger size, a glyph whose upright mask
//   is more than 256 pixels a side. The outline, fitted as the style says,
//   is taken through the matrix and drawn.
// WHY THE SURFACE. Fitting moves a glyph's features onto whole pixels of
// the upright frame. A fitted outline that is then turned keeps what was
// moved and loses the whole pixels that made it worth moving: measured on
// the bundled Arimo at 13 pixels, the gap under the dot of an i is 1.45
// pixels unfitted and 1.00 fitted (at 11 pixels 1.24 and 1.06), and a gap
// of one pixel that is not on a pixel row is grey, so the dot closes up
// against its stem. In the upright mask that gap is a clear row, and the
// surface turns the clear row. By 20 pixels the two gaps agree (2.25 and
// 2.28) and a lattice grows with the mask's area, which is why larger
// text goes by its outline. For text to match an unhinted layout point
// for point, Fit::none with the matrix is the plain outline turned.
// PASSING THROUGH 0 DEGREES. With no turn the matrix changes nothing and
// the bytes are the upright ones. Just off it a glyph by its surface is
// the upright mask of NO part of a pixel, read at the part of a pixel the
// glyph has. For a glyph placed on a whole pixel that is the upright mask
// byte for byte, so nothing changes as the turn begins. For one placed
// between pixels it is that mask read between its pixels, where upright
// text draws the outline anew at the part of a pixel, and the two differ
// along the letter's upright edges: a stem that the whole-pixel mask has
// across two columns of pixels stays across two, where drawn anew a part
// of a pixel over it may fall on one. The letter does not move or change
// shape; its edges change. Measured on four bundled faces at 13 pixels, a
// twentieth of a degree off 0: between pixels the inked pixels differ by
// 17 to 23 levels of 255 on average (the most at any pixel about 110); at
// whole pixels by under half a level (the most, 2). A caller that wants no
// change as a turn begins places its glyphs on whole pixels while the
// text turns.
// WHAT IT COSTS. Measured on the M4 (the bundled Arimo and Noto Sans CJK,
// the mean of 7, 17, 30 and 45 degrees): a glyph's mask by its surface
// takes 0.8 to 1.2 times what its fitted outline turned takes at 11 to 16
// pixels (485 ns against 434 at 13 pixels; the upright mask 307), and 1.3
// to 1.6 times at 24 to 32, which is the other reason for the limit on
// size. Making a surface, once for a glyph at a size, takes 6 to 7
// microseconds at 11 to 13 pixels and 10 at 16.
// Glyphs are placed by the layout's own advances, which are not fitted,
// so turning never moves glyphs against one another; with
// DrawStyle::turn_places the places are taken through the matrix as well,
// each rounded once to a 64th of a pixel, and the part of a pixel left
// over goes to the drawing along both axes. Everything is whole-number
// arithmetic, the sine and cosine and the surface included: the same
// bytes on every host.
//
// Thicker strokes (DrawStyle::thicken), for a bold or a small capital a
// font has not. This is the definition:
// 1. t is the thickening in 26.6 pixels and h is t / 2, rounded down. The
//    outline is the one about to be drawn: fitted, and through the matrix
//    if there is one. (For text turned by its mask's surface the outline
//    drawn is the upright one, so that is what is thickened, and the
//    letters grow along the text's own axes.) Its points are taken contour
//    by contour, control points as any others.
// 2. A is the sum over every contour of x[i] * y[i+1] - x[i+1] * y[i], the
//    last point followed by the first. If A is 0 nothing is done. s is 1
//    if A is more than 0 and -1 if less.
// 3. For a point P, a is the nearest point before it in its contour that
//    is not at P, and b the nearest after it. If there is none, P's move
//    is nothing. Else, for the step d from a to P: its length l is the
//    whole number square root, rounded down, of dx * dx + dy * dy, and its
//    outward normal n1 is (s * dy * 65536 / l, -s * dx * 65536 / l), each
//    rounded toward zero. n2 is the same for the step from P to b.
// 4. m = n1 + n2. c = 65536 + ((n1.x * n2.x + n1.y * n2.y) rounded down to
//    a 65536th), and no less than 16384. P's move is (h * m.x / c,
//    h * m.y / c), each rounded toward zero: a point on a straight side
//    moves h outward, a square corner moves h along each side, and a
//    sharp turn no further than about 2.8 h.
// 5. Every point then moves by (h, h) more: the outline's left and bottom
//    stay where they were and it grows by t to the right and upward, so
//    letters stay on their baseline. An advance is the caller's to widen.
// Every move is worked out from the outline as it was. It is STXlib's own
// rule (the owner, 2026-11-16), not FreeType's emboldening.
//
// Starting up. Opening reads the font's tables and nothing of its glyphs;
// the hinter is not prepared until the first glyph is fitted with a hinted
// fit, and no glyph is read until it is asked for. A face's first hinted
// glyph costs about what FreeType's does, so nothing needs warming for
// start-up's sake; warm is for a caller that would rather pay before the
// first frame than in it, and costs what those glyphs would have cost at
// first use.
//
// Memory, in bytes. A Typeface's glyphs are bounded by TypefaceLimits and
// a Face's fitted outlines by FaceLimits::fitted_bytes: the storage for
// each never exceeds its limit. Besides those there is, for a Typeface, 4
// bytes a glyph of the font and the hinter's measures (4 bytes a glyph and
// a few kilobytes), and for a Face the working storage, which grows to the
// largest glyph and run met and is kept; and the lattices of glyphs turned
// by their surfaces, bounded by FaceLimits::lattice_bytes. bytes_held gives all of it as
// allocated now, and bytes_peak the most it has been. Once the glyphs of a
// text are held, drawing it again allocates nothing.
//
// Threads. A Typeface: any threads at once (it locks; a Face goes to it
// only for a glyph it has not fitted). A Face: one thread at a time. The
// font's bytes are borrowed: they must outlive the Typeface and must not
// change while it is open (a count read from them is read again where it
// is used, so bytes that change under it, a file mapped for writing, can
// take a read outside a table; a caller who cannot promise that opens a
// copy). What a call
// hands back as a view (FittedGlyph) is good until the next call on the
// Face.
//
// What it is made to last through. The interface is whole numbers in
// stated units (26.6 pixels, 16.16 matrices), plain structures whose
// members have defaults (a later version adds members, with defaults that
// mean what was meant before), and one object whose inside is not shown.
// What it does not do yet, and has room for: colour glyphs, variable
// fonts, coverage for striped displays, upright text in columns. Each
// would be a further member of DrawStyle or a further Fit, not a change to
// what is here.
//
// Failure. invalid_argument: a face not opened, a glyph the font has not,
// a size or a thickening out of range, or what the reader refuses.
// limit_exceeded: a limit. out_of_memory. In a run, a glyph that
// cannot be had is counted (missing_glyphs) and draws nothing: the run is
// still drawn. Nothing throws.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_mask.hpp"
#include "stxlib/glyph_store.hpp"
#include "stxlib/result.hpp"
#include "stxlib/shared.hpp"
#include "stxlib/types.hpp"
#include "stxlib/unique.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs {

// What this interface gives for the same font, glyphs, positions, sizes
// and style is fixed: the same numbers and the same bytes on every host
// and in every later version that carries this number. A change to any of
// them (a rule of the rasterizer, of the hinter, of thickening, of how a
// run is put together) changes this number, so that a caller that keeps
// masks keeps the number beside them and makes them again when it differs.
inline constexpr u32 face_profile_version = 1;

enum class Fit : u8 {
    none = 0,
    light = 1,
    monochrome = 2,
};

struct TypefaceLimits final {
    FontLimits font{};
    StoreLimits store{};
};

struct FaceLimits final {
    Limits mask{};
    // The storage for fitted outlines never exceeds this many bytes. An
    // outline takes 9 bytes a point of its segments and 96 for itself.
    // Storage is taken as it is needed, not all at once: the limit is
    // where it stops, and a text face in use holds a few tens of
    // kilobytes.
    usize fitted_bytes{4194304};
    // Lightly fitted text is turned by its masks' surfaces at sizes up to
    // this (26.6 pixels to the em; 20 pixels), and by its outlines above
    // it. 0: never by surfaces.
    i64 lattice_size{1280};
    // The storage for those surfaces' lattices never exceeds this many
    // bytes. A lattice takes 50 bytes a pixel of the glyph's upright mask
    // and its margin ((5 w + 6)(5 h + 6) numbers of two bytes for a mask of
    // w by h) and 64 for itself: about 3.5 kilobytes a glyph at 13 pixels
    // and 10 at 24. Taken as needed, as fitted_bytes is. A glyph whose
    // lattice is more than a fifth of this is made one each time it is
    // drawn, and none is kept for it.
    usize lattice_bytes{4194304};
};

// A glyph's outline at a size: borrowed from the face. 26.6 pixels, y
// upward, the glyph's origin at (0, 0).
struct FittedGlyph final {
    Outline outline{};
    // 26.6; a whole number of pixels in a hinted fit.
    i32 advance{0};
    // The box of its points, control points included (26.6). All 0 for a
    // glyph of no points.
    i32 left{0};
    i32 bottom{0};
    i32 right{0};
    i32 top{0};
};

// A glyph of a run: its number, and where its origin goes, in 26.6 pixels
// from the run's origin pixel, x rightward and y DOWNWARD (as a surface's
// rows run).
struct RunGlyph final {
    u32 glyph{0};
    i32 x{0};
    i32 y{0};
};

// How glyphs are drawn: for a run, and for one glyph's mask.
struct DrawStyle final {
    // 26.6 pixels to the em.
    i64 size{0};
    Fit fit{Fit::light};
    // Thicker strokes, by this much (26.6 pixels, 0 to 1024): the rule is
    // at the head of this file.
    i32 thicken{0};
    // Each glyph is taken through this matrix (16.16, on the glyph's own
    // axes, y upward) about its origin before it is placed:
    // x' = xx x + xy y, y' = yx x + yy y. With any fit; which of three
    // ways it is drawn is at the head of this file ("Turned text").
    bool transformed{false};
    i32 xx{0x10000};
    i32 xy{0};
    i32 yx{0};
    i32 yy{0x10000};
    // For a run with a matrix: the glyphs' places are given as laid out
    // before the turn and are taken through the matrix too, about the
    // run's origin (turn_place).
    bool turn_places{false};
    // For a run: only glyphs that can show in these pixels are drawn:
    // [left, right) by [top, bottom), from the run's origin pixel, y
    // downward.
    bool windowed{false};
    i32 window_left{0};
    i32 window_top{0};
    i32 window_right{0};
    i32 window_bottom{0};
};

// A matrix for DrawStyle: 16.16, on axes with y upward.
struct TextMatrix final {
    i32 xx{0x10000};
    i32 xy{0};
    i32 yx{0};
    i32 yy{0x10000};
};

// The turn by an angle in degrees (16.16; 90 degrees is 90 << 16), counter-
// clockwise as the text is seen: xx = yy = the cosine, yx = the sine,
// xy = minus the sine. Worked out in whole numbers, the same on every
// host; each entry is within one 65536th of the true value, the quarter
// turns are exact (0, 65536 and -65536), and the turn by minus an angle
// is this one's transpose, which undoes it.
[[nodiscard]] TextMatrix turn_matrix(i64 degrees) noexcept;

inline void set_matrix(DrawStyle& style, const TextMatrix& matrix) noexcept {
    style.transformed = true;
    style.xx = matrix.xx;
    style.xy = matrix.xy;
    style.yx = matrix.yx;
    style.yy = matrix.yy;
}

// A place on the surface (26.6 pixels from the run's origin, x rightward,
// y DOWNWARD) through the matrix: where a point of upright text is when
// the text is turned. Each coordinate rounded to the nearest 64th, halves
// away from zero. This is the rule a run's places are turned by.
[[nodiscard]] Point turn_place(const TextMatrix& matrix, Point place) noexcept;

// The matrix that undoes `matrix`, for finding what a point of the surface
// is in the upright text (hit testing, carets, selection): through
// turn_place with it. False when the matrix has no inverse or its inverse
// does not fit. For a turn, turn_matrix of minus the angle is exact;
// this is within a 65536th an entry.
[[nodiscard]] bool invert_matrix(const TextMatrix& matrix, TextMatrix& inverse) noexcept;

// A box on the surface (26.6, y downward): [left, right) by [top, bottom).
struct PlaceBox final {
    i32 left{0};
    i32 top{0};
    i32 right{0};
    i32 bottom{0};
};

// The box around a box's four corners taken through the matrix: where the
// upright text's box is when the text is turned (for damage, clipping and
// what is told of the text's place).
[[nodiscard]] PlaceBox turn_box(const TextMatrix& matrix, const PlaceBox& box) noexcept;

// Where a run's ink can be: whole pixels from the run's origin pixel, y
// downward, [left, right) by [top, bottom). Empty: all 0.
struct RunBox final {
    i32 left{0};
    i32 top{0};
    i32 right{0};
    i32 bottom{0};
    u32 missing_glyphs{0};
    u32 outside_glyphs{0};
    // The run's box is wider or taller than FaceLimits::mask allows.
    bool oversized{false};
};

// A run's mask: `pixels` is width * height bytes, the top row first.
struct RunCoverage final {
    Vector<u8> pixels{};
    u32 width{0};
    u32 height{0};
    // The pixel of the first byte, from the run's origin pixel, y downward.
    i32 left{0};
    i32 top{0};
    u32 missing_glyphs{0};
    u32 outside_glyphs{0};
    // Nothing is drawn (pixels is empty): the caller counts the run.
    bool oversized{false};
};

// A glyph's box at a size, 26.6 pixels, y upward from the baseline: left
// to right and bottom to top. All 0 for a glyph of no points.
struct InkBox final {
    i32 left{0};
    i32 bottom{0};
    i32 right{0};
    i32 top{0};
};

namespace detail {
// What a typeface and a face hold (src/algorithms/glyphs/glyph_face.cpp).
// Neither is part of this interface, so that it can change without a
// caller changing.
struct TypefaceState;
struct FaceState;
} // namespace detail

class Typeface final {
public:
    Typeface() noexcept;
    Typeface(Typeface&&) noexcept;
    Typeface& operator=(Typeface&&) noexcept;
    Typeface(const Typeface&) = delete;
    Typeface& operator=(const Typeface&) = delete;
    ~Typeface() noexcept;

    // Opens face `face_index` of the font in `bytes` (0 for a font that is
    // not a collection). Nothing of its glyphs is read. To be done before
    // the typeface is shared: this one call is not for several threads.
    // A typeface is opened once: open on one already open is refused
    // (invalid_argument), since the faces and shapers that hold it hold
    // its font.
    [[nodiscard]] Status open(View<u8> bytes, u32 face_index, const TypefaceLimits& limits = TypefaceLimits{}) noexcept;

    // The font opened (its numbers, and glyph_of, advance_of and
    // has_feature of it), or null. It does not change once opened.
    [[nodiscard]] const Font* font() const noexcept;

    // Reads the glyphs of these characters into the kept part of the glyph
    // store. `kept` is how many glyphs are kept afterwards.
    [[nodiscard]] Status keep(View<u32> characters, usize& kept) noexcept;

    // The glyph's outline at `size` (26.6 pixels to the em, at most 2^24)
    // in the form the font has it, and its advance (26.6): scaled for
    // Fit::none, hinted otherwise. This is what a Face fits from.
    [[nodiscard]] Status fit(u32 glyph, i64 size, Fit fit, GlyphOutline& outline, i32& advance) noexcept;

    // The glyph's ink box at `size`, unfitted. Nothing is kept of it: a
    // caller that asks often keeps the answer.
    [[nodiscard]] Status ink_box(u32 glyph, i64 size, InkBox& box) noexcept;

    // How many times a glyph has been read from the font into the store so
    // far.
    [[nodiscard]] usize glyphs_read() const noexcept;

    // Whether what the typeface holds agrees with itself (GlyphStore::whole).
    // For tests and checked builds.
    [[nodiscard]] bool whole() const noexcept;

    // The bytes held, as allocated now, and the most that has been since
    // the typeface was opened or reset_peak was last called.
    [[nodiscard]] usize bytes_held() const noexcept;
    [[nodiscard]] usize bytes_peak() const noexcept;
    void reset_peak() noexcept;

private:
    Unique<detail::TypefaceState> state_{};
};

// A typeface for the program to share: opened, and owned by however many
// faces and callers hold it.
[[nodiscard]] Result<Shared<Typeface>> open_typeface(View<u8> bytes, u32 face_index,
                                                     const TypefaceLimits& limits = TypefaceLimits{}) noexcept;

class Face final {
public:
    Face() noexcept;
    Face(Face&&) noexcept;
    Face& operator=(Face&&) noexcept;
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;
    ~Face() noexcept;

    // A face of the program's typeface: it holds the typeface for as long
    // as it is open.
    [[nodiscard]] Status open(const Shared<Typeface>& typeface, const FaceLimits& limits = FaceLimits{}) noexcept;

    // A face with a typeface of its own, for a program with one thread of
    // text, or a font nothing else uses.
    [[nodiscard]] Status open(View<u8> bytes, u32 face_index, const FaceLimits& limits = FaceLimits{},
                              const TypefaceLimits& typeface_limits = TypefaceLimits{}) noexcept;

    // The font, or null for a face not opened.
    [[nodiscard]] const Font* font() const noexcept;

    // The typeface it draws from (empty for a face not opened).
    [[nodiscard]] Shared<Typeface> typeface() const noexcept;

    // Reads the glyphs of these characters into the kept part of the
    // typeface's store and, if `size` is not 0, fits them at it. `warmed`
    // is how many glyphs were had.
    [[nodiscard]] Status warm(View<u32> characters, i64 size, Fit fit, usize& warmed) noexcept;

    // The glyph's outline at `size` (26.6 pixels to the em, at most 2^24).
    [[nodiscard]] Status fitted(u32 glyph, i64 size, Fit fit, FittedGlyph& glyph_out) noexcept;

    // The glyph's mask in `style` (its size, fit, thickening and matrix),
    // moved by `shift` (26.6, y upward) last: `bounds` as glyph_mask.hpp's
    // measure gives them for the outline drawn (for a glyph turned by its
    // mask's surface, the pixels whose centres the turned mask reaches:
    // turn_lattice.hpp, rule 3), `pixels` bounds.width * bounds.height
    // bytes, the top row first. A shift farther than 2^30 either way is
    // invalid_argument.
    [[nodiscard]] Status mask(u32 glyph, const DrawStyle& style, Point shift, Bounds& bounds, Vector<u8>& pixels) noexcept;

    // Where the run's ink can be, without drawing it.
    [[nodiscard]] Status measure_run(View<RunGlyph> glyphs, const DrawStyle& style, RunBox& box) noexcept;

    // The run's mask. `coverage` is replaced; its storage is reused.
    [[nodiscard]] Status draw_run(View<RunGlyph> glyphs, const DrawStyle& style, RunCoverage& coverage) noexcept;

    // How many times an outline has been fitted so far.
    [[nodiscard]] usize glyphs_fitted() const noexcept;

    // How many times a glyph's mask has been made a surface so far.
    [[nodiscard]] usize lattices_made() const noexcept;

    // Whether what the face holds agrees with itself: each half's fitted
    // outlines lie end to end, are counted right and are within the limit,
    // each can be found by its glyph, size and fit, and each has as many
    // points as its segments need; and the same of the lattices it holds.
    // Linear in what is held: for tests and
    // checked builds (which check it at the end of every call).
    [[nodiscard]] bool whole() const noexcept;

    // The bytes this face holds (its fitted outlines and working storage;
    // not the typeface's, which the typeface gives), as allocated now, and
    // the most that has been since it was opened or reset_peak was last
    // called.
    [[nodiscard]] usize bytes_held() const noexcept;
    [[nodiscard]] usize bytes_peak() const noexcept;
    void reset_peak() noexcept;

    // Frees everything; the face is as before open.
    void release() noexcept;

private:
    Unique<detail::FaceState> state_{};
};

} // namespace stxlib::glyphs
