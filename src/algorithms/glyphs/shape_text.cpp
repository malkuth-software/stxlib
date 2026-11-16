// stxlib::glyphs: shaping a run (shape_text.hpp). After harfrust 0.16.0,
// src/shape.rs, src/normalize.rs, src/fallback.rs and
// src/ot/shaper/hebrew.rs.

#include "algorithms/glyphs/shape_text.hpp"

#include "algorithms/glyphs/font_reader.hpp"
#include "algorithms/glyphs/shape_unicode.hpp"

namespace stxlib::glyphs::detail {

namespace {

inline constexpr usize most_combining_marks = 32;
// A character is taken apart no deeper than this (the longest chain the
// Unicode tables have is far shorter).
inline constexpr usize most_decomposition = 16;

[[nodiscard]] constexpr bool is_horizontal(const ShapeDirection direction) noexcept {
    const bool result = direction == ShapeDirection::left_to_right || direction == ShapeDirection::right_to_left;
    return result;
}

[[nodiscard]] constexpr bool is_forward(const ShapeDirection direction) noexcept {
    const bool result = direction == ShapeDirection::left_to_right || direction == ShapeDirection::top_to_bottom;
    return result;
}

struct Shaping final {
    const ShapePlan* plan{nullptr};
    const ShapeFont* font{nullptr};
    ShapeBuffer* buffer{nullptr};
    ApplyWork* work{nullptr};
    ShapeDirection target{ShapeDirection::unset};
};

[[nodiscard]] u32 nominal_glyph(const Shaping& shaping, const u32 character) noexcept {
    const u32 result = glyph_of(*(*shaping.font).font, character);
    return result;
}

// ---- properties and clusters -------------------------------------------------------------------------

void set_unicode_props(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const u32 global_mask = (*shaping.plan).global_mask;
    const usize length = buffer.len;
    for (usize index = 0; index < length; ++index) {
        ShapeGlyph& item = buffer.info[index];
        item.mask = global_mask;
        init_unicode_props(item, buffer.scratch);
        const u32 character = item.glyph;
        if (character < 0x80) continue;
        const u32 category = info_category(item);
        if (category == category_lowercase_letter || category == category_uppercase_letter || category == category_titlecase_letter ||
            category == category_other_letter || category == category_space_separator) {
            continue;
        }
        // What continues the character before it, besides marks: emoji
        // modifiers, the second of two regional indicators, what a zero
        // width joiner joins, the Katakana voicing marks and tags.
        if (category == category_modifier_symbol && character >= 0x1F3FB && character <= 0x1F3FF) {
            info_set_continuation(item, buffer.scratch);
        } else if (index != 0 && character >= 0x1F1E6 && character <= 0x1F1FF) {
            const ShapeGlyph& before = buffer.info[index - 1];
            if (before.glyph >= 0x1F1E6 && before.glyph <= 0x1F1FF && !info_is_continuation(before)) info_set_continuation(item, buffer.scratch);
        } else if (info_is_zwj(item)) {
            info_set_continuation(item, buffer.scratch);
            if (index + 1 < length && is_extended_pictographic(buffer.info[index + 1].glyph)) {
                ShapeGlyph& next = buffer.info[index + 1];
                next.mask = global_mask;
                init_unicode_props(next, buffer.scratch);
                info_set_continuation(next, buffer.scratch);
                index = index + 1;
            }
        } else if ((character >= 0xFF9E && character <= 0xFF9F) || (character >= 0xE0020 && character <= 0xE007F)) {
            info_set_continuation(item, buffer.scratch);
        } else if (character == 0x2044) {
            buffer.scratch = buffer.scratch | scratch_has_fraction_slash;
        }
    }
}

// A run that begins the text with a mark gets a dotted circle to carry it,
// when the font has one.
void insert_dotted_circle(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    if ((buffer.flags & buffer_beginning_of_text) == 0 || buffer.context_len[0] != 0 || buffer.len == 0 || !info_is_unicode_mark(buffer.info[0])) return;
    if (nominal_glyph(shaping, 0x25CC) == 0) return;
    buffer.clear_output();
    ShapeGlyph circle{};
    circle.glyph = 0x25CC;
    circle.mask = buffer.info[0].mask;
    circle.cluster = buffer.info[0].cluster;
    init_unicode_props(circle, buffer.scratch);
    buffer.output_info(circle);
    static_cast<void>(buffer.sync());
}

[[nodiscard]] bool same_grapheme(const ShapeGlyph& before, const ShapeGlyph& after) noexcept {
    static_cast<void>(before);
    const bool result = info_is_continuation(after);
    return result;
}

[[nodiscard]] bool same_cluster(const ShapeGlyph& before, const ShapeGlyph& after) noexcept {
    const bool result = before.cluster == after.cluster;
    return result;
}

void form_clusters(ShapeBuffer& buffer) noexcept {
    if ((buffer.scratch & scratch_has_continuations) == 0) return;
    usize start = 0;
    while (start < buffer.len) {
        const usize end = buffer.group_end(start, same_grapheme);
        buffer.merge_grapheme_clusters(start, end);
        start = end;
    }
}

// A run set against the way its script is written is turned, grapheme by
// grapheme: the lookups of a right-to-left script expect it that way.
void ensure_native_direction(ShapeBuffer& buffer) noexcept {
    const ShapeDirection direction = buffer.direction;
    ShapeDirection native = buffer.script != 0 ? way_of_script(buffer.script) : ShapeDirection::left_to_right;
    if (native == ShapeDirection::right_to_left && direction == ShapeDirection::left_to_right) {
        // Numbers in a right-to-left script go left to right.
        bool found_number = false;
        bool found_letter = false;
        bool found_indicator = false;
        for (usize index = 0; index < buffer.len && !found_letter; ++index) {
            const u32 category = info_category(buffer.info[index]);
            if (category == category_decimal_number) {
                found_number = true;
            } else if (category_is_letter(category)) {
                found_letter = true;
            } else if (buffer.info[index].glyph >= 0x1F1E6 && buffer.info[index].glyph <= 0x1F1FF) {
                found_indicator = true;
            }
        }
        if ((found_number || found_indicator) && !found_letter) native = ShapeDirection::left_to_right;
    }
    if (is_horizontal(direction) && direction != native && native != ShapeDirection::unset) {
        buffer.reverse_groups(same_grapheme, buffer.cluster_level == ClusterLevel::monotone_characters);
        buffer.direction = direction == ShapeDirection::left_to_right ? ShapeDirection::right_to_left : ShapeDirection::left_to_right;
    }
}

// For right-to-left text a bracket is its mirror image, when the font has
// that character; when not, the font's own mirroring feature is asked.
void rotate_characters(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    if (shaping.target != ShapeDirection::right_to_left) return;
    const u32 rtlm_mask = (*shaping.plan).rtlm_mask;
    for (usize index = 0; index < buffer.len; ++index) {
        ShapeGlyph& item = buffer.info[index];
        const u32 mirror = mirror_of(item.glyph);
        if (mirror != 0 && nominal_glyph(shaping, mirror) != 0) {
            item.glyph = mirror;
        } else {
            item.mask = item.mask | rtlm_mask;
        }
    }
}

// ---- Hangul ------------------------------------------------------------------------------------------

// Which letter of a syllable an item is, kept in its syllable number until
// the masks are set: 1 leading, 2 vowel, 3 trailing.
inline constexpr u32 hangul_l_base = 0x1100;
inline constexpr u32 hangul_v_base = 0x1161;
inline constexpr u32 hangul_t_base = 0x11A7;
inline constexpr u32 hangul_s_base = 0xAC00;
inline constexpr u32 hangul_l_count = 19;
inline constexpr u32 hangul_v_count = 21;
inline constexpr u32 hangul_t_count = 28;
inline constexpr u32 hangul_n_count = hangul_v_count * hangul_t_count;
inline constexpr u32 hangul_s_count = hangul_l_count * hangul_n_count;

[[nodiscard]] constexpr bool is_combining_l(const u32 u) noexcept { return u >= hangul_l_base && u < hangul_l_base + hangul_l_count; }
[[nodiscard]] constexpr bool is_combining_v(const u32 u) noexcept { return u >= hangul_v_base && u < hangul_v_base + hangul_v_count; }
[[nodiscard]] constexpr bool is_combining_t(const u32 u) noexcept { return u > hangul_t_base && u < hangul_t_base + hangul_t_count; }
[[nodiscard]] constexpr bool is_combined_s(const u32 u) noexcept { return u >= hangul_s_base && u < hangul_s_base + hangul_s_count; }
[[nodiscard]] constexpr bool is_hangul_l(const u32 u) noexcept { return (u >= 0x1100 && u <= 0x115F) || (u >= 0xA960 && u <= 0xA97C); }
[[nodiscard]] constexpr bool is_hangul_v(const u32 u) noexcept { return (u >= 0x1160 && u <= 0x11A7) || (u >= 0xD7B0 && u <= 0xD7C6); }
[[nodiscard]] constexpr bool is_hangul_t(const u32 u) noexcept { return (u >= 0x11A8 && u <= 0x11FF) || (u >= 0xD7CB && u <= 0xD7FB); }
[[nodiscard]] constexpr bool is_hangul_tone(const u32 u) noexcept { return u >= 0x302E && u <= 0x302F; }

[[nodiscard]] bool is_zero_width(const Shaping& shaping, const u32 character) noexcept {
    const u32 glyph = nominal_glyph(shaping, character);
    const bool result = glyph != 0 && shape_advance(*shaping.font, glyph) == 0;
    return result;
}

// Syllables as the font has them: letters that make a syllable the font
// has become it; a syllable the font has not becomes its letters, each
// marked for the form it takes; a tone mark goes before its syllable.
void preprocess_hangul(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    for (usize index = 0; index < buffer.len; ++index) buffer.info[index].syllable = 0;
    buffer.clear_output();
    usize start = 0;
    usize end = 0;
    while (buffer.idx < buffer.len && buffer.successful) {
        const u32 u = buffer.info[buffer.idx].glyph;
        if (is_hangul_tone(u)) {
            // After a syllable it is moved before it (unless it takes no
            // room, and is drawn over it); alone, it gets a dotted circle.
            if (start < end && end == buffer.out_len) {
                buffer.unsafe_to_break_from_output(start, buffer.idx);
                buffer.next_glyph();
                if (!is_zero_width(shaping, u) && buffer.successful && end < buffer.out_len) {
                    buffer.merge_out_clusters(start, end + 1);
                    const ShapeGlyph tone = buffer.out[end];
                    for (usize index = end; index > start; --index) buffer.out[index] = buffer.out[index - 1];
                    buffer.out[start] = tone;
                }
            } else if (nominal_glyph(shaping, 0x25CC) != 0) {
                const bool wide = !is_zero_width(shaping, u);
                const u32 pair[2] = {wide ? u : 0x25CCU, wide ? 0x25CCU : u};
                buffer.replace_glyphs(1, View<u32>(pair));
            } else {
                buffer.next_glyph();
            }
            start = buffer.out_len;
            end = buffer.out_len;
            continue;
        }
        start = buffer.out_len;
        if (is_hangul_l(u) && buffer.idx + 1 < buffer.len) {
            const u32 l = u;
            const u32 v = buffer.info[buffer.idx + 1].glyph;
            if (is_hangul_v(v)) {
                u32 t = 0;
                u32 t_index = 0;
                if (buffer.idx + 2 < buffer.len) {
                    t = buffer.info[buffer.idx + 2].glyph;
                    if (is_hangul_t(t)) {
                        t_index = t - hangul_t_base;
                    } else {
                        t = 0;
                    }
                }
                const usize letters = t != 0 ? 3 : 2;
                buffer.unsafe_to_break(buffer.idx, buffer.idx + letters);
                if (is_combining_l(l) && is_combining_v(v) && (t == 0 || is_combining_t(t))) {
                    const u32 syllable = hangul_s_base + (l - hangul_l_base) * hangul_n_count + (v - hangul_v_base) * hangul_t_count + t_index;
                    if (nominal_glyph(shaping, syllable) != 0) {
                        const u32 one[1] = {syllable};
                        buffer.replace_glyphs(letters, View<u32>(one));
                        end = start + 1;
                        continue;
                    }
                }
                // The font has not the syllable, or the letters are old
                // ones that make none: each letter takes its form.
                for (u32 letter = 1; letter <= letters && buffer.idx < buffer.len; ++letter) {
                    buffer.info[buffer.idx].syllable = static_cast<u8>(letter);
                    buffer.next_glyph();
                }
                end = start + letters;
                buffer.merge_out_grapheme_clusters(start, end);
                continue;
            }
        } else if (is_combined_s(u)) {
            const u32 syllable = u;
            const bool has_glyph = nominal_glyph(shaping, syllable) != 0;
            const u32 l_index = (syllable - hangul_s_base) / hangul_n_count;
            const u32 n_index = (syllable - hangul_s_base) % hangul_n_count;
            const u32 v_index = n_index / hangul_t_count;
            const u32 t_index = n_index % hangul_t_count;
            const bool more = buffer.idx + 1 < buffer.len;
            if (t_index == 0 && more && is_combining_t(buffer.info[buffer.idx + 1].glyph)) {
                // A syllable with no final letter, and a final letter.
                const u32 whole = syllable + (buffer.info[buffer.idx + 1].glyph - hangul_t_base);
                if (nominal_glyph(shaping, whole) != 0) {
                    const u32 one[1] = {whole};
                    buffer.replace_glyphs(2, View<u32>(one));
                    end = start + 1;
                    continue;
                }
                buffer.unsafe_to_break(buffer.idx, buffer.idx + 2);
            }
            if (!has_glyph || (t_index == 0 && more && is_hangul_t(buffer.info[buffer.idx + 1].glyph))) {
                const u32 letters[3] = {hangul_l_base + l_index, hangul_v_base + v_index, hangul_t_base + t_index};
                if (nominal_glyph(shaping, letters[0]) != 0 && nominal_glyph(shaping, letters[1]) != 0 &&
                    (t_index == 0 || nominal_glyph(shaping, letters[2]) != 0)) {
                    usize written = t_index != 0 ? 3 : 2;
                    buffer.replace_glyphs(1, View<u32>(letters, written));
                    if (has_glyph && t_index == 0) {
                        // The final letter that follows is the syllable's.
                        buffer.next_glyph();
                        written = written + 1;
                    }
                    end = start + written;
                    for (usize index = start; index < end && index < buffer.out_len; ++index) buffer.out[index].syllable = static_cast<u8>(index - start + 1);
                    buffer.merge_out_grapheme_clusters(start, end);
                    continue;
                }
            }
            if (has_glyph) {
                end = start + 1;
                buffer.next_glyph();
                continue;
            }
        }
        buffer.next_glyph();
    }
    static_cast<void>(buffer.sync());
}

void setup_masks_hangul(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const ShapePlan& plan = *shaping.plan;
    const u32 forms[4] = {0, plan_one_mask(plan, layout_tag('l', 'j', 'm', 'o')), plan_one_mask(plan, layout_tag('v', 'j', 'm', 'o')),
                          plan_one_mask(plan, layout_tag('t', 'j', 'm', 'o'))};
    const u32 calt_mask = plan_one_mask(plan, layout_tag('c', 'a', 'l', 't'));
    for (usize index = 0; index < buffer.len; ++index) {
        ShapeGlyph& item = buffer.info[index];
        item.mask = item.mask | forms[item.syllable < 4 ? item.syllable : 0];
        if (is_hangul_l(item.glyph) || is_hangul_v(item.glyph) || is_hangul_t(item.glyph)) item.mask = item.mask & ~calt_mask;
        item.syllable = 0;
    }
}

// ---- characters to glyphs: composing and taking apart --------------------------------------------------

// Hebrew: what Unicode composes, and, when the font cannot place marks,
// the presentation forms Unicode keeps out of composition.
[[nodiscard]] bool compose_hebrew(const ShapePlan& plan, const u32 first, const u32 second, u32& made) noexcept {
    if (compose(first, second, made)) return true;
    if (plan.has_gpos_mark) return false;
    const u32 dagesh_forms[27] = {0xFB30, 0xFB31, 0xFB32, 0xFB33, 0xFB34, 0xFB35, 0xFB36, 0x0000, 0xFB38, 0xFB39, 0xFB3A, 0xFB3B, 0xFB3C, 0x0000,
                                  0xFB3E, 0x0000, 0xFB40, 0xFB41, 0x0000, 0xFB43, 0xFB44, 0x0000, 0xFB46, 0xFB47, 0xFB48, 0xFB49, 0xFB4A};
    made = 0;
    if (second == 0x05B4 && first == 0x05D9) made = 0xFB1D;
    if (second == 0x05B7 && first == 0x05D9) made = 0xFB1F;
    if (second == 0x05B7 && first == 0x05D0) made = 0xFB2E;
    if (second == 0x05B8 && first == 0x05D0) made = 0xFB2F;
    if (second == 0x05B9 && first == 0x05D5) made = 0xFB4B;
    if (second == 0x05BC && first >= 0x05D0 && first <= 0x05EA) made = dagesh_forms[first - 0x05D0];
    if (second == 0x05BC && first == 0xFB2A) made = 0xFB2C;
    if (second == 0x05BC && first == 0xFB2B) made = 0xFB2D;
    if (second == 0x05BF && first == 0x05D1) made = 0xFB4C;
    if (second == 0x05BF && first == 0x05DB) made = 0xFB4D;
    if (second == 0x05BF && first == 0x05E4) made = 0xFB4E;
    if (second == 0x05C1 && first == 0x05E9) made = 0xFB2A;
    if (second == 0x05C1 && first == 0xFB49) made = 0xFB2C;
    if (second == 0x05C2 && first == 0x05E9) made = 0xFB2B;
    if (second == 0x05C2 && first == 0xFB49) made = 0xFB2D;
    const bool result = made != 0;
    return result;
}

[[nodiscard]] bool compose_for(const ShapePlan& plan, const u32 first, const u32 second, u32& made) noexcept {
    const bool result = plan.shaper == ShaperKind::hebrew ? compose_hebrew(plan, first, second, made) : compose(first, second, made);
    return result;
}

// A character written out as `character`, with its glyph.
void output_character(ShapeBuffer& buffer, const u32 character, const u32 glyph) noexcept {
    if (buffer.idx < buffer.len) buffer.info[buffer.idx].stage_data = static_cast<u16>(glyph);
    buffer.output_glyph(character);
    if (buffer.successful && buffer.out_len > 0) init_unicode_props(buffer.out[buffer.out_len - 1], buffer.scratch);
}

void next_character(ShapeBuffer& buffer, const u32 glyph) noexcept {
    if (buffer.idx < buffer.len) buffer.info[buffer.idx].stage_data = static_cast<u16>(glyph);
    buffer.next_glyph();
}

// The character is written as the parts it is made of, as far down as the
// font has glyphs for: its first part taken apart again, and again, until
// a part the font has (with `shortest`, the first such). False: it is
// made of nothing the font has, and nothing was written.
[[nodiscard]] bool decompose_character(Shaping& shaping, const bool shortest, const u32 character) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    u32 firsts[most_decomposition] = {};
    u32 first_glyphs[most_decomposition] = {};
    u32 seconds[most_decomposition] = {};
    u32 second_glyphs[most_decomposition] = {};
    usize levels = 0;
    u32 whole = character;
    bool going = true;
    bool at_once = false;
    while (going && levels < most_decomposition) {
        u32 first = 0;
        u32 second = 0;
        if (!decompose(whole, first, second)) {
            going = false;
        } else {
            const u32 second_glyph = second != 0 ? nominal_glyph(shaping, second) : 0;
            if (second != 0 && second_glyph == 0) {
                going = false;
            } else {
                firsts[levels] = first;
                first_glyphs[levels] = nominal_glyph(shaping, first);
                seconds[levels] = second;
                second_glyphs[levels] = second_glyph;
                levels = levels + 1;
                if (first_glyphs[levels - 1] != 0 && shortest) {
                    at_once = true;
                    going = false;
                } else {
                    whole = first;
                }
            }
        }
    }
    // The deepest first part the font has, then the second parts on the
    // way back up.
    usize deepest = levels;
    if (at_once) {
        deepest = levels - 1;
    } else {
        bool found = false;
        for (usize level = levels; level > 0 && !found; --level) {
            if (first_glyphs[level - 1] != 0) {
                deepest = level - 1;
                found = true;
            }
        }
        if (!found) return false;
    }
    output_character(buffer, firsts[deepest], first_glyphs[deepest]);
    for (usize level = deepest + 1; level > 0; --level) {
        if (seconds[level - 1] != 0) output_character(buffer, seconds[level - 1], second_glyphs[level - 1]);
    }
    return true;
}

void decompose_current(Shaping& shaping, const bool shortest) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    if (buffer.idx >= buffer.len) {
        buffer.successful = false;
        return;
    }
    const u32 character = buffer.info[buffer.idx].glyph;
    const u32 glyph = nominal_glyph(shaping, character);
    if (glyph != 0 && shortest) {
        next_character(buffer, glyph);
        return;
    }
    if (decompose_character(shaping, shortest, character)) {
        buffer.skip_glyph();
        return;
    }
    if (glyph != 0) {
        next_character(buffer, glyph);
        return;
    }
    // A space the font has not is the font's plain space, made the width
    // the missing one should have.
    if (info_is_space(buffer.info[buffer.idx])) {
        const u32 space = space_fallback(character);
        if (space != space_none) {
            u32 space_glyph = nominal_glyph(shaping, 0x0020);
            if (space_glyph == 0 && buffer.has_invisible) space_glyph = buffer.invisible;
            if (space_glyph != 0) {
                info_set_space_fallback(buffer.info[buffer.idx], space);
                next_character(buffer, space_glyph);
                buffer.scratch = buffer.scratch | scratch_has_space_fallback;
                return;
            }
        }
    }
    // A non-breaking hyphen the font has not is a hyphen.
    if (character == 0x2011) {
        const u32 hyphen = nominal_glyph(shaping, 0x2010);
        if (hyphen != 0) {
            next_character(buffer, hyphen);
            return;
        }
    }
    next_character(buffer, 0);
}

