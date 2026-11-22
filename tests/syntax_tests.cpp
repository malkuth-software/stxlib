// Tests of stxlib::syntax (include/stxlib/syntax.hpp).
//
// - Token kinds on fixed lines of several languages, carried states included (raw strings
//   with delimiters, nested and spliced comments, triple-quoted strings).
// - A token buffer of one gives the tokens a large buffer gives.
// - The document under random edits against a document reset on the same text: the same
//   line count, line starts and entry states after every edit (sampled, not proved: the
//   seeds and counts are below).
// - The registry: names, aliases and file names.

#include "stxlib/syntax.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace syntax = stxlib::syntax;
using stxlib::u32;
using stxlib::u64;
using stxlib::usize;
using syntax::Language;
using syntax::LineState;
using syntax::LineToken;
using syntax::TokenKind;

stxlib::View<char> view_of(const std::string_view text) { return stxlib::View<char>(text.data(), text.size()); }

struct Lexed final {
    std::vector<LineToken> tokens;
    LineState exit;
};

Lexed lex(const Language language, const std::string_view line, const LineState entry, const usize buffer) {
    Lexed result;
    syntax::LineCursor cursor = syntax::LineCursor::at_line_start(entry);
    std::vector<LineToken> scratch(buffer);
    usize rounds = 0;
    while (!cursor.done) {
        const usize written = syntax::lex_line(language, view_of(line), cursor, stxlib::MutableView<LineToken>(scratch.data(), scratch.size()));
        for (usize k = 0; k < written; ++k) result.tokens.push_back(scratch[k]);
        rounds = rounds + 1;
        if (rounds > line.size() + 8) {
            CHECK(false);
            break;
        }
    }
    result.exit = cursor.state;
    return result;
}

// Whether `line` has a token whose bytes are `text` with kind `kind`.
bool has(const std::string_view line, const Lexed& lexed, const std::string_view text, const TokenKind kind) {
    for (const LineToken& token : lexed.tokens) {
        if (line.substr(token.column, token.length) == text && token.kind == kind) return true;
    }
    return false;
}

void report(const std::string_view line, const Lexed& lexed) {
    std::printf("  line: %.*s\n", static_cast<int>(line.size()), line.data());
    for (const LineToken& token : lexed.tokens) {
        const stxlib::TextView name = syntax::token_kind_name(token.kind);
        std::printf("    [%u,+%u] %.*s  '%.*s'\n", token.column, token.length, static_cast<int>(name.count()), name.data(),
                    static_cast<int>(token.length), line.data() + token.column);
    }
}

void expect(const Language language, const std::string_view line, const LineState entry, const std::string_view text, const TokenKind kind) {
    const Lexed lexed = lex(language, line, entry, 64);
    const bool found = has(line, lexed, text, kind);
    if (!found) report(line, lexed);
    CHECK(found);
}

void tokens_are_ordered(const std::string_view line, const Lexed& lexed) {
    u32 end = 0;
    for (const LineToken& token : lexed.tokens) {
        CHECK(token.length != 0);
        CHECK(token.column >= end);
        end = token.column + token.length;
        CHECK(end <= line.size());
    }
}

