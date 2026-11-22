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
// Python f-string fields and C# interpolation holes (the strings are whole tokens); Swift
// raw strings; Lua long comments with = (--[==[); shell here-documents; trigraphs; the
// languages inside HTML's <script> and <style>.

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

[[nodiscard]] consteval GrammarSpec plain_spec() {
    GrammarSpec spec;
    spec.name = "plain";
    spec.display_name = "Plain text";
    spec.aliases = "text txt";
    spec.extensions = ".txt";
    return spec;
}
constexpr Grammar plain_grammar = make_grammar(plain_spec(), nullptr, 0);

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
[[nodiscard]] consteval GrammarSpec c_spec() {
    GrammarSpec spec;
    spec.name = "c";
    spec.display_name = "C";
    spec.extensions = ".c";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.line_splice = true;
    spec.documentation = true;
    spec.strings[0] = double_quoted;
    spec.strings[1] = character_quoted;
    spec.number_separator = '\'';
    spec.directive = '#';
    spec.upper_constants = true;
    spec.suffix_t_types = true;
    spec.call_functions = true;
    return spec;
}
constexpr Grammar c_grammar = make_grammar(c_spec(), c_index.slots, 256);

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
[[nodiscard]] consteval GrammarSpec cpp_spec() {
    GrammarSpec spec;
    spec.name = "cpp";
    spec.display_name = "C++";
    spec.aliases = "c++ cxx cc hpp";
    spec.extensions = ".cpp .hpp .cc .hh .cxx .hxx .h .inc .ipp .tpp .mm";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.line_splice = true;
    spec.documentation = true;
    spec.strings[0] = double_quoted;
    spec.strings[1] = character_quoted;
    spec.cpp_raw = true;
    spec.number_separator = '\'';
    spec.directive = '#';
    spec.capital_types = true;
    spec.upper_constants = true;
    spec.suffix_t_types = true;
    spec.call_functions = true;
    return spec;
}
constexpr Grammar cpp_grammar = make_grammar(cpp_spec(), cpp_index.slots, 512);

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
[[nodiscard]] consteval GrammarSpec csharp_spec() {
    GrammarSpec spec;
    spec.name = "csharp";
    spec.display_name = "C#";
    spec.aliases = "c# cs";
    spec.extensions = ".cs";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.documentation = true;
    spec.strings[0] = double_quoted;
    spec.strings[1] = character_quoted;
    spec.directive = '#';
    spec.capital_types = true;
    spec.call_functions = true;
    spec.calls_before_capitals = true;
    spec.strings[2] = StringRule{.quote = '"', .multiline = true, .doubled = true};
    spec.verbatim_rule = 3;
    return spec;
}
constexpr Grammar csharp_grammar = make_grammar(csharp_spec(), csharp_index.slots, 256);

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
[[nodiscard]] consteval GrammarSpec java_spec() {
    GrammarSpec spec;
    spec.name = "java";
    spec.display_name = "Java";
    spec.extensions = ".java";
    spec.word_extras = "$";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.documentation = true;
    spec.strings[0] = StringRule{.quote = '"', .escape = '\\', .triple = true};
    spec.strings[1] = character_quoted;
    spec.attribute = '@';
    spec.capital_types = true;
    spec.upper_constants = true;
    spec.call_functions = true;
    return spec;
}
constexpr Grammar java_grammar = make_grammar(java_spec(), java_index.slots, 256);

// ---------------------------------------------------------------- JavaScript and TypeScript

