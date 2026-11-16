// stxlib::glyphs: applying a font's lookups to a run (shape_apply.hpp).
// After harfrust 0.16.0, src/ot/apply.rs, src/ot/layout.rs, src/ot/lookup/
// and src/ot/gpos/.

#include "algorithms/glyphs/shape_apply.hpp"

#include "algorithms/glyphs/font_reader.hpp"

namespace stxlib::glyphs::detail {

namespace {

// What a lookup passes over when it matches its own glyphs, and when it
// matches what stands before and after them.
struct Matcher final {
    u32 mask{0};
    bool ignore_zwnj{false};
    bool ignore_zwj{false};
    bool ignore_hidden{false};
};

// What a glyph of a sequence is matched by: the `data`th number at
// `values`, which is a glyph, a class of the class definition at `base`,
// or the offset from `base` of a coverage.
enum class MatchBy : u8 {
    nothing = 0,
    always = 1,
    glyph = 2,
    glyph_class = 3,
    coverage = 4,
};

struct MatchRule final {
    MatchBy by{MatchBy::nothing};
    usize values{0};
    usize base{0};
};

// A contextual subtable that matched: the lookups it names are still to
// be applied.
struct Pending final {
    bool active{false};
    usize records{0};
    u32 record_count{0};
    usize input_count{0};
    usize match_end{0};
};

struct Applying final {
    const ShapePlan* plan{nullptr};
    ShapeBuffer* buffer{nullptr};
    const LayoutTable* layout{nullptr};
    const GlyphDefinitions* definitions{nullptr};
    ApplyWork* work{nullptr};
    GlyphDigest* digest{nullptr};
    ShapeScale scale{};
    bool substitution{true};
    bool has_classes{false};
    u32 lookup_mask{1};
    u32 lookup_props{0};
    bool auto_zwnj{true};
    bool auto_zwj{true};
    bool random{false};
    u32 random_state{1};
    // The base the last mark was attached to, and how far that holds.
    i64 last_base{-1};
    usize last_base_until{0};
    Matcher matcher{};
    Matcher context_matcher{};
    // The places a sequence matched at, the first being the item read.
    u32 positions[apply_most_context]{};
    usize position_count{0};
    bool nested{false};
};

enum class Skip : u8 { no = 0, yes = 1, maybe = 2 };
enum class Matched : u8 { yes = 0, no = 1, skip = 2 };

[[nodiscard]] Vector<ShapeGlyph>& written(ShapeBuffer& buffer) noexcept {
    Vector<ShapeGlyph>& result = buffer.have_output ? buffer.out : buffer.info;
    return result;
}

void fail(ShapeBuffer& buffer) noexcept { buffer.successful = false; }

// Whether there is an item being read.
[[nodiscard]] bool has_current(ShapeBuffer& buffer) noexcept {
    const bool result = buffer.idx < buffer.len && buffer.idx < buffer.info.count();
    if (!result) fail(buffer);
    return result;
}

[[nodiscard]] u32 glyph_props_of(const GlyphDefinitions& definitions, Vector<MemoEntry>& memo, const u32 glyph) noexcept {
    const u64 key = memo_key(memo_properties, 2, 0, glyph);
    u32 props = 0;
    if (memo_find(memo, key, props)) return props;
    const u32 glyph_class_number = glyph_class(definitions, glyph);
    if (glyph_class_number == glyph_class_base) props = glyph_is_base;
    if (glyph_class_number == glyph_class_ligature) props = glyph_is_ligature;
    if (glyph_class_number == glyph_class_mark) props = glyph_is_mark | (mark_attachment_class(definitions, glyph) << 8);
    memo_keep(memo, key, props);
    return props;
}

// A glyph's place in a coverage and its class in a class definition of the
// table at work, remembered.
[[nodiscard]] u32 covered(const Applying& applying, const usize coverage, const u32 glyph) noexcept {
    Vector<MemoEntry>& memo = (*applying.work).memo;
    const u64 key = memo_key(memo_coverage, applying.substitution ? 0U : 1U, coverage, glyph);
    u32 index = 0;
    if (memo_find(memo, key, index)) return index;
    index = coverage_index((*applying.layout).table, coverage, glyph);
    memo_keep(memo, key, index);
    return index;
}

[[nodiscard]] u32 classed(const Applying& applying, const usize class_definition, const u32 glyph) noexcept {
    Vector<MemoEntry>& memo = (*applying.work).memo;
    const u64 key = memo_key(memo_class, applying.substitution ? 0U : 1U, class_definition, glyph);
    u32 number = 0;
    if (memo_find(memo, key, number)) return number;
    number = class_of((*applying.layout).table, class_definition, glyph);
    memo_keep(memo, key, number);
    return number;
}

// Whether a lookup with these props sees the item at all.
[[nodiscard]] bool check_property(const Applying& applying, const ShapeGlyph& item, const u32 props) noexcept {
    const u32 glyph_props = item.glyph_props;
    if ((glyph_props & props & glyph_class_bits) != 0) return false;
    if ((glyph_props & glyph_is_mark) != 0) {
        if ((props & lookup_use_mark_set) != 0) {
            const bool in_set = in_mark_set(*applying.definitions, props >> 16, item.glyph);
            return in_set;
        }
        if ((props & lookup_mark_class_mask) != 0) {
            const bool same = (props & lookup_mark_class_mask) == (glyph_props & lookup_mark_class_mask);
            return same;
        }
    }
    return true;
}

void update_matchers(Applying& applying) noexcept {
    const bool positions = !applying.substitution;
    applying.matcher = Matcher{.mask = applying.lookup_mask, .ignore_zwnj = positions, .ignore_zwj = applying.auto_zwj, .ignore_hidden = positions};
    applying.context_matcher =
        Matcher{.mask = 0xFFFFFFFFU, .ignore_zwnj = positions || applying.auto_zwnj, .ignore_zwj = true, .ignore_hidden = positions};
}

[[nodiscard]] Skip may_skip(const Applying& applying, const Matcher& matcher, const ShapeGlyph& item, const u32 props) noexcept {
    if (!check_property(applying, item, props)) return Skip::yes;
    if (info_is_default_ignorable(item) && (matcher.ignore_zwnj || !info_is_zwnj(item)) && (matcher.ignore_zwj || !info_is_zwj(item)) &&
        (matcher.ignore_hidden || !info_is_hidden(item))) {
        return Skip::maybe;
    }
    return Skip::no;
}

[[nodiscard]] bool rule_matches(const Applying& applying, const MatchRule& rule, const ShapeGlyph& item, const u32 data) noexcept {
    const Span& table = (*applying.layout).table;
    const usize place = rule.values + static_cast<usize>(data) * 2;
    bool result = true;
    if (rule.by == MatchBy::glyph) {
        result = has(table, place, 2) && read_u16(table, place) == item.glyph;
    } else if (rule.by == MatchBy::glyph_class) {
        result = has(table, place, 2) && classed(applying, rule.base, item.glyph) == read_u16(table, place);
    } else if (rule.by == MatchBy::coverage) {
        const usize offset = peek_u16(table, place);
        result = offset != 0 && covered(applying, rule.base + offset, item.glyph) != not_covered;
    }
    return result;
}

[[nodiscard]] Matched match_item(const Applying& applying, const Matcher& matcher, const u32 props, const MatchRule& rule, const ShapeGlyph& item,
                                 const u32 data) noexcept {
    const Skip skip = may_skip(applying, matcher, item, props);
    if (skip == Skip::yes) return Matched::skip;
    // Whether it may be the glyph wanted: yes, no, or (with no rule) maybe.
    Skip matches = Skip::maybe;
    if ((item.mask & matcher.mask) == 0) {
        matches = Skip::no;
    } else if (rule.by != MatchBy::nothing) {
        matches = rule_matches(applying, rule, item, data) ? Skip::yes : Skip::no;
    }
    if (matches == Skip::yes || (matches == Skip::maybe && skip == Skip::no)) return Matched::yes;
    if (skip == Skip::no) return Matched::no;
    return Matched::skip;
}

// Going over the items a lookup sees, forward among those still to read
// or backward among those written.
struct Skipper final {
    usize index{0};
    usize length{0};
    u32 props{0};
    u32 data{0};
    const Matcher* matcher{nullptr};
    MatchRule rule{};
};

[[nodiscard]] Skipper skipper_of(const Applying& applying, const bool context, const MatchRule& rule, const usize start) noexcept {
    Skipper skipper{};
    skipper.index = start;
    skipper.length = (*applying.buffer).len;
    skipper.props = applying.lookup_props;
    skipper.matcher = context ? &applying.context_matcher : &applying.matcher;
    skipper.rule = rule;
    return skipper;
}

[[nodiscard]] bool skipper_next(const Applying& applying, Skipper& skipper, usize& unsafe_to) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    const usize stop = skipper.length == 0 ? 0 : skipper.length - 1;
    while (skipper.index < stop) {
        skipper.index = skipper.index + 1;
        Matched matched = Matched::no;
        if (skipper.index < buffer.info.count()) {
            matched = match_item(applying, *skipper.matcher, skipper.props, skipper.rule, buffer.info[skipper.index], skipper.data);
        }
        if (matched == Matched::yes) {
            skipper.data = skipper.data + 1;
            return true;
        }
        if (matched == Matched::no) {
            unsafe_to = skipper.index + 1;
            return false;
        }
        static_cast<void>(buffer.charge(1));
    }
    unsafe_to = skipper.index + 1;
    return false;
}

[[nodiscard]] bool skipper_prev(const Applying& applying, Skipper& skipper, usize& unsafe_from) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    Vector<ShapeGlyph>& items = written(buffer);
    while (skipper.index > 0) {
        skipper.index = skipper.index - 1;
        Matched matched = Matched::no;
        if (skipper.index < items.count()) {
            matched = match_item(applying, *skipper.matcher, skipper.props, skipper.rule, items[skipper.index], skipper.data);
        }
        if (matched == Matched::yes) {
            skipper.data = skipper.data + 1;
            return true;
        }
        if (matched == Matched::no) {
            unsafe_from = skipper.index > 1 ? skipper.index - 1 : 0;
            return false;
        }
        static_cast<void>(buffer.charge(1));
    }
    unsafe_from = 0;
    return false;
}

