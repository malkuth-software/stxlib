#pragma once

// What a language is to stxlib::syntax's engine (engine.cpp): constant data, built at
// compile time by languages.cpp. Nothing here is mutable.
//
//   - a class for each byte (space, word, digit, quote, punctuation, operator, other);
//   - the language's words in an open-addressed hash table (at most half full, built by
//     constant evaluation), each with its token kind and what it asks of the next word;
//   - comment forms, string forms and the naming conventions the engine applies.

#include "core/checked.hpp"
#include "stxlib/syntax.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib::syntax::detail {

enum class ByteClass : u8 {
    other = 0,
    space,
    word,
    digit,
    quote,
    punctuation,
    operator_symbol,
};

// What a word asks of what follows it.
inline constexpr u8 word_declares_type = 1;      // the next word is a type's name (class, struct)
inline constexpr u8 word_declares_function = 2;  // the next word is a function's name (fn, def)
inline constexpr u8 word_string_prefix = 4;      // directly before a quote, part of the string (u8, f, b)
inline constexpr u8 word_raw_prefix = 8;         // a string prefix that opens the language's raw form (R, r)
inline constexpr u8 word_include = 16;           // a directive word whose <...> argument is a string

struct WordList final {
    TextView words;  // separated by single spaces
    TokenKind kind{TokenKind::keyword};
    u8 flags{0};
};

struct WordSlot final {
    const char* text{nullptr};
    u8 length{0};  // 0: empty slot
    TokenKind kind{TokenKind::plain};
    u8 flags{0};
};

inline constexpr u32 word_hash_seed = 2166136261u;
// FNV-1a, modulo 2^32.
[[nodiscard]] STXLIB_MODULAR constexpr u32 word_hash_step(const u32 hash, const u8 byte) noexcept { return (hash ^ byte) * 16777619u; }

// Not constexpr on purpose: reaching it during constant evaluation is the compile error for
// a table more than half full or a word over 255 bytes.
void word_index_too_small();

template <usize Slots>
struct WordIndex final {
    WordSlot slots[Slots]{};
};

// Slots must be a power of two at least twice the number of words.
template <usize Slots, usize Lists>
[[nodiscard]] consteval WordIndex<Slots> make_word_index(const WordList (&lists)[Lists]) {
    WordIndex<Slots> index;
    usize used = 0;
    for (usize list = 0; list < Lists; ++list) {
        const char* const text = lists[list].words.data();
        const usize count = lists[list].words.count();
        usize i = 0;
        while (i < count) {
            while (i < count && text[i] == ' ') ++i;
            const usize start = i;
            u32 hash = word_hash_seed;
            while (i < count && text[i] != ' ') {
                hash = word_hash_step(hash, static_cast<u8>(text[i]));
                ++i;
            }
            if (i == start) continue;
            used = used + 1;
            if (used * 2 > Slots || i - start > 255 || (Slots & (Slots - 1)) != 0) word_index_too_small();
            usize slot = hash & (Slots - 1);
            while (index.slots[slot].length != 0) slot = (slot + 1) & (Slots - 1);
            index.slots[slot] = WordSlot{text + start, static_cast<u8>(i - start), lists[list].kind, lists[list].flags};
        }
    }
    return index;
}

struct StringRule final {
    char quote{0};
    // The byte that escapes the next one; 0 for none.
    char escape{0};
    // Runs across lines until closed (otherwise a line's end closes it).
    bool multiline{false};
    // A doubled quote is an escaped quote (SQL).
    bool doubled{false};
    // Three quotes open a form that runs across lines until three quotes close it.
    bool triple{false};
    // A character literal: it must close within a few bytes, or the quote is not a literal.
    bool character{false};
    // The language's interpolation opener inside it starts code (GrammarSpec).
    bool interpolates{false};
    TokenKind kind{TokenKind::string};
};

inline constexpr usize string_rule_limit = 4;

// Which engine lexes the language.
enum class Family : u8 {
    c_like = 0,  // engine.cpp
    css,         // css.cpp
    markup,      // markup.cpp
};

// What languages.cpp writes for each language.
struct GrammarSpec final {
    Family family{Family::c_like};
    TextView name;
    TextView display_name;
    // Other names language_named accepts, and file-name endings, each separated by spaces.
    TextView aliases;
    TextView extensions;

    // Bytes besides letters, '_' and bytes of 0x80 and above that may start or continue a word.
    TextView word_extras;
    // Words compare without regard to ASCII case (the table holds them in lower case).
    bool fold_case{false};

