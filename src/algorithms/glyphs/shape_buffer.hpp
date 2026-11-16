// stxlib::glyphs: the shaper's buffer: the characters of a run of text
// turning into the glyphs of it. After harfrust 0.16.0, src/buffer.rs (itself
// after HarfBuzz's hb-buffer): the items, their clusters, the two halves a
// substitution works between, and the positions.
//
// How it is used. Text is added as characters, each with the place in the
// text it came from (its cluster). Substitution goes through the items
// once for a lookup: it reads from `info` at `idx` and writes to the
// output, and sync makes the output the items again. Positioning works on
// the items in place, with a position for each.
//
// Clusters. When items are replaced, merged or reordered, their clusters
// are joined (the least of them) so that a cluster always names a stretch
// of the text that maps to a stretch of the glyphs: the cluster level says
// how far that goes.
//
// Where it differs from harfrust: the output is always a second array
// (harfrust writes over the items while the output is no longer than what
// was read, as a saving); what is in the arrays is the same. The digest
// and the set of glyphs harfrust keeps to skip lookups early are not kept.
//
// Bounds. Shaping a run may lengthen it, and a font's lookups may loop: the
// buffer has a most length and a count of operations left (enter sets
// both from the run's length, as HarfBuzz does), and when either is spent
// `successful` is false and the shaping is given up. Nothing reads or
// writes outside the arrays: an index out of range sets `successful` false.

#ifndef STXLIB_ALGORITHMS_GLYPHS_SHAPE_BUFFER_HPP
#define STXLIB_ALGORITHMS_GLYPHS_SHAPE_BUFFER_HPP

#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs::detail {

// What a glyph's mask says of it to the caller, in its low bits; the bits
// above them are the features that apply to it.
inline constexpr u32 glyph_unsafe_to_break = 0x1;
inline constexpr u32 glyph_unsafe_to_concat = 0x2;
inline constexpr u32 glyph_safe_to_insert_tatweel = 0x4;
inline constexpr u32 glyph_flags_defined = 0x7;

// What is known of a character, as bits (unicode_props).
inline constexpr u32 unicode_general_category = 0x001F;
inline constexpr u32 unicode_ignorable = 0x0020;
inline constexpr u32 unicode_hidden = 0x0040;
inline constexpr u32 unicode_continuation = 0x0080;
inline constexpr u32 unicode_format_zwj = 0x0100;
inline constexpr u32 unicode_format_zwnj = 0x0200;
inline constexpr u32 unicode_format_variation_selector = 0x0400;

// What is known of a glyph, as bits (glyph_props): the first three are the
// lookup flags' own numbers.
inline constexpr u32 glyph_is_base = 0x02;
inline constexpr u32 glyph_is_ligature = 0x04;
inline constexpr u32 glyph_is_mark = 0x08;
inline constexpr u32 glyph_class_bits = 0x0E;
inline constexpr u32 glyph_substituted = 0x10;
inline constexpr u32 glyph_ligated = 0x20;
inline constexpr u32 glyph_multiplied = 0x40;
inline constexpr u32 glyph_preserved_bits = 0x70;

// A character before shaping, a glyph after.
struct ShapeGlyph final {
    u32 glyph{0};
    u32 mask{0};
    u32 cluster{0};
    u16 glyph_props{0};
    u8 lig_props{0};
    u8 syllable{0};
    u16 unicode_props{0};
    // A number for whichever stage is at work on the buffer.
    u16 stage_data{0};
};

struct ShapePosition final {
    i32 x_advance{0};
    i32 y_advance{0};
    i32 x_offset{0};
    i32 y_offset{0};
    // The glyph this one is attached to, counted from this one (negative:
    // before it); 0 for none. And how.
    i16 attach_chain{0};
    u8 attach_type{0};
};

// How far clusters are joined. monotone: clusters never go down along the
// run. graphemes: a mark is of its base's cluster.
enum class ClusterLevel : u8 {
    monotone_graphemes = 0,
    monotone_characters = 1,
    characters = 2,
    graphemes = 3,
};

// Which way a run is set. unset: not said yet.
enum class ShapeDirection : u8 {
    unset = 0,
    left_to_right = 1,
    right_to_left = 2,
    top_to_bottom = 3,
    bottom_to_top = 4,
};