// The item read and `input_count` more, each matched by the rule. The
// places go to applying.positions; `end` is one past the last (or, when
// there is no match, how far the looking went).
[[nodiscard]] bool match_input(Applying& applying, const usize input_count, const MatchRule& rule, usize& end, u32* const total_parts) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (!has_current(buffer)) return false;
    const usize count = input_count + 1;
    const ShapeGlyph first = buffer.info[buffer.idx];
    if (count == 1) {
        end = buffer.idx + 1;
        applying.position_count = 1;
        applying.positions[0] = static_cast<u32>(buffer.idx);
        if (total_parts != nullptr) *total_parts = info_lig_num_comps(first);
        return true;
    }
    if (count > apply_most_context) return false;
    applying.position_count = count;
    Skipper skipper = skipper_of(applying, false, rule, buffer.idx);
    const u32 first_lig_id = info_lig_id(first);
    const u32 first_lig_comp = info_lig_comp(first);
    u32 parts = 0;
    // Whether the ligature the first item is a mark of may be passed over:
    // 0 not looked at yet, 1 it may not, 2 it may.
    u32 ligature_base = 0;
    for (usize index = 1; index < count; ++index) {
        usize unsafe_to = 0;
        if (!skipper_next(applying, skipper, unsafe_to)) {
            end = unsafe_to;
            return false;
        }
        applying.positions[index] = static_cast<u32>(skipper.index);
        const ShapeGlyph item = buffer.info[skipper.index];
        const u32 lig_id = info_lig_id(item);
        const u32 lig_comp = info_lig_comp(item);
        if (first_lig_id != 0 && first_lig_comp != 0) {
            // The first is a mark on a part of a ligature: the others are
            // marks on the same part, unless the ligature itself is
            // something this lookup passes over.
            if (first_lig_id != lig_id || first_lig_comp != lig_comp) {
                if (ligature_base == 0) {
                    Vector<ShapeGlyph>& out = written(buffer);
                    usize back = buffer.out_len < out.count() ? buffer.out_len : out.count();
                    bool found = false;
                    while (back > 0 && info_lig_id(out[back - 1]) == first_lig_id && !found) {
                        back = back - 1;
                        found = info_lig_comp(out[back]) == 0;
                    }
                    static_cast<void>(buffer.charge(buffer.out_len - back));
                    const bool passed = found && may_skip(applying, *skipper.matcher, out[back], skipper.props) == Skip::yes;
                    ligature_base = passed ? 2 : 1;
                }
                if (ligature_base == 1) return false;
            }
        } else if (lig_id != 0 && lig_comp != 0 && lig_id != first_lig_id) {
            // The first is no such mark: none of the others may be one of
            // another ligature.
            return false;
        }
        parts = parts + info_lig_num_comps_in_ligation(item);
    }
    end = skipper.index + 1;
    if (total_parts != nullptr) *total_parts = parts + info_lig_num_comps(first);
    applying.positions[0] = static_cast<u32>(buffer.idx);
    return true;
}

[[nodiscard]] bool match_backtrack(Applying& applying, const usize count, const MatchRule& rule, usize& start) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (count == 0) {
        start = buffer.backtrack_len();
        return true;
    }
    Skipper skipper = skipper_of(applying, true, rule, buffer.backtrack_len());
    for (usize index = 0; index < count; ++index) {
        usize unsafe_from = 0;
        if (!skipper_prev(applying, skipper, unsafe_from)) {
            start = unsafe_from;
            return false;
        }
    }
    start = skipper.index;
    return true;
}

[[nodiscard]] bool match_lookahead(Applying& applying, const usize count, const MatchRule& rule, const usize start, usize& end) noexcept {
    if (count == 0) {
        end = start;
        return true;
    }
    if (start == 0) return false;
    Skipper skipper = skipper_of(applying, true, rule, start - 1);
    for (usize index = 0; index < count; ++index) {
        usize unsafe_to = 0;
        if (!skipper_next(applying, skipper, unsafe_to)) {
            end = unsafe_to;
            return false;
        }
    }
    end = skipper.index + 1;
    return true;
}

// ---- writing glyphs ------------------------------------------------------------------------------------

void set_glyph_class(Applying& applying, const u32 glyph, const u32 class_guess, const bool ligature, const bool component) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    digest_add(*applying.digest, glyph);
    note_glyph(*applying.work, glyph);
    if (!has_current(buffer)) return;
    ShapeGlyph& item = buffer.info[buffer.idx];
    u32 props = item.glyph_props | glyph_substituted;
    if (ligature) {
        props = props | glyph_ligated;
        // A ligature of what was multiplied is no longer multiplied.
        props = props & ~glyph_multiplied;
    }
    if (component) props = props | glyph_multiplied;
    if (applying.has_classes) {
        props = (props & glyph_preserved_bits) | glyph_props_of(*applying.definitions, (*applying.work).memo, glyph);
    } else if (class_guess != 0) {
        props = (props & glyph_preserved_bits) | class_guess;
    }
    item.glyph_props = static_cast<u16>(props);
}

void replace_glyph(Applying& applying, const u32 glyph) noexcept {
    set_glyph_class(applying, glyph, 0, false, false);
    (*applying.buffer).replace_glyph(glyph);
}

void replace_glyph_in_place(Applying& applying, const u32 glyph) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    set_glyph_class(applying, glyph, 0, false, false);
    if (has_current(buffer)) buffer.info[buffer.idx].glyph = glyph;
}

void output_for_component(Applying& applying, const u32 glyph, const u32 class_guess) noexcept {
    set_glyph_class(applying, glyph, class_guess, false, true);
    (*applying.buffer).output_glyph(glyph);
}

// The glyphs matched (applying.positions) become one ligature; the marks
// among them stay, each remembering which part it was on.
void ligate_input(Applying& applying, const usize count, const usize match_end, const u32 total_parts, const u32 ligature_glyph) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (!has_current(buffer)) return;
    for (usize index = 0; index < count; ++index) {
        if (applying.positions[index] >= buffer.len) {
            fail(buffer);
            return;
        }
    }
    const ShapeGlyph first = buffer.info[applying.positions[0]];
    // A base with marks is still a base, and marks together a mark: only
    // the rest are ligatures whose marks need to know their part.
    bool base_ligature = info_is_base(first);
    bool mark_ligature = info_is_mark(first);
    for (usize index = 1; index < count; ++index) {
        if (!info_is_mark(buffer.info[applying.positions[index]])) {
            base_ligature = false;
            mark_ligature = false;
        }
    }
    buffer.merge_clusters(buffer.idx, match_end);
    const bool is_ligature = !base_ligature && !mark_ligature;
    const u32 class_guess = is_ligature ? glyph_is_ligature : 0;
    const u32 lig_id = is_ligature ? buffer.allocate_ligature_id() : 0;
    ShapeGlyph& current = buffer.info[buffer.idx];
    u32 last_lig_id = info_lig_id(current);
    u32 last_parts = info_lig_num_comps(current);
    u32 parts_so_far = last_parts;
    if (is_ligature) {
        info_set_lig_for_ligature(current, lig_id, total_parts);
        if (info_category(current) == category_non_spacing_mark) info_set_category(current, category_other_letter);
    }
    set_glyph_class(applying, ligature_glyph, class_guess, true, false);
    buffer.replace_glyph(ligature_glyph);
    for (usize index = 1; index < count; ++index) {
        while (buffer.idx < applying.positions[index] && buffer.successful) {
            if (is_ligature && buffer.idx < buffer.len) {
                ShapeGlyph& item = buffer.info[buffer.idx];
                u32 part = info_lig_comp(item);
                if (part == 0) part = last_parts;
                const u32 new_part = parts_so_far - last_parts + (part < last_parts ? part : last_parts);
                info_set_lig_for_mark(item, lig_id, new_part);
            }
            buffer.next_glyph();
        }
        if (!buffer.successful || !has_current(buffer)) return;
        const ShapeGlyph& taken = buffer.info[buffer.idx];
        last_lig_id = info_lig_id(taken);
        last_parts = info_lig_num_comps_in_ligation(taken);
        parts_so_far = parts_so_far + last_parts;
        // The part is dropped: the ligature stands for it.
        buffer.idx = buffer.idx + 1;
    }
    if (!mark_ligature && last_lig_id != 0) {
        // The marks after the last part that were marks of it.
        usize walked = 0;
        bool going = true;
        for (usize index = buffer.idx; index < buffer.len && going; ++index) {
            ShapeGlyph& item = buffer.info[index];
            if (last_lig_id != info_lig_id(item)) {
                going = false;
            } else {
                walked = walked + 1;
                const u32 part = info_lig_comp(item);
                if (part == 0) {
                    going = false;
                } else {
                    const u32 new_part = parts_so_far - last_parts + (part < last_parts ? part : last_parts);
                    info_set_lig_for_mark(item, lig_id, new_part);
                }
            }
        }
        static_cast<void>(buffer.charge(walked));
    }
}

// ---- substitutions ---------------------------------------------------------------------------------------

[[nodiscard]] bool apply_single(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    const u32 glyph = buffer.info[buffer.idx].glyph;
    if (subtable.format == 1) {
        // The glyph's number moved by a fixed amount, within 16 bits.
        const i32 delta = peek_i16(table, subtable.place + 4);
        replace_glyph(applying, (glyph + static_cast<u32>(delta + 0x10000)) & 0xFFFFU);
        return true;
    }
    if (subtable.format == 2) {
        if (index >= peek_u16(table, subtable.place + 4) || !has(table, subtable.place + 6 + static_cast<usize>(index) * 2, 2)) return false;
        replace_glyph(applying, read_u16(table, subtable.place + 6 + static_cast<usize>(index) * 2));
        return true;
    }
    return false;
}

[[nodiscard]] bool apply_multiple(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (subtable.format != 1 || index >= peek_u16(table, subtable.place + 4)) return false;
    const usize offset = peek_u16(table, subtable.place + 6 + static_cast<usize>(index) * 2);
    if (offset == 0 || !has(table, subtable.place + offset, 2)) return false;
    const usize sequence = subtable.place + offset;
    const usize count = read_u16(table, sequence);
    if (!has(table, sequence + 2, count * 2)) return false;
    if (count == 0) {
        buffer.delete_glyph();
        return true;
    }
    if (count == 1) {
        replace_glyph(applying, read_u16(table, sequence + 2));
        return true;
    }
    // One glyph becomes several: each a part of it, and a ligature so
    // parted a base.
    const u32 class_guess = info_is_ligature(buffer.info[buffer.idx]) ? glyph_is_base : 0;
    const u32 lig_id = info_lig_id(buffer.info[buffer.idx]);
    for (usize part = 0; part < count && buffer.successful; ++part) {
        if (lig_id == 0) info_set_lig_for_mark(buffer.info[buffer.idx], 0, static_cast<u32>(part));
        output_for_component(applying, read_u16(table, sequence + 2 + part * 2), class_guess);
    }
    buffer.skip_glyph();
    return true;
}

[[nodiscard]] u32 next_random(Applying& applying) noexcept {
    // The generator HarfBuzz uses: the product kept to 32 bits, then the
    // remainder by 2^31 - 1.
    const u64 product = (static_cast<u64>(applying.random_state) * 48271U) & 0xFFFFFFFFU;
    applying.random_state = static_cast<u32>(product % 2147483647U);
    return applying.random_state;
}