constexpr WordList javascript_lists[] = {
    {"async await const debugger delete export from get import in instanceof let new of set static super typeof var void with as", keyword, 0},
    {"class extends", keyword, word_declares_type},
    {"function", keyword, word_declares_function},
    {"if else for while do switch case default break continue return try catch finally throw yield", control, 0},
    {"true false null undefined NaN Infinity this globalThis", constant, 0},
};
constexpr WordIndex<128> javascript_index = make_word_index<128>(javascript_lists);
constexpr StringRule template_quoted{.quote = '`', .escape = '\\', .multiline = true, .interpolates = true};
[[nodiscard]] consteval GrammarSpec javascript_spec() {
    GrammarSpec spec;
    spec.name = "javascript";
    spec.display_name = "JavaScript";
    spec.aliases = "js jsx node mjs";
    spec.extensions = ".js .mjs .cjs .jsx";
    spec.word_extras = "$";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.documentation = true;
    spec.strings[0] = double_quoted;
    spec.strings[1] = single_quoted;
    spec.strings[2] = template_quoted;
    spec.attribute = '@';
    spec.capital_types = true;
    spec.upper_constants = true;
    spec.call_functions = true;
    spec.regex_literals = true;
    spec.interpolation_open = "${";
    spec.interpolation_close = '}';
    spec.interpolation_nest = '{';
    return spec;
}
constexpr Grammar javascript_grammar = make_grammar(javascript_spec(), javascript_index.slots, 128);

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
[[nodiscard]] consteval GrammarSpec typescript_spec() {
    GrammarSpec spec;
    spec.name = "typescript";
    spec.display_name = "TypeScript";
    spec.aliases = "ts tsx";
    spec.extensions = ".ts .tsx .mts .cts";
    spec.word_extras = "$";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.documentation = true;
    spec.strings[0] = double_quoted;
    spec.strings[1] = single_quoted;
    spec.strings[2] = template_quoted;
    spec.attribute = '@';
    spec.capital_types = true;
    spec.upper_constants = true;
    spec.call_functions = true;
    spec.regex_literals = true;
    spec.interpolation_open = "${";
    spec.interpolation_close = '}';
    spec.interpolation_nest = '{';
    return spec;
}
constexpr Grammar typescript_grammar = make_grammar(typescript_spec(), typescript_index.slots, 256);

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
[[nodiscard]] consteval GrammarSpec go_spec() {
    GrammarSpec spec;
    spec.name = "go";
    spec.display_name = "Go";
    spec.aliases = "golang";
    spec.extensions = ".go";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.strings[0] = double_quoted;
    spec.strings[1] = character_quoted;
    spec.strings[2] = StringRule{.quote = '`', .multiline = true};
    spec.call_functions = true;
    return spec;
}
constexpr Grammar go_grammar = make_grammar(go_spec(), go_index.slots, 128);

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
[[nodiscard]] consteval GrammarSpec rust_spec() {
    GrammarSpec spec;
    spec.name = "rust";
    spec.display_name = "Rust";
    spec.aliases = "rs";
    spec.extensions = ".rs";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.block_nests = true;
    spec.documentation = true;
    spec.strings[0] = StringRule{.quote = '"', .escape = '\\', .multiline = true};
    spec.strings[1] = character_quoted;
    spec.rust_raw = true;
    spec.bracket_attribute = true;
    spec.macro_bang = true;
    spec.quote_labels = true;
    spec.capital_types = true;
    spec.upper_constants = true;
    spec.call_functions = true;
    return spec;
}
constexpr Grammar rust_grammar = make_grammar(rust_spec(), rust_index.slots, 128);

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
[[nodiscard]] consteval GrammarSpec swift_spec() {
    GrammarSpec spec;
    spec.name = "swift";
    spec.display_name = "Swift";
    spec.extensions = ".swift";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.block_nests = true;
    spec.documentation = true;
    spec.strings[0] = StringRule{.quote = '"', .escape = '\\', .triple = true};
    spec.directive = '#';
    spec.attribute = '@';
    spec.capital_types = true;
    spec.call_functions = true;
    spec.strings[0].interpolates = true;
    spec.interpolation_open = "\\(";
    spec.interpolation_close = ')';
    spec.interpolation_nest = '(';
    return spec;
}
constexpr Grammar swift_grammar = make_grammar(swift_spec(), swift_index.slots, 256);

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
[[nodiscard]] consteval GrammarSpec kotlin_spec() {
    GrammarSpec spec;
    spec.name = "kotlin";
    spec.display_name = "Kotlin";
    spec.aliases = "kt kts";
    spec.extensions = ".kt .kts";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.block_nests = true;
    spec.documentation = true;
    spec.strings[0] = StringRule{.quote = '"', .escape = '\\', .triple = true};
    spec.strings[1] = character_quoted;
    spec.attribute = '@';
    spec.capital_types = true;
    spec.upper_constants = true;
    spec.call_functions = true;
    spec.strings[0].interpolates = true;
    spec.interpolation_open = "${";
    spec.interpolation_close = '}';
    spec.interpolation_nest = '{';
    return spec;
}
constexpr Grammar kotlin_grammar = make_grammar(kotlin_spec(), kotlin_index.slots, 256);

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
[[nodiscard]] consteval GrammarSpec python_spec() {
    GrammarSpec spec;
    spec.name = "python";
    spec.display_name = "Python";
    spec.aliases = "py python3";
    spec.extensions = ".py .pyi .pyw";
    spec.line_comment[0] = "#";
    spec.strings[0] = StringRule{.quote = '"', .escape = '\\', .triple = true};
    spec.strings[1] = StringRule{.quote = '\'', .escape = '\\', .triple = true};
    spec.attribute = '@';
    spec.capital_types = true;
    spec.upper_constants = true;
    spec.call_functions = true;
    return spec;
}
constexpr Grammar python_grammar = make_grammar(python_spec(), python_index.slots, 256);

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
[[nodiscard]] consteval GrammarSpec sql_spec() {
    GrammarSpec spec;
    spec.name = "sql";
    spec.display_name = "SQL";
    spec.extensions = ".sql";
    spec.fold_case = true;
    spec.line_comment[0] = "--";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.strings[0] = StringRule{.quote = '\'', .multiline = true, .doubled = true};
    spec.strings[1] = StringRule{.quote = '"', .doubled = true};
    spec.call_functions = true;
    return spec;
}
constexpr Grammar sql_grammar = make_grammar(sql_spec(), sql_index.slots, 512);

// ---------------------------------------------------------------- Lua

