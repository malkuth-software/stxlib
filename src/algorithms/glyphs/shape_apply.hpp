// stxlib::glyphs: applying a font's lookups to a run: the substitutions
// (GSUB) and the positions (GPOS) of a plan. After harfrust 0.16.0,
// src/ot/apply.rs, src/ot/layout.rs, src/ot/lookup/ and src/ot/gpos/
// (HarfBuzz's hb-ot-layout-gsubgpos and its GSUB and GPOS).
//
// What is read. Lookups of every kind the format has for 16-bit glyph
// numbers: single, multiple, alternate and ligature substitution, the
// contextual and chained contextual kinds in their three formats, the
// reverse chained single substitution; single and pair adjustment, cursive
// attachment, marks to bases, to ligatures and to marks. Not read: the
// tables' forms for more than 65,535 glyphs, device tables (text is laid
// out unhinted) and variations.
//
// No function here calls itself. A contextual lookup names other lookups
// to apply inside what it matched, and those may be contextual too: that
// nesting is a stack of frames of a fixed depth (apply_most_nesting, as
// HarfBuzz allows), each holding the places it matched.
//
// Bounds. Every step that could repeat is charged to the buffer's count of
// operations (ShapeBuffer::max_ops, set from the run's length), and the run
// may not grow past ShapeBuffer::max_len; when either is spent the buffer
// says so (`successful` false) and nothing more is applied. A sequence
// matched is no longer than apply_most_context glyphs. Every read of the
// font is behind a check of its place (font_reader.hpp), and a table that
// lies about its counts is read as if it ended where it does.

#ifndef STXLIB_ALGORITHMS_GLYPHS_SHAPE_APPLY_HPP
#define STXLIB_ALGORITHMS_GLYPHS_SHAPE_APPLY_HPP

#include "algorithms/glyphs/shape_buffer.hpp"
#include "algorithms/glyphs/shape_plan.hpp"
#include "algorithms/glyphs/shape_unicode.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::glyphs::detail {

inline constexpr usize apply_most_nesting = 64;
inline constexpr usize apply_most_context = 64;

// How a glyph is attached to another, in its position.
inline constexpr u8 attached_as_mark = 1;
inline constexpr u8 attached_cursively = 2;

// ---- what is known of an item ----------------------------------------------------------------------

[[nodiscard]] constexpr u32 info_category(const ShapeGlyph& item) noexcept {
    const u32 result = item.unicode_props & unicode_general_category;
    return result;
}

[[nodiscard]] constexpr bool info_is_unicode_mark(const ShapeGlyph& item) noexcept {
    const bool result = category_is_mark(info_category(item));
    return result;
}

// The class a mark is put in order by; 0 for what is no mark.
[[nodiscard]] constexpr u32 info_mark_class(const ShapeGlyph& item) noexcept {
    const u32 result = info_is_unicode_mark(item) ? static_cast<u32>(item.unicode_props) >> 8 : 0;
    return result;
}

constexpr void info_set_mark_class(ShapeGlyph& item, const u32 mark_class) noexcept {
    if (info_is_unicode_mark(item)) item.unicode_props = static_cast<u16>(((mark_class & 0xFFU) << 8) | (item.unicode_props & 0xFFU));
}

// The category is replaced and what the high byte held is dropped.
constexpr void info_set_category(ShapeGlyph& item, const u32 category) noexcept {
    item.unicode_props = static_cast<u16>(category | (item.unicode_props & (0xFFU & ~unicode_general_category)));
}

[[nodiscard]] constexpr bool info_is_space(const ShapeGlyph& item) noexcept {
    const bool result = info_category(item) == category_space_separator;
    return result;
}

[[nodiscard]] constexpr u32 info_space_fallback(const ShapeGlyph& item) noexcept {
    const u32 result = info_is_space(item) ? static_cast<u32>(item.unicode_props) >> 8 : space_none;
    return result;
}

constexpr void info_set_space_fallback(ShapeGlyph& item, const u32 space) noexcept {
    if (info_is_space(item)) item.unicode_props = static_cast<u16>(((space & 0xFFU) << 8) | (item.unicode_props & 0xFFU));
}

[[nodiscard]] constexpr bool info_substituted(const ShapeGlyph& item) noexcept {
    const bool result = (item.glyph_props & glyph_substituted) != 0;
    return result;
}