// How the text is to be treated, as bits.
inline constexpr u32 buffer_beginning_of_text = 0x01;
inline constexpr u32 buffer_end_of_text = 0x02;
inline constexpr u32 buffer_preserve_default_ignorables = 0x04;
inline constexpr u32 buffer_remove_default_ignorables = 0x08;
inline constexpr u32 buffer_produce_unsafe_to_concat = 0x40;
inline constexpr u32 buffer_produce_safe_to_insert_tatweel = 0x80;

// What shaping has noticed of the buffer, as bits (scratch).
inline constexpr u32 scratch_has_fraction_slash = 0x01;
inline constexpr u32 scratch_has_default_ignorables = 0x02;
inline constexpr u32 scratch_has_space_fallback = 0x04;
inline constexpr u32 scratch_has_attachment = 0x08;
inline constexpr u32 scratch_has_grapheme_joiner = 0x10;
inline constexpr u32 scratch_has_variation_selector_fallback = 0x40;
inline constexpr u32 scratch_has_continuations = 0x80;

// "To the end of the buffer", for a range's end.
inline constexpr usize to_the_end = ~static_cast<usize>(0);

inline constexpr usize context_length = 5;

// Whether two neighbouring items are of one group.
using SameGroup = bool (*)(const ShapeGlyph& before, const ShapeGlyph& after) noexcept;
// Whether the first of two items sorts after the second.
using SortsAfter = bool (*)(const ShapeGlyph& first, const ShapeGlyph& second) noexcept;
// Whether an item is to be taken out.
using Unwanted = bool (*)(const ShapeGlyph& item) noexcept;

struct ShapeBuffer final {
    // How the text is treated.
    u32 flags{0};
    ClusterLevel cluster_level{ClusterLevel::monotone_graphemes};
    // The glyph for a character that is not to show, when there is one.
    bool has_invisible{false};
    u32 invisible{0};
    // The run's direction and its script (an ISO 15924 tag; 0: not said
    // yet). clear unsets both.
    ShapeDirection direction{ShapeDirection::unset};
    u32 script{0};
    // What it holds.
    bool successful{true};
    bool have_output{false};
    bool have_positions{false};
    usize idx{0};
    usize len{0};
    usize out_len{0};
    Vector<ShapeGlyph> info{};
    Vector<ShapeGlyph> out{};
    Vector<ShapePosition> pos{};
    // The characters before and after the run, nearest first.
    u32 context[2][context_length]{};
    usize context_len[2]{};
    u8 serial{0};
    u32 scratch{0};
    usize max_len{0x3FFFFFFF};
    i32 max_ops{0x1FFFFFFF};

    // Empties the buffer; how the text is treated stays. The arrays keep
    // their room (release gives it back).
    void clear() noexcept;
    // Empties it and puts how the text is treated back as at first.
    void reset() noexcept;

    // A character at the end, of cluster `cluster`.
    void add(u32 character, u32 cluster) noexcept;
    // A character before (side 0) or after (side 1) the run; no more than
    // five a side are kept.
    void add_context(usize side, u32 character) noexcept;

    void reverse() noexcept;
    void reverse_range(usize start, usize end) noexcept;
    // Each group is put in reverse and then the whole, which leaves the
    // groups in reverse order with each group's items in order.
    void reverse_groups(SameGroup same, bool merge) noexcept;
    void reverse_clusters() noexcept;
    void reset_clusters() noexcept;
    // The end of the group that begins at `start`.
    [[nodiscard]] usize group_end(usize start, SameGroup same) const noexcept;

    // How many items lie behind the one being read, and how many from it
    // on.
    [[nodiscard]] usize backtrack_len() const noexcept {
        const usize result = have_output ? out_len : idx;
        return result;
    }
    [[nodiscard]] usize lookahead_len() const noexcept {
        const usize result = len - idx;
        return result;
    }