    TextView line_comment[2];
    TextView block_open;
    TextView block_close;
    bool block_nests{false};
    // A backslash at a line's end continues a line comment or a string onto the next line.
    bool line_splice{false};
    // ///, //! and /** */ are documentation.
    bool documentation{false};

    StringRule strings[string_rule_limit];
    // R"delimiter( ... )delimiter" after a raw prefix.
    bool cpp_raw{false};
    // r#" ... "# after a raw prefix, any number of #.
    bool rust_raw{false};
    // A string directly before ':' is a property (JSON).
    bool string_keys{false};

    // A byte allowed between a number's digits (' or _); 0 for none.
    char number_separator{0};
    // The byte that starts a preprocessor directive as a line's first token; 0 for none.
    char directive{0};
    // The byte that starts an annotation before a word; 0 for none.
    char attribute{0};
    // #[...] and #![...] are attributes.
    bool bracket_attribute{false};
    // name! is a macro.
    bool macro_bang{false};
    // 'name not closed as a character literal is a label.
    bool quote_labels{false};

    // Conventions for a word that is not in the table.
    bool capital_types{false};    // Capitalized with a lower-case letter: a type
    bool upper_constants{false};  // ALL_CAPS of two bytes or more: a constant
    bool suffix_t_types{false};   // ends in _t: a type
    bool call_functions{false};   // before '(': a function
    // A capitalized word before '(' is a function, not a type (C#'s method names).
    bool calls_before_capitals{false};

    // /pattern/flags where a value cannot stand is a regular-expression literal.
    bool regex_literals{false};
    // Inside a string rule that interpolates: the opener ("${", "\\("), the byte that
    // closes the code and the byte that nests inside it ('}' and '{', or ')' and '(').
    TextView interpolation_open;
    char interpolation_close{0};
    char interpolation_nest{0};
    // The byte that starts a variable ($name, ${name}); 0 for none.
    char variable_sigil{0};
    // A line comment starts only at the line's start or after a space (shell's #).
    bool comment_needs_space{false};
    // [[ ... ]] and [=[ ... ]=] are strings across lines (Lua).
    bool long_brackets{false};
    // One more than the index of the string rule that @" opens (C# verbatim); 0 for none.
    u8 verbatim_rule{0};
    // Markup: element and attribute names compare without regard to case, and the
    // elements whose content is another language are known (HTML).
    bool html{false};
};

struct Grammar final {
    GrammarSpec spec;
    ByteClass classes[256]{};
    const WordSlot* slots{nullptr};
    u32 slot_mask{0};
};

[[nodiscard]] consteval Grammar make_grammar(const GrammarSpec& spec, const WordSlot* const slots, const usize slot_count) {
    Grammar grammar;
    grammar.spec = spec;
    grammar.slots = slots;
    grammar.slot_mask = slot_count == 0 ? 0 : static_cast<u32>(slot_count - 1);
    for (usize byte = 0; byte < 256; ++byte) {
        ByteClass kind = ByteClass::other;
        const bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        if (letter || byte == '_' || byte >= 0x80) kind = ByteClass::word;
        if (byte >= '0' && byte <= '9') kind = ByteClass::digit;
        if (byte == ' ' || byte == '\t' || byte == '\f' || byte == '\v' || byte == '\r' || byte == '\n') kind = ByteClass::space;
        grammar.classes[byte] = kind;
    }
    const TextView operators{"+-*/%=<>!&|^~?:"};
    for (usize k = 0; k < operators.count(); ++k) grammar.classes[static_cast<u8>(operators.data()[k])] = ByteClass::operator_symbol;
    const TextView punctuation{"()[]{};,."};
    for (usize k = 0; k < punctuation.count(); ++k) grammar.classes[static_cast<u8>(punctuation.data()[k])] = ByteClass::punctuation;
    for (usize k = 0; k < spec.word_extras.count(); ++k) grammar.classes[static_cast<u8>(spec.word_extras.data()[k])] = ByteClass::word;
    for (usize k = 0; k < string_rule_limit; ++k) {
        if (spec.strings[k].quote != 0) grammar.classes[static_cast<u8>(spec.strings[k].quote)] = ByteClass::quote;
    }
    return grammar;
}

// The grammar of a language; plain's for a value outside the set.
[[nodiscard]] const Grammar& grammar_of(Language language) noexcept;

} // namespace stxlib::syntax::detail