void cpp_kinds() {
    const Language cpp = Language::cpp;
    const LineState code;
    expect(cpp, "#include <vector>", code, "#include", TokenKind::directive);
    expect(cpp, "#include <vector>", code, "<vector>", TokenKind::string);
    expect(cpp, "  #  define LIMIT 10", code, "#  define", TokenKind::directive);
    expect(cpp, "  #  define LIMIT 10", code, "LIMIT", TokenKind::constant);
    expect(cpp, "template <typename Element> class Vector final {", code, "template", TokenKind::keyword);
    expect(cpp, "template <typename Element> class Vector final {", code, "Element", TokenKind::type);
    expect(cpp, "template <typename Element> class Vector final {", code, "Vector", TokenKind::type);
    expect(cpp, "    if (count == 0) return nullptr;", code, "if", TokenKind::control);
    expect(cpp, "    if (count == 0) return nullptr;", code, "return", TokenKind::control);
    expect(cpp, "    if (count == 0) return nullptr;", code, "nullptr", TokenKind::constant);
    expect(cpp, "    if (count == 0) return nullptr;", code, "==", TokenKind::operator_symbol);
    expect(cpp, "    const usize n = measure(text, 0x1F'FFu) + 1.5e-3;", code, "measure", TokenKind::function);
    expect(cpp, "    const usize n = measure(text, 0x1F'FFu) + 1.5e-3;", code, "0x1F'FFu", TokenKind::number);
    expect(cpp, "    const usize n = measure(text, 0x1F'FFu) + 1.5e-3;", code, "1.5e-3", TokenKind::number);
    expect(cpp, "    const usize n = measure(text, 0x1F'FFu) + 1.5e-3;", code, "usize", TokenKind::identifier);
    expect(cpp, "    size_t n = u8\"text\" [0] + 'x' + '\\n';", code, "size_t", TokenKind::type);
    expect(cpp, "    size_t n = u8\"text\" [0] + 'x' + '\\n';", code, "u8\"text\"", TokenKind::string);
    expect(cpp, "    size_t n = u8\"text\" [0] + 'x' + '\\n';", code, "'x'", TokenKind::character);
    expect(cpp, "    size_t n = u8\"text\" [0] + 'x' + '\\n';", code, "'\\n'", TokenKind::character);
    expect(cpp, "int x; /// the count", code, "/// the count", TokenKind::documentation);
    expect(cpp, "int x; //// ruler", code, "//// ruler", TokenKind::comment);
    expect(cpp, "a = b /* c */ + d; // e", code, "/* c */", TokenKind::comment);
    expect(cpp, "a = b /* c */ + d; // e", code, "// e", TokenKind::comment);
    expect(cpp, "x = a*/*c*/b;", code, "/*c*/", TokenKind::comment);

    // A block comment across lines, then code.
    Lexed open = lex(cpp, "int a; /* open", code, 64);
    CHECK(open.exit != code);
    Lexed middle = lex(cpp, "  still", open.exit, 64);
    CHECK(middle.tokens.size() == 1 && middle.tokens[0].kind == TokenKind::comment && middle.tokens[0].length == 7);
    CHECK(middle.exit == open.exit);
    Lexed empty = lex(cpp, "", open.exit, 64);
    CHECK(empty.tokens.empty() && empty.exit == open.exit);
    Lexed close = lex(cpp, "done */ int b;", open.exit, 64);
    CHECK(close.exit == code);
    CHECK(has("done */ int b;", close, "done */", TokenKind::comment));
    CHECK(has("done */ int b;", close, "int", TokenKind::type));

    // A raw string with a delimiter across lines; a near miss does not close it.
    const std::string_view raw0 = "auto s = R\"sql(SELECT \")\" FROM";
    Lexed raw = lex(cpp, raw0, code, 64);
    CHECK(raw.exit != code);
    CHECK(has(raw0, raw, "R\"sql(SELECT \")\" FROM", TokenKind::string));
    const std::string_view raw1 = "t WHERE x = ')sq\" )sql\" + 1;";
    Lexed raw_close = lex(cpp, raw1, raw.exit, 64);
    CHECK(raw_close.exit == code);
    CHECK(has(raw1, raw_close, "t WHERE x = ')sq\" )sql\"", TokenKind::string));
    CHECK(has(raw1, raw_close, "1", TokenKind::number));
    // A delimiter over seven bytes (hashed in the state).
    const std::string_view long0 = "x = R\"0123456789abcdef(body";
    Lexed long_raw = lex(cpp, long0, code, 64);
    CHECK(long_raw.exit != code);
    Lexed long_miss = lex(cpp, ")0123456789abcdeX\" no", long_raw.exit, 64);
    CHECK(long_miss.exit == long_raw.exit);
    Lexed long_close = lex(cpp, ")0123456789abcdef\"; y", long_raw.exit, 64);
    CHECK(long_close.exit == code);

    // A spliced line comment and a spliced string.
    Lexed spliced = lex(cpp, "// note \\", code, 64);
    CHECK(spliced.exit != code);
    Lexed after = lex(cpp, "still comment", spliced.exit, 64);
    CHECK(after.tokens.size() == 1 && after.tokens[0].kind == TokenKind::comment && after.exit == code);
    Lexed string_open = lex(cpp, "s = \"abc\\", code, 64);
    CHECK(string_open.exit != code);
    Lexed string_close = lex(cpp, "def\"; x", string_open.exit, 64);
    CHECK(string_close.exit == code && has("def\"; x", string_close, "def\"", TokenKind::string));
    // An unterminated string ends with its line.
    CHECK(lex(cpp, "s = \"abc", code, 64).exit == code);
}

