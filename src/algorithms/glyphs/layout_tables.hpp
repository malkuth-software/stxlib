// stxlib::glyphs: reading a font's layout tables: the glyph definitions
// (GDEF), and what the tables of substitutions (GSUB) and positions (GPOS)
// have in common: scripts and their language systems, features, lookups,
// coverages and class definitions. After read-fonts 0.47.0, tables/
// layout.rs, gdef.rs, gsub.rs and gpos.rs (the tables harfrust reads
// through), in the house's bounded reads: every function here answers
// "nothing" for a table that is cut short or says something impossible,
// and none reads outside its table.
//
// A place in a table is a byte offset from the table's start; 0 is "no
// such thing", as the format has it. Searches are by halving over sorted
// records, as the format requires them sorted; a table that is not sorted
// is searched the same way and may miss an entry, and nothing else.

#ifndef STXLIB_ALGORITHMS_GLYPHS_LAYOUT_TABLES_HPP
#define STXLIB_ALGORITHMS_GLYPHS_LAYOUT_TABLES_HPP

#include "algorithms/glyphs/font_reader.hpp"

namespace stxlib::glyphs::detail {

inline constexpr u32 not_covered = 0xFFFFFFFFU;
inline constexpr u32 no_feature = 0xFFFFU;

// The glyph's index in the coverage at `offset`, or not_covered.
[[nodiscard]] u32 coverage_index(const Span& table, usize offset, u32 glyph) noexcept;

// The glyph's class by the class definition at `offset`: 0 for a glyph it
// does not name, or for no definition.
[[nodiscard]] u32 class_of(const Span& table, usize offset, u32 glyph) noexcept;

// ---- glyph definitions (GDEF) -------------------------------------------------------------------

inline constexpr u32 glyph_class_base = 1;
inline constexpr u32 glyph_class_ligature = 2;
inline constexpr u32 glyph_class_mark = 3;
inline constexpr u32 glyph_class_component = 4;

struct GlyphDefinitions final {
    Span table{};
    // The class definition of glyph classes, that of mark attachment
    // classes, and the list of mark glyph sets (version 1.2); 0 for none.
    usize classes{0};
    usize mark_classes{0};
    usize mark_sets{0};
};

// The font's glyph definitions: empty (every glyph of class 0) when it has
// none or they cannot be read.
[[nodiscard]] GlyphDefinitions open_definitions(const Font& font) noexcept;
[[nodiscard]] u32 glyph_class(const GlyphDefinitions& definitions, u32 glyph) noexcept;
[[nodiscard]] u32 mark_attachment_class(const GlyphDefinitions& definitions, u32 glyph) noexcept;
[[nodiscard]] bool in_mark_set(const GlyphDefinitions& definitions, u32 set, u32 glyph) noexcept;

// ---- substitutions and positions: what they have in common -------------------------------------

// What a lookup passes over, as bits (the format's lookup flags).
inline constexpr u32 lookup_right_to_left = 0x0001;
inline constexpr u32 lookup_ignore_bases = 0x0002;
inline constexpr u32 lookup_ignore_ligatures = 0x0004;
inline constexpr u32 lookup_ignore_marks = 0x0008;
inline constexpr u32 lookup_use_mark_set = 0x0010;
inline constexpr u32 lookup_mark_class_mask = 0xFF00;

// The kind of lookup that only holds another (an "extension").
inline constexpr u32 substitution_extension = 7;
inline constexpr u32 position_extension = 9;

struct LayoutTable final {
    Span table{};
    bool substitution{true};
    // The lists of scripts, features and lookups; 0 for none.
    usize scripts{0};
    usize features{0};
    usize lookups{0};
};

// The font's substitutions (`substitution`) or positions: empty when it has
// none or the header cannot be read.
[[nodiscard]] LayoutTable open_layout(const Font& font, bool substitution) noexcept;

[[nodiscard]] u32 script_count(const LayoutTable& layout) noexcept;
// The script with this tag: its place, or false.
[[nodiscard]] bool find_script(const LayoutTable& layout, u32 tag, usize& script) noexcept;

// A script's language system: the features it uses, by their numbers.
struct LanguageSystem final {
    bool found{false};
    // The feature every use of the system has, or no_feature.
    u32 required_feature{no_feature};
    // Where the numbers of its other features are, and how many.
    usize features{0};
    u32 feature_count{0};
};

// The language system of `language` in a script, or the script's default
// one when `language` is 0 or the script has none for it. found is false
// when there is neither.
[[nodiscard]] LanguageSystem language_system(const LayoutTable& layout, usize script, u32 language) noexcept;
// Whether the script has a language system of its own for `language`.
[[nodiscard]] bool has_language(const LayoutTable& layout, usize script, u32 language) noexcept;
// The number of the system's `index`th feature (index < feature_count).
[[nodiscard]] u32 language_feature(const LayoutTable& layout, const LanguageSystem& system, u32 index) noexcept;

struct LayoutFeature final {
    u32 tag{0};
    // Where its lookups' numbers are, and how many.
    usize lookups{0};
    u32 lookup_count{0};
};

[[nodiscard]] u32 feature_count(const LayoutTable& layout) noexcept;
// Feature number `index`: false when the table has no such feature.
[[nodiscard]] bool feature_at(const LayoutTable& layout, u32 index, LayoutFeature& feature) noexcept;
// The number of the feature's `index`th lookup (index < lookup_count).
[[nodiscard]] u32 feature_lookup(const LayoutTable& layout, const LayoutFeature& feature, u32 index) noexcept;

struct LayoutLookup final {
    // The kind of its subtables; never an extension (subtable_at looks
    // through one), 0 when it could not be told.
    u32 kind{0};
    u32 flags{0};
    // The mark glyph set it keeps to, when flags say it uses one.
    u32 mark_set{0};
    u32 subtable_count{0};
    usize place{0};
};

[[nodiscard]] u32 lookup_count(const LayoutTable& layout) noexcept;
// Lookup number `index`: false when the table has no such lookup.
[[nodiscard]] bool lookup_at(const LayoutTable& layout, u32 index, LayoutLookup& lookup) noexcept;
// The lookup's `index`th subtable (index < subtable_count): its place in
// the table, an extension looked through. False when it cannot be read or
// an extension holds another kind than the lookup's first did.
[[nodiscard]] bool subtable_at(const LayoutTable& layout, const LayoutLookup& lookup, u32 index, usize& subtable) noexcept;

} // namespace stxlib::glyphs::detail

#endif