constexpr WordList lua_lists[] = {
    {"and local not or in", keyword, 0},
    {"function", keyword, word_declares_function},
    {"if then else elseif end for while do repeat until break return goto", control, 0},
    {"true false nil self", constant, 0},
};
constexpr WordIndex<64> lua_index = make_word_index<64>(lua_lists);
[[nodiscard]] consteval GrammarSpec lua_spec() {
    GrammarSpec spec;
    spec.name = "lua";
    spec.display_name = "Lua";
    spec.extensions = ".lua";
    spec.line_comment[0] = "--";
    spec.block_open = "--[[";
    spec.block_close = "]]";
    spec.strings[0] = double_quoted;
    spec.strings[1] = single_quoted;
    spec.call_functions = true;
    spec.long_brackets = true;
    return spec;
}
constexpr Grammar lua_grammar = make_grammar(lua_spec(), lua_index.slots, 64);

// ---------------------------------------------------------------- JSON

constexpr WordList json_lists[] = {
    {"true false null", constant, 0},
};
constexpr WordIndex<8> json_index = make_word_index<8>(json_lists);
[[nodiscard]] consteval GrammarSpec json_spec() {
    GrammarSpec spec;
    spec.name = "json";
    spec.display_name = "JSON";
    spec.aliases = "jsonc json5";
    spec.extensions = ".json .jsonc .json5";
    spec.line_comment[0] = "//";
    spec.block_open = "/*";
    spec.block_close = "*/";
    spec.strings[0] = double_quoted;
    spec.string_keys = true;
    return spec;
}
constexpr Grammar json_grammar = make_grammar(json_spec(), json_index.slots, 8);

// ---------------------------------------------------------------- shell

constexpr WordList shell_lists[] = {
    {"alias declare export function local readonly set shift source typeset unset eval exec trap in time", keyword, 0},
    {"if then else elif fi for while until do done case esac select return exit break continue", control, 0},
    {"true false", constant, 0},
};
constexpr WordIndex<128> shell_index = make_word_index<128>(shell_lists);
[[nodiscard]] consteval GrammarSpec shell_spec() {
    GrammarSpec spec;
    spec.name = "shell";
    spec.display_name = "Shell";
    spec.aliases = "sh bash zsh";
    spec.extensions = ".sh .bash .zsh";
    spec.line_comment[0] = "#";
    spec.strings[0] = StringRule{.quote = '"', .escape = '\\', .multiline = true};
    spec.strings[1] = StringRule{.quote = '\'', .multiline = true};
    spec.strings[2] = StringRule{.quote = '`', .escape = '\\', .multiline = true};
    spec.variable_sigil = '$';
    spec.comment_needs_space = true;
    return spec;
}
constexpr Grammar shell_grammar = make_grammar(shell_spec(), shell_index.slots, 128);

// ---------------------------------------------------------------- CSS

constexpr WordList css_lists[] = {
    {"auto none inherit initial unset revert normal transparent currentcolor true false on off bold italic solid dashed dotted hidden visible "
     "block inline flex grid absolute relative fixed sticky static center left right top bottom middle",
     constant, 0},
    {"and not only or from to", keyword, 0},
};
constexpr WordIndex<128> css_index = make_word_index<128>(css_lists);
[[nodiscard]] consteval GrammarSpec css_spec() {
    GrammarSpec spec;
    spec.family = Family::css;
    spec.name = "css";
    spec.display_name = "CSS";
    spec.aliases = "scss less";
    spec.extensions = ".css";
    spec.fold_case = true;
    return spec;
}
constexpr Grammar css_grammar = make_grammar(css_spec(), css_index.slots, 128);

// ---------------------------------------------------------------- XML and HTML

[[nodiscard]] consteval GrammarSpec xml_spec() {
    GrammarSpec spec;
    spec.family = Family::markup;
    spec.name = "xml";
    spec.display_name = "XML";
    spec.aliases = "svg xsd xsl plist form control";
    spec.extensions = ".xml .svg .xsd .xsl .xslt .plist .form .control .xaml .csproj .vcxproj";
    return spec;
}
constexpr Grammar xml_grammar = make_grammar(xml_spec(), nullptr, 0);

[[nodiscard]] consteval GrammarSpec html_spec() {
    GrammarSpec spec;
    spec.family = Family::markup;
    spec.name = "html";
    spec.display_name = "HTML";
    spec.aliases = "htm xhtml";
    spec.extensions = ".html .htm .xhtml";
    spec.html = true;
    return spec;
}
constexpr Grammar html_grammar = make_grammar(html_spec(), nullptr, 0);

// In the order of Language's values.
constexpr const Grammar* grammars[language_count] = {
    &plain_grammar, &c_grammar,     &cpp_grammar,   &csharp_grammar, &java_grammar, &javascript_grammar, &typescript_grammar, &go_grammar,
    &rust_grammar,  &swift_grammar, &kotlin_grammar, &python_grammar, &sql_grammar,  &lua_grammar,        &json_grammar,
    &shell_grammar, &css_grammar,   &xml_grammar,    &html_grammar,
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
    case TokenKind::regex: return "regex";
    case TokenKind::variable: return "variable";
    case TokenKind::tag: return "tag";
    case TokenKind::selector: return "selector";
    case TokenKind::color: return "color";
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