[[nodiscard]] bool apply_alternate(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (subtable.format != 1 || index >= peek_u16(table, subtable.place + 4)) return false;
    const usize offset = peek_u16(table, subtable.place + 6 + static_cast<usize>(index) * 2);
    if (offset == 0 || !has(table, subtable.place + offset, 2)) return false;
    const usize set = subtable.place + offset;
    const u32 count = read_u16(table, set);
    if (count == 0 || !has(table, set + 2, static_cast<usize>(count) * 2)) return false;
    // Which alternate: the feature's value for this glyph, in the mask.
    const u32 lookup_mask = applying.lookup_mask;
    u32 shift = 0;
    while (shift < 32 && ((lookup_mask >> shift) & 1U) == 0) shift = shift + 1;
    u32 alternate = shift < 32 ? ((lookup_mask & buffer.info[buffer.idx].mask) >> shift) : 0;
    if (alternate == plan_most_value && applying.random) {
        // Any of them, and the run cannot be broken and come out the same.
        buffer.unsafe_to_break(0, buffer.len);
        alternate = next_random(applying) % count + 1;
    }
    if (alternate == 0 || alternate > count) return false;
    replace_glyph(applying, read_u16(table, set + 2 + static_cast<usize>(alternate - 1) * 2));
    return true;
}

// One ligature of a set: its glyph, and the glyphs after the first that
// make it.
[[nodiscard]] bool apply_ligature(Applying& applying, const usize ligature) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (!buffer.successful || !has(table, ligature, 4)) return false;
    const u32 glyph = read_u16(table, ligature);
    const usize parts = read_u16(table, ligature + 2);
    if (parts <= 1) {
        replace_glyph(applying, glyph);
        return true;
    }
    const MatchRule rule{.by = MatchBy::glyph, .values = ligature + 4, .base = 0};
    usize match_end = 0;
    u32 total_parts = 0;
    if (!match_input(applying, parts - 1, rule, match_end, &total_parts)) {
        buffer.unsafe_to_concat(buffer.idx, match_end);
        return false;
    }
    ligate_input(applying, parts, match_end, total_parts, glyph);
    return true;
}

[[nodiscard]] bool apply_ligature_set(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (subtable.format != 1 || index >= peek_u16(table, subtable.place + 4)) return false;
    const usize offset = peek_u16(table, subtable.place + 6 + static_cast<usize>(index) * 2);
    if (offset == 0 || !has(table, subtable.place + offset, 2)) return false;
    const usize set = subtable.place + offset;
    const usize count = read_u16(table, set);
    if (!has(table, set + 2, count * 2)) return false;
    // The glyph after this one, when it is plainly the second of any
    // ligature that could match: a ligature with another second glyph is
    // passed over without matching.
    bool second_known = false;
    u32 second = 0;
    usize unsafe_to = 0;
    if (count > 1) {
        const MatchRule always{.by = MatchBy::always, .values = 0, .base = 0};
        Skipper skipper = skipper_of(applying, true, always, buffer.idx);
        if (skipper_next(applying, skipper, unsafe_to)) {
            const ShapeGlyph& item = buffer.info[skipper.index];
            unsafe_to = skipper.index + 1;
            if (may_skip(applying, *skipper.matcher, item, skipper.props) == Skip::no) {
                second_known = true;
                second = item.glyph;
            }
        }
    }
    // No ligature of the subtable has this second glyph; or this set is
    // remembered to have none that begins with it.
    if (second_known && subtable.seconds_known && !digest_may_have(subtable.seconds, second)) {
        buffer.unsafe_to_concat(buffer.idx, unsafe_to);
        return false;
    }
    Vector<MemoEntry>& memo = (*applying.work).memo;
    const u64 key = memo_key(memo_ligature, 0, set, second);
    u32 none_begin = 0;
    if (second_known && memo_find(memo, key, none_begin) && none_begin == 1) {
        buffer.unsafe_to_concat(buffer.idx, unsafe_to);
        return false;
    }
    bool applied = false;
    bool passed_over = false;
    bool tried = false;
    for (usize number = 0; number < count && !applied; ++number) {
        const usize ligature_offset = read_u16(table, set + 2 + number * 2);
        if (ligature_offset == 0) continue;
        const usize ligature = set + ligature_offset;
        if (second_known && peek_u16(table, ligature + 2) > 1 && peek_u16(table, ligature + 4) != second) {
            passed_over = true;
            continue;
        }
        tried = true;
        applied = apply_ligature(applying, ligature);
    }
    if (second_known && !tried) memo_keep(memo, key, 1);
    if (passed_over) buffer.unsafe_to_concat(buffer.idx, unsafe_to);
    return applied;
}

[[nodiscard]] bool apply_reverse_chain(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    // Never inside another lookup.
    if (subtable.format != 1 || applying.nested) return false;
    const usize behind = peek_u16(table, subtable.place + 4);
    const usize ahead_place = subtable.place + 6 + behind * 2;
    const usize ahead = peek_u16(table, ahead_place);
    const usize substitutes = ahead_place + 2 + ahead * 2;
    if (index >= peek_u16(table, substitutes) || !has(table, substitutes + 2 + static_cast<usize>(index) * 2, 2)) return false;
    const u32 substitute = read_u16(table, substitutes + 2 + static_cast<usize>(index) * 2);
    const MatchRule behind_rule{.by = MatchBy::coverage, .values = subtable.place + 6, .base = subtable.place};
    const MatchRule ahead_rule{.by = MatchBy::coverage, .values = ahead_place + 2, .base = subtable.place};
    usize start = 0;
    usize end = 0;
    if (match_backtrack(applying, behind, behind_rule, start) && match_lookahead(applying, ahead, ahead_rule, buffer.idx + 1, end)) {
        buffer.unsafe_to_break_from_output(start, end);
        replace_glyph_in_place(applying, substitute);
        return true;
    }
    buffer.unsafe_to_concat_from_output(start, end);
    return false;
}

// ---- contexts -------------------------------------------------------------------------------------------

// One rule: the glyphs behind, the glyphs of the sequence after its first,
// the glyphs ahead, and the lookups to apply to the sequence.
struct Rule final {
    bool chained{false};
    usize behind_count{0};
    usize behind{0};
    usize input_count{0};
    usize input{0};
    usize ahead_count{0};
    usize ahead{0};
    u32 record_count{0};
    usize records{0};
};

// A rule of formats 1 and 2 at `place`.
[[nodiscard]] Rule read_rule(const Span& table, const usize place, const bool chained) noexcept {
    Rule rule{};
    rule.chained = chained;
    if (!chained) {
        const usize glyphs = peek_u16(table, place);
        rule.input_count = glyphs > 0 ? glyphs - 1 : 0;
        rule.record_count = peek_u16(table, place + 2);
        rule.input = place + 4;
        rule.records = rule.input + rule.input_count * 2;
        return rule;
    }
    rule.behind_count = peek_u16(table, place);
    rule.behind = place + 2;
    const usize input_place = rule.behind + rule.behind_count * 2;
    const usize glyphs = peek_u16(table, input_place);
    rule.input_count = glyphs > 0 ? glyphs - 1 : 0;
    rule.input = input_place + 2;
    const usize ahead_place = rule.input + rule.input_count * 2;
    rule.ahead_count = peek_u16(table, ahead_place);
    rule.ahead = ahead_place + 2;
    const usize record_place = rule.ahead + rule.ahead_count * 2;
    rule.record_count = peek_u16(table, record_place);
    rule.records = record_place + 2;
    return rule;
}

// The rule against the run: on a match the lookups it names are left
// pending.
[[nodiscard]] bool apply_rule(Applying& applying, const Rule& rule, const MatchBy by, const usize behind_base, const usize input_base,
                              const usize ahead_base, Pending& pending) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (!buffer.successful) return false;
    const MatchRule input_rule{.by = by, .values = rule.input, .base = input_base};
    usize end = buffer.idx;
    usize match_end = 0;
    if (!match_input(applying, rule.input_count, input_rule, match_end, nullptr)) {
        if (rule.chained) buffer.unsafe_to_concat(buffer.idx, end);
        return false;
    }
    if (!rule.chained) {
        buffer.unsafe_to_break(buffer.idx, match_end);
    } else {
        end = match_end;
        const MatchRule ahead_rule{.by = by, .values = rule.ahead, .base = ahead_base};
        if (!match_lookahead(applying, rule.ahead_count, ahead_rule, match_end, end)) {
            buffer.unsafe_to_concat(buffer.idx, end);
            return false;
        }
        usize start = buffer.out_len;
        const MatchRule behind_rule{.by = by, .values = rule.behind, .base = behind_base};
        if (!match_backtrack(applying, rule.behind_count, behind_rule, start)) {
            buffer.unsafe_to_concat_from_output(start, end);
            return false;
        }
        buffer.unsafe_to_break_from_output(start, end);
    }
    pending.active = true;
    pending.records = rule.records;
    pending.record_count = rule.record_count;
    pending.input_count = rule.input_count;
    pending.match_end = match_end;
    return true;
}

[[nodiscard]] bool apply_context(Applying& applying, const PlanSubtable& subtable, const u32 index, const bool chained, Pending& pending) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    const usize place = subtable.place;
    if (subtable.format == 3) {
        // One rule, each of its glyphs any of a coverage.
        Rule rule{};
        rule.chained = chained;
        if (!chained) {
            const usize glyphs = peek_u16(table, place + 2);
            rule.input_count = glyphs > 0 ? glyphs - 1 : 0;
            rule.record_count = peek_u16(table, place + 4);
            rule.input = place + 8;
            rule.records = place + 6 + glyphs * 2;
        } else {
            rule.behind_count = peek_u16(table, place + 2);
            rule.behind = place + 4;
            const usize input_place = rule.behind + rule.behind_count * 2;
            const usize glyphs = peek_u16(table, input_place);
            rule.input_count = glyphs > 0 ? glyphs - 1 : 0;
            rule.input = input_place + 4;
            const usize ahead_place = input_place + 2 + glyphs * 2;
            rule.ahead_count = peek_u16(table, ahead_place);
            rule.ahead = ahead_place + 2;
            const usize record_place = rule.ahead + rule.ahead_count * 2;
            rule.record_count = peek_u16(table, record_place);
            rule.records = record_place + 2;
        }
        const bool applied = apply_rule(applying, rule, MatchBy::coverage, place, place, place, pending);
        return applied;
    }
    // Formats 1 and 2: sets of rules, by the first glyph or by its class.
    usize set_count = 0;
    usize sets = 0;
    usize number = index;
    MatchBy by = MatchBy::glyph;
    usize behind_base = 0;
    usize input_base = 0;
    usize ahead_base = 0;
    if (subtable.format == 1) {
        set_count = peek_u16(table, place + 4);
        sets = place + 6;
    } else if (subtable.format == 2) {
        by = MatchBy::glyph_class;
        if (!chained) {
            input_base = place + peek_u16(table, place + 4);
            set_count = peek_u16(table, place + 6);
            sets = place + 8;
        } else {
            behind_base = place + peek_u16(table, place + 4);
            input_base = place + peek_u16(table, place + 6);
            ahead_base = place + peek_u16(table, place + 8);
            set_count = peek_u16(table, place + 10);
            sets = place + 12;
        }
        number = classed(applying, input_base, buffer.info[buffer.idx].glyph);
    } else {
        return false;
    }
    if (number >= set_count) return false;
    const usize set_offset = peek_u16(table, sets + number * 2);
    if (set_offset == 0 || !has(table, place + set_offset, 2)) return false;
    const usize set = place + set_offset;
    const usize rule_count = read_u16(table, set);
    bool applied = false;
    for (usize rule_number = 0; rule_number < rule_count && !applied && buffer.successful; ++rule_number) {
        const usize rule_offset = peek_u16(table, set + 2 + rule_number * 2);
        if (rule_offset == 0) continue;
        const Rule rule = read_rule(table, set + rule_offset, chained);
        applied = apply_rule(applying, rule, by, behind_base, input_base, ahead_base, pending);
    }
    return applied;
}