[[nodiscard]] constexpr bool info_ligated(const ShapeGlyph& item) noexcept {
    const bool result = (item.glyph_props & glyph_ligated) != 0;
    return result;
}

[[nodiscard]] constexpr bool info_multiplied(const ShapeGlyph& item) noexcept {
    const bool result = (item.glyph_props & glyph_multiplied) != 0;
    return result;
}

// Not to show, unless a substitution has made a glyph of it.
[[nodiscard]] constexpr bool info_is_default_ignorable(const ShapeGlyph& item) noexcept {
    const bool result = (item.unicode_props & unicode_ignorable) != 0 && !info_substituted(item);
    return result;
}

[[nodiscard]] constexpr bool info_is_hidden(const ShapeGlyph& item) noexcept {
    const bool result = (item.unicode_props & unicode_hidden) != 0;
    return result;
}

[[nodiscard]] constexpr bool info_is_continuation(const ShapeGlyph& item) noexcept {
    const bool result = (item.unicode_props & unicode_continuation) != 0;
    return result;
}

constexpr void info_set_continuation(ShapeGlyph& item, u32& scratch) noexcept {
    scratch = scratch | scratch_has_continuations;
    item.unicode_props = static_cast<u16>(item.unicode_props | unicode_continuation);
}

[[nodiscard]] constexpr bool info_is_format(const ShapeGlyph& item) noexcept {
    const bool result = info_category(item) == category_format;
    return result;
}

[[nodiscard]] constexpr bool info_is_zwnj(const ShapeGlyph& item) noexcept {
    const bool result = info_is_format(item) && (item.unicode_props & unicode_format_zwnj) != 0;
    return result;
}

[[nodiscard]] constexpr bool info_is_zwj(const ShapeGlyph& item) noexcept {
    const bool result = info_is_format(item) && (item.unicode_props & unicode_format_zwj) != 0;
    return result;
}

// A variation selector the font had no sequence for is remembered as a
// format character with a bit of its own.
[[nodiscard]] constexpr bool info_is_variation_selector(const ShapeGlyph& item) noexcept {
    const bool result = info_is_format(item) && (item.unicode_props & unicode_format_variation_selector) != 0;
    return result;
}

constexpr void info_set_variation_selector(ShapeGlyph& item, const bool remembered) noexcept {
    if (remembered) {
        info_set_category(item, category_format);
        item.unicode_props = static_cast<u16>(item.unicode_props | unicode_format_variation_selector);
    } else {
        info_set_category(item, category_non_spacing_mark);
    }
}

[[nodiscard]] constexpr bool info_is_base(const ShapeGlyph& item) noexcept {
    const bool result = (item.glyph_props & glyph_is_base) != 0;
    return result;
}

[[nodiscard]] constexpr bool info_is_ligature(const ShapeGlyph& item) noexcept {
    const bool result = (item.glyph_props & glyph_is_ligature) != 0;
    return result;
}

[[nodiscard]] constexpr bool info_is_mark(const ShapeGlyph& item) noexcept {
    const bool result = (item.glyph_props & glyph_is_mark) != 0;
    return result;
}

// A ligature's number (1 to 7, 0 for none) and what an item is of it: the
// ligature itself, with how many parts it has, or a mark on one of them.
inline constexpr u32 lig_is_base = 0x10;

[[nodiscard]] constexpr u32 info_lig_id(const ShapeGlyph& item) noexcept {
    const u32 result = static_cast<u32>(item.lig_props) >> 5;
    return result;
}

[[nodiscard]] constexpr bool info_is_lig_base(const ShapeGlyph& item) noexcept {
    const bool result = (item.lig_props & lig_is_base) != 0;
    return result;
}

[[nodiscard]] constexpr u32 info_lig_comp(const ShapeGlyph& item) noexcept {
    const u32 result = info_is_lig_base(item) ? 0 : (item.lig_props & 0x0FU);
    return result;
}

[[nodiscard]] constexpr u32 info_lig_num_comps(const ShapeGlyph& item) noexcept {
    const u32 result = (info_is_ligature(item) && info_is_lig_base(item)) ? (item.lig_props & 0x0FU) : 1;
    return result;
}

