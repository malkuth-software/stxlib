// stxlib::glyphs, shaping (include/stxlib/glyph_shape.hpp): a font built
// here, with layout tables written byte by byte, shaped and compared with
// what the tables say, worked out by hand.
//
// The font. Units per em 1000. Glyphs (advance):
//    0 .notdef 500   1 a 600   2 b 600   3 f 300   4 i 250   5 fi 520
//    6 A 700   7 V 680   8 space 250   9 acute (U+0301) 200   10 e 550
//   11 e-acute (U+00E9) 550   12 x 500   13 x.alt 510   14 ( 300   15 ) 300
//   16 alef 600   17 bet 580   18 o 560   19 A.sc 500   20 dot below
//   (U+0323) 0   21 T 620   22 part 100   23 U+AC00 1000   24 U+1100 500
//   25 U+1161 500   26 c 540   27 d 560
// Glyph classes: 9 and 20 marks, 5 a ligature, the letters bases.
// Substitutions: liga f i -> fi; calt x -> x.alt between a and b (a
// chained context that names a single substitution); smcp A -> A.sc;
// ccmp c -> part part; rlig d: a context that names itself.
// Positions: kern A V -80 on A (pairs by glyph), T before o or a -50
// (pairs by class); mark: acute and dot below on a and o by anchors.
//
//   S1  plain text: glyphs, advances, clusters; nothing asked of the font;
//   S2  a ligature, and what keeps one from forming;
//   S3  kerning, by glyph and by class, in font units and at a size (the
//       scaling worked out by hand);
//   S4  marks: composed when the font has the composed character, placed
//       by anchors when not, put in the order of their classes;
//   S5  a chained context and the lookup it names;
//   S6  features asked for: on for the run, on for part of it;
//   S7  one glyph become several; a lookup that names itself is stopped
//       at the nesting allowed and the run refused;
//   S8  right to left: the order glyphs come out in, and mirrored brackets;
//   S9  Hangul letters become the syllable the font has;
//   S10 what is not to show, and spaces the font has not;
//   S11 what is refused, the plans kept, and the bytes held;
//   U1  the layout tables altered: every 16-bit field set to each of six
//       values and 20,000 copies with one to four bytes changed, from a
//       fixed seed; every run is refused or shaped, and what is shaped is
//       no longer than the bound says. The sanitizer builds make this a
//       memory check. A search of those cases and no proof of the rest.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_face.hpp"
#include "stxlib/glyph_shape.hpp"

#include "font_builder.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace glyphs = stxlib::glyphs;
using namespace font_builder;
using stxlib::i64;

// ---- layout tables, byte by byte -------------------------------------------------------------------

[[nodiscard]] Bytes coverage_of(const std::vector<u32>& listed) {
    Bytes table{};
    put16(table, 1);
    put16(table, static_cast<i32>(listed.size()));
    for (const u32 glyph : listed) put16(table, static_cast<i32>(glyph));
    return table;
}

struct ClassRange final {
    u32 first{0};
    u32 last{0};
    u32 glyph_class{0};
};

[[nodiscard]] Bytes classes_of(const std::vector<ClassRange>& ranges) {
    Bytes table{};
    put16(table, 2);
    put16(table, static_cast<i32>(ranges.size()));
    for (const ClassRange& range : ranges) {
        put16(table, static_cast<i32>(range.first));
        put16(table, static_cast<i32>(range.last));
        put16(table, static_cast<i32>(range.glyph_class));
    }
    return table;
}

// Parts laid one after another behind a header of `header_size` bytes that
// holds an offset to each: the offsets, from the header's start.
[[nodiscard]] std::vector<usize> offsets_after(const usize header_size, const std::vector<Bytes>& parts) {
    std::vector<usize> offsets{};
    usize at = header_size;
    for (const Bytes& part : parts) {
        offsets.push_back(at);
        at = at + part.size();
    }
    return offsets;
}

[[nodiscard]] Bytes single_substitution(const std::vector<std::pair<u32, u32>>& pairs) {
    std::vector<u32> from{};
    for (const std::pair<u32, u32>& pair : pairs) from.push_back(pair.first);
    Bytes table{};
    put16(table, 2);
    put16(table, static_cast<i32>(6 + pairs.size() * 2));
    put16(table, static_cast<i32>(pairs.size()));
    for (const std::pair<u32, u32>& pair : pairs) put16(table, static_cast<i32>(pair.second));
    append(table, coverage_of(from));
    return table;
}

// One first glyph, and the ligatures that begin with it: each the glyphs
// after the first and the glyph they make.
struct Ligature final {
    std::vector<u32> rest{};
    u32 glyph{0};
};

[[nodiscard]] Bytes ligature_substitution(const u32 first, const std::vector<Ligature>& ligatures) {
    std::vector<Bytes> bodies{};
    for (const Ligature& ligature : ligatures) {
        Bytes body{};
        put16(body, static_cast<i32>(ligature.glyph));
        put16(body, static_cast<i32>(ligature.rest.size() + 1));
        for (const u32 glyph : ligature.rest) put16(body, static_cast<i32>(glyph));
        bodies.push_back(body);
    }
    Bytes set{};
    put16(set, static_cast<i32>(bodies.size()));
    for (const usize offset : offsets_after(2 + bodies.size() * 2, bodies)) put16(set, static_cast<i32>(offset));
    for (const Bytes& body : bodies) append(set, body);
    Bytes table{};
    put16(table, 1);
    put16(table, static_cast<i32>(8 + set.size()));
    put16(table, 1);
    put16(table, 8);
    append(table, set);
    append(table, coverage_of({first}));
    return table;
}

