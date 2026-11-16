// stxlib::glyphs: what the shaper needs to know of a character
// (shape_unicode.hpp). After harfrust 0.16.0, src/unicode/mod.rs.

#include "algorithms/glyphs/shape_unicode.hpp"

namespace stxlib::glyphs::detail {

namespace {

struct UnicodeWay final {
    u32 script{0};
    u8 way{0};
};

struct UnicodePair final {
    u32 character{0};
    u32 mirror{0};
};

#include "algorithms/glyphs/shape_unicode.inc"

inline constexpr u32 last_character = 0x10FFFF;
inline constexpr u32 kind_category = 0x1F;
inline constexpr u32 kind_script_shift = 5;
inline constexpr u32 kind_class_shift = 13;
inline constexpr u32 kind_picture_shift = 21;
// A number that is no character: unassigned, the unknown script (the
// table's third), class 0.
inline constexpr u32 kind_beyond = category_unassigned | (2U << kind_script_shift);

inline constexpr usize level1_count = sizeof(unicode_level1) / sizeof(unicode_level1[0]);
inline constexpr usize level2_count = sizeof(unicode_level2) / sizeof(unicode_level2[0]);
inline constexpr usize level3_count = sizeof(unicode_level3) / sizeof(unicode_level3[0]);
inline constexpr usize kind_count = sizeof(unicode_kinds) / sizeof(unicode_kinds[0]);
inline constexpr usize script_count = sizeof(unicode_script_tags) / sizeof(unicode_script_tags[0]);
inline constexpr usize mirror_count = sizeof(unicode_mirrors) / sizeof(unicode_mirrors[0]);
inline constexpr usize made_of_count = sizeof(unicode_made_of) / sizeof(unicode_made_of[0]);
inline constexpr usize makes_count = sizeof(unicode_makes) / sizeof(unicode_makes[0]);
inline constexpr usize other_way_count = sizeof(unicode_other_ways) / sizeof(unicode_other_ways[0]);

// The index is whole: every character has a place at each level, each
// place a kind, each kind a script the table has. Checked when compiled,
// so that the reads below need no test of their own.
[[nodiscard]] constexpr bool index_whole() noexcept {
    bool whole = level1_count == ((static_cast<usize>(last_character) + 1) >> (unicode_low_bits + unicode_middle_bits)) &&
                 level2_count % (static_cast<usize>(1) << unicode_middle_bits) == 0 &&
                 level3_count % (static_cast<usize>(1) << unicode_low_bits) == 0 &&
                 sizeof(unicode_script_ways) / sizeof(unicode_script_ways[0]) == script_count &&
                 unicode_script_tags[0] == script_common && unicode_script_tags[1] == script_inherited &&
                 unicode_script_tags[2] == script_unknown;
    for (usize index = 0; whole && index < level1_count; ++index) {
        whole = (static_cast<usize>(unicode_level1[index]) + 1) << unicode_middle_bits <= level2_count;
    }
    for (usize index = 0; whole && index < level2_count; ++index) {
        whole = (static_cast<usize>(unicode_level2[index]) + 1) << unicode_low_bits <= level3_count;
    }
    for (usize index = 0; whole && index < level3_count; ++index) whole = unicode_level3[index] < kind_count;
    for (usize index = 0; whole && index < kind_count; ++index) {
        whole = ((unicode_kinds[index] >> kind_script_shift) & 0xFFU) < script_count && (unicode_kinds[index] & kind_category) <= 29;
    }
    return whole;
}
static_assert(index_whole());

[[nodiscard]] u32 kind_of(const u32 character) noexcept {
    if (character > last_character) return kind_beyond;
    const usize middle_mask = (static_cast<usize>(1) << unicode_middle_bits) - 1;
    const usize low_mask = (static_cast<usize>(1) << unicode_low_bits) - 1;
    const usize first = unicode_level1[character >> (unicode_low_bits + unicode_middle_bits)];
    const usize second = unicode_level2[(first << unicode_middle_bits) + ((character >> unicode_low_bits) & middle_mask)];
    const usize kind = unicode_level3[(second << unicode_low_bits) + (character & low_mask)];
    const u32 result = unicode_kinds[kind];
    return result;
}

// Hangul syllables are made of their letters by arithmetic.
inline constexpr u32 syllable_base = 0xAC00;
inline constexpr u32 leading_base = 0x1100;
inline constexpr u32 vowel_base = 0x1161;
inline constexpr u32 trailing_base = 0x11A7;
inline constexpr u32 leading_count = 19;
inline constexpr u32 vowel_count = 21;
inline constexpr u32 trailing_count = 28;
inline constexpr u32 vowel_trailing_count = vowel_count * trailing_count;
inline constexpr u32 syllable_count = leading_count * vowel_trailing_count;

[[nodiscard]] bool compose_hangul(const u32 first, const u32 second, u32& made) noexcept {
    if (first >= leading_base && first < leading_base + leading_count && second >= vowel_base && second < vowel_base + vowel_count) {
        made = syllable_base + (first - leading_base) * vowel_trailing_count + (second - vowel_base) * trailing_count;
        return true;
    }
    if (first >= syllable_base && first <= syllable_base + syllable_count - trailing_count && second > trailing_base &&
        second < trailing_base + trailing_count && (first - syllable_base) % trailing_count == 0) {
        made = first + (second - trailing_base);
        return true;
    }
    return false;
}

[[nodiscard]] bool decompose_hangul(const u32 character, u32& first, u32& second) noexcept {
    if (character < syllable_base || character - syllable_base >= syllable_count) return false;
    const u32 index = character - syllable_base;
    if (index % trailing_count != 0) {
        // A syllable with a trailing letter: the one without, and the letter.
        first = syllable_base + (index / trailing_count) * trailing_count;
        second = trailing_base + index % trailing_count;
    } else {
        first = leading_base + index / vowel_trailing_count;
        second = vowel_base + (index % vowel_trailing_count) / trailing_count;
    }
    return true;
}

inline constexpr u64 part_mask = 0x1FFFFF;

} // namespace

u32 general_category(const u32 character) noexcept {
    const u32 result = kind_of(character) & kind_category;
    return result;
}

u32 script_of(const u32 character) noexcept {
    const u32 result = unicode_script_tags[(kind_of(character) >> kind_script_shift) & 0xFFU];
    return result;
}

u32 combining_class(const u32 character) noexcept {
    const u32 result = (kind_of(character) >> kind_class_shift) & 0xFFU;
    return result;
}

u32 modified_combining_class(const u32 character) noexcept {
    // SAKOT comes after any tone marks, PADMA after any vowel marks, and
    // TSA -PHRU before U+0F74.
    if (character == 0x1A60 || character == 0x0FC6) return 254;
    if (character == 0x0F39) return 127;
    const u32 result = unicode_modified_classes[combining_class(character)];
    return result;
}

bool is_extended_pictographic(const u32 character) noexcept {
    const bool result = ((kind_of(character) >> kind_picture_shift) & 1U) != 0;
    return result;
}

u32 mirror_of(const u32 character) noexcept {
    usize low = 0;
    usize high = mirror_count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (unicode_mirrors[middle].character < character) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const u32 result = (low < mirror_count && unicode_mirrors[low].character == character) ? unicode_mirrors[low].mirror : 0;
    return result;
}

bool decompose(const u32 character, u32& first, u32& second) noexcept {
    first = 0;
    second = 0;
    if (decompose_hangul(character, first, second)) return true;
    if (character > last_character) return false;
    usize low = 0;
    usize high = made_of_count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if ((unicode_made_of[middle] >> 42) < character) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == made_of_count || (unicode_made_of[low] >> 42) != character) return false;
    first = static_cast<u32>((unicode_made_of[low] >> 21) & part_mask);
    second = static_cast<u32>(unicode_made_of[low] & part_mask);
    return true;
}

