// stxlib::glyphs, reading a font's layout tables
// (src/algorithms/glyphs/layout_tables.hpp), against tables this test
// writes byte by byte, so that every expected number is the test's own.
//
//   L1  coverages of both formats: a glyph's index, glyphs before, between
//       and after what is covered, and a coverage cut short;
//   L2  class definitions of both formats;
//   L3  glyph definitions: classes, mark attachment classes, mark glyph
//       sets, and a table of the version without sets;
//   L4  a table of substitutions: scripts by tag, a script's default
//       language system and one by tag, the required feature, features and
//       their lookups, lookups with their flags and mark set, and a lookup
//       of extensions looked through;
//   L5  robustness: every prefix of the tables and many copies with bytes
//       changed are read through every function; each answer is an answer
//       (the sanitizer builds are what make this a memory check).

#include "algorithms/glyphs/layout_tables.hpp"

#include "font_builder.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace glyphs = stxlib::glyphs;
namespace detail = stxlib::glyphs::detail;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

using font_builder::append;
using font_builder::Bytes;
using font_builder::put16;
using font_builder::put32;

[[nodiscard]] constexpr u32 tag(const char a, const char b, const char c, const char d) {
    const u32 result = (static_cast<u32>(static_cast<unsigned char>(a)) << 24) | (static_cast<u32>(static_cast<unsigned char>(b)) << 16) |
                       (static_cast<u32>(static_cast<unsigned char>(c)) << 8) | static_cast<u32>(static_cast<unsigned char>(d));
    return result;
}

[[nodiscard]] detail::Span span_of(const Bytes& bytes) {
    const detail::Span result{.data = bytes.data(), .size = bytes.size()};
    return result;
}

// A coverage that lists glyphs, and one of ranges.
[[nodiscard]] Bytes coverage_list(const std::vector<u32>& glyphs_listed) {
    Bytes bytes{};
    put16(bytes, 1);
    put16(bytes, static_cast<int>(glyphs_listed.size()));
    for (const u32 glyph : glyphs_listed) put16(bytes, static_cast<int>(glyph));
    return bytes;
}

struct Range final {
    u32 first{0};
    u32 last{0};
    u32 value{0};
};

[[nodiscard]] Bytes ranges_of(const std::vector<Range>& ranges) {
    Bytes bytes{};
    put16(bytes, 2);
    put16(bytes, static_cast<int>(ranges.size()));
    for (const Range& range : ranges) {
        put16(bytes, static_cast<int>(range.first));
        put16(bytes, static_cast<int>(range.last));
        put16(bytes, static_cast<int>(range.value));
    }
    return bytes;
}

void coverages() {
    // Eight bytes of something else before each, so that an offset of 0
    // (none) is not the coverage.
    Bytes table(8, 0xEE);
    const usize listed = table.size();
    append(table, coverage_list({3, 7, 8, 200, 65535}));
    const usize ranged = table.size();
    append(table, ranges_of({{10, 12, 0}, {20, 20, 3}, {300, 400, 4}}));
    const detail::Span span = span_of(table);
    CHECK(detail::coverage_index(span, listed, 3) == 0 && detail::coverage_index(span, listed, 7) == 1 && detail::coverage_index(span, listed, 8) == 2);
    CHECK(detail::coverage_index(span, listed, 200) == 3 && detail::coverage_index(span, listed, 65535) == 4);
    CHECK(detail::coverage_index(span, listed, 0) == detail::not_covered && detail::coverage_index(span, listed, 9) == detail::not_covered &&
          detail::coverage_index(span, listed, 70000) == detail::not_covered);
    CHECK(detail::coverage_index(span, ranged, 10) == 0 && detail::coverage_index(span, ranged, 12) == 2 && detail::coverage_index(span, ranged, 20) == 3);
    CHECK(detail::coverage_index(span, ranged, 300) == 4 && detail::coverage_index(span, ranged, 400) == 104);
    CHECK(detail::coverage_index(span, ranged, 9) == detail::not_covered && detail::coverage_index(span, ranged, 13) == detail::not_covered &&
          detail::coverage_index(span, ranged, 401) == detail::not_covered);
    // No coverage, one of a format there is not, and one cut short.
    CHECK(detail::coverage_index(span, 0, 3) == detail::not_covered);
    Bytes other = coverage_list({3});
    other[1] = 9;
    CHECK(detail::coverage_index(span_of(other), 0, 3) == detail::not_covered);
    const detail::Span cut{.data = table.data(), .size = listed + 9};
    CHECK(detail::coverage_index(cut, listed, 3) == detail::not_covered);
}