void set_current_glyph(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    if (buffer.idx >= buffer.len) return;
    const u32 glyph = nominal_glyph(shaping, buffer.info[buffer.idx].glyph);
    if (glyph != 0) buffer.info[buffer.idx].stage_data = static_cast<u16>(glyph);
}

// A cluster with a variation selector in it: a character and its selector
// are one glyph when the font lists the sequence; a selector the font has
// nothing for stays, remembered as one.
void handle_variation_selectors(Shaping& shaping, const usize end) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const Font& font = *(*shaping.font).font;
    while (end > 0 && buffer.idx < end - 1 && buffer.successful) {
        if (is_variation_selector(buffer.info[buffer.idx + 1].glyph)) {
            const u32 base = buffer.info[buffer.idx].glyph;
            const u32 selector = buffer.info[buffer.idx + 1].glyph;
            bool use_default = false;
            u32 glyph = variation_glyph(font, base, selector, use_default);
            if (use_default) glyph = nominal_glyph(shaping, base);
            if (glyph != 0) {
                buffer.info[buffer.idx].stage_data = static_cast<u16>(glyph);
                const u32 kept[1] = {base};
                buffer.replace_glyphs(2, View<u32>(kept));
            } else {
                set_current_glyph(shaping);
                buffer.next_glyph();
                buffer.scratch = buffer.scratch | scratch_has_variation_selector_fallback;
                if (buffer.idx < buffer.len) info_set_variation_selector(buffer.info[buffer.idx], true);
                set_current_glyph(shaping);
                buffer.next_glyph();
            }
            // Further selectors after one are just passed.
            while (buffer.idx < end && buffer.successful && is_variation_selector(buffer.info[buffer.idx].glyph)) {
                set_current_glyph(shaping);
                buffer.next_glyph();
            }
        } else {
            set_current_glyph(shaping);
            buffer.next_glyph();
        }
    }
    if (buffer.idx < end && buffer.successful) {
        set_current_glyph(shaping);
        buffer.next_glyph();
    }
}

