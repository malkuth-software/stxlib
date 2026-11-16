// stxlib::glyphs: reading a font's layout tables (layout_tables.hpp).

#include "algorithms/glyphs/layout_tables.hpp"

namespace stxlib::glyphs::detail {

u32 coverage_index(const Span& table, const usize offset, const u32 glyph) noexcept {
    if (offset == 0 || !has(table, offset, 4)) return not_covered;
    const u32 format = read_u16(table, offset);
    const usize count = read_u16(table, offset + 2);
    u32 found = not_covered;
    if (format == 1) {
        // Glyphs in order: a glyph's index is its place.
        if (!has(table, offset + 4, count * 2)) return not_covered;
        usize low = 0;
        usize high = count;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            const u32 listed = read_u16(table, offset + 4 + middle * 2);
            if (listed < glyph) {
                low = middle + 1;
            } else if (listed > glyph) {
                high = middle;
            } else {
                found = static_cast<u32>(middle);
                break;
            }
        }
    } else if (format == 2) {
        // Ranges in order: first glyph, last glyph, the index of the first.
        if (!has(table, offset + 4, count * 6)) return not_covered;
        usize low = 0;
        usize high = count;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            const usize range = offset + 4 + middle * 6;
            const u32 first = read_u16(table, range);
            const u32 last = read_u16(table, range + 2);
            if (last < glyph) {
                low = middle + 1;
            } else if (first > glyph) {
                high = middle;
            } else {
                found = read_u16(table, range + 4) + (glyph - first);
                break;
            }
        }
    }
    return found;
}

u32 class_of(const Span& table, const usize offset, const u32 glyph) noexcept {
    if (offset == 0 || !has(table, offset, 4)) return 0;
    const u32 format = read_u16(table, offset);
    u32 found = 0;
    if (format == 1) {
        // A first glyph and a class for each glyph from it on.
        if (!has(table, offset, 6)) return 0;
        const u32 first = read_u16(table, offset + 2);
        const usize count = read_u16(table, offset + 4);
        if (glyph >= first && glyph - first < count && has(table, offset + 6, count * 2)) {
            found = read_u16(table, offset + 6 + static_cast<usize>(glyph - first) * 2);
        }
    } else if (format == 2) {
        // Ranges in order: first glyph, last glyph, their class.
        const usize count = read_u16(table, offset + 2);
        if (!has(table, offset + 4, count * 6)) return 0;
        usize low = 0;
        usize high = count;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            const usize range = offset + 4 + middle * 6;
            const u32 first = read_u16(table, range);
            const u32 last = read_u16(table, range + 2);
            if (last < glyph) {
                low = middle + 1;
            } else if (first > glyph) {
                high = middle;
            } else {
                found = read_u16(table, range + 4);
                break;
            }
        }
    }
    return found;
}

// ---- glyph definitions --------------------------------------------------------------------------

GlyphDefinitions open_definitions(const Font& font) noexcept {
    GlyphDefinitions definitions{};
    const Span table = table_of(font, font.definitions);
    // Version 1.0: the offsets of the glyph classes, the attachment list,
    // the ligature carets and the mark attachment classes. Version 1.2
    // adds the mark glyph sets.
    if (!has(table, 0, 12) || read_u16(table, 0) != 1) return definitions;
    definitions.table = table;
    definitions.classes = read_u16(table, 4);
    definitions.mark_classes = read_u16(table, 10);
    if (read_u16(table, 2) >= 2 && has(table, 12, 2)) definitions.mark_sets = read_u16(table, 12);
    return definitions;
}

u32 glyph_class(const GlyphDefinitions& definitions, const u32 glyph) noexcept {
    const u32 result = class_of(definitions.table, definitions.classes, glyph);
    return result;
}

u32 mark_attachment_class(const GlyphDefinitions& definitions, const u32 glyph) noexcept {
    const u32 result = class_of(definitions.table, definitions.mark_classes, glyph);
    return result;
}

bool in_mark_set(const GlyphDefinitions& definitions, const u32 set, const u32 glyph) noexcept {
    const Span& table = definitions.table;
    const usize list = definitions.mark_sets;
    // A format of 1, a count, and a 32-bit offset of a coverage for each
    // set, from the list's start.
    if (list == 0 || !has(table, list, 4) || read_u16(table, list) != 1) return false;
    const usize count = read_u16(table, list + 2);
    if (set >= count || !has(table, list + 4, count * 4)) return false;
    const usize coverage = read_u32(table, list + 4 + static_cast<usize>(set) * 4);
    if (coverage == 0 || coverage > table.size - list) return false;
    const bool result = coverage_index(table, list + coverage, glyph) != not_covered;
    return result;
}