void classes() {
    Bytes table(6, 0xEE);
    const usize by_glyph = table.size();
    // Format 1: from glyph 50, the classes 2, 0, 5.
    put16(table, 1);
    put16(table, 50);
    put16(table, 3);
    put16(table, 2);
    put16(table, 0);
    put16(table, 5);
    const usize by_range = table.size();
    append(table, ranges_of({{4, 9, 1}, {30, 30, 3}, {1000, 2000, 7}}));
    const detail::Span span = span_of(table);
    CHECK(detail::class_of(span, by_glyph, 50) == 2 && detail::class_of(span, by_glyph, 51) == 0 && detail::class_of(span, by_glyph, 52) == 5);
    CHECK(detail::class_of(span, by_glyph, 49) == 0 && detail::class_of(span, by_glyph, 53) == 0);
    CHECK(detail::class_of(span, by_range, 4) == 1 && detail::class_of(span, by_range, 9) == 1 && detail::class_of(span, by_range, 30) == 3 &&
          detail::class_of(span, by_range, 1500) == 7);
    CHECK(detail::class_of(span, by_range, 3) == 0 && detail::class_of(span, by_range, 10) == 0 && detail::class_of(span, by_range, 2001) == 0);
    CHECK(detail::class_of(span, 0, 4) == 0);
}

// A font of nothing but the tables given.
[[nodiscard]] glyphs::Font font_of_tables(const Bytes& bytes, const usize gdef_at, const usize gdef_size, const usize gsub_at, const usize gsub_size) {
    glyphs::Font font{};
    font.bytes = stxlib::View<u8>(bytes.data(), bytes.size());
    font.glyph_count = 65535;
    font.definitions = glyphs::FontTable{.offset = static_cast<u32>(gdef_at), .length = static_cast<u32>(gdef_size)};
    font.substitutions = glyphs::FontTable{.offset = static_cast<u32>(gsub_at), .length = static_cast<u32>(gsub_size)};
    font.positions = font.substitutions;
    return font;
}

[[nodiscard]] Bytes definitions_table(const bool with_sets) {
    // Header, then: glyph classes (5 to 6 bases, 7 a ligature, 8 to 9
    // marks), mark attachment classes (8 is 1, 9 is 2), and two mark sets
    // (8 alone; 8 and 9).
    const Bytes glyph_classes = ranges_of({{5, 6, 1}, {7, 7, 2}, {8, 9, 3}});
    const Bytes mark_classes = ranges_of({{8, 8, 1}, {9, 9, 2}});
    const Bytes first_set = coverage_list({8});
    const Bytes second_set = coverage_list({8, 9});
    const usize header = with_sets ? 14 : 12;
    Bytes table{};
    put16(table, 1);
    put16(table, with_sets ? 2 : 0);
    put16(table, static_cast<int>(header));
    put16(table, 0);
    put16(table, 0);
    put16(table, static_cast<int>(header + glyph_classes.size()));
    const usize sets_at = header + glyph_classes.size() + mark_classes.size();
    if (with_sets) put16(table, static_cast<int>(sets_at));
    append(table, glyph_classes);
    append(table, mark_classes);
    if (with_sets) {
        put16(table, 1);
        put16(table, 2);
        put32(table, 12);
        put32(table, static_cast<u32>(12 + first_set.size()));
        append(table, first_set);
        append(table, second_set);
    }
    return table;
}