void decompose_cluster(Shaping& shaping, const usize end, const bool shortest) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    for (usize index = buffer.idx; index < end && buffer.successful; ++index) {
        if (is_variation_selector(buffer.info[index].glyph)) {
            handle_variation_selectors(shaping, end);
            return;
        }
    }
    while (buffer.idx < end && buffer.successful) decompose_current(shaping, shortest);
}

[[nodiscard]] bool class_sorts_after(const ShapeGlyph& first, const ShapeGlyph& second) noexcept {
    const bool result = info_mark_class(first) > info_mark_class(second);
    return result;
}

// Hebrew: a patah or qamats, then a sheva or hiriq, then a meteg, are
// wanted with the meteg before the vowel after it.
void reorder_marks_hebrew(ShapeBuffer& buffer, const usize start, const usize end) noexcept {
    bool found = false;
    for (usize index = start; index + 2 < end && !found; ++index) {
        const u32 c0 = info_mark_class(buffer.info[index]);
        const u32 c1 = info_mark_class(buffer.info[index + 1]);
        const u32 c2 = info_mark_class(buffer.info[index + 2]);
        if ((c0 == 20 || c0 == 21) && (c1 == 22 || c1 == 23) && (c2 == 25 || c2 == 220)) {
            buffer.merge_clusters(index + 1, index + 3);
            const ShapeGlyph moved = buffer.info[index + 1];
            buffer.info[index + 1] = buffer.info[index + 2];
            buffer.info[index + 2] = moved;
            found = true;
        }
    }
}