void other_languages() {
    const LineState code;
    // Rust: nested comments, lifetimes, macros, attributes, raw strings.
    expect(Language::rust, "fn first<'a>(x: &'a str) -> Option<char> {", code, "fn", TokenKind::keyword);
    expect(Language::rust, "fn first<'a>(x: &'a str) -> Option<char> {", code, "first", TokenKind::function);
    expect(Language::rust, "fn first<'a>(x: &'a str) -> Option<char> {", code, "'a", TokenKind::label);
    expect(Language::rust, "fn first<'a>(x: &'a str) -> Option<char> {", code, "Option", TokenKind::type);
    expect(Language::rust, "fn first<'a>(x: &'a str) -> Option<char> {", code, "char", TokenKind::type);
    expect(Language::rust, "    println!(\"{}\", 'x'); #[derive(Debug)]", code, "println!", TokenKind::macro);
    expect(Language::rust, "    println!(\"{}\", 'x'); #[derive(Debug)]", code, "'x'", TokenKind::character);
    expect(Language::rust, "    println!(\"{}\", 'x'); #[derive(Debug)]", code, "#[derive(Debug)]", TokenKind::attribute);
    expect(Language::rust, "let s = r#\"a \"quoted\" b\"#; let n = 1..2;", code, "r#\"a \"quoted\" b\"#", TokenKind::string);
    expect(Language::rust, "let s = r#\"a \"quoted\" b\"#; let n = 1..2;", code, "1", TokenKind::number);
    Lexed nested = lex(Language::rust, "/* a /* b */ still", code, 64);
    CHECK(nested.exit != code);
    Lexed nested_close = lex(Language::rust, "c */ fn", nested.exit, 64);
    CHECK(nested_close.exit == code && has("c */ fn", nested_close, "fn", TokenKind::keyword));
    Lexed raw_open = lex(Language::rust, "let s = r##\"open", code, 64);
    CHECK(raw_open.exit != code);
    CHECK(lex(Language::rust, "not yet \"# more", raw_open.exit, 64).exit == raw_open.exit);
    CHECK(lex(Language::rust, "end\"## ;", raw_open.exit, 64).exit == code);

    // Python: triple-quoted strings, decorators, def and class.
    expect(Language::python, "@cache", code, "@cache", TokenKind::attribute);
    expect(Language::python, "def area(self, r: float) -> float:  # note", code, "def", TokenKind::keyword);
    expect(Language::python, "def area(self, r: float) -> float:  # note", code, "area", TokenKind::function);
    expect(Language::python, "def area(self, r: float) -> float:  # note", code, "float", TokenKind::type);
    expect(Language::python, "def area(self, r: float) -> float:  # note", code, "# note", TokenKind::comment);
    expect(Language::python, "x = rb'\\d+' if True else f\"{y}\"", code, "rb'\\d+'", TokenKind::string);
    expect(Language::python, "x = rb'\\d+' if True else f\"{y}\"", code, "True", TokenKind::constant);
    Lexed doc = lex(Language::python, "    \"\"\"Opens here", code, 64);
    CHECK(doc.exit != code);
    CHECK(lex(Language::python, "a \"quote\" inside", doc.exit, 64).exit == doc.exit);
    CHECK(lex(Language::python, "closes\"\"\" + 1", doc.exit, 64).exit == code);

    // JavaScript and TypeScript.
    expect(Language::javascript, "const f = async function load(url) { return `a ${url}`; }", code, "load", TokenKind::function);
    expect(Language::javascript, "const f = async function load(url) { return `a ${url}`; }", code, "`a ${url}`", TokenKind::string);
    expect(Language::typescript, "interface Shape { area(): number }", code, "Shape", TokenKind::type);
    expect(Language::typescript, "interface Shape { area(): number }", code, "number", TokenKind::type);
    CHECK(lex(Language::javascript, "x = `open", code, 64).exit != code);

    // JSON: keys are properties, values are strings.
    expect(Language::json, "{ \"name\": \"value\", \"n\": -1.5e3, \"ok\": true }", code, "\"name\"", TokenKind::property);
    expect(Language::json, "{ \"name\": \"value\", \"n\": -1.5e3, \"ok\": true }", code, "\"value\"", TokenKind::string);
    expect(Language::json, "{ \"name\": \"value\", \"n\": -1.5e3, \"ok\": true }", code, "true", TokenKind::constant);

    // SQL: words without regard to case, doubled quotes.
    expect(Language::sql, "SELECT name FROM t WHERE x = 'it''s' -- done", code, "SELECT", TokenKind::keyword);
    expect(Language::sql, "SELECT name FROM t WHERE x = 'it''s' -- done", code, "'it''s'", TokenKind::string);
    expect(Language::sql, "SELECT name FROM t WHERE x = 'it''s' -- done", code, "-- done", TokenKind::comment);

    // The rest: one line each.
    expect(Language::c, "static uint32_t count(struct node *n) { return n ? 1 : 0; }", code, "uint32_t", TokenKind::type);
    expect(Language::c, "static uint32_t count(struct node *n) { return n ? 1 : 0; }", code, "node", TokenKind::type);
    expect(Language::go, "func (s *Server) Run(ctx context.Context) error {", code, "func", TokenKind::keyword);
    expect(Language::go, "func (s *Server) Run(ctx context.Context) error {", code, "error", TokenKind::type);
    expect(Language::java, "@Override public String toString() { return NAME; }", code, "@Override", TokenKind::attribute);
    expect(Language::java, "@Override public String toString() { return NAME; }", code, "String", TokenKind::type);
    expect(Language::java, "@Override public String toString() { return NAME; }", code, "NAME", TokenKind::constant);
    expect(Language::csharp, "public void WriteLine(string s) => Console.WriteLine(s);", code, "WriteLine", TokenKind::function);
    expect(Language::csharp, "public void WriteLine(string s) => Console.WriteLine(s);", code, "Console", TokenKind::type);
    expect(Language::swift, "func draw(in rect: CGRect) -> Bool { return true }", code, "draw", TokenKind::function);
    expect(Language::swift, "func draw(in rect: CGRect) -> Bool { return true }", code, "CGRect", TokenKind::type);
    expect(Language::kotlin, "data class Point(val x: Int) { fun norm(): Double = TODO() }", code, "Point", TokenKind::type);
    expect(Language::kotlin, "data class Point(val x: Int) { fun norm(): Double = TODO() }", code, "norm", TokenKind::function);
    expect(Language::lua, "local function add(a, b) return a + b end -- sum", code, "add", TokenKind::function);
    expect(Language::lua, "local function add(a, b) return a + b end -- sum", code, "-- sum", TokenKind::comment);
    CHECK(lex(Language::lua, "--[[ open", code, 64).exit != code);
    CHECK(lex(Language::plain, "int x = 1;", code, 64).tokens.empty());
}