bool compose(const u32 first, const u32 second, u32& made) noexcept {
    made = 0;
    if (compose_hangul(first, second, made)) return true;
    if (first > last_character || second > last_character) return false;
    // The list is in the order of the two characters as one number.
    const u64 wanted = (static_cast<u64>(first) << 21) | second;
    usize low = 0;
    usize high = makes_count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if ((unicode_makes[middle] >> 21) < wanted) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == makes_count || (unicode_makes[low] >> 21) != wanted) return false;
    made = static_cast<u32>(unicode_makes[low] & part_mask);
    return true;
}

bool is_default_ignorable(const u32 character) noexcept {
    const u32 plane = character >> 16;
    bool result = false;
    if (plane == 0) {
        const u32 page = character >> 8;
        if (page == 0x00) result = character == 0x00AD;
        if (page == 0x03) result = character == 0x034F;
        if (page == 0x06) result = character == 0x061C;
        if (page == 0x17) result = character >= 0x17B4 && character <= 0x17B5;
        if (page == 0x18) result = character >= 0x180B && character <= 0x180F;
        if (page == 0x20) {
            result = (character >= 0x200B && character <= 0x200F) || (character >= 0x202A && character <= 0x202E) ||
                     (character >= 0x2060 && character <= 0x206F);
        }
        if (page == 0xFE) result = (character >= 0xFE00 && character <= 0xFE0F) || character == 0xFEFF;
        if (page == 0xFF) result = character >= 0xFFF0 && character <= 0xFFF8;
    } else if (plane == 0x01) {
        result = character >= 0x1D173 && character <= 0x1D17A;
    } else if (plane == 0x0E) {
        result = character >= 0xE0000 && character <= 0xE0FFF;
    }
    return result;
}