void normalize(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const ShapePlan& plan = *shaping.plan;
    if (buffer.len == 0) return;
    // Composed where the font has the composed character; a character the
    // font has is not taken apart. Hangul is left as it was prepared.
    const bool composing = plan.shaper != ShaperKind::hangul;
    bool all_simple = true;
    const usize count = buffer.len;
    buffer.clear_output();
    bool going = true;
    while (going) {
        // A stretch with no marks: every character the font has passes at
        // once.
        usize end = buffer.idx + 1;
        while (end < count && !info_is_unicode_mark(buffer.info[end])) end = end + 1;
        if (end < count) end = end - 1;
        usize done = 0;
        while (buffer.idx + done < end) {
            const u32 glyph = nominal_glyph(shaping, buffer.info[buffer.idx + done].glyph);
            if (glyph == 0) break;
            buffer.info[buffer.idx + done].stage_data = static_cast<u16>(glyph);
            done = done + 1;
        }
        buffer.next_glyphs(done);
        while (buffer.idx < end && buffer.successful) decompose_current(shaping, true);
        if (buffer.idx == count || !buffer.successful) {
            going = false;
        } else {
            // A base and its marks.
            all_simple = false;
            end = buffer.idx + 1;
            while (end < count && info_is_unicode_mark(buffer.info[end])) end = end + 1;
            decompose_cluster(shaping, end, !composing);
            if (buffer.idx >= count || !buffer.successful) going = false;
        }
    }
    static_cast<void>(buffer.sync());

    // Marks in the order of their classes.
    if (!all_simple) {
        const usize length = buffer.len;
        usize index = 0;
        while (index < length) {
            if (info_mark_class(buffer.info[index]) == 0) {
                index = index + 1;
                continue;
            }
            usize end = index + 1;
            while (end < length && info_mark_class(buffer.info[end]) != 0) end = end + 1;
            if (end - index <= most_combining_marks) {
                buffer.sort(index, end, class_sorts_after);
                if (plan.shaper == ShaperKind::hebrew) reorder_marks_hebrew(buffer, index, end);
            }
            index = end + 1;
        }
    }
    // A grapheme joiner that kept two marks from being put in order has
    // done its work and is hidden no longer.
    if ((buffer.scratch & scratch_has_grapheme_joiner) != 0) {
        for (usize index = 1; index + 1 < buffer.len; ++index) {
            if (buffer.info[index].glyph == 0x034F) {
                const u32 before = info_mark_class(buffer.info[index - 1]);
                const u32 after = info_mark_class(buffer.info[index + 1]);
                if (after == 0 || before <= after) buffer.info[index].unicode_props = static_cast<u16>(buffer.info[index].unicode_props & ~unicode_hidden);
            }
        }
    }
    // A base and a mark the font has as one character become it.
    if (!all_simple && buffer.successful && composing) {
        const usize length = buffer.len;
        usize starter = 0;
        buffer.clear_output();
        buffer.next_glyph();
        while (buffer.idx < length && buffer.successful && buffer.out_len > 0) {
            const ShapeGlyph current = buffer.info[buffer.idx];
            bool composed_now = false;
            if (info_is_unicode_mark(current) &&
                (starter == buffer.out_len - 1 || info_mark_class(buffer.out[buffer.out_len - 1]) < info_mark_class(current))) {
                u32 made = 0;
                if (starter < buffer.out_len && compose_for(plan, buffer.out[starter].glyph, current.glyph, made)) {
                    const u32 glyph = nominal_glyph(shaping, made);
                    if (glyph != 0) {
                        buffer.next_glyph();
                        if (!buffer.successful) return;
                        buffer.merge_out_clusters(starter, buffer.out_len);
                        buffer.out_len = buffer.out_len - 1;
                        ShapeGlyph& joined = buffer.out[starter];
                        joined.glyph = made;
                        joined.stage_data = static_cast<u16>(glyph);
                        init_unicode_props(joined, buffer.scratch);
                        composed_now = true;
                    }
                }
            }
            if (!composed_now) {
                buffer.next_glyph();
                if (buffer.successful && buffer.out_len > 0 && info_mark_class(buffer.out[buffer.out_len - 1]) == 0) starter = buffer.out_len - 1;
            }
        }
        static_cast<void>(buffer.sync());
    }
}