[[nodiscard]] Bytes multiple_substitution(const u32 from, const std::vector<u32>& to) {
    Bytes table{};
    put16(table, 1);
    put16(table, static_cast<i32>(8 + 2 + to.size() * 2));
    put16(table, 1);
    put16(table, 8);
    put16(table, static_cast<i32>(to.size()));
    for (const u32 glyph : to) put16(table, static_cast<i32>(glyph));
    append(table, coverage_of({from}));
    return table;
}

struct NamedLookup final {
    u32 place{0};
    u32 lookup{0};
};

// A chained context of format 3: one glyph behind, the glyphs matched, one
// glyph ahead, each any of a coverage.
[[nodiscard]] Bytes chained_context(const std::vector<u32>& behind, const std::vector<u32>& input, const std::vector<u32>& ahead,
                                    const std::vector<NamedLookup>& named) {
    const std::vector<Bytes> coverages = {coverage_of(behind), coverage_of(input), coverage_of(ahead)};
    const usize header = 2 + 2 + 2 + 2 + 2 + 2 + 2 + 2 + named.size() * 4;
    const std::vector<usize> offsets = offsets_after(header, coverages);
    Bytes table{};
    put16(table, 3);
    put16(table, 1);
    put16(table, static_cast<i32>(offsets[0]));
    put16(table, 1);
    put16(table, static_cast<i32>(offsets[1]));
    put16(table, 1);
    put16(table, static_cast<i32>(offsets[2]));
    put16(table, static_cast<i32>(named.size()));
    for (const NamedLookup& lookup : named) {
        put16(table, static_cast<i32>(lookup.place));
        put16(table, static_cast<i32>(lookup.lookup));
    }
    for (const Bytes& coverage : coverages) append(table, coverage);
    return table;
}

// A context of format 3 of one glyph.
[[nodiscard]] Bytes plain_context(const std::vector<u32>& input, const std::vector<NamedLookup>& named) {
    Bytes table{};
    put16(table, 3);
    put16(table, 1);
    put16(table, static_cast<i32>(named.size()));
    put16(table, static_cast<i32>(8 + named.size() * 4));
    for (const NamedLookup& lookup : named) {
        put16(table, static_cast<i32>(lookup.place));
        put16(table, static_cast<i32>(lookup.lookup));
    }
    append(table, coverage_of(input));
    return table;
}

// Pairs by glyph: one first glyph, its seconds in order, each with an
// adjustment of the first glyph's advance.
[[nodiscard]] Bytes pairs_by_glyph(const u32 first, const std::vector<std::pair<u32, i32>>& seconds) {
    Bytes set{};
    put16(set, static_cast<i32>(seconds.size()));
    for (const std::pair<u32, i32>& second : seconds) {
        put16(set, static_cast<i32>(second.first));
        put16(set, second.second);
    }
    Bytes table{};
    put16(table, 1);
    put16(table, static_cast<i32>(12 + set.size()));
    put16(table, 0x0004);
    put16(table, 0);
    put16(table, 1);
    put16(table, 12);
    append(table, set);
    append(table, coverage_of({first}));
    return table;
}

// Pairs by class: first glyphs of class 1, second glyphs of class 1, and
// the adjustment of the first's advance for that pair of classes.
[[nodiscard]] Bytes pairs_by_class(const std::vector<u32>& firsts, const std::vector<ClassRange>& first_classes, const std::vector<ClassRange>& second_classes,
                                   const i32 adjustment) {
    const std::vector<Bytes> parts = {coverage_of(firsts), classes_of(first_classes), classes_of(second_classes)};
    const usize header = 16 + 4 * 2;
    const std::vector<usize> offsets = offsets_after(header, parts);
    Bytes table{};
    put16(table, 2);
    put16(table, static_cast<i32>(offsets[0]));
    put16(table, 0x0004);
    put16(table, 0);
    put16(table, static_cast<i32>(offsets[1]));
    put16(table, static_cast<i32>(offsets[2]));
    put16(table, 2);
    put16(table, 2);
    // Classes 0 0, 0 1, 1 0, 1 1.
    put16(table, 0);
    put16(table, 0);
    put16(table, 0);
    put16(table, adjustment);
    for (const Bytes& part : parts) append(table, part);
    return table;
}

struct Place final {
    i32 x{0};
    i32 y{0};
};

[[nodiscard]] Bytes anchor_of(const Place place) {
    Bytes table{};
    put16(table, 1);
    put16(table, place.x);
    put16(table, place.y);
    return table;
}

struct MarkGlyph final {
    u32 glyph{0};
    u32 mark_class{0};
    Place place{};
};

struct BaseGlyph final {
    u32 glyph{0};
    std::vector<Place> places{};
};

