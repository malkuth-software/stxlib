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
// What is kept, and for how long. The store is of a fixed size in bytes
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
// Bounds. Memory: at most kept_bytes + recent_bytes for the glyphs held,
// 4 bytes a glyph of the font for finding them, and the largest glyph met
// once more to read into; bytes_held gives what is allocated now.
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

// How much a store holds, in bytes of storage. A glyph takes 9 bytes a
// point, 4 a contour and 24 for itself. The storage for the kept glyphs
// never exceeds kept_bytes, nor that for the recent ones recent_bytes
// (storage grows by doubling, so what is held is at most half of each).
struct StoreLimits final {
    // The glyphs named in advance. Storage is taken as it is needed, not all
    // at once: these are where it stops.
    usize kept_bytes{1048576};
    // The glyphs in recent use, both halves together.
    usize recent_bytes{4194304};
};

// A glyph as the store holds it: borrowed from the store.
struct StoredGlyph final {
    View<Point> points{};
    View<PointTag> tags{};
    View<u32> contour_ends{};
    // In font units.
    i32 advance{0};
};

// ic: stxlib::glyphs::GlyphStore: generated from include/stxlib/glyph_store.ic.toml by tools/ic_declarations.py; edit
// ic: the declaration, not these lines. States: closed (initial), open. Thread: owner, unless a call says otherwise.
class GlyphStore final {
public:
    GlyphStore() noexcept = default;
    GlyphStore(GlyphStore&&) noexcept = default;
    GlyphStore& operator=(GlyphStore&&) noexcept = default;
    GlyphStore(const GlyphStore&) = delete;
    GlyphStore& operator=(const GlyphStore&) = delete;

    // Makes the store ready for `font`, holding nothing yet. `limits` are
    // those every glyph is read under.
    // ic: Admitted in any state; then open. Fails out_of_memory (the glyph index), partly changed; invalid_argument (a
    // ic: font the store cannot serve), partly changed.
    [[nodiscard]] OperationStatus open(const Font& font, const FontLimits& limits,
                                       const StoreLimits& store_limits = StoreLimits{}) noexcept;

    // Reads the glyphs of these characters into the kept part, in order,
    // for as long as there is room there; a character the font has no
    // glyph for, or whose glyph is refused, is passed over. `kept` is how
    // many glyphs are in the kept part afterwards.
    // ic: Admitted in open. In closed: failure invalid_argument, unchanged. Fails out_of_memory (storage for a kept
    // ic: glyph), partly changed.
    [[nodiscard]] OperationStatus keep(const Font& font, View<u32> characters, usize& kept) noexcept;

    // The glyph, read from `font` (the font opened) if it is not held.
    // ic: Admitted in open. In closed: failure invalid_argument, unchanged. Requires glyph < glyph_count, else failure
    // ic: invalid_argument, unchanged. Fails out_of_memory (storage for the glyph), partly changed; invalid_argument
    // ic: (the font refuses the glyph's outline), partly changed; limit_exceeded (the outline is beyond the store's
    // ic: limits), partly changed.
    [[nodiscard]] OperationStatus glyph(const Font& font, u32 glyph, StoredGlyph& stored) noexcept;

    // How many times a glyph has been read from the font so far.
    [[nodiscard]] usize glyphs_read() const noexcept { return read_; }

    // The bytes held, as allocated now.
    [[nodiscard]] usize bytes_held() const noexcept;

    // Whether what the store holds agrees with itself: each part's outlines
    // lie end to end and are counted right, each within its limit, and
    // every glyph the index names is where the index says. Linear in the
    // font's glyphs and in what is held: for tests and checked builds
    // (which check it at the end of every call).
    [[nodiscard]] bool whole() const noexcept;

    // Frees what is held.
    // ic: Admitted in any state; then closed.
    void release() noexcept;

private:
    // A glyph held in a part of the store.
    struct Record final {
        u32 glyph{0};
        u32 first_point{0};
        u32 point_count{0};
        u32 first_contour{0};
        u32 contour_count{0};
        // In font units.
        i32 advance{0};
    };
    // One part of the store: outlines end to end, and a record for each.
    struct Part final {
        Vector<Point> points{};
        Vector<PointTag> tags{};
        Vector<u32> contour_ends{};
        Vector<Record> records{};
    };

    [[nodiscard]] bool put(u32 part, u32 glyph, i32 advance, View<Point> points, View<PointTag> tags, View<u32> contour_ends) noexcept;
    void give_up(u32 part) noexcept;
    [[nodiscard]] OperationStatus place(const Font& font, u32 glyph, bool in_kept, StoredGlyph& stored) noexcept;

    // For each glyph of the font, where it is: 0 for nowhere; else the
    // part in the low two bits (3: refused) and above them the record's
    // number in the part plus one (or the kind of refusal).
    Vector<u32> index_{};
    // 0: the kept glyphs; 1 and 2: the halves of the recent ones.
    Part parts_[3]{};
    // What each part holds, in bytes (StoreLimits' measure).
    usize held_[3]{};
    u32 newer_{1};
    GlyphOutline loaded_{};
    FontLimits limits_{};
    StoreLimits store_limits_{};
    usize read_{0};
    // The font opened for, as told apart from another: its bytes, their
    // length, and where its outlines and character map are (two faces of
    // a collection share the bytes, not those).
    const u8* font_bytes_{nullptr};
    usize font_size_{0};
    FontTable font_glyf_{};
    FontTable font_charstrings_{};
    FontTable font_map_{};

    [[nodiscard]] bool opened_for(const Font& font) const noexcept;
};

} // namespace stxlib::glyphs