// ---- positions ------------------------------------------------------------------------------------------

inline constexpr u32 value_x_placement = 0x0001;
inline constexpr u32 value_y_placement = 0x0002;
inline constexpr u32 value_x_advance = 0x0004;
inline constexpr u32 value_y_advance = 0x0008;

[[nodiscard]] usize value_size(const u32 format) noexcept {
    // How many of the eight fields the record has: the bits of the format
    // counted two, then four, then eight at a time.
    u32 bits = format & 0xFFU;
    bits = bits - ((bits >> 1) & 0x55U);
    bits = (bits & 0x33U) + ((bits >> 2) & 0x33U);
    bits = (bits + (bits >> 4)) & 0x0FU;
    const usize result = static_cast<usize>(bits) * 2;
    return result;
}

// A record of adjustments at `place`, of `format`, added to item
// `index`'s position. Whether anything but zeros was added.
[[nodiscard]] bool apply_value(Applying& applying, const usize index, const usize place, const u32 format) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (index >= buffer.len || !has(table, place, value_size(format))) return false;
    const bool horizontal = buffer.direction == ShapeDirection::left_to_right || buffer.direction == ShapeDirection::right_to_left;
    ShapePosition& position = buffer.pos[index];
    usize at = place;
    bool worked = false;
    if ((format & value_x_placement) != 0) {
        const i32 value = read_i16(table, at);
        position.x_offset = saturate(static_cast<i64>(position.x_offset) + scale_value(value, applying.scale.x_mult));
        worked = worked || value != 0;
        at = at + 2;
    }
    if ((format & value_y_placement) != 0) {
        const i32 value = read_i16(table, at);
        position.y_offset = saturate(static_cast<i64>(position.y_offset) + scale_value(value, applying.scale.y_mult));
        worked = worked || value != 0;
        at = at + 2;
    }
    if ((format & value_x_advance) != 0) {
        const i32 value = read_i16(table, at);
        if (horizontal) {
            position.x_advance = saturate(static_cast<i64>(position.x_advance) + scale_value(value, applying.scale.x_mult));
            worked = worked || value != 0;
        }
        at = at + 2;
    }
    if ((format & value_y_advance) != 0) {
        const i32 value = read_i16(table, at);
        if (!horizontal) {
            // Advances down the page are counted downward.
            position.y_advance = saturate(static_cast<i64>(position.y_advance) - scale_value(value, applying.scale.y_mult));
            worked = worked || value != 0;
        }
    }
    return worked;
}

[[nodiscard]] bool apply_single_position(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    const u32 format = peek_u16(table, subtable.place + 4);
    if (subtable.format == 1) {
        static_cast<void>(apply_value(applying, buffer.idx, subtable.place + 6, format));
    } else if (subtable.format == 2) {
        if (index >= peek_u16(table, subtable.place + 6)) return false;
        static_cast<void>(apply_value(applying, buffer.idx, subtable.place + 8 + static_cast<usize>(index) * value_size(format), format));
    } else {
        return false;
    }
    buffer.idx = buffer.idx + 1;
    return true;
}

[[nodiscard]] bool apply_pair(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (subtable.format != 1 && subtable.format != 2) return false;
    // The next glyph this lookup sees.
    const MatchRule none{};
    Skipper skipper = skipper_of(applying, false, none, buffer.idx);
    usize unsafe_to = 0;
    if (!skipper_next(applying, skipper, unsafe_to)) {
        buffer.unsafe_to_concat(buffer.idx, unsafe_to);
        return false;
    }
    const usize second_index = skipper.index;
    const u32 second = buffer.info[second_index].glyph;
    const u32 format1 = peek_u16(table, subtable.place + 4);
    const u32 format2 = peek_u16(table, subtable.place + 6);
    const usize size1 = value_size(format1);
    const usize size2 = value_size(format2);
    usize values = 0;
    if (subtable.format == 1) {
        // The first glyph's pairs, in order of the second.
        if (index >= peek_u16(table, subtable.place + 8)) return false;
        const usize set_offset = peek_u16(table, subtable.place + 10 + static_cast<usize>(index) * 2);
        if (set_offset == 0 || !has(table, subtable.place + set_offset, 2)) return false;
        const usize set = subtable.place + set_offset;
        const usize record = 2 + size1 + size2;
        usize low = 0;
        usize high = read_u16(table, set);
        if (!has(table, set + 2, high * record)) return false;
        while (low < high && values == 0) {
            const usize middle = low + (high - low) / 2;
            const u32 listed = read_u16(table, set + 2 + middle * record);
            if (listed < second) {
                low = middle + 1;
            } else if (listed > second) {
                high = middle;
            } else {
                values = set + 2 + middle * record + 2;
            }
        }
    } else {
        // By the classes of the two glyphs.
        const u32 class1 = classed(applying, subtable.place + peek_u16(table, subtable.place + 8), buffer.info[buffer.idx].glyph);
        const u32 class2 = classed(applying, subtable.place + peek_u16(table, subtable.place + 10), second);
        const u32 count1 = peek_u16(table, subtable.place + 12);
        const u32 count2 = peek_u16(table, subtable.place + 14);
        if (class1 < count1 && class2 < count2) {
            values = subtable.place + 16 + (static_cast<usize>(class1) * count2 + class2) * (size1 + size2);
        }
    }
    if (values == 0) {
        buffer.unsafe_to_concat(buffer.idx, second_index + 1);
        return false;
    }
    const bool worked1 = format1 != 0 && apply_value(applying, buffer.idx, values, format1);
    const bool worked2 = format2 != 0 && apply_value(applying, second_index, values + size1, format2);
    if (worked1 || worked2) {
        buffer.unsafe_to_break(buffer.idx, second_index + 1);
    } else {
        buffer.unsafe_to_concat(buffer.idx, second_index + 1);
    }
    // A pair that adjusts its second glyph is done with it; otherwise the
    // second glyph is the first of the next pair.
    usize next = second_index;
    if (format2 != 0) {
        next = next + 1;
        buffer.unsafe_to_break(buffer.idx, next + 1);
    }
    buffer.idx = next;
    return true;
}

struct Anchor final {
    bool found{false};
    i32 x{0};
    i32 y{0};
};

// The place a glyph attaches at, at `offset` from `base` (0: none).
[[nodiscard]] Anchor read_anchor(const Span& table, const usize base, const usize offset) noexcept {
    Anchor anchor{};
    if (offset == 0 || !has(table, base + offset, 6)) return anchor;
    anchor.found = true;
    anchor.x = read_i16(table, base + offset + 2);
    anchor.y = read_i16(table, base + offset + 4);
    return anchor;
}

// An attached glyph's offset across the line, with that of everything it
// is cursively attached to.
[[nodiscard]] i32 cross_offset(const ShapeBuffer& buffer, const usize start, const bool horizontal) noexcept {
    usize index = start;
    i64 offset = horizontal ? buffer.pos[index].y_offset : buffer.pos[index].x_offset;
    bool going = true;
    for (usize step = 1; step < buffer.len && going; ++step) {
        const ShapePosition& position = buffer.pos[index];
        if ((position.attach_type & attached_cursively) == 0 || position.attach_chain == 0) {
            going = false;
        } else {
            const i64 parent = static_cast<i64>(index) + position.attach_chain;
            if (parent < 0 || parent >= static_cast<i64>(buffer.len)) {
                going = false;
            } else {
                index = static_cast<usize>(parent);
                offset = saturate(offset + (horizontal ? buffer.pos[index].y_offset : buffer.pos[index].x_offset));
            }
        }
    }
    const i32 result = saturate(offset);
    return result;
}

// The mark being read is put where its anchor meets the base's.
[[nodiscard]] bool attach_mark(Applying& applying, const Anchor& base_anchor, const Anchor& mark_anchor, const usize base) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (base >= buffer.len || !has_current(buffer)) return false;
    const bool horizontal = buffer.direction == ShapeDirection::left_to_right || buffer.direction == ShapeDirection::right_to_left;
    // As HarfBuzz works it out, in single precision: each anchor scaled,
    // then their difference rounded.
    const float base_x = static_cast<float>(base_anchor.x) * applying.scale.x_multf;
    const float base_y = static_cast<float>(base_anchor.y) * applying.scale.y_multf;
    const float mark_x = static_cast<float>(mark_anchor.x) * applying.scale.x_multf;
    const float mark_y = static_cast<float>(mark_anchor.y) * applying.scale.y_multf;
    const float across = base_x - mark_x;
    const float along = base_y - mark_y;
    const i32 base_offset = cross_offset(buffer, base, horizontal);
    buffer.unsafe_to_break(base, buffer.idx + 1);
    ShapePosition& position = buffer.pos[buffer.idx];
    const i64 chain = static_cast<i64>(base) - static_cast<i64>(buffer.idx);
    if (chain < -32768 || chain > 32767) {
        position.attach_chain = 0;
        buffer.idx = buffer.idx + 1;
        return true;
    }
    position.x_offset = round_real(across);
    position.y_offset = round_real(along);
    if (horizontal) {
        position.y_offset = saturate(static_cast<i64>(position.y_offset) + base_offset);
    } else {
        position.x_offset = saturate(static_cast<i64>(position.x_offset) + base_offset);
    }
    position.attach_type = attached_as_mark;
    position.attach_chain = static_cast<i16>(chain);
    buffer.scratch = buffer.scratch | scratch_has_attachment;
    buffer.idx = buffer.idx + 1;
    return true;
}