// Marks on bases: each mark's class and anchor, each base's anchor for
// each class. Marks and bases in order of glyph.
[[nodiscard]] Bytes marks_on_bases(const std::vector<MarkGlyph>& marks, const std::vector<BaseGlyph>& bases, const u32 class_count) {
    std::vector<u32> mark_glyphs{};
    std::vector<u32> base_glyphs{};
    for (const MarkGlyph& mark : marks) mark_glyphs.push_back(mark.glyph);
    for (const BaseGlyph& base : bases) base_glyphs.push_back(base.glyph);
    Bytes mark_array{};
    put16(mark_array, static_cast<i32>(marks.size()));
    for (usize index = 0; index < marks.size(); ++index) {
        put16(mark_array, static_cast<i32>(marks[index].mark_class));
        put16(mark_array, static_cast<i32>(2 + marks.size() * 4 + index * 6));
    }
    for (const MarkGlyph& mark : marks) append(mark_array, anchor_of(mark.place));
    Bytes base_array{};
    put16(base_array, static_cast<i32>(bases.size()));
    const usize cells = bases.size() * class_count;
    for (usize index = 0; index < cells; ++index) put16(base_array, static_cast<i32>(2 + cells * 2 + index * 6));
    for (const BaseGlyph& base : bases) {
        for (const Place place : base.places) append(base_array, anchor_of(place));
    }
    const std::vector<Bytes> parts = {coverage_of(mark_glyphs), coverage_of(base_glyphs), mark_array, base_array};
    const std::vector<usize> offsets = offsets_after(12, parts);
    Bytes table{};
    put16(table, 1);
    put16(table, static_cast<i32>(offsets[0]));
    put16(table, static_cast<i32>(offsets[1]));
    put16(table, static_cast<i32>(class_count));
    put16(table, static_cast<i32>(offsets[2]));
    put16(table, static_cast<i32>(offsets[3]));
    for (const Bytes& part : parts) append(table, part);
    return table;
}

struct Lookup final {
    u32 kind{0};
    std::vector<Bytes> subtables{};
};

struct Feature final {
    std::string tag{};
    std::vector<u32> lookups{};
};

// A table of substitutions or positions: one script (the default one)
// whose default language system has every feature.
[[nodiscard]] Bytes layout_of(const std::vector<Feature>& features, const std::vector<Lookup>& lookups) {
    Bytes scripts{};
    put16(scripts, 1);
    for (const char letter : std::string("DFLT")) put8(scripts, static_cast<u8>(letter));
    put16(scripts, 8);
    put16(scripts, 4);
    put16(scripts, 0);
    put16(scripts, 0);
    put16(scripts, 0xFFFF);
    put16(scripts, static_cast<i32>(features.size()));
    for (usize index = 0; index < features.size(); ++index) put16(scripts, static_cast<i32>(index));
    std::vector<Bytes> feature_bodies{};
    for (const Feature& feature : features) {
        Bytes body{};
        put16(body, 0);
        put16(body, static_cast<i32>(feature.lookups.size()));
        for (const u32 lookup : feature.lookups) put16(body, static_cast<i32>(lookup));
        feature_bodies.push_back(body);
    }
    Bytes feature_list{};
    put16(feature_list, static_cast<i32>(features.size()));
    const std::vector<usize> feature_offsets = offsets_after(2 + features.size() * 6, feature_bodies);
    for (usize index = 0; index < features.size(); ++index) {
        for (const char letter : features[index].tag) put8(feature_list, static_cast<u8>(letter));
        put16(feature_list, static_cast<i32>(feature_offsets[index]));
    }
    for (const Bytes& body : feature_bodies) append(feature_list, body);
    std::vector<Bytes> lookup_bodies{};
    for (const Lookup& lookup : lookups) {
        Bytes body{};
        put16(body, static_cast<i32>(lookup.kind));
        put16(body, 0);
        put16(body, static_cast<i32>(lookup.subtables.size()));
        for (const usize offset : offsets_after(6 + lookup.subtables.size() * 2, lookup.subtables)) put16(body, static_cast<i32>(offset));
        for (const Bytes& subtable : lookup.subtables) append(body, subtable);
        lookup_bodies.push_back(body);
    }
    Bytes lookup_list{};
    put16(lookup_list, static_cast<i32>(lookups.size()));
    for (const usize offset : offsets_after(2 + lookups.size() * 2, lookup_bodies)) put16(lookup_list, static_cast<i32>(offset));
    for (const Bytes& body : lookup_bodies) append(lookup_list, body);
    Bytes table{};
    put32(table, 0x00010000U);
    put16(table, 10);
    put16(table, static_cast<i32>(10 + scripts.size()));
    put16(table, static_cast<i32>(10 + scripts.size() + feature_list.size()));
    append(table, scripts);
    append(table, feature_list);
    append(table, lookup_list);
    return table;
}

// ---- the font ------------------------------------------------------------------------------------------

inline constexpr u32 glyph_count = 28;

struct Built final {
    Bytes bytes{};
    usize layout_start{0};
    usize layout_size{0};
};