    // Substitution: the output begun, items passed to it, and the output
    // made the items. sync is false when the shaping has failed.
    void clear_output() noexcept;
    [[nodiscard]] bool sync() noexcept;
    void clear_positions() noexcept;
    // `num_in` items read are replaced by the glyphs given; each takes on
    // what the first item read had.
    void replace_glyphs(usize num_in, View<u32> glyphs) noexcept;
    void replace_glyph(u32 glyph) noexcept;
    // A glyph written with nothing read: it takes on what the item being
    // read has, or the last one written when none is left.
    void output_glyph(u32 glyph) noexcept;
    void output_info(const ShapeGlyph& item) noexcept;
    void copy_glyph() noexcept;
    void next_glyph() noexcept {
        if (have_output) {
            if (!ensure(out_len + 1)) return;
            if (idx >= len) {
                successful = false;
                return;
            }
            out[out_len] = info[idx];
            out_len = out_len + 1;
        }
        idx = idx + 1;
    }
    void next_glyphs(const usize count) noexcept {
        if (have_output) {
            if (!ensure(out_len + count)) return;
            if (idx + count > len) {
                successful = false;
                return;
            }
            for (usize index = 0; index < count; ++index) out[out_len + index] = info[idx + index];
            out_len = out_len + count;
        }
        idx = idx + count;
    }
    void skip_glyph() noexcept { idx = idx + 1; }
    // Reading goes to output place `place`: forward by passing items on,
    // back by taking written items back.
    [[nodiscard]] bool move_to(usize place) noexcept;

    // The bits of `mask` in every item (of clusters from cluster_start to
    // before cluster_end) are set to `value`'s.
    void set_masks(u32 value, u32 mask, u32 cluster_start, u32 cluster_end) noexcept;

    // The items from start to before end become one cluster (the least of
    // theirs), with whatever shares a cluster with them.
    void merge_clusters(usize start, usize end) noexcept;
    void merge_grapheme_clusters(usize start, usize end) noexcept;
    void merge_out_clusters(usize start, usize end) noexcept;
    void merge_out_grapheme_clusters(usize start, usize end) noexcept;
    // The item being read is dropped, its cluster given to a neighbour if
    // it would otherwise be lost.
    void delete_glyph() noexcept;
    // Every unwanted item is taken out, positions with them.
    void delete_glyphs_in_place(Unwanted unwanted) noexcept;

    // The run cannot be broken (or two shaped runs joined) inside these
    // items and come out the same. `end` may be to_the_end.
    void unsafe_to_break(usize start, usize end) noexcept;
    void unsafe_to_concat(usize start, usize end) noexcept;
    void unsafe_to_break_from_output(usize start, usize end) noexcept;
    void unsafe_to_concat_from_output(usize start, usize end) noexcept;

    // The items from start to before end in order (an insertion sort that
    // keeps equal items as they were), clusters joined as items pass each
    // other.
    void sort(usize start, usize end, SortsAfter after) noexcept;

    // Room for `size` items, within the most length.
    [[nodiscard]] bool ensure(const usize size) noexcept {
        // The most length holds whatever room the arrays have.
        if (size > max_len) {
            successful = false;
            return false;
        }
        const bool result = size <= info.count() || grow(size);
        return result;
    }
    // The arrays made larger, for ensure.
    [[nodiscard]] bool grow(usize size) noexcept;
    // `count` operations are spent; false when no more are left.
    [[nodiscard]] bool charge(const usize count) noexcept {
        const i64 left = static_cast<i64>(max_ops) - static_cast<i64>(count < 0x7FFFFFFF ? count : 0x7FFFFFFF);
        max_ops = left < -0x7FFFFFFF ? -0x7FFFFFFF : static_cast<i32>(left);
        if (max_ops < 0) successful = false;
        const bool result = max_ops >= 0;
        return result;
    }
    // Around a shaping: the limits set from the run's length, and put back.
    void enter() noexcept;
    void leave() noexcept;
    // A number from 1 to 7 for a ligature made now.
    [[nodiscard]] u32 allocate_ligature_id() noexcept;

    // Whether the buffer agrees with itself: the three arrays one length,
    // what is read and written inside them, the contexts within their five.
    [[nodiscard]] bool whole() const noexcept;

    // Frees the arrays.
    void release() noexcept;
};

// An item's cluster becomes `cluster`; if that is a change, its flags
// become `mask`'s.
void set_cluster(ShapeGlyph& item, u32 cluster, u32 mask) noexcept;

} // namespace stxlib::glyphs::detail

#endif