// ---- masks -------------------------------------------------------------------------------------------

// Digits either side of a fraction slash get the features that make a
// fraction of them.
void setup_fraction_masks(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const ShapePlan& plan = *shaping.plan;
    if ((buffer.scratch & scratch_has_fraction_slash) == 0 || !plan.has_frac) return;
    const bool forward = is_forward(buffer.direction);
    const u32 pre_mask = forward ? (plan.numr_mask | plan.frac_mask) : (plan.frac_mask | plan.dnom_mask);
    const u32 post_mask = forward ? (plan.frac_mask | plan.dnom_mask) : (plan.numr_mask | plan.frac_mask);
    const usize length = buffer.len;
    usize index = 0;
    while (index < length) {
        if (buffer.info[index].glyph != 0x2044) {
            index = index + 1;
            continue;
        }
        usize start = index;
        while (start > 0 && info_category(buffer.info[start - 1]) == category_decimal_number) start = start - 1;
        usize end = index + 1;
        while (end < length && info_category(buffer.info[end]) == category_decimal_number) end = end + 1;
        if (start == index || end == index + 1) {
            if (start == index) buffer.unsafe_to_concat(start, start + 1);
            if (end == index + 1) buffer.unsafe_to_concat(end - 1, end);
            index = index + 1;
            continue;
        }
        buffer.unsafe_to_break(start, end);
        for (usize at = start; at < index; ++at) buffer.info[at].mask = buffer.info[at].mask | pre_mask;
        buffer.info[index].mask = buffer.info[index].mask | plan.frac_mask;
        for (usize at = index + 1; at < end; ++at) buffer.info[at].mask = buffer.info[at].mask | post_mask;
        index = end;
    }
}

void setup_masks(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const ShapePlan& plan = *shaping.plan;
    setup_fraction_masks(shaping);
    if (plan.shaper == ShaperKind::hangul) setup_masks_hangul(shaping);
    for (usize index = 0; index < plan.ranged.count(); ++index) {
        const PlanRequest& request = plan.ranged[index];
        u32 shift = 0;
        const u32 mask = plan_mask(plan, request.tag, shift);
        if (mask != 0) buffer.set_masks(request.value << shift, mask, request.start, request.end);
    }
}

// ---- marks placed without the font's help -----------------------------------------------------------