void definitions() {
    const Bytes table = definitions_table(true);
    const glyphs::Font font = font_of_tables(table, 0, table.size(), 0, 0);
    const detail::GlyphDefinitions gdef = detail::open_definitions(font);
    CHECK(gdef.classes != 0 && gdef.mark_classes != 0 && gdef.mark_sets != 0);
    CHECK(detail::glyph_class(gdef, 5) == detail::glyph_class_base && detail::glyph_class(gdef, 7) == detail::glyph_class_ligature &&
          detail::glyph_class(gdef, 9) == detail::glyph_class_mark && detail::glyph_class(gdef, 4) == 0 && detail::glyph_class(gdef, 10) == 0);
    CHECK(detail::mark_attachment_class(gdef, 8) == 1 && detail::mark_attachment_class(gdef, 9) == 2 && detail::mark_attachment_class(gdef, 5) == 0);
    CHECK(detail::in_mark_set(gdef, 0, 8) && !detail::in_mark_set(gdef, 0, 9) && detail::in_mark_set(gdef, 1, 9) && detail::in_mark_set(gdef, 1, 8));
    CHECK(!detail::in_mark_set(gdef, 2, 8) && !detail::in_mark_set(gdef, 0, 5));
    // The version without sets: no glyph is in any.
    const Bytes older = definitions_table(false);
    const glyphs::Font older_font = font_of_tables(older, 0, older.size(), 0, 0);
    const detail::GlyphDefinitions plain = detail::open_definitions(older_font);
    CHECK(plain.mark_sets == 0 && detail::glyph_class(plain, 8) == detail::glyph_class_mark && !detail::in_mark_set(plain, 0, 8));
    // No table: every glyph is of no class.
    const glyphs::Font none = font_of_tables(table, 0, 0, 0, 0);
    const detail::GlyphDefinitions empty = detail::open_definitions(none);
    CHECK(detail::glyph_class(empty, 8) == 0 && !detail::in_mark_set(empty, 0, 8));
}

// A table of substitutions with two scripts, three features and three
// lookups, the third a lookup of extensions.
[[nodiscard]] Bytes substitutions_table() {
    // Scripts. DFLT: a default system with features 0 and 1. latn: a
    // default system with feature 1 and the required feature 2, and a
    // system for 'TRK ' with features 0, 1 and 2.
    Bytes dflt{};
    put16(dflt, 4);
    put16(dflt, 0);
    put16(dflt, 0);
    put16(dflt, 0xFFFF);
    put16(dflt, 2);
    put16(dflt, 0);
    put16(dflt, 1);
    Bytes latn{};
    put16(latn, 10);
    put16(latn, 1);
    put32(latn, tag('T', 'R', 'K', ' '));
    put16(latn, 18);
    put16(latn, 0);
    put16(latn, 2);
    put16(latn, 1);
    put16(latn, 1);
    put16(latn, 0);
    put16(latn, 0xFFFF);
    put16(latn, 3);
    put16(latn, 0);
    put16(latn, 1);
    put16(latn, 2);
    Bytes scripts{};
    put16(scripts, 2);
    put32(scripts, tag('D', 'F', 'L', 'T'));
    put16(scripts, 14);
    put32(scripts, tag('l', 'a', 't', 'n'));
    put16(scripts, static_cast<int>(14 + dflt.size()));
    append(scripts, dflt);
    append(scripts, latn);
    // Features: kern with lookup 1; liga with lookups 0 and 2; locl with
    // none.
    Bytes features{};
    put16(features, 3);
    put32(features, tag('k', 'e', 'r', 'n'));
    put16(features, 20);
    put32(features, tag('l', 'i', 'g', 'a'));
    put16(features, 26);
    put32(features, tag('l', 'o', 'c', 'l'));
    put16(features, 34);
    put16(features, 0);
    put16(features, 1);
    put16(features, 1);
    put16(features, 0);
    put16(features, 2);
    put16(features, 0);
    put16(features, 2);
    put16(features, 0);
    put16(features, 0);
    // Lookups. 0: ligatures, marks passed over, one subtable. 1: single
    // substitutions keeping to mark set 5 and attachment class 2, two
    // subtables. 2: extensions of chained contexts, two subtables.
    Bytes first{};
    put16(first, 4);
    put16(first, 0x0008);
    put16(first, 1);
    put16(first, 8);
    put16(first, 0xAAAA);
    Bytes second{};
    put16(second, 1);
    put16(second, 0x0210);
    put16(second, 2);
    put16(second, 12);
    put16(second, 14);
    put16(second, 5);
    put16(second, 0xBBBB);
    put16(second, 0xCCCC);
    Bytes third{};
    put16(third, 7);
    put16(third, 0);
    put16(third, 2);
    put16(third, 10);
    put16(third, 18);
    put16(third, 1);
    put16(third, 6);
    put32(third, 16);
    put16(third, 1);
    put16(third, 6);
    put32(third, 10);
    put16(third, 0xDDDD);
    put16(third, 0xEEEE);
    Bytes lookups{};
    put16(lookups, 3);
    put16(lookups, 8);
    put16(lookups, static_cast<int>(8 + first.size()));
    put16(lookups, static_cast<int>(8 + first.size() + second.size()));
    append(lookups, first);
    append(lookups, second);
    append(lookups, third);
    Bytes table{};
    put16(table, 1);
    put16(table, 0);
    put16(table, 10);
    put16(table, static_cast<int>(10 + scripts.size()));
    put16(table, static_cast<int>(10 + scripts.size() + features.size()));
    append(table, scripts);
    append(table, features);
    append(table, lookups);
    return table;
}

