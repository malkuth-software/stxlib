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
// more, for text that is turned or sheared (a matrix is given with it) and
// for measuring.
//
// Thicker strokes (DrawStyle::thicken), for a bold or a small capital a
// font has not. This is the definition:
// 1. t is the thickening in 26.6 pixels and h is t / 2, rounded down. The
//    outline is the one about to be drawn: fitted, and through the matrix
//    if there is one. Its points are taken contour by contour, control
//    points as any others.
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
// largest glyph and run met and is kept. bytes_held gives all of it as
// allocated now, and bytes_peak the most it has been. Once the glyphs of a
// text are held, drawing it again allocates nothing.
//
// Threads. A Typeface: any threads at once (it locks; a Face goes to it
// only for a glyph it has not fitted). A Face: one thread at a time. The
// font's bytes are borrowed: they must outlive the Typeface. What a call
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
// a size or a thickening out of range, a matrix with a hinted fit, or what
// the reader refuses. limit_exceeded: a limit. out_of_memory. In a run, a glyph that
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
    // With Fit::none only: each outline is taken through this matrix
    // (16.16, on the outline's own axes, y upward) about its origin before
    // it is placed: x' = xx x + xy y, y' = yx x + yy y.
    bool transformed{false};
    i32 xx{0x10000};
    i32 xy{0};
    i32 yx{0};
    i32 yy{0x10000};
    // For a run: only glyphs that can show in these pixels are drawn:
    // [left, right) by [top, bottom), from the run's origin pixel, y
    // downward.
    bool windowed{false};
    i32 window_left{0};
    i32 window_top{0};
    i32 window_right{0};
    i32 window_bottom{0};
};

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

    // How many times a glyph has been read from the font so far.
    [[nodiscard]] usize glyphs_read() const noexcept;

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
    // with its outline moved by `shift` (26.6, y upward) last: `bounds` as
    // glyph_mask.hpp's measure gives them, `pixels` bounds.width *
    // bounds.height bytes, the top row first.
    [[nodiscard]] Status mask(u32 glyph, const DrawStyle& style, Point shift, Bounds& bounds, Vector<u8>& pixels) noexcept;

    // Where the run's ink can be, without drawing it.
    [[nodiscard]] Status measure_run(View<RunGlyph> glyphs, const DrawStyle& style, RunBox& box) noexcept;

    // The run's mask. `coverage` is replaced; its storage is reused.
    [[nodiscard]] Status draw_run(View<RunGlyph> glyphs, const DrawStyle& style, RunCoverage& coverage) noexcept;

    // How many times an outline has been fitted so far.
    [[nodiscard]] usize glyphs_fitted() const noexcept;

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
