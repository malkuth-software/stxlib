// stxlib::glyphs: the plan of a shaping: for a font, a script, a direction
// and the features asked for, which of the font's lookups are applied, in
// what order, and to which glyphs. After harfrust 0.16.0, src/ot/map.rs,
// src/planner.rs and src/tag.rs (HarfBuzz's hb-ot-map and hb-ot-shape's
// planner).
//
// A plan is made once and used for every run of that font, script and
// direction: making it reads the font's lists of scripts, features and
// lookups; using it reads none of them again.
//
// What makes a run fast. Each lookup of the plan carries a digest of the
// glyphs its subtables begin at (shape_digest.hpp), and each subtable its
// own: a lookup none of whose glyphs is in the run is passed over with one
// test, and inside a lookup a glyph is tried only against the subtables
// that may begin at it.
//
// Bounds. A plan has no more features than were asked for and no more
// lookups than the font lists for them; reading a coverage for a digest is
// charged against a budget (plan_digest_budget entries for the whole
// plan), and a lookup read after the budget is spent has a digest of
// everything, which is always right and only slower.
//
// Not converted: feature variations (variable fonts), Apple's layout tables
// (morx, kerx, trak), and the table of language names: a caller names a
// language system by its OpenType tag or not at all.

#ifndef STXLIB_ALGORITHMS_GLYPHS_SHAPE_PLAN_HPP
#define STXLIB_ALGORITHMS_GLYPHS_SHAPE_PLAN_HPP

#include "algorithms/glyphs/layout_tables.hpp"
#include "algorithms/glyphs/shape_buffer.hpp"
#include "algorithms/glyphs/shape_digest.hpp"
#include "stxlib/font_file.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::glyphs::detail {

[[nodiscard]] constexpr u32 layout_tag(const char a, const char b, const char c, const char d) noexcept {
    const u32 result = (static_cast<u32>(static_cast<u8>(a)) << 24) | (static_cast<u32>(static_cast<u8>(b)) << 16) |
                       (static_cast<u32>(static_cast<u8>(c)) << 8) | static_cast<u32>(static_cast<u8>(d));
    return result;
}

// A feature a caller asks for: on (value 1), off (0) or the number of an
// alternate, for the characters whose clusters are from `start` to before
// `end`; from 0 to feature_everywhere is the whole run.
inline constexpr u32 feature_everywhere = 0xFFFFFFFFU;
struct PlanRequest final {
    u32 tag{0};
    u32 value{0};
    u32 start{0};
    u32 end{feature_everywhere};
};

// Which script's own rules are used besides the font's: none, Hebrew's
// (presentation forms when the font cannot place marks) or Hangul's
// (syllables kept whole or put together from their letters, as the font
// has them).
enum class ShaperKind : u8 {
    plain = 0,
    hebrew = 1,
    hangul = 2,
};

// A feature of the plan: its bits in a glyph's mask.
struct PlanFeature final {
    u32 tag{0};
    u32 index[2]{no_feature, no_feature};
    u32 stage[2]{0, 0};
    u32 shift{0};
    u32 mask{0};
    u32 one_mask{0};
    bool needs_fallback{false};
    bool auto_zwnj{true};
    bool auto_zwj{true};
    bool random{false};
};

// A subtable of a lookup of the plan: where it is, its format, where its
// coverage is (0: none), and the digest of that coverage.
struct PlanSubtable final {
    usize place{0};
    u32 format{0};
    usize coverage{0};
    GlyphDigest digest{};
};

// A lookup of the plan.
struct PlanLookup final {
    u32 index{0};
    u32 mask{0};
    bool auto_zwnj{true};
    bool auto_zwj{true};
    bool random{false};
    // The kind of its subtables, its flags with the mark set above them
    // (the "props" a glyph is matched against), and its subtables.
    u32 kind{0};
    u32 props{0};
    u32 first_subtable{0};
    u32 subtable_count{0};
    GlyphDigest digest{};
};

inline constexpr usize plan_digest_budget = static_cast<usize>(1) << 20;
// A feature's value takes no more than this many bits of a mask.
inline constexpr u32 plan_value_bits = 8;
inline constexpr u32 plan_most_value = (1U << plan_value_bits) - 1;

struct ShapePlan final {
    ShapeDirection direction{ShapeDirection::unset};
    u32 script{0};
    u32 language{0};
    ShaperKind shaper{ShaperKind::plain};
    // The font's tables, as the plan read them: [0] substitutions, [1]
    // positions.
    LayoutTable tables[2]{};
    GlyphDefinitions definitions{};
    bool found_script[2]{false, false};
    u32 chosen_script[2]{0, 0};
    u32 global_mask{0};
    Vector<PlanFeature> features{};
    Vector<PlanLookup> lookups[2]{};
    Vector<PlanSubtable> subtables[2]{};
    // For each stage of a table, one past its last lookup.
    Vector<u32> stages[2]{};
    // The caller's features that are not for the whole run.
    Vector<PlanRequest> ranged{};
    u32 frac_mask{0};
    u32 numr_mask{0};
    u32 dnom_mask{0};
    u32 rtlm_mask{0};
    u32 kern_mask{0};
    bool has_frac{false};
    bool has_gpos_mark{false};
    bool requested_kerning{false};
    bool zero_marks{false};
    bool fallback_glyph_classes{false};
    bool fallback_mark_positioning{false};
    bool adjust_mark_positioning_when_zeroing{false};
    bool apply_gpos{false};
    bool apply_kern{false};
};

// The plan for a font. `script` is an ISO 15924 tag or 0, `language` an
// OpenType language system tag or 0. Fails only for want of memory.
[[nodiscard]] Status compile_plan(const Font& font, ShapeDirection direction, u32 script, u32 language, View<PlanRequest> requests,
                                  ShapePlan& plan) noexcept;

// A feature's mask and where its value goes in it (0 and 0 when the plan
// has not the feature), and the mask of its value 1.
[[nodiscard]] u32 plan_mask(const ShapePlan& plan, u32 tag, u32& shift) noexcept;
[[nodiscard]] u32 plan_one_mask(const ShapePlan& plan, u32 tag) noexcept;

// What the subtables of a lookup have in common, for one of them: its
// format, and where the coverage of the glyph it begins at is (0: none or
// not readable). `kind` is the lookup's.
void describe_subtable(const LayoutTable& layout, u32 kind, usize place, PlanSubtable& subtable) noexcept;

// A lookup's props: its flags, with its mark set above them.
[[nodiscard]] constexpr u32 props_of(const LayoutLookup& lookup) noexcept {
    const u32 result = (lookup.flags & lookup_use_mark_set) != 0 ? (lookup.flags | (lookup.mark_set << 16)) : lookup.flags;
    return result;
}

// The bytes a plan holds.
[[nodiscard]] usize bytes_of(const ShapePlan& plan) noexcept;

} // namespace stxlib::glyphs::detail

#endif