[[nodiscard]] Built shaping_font(const bool with_layout) {
    const u32 advances[glyph_count] = {500, 600, 600, 300, 250, 520, 700, 680, 250, 200, 550, 550, 500, 510,
                                       300, 300, 600, 580, 560, 500, 0,   620, 100, 1000, 500, 500, 540, 560};
    std::vector<Bytes> glyph_bytes{};
    std::vector<Metric> metrics{};
    for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
        // A square for each glyph but the space; the marks' squares small.
        const bool mark = glyph == 9 || glyph == 20;
        if (glyph == 8) {
            glyph_bytes.push_back(Bytes{});
        } else if (mark) {
            glyph_bytes.push_back(simple_glyph({{{40, 600, true}, {40, 700, true}, {160, 700, true}, {160, 600, true}}}));
        } else {
            glyph_bytes.push_back(simple_glyph({{{50, 0, true}, {50, 500, true}, {450, 500, true}, {450, 0, true}}}));
        }
        metrics.push_back(Metric{advances[glyph], 0});
    }
    Bytes glyf{};
    Bytes loca{};
    Bytes hmtx{};
    glyph_tables(glyph_bytes, metrics, glyph_count, true, glyf, loca, hmtx);
    const Bytes map = map_format_12({{' ', ' ', 8},   {'(', '(', 14},      {')', ')', 15},      {'A', 'A', 6},       {'T', 'T', 21},      {'V', 'V', 7},
                                     {'a', 'b', 1},   {'c', 'd', 26},      {'e', 'e', 10},      {'f', 'f', 3},       {'i', 'i', 4},       {'o', 'o', 18},
                                     {'x', 'x', 12},  {0xE9, 0xE9, 11},    {0x301, 0x301, 9},   {0x323, 0x323, 20},  {0x5D0, 0x5D1, 16},  {0x1100, 0x1100, 24},
                                     {0x1161, 0x1161, 25}, {0xAC00, 0xAC00, 23}});
    // Glyph classes: bases 1, the ligature 2, marks 3.
    Bytes gdef{};
    put32(gdef, 0x00010000U);
    put16(gdef, 12);
    put16(gdef, 0);
    put16(gdef, 0);
    put16(gdef, 0);
    append(gdef, classes_of({{1, 4, 1}, {5, 5, 2}, {6, 8, 1}, {9, 9, 3}, {10, 19, 1}, {20, 20, 3}, {21, 27, 1}}));
    const Bytes gsub = layout_of({{"calt", {2}}, {"ccmp", {4}}, {"liga", {0}}, {"rlig", {5}}, {"smcp", {3}}},
                                 {{4, {ligature_substitution(3, {{{4}, 5}})}},
                                  {1, {single_substitution({{12, 13}})}},
                                  {6, {chained_context({1}, {12}, {2}, {{0, 1}})}},
                                  {1, {single_substitution({{6, 19}})}},
                                  {2, {multiple_substitution(26, {22, 22})}},
                                  {5, {plain_context({27}, {{0, 5}})}}});
    const Bytes gpos = layout_of({{"kern", {0, 1}}, {"mark", {2}}},
                                 {{2, {pairs_by_glyph(6, {{7, -80}})}},
                                  {2, {pairs_by_class({21}, {{21, 21, 1}}, {{1, 1, 1}, {18, 18, 1}}, -50)}},
                                  {4, {marks_on_bases({{9, 0, {100, 500}}, {20, 1, {90, -20}}}, {{1, {{300, 520}, {300, -10}}}, {18, {{280, 510}, {280, -10}}}}, 2)}}});
    std::vector<NamedTable> tables = {};
    if (with_layout) {
        tables.push_back({"GDEF", gdef});
        tables.push_back({"GPOS", gpos});
        tables.push_back({"GSUB", gsub});
    }
    tables.push_back({"cmap", cmap_table({{3, 10, map}})});
    tables.push_back({"glyf", glyf});
    tables.push_back({"head", head_table(1000, true)});
    tables.push_back({"hhea", hhea_table(800, -200, 0, glyph_count)});
    tables.push_back({"hmtx", hmtx});
    tables.push_back({"loca", loca});
    tables.push_back({"maxp", maxp_table(glyph_count)});
    Built built{};
    built.bytes = font_of(0x00010000U, tables);
    if (with_layout) {
        // The three layout tables are the first in the file.
        built.layout_start = 12 + tables.size() * 16;
        built.layout_size = ((gdef.size() + 3) / 4) * 4 + ((gpos.size() + 3) / 4) * 4 + ((gsub.size() + 3) / 4) * 4;
    }
    return built;
}

// ---- shaping and comparing --------------------------------------------------------------------------------

struct Want final {
    u32 glyph{0};
    u32 cluster{0};
    i32 x_advance{0};
    i32 x_offset{0};
    i32 y_offset{0};
};

struct Opened final {
    Bytes bytes{};
    stxlib::Shared<glyphs::Typeface> typeface{};
    glyphs::Shaper shaper{};
    bool ready{false};
};

void open_font(Opened& opened, const bool with_layout) {
    opened.bytes = shaping_font(with_layout).bytes;
    stxlib::Result<stxlib::Shared<glyphs::Typeface>> made = glyphs::open_typeface(view_of(opened.bytes), 0);
    CHECK(made.succeeded());
    if (made.failed()) return;
    opened.typeface = *made.value();
    opened.ready = opened.shaper.open(opened.typeface).succeeded();
    CHECK(opened.ready);
}