// Where a mark of a class sits when the font does not say: the class the
// placing below goes by.
[[nodiscard]] u32 recategorize_class(const u32 character, const u32 mark_class) noexcept {
    if (mark_class >= 200) return mark_class;
    u32 result = mark_class;
    // Thai and Lao marks have no classes that say where they sit.
    if ((character & ~0xFFU) == 0x0E00) {
        if (result == 0) {
            if (character == 0x0E31 || character == 0x0E34 || character == 0x0E35 || character == 0x0E36 || character == 0x0E37 ||
                character == 0x0E47 || character == 0x0E4C || character == 0x0E4D || character == 0x0E4E) {
                result = 232;
            } else if (character == 0x0EB1 || character == 0x0EB4 || character == 0x0EB5 || character == 0x0EB6 || character == 0x0EB7 ||
                       character == 0x0EBB || character == 0x0ECC || character == 0x0ECD) {
                result = 230;
            } else if (character == 0x0EBC) {
                result = 220;
            }
        } else if (character == 0x0E3A) {
            result = 222;
        }
    }
    // Hebrew points, Arabic and Syriac marks, Thai, Lao and Tibetan signs,
    // by the classes the reordering gave them.
    const u32 below = 220;
    const u32 above = 230;
    if (result == 22 || result == 15 || result == 16 || result == 17 || result == 23 || result == 18 || result == 19 || result == 20 || result == 21 ||
        result == 24 || result == 25) {
        result = below;
    } else if (result == 13) {
        result = 214;
    } else if (result == 10) {
        result = 232;
    } else if (result == 11 || result == 14) {
        result = 228;
    } else if (result == 26) {
        result = above;
    } else if (result == 12) {
        result = 12;
    } else if (result == 28 || result == 29 || result == 31 || result == 32 || result == 27 || result == 34 || result == 35 || result == 36) {
        result = above;
    } else if (result == 30 || result == 33) {
        result = below;
    } else if (result == 3) {
        result = 222;
    } else if (result == 107) {
        result = 232;
    } else if (result == 118) {
        result = below;
    } else if (result == 122) {
        result = above;
    } else if (result == 129) {
        result = below;
    } else if (result == 132) {
        result = above;
    } else if (result == 131) {
        result = below;
    }
    return result;
}

void recategorize_marks(ShapeBuffer& buffer) noexcept {
    for (usize index = 0; index < buffer.len; ++index) {
        ShapeGlyph& item = buffer.info[index];
        if (info_category(item) == category_non_spacing_mark) info_set_mark_class(item, recategorize_class(item.glyph, info_mark_class(item)));
    }
}

[[nodiscard]] bool glyph_box(const ShapeFont& font, const u32 glyph, GlyphBox& box) noexcept {
    box = GlyphBox{};
    const bool result = font.boxes != nullptr && font.boxes(font.context, glyph, box);
    return result;
}

void zero_mark_advances(ShapeBuffer& buffer, const usize start, const usize end, const bool adjust_offsets) noexcept {
    for (usize index = start; index < end; ++index) {
        if (info_category(buffer.info[index]) != category_non_spacing_mark) continue;
        ShapePosition& position = buffer.pos[index];
        if (adjust_offsets) {
            position.x_offset = saturate(static_cast<i64>(position.x_offset) - position.x_advance);
            position.y_offset = saturate(static_cast<i64>(position.y_offset) - position.y_advance);
        }
        position.x_advance = 0;
        position.y_advance = 0;
    }
}

// A mark against the box of what it sits on, which then grows by the mark.
void position_mark(const Shaping& shaping, const u32 glyph, ShapePosition& position, GlyphBox& base, const u32 mark_class) noexcept {
    const ShapeFont& font = *shaping.font;
    const ShapeDirection direction = (*shaping.buffer).direction;
    GlyphBox mark{};
    if (!glyph_box(font, glyph, mark)) return;
    const i32 gap = static_cast<i32>(font.scale.em / 16);
    position.x_offset = 0;
    position.y_offset = 0;
    // Along the line: over the middle, or to one side.
    const bool to_left = mark_class == 200 || mark_class == 218 || mark_class == 228;
    const bool to_right = mark_class == 216 || mark_class == 222 || mark_class == 232;
    const bool doubled = (mark_class == 233 || mark_class == 234) && is_horizontal(direction);
    if (doubled) {
        position.x_offset = saturate(static_cast<i64>(base.x_bearing) + (is_forward(direction) ? base.width : 0) - mark.width / 2 - mark.x_bearing);
    } else if (to_left) {
        position.x_offset = saturate(static_cast<i64>(base.x_bearing) - mark.x_bearing);
    } else if (to_right) {
        position.x_offset = saturate(static_cast<i64>(base.x_bearing) + base.width - mark.width - mark.x_bearing);
    } else {
        position.x_offset = saturate(static_cast<i64>(base.x_bearing) + saturate(static_cast<i64>(base.width) - mark.width) / 2 - mark.x_bearing);
    }
    // Across it: below or above, touching for the attached classes and
    // with a gap for the others.
    const bool attached = mark_class == 200 || mark_class == 202 || mark_class == 214 || mark_class == 216;
    const bool below = mark_class == 233 || mark_class == 218 || mark_class == 220 || mark_class == 222 || mark_class == 200 || mark_class == 202;
    const bool above = mark_class == 234 || mark_class == 228 || mark_class == 230 || mark_class == 232 || mark_class == 214 || mark_class == 216;
    if (below) {
        if (!attached) base.height = saturate(static_cast<i64>(base.height) - gap);
        position.y_offset = saturate(static_cast<i64>(base.y_bearing) + base.height - mark.y_bearing);
        // Never moved up to meet a base it is already below.
        if ((gap > 0) == (position.y_offset > 0)) {
            base.height = saturate(static_cast<i64>(base.height) - position.y_offset);
            position.y_offset = 0;
        }
        base.height = saturate(static_cast<i64>(base.height) + mark.height);
    } else if (above) {
        if (!attached) {
            base.y_bearing = saturate(static_cast<i64>(base.y_bearing) + gap);
            base.height = saturate(static_cast<i64>(base.height) - gap);
        }
        position.y_offset = saturate(static_cast<i64>(base.y_bearing) - (static_cast<i64>(mark.y_bearing) + mark.height));
        // Never moved down to meet a base it is already above.
        if ((gap > 0) != (position.y_offset > 0)) {
            const i32 correction = -(position.y_offset / 2);
            base.y_bearing = saturate(static_cast<i64>(base.y_bearing) + correction);
            base.height = saturate(static_cast<i64>(base.height) - correction);
            position.y_offset = saturate(static_cast<i64>(position.y_offset) + correction);
        }
        base.y_bearing = saturate(static_cast<i64>(base.y_bearing) - mark.height);
        base.height = saturate(static_cast<i64>(base.height) + mark.height);
    }
}

