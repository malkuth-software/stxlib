#pragma once

// stxlib::glyphs: the glyphs of a font in use, kept in memory. A font's
// bytes are whatever the font's maker put there; a glyph's outline is read
// out of them by the font reader (font_file.hpp), which refuses what is
// wrong and bounds what it takes. The store keeps what the reader gave, so
// that the glyphs a text is made of are read from the font once and then
// served from memory: the hinter (glyph_hinter.hpp) and whatever draws a
// glyph work from the stored outline, at any number of sizes.
//
// A stored glyph is its outline in font units as load_glyph gives it
// (points in contour order, each tagged, and the last point of each
// contour) and its advance in font units: one entry serves every size.
//
// What is kept, and for how long. The store is of a fixed size
// (StoreLimits), in two parts.
// - The kept glyphs: those the caller names in advance (keep: the letters
//   a text is most likely made of). They stay until the store is released.
// - The recent glyphs: everything else that is asked for. These are held
//   in two halves, a newer and an older. A glyph read from the font goes
//   into the newer half; a glyph found in the older half is moved to the
//   newer. When the newer half is full the older half is given up, the
//   newer becomes the older, and a fresh newer half begins. So a glyph
//   that goes unused while a half fills twice is forgotten, and one in use
//   never is. A forgotten glyph is read from the font again when asked
//   for.
// A glyph the reader refused is remembered as refused, with the kind of
// refusal, and is not read again. A glyph too large for a half is read
// each time it is asked for and not kept.
//
// Bounds. Memory: at most kept_points + recent_points points (9 bytes a
// point and at most 4 more a contour), and 24 bytes a glyph of the font.
// Asking for a glyph that is held is constant time; moving one to the
// newer half is linear in its points; giving up a half is linear in the
// glyphs that were in it.
//
// What is borrowed. A StoredGlyph's views are into the store: they are
// good until the store is next asked for a glyph (which can move or forget
// what it holds), told to keep one, or released. Take the glyph again
// rather than hold on to it.
//
// Failure. invalid_argument: a glyph the font has not, a store not opened
// for this font, or what load_glyph refuses. limit_exceeded: a FontLimits
// bound. out_of_memory: the glyph stays unread. Nothing throws.

#include "stxlib/font_file.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs {

// How much a store holds, in points of outline.
struct StoreLimits final {
    // The glyphs named in advance: about a thousand Latin letters.
    u32 kept_points{32768};
    // The glyphs in recent use, both halves together.
    u32 recent_points{131072};
};

// A glyph as the store holds it: borrowed from the store.
struct StoredGlyph final {
    View<Point> points{};
    View<PointTag> tags{};
    View<u32> contour_ends{};
    // In font units.
    i32 advance{0};
};

class GlyphStore final {
public:
    GlyphStore() noexcept = default;
    GlyphStore(GlyphStore&&) noexcept = default;
    GlyphStore& operator=(GlyphStore&&) noexcept = default;
    GlyphStore(const GlyphStore&) = delete;
    GlyphStore& operator=(const GlyphStore&) = delete;

    // Makes the store ready for `font`, holding nothing yet. `limits` are
    // those every glyph is read under.
    [[nodiscard]] Status open(const Font& font, const FontLimits& limits, const StoreLimits& store_limits = StoreLimits{}) noexcept;

    // Reads the glyphs of these characters into the kept part, in order,
    // for as long as there is room there; a character the font has no
    // glyph for, or whose glyph is refused, is passed over. `kept` is how
    // many glyphs are in the kept part afterwards.
    [[nodiscard]] Status keep(const Font& font, View<u32> characters, usize& kept) noexcept;

    // The glyph, read from `font` (the font opened) if it is not held.
    [[nodiscard]] Status glyph(const Font& font, u32 glyph, StoredGlyph& stored) noexcept;

    // How many times a glyph has been read from the font so far.
    [[nodiscard]] usize glyphs_read() const noexcept { return read_; }

    // Frees what is held.
    void release() noexcept;

private:
    struct Entry final {
        // Where the glyph is (0: nowhere), then its place there; for a
        // refused glyph `first_point` is the kind of refusal.
        u32 where{0};
        u32 first_point{0};
        u32 point_count{0};
        u32 first_contour{0};
        u32 contour_count{0};
        // In font units.
        i32 advance{0};
    };
    // One part of the store: outlines end to end, and whose they are.
    struct Part final {
        Vector<Point> points{};
        Vector<PointTag> tags{};
        Vector<u32> contour_ends{};
        Vector<u32> glyphs{};
    };

    [[nodiscard]] bool put(Part& part, u32 where, u32 glyph, i32 advance, View<Point> points, View<PointTag> tags,
                           View<u32> contour_ends) noexcept;
    void give_up(Part& part) noexcept;
    [[nodiscard]] Status place(const Font& font, u32 glyph, bool in_kept, StoredGlyph& stored) noexcept;

    Vector<Entry> entries_{};
    // 0: the kept glyphs; 1 and 2: the halves of the recent ones.
    Part parts_[3]{};
    u32 newer_{1};
    GlyphOutline loaded_{};
    FontLimits limits_{};
    StoreLimits store_limits_{};
    usize read_{0};
};

} // namespace stxlib::glyphs