// A glyph that was attached to something is attached to `new_parent`
// instead: the old chain is turned round.
void reverse_cursive_minor_offset(ShapeBuffer& buffer, const usize start, const bool horizontal, const usize new_parent) noexcept {
    usize from[apply_most_nesting + 1] = {};
    usize to[apply_most_nesting + 1] = {};
    i16 chains[apply_most_nesting + 1] = {};
    u8 kinds[apply_most_nesting + 1] = {};
    usize links = 0;
    usize index = start;
    bool going = true;
    for (usize step = 0; step <= apply_most_nesting && going; ++step) {
        ShapePosition& position = buffer.pos[index];
        const i16 chain = position.attach_chain;
        const u8 kind = position.attach_type;
        if (chain == 0 || (kind & attached_cursively) == 0) {
            going = false;
        } else {
            position.attach_chain = 0;
            const i64 next = static_cast<i64>(index) + chain;
            if (next < 0 || next >= static_cast<i64>(buffer.len) || static_cast<usize>(next) == new_parent) {
                going = false;
            } else {
                from[links] = index;
                to[links] = static_cast<usize>(next);
                chains[links] = chain;
                kinds[links] = kind;
                links = links + 1;
                index = static_cast<usize>(next);
            }
        }
    }
    for (usize link = links; link > 0; --link) {
        const ShapePosition& old_child = buffer.pos[from[link - 1]];
        const i32 offset = horizontal ? old_child.y_offset : old_child.x_offset;
        ShapePosition& old_parent = buffer.pos[to[link - 1]];
        if (horizontal) {
            old_parent.y_offset = saturate(-static_cast<i64>(offset));
        } else {
            old_parent.x_offset = saturate(-static_cast<i64>(offset));
        }
        old_parent.attach_chain = static_cast<i16>(-chains[link - 1]);
        old_parent.attach_type = kinds[link - 1];
    }
}

[[nodiscard]] bool apply_cursive(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (subtable.format != 1) return false;
    const u32 count = peek_u16(table, subtable.place + 4);
    if (index >= count) return false;
    const Anchor entry = read_anchor(table, subtable.place, peek_u16(table, subtable.place + 6 + static_cast<usize>(index) * 4));
    if (!entry.found) return false;
    const MatchRule none{};
    Skipper skipper = skipper_of(applying, false, none, buffer.idx);
    usize unsafe_from = 0;
    if (!skipper_prev(applying, skipper, unsafe_from)) {
        buffer.unsafe_to_concat_from_output(unsafe_from, buffer.idx + 1);
        return false;
    }
    const usize before = skipper.index;
    const usize after = buffer.idx;
    const u32 previous = covered(applying, subtable.coverage, buffer.info[before].glyph);
    if (previous == not_covered || previous >= count) return false;
    const Anchor exit = read_anchor(table, subtable.place, peek_u16(table, subtable.place + 6 + static_cast<usize>(previous) * 4 + 2));
    if (!exit.found) {
        buffer.unsafe_to_concat_from_output(before, buffer.idx + 1);
        return false;
    }
    const float exit_x = static_cast<float>(exit.x) * applying.scale.x_multf;
    const float exit_y = static_cast<float>(exit.y) * applying.scale.y_multf;
    const float entry_x = static_cast<float>(entry.x) * applying.scale.x_multf;
    const float entry_y = static_cast<float>(entry.y) * applying.scale.y_multf;
    buffer.unsafe_to_break(before, after + 1);
    ShapePosition& first = buffer.pos[before];
    ShapePosition& second = buffer.pos[after];
    // Along the line: the first glyph ends at its exit, the second begins
    // at its entry.
    if (buffer.direction == ShapeDirection::left_to_right) {
        first.x_advance = saturate(static_cast<i64>(round_real(exit_x)) + first.x_offset);
        const i32 distance = saturate(static_cast<i64>(round_real(entry_x)) + second.x_offset);
        second.x_advance = saturate(static_cast<i64>(second.x_advance) - distance);
        second.x_offset = saturate(static_cast<i64>(second.x_offset) - distance);
    } else if (buffer.direction == ShapeDirection::right_to_left) {
        const i32 distance = saturate(static_cast<i64>(round_real(exit_x)) + first.x_offset);
        first.x_advance = saturate(static_cast<i64>(first.x_advance) - distance);
        first.x_offset = saturate(static_cast<i64>(first.x_offset) - distance);
        second.x_advance = saturate(static_cast<i64>(round_real(entry_x)) + second.x_offset);
    } else if (buffer.direction == ShapeDirection::top_to_bottom) {
        first.y_advance = saturate(static_cast<i64>(round_real(exit_y)) + first.y_offset);
        const i32 distance = saturate(static_cast<i64>(round_real(entry_y)) + second.y_offset);
        second.y_advance = saturate(static_cast<i64>(second.y_advance) - distance);
        second.y_offset = saturate(static_cast<i64>(second.y_offset) - distance);
    } else if (buffer.direction == ShapeDirection::bottom_to_top) {
        const i32 distance = saturate(static_cast<i64>(round_real(exit_y)) + first.y_offset);
        first.y_advance = saturate(static_cast<i64>(first.y_advance) - distance);
        first.y_offset = saturate(static_cast<i64>(first.y_offset) - distance);
        second.y_advance = round_real(entry_y);
    }
    // Across the line: one glyph hangs from the other (which one, the
    // lookup says).
    const bool horizontal = buffer.direction == ShapeDirection::left_to_right || buffer.direction == ShapeDirection::right_to_left;
    const float across_real = entry_x - exit_x;
    const float along_real = entry_y - exit_y;
    usize child = before;
    usize parent = after;
    i32 x_offset = round_real(across_real);
    i32 y_offset = round_real(along_real);
    if ((applying.lookup_props & lookup_right_to_left) == 0) {
        child = after;
        parent = before;
        x_offset = saturate(-static_cast<i64>(x_offset));
        y_offset = saturate(-static_cast<i64>(y_offset));
    }
    reverse_cursive_minor_offset(buffer, child, horizontal, parent);
    ShapePosition& hanging = buffer.pos[child];
    hanging.attach_type = attached_cursively;
    const i64 chain = static_cast<i64>(parent) - static_cast<i64>(child);
    hanging.attach_chain = (chain > -32768 && chain <= 32767) ? static_cast<i16>(chain) : static_cast<i16>(0);
    if (horizontal) {
        hanging.y_offset = y_offset;
    } else {
        hanging.x_offset = x_offset;
    }
    buffer.scratch = buffer.scratch | scratch_has_attachment;
    // Two glyphs do not hang from each other.
    ShapePosition& held = buffer.pos[parent];
    if (hanging.attach_chain != 0 && held.attach_chain == -hanging.attach_chain) {
        held.attach_chain = 0;
        if (horizontal) {
            held.y_offset = 0;
        } else {
            held.x_offset = 0;
        }
    }
    buffer.idx = buffer.idx + 1;
    return true;
}

// Looking back from the mark being read for what it sits on: a glyph that
// is no mark, and (for a glyph that was one of several made from one) the
// first of them. Remembered from mark to mark.
[[nodiscard]] bool find_mark_base(Applying& applying, const bool ligature, const usize coverage, usize& base) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (applying.last_base_until > buffer.idx) {
        applying.last_base_until = 0;
        applying.last_base = -1;
    }
    const MatchRule none{};
    Skipper skipper = skipper_of(applying, false, none, buffer.idx);
    skipper.props = lookup_ignore_marks;
    usize back = buffer.idx;
    i64 last_base = applying.last_base;
    bool found = false;
    while (back > applying.last_base_until && !found) {
        const ShapeGlyph& item = buffer.info[back - 1];
        Matched matched = match_item(applying, *skipper.matcher, skipper.props, skipper.rule, item, 0);
        if (matched == Matched::yes) {
            bool accepted = false;
            if (ligature) {
                accepted = !info_multiplied(item) || info_lig_comp(item) == 0;
            } else {
                accepted = !info_multiplied(item) || info_lig_comp(item) == 0 || back - 1 == 0;
                if (!accepted) {
                    const ShapeGlyph& earlier = buffer.info[back - 2];
                    accepted = info_is_mark(earlier) || !info_multiplied(earlier) || info_lig_id(item) != info_lig_id(earlier) ||
                               info_lig_comp(item) != info_lig_comp(earlier) + 1;
                }
            }
            if (!accepted && covered(applying, coverage, item.glyph) == not_covered) matched = Matched::skip;
        }
        if (matched == Matched::yes) {
            last_base = static_cast<i64>(back) - 1;
            found = true;
        } else {
            back = back - 1;
        }
    }
    if (!buffer.charge(buffer.idx - back)) return false;
    applying.last_base_until = buffer.idx;
    applying.last_base = last_base;
    if (last_base < 0) {
        buffer.unsafe_to_concat_from_output(0, buffer.idx + 1);
        return false;
    }
    base = static_cast<usize>(last_base);
    return true;
}

// A mark's class and anchor from the list of marks at `marks`.
[[nodiscard]] bool read_mark(const Span& table, const usize marks, const u32 index, u32& mark_class, Anchor& anchor) noexcept {
    if (index >= peek_u16(table, marks)) return false;
    const usize record = marks + 2 + static_cast<usize>(index) * 4;
    mark_class = peek_u16(table, record);
    anchor = read_anchor(table, marks, peek_u16(table, record + 2));
    return anchor.found;
}