void substitutions() {
    const Bytes table = substitutions_table();
    const glyphs::Font font = font_of_tables(table, 0, 0, 0, table.size());
    const detail::LayoutTable layout = detail::open_layout(font, true);
    CHECK(layout.scripts == 10 && detail::script_count(layout) == 2 && detail::feature_count(layout) == 3 && detail::lookup_count(layout) == 3);
    // Scripts.
    usize dflt = 0;
    usize latn = 0;
    usize none = 0;
    CHECK(detail::find_script(layout, tag('D', 'F', 'L', 'T'), dflt) && detail::find_script(layout, tag('l', 'a', 't', 'n'), latn));
    CHECK(!detail::find_script(layout, tag('g', 'r', 'e', 'k'), none) && !detail::find_script(layout, tag('A', 'A', 'A', 'A'), none) &&
          !detail::find_script(layout, tag('z', 'z', 'z', 'z'), none));
    // Language systems.
    const detail::LanguageSystem dflt_system = detail::language_system(layout, dflt, 0);
    CHECK(dflt_system.found && dflt_system.required_feature == detail::no_feature && dflt_system.feature_count == 2);
    CHECK(detail::language_feature(layout, dflt_system, 0) == 0 && detail::language_feature(layout, dflt_system, 1) == 1 &&
          detail::language_feature(layout, dflt_system, 2) == detail::no_feature);
    const detail::LanguageSystem latn_default = detail::language_system(layout, latn, 0);
    CHECK(latn_default.found && latn_default.required_feature == 2 && latn_default.feature_count == 1 && detail::language_feature(layout, latn_default, 0) == 1);
    const detail::LanguageSystem turkish = detail::language_system(layout, latn, tag('T', 'R', 'K', ' '));
    CHECK(turkish.found && turkish.required_feature == detail::no_feature && turkish.feature_count == 3 && detail::language_feature(layout, turkish, 2) == 2);
    // A language the script has no system for gets the default one.
    const detail::LanguageSystem other = detail::language_system(layout, latn, tag('D', 'E', 'U', ' '));
    CHECK(other.found && other.required_feature == 2 && other.feature_count == 1);
    CHECK(!detail::language_system(layout, 0, 0).found);
    // Features.
    detail::LayoutFeature feature{};
    CHECK(detail::feature_at(layout, 0, feature) && feature.tag == tag('k', 'e', 'r', 'n') && feature.lookup_count == 1 &&
          detail::feature_lookup(layout, feature, 0) == 1);
    CHECK(detail::feature_at(layout, 1, feature) && feature.tag == tag('l', 'i', 'g', 'a') && feature.lookup_count == 2 &&
          detail::feature_lookup(layout, feature, 0) == 0 && detail::feature_lookup(layout, feature, 1) == 2);
    CHECK(detail::feature_at(layout, 2, feature) && feature.tag == tag('l', 'o', 'c', 'l') && feature.lookup_count == 0);
    CHECK(!detail::feature_at(layout, 3, feature));
    // Lookups.
    detail::LayoutLookup lookup{};
    usize subtable = 0;
    const detail::Span span = span_of(table);
    CHECK(detail::lookup_at(layout, 0, lookup) && lookup.kind == 4 && lookup.flags == detail::lookup_ignore_marks && lookup.subtable_count == 1);
    CHECK(detail::subtable_at(layout, lookup, 0, subtable) && detail::read_u16(span, subtable) == 0xAAAA && !detail::subtable_at(layout, lookup, 1, subtable));
    CHECK(detail::lookup_at(layout, 1, lookup) && lookup.kind == 1 && (lookup.flags & detail::lookup_use_mark_set) != 0 &&
          (lookup.flags & detail::lookup_mark_class_mask) == 0x0200 && lookup.mark_set == 5 && lookup.subtable_count == 2);
    CHECK(detail::subtable_at(layout, lookup, 0, subtable) && detail::read_u16(span, subtable) == 0xBBBB);
    CHECK(detail::subtable_at(layout, lookup, 1, subtable) && detail::read_u16(span, subtable) == 0xCCCC);
    // Extensions looked through: the lookup is of the kind they hold.
    CHECK(detail::lookup_at(layout, 2, lookup) && lookup.kind == 6 && lookup.subtable_count == 2);
    CHECK(detail::subtable_at(layout, lookup, 0, subtable) && detail::read_u16(span, subtable) == 0xDDDD);
    CHECK(detail::subtable_at(layout, lookup, 1, subtable) && detail::read_u16(span, subtable) == 0xEEEE);
    CHECK(!detail::lookup_at(layout, 3, lookup));
    // An extension that holds another kind than the first is not read.
    Bytes mixed = table;
    CHECK(detail::lookup_at(layout, 2, lookup));
    const usize second_extension = lookup.place + 18;
    mixed[second_extension + 3] = 5;
    const glyphs::Font mixed_font = font_of_tables(mixed, 0, 0, 0, mixed.size());
    const detail::LayoutTable mixed_layout = detail::open_layout(mixed_font, true);
    CHECK(detail::lookup_at(mixed_layout, 2, lookup) && lookup.kind == 6 && detail::subtable_at(mixed_layout, lookup, 0, subtable) &&
          !detail::subtable_at(mixed_layout, lookup, 1, subtable));
    // The same bytes as positions: an extension there is kind 9, so kind 7
    // is a kind of its own.
    const detail::LayoutTable positions = detail::open_layout(font, false);
    CHECK(detail::lookup_at(positions, 2, lookup) && lookup.kind == 7);
    // No table.
    const glyphs::Font bare = font_of_tables(table, 0, 0, 0, 0);
    const detail::LayoutTable empty = detail::open_layout(bare, true);
    CHECK(detail::script_count(empty) == 0 && detail::feature_count(empty) == 0 && detail::lookup_count(empty) == 0 && !detail::find_script(empty, 1, none));
}

