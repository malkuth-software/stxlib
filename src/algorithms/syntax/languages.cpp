// stxlib::syntax's languages (include/stxlib/syntax.hpp): each one is a word table and a
// GrammarSpec (grammar.hpp), all constant data built at compile time, and the registry that
// finds a language by name or by file name.
//
// Adding a language: its word lists, a WordIndex of a power of two at least twice its word
// count (a smaller one does not compile), its GrammarSpec, a Language value, and a row in
// `grammars` below. Nothing in the engine changes unless the language has a form the engine
// does not have yet.
//
// Known gaps of these tables (each is lexical, and listed so nobody takes it for proved):
// JavaScript and TypeScript regular-expression literals and template interpolation; C#
// verbatim and interpolated strings; Lua long-bracket strings; Python f-string fields;
// Swift string interpolation and raw strings; trigraphs.

#include "grammar.hpp"

namespace stxlib::syntax {

namespace detail {

namespace {

constexpr TokenKind keyword = TokenKind::keyword;
constexpr TokenKind control = TokenKind::control;
constexpr TokenKind type = TokenKind::type;
constexpr TokenKind constant = TokenKind::constant;
constexpr TokenKind prefix = TokenKind::string;
constexpr TokenKind directive = TokenKind::directive;

constexpr StringRule double_quoted{.quote = '"', .escape = '\\'};
constexpr StringRule single_quoted{.quote = '\'', .escape = '\\'};
constexpr StringRule character_quoted{.quote = '\'', .escape = '\\', .character = true, .kind = TokenKind::character};

// ---------------------------------------------------------------- plain

constexpr Grammar plain_grammar = make_grammar(GrammarSpec{.name = "plain", .display_name = "Plain text", .aliases = "text txt", .extensions = ".txt"},
                                               nullptr, 0);

// ---------------------------------------------------------------- C

constexpr WordList c_lists[] = {
    {"auto const extern inline register restrict static volatile typedef sizeof alignas alignof constexpr static_assert thread_local typeof "
     "_Alignas _Alignof _Atomic _Generic _Noreturn _Static_assert _Thread_local",
     keyword, 0},
    {"struct union enum", keyword, word_declares_type},
    {"if else for while do switch case default break continue return goto", control, 0},
    {"void char short int long float double signed unsigned bool _Bool _Complex _Imaginary", type, 0},
    {"true false NULL nullptr", constant, 0},
    {"u8 u U L", prefix, word_string_prefix},
    {"include import embed", directive, word_include},
};
constexpr WordIndex<256> c_index = make_word_index<256>(c_lists);
constexpr Grammar c_grammar = make_grammar(GrammarSpec{.name = "c",
                                                       .display_name = "C",
                                                       .extensions = ".c",
                                                       .line_comment = {"//"},
                                                       .block_open = "/*",
                                                       .block_close = "*/",
                                                       .line_splice = true,
                                                       .documentation = true,
                                                       .strings = {double_quoted, character_quoted},
                                                       .number_separator = '\'',
                                                       .directive = '#',
                                                       .upper_constants = true,
                                                       .suffix_t_types = true,
                                                       .call_functions = true},
                                           c_index.slots, 256);

// ---------------------------------------------------------------- C++

constexpr WordList cpp_lists[] = {
    {"alignas alignof asm const consteval constexpr constinit const_cast decltype delete dynamic_cast explicit export extern friend inline "
     "mutable namespace new noexcept operator private protected public register reinterpret_cast requires sizeof static static_assert "
     "static_cast template thread_local typedef typeid using virtual volatile final override import module and and_eq bitand bitor compl "
     "not not_eq or or_eq xor xor_eq",
     keyword, 0},
    {"class struct union enum typename concept", keyword, word_declares_type},
    {"if else for while do switch case default break continue return goto try catch throw co_await co_return co_yield", control, 0},
    {"void bool char char8_t char16_t char32_t wchar_t short int long float double signed unsigned auto", type, 0},
    {"true false nullptr this NULL", constant, 0},
    {"u8 u U L", prefix, word_string_prefix},
    {"R u8R uR UR LR", prefix, word_raw_prefix},
    {"include embed", directive, word_include},
};
constexpr WordIndex<512> cpp_index = make_word_index<512>(cpp_lists);
constexpr Grammar cpp_grammar = make_grammar(GrammarSpec{.name = "cpp",
                                                         .display_name = "C++",
                                                         .aliases = "c++ cxx cc hpp",
                                                         .extensions = ".cpp .hpp .cc .hh .cxx .hxx .h .inc .ipp .tpp .mm",
                                                         .line_comment = {"//"},
                                                         .block_open = "/*",
                                                         .block_close = "*/",
                                                         .line_splice = true,
                                                         .documentation = true,
                                                         .strings = {double_quoted, character_quoted},
                                                         .cpp_raw = true,
                                                         .number_separator = '\'',
                                                         .directive = '#',
                                                         .capital_types = true,
                                                         .upper_constants = true,
                                                         .suffix_t_types = true,
                                                         .call_functions = true},
                                             cpp_index.slots, 512);

// ---------------------------------------------------------------- C#

constexpr WordList csharp_lists[] = {
    {"abstract as async await base checked const delegate event explicit extern fixed implicit in internal is lock namespace new operator "
     "out override params partial private protected public readonly ref sealed sizeof stackalloc static typeof unchecked unsafe using "
     "virtual volatile get set init add remove where when with required file global nameof var dynamic",
     keyword, 0},
    {"class struct interface enum record", keyword, word_declares_type},
    {"if else for foreach while do switch case default break continue return goto try catch finally throw yield", control, 0},
    {"void bool byte sbyte char short ushort int uint long ulong nint nuint float double decimal string object", type, 0},
    {"true false null this", constant, 0},
};
constexpr WordIndex<256> csharp_index = make_word_index<256>(csharp_lists);
constexpr Grammar csharp_grammar = make_grammar(GrammarSpec{.name = "csharp",
                                                            .display_name = "C#",
                                                            .aliases = "c# cs",
                                                            .extensions = ".cs",
                                                            .line_comment = {"//"},
                                                            .block_open = "/*",
                                                            .block_close = "*/",
                                                            .documentation = true,
                                                            .strings = {double_quoted, character_quoted},
                                                            .directive = '#',
                                                            .capital_types = true,
                                                            .call_functions = true,
                                                            .calls_before_capitals = true},
                                                csharp_index.slots, 256);

// ---------------------------------------------------------------- Java

constexpr WordList java_lists[] = {
    {"abstract assert final import instanceof native new package private protected public static strictfp super synchronized throws "
     "transient volatile var module requires exports opens uses provides sealed permits",
     keyword, 0},
    {"class interface enum record extends implements", keyword, word_declares_type},
    {"if else for while do switch case default break continue return try catch finally throw yield", control, 0},
    {"void boolean byte char short int long float double", type, 0},
    {"true false null this", constant, 0},
};
constexpr WordIndex<256> java_index = make_word_index<256>(java_lists);
constexpr Grammar java_grammar = make_grammar(GrammarSpec{.name = "java",
                                                          .display_name = "Java",
                                                          .extensions = ".java",
                                                          .word_extras = "$",
                                                          .line_comment = {"//"},
                                                          .block_open = "/*",
                                                          .block_close = "*/",
                                                          .documentation = true,
                                                          .strings = {StringRule{.quote = '"', .escape = '\\', .triple = true}, character_quoted},
                                                          .attribute = '@',
                                                          .capital_types = true,
                                                          .upper_constants = true,
                                                          .call_functions = true},
                                              java_index.slots, 256);

// ---------------------------------------------------------------- JavaScript and TypeScript

constexpr WordList javascript_lists[] = {
    {"async await const debugger delete export from get import in instanceof let new of set static super typeof var void with as", keyword, 0},
    {"class extends", keyword, word_declares_type},
    {"function", keyword, word_declares_function},
    {"if else for while do switch case default break continue return try catch finally throw yield", control, 0},
    {"true false null undefined NaN Infinity this globalThis", constant, 0},
};
constexpr WordIndex<128> javascript_index = make_word_index<128>(javascript_lists);
constexpr StringRule template_quoted{.quote = '`', .escape = '\\', .multiline = true};
constexpr Grammar javascript_grammar = make_grammar(GrammarSpec{.name = "javascript",
                                                                .display_name = "JavaScript",
                                                                .aliases = "js jsx node mjs",
                                                                .extensions = ".js .mjs .cjs .jsx",
                                                                .word_extras = "$",
                                                                .line_comment = {"//"},
                                                                .block_open = "/*",
                                                                .block_close = "*/",
                                                                .documentation = true,
                                                                .strings = {double_quoted, single_quoted, template_quoted},
                                                                .attribute = '@',
                                                                .capital_types = true,
                                                                .upper_constants = true,
                                                                .call_functions = true},
                                                    javascript_index.slots, 128);

constexpr WordList typescript_lists[] = {
    {"async await const debugger delete export from get import in instanceof let new of set static super typeof var with as abstract "
     "declare is keyof namespace module override private protected public readonly satisfies infer asserts unique accessor",
     keyword, 0},
    {"class extends interface type enum implements", keyword, word_declares_type},
    {"function", keyword, word_declares_function},
    {"if else for while do switch case default break continue return try catch finally throw yield", control, 0},
    {"any bigint boolean never number object string symbol unknown void", type, 0},
    {"true false null undefined NaN Infinity this globalThis", constant, 0},
};
constexpr WordIndex<256> typescript_index = make_word_index<256>(typescript_lists);
constexpr Grammar typescript_grammar = make_grammar(GrammarSpec{.name = "typescript",
                                                                .display_name = "TypeScript",
                                                                .aliases = "ts tsx",
                                                                .extensions = ".ts .tsx .mts .cts",
                                                                .word_extras = "$",
                                                                .line_comment = {"//"},
                                                                .block_open = "/*",
                                                                .block_close = "*/",
                                                                .documentation = true,
                                                                .strings = {double_quoted, single_quoted, template_quoted},
                                                                .attribute = '@',
                                                                .capital_types = true,
                                                                .upper_constants = true,
                                                                .call_functions = true},
                                                    typescript_index.slots, 256);

// ---------------------------------------------------------------- Go

constexpr WordList go_lists[] = {
    {"chan const defer go import interface map package range struct var", keyword, 0},
    {"type", keyword, word_declares_type},
    {"func", keyword, word_declares_function},
    {"if else for switch case default break continue return goto select fallthrough", control, 0},
    {"bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr any "
     "comparable",
     type, 0},
    {"true false nil iota", constant, 0},
};
constexpr WordIndex<128> go_index = make_word_index<128>(go_lists);
constexpr Grammar go_grammar = make_grammar(GrammarSpec{.name = "go",
                                                        .display_name = "Go",
                                                        .aliases = "golang",
                                                        .extensions = ".go",
                                                        .line_comment = {"//"},
                                                        .block_open = "/*",
                                                        .block_close = "*/",
                                                        .strings = {double_quoted, character_quoted, StringRule{.quote = '`', .multiline = true}},
                                                        .call_functions = true},
                                            go_index.slots, 128);

// ---------------------------------------------------------------- Rust

constexpr WordList rust_lists[] = {
    {"as async await const crate dyn extern in let mod move mut pub ref static super unsafe use where", keyword, 0},
    {"struct enum trait type union impl", keyword, word_declares_type},
    {"fn", keyword, word_declares_function},
    {"if else for while loop match break continue return yield", control, 0},
    {"bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize", type, 0},
    {"true false self", constant, 0},
    {"b c", prefix, word_string_prefix},
    {"r br cr", prefix, word_raw_prefix},
};
constexpr WordIndex<128> rust_index = make_word_index<128>(rust_lists);
constexpr Grammar rust_grammar = make_grammar(GrammarSpec{.name = "rust",
                                                          .display_name = "Rust",
                                                          .aliases = "rs",
                                                          .extensions = ".rs",
                                                          .line_comment = {"//"},
                                                          .block_open = "/*",
                                                          .block_close = "*/",
                                                          .block_nests = true,
                                                          .documentation = true,
                                                          .strings = {StringRule{.quote = '"', .escape = '\\', .multiline = true}, character_quoted},
                                                          .rust_raw = true,
                                                          .bracket_attribute = true,
                                                          .macro_bang = true,
                                                          .quote_labels = true,
                                                          .capital_types = true,
                                                          .upper_constants = true,
                                                          .call_functions = true},
                                              rust_index.slots, 128);

// ---------------------------------------------------------------- Swift

constexpr WordList swift_lists[] = {
    {"as associatedtype async await convenience deinit didSet dynamic fileprivate final get import indirect infix init inout internal is "
     "lazy let mutating nonisolated nonmutating open operator optional override postfix prefix private public required rethrows set some "
     "any static subscript super throws unowned var weak willSet where",
     keyword, 0},
    {"class struct enum protocol extension actor typealias", keyword, word_declares_type},
    {"func", keyword, word_declares_function},
    {"if else for while repeat switch case default break continue return guard defer do try catch throw fallthrough in", control, 0},
    {"true false nil self Self", constant, 0},
};
constexpr WordIndex<256> swift_index = make_word_index<256>(swift_lists);
constexpr Grammar swift_grammar = make_grammar(GrammarSpec{.name = "swift",
                                                           .display_name = "Swift",
                                                           .extensions = ".swift",
                                                           .line_comment = {"//"},
                                                           .block_open = "/*",
                                                           .block_close = "*/",
                                                           .block_nests = true,
                                                           .documentation = true,
                                                           .strings = {StringRule{.quote = '"', .escape = '\\', .triple = true}},
                                                           .directive = '#',
                                                           .attribute = '@',
                                                           .capital_types = true,
                                                           .call_functions = true},
                                               swift_index.slots, 256);

// ---------------------------------------------------------------- Kotlin

constexpr WordList kotlin_lists[] = {
    {"abstract actual annotation as by companion const constructor crossinline data expect external final get import in infix init inline "
     "inner internal is lateinit noinline open operator out override package private protected public reified sealed set super suspend "
     "tailrec val var vararg where",
     keyword, 0},
    {"class interface object typealias enum", keyword, word_declares_type},
    {"fun", keyword, word_declares_function},
    {"if else for while do when break continue return try catch finally throw", control, 0},
    {"true false null this", constant, 0},
};
constexpr WordIndex<256> kotlin_index = make_word_index<256>(kotlin_lists);
constexpr Grammar kotlin_grammar = make_grammar(GrammarSpec{.name = "kotlin",
                                                            .display_name = "Kotlin",
                                                            .aliases = "kt kts",
                                                            .extensions = ".kt .kts",
                                                            .line_comment = {"//"},
                                                            .block_open = "/*",
                                                            .block_close = "*/",
                                                            .block_nests = true,
                                                            .documentation = true,
                                                            .strings = {StringRule{.quote = '"', .escape = '\\', .triple = true}, character_quoted},
                                                            .attribute = '@',
                                                            .capital_types = true,
                                                            .upper_constants = true,
                                                            .call_functions = true},
                                                kotlin_index.slots, 256);

// ---------------------------------------------------------------- Python

constexpr WordList python_lists[] = {
    {"and as assert async await del from global import in is lambda nonlocal not or with type", keyword, 0},
    {"class", keyword, word_declares_type},
    {"def", keyword, word_declares_function},
    {"if elif else for while break continue return try except finally raise yield pass match case", control, 0},
    {"bool bytes complex dict float frozenset int list object set str tuple", type, 0},
    {"True False None self cls", constant, 0},
    {"r R b B f F u U rb rB Rb RB br bR Br BR fr fR Fr FR rf rF Rf RF", prefix, word_string_prefix},
};
constexpr WordIndex<256> python_index = make_word_index<256>(python_lists);
constexpr Grammar python_grammar = make_grammar(GrammarSpec{.name = "python",
                                                            .display_name = "Python",
                                                            .aliases = "py python3",
                                                            .extensions = ".py .pyi .pyw",
                                                            .line_comment = {"#"},
                                                            .strings = {StringRule{.quote = '"', .escape = '\\', .triple = true},
                                                                        StringRule{.quote = '\'', .escape = '\\', .triple = true}},
                                                            .attribute = '@',
                                                            .capital_types = true,
                                                            .upper_constants = true,
                                                            .call_functions = true},
                                                python_index.slots, 256);

// ---------------------------------------------------------------- SQL

constexpr WordList sql_lists[] = {
    {"add all alter and any as asc between by check column constraint create cross database default delete desc distinct drop exists "
     "foreign from full group having in index inner insert into is join key left like limit not offset on or order outer primary "
     "references right select set table top truncate union unique update values view where with returning window over partition "
     "begin commit rollback transaction grant revoke trigger procedure function replace if then else case when end",
     keyword, 0},
    {"bigint binary bit blob boolean char date datetime decimal double float int integer numeric real smallint text time timestamp "
     "tinyint uuid varbinary varchar json",
     type, 0},
    {"true false null", constant, 0},
};
constexpr WordIndex<512> sql_index = make_word_index<512>(sql_lists);
constexpr Grammar sql_grammar = make_grammar(GrammarSpec{.name = "sql",
                                                         .display_name = "SQL",
                                                         .extensions = ".sql",
                                                         .fold_case = true,
                                                         .line_comment = {"--"},
                                                         .block_open = "/*",
                                                         .block_close = "*/",
                                                         .strings = {StringRule{.quote = '\'', .multiline = true, .doubled = true},
                                                                     StringRule{.quote = '"', .doubled = true}},
                                                         .call_functions = true},
                                             sql_index.slots, 512);

// ---------------------------------------------------------------- Lua

constexpr WordList lua_lists[] = {
    {"and local not or in", keyword, 0},
    {"function", keyword, word_declares_function},
    {"if then else elseif end for while do repeat until break return goto", control, 0},
    {"true false nil self", constant, 0},
};
constexpr WordIndex<64> lua_index = make_word_index<64>(lua_lists);
constexpr Grammar lua_grammar = make_grammar(GrammarSpec{.name = "lua",
                                                         .display_name = "Lua",
                                                         .extensions = ".lua",
                                                         .line_comment = {"--"},
                                                         .block_open = "--[[",
                                                         .block_close = "]]",
                                                         .strings = {double_quoted, single_quoted},
                                                         .call_functions = true},
                                             lua_index.slots, 64);

// ---------------------------------------------------------------- JSON

constexpr WordList json_lists[] = {
    {"true false null", constant, 0},
};
constexpr WordIndex<8> json_index = make_word_index<8>(json_lists);
constexpr Grammar json_grammar = make_grammar(GrammarSpec{.name = "json",
                                                          .display_name = "JSON",
                                                          .aliases = "jsonc json5",
                                                          .extensions = ".json .jsonc .json5",
                                                          .line_comment = {"//"},
                                                          .block_open = "/*",
                                                          .block_close = "*/",
                                                          .strings = {double_quoted},
                                                          .string_keys = true},
                                              json_index.slots, 8);

// In the order of Language's values.
constexpr const Grammar* grammars[language_count] = {
    &plain_grammar, &c_grammar,     &cpp_grammar,   &csharp_grammar, &java_grammar, &javascript_grammar, &typescript_grammar, &go_grammar,
    &rust_grammar,  &swift_grammar, &kotlin_grammar, &python_grammar, &sql_grammar,  &lua_grammar,        &json_grammar,
};

[[nodiscard]] constexpr char folded(const char byte) noexcept {
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte;
}

// Whether `text` (count bytes), folded, is one of the space-separated words of `list`.
[[nodiscard]] bool listed(const TextView list, const char* const text, const usize count) noexcept {
    const char* const words = list.data();
    const usize total = list.count();
    usize i = 0;
    while (i < total) {
        while (i < total && words[i] == ' ') ++i;
        const usize start = i;
        while (i < total && words[i] != ' ') ++i;
        if (i - start != count || count == 0) continue;
        bool same = true;
        for (usize k = 0; k < count && same; ++k) same = folded(text[k]) == words[start + k];
        if (same) return true;
    }
    return false;
}

// The longest ending of `list` that `name` (folded) ends with; 0 for none.
[[nodiscard]] usize longest_ending(const TextView list, const char* const name, const usize count) noexcept {
    const char* const words = list.data();
    const usize total = list.count();
    usize best = 0;
    usize i = 0;
    while (i < total) {
        while (i < total && words[i] == ' ') ++i;
        const usize start = i;
        while (i < total && words[i] != ' ') ++i;
        const usize length = i - start;
        if (length == 0 || length > count || length <= best) continue;
        bool same = true;
        for (usize k = 0; k < length && same; ++k) same = folded(name[count - length + k]) == words[start + k];
        if (same) best = length;
    }
    return best;
}

} // namespace

const Grammar& grammar_of(const Language language) noexcept {
    const usize index = static_cast<usize>(language);
    return index < language_count ? *grammars[index] : plain_grammar;
}

} // namespace detail

TextView token_kind_name(const TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::plain: return "plain";
    case TokenKind::comment: return "comment";
    case TokenKind::documentation: return "documentation";
    case TokenKind::string: return "string";
    case TokenKind::character: return "character";
    case TokenKind::number: return "number";
    case TokenKind::keyword: return "keyword";
    case TokenKind::control: return "control";
    case TokenKind::type: return "type";
    case TokenKind::constant: return "constant";
    case TokenKind::function: return "function";
    case TokenKind::macro: return "macro";
    case TokenKind::directive: return "directive";
    case TokenKind::attribute: return "attribute";
    case TokenKind::property: return "property";
    case TokenKind::label: return "label";
    case TokenKind::operator_symbol: return "operator_symbol";
    case TokenKind::punctuation: return "punctuation";
    case TokenKind::identifier: return "identifier";
    case TokenKind::invalid: return "invalid";
    }
    return "unknown";
}

TextView language_name(const Language language) noexcept { return detail::grammar_of(language).spec.name; }

TextView language_display_name(const Language language) noexcept { return detail::grammar_of(language).spec.display_name; }

Language language_named(const View<char> name) noexcept {
    for (usize index = 0; index < language_count; ++index) {
        const detail::GrammarSpec& spec = (*detail::grammars[index]).spec;
        if (detail::listed(spec.name, name.data(), name.count()) || detail::listed(spec.aliases, name.data(), name.count())) {
            return static_cast<Language>(index);
        }
    }
    return Language::plain;
}

Language language_for_file_name(const View<char> file_name) noexcept {
    Language best = Language::plain;
    usize best_length = 0;
    for (usize index = 0; index < language_count; ++index) {
        const usize length = detail::longest_ending((*detail::grammars[index]).spec.extensions, file_name.data(), file_name.count());
        if (length > best_length) {
            best_length = length;
            best = static_cast<Language>(index);
        }
    }
    return best;
}

} // namespace stxlib::syntax