// ---- substitutions and positions ------------------------------------------------------------------

LayoutTable open_layout(const Font& font, const bool substitution) noexcept {
    LayoutTable layout{};
    layout.substitution = substitution;
    const Span table = table_of(font, substitution ? font.substitutions : font.positions);
    // Version 1.0 or 1.1, then the offsets of the lists of scripts,
    // features and lookups.
    if (!has(table, 0, 10) || read_u16(table, 0) != 1) return layout;
    layout.table = table;
    layout.scripts = read_u16(table, 4);
    layout.features = read_u16(table, 6);
    layout.lookups = read_u16(table, 8);
    return layout;
}

u32 script_count(const LayoutTable& layout) noexcept {
    const Span& table = layout.table;
    if (layout.scripts == 0 || !has(table, layout.scripts, 2)) return 0;
    const u32 count = read_u16(table, layout.scripts);
    const u32 result = has(table, layout.scripts + 2, static_cast<usize>(count) * 6) ? count : 0;
    return result;
}

bool find_script(const LayoutTable& layout, const u32 tag, usize& script) noexcept {
    script = 0;
    const Span& table = layout.table;
    const usize count = script_count(layout);
    // Records in order of tag: a tag and the script's offset from the list.
    usize low = 0;
    usize high = count;
    bool found = false;
    while (low < high && !found) {
        const usize middle = low + (high - low) / 2;
        const usize record = layout.scripts + 2 + middle * 6;
        const u32 listed = read_u32(table, record);
        if (listed < tag) {
            low = middle + 1;
        } else if (listed > tag) {
            high = middle;
        } else {
            const usize offset = read_u16(table, record + 4);
            found = offset != 0 && has(table, layout.scripts + offset, 4);
            if (found) script = layout.scripts + offset;
            break;
        }
    }
    return found;
}

LanguageSystem language_system(const LayoutTable& layout, const usize script, const u32 language) noexcept {
    LanguageSystem system{};
    const Span& table = layout.table;
    if (script == 0 || !has(table, script, 4)) return system;
    // The script: the offset of its default language system, then records
    // in order of tag, each a tag and a system's offset from the script.
    usize place = read_u16(table, script);
    const usize count = read_u16(table, script + 2);
    if (language != 0 && has(table, script + 4, count * 6)) {
        usize low = 0;
        usize high = count;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            const usize record = script + 4 + middle * 6;
            const u32 listed = read_u32(table, record);
            if (listed < language) {
                low = middle + 1;
            } else if (listed > language) {
                high = middle;
            } else {
                const usize offset = read_u16(table, record + 4);
                if (offset != 0) place = offset;
                break;
            }
        }
    }
    if (place == 0 || !has(table, script + place, 6)) return system;
    // A language system: an offset kept for a use it never had, the
    // required feature, and the numbers of its features.
    const usize start = script + place;
    const u32 features = read_u16(table, start + 4);
    if (!has(table, start + 6, static_cast<usize>(features) * 2)) return system;
    system.found = true;
    system.required_feature = read_u16(table, start + 2);
    system.features = start + 6;
    system.feature_count = features;
    return system;
}

u32 language_feature(const LayoutTable& layout, const LanguageSystem& system, const u32 index) noexcept {
    const u32 result = index < system.feature_count ? read_u16(layout.table, system.features + static_cast<usize>(index) * 2) : no_feature;
    return result;
}

u32 feature_count(const LayoutTable& layout) noexcept {
    const Span& table = layout.table;
    if (layout.features == 0 || !has(table, layout.features, 2)) return 0;
    const u32 count = read_u16(table, layout.features);
    const u32 result = has(table, layout.features + 2, static_cast<usize>(count) * 6) ? count : 0;
    return result;
}