[[nodiscard]] bool shapes_to(Opened& opened, const std::u32string& text, const std::vector<Want>& wanted, const bool right_to_left = false,
                             const i64 size = 0, const std::vector<glyphs::TextFeature>& features = {}) {
    if (!opened.ready) return false;
    glyphs::TextRun run{};
    run.characters = stxlib::View<u32>(reinterpret_cast<const u32*>(text.data()), text.size());
    run.direction = right_to_left ? glyphs::TextDirection::right_to_left : glyphs::TextDirection::left_to_right;
    run.size = size;
    run.features = stxlib::View<glyphs::TextFeature>(features.data(), features.size());
    stxlib::Vector<glyphs::ShapedGlyph> shaped{};
    if (opened.shaper.shape(run, shaped).failed()) {
        std::fprintf(stderr, "  shaping was refused\n");
        return false;
    }
    bool same = shaped.count() == wanted.size();
    for (usize index = 0; same && index < wanted.size(); ++index) {
        same = shaped[index].glyph == wanted[index].glyph && shaped[index].cluster == wanted[index].cluster && shaped[index].x_advance == wanted[index].x_advance &&
               shaped[index].x_offset == wanted[index].x_offset && shaped[index].y_offset == wanted[index].y_offset && shaped[index].y_advance == 0;
    }
    if (!same) {
        std::fprintf(stderr, "  shaped:");
        for (usize index = 0; index < shaped.count(); ++index) {
            std::fprintf(stderr, " [%u c%u a%d o%d,%d]", shaped[index].glyph, shaped[index].cluster, shaped[index].x_advance, shaped[index].x_offset, shaped[index].y_offset);
        }
        std::fprintf(stderr, "\n");
    }
    return same;
}

void plain_text() {
    Opened plain{};
    open_font(plain, false);
    CHECK(shapes_to(plain, U"ab", {{1, 0, 600, 0, 0}, {2, 1, 600, 0, 0}}));
    // No layout tables: no ligature, no kerning; a character the font has
    // not is glyph 0.
    CHECK(shapes_to(plain, U"fi", {{3, 0, 300, 0, 0}, {4, 1, 250, 0, 0}}));
    CHECK(shapes_to(plain, U"AV", {{6, 0, 700, 0, 0}, {7, 1, 680, 0, 0}}));
    CHECK(shapes_to(plain, U"aZb", {{1, 0, 600, 0, 0}, {0, 1, 500, 0, 0}, {2, 2, 600, 0, 0}}));
    CHECK(shapes_to(plain, U"", {}));
    Opened opened{};
    open_font(opened, true);
    CHECK(shapes_to(opened, U"ab", {{1, 0, 600, 0, 0}, {2, 1, 600, 0, 0}}));
    // Clusters are the caller's, when given.
    const std::u32string text = U"ab";
    const u32 clusters[2] = {10, 17};
    glyphs::TextRun run{};
    run.characters = stxlib::View<u32>(reinterpret_cast<const u32*>(text.data()), text.size());
    run.clusters = stxlib::View<u32>(clusters);
    stxlib::Vector<glyphs::ShapedGlyph> shaped{};
    CHECK(opened.shaper.shape(run, shaped).succeeded() && shaped.count() == 2 && shaped[0].cluster == 10 && shaped[1].cluster == 17);
}

void ligatures() {
    Opened opened{};
    open_font(opened, true);
    CHECK(shapes_to(opened, U"fi", {{5, 0, 520, 0, 0}}));
    CHECK(shapes_to(opened, U"afib", {{1, 0, 600, 0, 0}, {5, 1, 520, 0, 0}, {2, 3, 600, 0, 0}}));
    CHECK(shapes_to(opened, U"if", {{4, 0, 250, 0, 0}, {3, 1, 300, 0, 0}}));
    // A zero width non-joiner between them keeps them apart; it is drawn
    // as the space, with no width. A joiner does not keep them apart.
    CHECK(shapes_to(opened, U"f‌i", {{3, 0, 300, 0, 0}, {8, 1, 0, 0, 0}, {4, 2, 250, 0, 0}}));
    CHECK(shapes_to(opened, U"f‍i", {{5, 0, 520, 0, 0}, {8, 0, 0, 0, 0}}));
    // The feature turned off.
    CHECK(shapes_to(opened, U"fi", {{3, 0, 300, 0, 0}, {4, 1, 250, 0, 0}}, false, 0, {{glyphs::text_tag('l', 'i', 'g', 'a'), 0, 0, glyphs::whole_run}}));
}

