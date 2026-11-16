#pragma once

// stxlib::glyphs: a face to draw text from. One object for one face of one
// font: it reads the font (font_file.hpp), keeps the glyphs in use
// (glyph_store.hpp), fits them to a size (glyph_hinter.hpp), keeps the
// fitted outlines, and draws them (glyph_mask.hpp). A caller that has
// glyph numbers and positions (from shaping) needs nothing else of
// stxlib::glyphs.
//
// What is kept, and why at this level. Text is drawn with each glyph at a
// position of its own to 1/64 of a pixel, so a glyph's mask is different
// almost every time it is drawn and is not worth keeping. What is the same
// at every position is the glyph's outline fitted to the size: that is
// kept, by glyph, size and fit. Under it the glyph store keeps the outline
// in font units, which serves every size. A mask is made from the fitted
// outline each time, which is the cheap part.
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
// Starting up. open reads the font's tables and nothing of its glyphs; the
// hinter is not prepared until the first glyph is fitted with a hinted
// fit (about a millisecond for a font, once), and no glyph is read until
// it is asked for. warm reads and fits the glyphs of the characters given,
// for a caller that would rather pay before the first frame than in it:
// what it costs is what those glyphs would have cost at first use, no
// more, so warming a hundred letters at one size is well under a
// millisecond and warming a whole font is not asked of anybody.
//
// Memory. Fixed by FaceLimits: the glyph store's, and for the fitted
// outlines at most fitted_points points (8 bytes and a byte of kind each)
// and fitted_glyphs records. The working storage grows to the largest
// glyph and run met and is kept. bytes_held gives what is held now. Once
// the glyphs of a text are held, drawing it again allocates nothing.
//
// One thread at a time. The font's bytes are borrowed: they must outlive
// the face. What a call hands back as a view (FittedGlyph) is good until
// the next call on the face.
//
// What it is made to last through. The interface is whole numbers in
// stated units (26.6 pixels, 16.16 matrices), plain structures whose
// members have defaults (a later version adds members, with defaults that
// mean what was meant before), and one object whose inside is not shown.
// What it does not do yet, and has room for: synthesized bold, colour
// glyphs, variable fonts, coverage for striped displays, upright text in
// columns. Each would be a further member of RunStyle or a further Fit,
// not a change to what is here.
//
// Failure. invalid_argument: a face not opened, a glyph the font has not,
// a size out of range, a matrix with a hinted fit, or what the reader
// refuses. limit_exceeded: a limit. out_of_memory. In a run, a glyph that
// cannot be had is counted (missing_glyphs) and draws nothing: the run is
// still drawn. Nothing throws.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_mask.hpp"
#include "stxlib/glyph_store.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/unique.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs {

// What this interface gives for the same font, glyphs, positions, sizes
// and fit is fixed: the same numbers and the same bytes on every host and
// in every later version that carries this number. A change to any of
// them (a rule of the rasterizer, of the hinter, of how a run is put
// together) changes this number, so that a caller that keeps masks keeps
// the number beside them and makes them again when it differs.
inline constexpr u32 face_profile_version = 1;

enum class Fit : u8 {
    none = 0,
    light = 1,
    monochrome = 2,
};

struct FaceLimits final {
    FontLimits font{};
    StoreLimits store{};
    Limits mask{};
    // Fitted outlines, both halves together.
    u32 fitted_points{131072};
    u32 fitted_glyphs{4096};
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

struct RunStyle final {
    // 26.6 pixels to the em.
    i64 size{0};
    Fit fit{Fit::light};
    // With Fit::none only: each outline is taken through this matrix
    // (16.16, on the outline's own axes, y upward) about its origin before
    // it is placed: x' = xx x + xy y, y' = yx x + yy y.
    bool transformed{false};
    i32 xx{0x10000};
    i32 xy{0};
    i32 yx{0};
    i32 yy{0x10000};
    // Only glyphs that can show in these pixels are drawn: [left, right) by
    // [top, bottom), from the run's origin pixel, y downward.
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
// What a face holds (src/algorithms/glyphs/glyph_face.cpp). It is not part
// of this interface, so that it can change without a caller changing.
struct FaceState;
} // namespace detail

class Face final {
public:
    Face() noexcept;
    Face(Face&&) noexcept;
    Face& operator=(Face&&) noexcept;
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;
    ~Face() noexcept;

    // Opens face `face_index` of the font in `bytes` (0 for a font that is
    // not a collection). Nothing of its glyphs is read.
    [[nodiscard]] Status open(View<u8> bytes, u32 face_index, const FaceLimits& limits = FaceLimits{}) noexcept;

    // The font opened (its numbers, and glyph_of and advance_of of it), or
    // null for a face not opened. Good until the face is released or
    // opened again.
    [[nodiscard]] const Font* font() const noexcept;

    // Reads the glyphs of these characters into the kept part of the glyph
    // store and, if `size` is not 0, fits them at it. `warmed` is how many
    // glyphs were had.
    [[nodiscard]] Status warm(View<u32> characters, i64 size, Fit fit, usize& warmed) noexcept;

    // The glyph's outline at `size` (26.6 pixels to the em, at most 2^24).
    [[nodiscard]] Status fitted(u32 glyph, i64 size, Fit fit, FittedGlyph& glyph_out) noexcept;

    // The glyph's mask with its outline moved by `shift` (26.6, y upward)
    // first: `bounds` as glyph_mask.hpp's measure gives them, `pixels`
    // bounds.width * bounds.height bytes, the top row first.
    [[nodiscard]] Status mask(u32 glyph, i64 size, Fit fit, Point shift, Bounds& bounds, Vector<u8>& pixels) noexcept;

    // Where the run's ink can be, without drawing it.
    [[nodiscard]] Status measure_run(View<RunGlyph> glyphs, const RunStyle& style, RunBox& box) noexcept;

    // The run's mask. `coverage` is replaced; its storage is reused.
    [[nodiscard]] Status draw_run(View<RunGlyph> glyphs, const RunStyle& style, RunCoverage& coverage) noexcept;

    // How many times a glyph has been read from the font, and how many
    // times an outline has been fitted, so far.
    [[nodiscard]] usize glyphs_read() const noexcept;
    [[nodiscard]] usize glyphs_fitted() const noexcept;

    // The bytes of the glyphs and fitted outlines held and of the working
    // storage, as allocated now (the hinter's own working memory, a few
    // kilobytes, is not counted).
    [[nodiscard]] usize bytes_held() const noexcept;

    // Frees everything; the face is as before open.
    void release() noexcept;

private:
    Unique<detail::FaceState> state_{};
};

} // namespace stxlib::glyphs