std::uint64_t random_state = 0x853c49e6748fea9bULL;
[[nodiscard]] std::uint64_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

// Every function on a table, whatever it is; the sum is so that nothing
// is left out by the compiler.
[[nodiscard]] std::uint64_t read_everything(const Bytes& definitions_bytes, const Bytes& layout_bytes) {
    Bytes both = definitions_bytes;
    append(both, layout_bytes);
    const glyphs::Font font = font_of_tables(both, 0, definitions_bytes.size(), definitions_bytes.size(), layout_bytes.size());
    std::uint64_t sum = 0;
    const detail::GlyphDefinitions gdef = detail::open_definitions(font);
    for (u32 glyph = 0; glyph < 12; ++glyph) {
        sum = sum + detail::glyph_class(gdef, glyph) + detail::mark_attachment_class(gdef, glyph);
        for (u32 set = 0; set < 3; ++set) sum = sum + (detail::in_mark_set(gdef, set, glyph) ? 1U : 0U);
    }
    for (int pass = 0; pass < 2; ++pass) {
        const detail::LayoutTable layout = detail::open_layout(font, pass == 0);
        const u32 tags[] = {tag('D', 'F', 'L', 'T'), tag('l', 'a', 't', 'n'), 0, 0xFFFFFFFFU};
        for (const u32 script_tag : tags) {
            usize script = 0;
            if (!detail::find_script(layout, script_tag, script)) continue;
            const u32 languages[] = {0, tag('T', 'R', 'K', ' '), 0xFFFFFFFFU};
            for (const u32 language : languages) {
                const detail::LanguageSystem system = detail::language_system(layout, script, language);
                const u32 features = system.feature_count < 64 ? system.feature_count : 64;
                for (u32 index = 0; index < features; ++index) sum = sum + detail::language_feature(layout, system, index);
                sum = sum + system.required_feature;
            }
        }
        const u32 features = detail::feature_count(layout) < 64 ? detail::feature_count(layout) : 64;
        for (u32 index = 0; index < features; ++index) {
            detail::LayoutFeature feature{};
            if (!detail::feature_at(layout, index, feature)) continue;
            const u32 lookups = feature.lookup_count < 64 ? feature.lookup_count : 64;
            for (u32 lookup = 0; lookup < lookups; ++lookup) sum = sum + detail::feature_lookup(layout, feature, lookup);
        }
        const u32 lookups = detail::lookup_count(layout) < 64 ? detail::lookup_count(layout) : 64;
        for (u32 index = 0; index < lookups; ++index) {
            detail::LayoutLookup lookup{};
            if (!detail::lookup_at(layout, index, lookup)) continue;
            const u32 subtables = lookup.subtable_count < 64 ? lookup.subtable_count : 64;
            for (u32 number = 0; number < subtables; ++number) {
                usize subtable = 0;
                if (detail::subtable_at(layout, lookup, number, subtable)) {
                    sum = sum + detail::read_u16(layout.table, subtable);
                    for (u32 glyph = 0; glyph < 6; ++glyph) sum = sum + detail::coverage_index(layout.table, subtable, glyph) + detail::class_of(layout.table, subtable, glyph);
                }
            }
        }
    }
    return sum;
}