bool is_variation_selector(const u32 character) noexcept {
    const bool result = (character >= 0xFE00 && character <= 0xFE0F) || (character >= 0xE0100 && character <= 0xE01EF);
    return result;
}

u32 space_fallback(const u32 character) noexcept {
    u32 result = space_none;
    if (character == 0x0020 || character == 0x00A0) result = space_plain;
    if (character == 0x2000 || character == 0x2002) result = space_em_2;
    if (character == 0x2001 || character == 0x2003 || character == 0x3000) result = space_em;
    if (character == 0x2004) result = space_em_3;
    if (character == 0x2005) result = space_em_4;
    if (character == 0x2006) result = space_em_6;
    if (character == 0x2007) result = space_figure;
    if (character == 0x2008) result = space_punctuation;
    if (character == 0x2009) result = space_em_5;
    if (character == 0x200A) result = space_em_16;
    if (character == 0x202F) result = space_narrow;
    if (character == 0x205F) result = space_4_em_18;
    return result;
}

ShapeDirection way_of_script(const u32 script) noexcept {
    // 1 left to right, 2 right to left, 0 either; a script the tables have
    // not is written left to right.
    u32 way = 1;
    bool found = false;
    for (usize index = 0; index < script_count && !found; ++index) {
        if (unicode_script_tags[index] == script) {
            way = unicode_script_ways[index];
            found = true;
        }
    }
    for (usize index = 0; index < other_way_count && !found; ++index) {
        if (unicode_other_ways[index].script == script && script != 0) {
            way = unicode_other_ways[index].way;
            found = true;
        }
    }
    const ShapeDirection result = way == 1 ? ShapeDirection::left_to_right : (way == 2 ? ShapeDirection::right_to_left : ShapeDirection::unset);
    return result;
}

void init_unicode_props(ShapeGlyph& item, u32& scratch) noexcept {
    const u32 character = item.glyph;
    const u32 category = general_category(character);
    u32 props = category;
    if (character >= 0x80) {
        if (is_default_ignorable(character)) {
            props = props | unicode_ignorable;
            scratch = scratch | scratch_has_default_ignorables;
            if (character == 0x200C) props = props | unicode_format_zwnj;
            if (character == 0x200D) props = props | unicode_format_zwj;
            // The Mongolian free variation selectors and the tag characters
            // are hidden like the rest but are not skipped while shaping.
            if ((character >= 0x180B && character <= 0x180D) || character == 0x180F) props = props | unicode_hidden;
            if (character >= 0xE0020 && character <= 0xE007F) props = props | unicode_hidden;
            // The combining grapheme joiner is not skipped by substitution.
            if (character == 0x034F) {
                props = props | unicode_hidden;
                scratch = scratch | scratch_has_grapheme_joiner;
            }
        }
        if (category_is_mark(category)) {
            scratch = scratch | scratch_has_continuations;
            props = props | unicode_continuation | (modified_combining_class(character) << 8);
        }
    }
    item.unicode_props = static_cast<u16>(props);
}

void guess_segment_properties(ShapeBuffer& buffer) noexcept {
    if (buffer.script == 0) {
        bool found = false;
        for (usize index = 0; index < buffer.len && !found; ++index) {
            const u32 script = script_of(buffer.info[index].glyph);
            if (script != script_common && script != script_inherited && script != script_unknown) {
                buffer.script = script;
                found = true;
            }
        }
    }
    if (buffer.direction == ShapeDirection::unset) {
        if (buffer.script != 0) buffer.direction = way_of_script(buffer.script);
        if (buffer.direction == ShapeDirection::unset) buffer.direction = ShapeDirection::left_to_right;
    }
}

u64 unicode_table_sum() noexcept { return unicode_sum; }

} // namespace stxlib::glyphs::detail