[[nodiscard]] bool apply_mark_to_base(Applying& applying, const PlanSubtable& subtable, const u32 index, const bool ligature) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (subtable.format != 1) return false;
    const usize place = subtable.place;
    const usize base_coverage_offset = peek_u16(table, place + 4);
    if (base_coverage_offset == 0) return false;
    const usize base_coverage = place + base_coverage_offset;
    usize base = 0;
    if (!find_mark_base(applying, ligature, base_coverage, base)) return false;
    const ShapeGlyph base_item = buffer.info[base];
    const u32 base_index = covered(applying, base_coverage, base_item.glyph);
    if (base_index == not_covered) {
        buffer.unsafe_to_concat_from_output(base, buffer.idx + 1);
        return false;
    }
    const u32 class_count = peek_u16(table, place + 6);
    const usize marks = place + peek_u16(table, place + 8);
    const usize bases = place + peek_u16(table, place + 10);
    u32 mark_class = 0;
    Anchor mark_anchor{};
    if (marks == place || bases == place) return false;
    if (!ligature) {
        if (!read_mark(table, marks, index, mark_class, mark_anchor) || mark_class >= class_count) return false;
        if (base_index >= peek_u16(table, bases)) return false;
        const usize cell = bases + 2 + (static_cast<usize>(base_index) * class_count + mark_class) * 2;
        const Anchor base_anchor = read_anchor(table, bases, peek_u16(table, cell));
        if (!base_anchor.found) return false;
        const bool attached = attach_mark(applying, base_anchor, mark_anchor, base);
        return attached;
    }
    // A ligature: a row of anchors for each of its parts. A mark of the
    // ligature goes on its own part, any other on the last.
    if (base_index >= peek_u16(table, bases)) return false;
    const usize attach_offset = peek_u16(table, bases + 2 + static_cast<usize>(base_index) * 2);
    if (attach_offset == 0) return false;
    const usize rows = bases + attach_offset;
    const u32 part_count = peek_u16(table, rows);
    if (part_count == 0) {
        buffer.unsafe_to_concat_from_output(base, buffer.idx + 1);
        return false;
    }
    const u32 lig_id = info_lig_id(base_item);
    const u32 mark_id = info_lig_id(buffer.info[buffer.idx]);
    const u32 mark_part = info_lig_comp(buffer.info[buffer.idx]);
    u32 part = part_count;
    if (lig_id != 0 && lig_id == mark_id && mark_part > 0) part = mark_part < part_count ? mark_part : part_count;
    part = part - 1;
    if (!read_mark(table, marks, index, mark_class, mark_anchor) || mark_class >= class_count) return false;
    const usize cell = rows + 2 + (static_cast<usize>(part) * class_count + mark_class) * 2;
    const Anchor base_anchor = read_anchor(table, rows, peek_u16(table, cell));
    if (!base_anchor.found) return false;
    const bool attached = attach_mark(applying, base_anchor, mark_anchor, base);
    return attached;
}

[[nodiscard]] bool apply_mark_to_mark(Applying& applying, const PlanSubtable& subtable, const u32 index) noexcept {
    const Span& table = (*applying.layout).table;
    ShapeBuffer& buffer = *applying.buffer;
    if (subtable.format != 1) return false;
    const usize place = subtable.place;
    // The glyph before, marks included; it has to be a mark.
    const MatchRule none{};
    Skipper skipper = skipper_of(applying, false, none, buffer.idx);
    skipper.props = applying.lookup_props & ~glyph_class_bits;
    usize unsafe_from = 0;
    if (!skipper_prev(applying, skipper, unsafe_from)) {
        buffer.unsafe_to_concat_from_output(unsafe_from, buffer.idx + 1);
        return false;
    }
    const usize other = skipper.index;
    const ShapeGlyph other_item = buffer.info[other];
    if (!info_is_mark(other_item)) {
        buffer.unsafe_to_concat_from_output(other, buffer.idx + 1);
        return false;
    }
    const ShapeGlyph& current = buffer.info[buffer.idx];
    const u32 id1 = info_lig_id(current);
    const u32 id2 = info_lig_id(other_item);
    const u32 part1 = info_lig_comp(current);
    const u32 part2 = info_lig_comp(other_item);
    // Marks of one base, or of one part of a ligature; or one of them is
    // itself a ligature of marks.
    const bool together = id1 == id2 ? (id1 == 0 || part1 == part2) : ((id1 > 0 && part1 == 0) || (id2 > 0 && part2 == 0));
    if (!together) {
        buffer.unsafe_to_concat_from_output(other, buffer.idx + 1);
        return false;
    }
    const usize other_coverage = peek_u16(table, place + 4);
    if (other_coverage == 0) return false;
    const u32 other_index = covered(applying, place + other_coverage, other_item.glyph);
    if (other_index == not_covered) return false;
    const u32 class_count = peek_u16(table, place + 6);
    const usize marks = place + peek_u16(table, place + 8);
    const usize others = place + peek_u16(table, place + 10);
    if (marks == place || others == place) return false;
    u32 mark_class = 0;
    Anchor mark_anchor{};
    if (!read_mark(table, marks, index, mark_class, mark_anchor) || mark_class >= class_count) return false;
    if (other_index >= peek_u16(table, others)) return false;
    const usize cell = others + 2 + (static_cast<usize>(other_index) * class_count + mark_class) * 2;
    const Anchor other_anchor = read_anchor(table, others, peek_u16(table, cell));
    if (!other_anchor.found) return false;
    const bool attached = attach_mark(applying, other_anchor, mark_anchor, other);
    return attached;
}

// ---- a lookup at a place --------------------------------------------------------------------------------

[[nodiscard]] bool apply_subtable(Applying& applying, const u32 kind, const PlanSubtable& subtable, Pending& pending) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (!has_current(buffer)) return false;
    const u32 glyph = buffer.info[buffer.idx].glyph;
    u32 index = 0;
    const u32 context = applying.substitution ? 5U : 7U;
    const u32 chain = applying.substitution ? 6U : 8U;
    const bool by_class = (kind == context || kind == chain) && subtable.format == 2;
    index = covered(applying, subtable.coverage, glyph);
    if (index == not_covered) return false;
    static_cast<void>(by_class);
    bool applied = false;
    if (kind == context) {
        applied = apply_context(applying, subtable, index, false, pending);
    } else if (kind == chain) {
        applied = apply_context(applying, subtable, index, true, pending);
    } else if (applying.substitution) {
        if (kind == 1) applied = apply_single(applying, subtable, index);
        if (kind == 2) applied = apply_multiple(applying, subtable, index);
        if (kind == 3) applied = apply_alternate(applying, subtable, index);
        if (kind == 4) applied = apply_ligature_set(applying, subtable, index);
        if (kind == 8) applied = apply_reverse_chain(applying, subtable, index);
    } else {
        if (kind == 1) applied = apply_single_position(applying, subtable, index);
        if (kind == 2) applied = apply_pair(applying, subtable, index);
        if (kind == 3) applied = apply_cursive(applying, subtable, index);
        if (kind == 4) applied = apply_mark_to_base(applying, subtable, index, false);
        if (kind == 5) applied = apply_mark_to_base(applying, subtable, index, true);
        if (kind == 6) applied = apply_mark_to_mark(applying, subtable, index);
    }
    return applied;
}

// A lookup of the plan at the item read: the first of its subtables that
// applies.
[[nodiscard]] bool try_planned(Applying& applying, const PlanLookup& lookup, Pending& pending) noexcept {
    const ShapePlan& plan = *applying.plan;
    const Vector<PlanSubtable>& subtables = plan.subtables[applying.substitution ? 0 : 1];
    ShapeBuffer& buffer = *applying.buffer;
    if (!has_current(buffer)) return false;
    const u32 glyph = buffer.info[buffer.idx].glyph;
    bool applied = false;
    for (u32 number = 0; number < lookup.subtable_count && !applied; ++number) {
        const PlanSubtable& subtable = subtables[lookup.first_subtable + number];
        if (!digest_may_have(subtable.digest, glyph)) continue;
        applied = apply_subtable(applying, lookup.kind, subtable, pending);
    }
    return applied;
}

// A lookup named by a contextual one.
[[nodiscard]] bool try_nested(Applying& applying, const LayoutLookup& lookup, Pending& pending) noexcept {
    const LayoutTable& layout = *applying.layout;
    bool applied = false;
    for (u32 number = 0; number < lookup.subtable_count && !applied; ++number) {
        usize place = 0;
        if (!subtable_at(layout, lookup, number, place)) continue;
        PlanSubtable subtable{};
        describe_subtable(layout, lookup.kind, place, subtable);
        if (subtable.coverage == 0) continue;
        applied = apply_subtable(applying, lookup.kind, subtable, pending);
    }
    return applied;
}

// A frame for what a contextual subtable matched: the places become
// places in what is written and what is still to read, taken together.
void begin_frame(Applying& applying, ApplyFrame& frame, const Pending& pending, const u32 props) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    frame = ApplyFrame{};
    frame.lookup_props = props;
    frame.count = static_cast<i64>(pending.input_count) + 1;
    const i64 behind = static_cast<i64>(buffer.backtrack_len());
    const i64 delta = behind - static_cast<i64>(buffer.idx);
    for (i64 index = 0; index < frame.count && index < static_cast<i64>(apply_most_context); ++index) {
        frame.positions[index] = static_cast<u32>(static_cast<i64>(applying.positions[index]) + delta);
    }
    frame.end = behind + static_cast<i64>(pending.match_end) - static_cast<i64>(buffer.idx);
    frame.records = pending.records;
    frame.record_count = pending.record_count;
}

// After a lookup applied inside a frame changed the length of the run: the
// places after it move with it.
void after_nested(Applying& applying, ApplyFrame& frame) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    const i64 index = frame.sequence_index;
    const i64 new_length = static_cast<i64>(buffer.backtrack_len() + buffer.lookahead_len());
    i64 delta = new_length - static_cast<i64>(frame.original_length);
    if (delta == 0) return;
    frame.end = frame.end + delta;
    if (frame.end < static_cast<i64>(frame.positions[index])) {
        // The lookup took away more than the sequence had: what is left
        // ends where it was applied.
        delta = delta + static_cast<i64>(frame.positions[index]) - frame.end;
        frame.end = frame.positions[index];
    }
    i64 next = index + 1;
    if (delta > 0) {
        if (delta + frame.count > static_cast<i64>(apply_most_context)) {
            frame.stopped = true;
            return;
        }
    } else {
        // No more places can go than there are after this one.
        if (delta < next - frame.count) delta = next - frame.count;
        next = next - delta;
    }
    // The places from `next` on move by delta.
    if (delta > 0) {
        for (i64 at = frame.count; at > next; --at) frame.positions[at - 1 + delta] = frame.positions[at - 1];
    } else {
        for (i64 at = next; at < frame.count; ++at) frame.positions[at + delta] = frame.positions[at];
    }
    next = next + delta;
    frame.count = frame.count + delta;
    // New places follow the one the lookup was applied at.
    for (i64 at = index + 1; at < next; ++at) frame.positions[at] = frame.positions[at - 1] + 1;
    for (i64 at = next; at < frame.count; ++at) frame.positions[at] = static_cast<u32>(static_cast<i64>(frame.positions[at]) + delta);
}