void kerning() {
    Opened opened{};
    open_font(opened, true);
    CHECK(shapes_to(opened, U"AV", {{6, 0, 620, 0, 0}, {7, 1, 680, 0, 0}}));
    CHECK(shapes_to(opened, U"VA", {{7, 0, 680, 0, 0}, {6, 1, 700, 0, 0}}));
    // By class: T before o or a, not before b.
    CHECK(shapes_to(opened, U"To", {{21, 0, 570, 0, 0}, {18, 1, 560, 0, 0}}));
    CHECK(shapes_to(opened, U"Ta", {{21, 0, 570, 0, 0}, {1, 1, 600, 0, 0}}));
    CHECK(shapes_to(opened, U"Tb", {{21, 0, 620, 0, 0}, {2, 1, 600, 0, 0}}));
    CHECK(shapes_to(opened, U"AV", {{6, 0, 700, 0, 0}, {7, 1, 680, 0, 0}}, false, 0, {{glyphs::text_tag('k', 'e', 'r', 'n'), 0, 0, glyphs::whole_run}}));
    // At 20 pixels (1280 in 26.6). The scale for advances is
    // (1280 * 65536 + 500) / 1000 = 83886; the shaper's em is
    // (83886 * 1000 + 32768) >> 16 = 1280 and its multiplier
    // (1280 << 16) / 1000 = 83886. A: (700 * 83886 + 32) / 64 = 917503,
    // (917503 + 512) >> 10 = 896. V: (680 * 83886 + 32) / 64 = 891288,
    // (891288 + 512) >> 10 = 870. The kerning: (-80 * 83886 + 32768) >> 16
    // = -102. So A advances 794.
    CHECK(shapes_to(opened, U"AV", {{6, 0, 794, 0, 0}, {7, 1, 870, 0, 0}}, false, 1280));
    CHECK(shapes_to(opened, U"a", {{1, 0, 768, 0, 0}}, false, 1280));
}

void marks() {
    Opened opened{};
    open_font(opened, true);
    // The font has e with acute: the two characters are that one.
    CHECK(shapes_to(opened, U"é", {{11, 0, 550, 0, 0}}));
    CHECK(shapes_to(opened, U"é", {{11, 0, 550, 0, 0}}));
    // It has no a with acute: the acute is placed by its anchor (100, 500)
    // on a's (300, 520), after a's advance of 600: 200 - 600 across, 20 up.
    // A mark has no advance.
    CHECK(shapes_to(opened, U"á", {{1, 0, 600, 0, 0}, {9, 0, 0, -400, 20}}));
    // A character made of a base and a mark the font has separately is
    // taken apart: a with acute (U+00E1).
    CHECK(shapes_to(opened, U"á", {{1, 0, 600, 0, 0}, {9, 0, 0, -400, 20}}));
    // Two marks come in the order of their classes, the one below first,
    // whichever way they were typed; each is placed on the base. On o:
    // dot below (90, -20) on (280, -10): 190 - 560, 10; acute on (280,
    // 510): 180 - 560, 10.
    CHECK(shapes_to(opened, U"ọ́", {{18, 0, 560, 0, 0}, {20, 0, 0, -370, 10}, {9, 0, 0, -380, 10}}));
    CHECK(shapes_to(opened, U"ọ́", {{18, 0, 560, 0, 0}, {20, 0, 0, -370, 10}, {9, 0, 0, -380, 10}}));
    // A mark on a base the table has no anchor for stays where it is, with
    // no advance.
    CHECK(shapes_to(opened, U"b́", {{2, 0, 600, 0, 0}, {9, 0, 0, 0, 0}}));
}

void contexts() {
    Opened opened{};
    open_font(opened, true);
    CHECK(shapes_to(opened, U"axb", {{1, 0, 600, 0, 0}, {13, 1, 510, 0, 0}, {2, 2, 600, 0, 0}}));
    CHECK(shapes_to(opened, U"ax", {{1, 0, 600, 0, 0}, {12, 1, 500, 0, 0}}));
    CHECK(shapes_to(opened, U"xb", {{12, 0, 500, 0, 0}, {2, 1, 600, 0, 0}}));
    CHECK(shapes_to(opened, U"bxa", {{2, 0, 600, 0, 0}, {12, 1, 500, 0, 0}, {1, 2, 600, 0, 0}}));
    CHECK(shapes_to(opened, U"axbaxb", {{1, 0, 600, 0, 0}, {13, 1, 510, 0, 0}, {2, 2, 600, 0, 0}, {1, 3, 600, 0, 0}, {13, 4, 510, 0, 0}, {2, 5, 600, 0, 0}}));
    // A mark between them is not in the way: the context passes over what
    // is not to show (a joiner), and the x is still found.
    CHECK(shapes_to(opened, U"a‍xb", {{1, 0, 600, 0, 0}, {8, 0, 0, 0, 0}, {13, 2, 510, 0, 0}, {2, 3, 600, 0, 0}}));
}

void features() {
    Opened opened{};
    open_font(opened, true);
    const u32 smcp = glyphs::text_tag('s', 'm', 'c', 'p');
    CHECK(shapes_to(opened, U"AA", {{6, 0, 700, 0, 0}, {6, 1, 700, 0, 0}}));
    CHECK(shapes_to(opened, U"AA", {{19, 0, 500, 0, 0}, {19, 1, 500, 0, 0}}, false, 0, {{smcp, 1, 0, glyphs::whole_run}}));
    // For the second character only.
    CHECK(shapes_to(opened, U"AA", {{6, 0, 700, 0, 0}, {19, 1, 500, 0, 0}}, false, 0, {{smcp, 1, 1, 2}}));
    // A feature the font has not changes nothing.
    CHECK(shapes_to(opened, U"AA", {{6, 0, 700, 0, 0}, {6, 1, 700, 0, 0}}, false, 0, {{glyphs::text_tag('z', 'z', 'z', 'z'), 1, 0, glyphs::whole_run}}));
}