const char* const sample_lines[] = {
    "#include <vector>",
    "template <typename T> class Box final { /* a",
    "   comment */ T value{}; };",
    "const char* s = R\"x(raw \" text",
    "more )x\"; int y = 0x10 + 'c';",
    "// spliced \\",
    "comment still",
    "auto f(int a) -> int { return a * 2; } /// doc",
    "std::string t = \"quote \\\" inside\"; /* b */",
    "",
};

void small_buffers_agree() {
    const Language languages[] = {Language::cpp, Language::rust, Language::python, Language::javascript, Language::sql, Language::json};
    for (const Language language : languages) {
        LineState state;
        for (const char* const text : sample_lines) {
            const std::string_view line(text);
            const Lexed wide = lex(language, line, state, 64);
            const Lexed narrow = lex(language, line, state, 1);
            const Lexed three = lex(language, line, state, 3);
            tokens_are_ordered(line, wide);
            CHECK(wide.exit == narrow.exit && wide.exit == three.exit);
            CHECK(wide.tokens.size() == narrow.tokens.size() && wide.tokens.size() == three.tokens.size());
            for (usize k = 0; k < wide.tokens.size() && k < narrow.tokens.size(); ++k) {
                CHECK(wide.tokens[k].column == narrow.tokens[k].column && wide.tokens[k].length == narrow.tokens[k].length &&
                      wide.tokens[k].kind == narrow.tokens[k].kind);
            }
            CHECK(syntax::line_exit_state(language, view_of(line), state) == wide.exit);
            state = wide.exit;
        }
    }
}