bool feature_at(const LayoutTable& layout, const u32 index, LayoutFeature& feature) noexcept {
    feature = LayoutFeature{};
    const Span& table = layout.table;
    if (index >= feature_count(layout)) return false;
    // A record: the tag and the feature's offset from the list. A feature:
    // an offset of parameters, and the numbers of its lookups.
    const usize record = layout.features + 2 + static_cast<usize>(index) * 6;
    const usize offset = read_u16(table, record + 4);
    if (offset == 0 || !has(table, layout.features + offset, 4)) return false;
    const usize start = layout.features + offset;
    const u32 lookups = read_u16(table, start + 2);
    if (!has(table, start + 4, static_cast<usize>(lookups) * 2)) return false;
    feature.tag = read_u32(table, record);
    feature.lookups = start + 4;
    feature.lookup_count = lookups;
    return true;
}

u32 feature_lookup(const LayoutTable& layout, const LayoutFeature& feature, const u32 index) noexcept {
    const u32 result = index < feature.lookup_count ? read_u16(layout.table, feature.lookups + static_cast<usize>(index) * 2) : 0xFFFFU;
    return result;
}

u32 lookup_count(const LayoutTable& layout) noexcept {
    const Span& table = layout.table;
    if (layout.lookups == 0 || !has(table, layout.lookups, 2)) return 0;
    const u32 count = read_u16(table, layout.lookups);
    const u32 result = has(table, layout.lookups + 2, static_cast<usize>(count) * 2) ? count : 0;
    return result;
}

namespace {

// Where an extension subtable's own subtable is, and its kind: a format
// of 1, the kind, and a 32-bit offset from the extension.
[[nodiscard]] bool look_through(const Span& table, const usize extension, usize& subtable, u32& kind) noexcept {
    if (!has(table, extension, 8) || read_u16(table, extension) != 1) return false;
    const usize offset = read_u32(table, extension + 4);
    if (offset == 0 || offset > table.size - extension) return false;
    kind = read_u16(table, extension + 2);
    subtable = extension + offset;
    return true;
}

} // namespace

bool lookup_at(const LayoutTable& layout, const u32 index, LayoutLookup& lookup) noexcept {
    lookup = LayoutLookup{};
    const Span& table = layout.table;
    if (index >= lookup_count(layout)) return false;
    const usize offset = read_u16(table, layout.lookups + 2 + static_cast<usize>(index) * 2);
    if (offset == 0 || !has(table, layout.lookups + offset, 6)) return false;
    // A lookup: its kind, its flags, the offsets of its subtables from
    // itself, and the mark glyph set if the flags say it has one.
    const usize start = layout.lookups + offset;
    const u32 subtables = read_u16(table, start + 4);
    if (!has(table, start + 6, static_cast<usize>(subtables) * 2)) return false;
    lookup.place = start;
    lookup.kind = read_u16(table, start);
    lookup.flags = read_u16(table, start + 2);
    lookup.subtable_count = subtables;
    if ((lookup.flags & lookup_use_mark_set) != 0) {
        const usize after = start + 6 + static_cast<usize>(subtables) * 2;
        if (!has(table, after, 2)) return false;
        lookup.mark_set = read_u16(table, after);
    }
    // An extension's kind is that of what its first subtable holds.
    const u32 extension = layout.substitution ? substitution_extension : position_extension;
    if (lookup.kind == extension) {
        lookup.kind = 0;
        if (subtables > 0) {
            const usize first = read_u16(table, start + 6);
            usize inner = 0;
            u32 kind = 0;
            if (first != 0 && look_through(table, start + first, inner, kind) && kind != extension) lookup.kind = kind;
        }
    }
    return true;
}

bool subtable_at(const LayoutTable& layout, const LayoutLookup& lookup, const u32 index, usize& subtable) noexcept {
    subtable = 0;
    const Span& table = layout.table;
    if (index >= lookup.subtable_count || lookup.place == 0) return false;
    const usize offset = read_u16(table, lookup.place + 6 + static_cast<usize>(index) * 2);
    if (offset == 0 || !has(table, lookup.place + offset, 2)) return false;
    usize place = lookup.place + offset;
    const u32 extension = layout.substitution ? substitution_extension : position_extension;
    if (read_u16(table, lookup.place) == extension) {
        u32 kind = 0;
        usize inner = 0;
        if (!look_through(table, place, inner, kind) || kind != lookup.kind || !has(table, inner, 2)) return false;
        place = inner;
    }
    subtable = place;
    return true;
}

} // namespace stxlib::glyphs::detail