void position_around_base(Shaping& shaping, const usize base, const usize end, const bool adjust_offsets) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const ShapeFont& font = *shaping.font;
    const ShapePlan& plan = *shaping.plan;
    buffer.unsafe_to_break(base, end);
    const ShapeGlyph base_item = buffer.info[base];
    const ShapePosition base_position = buffer.pos[base];
    GlyphBox base_box{};
    if (!glyph_box(font, base_item.glyph, base_box)) {
        zero_mark_advances(buffer, base + 1, end, adjust_offsets);
        return;
    }
    base_box.y_bearing = saturate(static_cast<i64>(base_box.y_bearing) + base_position.y_offset);
    // The marks are set over the base's advance, not over its ink.
    base_box.x_bearing = 0;
    base_box.width = shape_advance(font, base_item.glyph);
    const u32 lig_id = info_lig_id(base_item);
    const i32 parts = static_cast<i32>(info_lig_num_comps(base_item));
    const bool forward = is_forward(buffer.direction);
    i64 x_offset = forward ? -static_cast<i64>(base_position.x_advance) : 0;
    i64 y_offset = forward ? -static_cast<i64>(base_position.y_advance) : 0;
    i32 last_part = -1;
    u32 last_class = 255;
    GlyphBox part_box = base_box;
    GlyphBox cluster_box = base_box;
    ShapeDirection across = ShapeDirection::unset;
    for (usize index = base + 1; index < end; ++index) {
        const ShapeGlyph& item = buffer.info[index];
        ShapePosition& position = buffer.pos[index];
        const u32 mark_class = info_mark_class(item);
        if (mark_class != 0) {
            if (parts > 1) {
                // A mark on a ligature goes over its part of the advance.
                const u32 mark_lig_id = info_lig_id(item);
                i32 part = static_cast<i32>(info_lig_comp(item)) - 1;
                if (lig_id == 0 || lig_id != mark_lig_id || part >= parts) part = parts - 1;
                if (last_part != part) {
                    last_part = part;
                    last_class = 255;
                    part_box = base_box;
                    if (across == ShapeDirection::unset) {
                        across = plan.direction;
                        if (!is_horizontal(across)) {
                            across = plan.script != 0 ? way_of_script(plan.script) : ShapeDirection::left_to_right;
                            if (across == ShapeDirection::unset) across = ShapeDirection::left_to_right;
                        }
                    }
                    const i32 from_left = across == ShapeDirection::left_to_right ? part : parts - 1 - part;
                    part_box.x_bearing = saturate(static_cast<i64>(part_box.x_bearing) + static_cast<i64>(from_left) * part_box.width / parts);
                    part_box.width = part_box.width / parts;
                }
            }
            if (last_class != mark_class) {
                last_class = mark_class;
                cluster_box = part_box;
            }
            position_mark(shaping, item.glyph, position, cluster_box, mark_class);
            position.x_advance = 0;
            position.y_advance = 0;
            position.x_offset = saturate(static_cast<i64>(position.x_offset) + x_offset);
            position.y_offset = saturate(static_cast<i64>(position.y_offset) + y_offset);
        } else if (forward) {
            x_offset = saturate(x_offset - position.x_advance);
            y_offset = saturate(y_offset - position.y_advance);
        } else {
            x_offset = saturate(x_offset + position.x_advance);
            y_offset = saturate(y_offset + position.y_advance);
        }
    }
}

[[nodiscard]] bool rides_along(const ShapeGlyph& item) noexcept {
    const bool result = info_is_hidden(item) || info_is_default_ignorable(item) || info_is_unicode_mark(item);
    return result;
}

void position_cluster(Shaping& shaping, const usize start, const usize end, const bool adjust_offsets) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    if (end - start < 2) return;
    usize index = start;
    while (index < end) {
        if (!info_is_unicode_mark(buffer.info[index])) {
            usize after = index + 1;
            while (after < end && rides_along(buffer.info[after])) after = after + 1;
            position_around_base(shaping, index, after, adjust_offsets);
            index = after - 1;
        }
        index = index + 1;
    }
}

void position_marks(Shaping& shaping, const bool adjust_offsets) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    usize start = 0;
    for (usize index = 1; index < buffer.len; ++index) {
        if (!rides_along(buffer.info[index])) {
            position_cluster(shaping, start, index, adjust_offsets);
            start = index;
        }
    }
    position_cluster(shaping, start, buffer.len, adjust_offsets);
}

// A space the font had no glyph for is given the width it should have.
void fallback_spaces(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const ShapeFont& font = *shaping.font;
    const i32 em = static_cast<i32>(font.scale.em);
    for (usize index = 0; index < buffer.len; ++index) {
        const ShapeGlyph& item = buffer.info[index];
        if (!info_is_space(item) || info_ligated(item)) continue;
        const u32 space = info_space_fallback(item);
        ShapePosition& position = buffer.pos[index];
        if (space == space_em || space == space_em_2 || space == space_em_3 || space == space_em_4 || space == space_em_5 || space == space_em_6 ||
            space == space_em_16) {
            const i32 part = static_cast<i32>(space);
            position.x_advance = (em + part / 2) / part;
        } else if (space == space_4_em_18) {
            position.x_advance = static_cast<i32>(static_cast<i64>(em) * 4 / 18);
        } else if (space == space_figure) {
            bool found = false;
            for (u32 digit = '0'; digit <= '9' && !found; ++digit) {
                const u32 glyph = glyph_of(*font.font, digit);
                if (glyph != 0) {
                    position.x_advance = shape_advance(font, glyph);
                    found = true;
                }
            }
        } else if (space == space_punctuation) {
            u32 glyph = glyph_of(*font.font, '.');
            if (glyph == 0) glyph = glyph_of(*font.font, ',');
            if (glyph != 0) position.x_advance = shape_advance(font, glyph);
        } else if (space == space_narrow) {
            // Half of the space's own width.
            position.x_advance = position.x_advance / 2;
        }
    }
}

// ---- the sequence ------------------------------------------------------------------------------------

void zero_mark_widths(ShapeBuffer& buffer, const bool adjust_offsets) noexcept {
    for (usize index = 0; index < buffer.len; ++index) {
        if (!info_is_mark(buffer.info[index])) continue;
        ShapePosition& position = buffer.pos[index];
        if (adjust_offsets) {
            position.x_offset = saturate(static_cast<i64>(position.x_offset) - position.x_advance);
            position.y_offset = saturate(static_cast<i64>(position.y_offset) - position.y_advance);
        }
        position.x_advance = 0;
        position.y_advance = 0;
    }
}