void growth_and_nesting() {
    Opened opened{};
    open_font(opened, true);
    // One glyph becomes two, both of its cluster.
    CHECK(shapes_to(opened, U"aca", {{1, 0, 600, 0, 0}, {22, 1, 100, 0, 0}, {22, 1, 100, 0, 0}, {1, 2, 600, 0, 0}}));
    // d's lookup names itself: it is entered as deep as is allowed and no
    // deeper, and the run is refused, not shaped wrongly.
    const std::u32string text = U"ada";
    glyphs::TextRun run{};
    run.characters = stxlib::View<u32>(reinterpret_cast<const u32*>(text.data()), text.size());
    stxlib::Vector<glyphs::ShapedGlyph> shaped{};
    const stxlib::Status status = opened.shaper.shape(run, shaped);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded && shaped.count() == 0);
    // The shaper is as good as before.
    CHECK(shapes_to(opened, U"ab", {{1, 0, 600, 0, 0}, {2, 1, 600, 0, 0}}));
}

void right_to_left() {
    Opened opened{};
    open_font(opened, true);
    // Glyphs come out in the order they are drawn, from the left: the last
    // character first.
    CHECK(shapes_to(opened, U"אב", {{17, 1, 580, 0, 0}, {16, 0, 600, 0, 0}}, true));
    // A bracket is drawn as its mirror image.
    CHECK(shapes_to(opened, U"(א)", {{14, 2, 300, 0, 0}, {16, 1, 600, 0, 0}, {15, 0, 300, 0, 0}}, true));
    // Hebrew set left to right (as a caller's layout may ask) comes out in
    // reading order.
    CHECK(shapes_to(opened, U"אב", {{16, 0, 600, 0, 0}, {17, 1, 580, 0, 0}}, false));
    // Latin set right to left.
    CHECK(shapes_to(opened, U"ab", {{2, 1, 600, 0, 0}, {1, 0, 600, 0, 0}}, true));
}

void hangul() {
    Opened opened{};
    open_font(opened, true);
    // The two letters are the syllable, which the font has.
    CHECK(shapes_to(opened, U"가", {{23, 0, 1000, 0, 0}}));
    CHECK(shapes_to(opened, U"가", {{23, 0, 1000, 0, 0}}));
    // A leading letter alone stays a letter.
    CHECK(shapes_to(opened, U"ᄀ", {{24, 0, 500, 0, 0}}));
}

void hidden_and_spaces() {
    Opened opened{};
    open_font(opened, true);
    // A soft hyphen is not shown: the space's glyph, with no width.
    CHECK(shapes_to(opened, U"a­b", {{1, 0, 600, 0, 0}, {8, 1, 0, 0, 0}, {2, 2, 600, 0, 0}}));
    // Spaces the font has not are its space, as wide as they should be:
    // an em, half an em, a fifth, and a narrow one half of the space.
    CHECK(shapes_to(opened, U" ", {{8, 0, 1000, 0, 0}}));
    CHECK(shapes_to(opened, U" ", {{8, 0, 500, 0, 0}}));
    CHECK(shapes_to(opened, U" ", {{8, 0, 200, 0, 0}}));
    CHECK(shapes_to(opened, U" ", {{8, 0, 125, 0, 0}}));
    CHECK(shapes_to(opened, U"a b", {{1, 0, 600, 0, 0}, {8, 1, 250, 0, 0}, {2, 2, 600, 0, 0}}));
}

void refusals_and_plans() {
    Opened opened{};
    open_font(opened, true);
    glyphs::Shaper closed{};
    stxlib::Vector<glyphs::ShapedGlyph> shaped{};
    const std::u32string text = U"ab";
    glyphs::TextRun run{};
    run.characters = stxlib::View<u32>(reinterpret_cast<const u32*>(text.data()), text.size());
    CHECK(closed.shape(run, shaped).failed() && closed.plans_made() == 0 && closed.bytes_held() == 0);
    CHECK(closed.open(stxlib::Shared<glyphs::Typeface>()).failed());
    // Clusters that are not as many as the characters; a size out of range.
    const u32 one_cluster[1] = {0};
    glyphs::TextRun wrong = run;
    wrong.clusters = stxlib::View<u32>(one_cluster);
    CHECK(opened.shaper.shape(wrong, shaped).failed());
    wrong = run;
    wrong.size = -1;
    CHECK(opened.shaper.shape(wrong, shaped).failed());
    wrong.size = (static_cast<i64>(1) << 24) + 1;
    CHECK(opened.shaper.shape(wrong, shaped).failed());
    // A plan is made once for a script, direction and set of features.
    CHECK(opened.shaper.plans_made() == 0);
    CHECK(opened.shaper.shape(run, shaped).succeeded() && opened.shaper.plans_made() == 1);
    const usize held = opened.shaper.bytes_held();
    CHECK(held > 0);
    for (int round = 0; round < 50; ++round) CHECK(opened.shaper.shape(run, shaped).succeeded());
    CHECK(opened.shaper.plans_made() == 1 && opened.shaper.bytes_held() == held);
    run.direction = glyphs::TextDirection::right_to_left;
    CHECK(opened.shaper.shape(run, shaped).succeeded() && opened.shaper.plans_made() == 2);
    // A limit of one plan: every change of plan makes one again.
    glyphs::Shaper small{};
    glyphs::ShaperLimits limits{};
    limits.plans = 1;
    limits.characters = 4;
    CHECK(small.open(opened.typeface, limits).succeeded());
    run.direction = glyphs::TextDirection::left_to_right;
    CHECK(small.shape(run, shaped).succeeded() && small.plans_made() == 1);
    run.direction = glyphs::TextDirection::right_to_left;
    CHECK(small.shape(run, shaped).succeeded() && small.plans_made() == 2);
    run.direction = glyphs::TextDirection::left_to_right;
    CHECK(small.shape(run, shaped).succeeded() && small.plans_made() == 3 && shaped.count() == 2 && shaped[0].glyph == 1);
    // A run longer than the limit.
    const std::u32string longer = U"ababa";
    run.characters = stxlib::View<u32>(reinterpret_cast<const u32*>(longer.data()), longer.size());
    const stxlib::Status status = small.shape(run, shaped);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    small.release();
    CHECK(small.bytes_held() == 0 && small.shape(run, shaped).failed());
}