// A lookup of the plan at the item read, with whatever lookups it names
// inside what it matched, and those inside what they match.
[[nodiscard]] bool apply_at(Applying& applying, const PlanLookup& lookup) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    const LayoutTable& layout = *applying.layout;
    const Span& table = layout.table;
    Pending pending{};
    applying.nested = false;
    const bool applied = try_planned(applying, lookup, pending);
    if (!applied || !pending.active) return applied;
    Vector<ApplyFrame>& frames = (*applying.work).frames;
    if (frames.count() < apply_most_nesting) {
        fail(buffer);
        return true;
    }
    usize depth = 0;
    begin_frame(applying, frames[0], pending, applying.lookup_props);
    applying.nested = true;
    bool running = true;
    while (running) {
        ApplyFrame& frame = frames[depth];
        bool pushed = false;
        while (frame.next_record < frame.record_count && buffer.successful && !frame.stopped && !pushed) {
            const usize record = frame.records + static_cast<usize>(frame.next_record) * 4;
            frame.next_record = frame.next_record + 1;
            const i64 sequence_index = peek_u16(table, record);
            const u32 nested_number = peek_u16(table, record + 2);
            if (!has(table, record, 4) || sequence_index >= frame.count) {
                static_cast<void>(buffer.charge(1));
                continue;
            }
            const usize original_length = buffer.backtrack_len() + buffer.lookahead_len();
            if (frame.positions[sequence_index] >= original_length) {
                static_cast<void>(buffer.charge(1));
                continue;
            }
            if (!buffer.move_to(frame.positions[sequence_index]) || buffer.max_ops <= 0) {
                frame.stopped = true;
                continue;
            }
            // Deeper than is allowed, or out of operations: the shaping is
            // given up.
            if (depth + 1 >= apply_most_nesting) {
                fail(buffer);
                continue;
            }
            buffer.max_ops = buffer.max_ops - 1;
            if (buffer.max_ops < 0) {
                fail(buffer);
                continue;
            }
            LayoutLookup nested{};
            if (!lookup_at(layout, nested_number, nested) || nested.kind == 0) continue;
            applying.lookup_props = props_of(nested);
            update_matchers(applying);
            Pending inner{};
            const bool inner_applied = try_nested(applying, nested, inner);
            frame.sequence_index = sequence_index;
            frame.original_length = original_length;
            if (inner_applied && inner.active) {
                depth = depth + 1;
                begin_frame(applying, frames[depth], inner, applying.lookup_props);
                pushed = true;
            } else {
                applying.lookup_props = frame.lookup_props;
                update_matchers(applying);
                if (inner_applied) after_nested(applying, frame);
            }
        }
        if (pushed) continue;
        // The frame is done: reading goes on after what it matched.
        static_cast<void>(buffer.move_to(frame.end > 0 ? static_cast<usize>(frame.end) : 0));
        if (depth == 0) {
            running = false;
        } else {
            depth = depth - 1;
            ApplyFrame& parent = frames[depth];
            applying.lookup_props = parent.lookup_props;
            update_matchers(applying);
            after_nested(applying, parent);
        }
    }
    applying.nested = false;
    applying.lookup_props = frames[0].lookup_props;
    update_matchers(applying);
    return true;
}

// Whether any glyph the run has had is one a subtable of the lookup begins
// at: by the coverages themselves, so a lookup passed over here could not
// have applied. With too many different glyphs to list, it is not known.
[[nodiscard]] bool run_may_use(const Applying& applying, const PlanLookup& lookup) noexcept {
    const ApplyWork& work = *applying.work;
    if (work.distinct_over) return true;
    const Vector<PlanSubtable>& subtables = (*applying.plan).subtables[applying.substitution ? 0 : 1];
    // No more coverages are asked than this: past it, the lookup is gone
    // through as if it might apply.
    const usize most_asked = 64;
    usize asked = 0;
    bool found = false;
    for (usize index = 0; index < work.distinct_count && !found; ++index) {
        const u32 glyph = work.distinct[index];
        if (!digest_may_have(lookup.digest, glyph)) continue;
        for (u32 number = 0; number < lookup.subtable_count && !found; ++number) {
            const PlanSubtable& subtable = subtables[lookup.first_subtable + number];
            if (!digest_may_have(subtable.digest, glyph)) continue;
            asked = asked + 1;
            found = asked > most_asked || covered(applying, subtable.coverage, glyph) != not_covered;
        }
    }
    return found;
}

// What a kerning lookup gives a pair of glyphs, in the font's units: the
// first of its subtables that has the first glyph and (by glyph) the pair
// or (by class) both classes decides, as when the lookup is applied to
// the first glyph with the second after it.
[[nodiscard]] i32 kerning_of(const Applying& applying, const PlanLookup& lookup, const u32 first, const u32 second) noexcept {
    const Span& table = (*applying.layout).table;
    const Vector<PlanSubtable>& subtables = (*applying.plan).subtables[1];
    i32 value = 0;
    bool decided = false;
    for (u32 number = 0; number < lookup.subtable_count && !decided; ++number) {
        const PlanSubtable& subtable = subtables[lookup.first_subtable + number];
        if (!digest_may_have(subtable.digest, first)) continue;
        const u32 index = covered(applying, subtable.coverage, first);
        if (index == not_covered) continue;
        const usize record_size = value_size(peek_u16(table, subtable.place + 4));
        if (subtable.format == 1) {
            if (index >= peek_u16(table, subtable.place + 8)) continue;
            const usize set_offset = peek_u16(table, subtable.place + 10 + static_cast<usize>(index) * 2);
            if (set_offset == 0 || !has(table, subtable.place + set_offset, 2)) continue;
            const usize set = subtable.place + set_offset;
            const usize record = 2 + record_size;
            usize low = 0;
            usize high = read_u16(table, set);
            if (!has(table, set + 2, high * record)) continue;
            while (low < high && !decided) {
                const usize middle = low + (high - low) / 2;
                const u32 listed = read_u16(table, set + 2 + middle * record);
                if (listed < second) {
                    low = middle + 1;
                } else if (listed > second) {
                    high = middle;
                } else {
                    value = read_i16(table, set + 2 + middle * record + 2);
                    decided = true;
                }
            }
        } else {
            const u32 class1 = classed(applying, subtable.place + peek_u16(table, subtable.place + 8), first);
            const u32 class2 = classed(applying, subtable.place + peek_u16(table, subtable.place + 10), second);
            const u32 count1 = peek_u16(table, subtable.place + 12);
            const u32 count2 = peek_u16(table, subtable.place + 14);
            if (class1 < count1 && class2 < count2) {
                // A record not all inside the table adjusts nothing.
                const usize place = subtable.place + 16 + (static_cast<usize>(class1) * count2 + class2) * record_size;
                value = has(table, place, record_size) ? read_i16(table, place) : 0;
                decided = true;
            }
        }
    }
    return value;
}

// A kerning lookup over a run all of whose items it sees: each glyph and
// the one after it, the pair's value remembered. No item is looked at for
// whether the lookup applies to it; that was settled for the run.
void kern_run(Applying& applying, const PlanLookup& lookup, const u16 lookup_number) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    Vector<PairEntry>& pairs = (*applying.work).pairs;
    const u64 serial = (*applying.plan).serial;
    const bool remembering = serial != 0 && pairs.count() == pair_memo_size;
    const i64 mult = applying.scale.x_mult;
    for (usize index = 0; index + 1 < buffer.len; ++index) {
        const u32 first = buffer.info[index].glyph;
        const u32 second = buffer.info[index + 1].glyph;
        i32 value = 0;
        const usize slot = ((first * 31U) ^ second ^ (static_cast<u32>(lookup_number) * 7U)) & (pair_memo_size - 1);
        bool known = false;
        if (remembering) {
            const PairEntry& entry = pairs[slot];
            if (entry.serial == serial && entry.lookup == lookup_number && entry.first == first && entry.second == second) {
                value = entry.value;
                known = true;
            }
        }
        if (!known) {
            value = digest_may_have(lookup.digest, first) ? kerning_of(applying, lookup, first, second) : 0;
            if (remembering && first <= 0xFFFFU && second <= 0xFFFFU) {
                pairs[slot] = PairEntry{.serial = serial,
                                        .lookup = lookup_number,
                                        .first = static_cast<u16>(first),
                                        .second = static_cast<u16>(second),
                                        .value = static_cast<i16>(value)};
            }
        }
        if (value != 0) {
            ShapePosition& position = buffer.pos[index];
            position.x_advance = saturate(static_cast<i64>(position.x_advance) + scale_value(value, mult));
            buffer.unsafe_to_break(index, index + 2);
        }
    }
}

// Whether the lookup sees every item of the run, and is on for every one:
// no glyph class it passes over is in the run, no mark it would have to
// ask about, nothing that is not to show.
[[nodiscard]] bool sees_whole_run(const Applying& applying, const PlanLookup& lookup) noexcept {
    const ApplyWork& work = *applying.work;
    const bool marks_asked_about = (lookup.props & (lookup_use_mark_set | lookup_mark_class_mask)) != 0;
    const bool result = (work.run_props & lookup.props & glyph_class_bits) == 0 && ((work.run_props & glyph_is_mark) == 0 || !marks_asked_about) &&
                        !work.run_ignorable && (work.run_mask & lookup.mask) != 0;
    return result;
}

[[nodiscard]] bool may_begin(const Applying& applying, const PlanLookup& lookup, const ShapeGlyph& item) noexcept {
    const bool result = digest_may_have(lookup.digest, item.glyph) && (item.mask & applying.lookup_mask) != 0 && check_property(applying, item, lookup.props);
    return result;
}

// A lookup of the plan over the whole run.
void apply_string(Applying& applying, const PlanLookup& lookup) noexcept {
    ShapeBuffer& buffer = *applying.buffer;
    if (buffer.len == 0 || applying.lookup_mask == 0 || lookup.subtable_count == 0) return;
    // Every lookup gone into is charged for the run's length, as HarfBuzz
    // charges it: a font with very many lookups runs out of operations.
    if (!buffer.charge(buffer.len / 16)) return;
    applying.lookup_props = lookup.props;
    update_matchers(applying);
    const bool backward = applying.substitution && lookup.kind == 8;
    // Nothing in the run that the lookup could begin at: nothing to do, and
    // the run is not gone through a second time to say so.
    usize first = 0;
    while (first < buffer.len && !may_begin(applying, lookup, buffer.info[first])) first = first + 1;
    if (first == buffer.len) return;
    if (!backward) {
        if (applying.substitution) buffer.clear_output();
        buffer.idx = 0;
        while (buffer.successful) {
            // Straight to the next item the lookup could begin at.
            usize next = buffer.idx;
            while (next < buffer.len && !may_begin(applying, lookup, buffer.info[next])) next = next + 1;
            if (next > buffer.idx) buffer.next_glyphs(next - buffer.idx);
            if (buffer.idx >= buffer.len || !buffer.successful) break;
            if (!apply_at(applying, lookup)) buffer.next_glyph();
        }
        if (applying.substitution) static_cast<void>(buffer.sync());
        return;
    }
    // From the end of the run to its beginning, in place.
    buffer.idx = buffer.len - 1;
    bool going = true;
    while (going) {
        usize candidate = buffer.idx + 1;
        while (candidate > 0 && !may_begin(applying, lookup, buffer.info[candidate - 1])) candidate = candidate - 1;
        if (candidate == 0) {
            buffer.idx = 0;
            going = false;
        } else {
            buffer.idx = candidate - 1;
            static_cast<void>(apply_at(applying, lookup));
            if (!buffer.successful || candidate - 1 == 0) {
                going = false;
            } else {
                buffer.idx = candidate - 2;
            }
        }
    }
}