void zero_width_default_ignorables(ShapeBuffer& buffer) noexcept {
    if ((buffer.scratch & scratch_has_default_ignorables) == 0 || (buffer.flags & buffer_preserve_default_ignorables) != 0 ||
        (buffer.flags & buffer_remove_default_ignorables) != 0) {
        return;
    }
    for (usize index = 0; index < buffer.len; ++index) {
        if (!info_is_default_ignorable(buffer.info[index])) continue;
        ShapePosition& position = buffer.pos[index];
        position.x_advance = 0;
        position.y_advance = 0;
        position.x_offset = 0;
    }
}

[[nodiscard]] bool unwanted_ignorable(const ShapeGlyph& item) noexcept {
    const bool result = info_is_default_ignorable(item);
    return result;
}

// What is not to show becomes the font's space glyph (which the positions
// have already made of no width), or is taken out when the font has none.
void hide_default_ignorables(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    if ((buffer.scratch & scratch_has_default_ignorables) == 0 || (buffer.flags & buffer_preserve_default_ignorables) != 0) return;
    if ((buffer.flags & buffer_remove_default_ignorables) == 0) {
        u32 invisible = buffer.has_invisible ? buffer.invisible : nominal_glyph(shaping, ' ');
        if (invisible != 0 || buffer.has_invisible) {
            for (usize index = 0; index < buffer.len; ++index) {
                if (info_is_default_ignorable(buffer.info[index])) buffer.info[index].glyph = invisible;
            }
            return;
        }
    }
    buffer.delete_glyphs_in_place(unwanted_ignorable);
}

// What a glyph says of breaking is said by every glyph of its cluster.
void propagate_flags(ShapeBuffer& buffer) noexcept {
    u32 kept = glyph_flags_defined;
    if ((buffer.flags & buffer_produce_unsafe_to_concat) == 0) kept = kept & ~glyph_unsafe_to_concat;
    if ((buffer.flags & buffer_produce_safe_to_insert_tatweel) == 0) kept = kept & ~glyph_safe_to_insert_tatweel;
    usize start = 0;
    while (start < buffer.len) {
        const usize end = buffer.group_end(start, same_cluster);
        u32 mask = 0;
        for (usize index = start; index < end; ++index) mask = mask | buffer.info[index].mask;
        mask = mask & kept;
        for (usize index = start; index < end; ++index) buffer.info[index].mask = end - start == 1 ? (buffer.info[index].mask & kept) : mask;
        start = end;
    }
}

void shape_internal(Shaping& shaping) noexcept {
    ShapeBuffer& buffer = *shaping.buffer;
    const ShapePlan& plan = *shaping.plan;
    const ShapeFont& font = *shaping.font;
    set_unicode_props(shaping);
    insert_dotted_circle(shaping);
    form_clusters(buffer);
    ensure_native_direction(buffer);
    if (plan.shaper == ShaperKind::hangul) preprocess_hangul(shaping);

    // Characters to glyphs.
    rotate_characters(shaping);
    normalize(shaping);
    setup_masks(shaping);
    if (plan.fallback_mark_positioning) recategorize_marks(buffer);
    for (usize index = 0; index < buffer.len; ++index) buffer.info[index].glyph = buffer.info[index].stage_data;

    // The font's substitutions.
    GlyphDigest digest{};
    start_substitutions(plan, buffer, digest);
    if (plan.fallback_glyph_classes) {
        // A font that does not say which glyphs are marks: the characters
        // do.
        for (usize index = 0; index < buffer.len; ++index) {
            ShapeGlyph& item = buffer.info[index];
            const bool base = info_category(item) != category_non_spacing_mark || info_is_default_ignorable(item);
            item.glyph_props = static_cast<u16>(base ? glyph_is_base : glyph_is_mark);
        }
    }
    apply_substitutions(plan, buffer, *shaping.work, digest);

    // Advances, then the font's positions or what stands in for them.
    buffer.clear_positions();
    for (usize index = 0; index < buffer.len; ++index) buffer.pos[index].x_advance = shape_advance(font, buffer.info[index].glyph);
    if ((buffer.scratch & scratch_has_space_fallback) != 0) fallback_spaces(shaping);
    const bool adjust_offsets = plan.adjust_mark_positioning_when_zeroing && is_forward(buffer.direction);
    start_positions(buffer);
    if (plan.apply_gpos) apply_positions(plan, buffer, *shaping.work, font.scale, digest);
    if (plan.apply_kern) apply_old_kerning(*font.font, plan, buffer, font.scale);
    if (plan.zero_marks) zero_mark_widths(buffer, adjust_offsets);
    zero_width_default_ignorables(buffer);
    finish_positions(buffer);
    if (plan.fallback_mark_positioning) position_marks(shaping, adjust_offsets);
    if (!is_forward(buffer.direction)) buffer.reverse();

    hide_default_ignorables(shaping);
    propagate_flags(buffer);
    buffer.direction = shaping.target;
}

} // namespace

ShapeFont shape_font_of(const Font& font, const i64 size) noexcept {
    ShapeFont result{};
    result.font = &font;
    result.scale.em = font.units_per_em;
    if (size <= 0 || font.units_per_em == 0) return result;
    // FreeType's scale for the size, HarfBuzz's scale from that, and its
    // two multipliers from its scale.
    const i64 units = font.units_per_em;
    const i64 advance_scale = scale_of(font, size);
    const i64 shaper_scale = (advance_scale * units + 0x8000) >> 16;
    result.advance_scale = advance_scale;
    result.scale.em = shaper_scale;
    result.scale.x_mult = (shaper_scale << 16) / units;
    result.scale.y_mult = result.scale.x_mult;
    result.scale.x_multf = static_cast<float>(shaper_scale) / static_cast<float>(units);
    result.scale.y_multf = result.scale.x_multf;
    return result;
}

i32 shape_advance(const ShapeFont& font, const u32 glyph) noexcept {
    const i64 units = advance_of(*font.font, glyph);
    if (font.advance_scale == 0) return static_cast<i32>(units);
    // The advance in 16.16 pixels, rounded, and that to 26.6, rounded.
    const i64 fine = (units * font.advance_scale + 32) / 64;
    const i32 result = static_cast<i32>((fine + 512) >> 10);
    return result;
}

bool shape_run(const ShapePlan& plan, const ShapeFont& font, ShapeBuffer& buffer, ApplyWork& work) noexcept {
    Shaping shaping{};
    shaping.plan = &plan;
    shaping.font = &font;
    shaping.buffer = &buffer;
    shaping.work = &work;
    shaping.target = buffer.direction;
    buffer.enter();
    if (buffer.len > 0) shape_internal(shaping);
    buffer.leave();
    const bool result = buffer.successful;
    return result;
}

} // namespace stxlib::glyphs::detail
