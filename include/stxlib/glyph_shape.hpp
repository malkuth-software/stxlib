// stxlib::glyphs: shaping: from the characters of a run of text to the
// glyphs that draw it and where each goes. A font says, in its layout
// tables, which glyphs stand for which characters in which company
// (ligatures, alternates, forms in context) and how glyphs sit against one
// another (kerning, marks on their bases); shaping is reading that.
//
// A run is text of one font, one script and one direction, as a caller's
// layout has cut it. What comes back is the run's glyphs in the order they
// are drawn, left to right, each with its advance and an offset from where
// the pen stands, and the cluster of the text it came from.
//
// What is shaped. Everything a font's own substitution and positioning
// lookups say, for any script, by the rules HarfBuzz applies to scripts
// that need no more than that: Latin, Greek, Cyrillic, Armenian, Georgian,
// Hebrew, the CJK scripts and the many like them. Scripts whose letters
// must be reordered or joined by rules outside the font (Arabic, the Indic
// scripts, Khmer, Myanmar, Thai's older fonts, Hangul written as jamo) get
// the font's default lookups and no more: their text is drawn, not shaped.
//
// Where it comes from. A conversion of harfrust 0.16.0 (a Rust port of
// HarfBuzz), with HarfBuzz 14.6 as the measure: for the scripts shaped, the
// glyphs, clusters, advances and offsets are HarfBuzz's for a FreeType font
// at the same size, unhinted.
//
// Cost and bounds. The plan for a font, script, direction and set of
// features is made once and kept (ShaperLimits::plans of them, the least
// recently used given up). Shaping a run with a plan is linear in the run
// for a font's ordinary lookups; a lookup that cannot touch any glyph of
// the run is passed over with one test, and what has been read of the
// font's tables for a glyph is remembered in a small table of fixed size.
// A font's lookups may make a run longer or call one another: a result is
// no longer than 256 times the run (or 65,536 glyphs if that is more) nor
// than ShaperLimits::glyphs, the work no more than 4,096 operations a
// character (or 65,536), lookups nest no deeper than 64, and a run that
// passes any of these is refused, not cut. An operation is a bounded step:
// an item looked at by a rule (matched or passed over), a subtable, rule or
// ligature tried, 64 subtable digests tested, a cursive link followed; a
// glyph's box read from the font for marks placed without the font's help
// is 64 (boxes are remembered for a run, refusals too), an old kerning
// subtable's walk of the run its length. Making a plan has a
// budget of its own (256 times the table's size, from 4 MB to 128 MB, of
// visits and of bytes; a lookup past it is left out). Nothing is read
// outside the font's bytes whatever they hold.
//
// A Shaper is for one thread; the Typeface it reads is the program's.

#ifndef STXLIB_GLYPH_SHAPE_HPP
#define STXLIB_GLYPH_SHAPE_HPP

#include "stxlib/glyph_face.hpp"
#include "stxlib/result.hpp"
#include "stxlib/shared.hpp"
#include "stxlib/types.hpp"
#include "stxlib/unique.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs {

// The number of the rules above: it changes when the same run may come
// out differently.
inline constexpr u32 shape_profile_version = 1;

enum class TextDirection : u8 {
    left_to_right = 0,
    right_to_left = 1,
};

// Four letters as one number: the tag of a script ('L','a','t','n'), of a
// language system ('T','R','K',' ') or of a feature ('l','i','g','a').
[[nodiscard]] constexpr u32 text_tag(const char a, const char b, const char c, const char d) noexcept {
    const u32 result = (static_cast<u32>(static_cast<u8>(a)) << 24) | (static_cast<u32>(static_cast<u8>(b)) << 16) |
                       (static_cast<u32>(static_cast<u8>(c)) << 8) | static_cast<u32>(static_cast<u8>(d));
    return result;
}

// A feature turned on (value 1), off (0) or set to the number of an
// alternate, for the characters whose clusters are from `start` to before
// `end`. The features every run has need no asking: composition, local
// forms, required and standard ligatures, contextual forms, kerning and
// the placing of marks.
inline constexpr u32 whole_run = 0xFFFFFFFFU;
struct TextFeature final {
    u32 tag{0};
    u32 value{1};
    u32 start{0};
    u32 end{whole_run};
};

struct TextRun final {
    // The characters, as Unicode scalar values, in the order they are
    // read.
    View<u32> characters{};
    // A cluster for each character (where it is in the caller's text), not
    // going down along the run; or none, and a character's cluster is its
    // place in `characters`.
    View<u32> clusters{};
    TextDirection direction{TextDirection::left_to_right};
    // The script, as an ISO 15924 tag; 0: that of the first character
    // that has one of its own.
    u32 script{0};
    // The language system, as an OpenType tag; 0: the script's default.
    u32 language{0};
    // The size, in 26.6 pixels: advances and offsets are in 26.6 pixels.
    // 0: they are in the font's units.
    i64 size{0};
    View<TextFeature> features{};
};

struct ShapedGlyph final {
    u32 glyph{0};
    // The cluster of the text this glyph came from: the least of the
    // clusters of the characters it stands for. Glyphs of one cluster are
    // not to be parted (a base and its marks, a ligature).
    u32 cluster{0};
    // How far the pen moves after the glyph, and where the glyph is drawn
    // from where the pen stands (y upward).
    i32 x_advance{0};
    i32 y_advance{0};
    i32 x_offset{0};
    i32 y_offset{0};
    // The run cannot be cut before this glyph and its two halves shaped
    // apart to the same result.
    bool unsafe_to_break{false};
};

struct ShaperLimits final {
    // How many plans are kept.
    usize plans{8};
    // The longest run taken, in characters.
    usize characters{static_cast<usize>(1) << 20};
    // The most features a run may ask for.
    usize features{256};
    // The most glyphs a run may come to, whatever its length: the bound on
    // the buffer (about 60 bytes a glyph) and on the result (28), besides
    // the factor of 256 above. Never less than the run's characters.
    usize glyphs{static_cast<usize>(1) << 21};
};

namespace detail {
struct ShaperState;
} // namespace detail

class Shaper final {
public:
    Shaper() noexcept;
    Shaper(Shaper&&) noexcept;
    Shaper& operator=(Shaper&&) noexcept;
    Shaper(const Shaper&) = delete;
    Shaper& operator=(const Shaper&) = delete;
    ~Shaper() noexcept;

    // A shaper of the program's typeface: it holds the typeface for as
    // long as it is open.
    [[nodiscard]] OperationStatus open(const Shared<Typeface>& typeface, const ShaperLimits& limits = ShaperLimits{}) noexcept;

    // The run's glyphs, in the order they are drawn from left to right.
    // Failures: invalid_argument for a shaper not opened, clusters that
    // are not as many as the characters, or a size out of range;
    // limit_exceeded for a run longer than the limit, with more features
    // than the limit, or one whose shaping passes the bounds above;
    // out_of_memory. `glyphs` is then empty.
    [[nodiscard]] OperationStatus shape(const TextRun& run, Vector<ShapedGlyph>& glyphs) noexcept;

    // How many plans have been made since the shaper was opened (a count
    // that stays still while the same fonts, scripts and features recur).
    [[nodiscard]] usize plans_made() const noexcept;

    // The bytes this shaper holds, as allocated now.
    [[nodiscard]] usize bytes_held() const noexcept;

    // Frees everything; the shaper is as before open.
    void release() noexcept;

private:
    [[nodiscard]] OperationStatus shape_into(const TextRun& run, Vector<ShapedGlyph>& glyphs) noexcept;

    Unique<detail::ShaperState> state_{};
};

} // namespace stxlib::glyphs

#endif
