// stxlib::glyphs: the plan of a shaping (shape_plan.hpp). After harfrust
// 0.16.0, src/ot/map.rs, src/planner.rs and src/tag.rs.

#include "algorithms/glyphs/shape_plan.hpp"

#include "algorithms/glyphs/font_reader.hpp"
#include "algorithms/glyphs/shape_unicode.hpp"

namespace stxlib::glyphs::detail {

namespace {

inline constexpr u32 flag_global = 0x0001;
inline constexpr u32 flag_has_fallback = 0x0002;
inline constexpr u32 flag_manual_zwnj = 0x0004;
inline constexpr u32 flag_manual_zwj = 0x0008;
inline constexpr u32 flag_global_search = 0x0010;
inline constexpr u32 flag_random = 0x0020;

inline constexpr u32 global_bit_shift = 31;
inline constexpr u32 global_bit_mask = 1U << global_bit_shift;

// A feature as it was asked for, before the font is consulted.
struct Wanted final {
    u32 tag{0};
    u32 sequence{0};
    u32 most_value{0};
    u32 flags{0};
    u32 default_value{0};
    u32 stage[2]{0, 0};
};

struct Collecting final {
    Vector<Wanted> wanted{};
    u32 stage[2]{0, 0};
    bool memory{true};
};

void add_feature(Collecting& collecting, const u32 tag, const u32 flags, const u32 value) noexcept {
    if (tag == 0) return;
    Wanted wanted{};
    wanted.tag = tag;
    wanted.sequence = static_cast<u32>(collecting.wanted.count());
    wanted.most_value = value;
    wanted.flags = flags;
    wanted.default_value = (flags & flag_global) != 0 ? value : 0;
    wanted.stage[0] = collecting.stage[0];
    wanted.stage[1] = collecting.stage[1];
    if (collecting.wanted.push_copy(wanted).failed()) collecting.memory = false;
}

void enable_feature(Collecting& collecting, const u32 tag, const u32 flags, const u32 value) noexcept {
    add_feature(collecting, tag, flags | flag_global, value);
}

// The OpenType script tags to look for, most wanted first: the tags of the
// later shaping models of the scripts that have them, then the script's
// own tag in small letters, with the handful of exceptions.
usize script_tags(const u32 script, u32 tags[3]) noexcept {
    usize count = 0;
    if (script == 0) return count;
    u32 later = 0;
    if (script == layout_tag('B', 'e', 'n', 'g')) later = layout_tag('b', 'n', 'g', '2');
    if (script == layout_tag('D', 'e', 'v', 'a')) later = layout_tag('d', 'e', 'v', '2');
    if (script == layout_tag('G', 'u', 'j', 'r')) later = layout_tag('g', 'j', 'r', '2');
    if (script == layout_tag('G', 'u', 'r', 'u')) later = layout_tag('g', 'u', 'r', '2');
    if (script == layout_tag('K', 'n', 'd', 'a')) later = layout_tag('k', 'n', 'd', '2');
    if (script == layout_tag('M', 'l', 'y', 'm')) later = layout_tag('m', 'l', 'm', '2');
    if (script == layout_tag('O', 'r', 'y', 'a')) later = layout_tag('o', 'r', 'y', '2');
    if (script == layout_tag('T', 'a', 'm', 'l')) later = layout_tag('t', 'm', 'l', '2');
    if (script == layout_tag('T', 'e', 'l', 'u')) later = layout_tag('t', 'e', 'l', '2');
    if (script == layout_tag('M', 'y', 'm', 'r')) later = layout_tag('m', 'y', 'm', '2');
    if (later != 0) {
        // There is no third model for Myanmar.
        if (later != layout_tag('m', 'y', 'm', '2')) {
            tags[count] = (later & 0xFFFFFF00U) | static_cast<u32>('3');
            count = count + 1;
        }
        tags[count] = later;
        count = count + 1;
    }
    u32 own = script | 0x20000000U;
    if (script == layout_tag('Z', 'm', 't', 'h')) own = layout_tag('m', 'a', 't', 'h');
    if (script == layout_tag('H', 'i', 'r', 'a')) own = layout_tag('k', 'a', 'n', 'a');
    if (script == layout_tag('L', 'a', 'o', 'o')) own = layout_tag('l', 'a', 'o', ' ');
    if (script == layout_tag('Y', 'i', 'i', 'i')) own = layout_tag('y', 'i', ' ', ' ');
    if (script == layout_tag('N', 'k', 'o', 'o')) own = layout_tag('n', 'k', 'o', ' ');
    if (script == layout_tag('V', 'a', 'i', 'i')) own = layout_tag('v', 'a', 'i', ' ');
    if (count < 3) {
        tags[count] = own;
        count = count + 1;
    }
    return count;
}

// A table's script and language system for the plan.
struct Chosen final {
    bool has_script{false};
    usize script{0};
    LanguageSystem system{};
};

void choose_script(const LayoutTable& layout, const u32 tags[3], const usize tag_count, const u32 language, const usize table, ShapePlan& plan,
                   Chosen& chosen) noexcept {
    chosen = Chosen{};
    if (layout.table.size == 0) return;
    for (usize index = 0; index < tag_count && !chosen.has_script; ++index) {
        if (find_script(layout, tags[index], chosen.script)) {
            chosen.has_script = true;
            plan.found_script[table] = true;
            plan.chosen_script[table] = tags[index];
        }
    }
    // No script of the text's: the default script, or Latin, which many
    // fonts put everything under.
    const u32 fallbacks[3] = {layout_tag('D', 'F', 'L', 'T'), layout_tag('d', 'f', 'l', 't'), layout_tag('l', 'a', 't', 'n')};
    for (usize index = 0; index < 3 && !chosen.has_script; ++index) {
        if (find_script(layout, fallbacks[index], chosen.script)) {
            chosen.has_script = true;
            plan.chosen_script[table] = fallbacks[index];
        }
    }
    if (!chosen.has_script) return;
    // The language's system, else one named 'dflt', else the script's
    // default one.
    const u32 default_language = layout_tag('d', 'f', 'l', 't');
    u32 wanted = 0;
    if (has_language(layout, chosen.script, language)) {
        wanted = language;
    } else if (has_language(layout, chosen.script, default_language)) {
        wanted = default_language;
    }
    chosen.system = language_system(layout, chosen.script, wanted);
}

// The features asked for, in order of tag, each tag once: a later asking
// changes an earlier one.
void merge_wanted(Vector<Wanted>& wanted) noexcept {
    const usize count = wanted.count();
    if (count == 0) return;
    // In order of tag, and of asking for one tag (an insertion sort: there
    // are a few dozen at most).
    for (usize index = 1; index < count; ++index) {
        const Wanted moving = wanted[index];
        usize place = index;
        while (place > 0 && (wanted[place - 1].tag > moving.tag || (wanted[place - 1].tag == moving.tag && wanted[place - 1].sequence > moving.sequence))) {
            wanted[place] = wanted[place - 1];
            place = place - 1;
        }
        wanted[place] = moving;
    }
    usize kept = 0;
    for (usize index = 1; index < count; ++index) {
        if (wanted[index].tag != wanted[kept].tag) {
            kept = kept + 1;
            wanted[kept] = wanted[index];
            continue;
        }
        if ((wanted[index].flags & flag_global) != 0) {
            wanted[kept].flags = wanted[kept].flags | flag_global;
            wanted[kept].most_value = wanted[index].most_value;
            wanted[kept].default_value = wanted[index].default_value;
        } else {
            if ((wanted[kept].flags & flag_global) != 0) wanted[kept].flags = wanted[kept].flags ^ flag_global;
            if (wanted[index].most_value > wanted[kept].most_value) wanted[kept].most_value = wanted[index].most_value;
        }
        wanted[kept].flags = wanted[kept].flags | (wanted[index].flags & flag_has_fallback);
        if (wanted[index].stage[0] < wanted[kept].stage[0]) wanted[kept].stage[0] = wanted[index].stage[0];
        if (wanted[index].stage[1] < wanted[kept].stage[1]) wanted[kept].stage[1] = wanted[index].stage[1];
    }
    wanted.truncate(kept + 1);
}

[[nodiscard]] usize find_wanted(const Vector<Wanted>& wanted, const u32 tag) noexcept {
    usize low = 0;
    usize high = wanted.count();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (wanted[middle].tag < tag) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const usize result = (low < wanted.count() && wanted[low].tag == tag) ? low : wanted.count();
    return result;
}

[[nodiscard]] u32 bit_width(const u32 value) noexcept {
    u32 bits = 0;
    for (u32 left = value; left != 0; left = left >> 1) bits = bits + 1;
    return bits;
}

// The first feature of the table with the tag, whatever script it is under.
[[nodiscard]] u32 feature_anywhere(const LayoutTable& layout, const u32 tag) noexcept {
    const u32 count = feature_count(layout);
    u32 found = no_feature;
    for (u32 index = 0; index < count && found == no_feature; ++index) {
        LayoutFeature feature{};
        if (feature_at(layout, index, feature) && feature.tag == tag) found = index;
    }
    return found;
}

// A coverage's glyphs into a digest; the entries read are taken from the
// budget, and when it is spent the digest is of everything.
void digest_coverage(const Span& table, const usize coverage, usize& budget, GlyphDigest& digest) noexcept {
    if (coverage == 0 || !has(table, coverage, 4)) return;
    const u32 format = read_u16(table, coverage);
    const usize count = read_u16(table, coverage + 2);
    if (count > budget) {
        budget = 0;
        digest_fill(digest);
        return;
    }
    budget = budget - count;
    if (format == 1 && has(table, coverage + 4, count * 2)) {
        for (usize index = 0; index < count; ++index) digest_add(digest, read_u16(table, coverage + 4 + index * 2));
    } else if (format == 2 && has(table, coverage + 4, count * 6)) {
        for (usize index = 0; index < count; ++index) {
            const u32 first = read_u16(table, coverage + 4 + index * 6);
            const u32 last = read_u16(table, coverage + 4 + index * 6 + 2);
            if (first <= last) digest_add_range(digest, first, last);
        }
    }
}

// The lookups of one stage as they are gathered: a lookup named by several
// features is applied once, for the glyphs of any of them.
struct Gathering final {
    Vector<PlanLookup>* lookups{nullptr};
    // For each lookup of the font, where it is in `lookups` plus one (0:
    // not in this stage).
    Vector<u32> placed{};
    usize stage_start{0};
    bool memory{true};
};

void gather_feature(const LayoutTable& layout, Gathering& gathering, const u32 feature_index, const u32 mask, const bool auto_zwnj, const bool auto_zwj,
                    const bool random) noexcept {
    LayoutFeature feature{};
    if (!feature_at(layout, feature_index, feature)) return;
    Vector<PlanLookup>& lookups = *gathering.lookups;
    for (u32 index = 0; index < feature.lookup_count; ++index) {
        const u32 number = feature_lookup(layout, feature, index);
        if (number >= gathering.placed.count()) continue;
        const u32 place = gathering.placed[number];
        if (place != 0 && place - 1 >= gathering.stage_start) {
            PlanLookup& first = lookups[place - 1];
            first.mask = first.mask | mask;
            first.auto_zwnj = first.auto_zwnj && auto_zwnj;
            first.auto_zwj = first.auto_zwj && auto_zwj;
            first.random = first.random && random;
            continue;
        }
        PlanLookup lookup{};
        lookup.index = number;
        lookup.mask = mask;
        lookup.auto_zwnj = auto_zwnj;
        lookup.auto_zwj = auto_zwj;
        lookup.random = random;
        if (lookups.push_copy(lookup).failed()) {
            gathering.memory = false;
            return;
        }
        gathering.placed[number] = static_cast<u32>(lookups.count());
    }
}

// A stage's lookups in the order of their numbers.
void finish_stage(Gathering& gathering) noexcept {
    Vector<PlanLookup>& lookups = *gathering.lookups;
    for (usize index = gathering.stage_start + 1; index < lookups.count(); ++index) {
        const PlanLookup moving = lookups[index];
        usize place = index;
        while (place > gathering.stage_start && lookups[place - 1].index > moving.index) {
            lookups[place] = lookups[place - 1];
            place = place - 1;
        }
        lookups[place] = moving;
    }
    gathering.stage_start = lookups.count();
}

// The old kerning table has a part that moves glyphs across the line.
[[nodiscard]] bool kerning_crosses(const Font& font) noexcept {
    const Span table = table_of(font, font.kerning);
    if (!has(table, 0, 4) || read_u16(table, 0) != 0) return false;
    const usize count = read_u16(table, 2);
    usize place = 4;
    bool crosses = false;
    for (usize index = 0; index < count && !crosses && has(table, place, 6); ++index) {
        const usize length = read_u16(table, place + 2);
        crosses = (read_u16(table, place + 4) & 0x0004U) != 0;
        if (length < 6) break;
        place = place + length;
    }
    return crosses;
}

} // namespace

void describe_subtable(const LayoutTable& layout, const u32 kind, const usize place, PlanSubtable& subtable) noexcept {
    const Span& table = layout.table;
    subtable = PlanSubtable{};
    subtable.place = place;
    subtable.format = peek_u16(table, place);
    // A contextual subtable of format 3 begins at the first of its own
    // coverages; every other at the coverage its second field names.
    const u32 context = layout.substitution ? 5U : 7U;
    const u32 chain = layout.substitution ? 6U : 8U;
    usize offset = 0;
    if (kind == context && subtable.format == 3) {
        offset = peek_u16(table, place + 2) != 0 ? peek_u16(table, place + 6) : 0;
    } else if (kind == chain && subtable.format == 3) {
        const usize behind = peek_u16(table, place + 2);
        const usize inputs = peek_u16(table, place + 4 + behind * 2);
        offset = inputs != 0 ? peek_u16(table, place + 6 + behind * 2) : 0;
    } else {
        offset = peek_u16(table, place + 2);
    }
    subtable.coverage = offset != 0 ? place + offset : 0;
}

u32 plan_mask(const ShapePlan& plan, const u32 tag, u32& shift) noexcept {
    shift = 0;
    u32 mask = 0;
    for (usize index = 0; index < plan.features.count(); ++index) {
        if (plan.features[index].tag == tag) {
            mask = plan.features[index].mask;
            shift = plan.features[index].shift;
        }
    }
    return mask;
}

u32 plan_one_mask(const ShapePlan& plan, const u32 tag) noexcept {
    u32 mask = 0;
    for (usize index = 0; index < plan.features.count(); ++index) {
        if (plan.features[index].tag == tag) mask = plan.features[index].one_mask;
    }
    return mask;
}

usize bytes_of(const ShapePlan& plan) noexcept {
    usize bytes = sizeof(ShapePlan) + plan.features.capacity() * sizeof(PlanFeature) + plan.ranged.capacity() * sizeof(PlanRequest);
    for (usize table = 0; table < 2; ++table) {
        bytes = bytes + plan.lookups[table].capacity() * sizeof(PlanLookup) + plan.subtables[table].capacity() * sizeof(PlanSubtable) +
                plan.stages[table].capacity() * sizeof(u32);
    }
    return bytes;
}

Status compile_plan(const Font& font, const ShapeDirection direction, const u32 script, const u32 language, const View<PlanRequest> requests,
                    ShapePlan& plan) noexcept {
    const Operation operation("stxlib.glyphs.shape_plan");
    plan = ShapePlan{};
    plan.direction = direction;
    plan.script = script;
    plan.language = language;
    plan.shaper = ShaperKind::plain;
    if (script == layout_tag('H', 'e', 'b', 'r')) plan.shaper = ShaperKind::hebrew;
    if (script == layout_tag('H', 'a', 'n', 'g')) plan.shaper = ShaperKind::hangul;
    plan.tables[0] = open_layout(font, true);
    plan.tables[1] = open_layout(font, false);
    plan.definitions = open_definitions(font);

    u32 tags[3] = {0, 0, 0};
    const usize tag_count = script_tags(script, tags);
    Chosen chosen[2] = {};
    for (usize table = 0; table < 2; ++table) choose_script(plan.tables[table], tags, tag_count, language, table, plan, chosen[table]);

    // The features every run has, then the caller's.
    Collecting collecting{};
    enable_feature(collecting, layout_tag('r', 'v', 'r', 'n'), 0, 1);
    collecting.stage[0] = collecting.stage[0] + 1;
    if (direction == ShapeDirection::left_to_right) {
        enable_feature(collecting, layout_tag('l', 't', 'r', 'a'), 0, 1);
        enable_feature(collecting, layout_tag('l', 't', 'r', 'm'), 0, 1);
    } else if (direction == ShapeDirection::right_to_left) {
        enable_feature(collecting, layout_tag('r', 't', 'l', 'a'), 0, 1);
        add_feature(collecting, layout_tag('r', 't', 'l', 'm'), 0, 1);
    }
    add_feature(collecting, layout_tag('f', 'r', 'a', 'c'), 0, 1);
    add_feature(collecting, layout_tag('n', 'u', 'm', 'r'), 0, 1);
    add_feature(collecting, layout_tag('d', 'n', 'o', 'm'), 0, 1);
    enable_feature(collecting, layout_tag('r', 'a', 'n', 'd'), flag_random, plan_most_value);
    enable_feature(collecting, layout_tag('t', 'r', 'a', 'k'), flag_has_fallback, 1);
    enable_feature(collecting, layout_tag('H', 'a', 'r', 'f'), 0, 1);
    enable_feature(collecting, layout_tag('H', 'A', 'R', 'F'), 0, 1);
    if (plan.shaper == ShaperKind::hangul) {
        // The forms of the letters of a syllable written out.
        add_feature(collecting, layout_tag('l', 'j', 'm', 'o'), 0, 1);
        add_feature(collecting, layout_tag('v', 'j', 'm', 'o'), 0, 1);
        add_feature(collecting, layout_tag('t', 'j', 'm', 'o'), 0, 1);
    }
    enable_feature(collecting, layout_tag('B', 'u', 'z', 'z'), 0, 1);
    enable_feature(collecting, layout_tag('B', 'U', 'Z', 'Z'), 0, 1);
    add_feature(collecting, layout_tag('a', 'b', 'v', 'm'), flag_global, 1);
    add_feature(collecting, layout_tag('b', 'l', 'w', 'm'), flag_global, 1);
    add_feature(collecting, layout_tag('c', 'c', 'm', 'p'), flag_global, 1);
    add_feature(collecting, layout_tag('l', 'o', 'c', 'l'), flag_global, 1);
    add_feature(collecting, layout_tag('m', 'a', 'r', 'k'), flag_global | flag_manual_zwnj | flag_manual_zwj, 1);
    add_feature(collecting, layout_tag('m', 'k', 'm', 'k'), flag_global | flag_manual_zwnj | flag_manual_zwj, 1);
    add_feature(collecting, layout_tag('r', 'l', 'i', 'g'), flag_global, 1);
    const bool horizontal = direction == ShapeDirection::left_to_right || direction == ShapeDirection::right_to_left;
    if (horizontal) {
        add_feature(collecting, layout_tag('c', 'a', 'l', 't'), flag_global, 1);
        add_feature(collecting, layout_tag('c', 'l', 'i', 'g'), flag_global, 1);
        add_feature(collecting, layout_tag('c', 'u', 'r', 's'), flag_global, 1);
        add_feature(collecting, layout_tag('d', 'i', 's', 't'), flag_global, 1);
        add_feature(collecting, layout_tag('k', 'e', 'r', 'n'), flag_global | flag_has_fallback, 1);
        add_feature(collecting, layout_tag('l', 'i', 'g', 'a'), flag_global, 1);
        add_feature(collecting, layout_tag('r', 'c', 'l', 't'), flag_global, 1);
    } else {
        enable_feature(collecting, layout_tag('v', 'e', 'r', 't'), flag_global_search, 1);
    }
    for (usize index = 0; index < requests.count(); ++index) {
        const PlanRequest& request = requests[index];
        const bool everywhere = request.start == 0 && request.end == feature_everywhere;
        add_feature(collecting, request.tag, everywhere ? flag_global : 0, request.value);
        if (!everywhere && plan.ranged.push_copy(request).failed()) collecting.memory = false;
    }
    // Hangul: contextual alternates are not for the letters of a syllable,
    // so the feature gets bits of its own to be taken off them.
    if (plan.shaper == ShaperKind::hangul) add_feature(collecting, layout_tag('c', 'a', 'l', 't'), 0, 1);
    if (!collecting.memory) return refuse_font(ErrorKind::out_of_memory, operation);

    // The feature every use of the language system has.
    u32 required_index[2] = {no_feature, no_feature};
    u32 required_tag[2] = {0, 0};
    u32 required_stage[2] = {0, 0};
    for (usize table = 0; table < 2; ++table) {
        LayoutFeature feature{};
        if (chosen[table].system.found && chosen[table].system.required_feature != no_feature &&
            feature_at(plan.tables[table], chosen[table].system.required_feature, feature)) {
            required_index[table] = chosen[table].system.required_feature;
            required_tag[table] = feature.tag;
        }
    }

    Vector<Wanted>& wanted = collecting.wanted;
    merge_wanted(wanted);
    // Each wanted feature's number in each table: the first of the
    // language system's features with its tag.
    Vector<u32> numbers{};
    if (numbers.resize(wanted.count() * 2).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    for (usize index = 0; index < numbers.count(); ++index) numbers[index] = no_feature;
    for (usize table = 0; table < 2; ++table) {
        const LanguageSystem& system = chosen[table].system;
        if (!system.found) continue;
        for (u32 index = 0; index < system.feature_count; ++index) {
            const u32 number = language_feature(plan.tables[table], system, index);
            LayoutFeature feature{};
            if (!feature_at(plan.tables[table], number, feature)) continue;
            const usize place = find_wanted(wanted, feature.tag);
            if (place < wanted.count() && numbers[place * 2 + table] == no_feature) numbers[place * 2 + table] = number;
        }
    }

    // Bits of the mask for each feature the font has (or that has a
    // fallback): none for a feature that is simply on everywhere.
    plan.global_mask = global_bit_mask;
    u32 next_bit = 4;
    for (usize index = 0; index < wanted.count(); ++index) {
        const Wanted& info = wanted[index];
        const bool simply_on = (info.flags & flag_global) != 0 && info.most_value == 1;
        const u32 width = bit_width(info.most_value);
        const u32 bits_needed = simply_on ? 0 : (width < plan_value_bits ? width : plan_value_bits);
        if (info.most_value == 0 || next_bit + bits_needed >= global_bit_shift) continue;
        bool found = false;
        PlanFeature feature{};
        for (usize table = 0; table < 2; ++table) {
            if (required_tag[table] != 0 && required_tag[table] == info.tag) required_stage[table] = info.stage[table];
            if (numbers[index * 2 + table] != no_feature) {
                feature.index[table] = numbers[index * 2 + table];
                found = true;
            }
        }
        if (!found && (info.flags & flag_global_search) != 0) {
            for (usize table = 0; table < 2; ++table) {
                const u32 anywhere = feature_anywhere(plan.tables[table], info.tag);
                if (anywhere != no_feature) {
                    feature.index[table] = anywhere;
                    found = true;
                }
            }
        }
        if (!found && (info.flags & flag_has_fallback) == 0) continue;
        if (simply_on) {
            feature.shift = global_bit_shift;
            feature.mask = global_bit_mask;
        } else {
            feature.shift = next_bit;
            feature.mask = (1U << (next_bit + bits_needed)) - (1U << next_bit);
            next_bit = next_bit + bits_needed;
            plan.global_mask = plan.global_mask | ((info.default_value << feature.shift) & feature.mask);
        }
        feature.tag = info.tag;
        feature.stage[0] = info.stage[0];
        feature.stage[1] = info.stage[1];
        feature.one_mask = (1U << feature.shift) & feature.mask;
        feature.needs_fallback = !found;
        feature.auto_zwnj = (info.flags & flag_manual_zwnj) == 0;
        feature.auto_zwj = (info.flags & flag_manual_zwj) == 0;
        feature.random = (info.flags & flag_random) != 0;
        if (plan.features.push_copy(feature).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    }

    // The lookups, a stage at a time, and what each begins at.
    const u32 stage_count[2] = {collecting.stage[0] + 1, collecting.stage[1] + 1};
    usize budget = plan_digest_budget;
    for (usize table = 0; table < 2; ++table) {
        const LayoutTable& layout = plan.tables[table];
        Gathering gathering{};
        gathering.lookups = &plan.lookups[table];
        if (gathering.placed.resize(lookup_count(layout)).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        for (usize index = 0; index < gathering.placed.count(); ++index) gathering.placed[index] = 0;
        for (u32 stage = 0; stage < stage_count[table]; ++stage) {
            if (required_index[table] != no_feature && required_stage[table] == stage) {
                gather_feature(layout, gathering, required_index[table], global_bit_mask, true, true, false);
            }
            for (usize index = 0; index < plan.features.count(); ++index) {
                const PlanFeature& feature = plan.features[index];
                if (feature.index[table] != no_feature && feature.stage[table] == stage) {
                    gather_feature(layout, gathering, feature.index[table], feature.mask, feature.auto_zwnj, feature.auto_zwj, feature.random);
                }
            }
            finish_stage(gathering);
            if (!gathering.memory || plan.stages[table].push_copy(static_cast<u32>(plan.lookups[table].count())).failed()) {
                return refuse_font(ErrorKind::out_of_memory, operation);
            }
        }
        // What each lookup is and begins at.
        for (usize index = 0; index < plan.lookups[table].count(); ++index) {
            PlanLookup& planned = plan.lookups[table][index];
            LayoutLookup lookup{};
            planned.first_subtable = static_cast<u32>(plan.subtables[table].count());
            if (!lookup_at(layout, planned.index, lookup) || lookup.kind == 0) continue;
            planned.kind = lookup.kind;
            planned.props = props_of(lookup);
            for (u32 number = 0; number < lookup.subtable_count; ++number) {
                usize place = 0;
                if (!subtable_at(layout, lookup, number, place)) continue;
                PlanSubtable subtable{};
                describe_subtable(layout, lookup.kind, place, subtable);
                if (subtable.coverage == 0) continue;
                if (budget == 0) {
                    digest_fill(subtable.digest);
                } else {
                    digest_coverage(layout.table, subtable.coverage, budget, subtable.digest);
                }
                digest_join(planned.digest, subtable.digest);
                if (plan.subtables[table].push_copy(subtable).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
            }
            planned.subtable_count = static_cast<u32>(plan.subtables[table].count()) - planned.first_subtable;
        }
    }

    // What the plan does besides the font's lookups.
    const bool hebrew = plan.shaper == ShaperKind::hebrew;
    plan.frac_mask = plan_one_mask(plan, layout_tag('f', 'r', 'a', 'c'));
    plan.numr_mask = plan_one_mask(plan, layout_tag('n', 'u', 'm', 'r'));
    plan.dnom_mask = plan_one_mask(plan, layout_tag('d', 'n', 'o', 'm'));
    plan.has_frac = plan.frac_mask != 0 || (plan.numr_mask != 0 && plan.dnom_mask != 0);
    plan.rtlm_mask = plan_one_mask(plan, layout_tag('r', 't', 'l', 'm'));
    u32 kern_shift = 0;
    const u32 kern_tag = horizontal ? layout_tag('k', 'e', 'r', 'n') : layout_tag('v', 'k', 'r', 'n');
    plan.kern_mask = plan_mask(plan, kern_tag, kern_shift);
    plan.requested_kerning = plan.kern_mask != 0;
    bool has_gpos_kern = false;
    for (usize index = 0; index < plan.features.count(); ++index) {
        if (plan.features[index].tag == kern_tag && plan.features[index].index[1] != no_feature) has_gpos_kern = true;
    }
    // Hebrew's positions are used only when the font has them for Hebrew.
    const bool disable_gpos = hebrew && plan.chosen_script[1] != layout_tag('h', 'e', 'b', 'r');
    plan.fallback_glyph_classes = plan.definitions.classes == 0;
    const bool has_gpos = !disable_gpos && plan.tables[1].table.size != 0;
    plan.apply_gpos = has_gpos;
    const bool has_kerning = table_of(font, font.kerning).size != 0;
    if ((!has_gpos_kern || !plan.apply_gpos) && has_kerning) plan.apply_kern = true;
    plan.zero_marks = plan.shaper != ShaperKind::hangul;
    plan.has_gpos_mark = plan_one_mask(plan, layout_tag('m', 'a', 'r', 'k')) != 0;
    plan.adjust_mark_positioning_when_zeroing = !plan.apply_gpos && (!plan.apply_kern || !kerning_crosses(font));
    plan.fallback_mark_positioning = plan.adjust_mark_positioning_when_zeroing;
    return Status::success();
}

} // namespace stxlib::glyphs::detail