void robustness() {
    const Bytes definitions_bytes = definitions_table(true);
    const Bytes layout_bytes = substitutions_table();
    std::uint64_t tried = 0;
    std::uint64_t sum = 0;
    for (usize length = 0; length <= definitions_bytes.size(); ++length) {
        const Bytes prefix(definitions_bytes.begin(), definitions_bytes.begin() + static_cast<std::ptrdiff_t>(length));
        sum = sum + read_everything(prefix, layout_bytes);
        tried = tried + 1;
    }
    for (usize length = 0; length <= layout_bytes.size(); ++length) {
        const Bytes prefix(layout_bytes.begin(), layout_bytes.begin() + static_cast<std::ptrdiff_t>(length));
        sum = sum + read_everything(definitions_bytes, prefix);
        tried = tried + 1;
    }
    for (int round = 0; round < 200000; ++round) {
        Bytes changed_definitions = definitions_bytes;
        Bytes changed_layout = layout_bytes;
        const usize changes = 1 + next_random() % 4;
        for (usize change = 0; change < changes; ++change) {
            Bytes& target = next_random() % 3 == 0 ? changed_definitions : changed_layout;
            target[next_random() % target.size()] = static_cast<u8>(next_random() & 0xFFU);
        }
        sum = sum + read_everything(changed_definitions, changed_layout);
        tried = tried + 1;
    }
    const u8 values[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
    for (usize at = 0; at < layout_bytes.size(); ++at) {
        for (const u8 value : values) {
            Bytes changed = layout_bytes;
            changed[at] = value;
            sum = sum + read_everything(definitions_bytes, changed);
            tried = tried + 1;
        }
    }
    std::printf("robustness: %llu tables read, sum %llu\n", static_cast<unsigned long long>(tried), static_cast<unsigned long long>(sum));
    CHECK(tried > 200000);
}

} // namespace

int main() {
    coverages();
    classes();
    definitions();
    substitutions();
    robustness();
    const int status = stxlib_tests::finish("stxlib_glyph_layout_tests");
    return status;
}