// ---- the layout tables altered ----------------------------------------------------------------------------

std::uint64_t random_state = 0x6A09E667F3BCC909ULL;
[[nodiscard]] std::uint64_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

// The runs every altered font is shaped on: each is refused or shaped, and
// a shaped one is within the bound.
[[nodiscard]] bool shapes_within_bounds(const Bytes& bytes, std::uint64_t& shaped_runs, std::uint64_t& refused_runs) {
    stxlib::Result<stxlib::Shared<glyphs::Typeface>> made = glyphs::open_typeface(view_of(bytes), 0);
    if (made.failed()) return true;
    glyphs::Shaper shaper{};
    if (shaper.open(*made.value()).failed()) return true;
    const std::u32string texts[] = {U"afib", U"AVTo", U"axbaxb", U"aca", U"ada", U"ọ́á", U"é á", U"אב(", U"가가", U"dddd cccc AAAA"};
    stxlib::Vector<glyphs::ShapedGlyph> shaped{};
    bool kept = true;
    for (const std::u32string& text : texts) {
        for (int direction = 0; direction < 2 && kept; ++direction) {
            glyphs::TextRun run{};
            run.characters = stxlib::View<u32>(reinterpret_cast<const u32*>(text.data()), text.size());
            run.direction = direction == 0 ? glyphs::TextDirection::left_to_right : glyphs::TextDirection::right_to_left;
            run.size = direction == 0 ? 0 : 13 * 64;
            if (shaper.shape(run, shaped).succeeded()) {
                shaped_runs = shaped_runs + 1;
                const usize most = text.size() * 64 > 16384 ? text.size() * 64 : 16384;
                kept = shaped.count() <= most;
            } else {
                refused_runs = refused_runs + 1;
                kept = shaped.count() == 0;
            }
        }
    }
    return kept;
}

void altered_tables() {
    const Built good = shaping_font(true);
    std::uint64_t shaped_runs = 0;
    std::uint64_t refused_runs = 0;
    std::uint64_t tried = 0;
    bool kept = shapes_within_bounds(good.bytes, shaped_runs, refused_runs);
    // Ten runs each way; the two with a d in them are refused both ways.
    CHECK(kept && shaped_runs == 16 && refused_runs == 4);
    const u32 values[] = {0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF};
    for (usize at = good.layout_start; at + 1 < good.layout_start + good.layout_size && kept; at = at + 2) {
        for (const u32 value : values) {
            Bytes changed = good.bytes;
            changed[at] = static_cast<u8>(value >> 8);
            changed[at + 1] = static_cast<u8>(value & 0xFFU);
            kept = kept && shapes_within_bounds(changed, shaped_runs, refused_runs);
            tried = tried + 1;
        }
    }
    CHECK(kept);
    for (int round = 0; round < 20000 && kept; ++round) {
        Bytes changed = good.bytes;
        const usize changes = 1 + next_random() % 4;
        for (usize change = 0; change < changes; ++change) {
            changed[good.layout_start + next_random() % good.layout_size] = static_cast<u8>(next_random() & 0xFFU);
        }
        kept = shapes_within_bounds(changed, shaped_runs, refused_runs);
        tried = tried + 1;
    }
    CHECK(kept);
    std::printf("search of layout tables through the shaper: %llu fonts, %llu runs shaped, %llu refused\n", static_cast<unsigned long long>(tried),
                static_cast<unsigned long long>(shaped_runs), static_cast<unsigned long long>(refused_runs));
    CHECK(tried > 20000 && shaped_runs > 100000);
}

} // namespace

int main() {
    plain_text();
    ligatures();
    kerning();
    marks();
    contexts();
    features();
    growth_and_nesting();
    right_to_left();
    hangul();
    hidden_and_spaces();
    refusals_and_plans();
    altered_tables();
    std::printf("glyph checks: %s\n", glyphs::glyph_checks_enabled() ? "on" : "off");
#if defined(STXLIB_GLYPHS_CHECKS_EXPECTED)
    CHECK(glyphs::glyph_checks_enabled());
#endif
    const int status = stxlib_tests::finish("stxlib_glyph_shape_tests");
    return status;
}