struct Random final {
    u64 state = 0x9e3779b97f4a7c15ull;
    u32 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(state >> 33);
    }
};

syntax::Document made(const Language language) {
    stxlib::OperationResult<syntax::Document> result = syntax::Document::create(language);
    CHECK(result.succeeded());
    return static_cast<stxlib::OperationResult<syntax::Document>&&>(result).take_value();
}

// The document after edits against a document reset on the same text.
void same_as_reset(const syntax::Document& edited, const std::string& text, const Language language) {
    syntax::Document fresh = made(language);
    CHECK(fresh.reset(view_of(text)).succeeded());
    CHECK(edited.text_size() == text.size());
    CHECK(edited.line_count() == fresh.line_count());
    const u64 lines = fresh.line_count() < edited.line_count() ? fresh.line_count() : edited.line_count();
    for (u64 line = 0; line < lines; ++line) {
        CHECK(edited.line_start(line) == fresh.line_start(line));
        CHECK(edited.line_entry_state(line) == fresh.line_entry_state(line));
    }
}

void document_basics() {
    syntax::Document document = made(Language::cpp);
    CHECK(document.ready() && document.line_count() == 1 && document.text_size() == 0);
    const std::string text = "int a; /* open\r\nstill\nclose */ int b;\rlast";
    CHECK(document.reset(view_of(text)).succeeded());
    CHECK(document.line_count() == 4);
    CHECK(document.line_start(0) == 0 && document.line_start(1) == 16 && document.line_start(2) == 22 && document.line_start(3) == 38);
    CHECK(document.line_at(0) == 0 && document.line_at(15) == 0 && document.line_at(16) == 1 && document.line_at(37) == 2);
    CHECK(document.line_at(text.size()) == 3 && document.line_at(text.size() + 100) == 3);
    CHECK(document.line_entry_state(0) == LineState{} && document.line_entry_state(1) != LineState{});
    CHECK(document.line_entry_state(2) == document.line_entry_state(1) && document.line_entry_state(3) == LineState{});
    const syntax::LineExtent first = document.line_extent(view_of(text), 0);
    CHECK(first.start == 0 && first.length == 16 && first.terminator == 2);
    const syntax::LineExtent third = document.line_extent(view_of(text), 2);
    CHECK(third.length == 16 && third.terminator == 1);
    CHECK(document.line_extent(view_of(text), 3).terminator == 0);
    // A text that ends with a terminator has an empty last line.
    CHECK(document.reset(view_of("a\n")).succeeded());
    CHECK(document.line_count() == 2 && document.line_start(1) == 2);
    // Refusals change nothing.
    syntax::TextEdit wrong;
    wrong.start = 5;
    CHECK(document.apply_edit(view_of("a\n"), wrong).failed());
    CHECK(document.ready() && document.line_count() == 2);
    syntax::Document none;
    CHECK(none.reset(view_of("x")).failed() && !none.ready() && none.line_count() == 0);
}