[[nodiscard]] constexpr u32 info_lig_num_comps_in_ligation(const ShapeGlyph& item) noexcept {
    const u32 result = (info_multiplied(item) && info_lig_comp(item) != 0) ? 0 : info_lig_num_comps(item);
    return result;
}

constexpr void info_set_lig_for_ligature(ShapeGlyph& item, const u32 lig_id, const u32 parts) noexcept {
    item.lig_props = static_cast<u8>(((lig_id & 7U) << 5) | lig_is_base | (parts & 0x0FU));
}

constexpr void info_set_lig_for_mark(ShapeGlyph& item, const u32 lig_id, const u32 part) noexcept {
    item.lig_props = static_cast<u8>(((lig_id & 7U) << 5) | (part & 0x0FU));
}

// ---- scaling ------------------------------------------------------------------------------------------

// From the font's units to the units of the result: a whole-number rule
// for advances and adjustments, and HarfBuzz's single-precision one for
// the places glyphs attach at. In font units both are 1.
struct ShapeScale final {
    i64 x_mult{65536};
    i64 y_mult{65536};
    float x_multf{1.0F};
    float y_multf{1.0F};
    // An em at the size, in the units of the result.
    i64 em{0};
};

[[nodiscard]] constexpr i32 saturate(const i64 value) noexcept {
    const i64 low = -2147483647LL - 1;
    const i64 high = 2147483647LL;
    const i32 result = static_cast<i32>(value < low ? low : (value > high ? high : value));
    return result;
}

// A value of the font (a 16-bit field; a wider one is taken at the nearest
// 16-bit value) at a multiplier of no more than 2^47 either way (a size of
// 2^24 pixels at 16 units to the em is 2^36): the product is inside 63
// bits, and the result is saturated to 32.
[[nodiscard]] constexpr i32 scale_value(const i32 value, const i64 mult) noexcept {
    const i64 most_mult = static_cast<i64>(1) << 47;
    const i64 field = value < -32768 ? -32768 : (value > 32767 ? 32767 : value);
    const i64 factor = mult < -most_mult ? -most_mult : (mult > most_mult ? most_mult : mult);
    const i32 result = saturate((field * factor + 32768) >> 16);
    return result;
}

// The nearest whole number to a small real one, as HarfBuzz rounds: a half
// is added in single precision and the sum rounded down.
[[nodiscard]] constexpr i32 round_real(const float value) noexcept {
    const float raised = value + 0.5F;
    if (!(raised > -1073741824.0F && raised < 1073741824.0F)) return 0;
    const i32 whole = static_cast<i32>(raised);
    const i32 result = (raised < 0.0F && static_cast<float>(whole) != raised) ? whole - 1 : whole;
    return result;
}


// ---- the work of applying ----------------------------------------------------------------------------

// A contextual lookup at work on the lookups it names.
struct ApplyFrame final {
    u32 lookup_props{0};
    u32 positions[apply_most_context]{};
    i64 count{0};
    i64 end{0};
    usize records{0};
    u32 record_count{0};
    u32 next_record{0};
    usize original_length{0};
    i64 sequence_index{0};
    bool stopped{false};
};

// What has been read of a font's tables for a glyph, remembered: its place
// in a coverage, its class in a class definition, its properties, the
// glyph of a character. A table of fixed size, each answer at the place
// its question hashes to, a later one over an earlier: an answer found is
// the answer the font gives, and one not found is read again. It is for
// one font (a Shaper's), whose tables do not change.
inline constexpr usize memo_size = 2048;
inline constexpr u32 memo_coverage = 1;
inline constexpr u32 memo_class = 2;
inline constexpr u32 memo_properties = 3;
inline constexpr u32 memo_character = 4;
inline constexpr u32 memo_ligature = 5;
// Which table a place is in: substitutions 0, positions 1, definitions 2,
// the character map 3.
struct MemoEntry final {
    u64 key{0};
    u32 value{0};
};

[[nodiscard]] constexpr u64 memo_key(const u32 what, const u32 table, const usize place, const u32 number) noexcept {
    const u64 result = (static_cast<u64>(1) << 63) | (static_cast<u64>(what & 7U) << 55) | (static_cast<u64>(table & 3U) << 53) |
                       ((static_cast<u64>(place) & 0xFFFFFFFFU) << 21) | (number & 0x1FFFFFU);
    return result;
}

