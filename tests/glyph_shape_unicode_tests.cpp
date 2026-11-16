// stxlib::glyphs, what the shaper knows of a character
// (src/algorithms/glyphs/shape_unicode.hpp). The values come from harfrust
// 0.16.0's tables through tools/gen_shape_unicode.py; the facts below are
// the Unicode Character Database's, written here by hand.
//
//   N1  the whole table: a sum over every character from 0 to 0x10FFFF of
//       what the functions give is the sum the generator worked out from
//       harfrust's own readers, so the two agree on every character;
//   N2  categories, scripts and combining classes of known characters, the
//       order marks are put in, and numbers that are no character;
//   N3  mirror images, and that a mirror's mirror is the character;
//   N4  what a character is made of and what two make: by hand, Hangul by
//       arithmetic for all 11,172 syllables, and for every character that
//       its parts either make it again or make nothing;
//   N5  what is not to show, variation selectors, spaces;
//   N6  which way a script is written;
//   N7  an item's properties from its character;
//   N8  a buffer's script and direction guessed from its text.

#include "algorithms/glyphs/shape_unicode.hpp"

#include "test_support.hpp"

#include <cstdio>

namespace {

namespace detail = stxlib::glyphs::detail;
using stxlib::u32;
using stxlib::u64;
using stxlib::usize;

[[nodiscard]] constexpr u32 tag(const char a, const char b, const char c, const char d) noexcept {
    const u32 result = (static_cast<u32>(static_cast<unsigned char>(a)) << 24) | (static_cast<u32>(static_cast<unsigned char>(b)) << 16) |
                       (static_cast<u32>(static_cast<unsigned char>(c)) << 8) | static_cast<u32>(static_cast<unsigned char>(d));
    return result;
}

// The generator's mixing, which has no sum that could wrap.
[[nodiscard]] u64 mix(const u64 before, const u64 value) noexcept {
    u64 sum = before ^ value;
    sum = sum ^ (sum << 13);
    sum = sum ^ (sum >> 7);
    sum = sum ^ (sum << 17);
    return sum;
}

[[nodiscard]] bool is_hangul_syllable(const u32 character) noexcept {
    const bool result = character >= 0xAC00 && character < 0xAC00 + 11172;
    return result;
}

void whole_table() {
    u64 sum = 0;
    for (u32 character = 0; character <= 0x10FFFF; ++character) {
        u32 first = 0;
        u32 second = 0;
        // The tables list no Hangul syllable: those are worked out.
        const bool made = !is_hangul_syllable(character) && detail::decompose(character, first, second);
        sum = mix(sum, detail::general_category(character));
        sum = mix(sum, detail::combining_class(character));
        sum = mix(sum, detail::script_of(character));
        sum = mix(sum, detail::is_extended_pictographic(character) ? 1U : 0U);
        sum = mix(sum, detail::mirror_of(character));
        sum = mix(sum, made ? first : 0U);
        sum = mix(sum, made ? second : 0U);
    }
    std::printf("the table's sum: %016llx\n", static_cast<unsigned long long>(sum));
    CHECK(sum == detail::unicode_table_sum());
}

void known_characters() {
    CHECK(detail::general_category('A') == detail::category_uppercase_letter && detail::script_of('A') == tag('L', 'a', 't', 'n'));
    CHECK(detail::general_category('a') == detail::category_lowercase_letter && detail::combining_class('a') == 0);
    CHECK(detail::general_category('0') == detail::category_decimal_number && detail::script_of('0') == detail::script_common);
    CHECK(detail::general_category(' ') == detail::category_space_separator);
    CHECK(detail::general_category('(') == detail::category_open_punctuation && detail::general_category(')') == detail::category_close_punctuation);
    CHECK(detail::general_category(0x0000) == detail::category_control);
    // COMBINING ACUTE ACCENT: a mark above, of whatever script it follows.
    CHECK(detail::general_category(0x0301) == detail::category_non_spacing_mark && detail::combining_class(0x0301) == 230 &&
          detail::script_of(0x0301) == detail::script_inherited && detail::modified_combining_class(0x0301) == 230);
    CHECK(detail::general_category(0x05D0) == detail::category_other_letter && detail::script_of(0x05D0) == tag('H', 'e', 'b', 'r'));
    CHECK(detail::script_of(0x0627) == tag('A', 'r', 'a', 'b') && detail::script_of(0x0915) == tag('D', 'e', 'v', 'a'));
    CHECK(detail::script_of(0x4E2D) == tag('H', 'a', 'n', 'i') && detail::script_of(0x3042) == tag('H', 'i', 'r', 'a') &&
          detail::script_of(0x30A2) == tag('K', 'a', 'n', 'a') && detail::script_of(0xAC00) == tag('H', 'a', 'n', 'g'));
    CHECK(detail::script_of(0x03B1) == tag('G', 'r', 'e', 'k') && detail::script_of(0x0416) == tag('C', 'y', 'r', 'l'));
    // GRINNING FACE and COPYRIGHT SIGN are pictographs; a letter is not.
    CHECK(detail::general_category(0x1F600) == detail::category_other_symbol && detail::script_of(0x1F600) == detail::script_common &&
          detail::is_extended_pictographic(0x1F600) && detail::is_extended_pictographic(0x00A9) && !detail::is_extended_pictographic('A'));
    // Not assigned, private use, a surrogate, and numbers that are no character.
    CHECK(detail::general_category(0x0378) == detail::category_unassigned && detail::script_of(0x0378) == detail::script_unknown);
    CHECK(detail::general_category(0xE000) == detail::category_private_use && detail::general_category(0xD800) == detail::category_surrogate);
    CHECK(detail::general_category(0x110000) == detail::category_unassigned && detail::script_of(0x110000) == detail::script_unknown &&
          detail::combining_class(0x110000) == 0 && detail::mirror_of(0x110000) == 0 && !detail::is_extended_pictographic(0xFFFFFFFFU) &&
          detail::general_category(0xFFFFFFFFU) == detail::category_unassigned);
    // The order marks are put in: HEBREW POINT SHEVA (class 10) after the
    // other vowels, DEVANAGARI SIGN VIRAMA as it is, and the three named
    // characters.
    CHECK(detail::combining_class(0x05B0) == 10 && detail::modified_combining_class(0x05B0) == 22);
    CHECK(detail::combining_class(0x094D) == 9 && detail::modified_combining_class(0x094D) == 9);
    CHECK(detail::modified_combining_class(0x1A60) == 254 && detail::modified_combining_class(0x0FC6) == 254 &&
          detail::modified_combining_class(0x0F39) == 127 && detail::modified_combining_class('A') == 0);
    CHECK(detail::category_is_mark(detail::general_category(0x0301)) && !detail::category_is_mark(detail::general_category('A')) &&
          detail::category_is_letter(detail::general_category('A')) && !detail::category_is_letter(detail::general_category('1')));
}

void mirrors() {
    CHECK(detail::mirror_of('(') == ')' && detail::mirror_of(')') == '(' && detail::mirror_of('<') == '>' && detail::mirror_of('[') == ']' &&
          detail::mirror_of('{') == '}');
    CHECK(detail::mirror_of(0x00AB) == 0x00BB && detail::mirror_of(0x00BB) == 0x00AB && detail::mirror_of(0x2264) == 0x2265);
    CHECK(detail::mirror_of('A') == 0 && detail::mirror_of(0) == 0 && detail::mirror_of(0x10FFFF) == 0);
    usize mirrored = 0;
    usize back = 0;
    for (u32 character = 0; character <= 0x10FFFF; ++character) {
        const u32 mirror = detail::mirror_of(character);
        if (mirror == 0) continue;
        mirrored = mirrored + 1;
        if (mirror != character && detail::mirror_of(mirror) == character) back = back + 1;
    }
    std::printf("mirrors: %zu characters, %zu whose mirror's mirror they are\n", mirrored, back);
    CHECK(mirrored > 300 && back == mirrored);
}

void made_of() {
    u32 first = 0;
    u32 second = 0;
    u32 made = 0;
    // LATIN CAPITAL LETTER A WITH RING ABOVE; ANGSTROM SIGN is that letter.
    CHECK(detail::decompose(0x00C5, first, second) && first == 0x0041 && second == 0x030A);
    CHECK(detail::decompose(0x212B, first, second) && first == 0x00C5 && second == 0);
    CHECK(detail::decompose(0x2126, first, second) && first == 0x03A9 && second == 0);
    // One step at a time: s with dot below and dot above is s with dot below, and a dot above.
    CHECK(detail::decompose(0x1E69, first, second) && first == 0x1E63 && second == 0x0307);
    CHECK(!detail::decompose('A', first, second) && first == 0 && second == 0 && !detail::decompose(0x110000, first, second) &&
          !detail::decompose(0xFFFFFFFFU, first, second));
    CHECK(detail::compose(0x0041, 0x030A, made) && made == 0x00C5);
    CHECK(detail::compose(0x1E63, 0x0307, made) && made == 0x1E69);
    CHECK(!detail::compose('A', 'B', made) && made == 0 && !detail::compose(0x110000, 0x0301, made) && !detail::compose(0x0041, 0xFFFFFFFFU, made));
    // Made of two, but two that are never put together again: DEVANAGARI
    // LETTER QA, HEBREW LETTER YOD WITH HIRIQ.
    CHECK(detail::decompose(0x0958, first, second) && first == 0x0915 && second == 0x093C && !detail::compose(0x0915, 0x093C, made));
    CHECK(detail::decompose(0xFB1D, first, second) && first == 0x05D9 && second == 0x05B4 && !detail::compose(0x05D9, 0x05B4, made));
    // A singleton is not composed back.
    CHECK(!detail::compose(0x00C5, 0, made));
    // Hangul: GA is KIYEOK and A; GAG is GA and a final KIYEOK.
    CHECK(detail::decompose(0xAC00, first, second) && first == 0x1100 && second == 0x1161);
    CHECK(detail::decompose(0xAC01, first, second) && first == 0xAC00 && second == 0x11A8);
    CHECK(detail::decompose(0xD7A3, first, second) && first == 0xD788 && second == 0x11C2 && !detail::decompose(0xD7A4, first, second) &&
          !detail::decompose(0xABFF, first, second));
    CHECK(detail::compose(0x1100, 0x1161, made) && made == 0xAC00 && detail::compose(0xAC00, 0x11A8, made) && made == 0xAC01);
    // A syllable that has a final letter takes no other; the filler is no final letter.
    CHECK(!detail::compose(0xAC01, 0x11A8, made) && !detail::compose(0xAC00, 0x11A7, made) && !detail::compose(0x1100, 0x1176, made));
    usize syllables = 0;
    for (u32 character = 0xAC00; character < 0xAC00 + 11172; ++character) {
        const bool parted = detail::decompose(character, first, second);
        if (parted && detail::compose(first, second, made) && made == character) syllables = syllables + 1;
    }
    CHECK(syllables == 11172);
    // Every character: its parts make it again or make nothing, and each
    // part is a character.
    usize parted = 0;
    usize again = 0;
    usize wrong = 0;
    for (u32 character = 0; character <= 0x10FFFF; ++character) {
        if (!detail::decompose(character, first, second)) continue;
        parted = parted + 1;
        if (first == 0 || first > 0x10FFFF || second > 0x10FFFF || first == character) wrong = wrong + 1;
        if (second == 0) continue;
        if (detail::compose(first, second, made)) {
            if (made == character) again = again + 1;
            if (made != character) wrong = wrong + 1;
        }
    }
    std::printf("made of others: %zu characters, %zu made again by their two parts\n", parted, again);
    CHECK(wrong == 0 && parted > 11172 + 2000 && again > 11172 + 900);
}

void hidden_and_spaces() {
    CHECK(detail::is_default_ignorable(0x00AD) && detail::is_default_ignorable(0x034F) && detail::is_default_ignorable(0x200B) &&
          detail::is_default_ignorable(0x200D) && detail::is_default_ignorable(0x2060) && detail::is_default_ignorable(0xFE0F) &&
          detail::is_default_ignorable(0xFEFF) && detail::is_default_ignorable(0xE0100) && detail::is_default_ignorable(0x1D173));
    // The Hangul fillers are drawn; a letter is a letter.
    CHECK(!detail::is_default_ignorable(0x115F) && !detail::is_default_ignorable(0x3164) && !detail::is_default_ignorable('A') &&
          !detail::is_default_ignorable(0x2010) && !detail::is_default_ignorable(0x1BCA0) && !detail::is_default_ignorable(0xFFFFFFFFU));
    CHECK(detail::is_variation_selector(0xFE00) && detail::is_variation_selector(0xFE0F) && detail::is_variation_selector(0xE0100) &&
          detail::is_variation_selector(0xE01EF) && !detail::is_variation_selector(0x180B) && !detail::is_variation_selector(0xFE10) &&
          !detail::is_variation_selector(0xE01F0));
    CHECK(detail::space_fallback(0x0020) == detail::space_plain && detail::space_fallback(0x00A0) == detail::space_plain &&
          detail::space_fallback(0x2003) == detail::space_em && detail::space_fallback(0x2002) == detail::space_em_2 &&
          detail::space_fallback(0x2009) == detail::space_em_5 && detail::space_fallback(0x200A) == detail::space_em_16 &&
          detail::space_fallback(0x202F) == detail::space_narrow && detail::space_fallback(0x205F) == detail::space_4_em_18 &&
          detail::space_fallback(0x3000) == detail::space_em);
    // OGHAM SPACE MARK is a space with ink; a letter is no space.
    CHECK(detail::space_fallback(0x1680) == detail::space_none && detail::space_fallback('A') == detail::space_none);
}

void ways() {
    using detail::ShapeDirection;
    CHECK(detail::way_of_script(tag('L', 'a', 't', 'n')) == ShapeDirection::left_to_right &&
          detail::way_of_script(tag('H', 'a', 'n', 'i')) == ShapeDirection::left_to_right);
    CHECK(detail::way_of_script(tag('H', 'e', 'b', 'r')) == ShapeDirection::right_to_left &&
          detail::way_of_script(tag('A', 'r', 'a', 'b')) == ShapeDirection::right_to_left &&
          detail::way_of_script(tag('S', 'y', 'r', 'c')) == ShapeDirection::right_to_left &&
          detail::way_of_script(tag('C', 'p', 'r', 't')) == ShapeDirection::right_to_left);
    // Runes and Old Italic were written either way.
    CHECK(detail::way_of_script(tag('R', 'u', 'n', 'r')) == ShapeDirection::unset && detail::way_of_script(tag('I', 't', 'a', 'l')) == ShapeDirection::unset);
    // A script the tables have not, and no script at all.
    CHECK(detail::way_of_script(tag('Q', 'a', 'a', 'a')) == ShapeDirection::left_to_right && detail::way_of_script(0) == ShapeDirection::left_to_right &&
          detail::way_of_script(detail::script_unknown) == ShapeDirection::left_to_right);
}

void properties() {
    u32 scratch = 0;
    detail::ShapeGlyph item{};
    item.glyph = 'A';
    detail::init_unicode_props(item, scratch);
    CHECK(item.unicode_props == detail::category_uppercase_letter && scratch == 0);
    // A mark: its bit, and the class it is put in order by, above.
    item.glyph = 0x0301;
    detail::init_unicode_props(item, scratch);
    CHECK(item.unicode_props == (detail::category_non_spacing_mark | detail::unicode_continuation | (230U << 8)) &&
          scratch == detail::scratch_has_continuations);
    scratch = 0;
    item.glyph = 0x200D;
    detail::init_unicode_props(item, scratch);
    CHECK(item.unicode_props == (detail::category_format | detail::unicode_ignorable | detail::unicode_format_zwj) &&
          scratch == detail::scratch_has_default_ignorables);
    item.glyph = 0x200C;
    detail::init_unicode_props(item, scratch);
    CHECK(item.unicode_props == (detail::category_format | detail::unicode_ignorable | detail::unicode_format_zwnj));
    // The combining grapheme joiner: a mark of class 0, hidden, and noted.
    scratch = 0;
    item.glyph = 0x034F;
    detail::init_unicode_props(item, scratch);
    CHECK(item.unicode_props == (detail::category_non_spacing_mark | detail::unicode_ignorable | detail::unicode_hidden | detail::unicode_continuation) &&
          scratch == (detail::scratch_has_default_ignorables | detail::scratch_has_grapheme_joiner | detail::scratch_has_continuations));
    // A tag character, and a Mongolian free variation selector.
    item.glyph = 0xE0041;
    detail::init_unicode_props(item, scratch);
    CHECK(item.unicode_props == (detail::category_format | detail::unicode_ignorable | detail::unicode_hidden));
    item.glyph = 0x180B;
    detail::init_unicode_props(item, scratch);
    CHECK((item.unicode_props & (detail::unicode_ignorable | detail::unicode_hidden)) == (detail::unicode_ignorable | detail::unicode_hidden));
    // Below 0x80 nothing is hidden or a mark, whatever the character.
    scratch = 0;
    for (u32 character = 0; character < 0x80; ++character) {
        item.glyph = character;
        detail::init_unicode_props(item, scratch);
        CHECK(item.unicode_props == detail::general_category(character));
    }
    CHECK(scratch == 0);
}

void fill(detail::ShapeBuffer& buffer, const u32* const characters, const usize count) {
    buffer.clear();
    for (usize index = 0; index < count; ++index) buffer.add(characters[index], static_cast<u32>(index));
}

void guessing() {
    using detail::ShapeDirection;
    detail::ShapeBuffer buffer{};
    // A space, a bracket, then alef: the first character with a script of its own decides.
    const u32 hebrew[] = {' ', '(', 0x05D0, 'a'};
    fill(buffer, hebrew, 4);
    detail::guess_segment_properties(buffer);
    CHECK(buffer.script == tag('H', 'e', 'b', 'r') && buffer.direction == ShapeDirection::right_to_left);
    const u32 latin[] = {'a', 0x05D0};
    fill(buffer, latin, 2);
    CHECK(buffer.script == 0 && buffer.direction == ShapeDirection::unset);
    detail::guess_segment_properties(buffer);
    CHECK(buffer.script == tag('L', 'a', 't', 'n') && buffer.direction == ShapeDirection::left_to_right);
    // Digits and a combining mark have no script of their own: none is set, and the run goes left to right.
    const u32 digits[] = {'1', '2', 0x0301};
    fill(buffer, digits, 3);
    detail::guess_segment_properties(buffer);
    CHECK(buffer.script == 0 && buffer.direction == ShapeDirection::left_to_right);
    // What the caller has said stays.
    fill(buffer, hebrew, 4);
    buffer.direction = ShapeDirection::left_to_right;
    detail::guess_segment_properties(buffer);
    CHECK(buffer.script == tag('H', 'e', 'b', 'r') && buffer.direction == ShapeDirection::left_to_right);
    fill(buffer, hebrew, 4);
    buffer.script = tag('L', 'a', 't', 'n');
    detail::guess_segment_properties(buffer);
    CHECK(buffer.script == tag('L', 'a', 't', 'n') && buffer.direction == ShapeDirection::left_to_right);
    // Runes: a script written either way goes left to right.
    const u32 runes[] = {0x16A0};
    fill(buffer, runes, 1);
    detail::guess_segment_properties(buffer);
    CHECK(buffer.script == tag('R', 'u', 'n', 'r') && buffer.direction == ShapeDirection::left_to_right);
    // An empty run.
    buffer.clear();
    detail::guess_segment_properties(buffer);
    CHECK(buffer.script == 0 && buffer.direction == ShapeDirection::left_to_right);
}

} // namespace

int main() {
    whole_table();
    known_characters();
    mirrors();
    made_of();
    hidden_and_spaces();
    ways();
    properties();
    guessing();
    const int status = stxlib_tests::finish("stxlib_glyph_shape_unicode_tests");
    return status;
}