void edits_converge() {
    // Typing inside one line of a long text lexes one or two lines.
    std::string text;
    for (usize k = 0; k < 2000; ++k) text += "int value = compute(k) + 1; // line\n";
    syntax::Document document = made(Language::cpp);
    CHECK(document.reset(view_of(text)).succeeded());
    const usize at = document.line_start(1000) + 4;
    text.insert(at, "x");
    syntax::TextEdit edit;
    edit.start = at;
    edit.inserted = 1;
    stxlib::OperationResult<syntax::Damage> damage = document.apply_edit(view_of(text), edit);
    CHECK(damage.succeeded());
    CHECK((*damage.value()).first_line == 1000 && (*damage.value()).line_count == 1);
    CHECK(document.counters().last_update_lines == 1);
    // Opening a comment re-lexes to the end; closing it converges again.
    text.insert(at, "/*");
    edit.inserted = 2;
    CHECK(document.apply_edit(view_of(text), edit).succeeded());
    CHECK(document.counters().last_update_lines == 1001);
    same_as_reset(document, text, Language::cpp);
    std::printf("document: %llu lines in %zu bytes of state (%.1f bytes a line)\n", static_cast<unsigned long long>(document.line_count()),
                document.storage_bytes(), static_cast<double>(document.storage_bytes()) / static_cast<double>(document.line_count()));
}

void random_edits() {
    const char* const pieces[] = {"/*", "*/", "\"", "\n", "\r\n", "\r", "//", "\\", "R\"ab(", ")ab\"", "int x = 1;", " ", "'", "#include <a>", "x"};
    const usize piece_count = sizeof(pieces) / sizeof(pieces[0]);
    const Language languages[] = {Language::cpp, Language::rust, Language::python};
    usize edits = 0;
    for (const Language language : languages) {
        Random random;
        random.state = random.state + static_cast<u64>(language);
        std::string text;
        for (const char* const line : sample_lines) {
            text += line;
            text += "\n";
        }
        syntax::Document document = made(language);
        CHECK(document.reset(view_of(text)).succeeded());
        for (usize round = 0; round < 1500; ++round) {
            syntax::TextEdit edit;
            edit.start = text.empty() ? 0 : random.next() % (text.size() + 1);
            const usize removable = text.size() - edit.start;
            edit.removed = random.next() % 3 == 0 ? (removable == 0 ? 0 : random.next() % (removable < 12 ? removable + 1 : 12)) : 0;
            std::string inserted;
            const usize count = random.next() % 3;
            for (usize k = 0; k < count; ++k) inserted += pieces[random.next() % piece_count];
            edit.inserted = inserted.size();
            text.replace(edit.start, edit.removed, inserted);
            CHECK(document.apply_edit(view_of(text), edit).succeeded());
            same_as_reset(document, text, language);
            edits = edits + 1;
        }
    }
    std::printf("random edits: %zu edits matched a fresh reset\n", edits);
}

void registry() {
    CHECK(syntax::language_named(view_of("cpp")) == Language::cpp);
    CHECK(syntax::language_named(view_of("C++")) == Language::cpp);
    CHECK(syntax::language_named(view_of("js")) == Language::javascript);
    CHECK(syntax::language_named(view_of("Py")) == Language::python);
    CHECK(syntax::language_named(view_of("nonesuch")) == Language::plain);
    CHECK(syntax::language_named(view_of("")) == Language::plain);
    CHECK(syntax::language_for_file_name(view_of("src/Main.CPP")) == Language::cpp);
    CHECK(syntax::language_for_file_name(view_of("vector.hpp")) == Language::cpp);
    CHECK(syntax::language_for_file_name(view_of("a.c")) == Language::c);
    CHECK(syntax::language_for_file_name(view_of("lib.rs")) == Language::rust);
    CHECK(syntax::language_for_file_name(view_of("types.d.ts")) == Language::typescript);
    CHECK(syntax::language_for_file_name(view_of("package.json")) == Language::json);
    CHECK(syntax::language_for_file_name(view_of("README")) == Language::plain);
    for (usize index = 0; index < syntax::language_count; ++index) {
        const Language language = static_cast<Language>(index);
        const stxlib::TextView name = syntax::language_name(language);
        CHECK(name.count() != 0 && syntax::language_display_name(language).count() != 0);
        CHECK(syntax::language_named(stxlib::View<char>(name.data(), name.count())) == language);
    }
    CHECK(syntax::token_kind_name(TokenKind::operator_symbol) == stxlib::TextView("operator_symbol"));
}

} // namespace

int main() {
    cpp_kinds();
    other_languages();
    small_buffers_agree();
    document_basics();
    edits_converge();
    random_edits();
    registry();
    return stxlib_tests::finish("stxlib syntax");
}