[[nodiscard]] constexpr usize memo_slot(const u64 key) noexcept {
    const usize result = static_cast<usize>((key ^ (key >> 19) ^ (key >> 37) ^ (key >> 53)) & (memo_size - 1));
    return result;
}

[[nodiscard]] inline bool memo_find(const Vector<MemoEntry>& memo, const u64 key, u32& value) noexcept {
    if (memo.count() != memo_size) return false;
    const MemoEntry& entry = memo[memo_slot(key)];
    if (entry.key != key) return false;
    value = entry.value;
    return true;
}

inline void memo_keep(Vector<MemoEntry>& memo, const u64 key, const u32 value) noexcept {
    if (memo.count() == memo_size) memo[memo_slot(key)] = MemoEntry{.key = key, .value = value};
}

// A pair of glyphs and what a kerning lookup of a plan gives it,
// remembered: text repeats its pairs.
inline constexpr usize pair_memo_size = 1024;
struct PairEntry final {
    u64 serial{0};
    u16 lookup{0};
    u16 first{0};
    u16 second{0};
    i16 value{0};
};

// The different glyphs a run has had, as far as there are few: a lookup
// that begins at none of them is not gone through at all.
inline constexpr usize apply_most_distinct = 96;

// The working memory of applying lookups, kept from run to run.
struct ApplyWork final {
    Vector<ApplyFrame> frames{};
    Vector<MemoEntry> memo{};
    Vector<PairEntry> pairs{};
    // The advances of the run summed from its beginning, along x then y,
    // as finish_positions takes them back for a mark.
    Vector<i64> totals{};
    // Of the run as it stands when its positions begin: the glyph
    // properties any item has, the mask bits every item has, and whether
    // any is a character not to show. A lookup that sees every item alike
    // needs no looking at each.
    u32 run_props{0};
    u32 run_mask{0};
    bool run_ignorable{false};
    u32 distinct[apply_most_distinct]{};
    usize distinct_count{0};
    // More different glyphs than the list holds: it says nothing.
    bool distinct_over{false};
    // Which glyphs are in the list, each at the place it hashes to with
    // the number of the run, so that nothing is cleared between runs.
    u64 seen[256]{};
    u64 run_number{0};
};

// A glyph of the run, for the list of its different glyphs.
inline void note_glyph(ApplyWork& work, const u32 glyph) noexcept {
    const usize slot = (glyph ^ (glyph >> 8)) & 0xFFU;
    const u64 mark = (work.run_number << 16) | (glyph & 0xFFFFU);
    if (work.seen[slot] == mark) return;
    work.seen[slot] = mark;
    if (work.distinct_count < apply_most_distinct) {
        work.distinct[work.distinct_count] = glyph;
        work.distinct_count = work.distinct_count + 1;
    } else {
        work.distinct_over = true;
    }
}

// Room for the frames and the memo; false for want of memory.
[[nodiscard]] bool prepare_work(ApplyWork& work) noexcept;

// The substitutions of the plan. The buffer's items are glyphs with their
// masks; their glyph and ligature properties are set here.
void apply_substitutions(const ShapePlan& plan, ShapeBuffer& buffer, ApplyWork& work, GlyphDigest& digest) noexcept;

// The positions of the plan: adjustments and attachments, each attached
// glyph still naming what it is attached to (finish_positions resolves
// them).
void apply_positions(const ShapePlan& plan, ShapeBuffer& buffer, ApplyWork& work, const ShapeScale& scale, GlyphDigest& digest) noexcept;

// Before the positions: nothing is attached. After: each attached glyph's
// offset takes in that of what it is attached to.
void start_positions(ShapeBuffer& buffer) noexcept;
// Linear in the run: the advances between a mark and its base are taken
// from running totals. Fails (the buffer not successful) only for want of
// memory for them.
void finish_positions(ShapeBuffer& buffer, ApplyWork& work) noexcept;

// Every item's glyph properties from the font's glyph classes, its
// ligature properties cleared, and the digest of the run's glyphs.
void start_substitutions(const ShapePlan& plan, ShapeBuffer& buffer, ApplyWork& work, GlyphDigest& digest) noexcept;

// The old kerning table, for a font whose positions have no kerning.
void apply_old_kerning(const Font& font, const ShapePlan& plan, ShapeBuffer& buffer, const ShapeScale& scale) noexcept;

} // namespace stxlib::glyphs::detail

#endif