void apply_table(const ShapePlan& plan, ShapeBuffer& buffer, ApplyWork& work, const ShapeScale& scale, GlyphDigest& digest, const bool substitution) noexcept {
    const usize table = substitution ? 0 : 1;
    if (plan.lookups[table].count() == 0 || buffer.len == 0) return;
    if (!prepare_work(work)) {
        buffer.successful = false;
        return;
    }
    Applying applying{};
    applying.plan = &plan;
    applying.buffer = &buffer;
    applying.layout = &plan.tables[table];
    applying.definitions = &plan.definitions;
    applying.work = &work;
    applying.digest = &digest;
    applying.scale = scale;
    applying.substitution = substitution;
    applying.has_classes = plan.definitions.classes != 0;
    const bool concat = (buffer.flags & buffer_produce_unsafe_to_concat) != 0;
    const bool horizontal = buffer.direction == ShapeDirection::left_to_right || buffer.direction == ShapeDirection::right_to_left;
    for (usize index = 0; index < plan.lookups[table].count() && buffer.successful; ++index) {
        const PlanLookup& lookup = plan.lookups[table][index];
        // No glyph of the run is one this lookup begins at: by the digests,
        // then by the coverages.
        if (!digest_may_intersect(lookup.digest, digest)) continue;
        if (!run_may_use(applying, lookup)) {
            static_cast<void>(buffer.charge(buffer.len / 16));
            continue;
        }
        // Plain kerning on a run it sees all of: the fixed loop.
        if (lookup.kerning && horizontal && !concat && index <= 0xFFFFU && sees_whole_run(applying, lookup)) {
            if (buffer.charge(buffer.len / 16)) kern_run(applying, lookup, static_cast<u16>(index));
            continue;
        }
        applying.lookup_mask = lookup.mask;
        applying.last_base = -1;
        applying.last_base_until = 0;
        applying.auto_zwj = lookup.auto_zwj;
        applying.auto_zwnj = lookup.auto_zwnj;
        applying.random = lookup.random;
        apply_string(applying, lookup);
    }
}

} // namespace

bool prepare_work(ApplyWork& work) noexcept {
    const bool result = (work.frames.count() >= apply_most_nesting || work.frames.resize(apply_most_nesting).succeeded()) &&
                        (work.memo.count() == memo_size || work.memo.resize(memo_size).succeeded()) &&
                        (work.pairs.count() == pair_memo_size || work.pairs.resize(pair_memo_size).succeeded());
    return result;
}

void start_substitutions(const ShapePlan& plan, ShapeBuffer& buffer, ApplyWork& work, GlyphDigest& digest) noexcept {
    digest = GlyphDigest{};
    work.run_number = work.run_number + 1;
    work.distinct_count = 0;
    work.distinct_over = false;
    for (usize index = 0; index < buffer.len; ++index) {
        ShapeGlyph& item = buffer.info[index];
        digest_add(digest, item.glyph);
        note_glyph(work, item.glyph);
        item.glyph_props = static_cast<u16>(glyph_props_of(plan.definitions, work.memo, item.glyph));
        item.lig_props = 0;
    }
}

void apply_substitutions(const ShapePlan& plan, ShapeBuffer& buffer, ApplyWork& work, GlyphDigest& digest) noexcept {
    const ShapeScale scale{};
    apply_table(plan, buffer, work, scale, digest, true);
}

void apply_positions(const ShapePlan& plan, ShapeBuffer& buffer, ApplyWork& work, const ShapeScale& scale, GlyphDigest& digest) noexcept {
    apply_table(plan, buffer, work, scale, digest, false);
}

void start_positions(ShapeBuffer& buffer) noexcept {
    for (usize index = 0; index < buffer.len; ++index) {
        buffer.pos[index].attach_chain = 0;
        buffer.pos[index].attach_type = 0;
    }
}

void finish_positions(ShapeBuffer& buffer) noexcept {
    if ((buffer.scratch & scratch_has_attachment) == 0 || buffer.len == 0) return;
    const usize length = buffer.len;
    const bool horizontal = buffer.direction == ShapeDirection::left_to_right || buffer.direction == ShapeDirection::right_to_left;
    const bool forward = buffer.direction == ShapeDirection::left_to_right || buffer.direction == ShapeDirection::top_to_bottom;
    // Each attached glyph, after whatever it is attached to: a chain is
    // followed to its end (no further than the nesting allows) and worked
    // back from there.
    usize chain_item[apply_most_nesting + 1] = {};
    usize chain_parent[apply_most_nesting + 1] = {};
    u8 chain_kind[apply_most_nesting + 1] = {};
    for (usize step = 0; step < length; ++step) {
        const usize start = forward ? step : length - 1 - step;
        if (buffer.pos[start].attach_chain == 0) continue;
        usize links = 0;
        usize index = start;
        bool going = true;
        while (going) {
            ShapePosition& position = buffer.pos[index];
            const i64 parent = static_cast<i64>(index) + position.attach_chain;
            const u8 kind = position.attach_type;
            position.attach_chain = 0;
            if (parent < 0 || parent >= static_cast<i64>(length) || links >= apply_most_nesting) {
                going = false;
            } else {
                chain_item[links] = index;
                chain_parent[links] = static_cast<usize>(parent);
                chain_kind[links] = kind;
                links = links + 1;
                index = static_cast<usize>(parent);
                going = buffer.pos[index].attach_chain != 0;
            }
        }
        for (usize link = links; link > 0; --link) {
            const usize item = chain_item[link - 1];
            const usize parent = chain_parent[link - 1];
            ShapePosition& position = buffer.pos[item];
            const ShapePosition held = buffer.pos[parent];
            if (chain_kind[link - 1] == attached_cursively) {
                if (horizontal) {
                    position.y_offset = saturate(static_cast<i64>(position.y_offset) + held.y_offset);
                } else {
                    position.x_offset = saturate(static_cast<i64>(position.x_offset) + held.x_offset);
                }
            } else if (chain_kind[link - 1] == attached_as_mark) {
                if (horizontal) {
                    position.x_offset = saturate(static_cast<i64>(position.x_offset) + held.x_offset);
                } else {
                    position.y_offset = saturate(static_cast<i64>(position.y_offset) + held.y_offset);
                }
                // The mark is drawn after the advances between it and its
                // base: those are taken back.
                usize from = 0;
                usize to = 0;
                i64 sign = 0;
                if (parent < item) {
                    from = forward ? parent : parent + 1;
                    to = forward ? item : item + 1;
                    sign = forward ? -1 : 1;
                } else {
                    from = forward ? item : item + 1;
                    to = forward ? parent : parent + 1;
                    sign = forward ? 1 : -1;
                }
                i64 x = 0;
                i64 y = 0;
                for (usize between = from; between < to && between < length; ++between) {
                    x = x + buffer.pos[between].x_advance;
                    y = y + buffer.pos[between].y_advance;
                }
                position.x_offset = saturate(static_cast<i64>(position.x_offset) + sign * x);
                position.y_offset = saturate(static_cast<i64>(position.y_offset) + sign * y);
            }
        }
    }
}

void apply_old_kerning(const Font& font, const ShapePlan& plan, ShapeBuffer& buffer, const ShapeScale& scale) noexcept {
    const Span table = table_of(font, font.kerning);
    if (!plan.requested_kerning || !has(table, 0, 4) || read_u16(table, 0) != 0 || buffer.len == 0) return;
    const bool horizontal = buffer.direction == ShapeDirection::left_to_right || buffer.direction == ShapeDirection::right_to_left;
    const bool backward = buffer.direction == ShapeDirection::right_to_left || buffer.direction == ShapeDirection::bottom_to_top;
    // A matcher for pairs: the glyphs the kerning feature is on for, marks
    // passed over.
    Applying applying{};
    GlyphDigest unused{};
    ApplyWork none{};
    applying.plan = &plan;
    applying.buffer = &buffer;
    applying.layout = &plan.tables[1];
    applying.definitions = &plan.definitions;
    applying.work = &none;
    applying.digest = &unused;
    applying.substitution = false;
    applying.lookup_mask = plan.kern_mask;
    applying.lookup_props = lookup_ignore_marks;
    update_matchers(applying);
    const usize count = read_u16(table, 2);
    usize place = 4;
    for (usize number = 0; number < count && has(table, place, 6); ++number) {
        const usize length = read_u16(table, place + 2);
        const u32 coverage = read_u16(table, place + 4);
        const usize subtable = place;
        if (length < 6) break;
        place = place + length;
        // Pairs along a horizontal line, by a list in order: the one form
        // read.
        const bool for_horizontal = (coverage & 0x0001U) != 0;
        const bool cross_stream = (coverage & 0x0004U) != 0;
        if ((coverage >> 8) != 0 || for_horizontal != horizontal || cross_stream || !has(table, subtable, 14)) continue;
        const usize pairs = read_u16(table, subtable + 6);
        if (!has(table, subtable + 14, pairs * 6)) continue;
        if (backward) buffer.reverse();
        for (usize index = 0; index < buffer.len;) {
            if ((buffer.info[index].mask & plan.kern_mask) == 0) {
                index = index + 1;
                continue;
            }
            const MatchRule any{};
            Skipper skipper = skipper_of(applying, false, any, index);
            usize unsafe_to = 0;
            if (!skipper_next(applying, skipper, unsafe_to)) {
                index = index + 1;
                continue;
            }
            const usize other = skipper.index;
            const u32 key = (buffer.info[index].glyph << 16) | (buffer.info[other].glyph & 0xFFFFU);
            usize low = 0;
            usize high = pairs;
            i32 kern = 0;
            bool found = false;
            while (low < high && !found) {
                const usize middle = low + (high - low) / 2;
                const u32 listed = read_u32(table, subtable + 14 + middle * 6);
                if (listed < key) {
                    low = middle + 1;
                } else if (listed > key) {
                    high = middle;
                } else {
                    kern = read_i16(table, subtable + 14 + middle * 6 + 4);
                    found = true;
                }
            }
            if (kern != 0) {
                // Half before the pair's second glyph, half on it.
                const i32 scaled = scale_value(kern, scale.x_mult);
                const i32 first_half = scaled >> 1;
                const i32 second_half = scaled - first_half;
                buffer.pos[index].x_advance = saturate(static_cast<i64>(buffer.pos[index].x_advance) + first_half);
                buffer.pos[other].x_advance = saturate(static_cast<i64>(buffer.pos[other].x_advance) + second_half);
                buffer.pos[other].x_offset = saturate(static_cast<i64>(buffer.pos[other].x_offset) + second_half);
                buffer.unsafe_to_break(index, other + 1);
            }
            index = other;
        }
        if (backward) buffer.reverse();
    }
}

} // namespace stxlib::glyphs::detail
