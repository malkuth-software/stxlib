#pragma once

// stxlib::syntax: syntax highlighting for source text, beside the text layout and the glyph
// rasterizer. Three layers, each usable alone:
//
//   1. lex_line: the tokens of one line, entered in the state the line before left. No
//      allocation, no failure: the caller hands a token buffer of any size and calls again
//      until the cursor is done. A painter lexes exactly the lines it draws.
//   2. line_exit_state: the state a line leaves, for carrying comments and strings from one
//      line to the next.
//   3. Document: one entry state per line and the line starts, kept current under edits.
//      It stores no tokens: a line's tokens are lexed when they are drawn (a screen of code
//      is a few kilobytes), so a document costs about 12 bytes a line, and an edit re-lexes
//      from the edited line until a line is entered in the state it had before.
//
// Method. One table-driven engine serves every language: a language is constant data (byte
// classes, a perfect-enough hash of its words with their kinds, its comment and string forms,
// and a few naming conventions: a word before `(` is a function, a capitalized word is a
// type where the language's own convention says so). It is a lexical highlighter: it does
// not parse, so it does not tell a variable from a parameter. What it gives for that is
// speed that does not depend on the document's size, a state of 8 bytes a line, and a new
// language for a page of tables. (Compared with SwiftEdit's hand-written lexers, from which
// the line-state idea comes: the same incremental rule, one engine instead of one lexer per
// language, and line starts in STXlib's order-statistic tree instead of a vector that every
// edit shifts.)
//
// Text is bytes, expected UTF-8; malformed bytes are word bytes and never fail. Lines end at
// LF, CR or CRLF. Columns and lengths count bytes.
//
// Execution: lex_line and line_exit_state are pure functions of their arguments. A Document
// belongs to one thread at a time.

#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::syntax {

// What a token is. A presenter maps each kind to a style of its own and may index a table
// of token_kind_count entries by the kind's value; the order is part of that contract.
enum class TokenKind : u8 {
    plain = 0,        // text no other kind describes; also every gap between tokens
    comment,          // a comment
    documentation,    // a documentation comment (///, /** */, //!)
    string,           // a string literal, with its prefix and quotes
    character,        // a character literal
    number,           // a numeric literal with its suffix
    keyword,          // a declaration or modifier word (class, static, const)
    control,          // a control-flow word (if, for, return)
    type,             // a built-in type, or a name that is a type by declaration or convention
    constant,         // true, false, null; an ALL_CAPS name where that is the convention
    function,         // a name before `(`, or after the language's function word
    macro,            // a macro invocation (Rust's name!)
    directive,        // a preprocessor directive (#include)
    attribute,        // an annotation or decorator (@Override, #[derive])
    property,         // a key (a JSON object's member name)
    label,            // a label or lifetime ('a)
    operator_symbol,  // + - * / = < > and the rest
    punctuation,      // ( ) [ ] { } ; , .
    identifier,       // any other name
    invalid,          // text the language cannot contain
};
inline constexpr usize token_kind_count = 20;

// A stable lower-case name for the kind ("operator_symbol"), for themes and reports.
[[nodiscard]] TextView token_kind_name(TokenKind kind) noexcept;

// One token: the bytes [column, column + length) of one line, and its kind. Tokens of a
// line come in increasing column order without overlap.
struct LineToken final {
    u32 column{0};
    u32 length{0};
    TokenKind kind{TokenKind::plain};
};

// The lexer's state at a line boundary: zero in ordinary code, otherwise the open comment,
// string or raw string (with its delimiter). Only stored and compared by callers.
struct LineState final {
    u64 value{0};
    [[nodiscard]] constexpr bool operator==(const LineState& other) const noexcept { return value == other.value; }
    [[nodiscard]] constexpr bool operator!=(const LineState& other) const noexcept { return value != other.value; }
};

// The languages built in. Values are stable: settings may store them, though the name
// (language_name) is the better thing to store.
enum class Language : u8 {
    plain = 0,
    c,
    cpp,
    csharp,
    java,
    javascript,
    typescript,
    go,
    rust,
    swift,
    kotlin,
    python,
    sql,
    lua,
    json,
};
inline constexpr usize language_count = 15;

// The registry name ("cpp") and what a person reads in a menu ("C++").
[[nodiscard]] TextView language_name(Language language) noexcept;
[[nodiscard]] TextView language_display_name(Language language) noexcept;
// The language with that registry name, compared without regard to ASCII case (a Markdown
// fence's info string: "cpp", "c++", "js", "py" and the other common short names are
// accepted too); plain when none matches.
[[nodiscard]] Language language_named(View<char> name) noexcept;
// The language a file name selects by its extension, without regard to ASCII case; plain
// when none matches.
[[nodiscard]] Language language_for_file_name(View<char> file_name) noexcept;

// Where lexing of one line stands. Start a line with its entry state; lex_line advances it.
struct LineCursor final {
    // The state at `position`; once done, the line's exit state.
    LineState state{};
    u32 position{0};
    // Line-local context (what the last word asks of the next); callers leave it alone.
    u8 context{0};
    bool done{false};

    [[nodiscard]] static constexpr LineCursor at_line_start(const LineState entry) noexcept {
        LineCursor cursor;
        cursor.state = entry;
        return cursor;
    }
};

// The longest line that is lexed. A longer line gets no tokens and leaves its entry state.
inline constexpr usize line_byte_limit = usize{1} << 24;

// Lexes more of `line` (its bytes without the terminator) into `tokens` and returns how many
// were written. Call again with the same line and cursor until cursor.done; a buffer of one
// token is enough, 64 is a good size. Never fails; mutates only the cursor and the buffer.
[[nodiscard]] usize lex_line(Language language, View<char> line, LineCursor& cursor, MutableView<LineToken> tokens) noexcept;

// The state the line after `line` is entered in.
[[nodiscard]] LineState line_exit_state(Language language, View<char> line, LineState entry) noexcept;

namespace detail {
struct DocumentState;
} // namespace detail

// One replacement: bytes [start, start + removed) of the old text became `inserted` bytes
// at `start` of the new text.
struct TextEdit final {
    usize start{0};
    usize removed{0};
    usize inserted{0};
};

// The lines whose tokens may have changed: [first_line, first_line + line_count).
struct Damage final {
    u64 first_line{0};
    u64 line_count{0};
};

// A line's bytes in the document's text: [start, start + length), the terminator included
// in `length` and counted in `terminator` (0, 1 or 2 bytes).
struct LineExtent final {
    usize start{0};
    usize length{0};
    usize terminator{0};
};

// Work counters, for tests and instrumentation.
struct DocumentCounters final {
    u64 resets{0};
    u64 edits{0};
    u64 lines_lexed{0};
    u64 last_update_lines{0};
};

// A document's line starts and entry states, kept current under edits. It does not keep the
// text: each call that needs it borrows the caller's current text, and the caller keeps
// that text and the document in step (every change of the text is one apply_edit or a
// reset). Move-only; the default document is empty and every operation refuses it.
//
// Failure: invalid_argument and limit_exceeded are found before anything changes.
// out_of_memory during reset or apply_edit may leave the document without content
// (ready() false) until a reset succeeds; nothing is leaked.
class Document final {
public:
    // The largest text a document takes.
    static constexpr usize text_byte_limit = usize{1} << 30;

    Document() noexcept = default;
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;

    // An empty document (one empty line) in `language`.
    [[nodiscard]] static OperationResult<Document> create(Language language) noexcept;

    // Lexes every line's state from the start, replacing what was known.
    [[nodiscard]] OperationStatus reset(View<char> text) noexcept;
    [[nodiscard]] OperationStatus set_language(Language language, View<char> text) noexcept;
    // Brings the document in step with one edit; `text` is the whole new text.
    [[nodiscard]] OperationResult<Damage> apply_edit(View<char> text, const TextEdit& edit) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Language language() const noexcept;
    [[nodiscard]] usize text_size() const noexcept;
    // At least 1 when ready: an empty text is one empty line.
    [[nodiscard]] u64 line_count() const noexcept;
    // The line holding `offset`, O(log n); offsets past the end give the last line.
    [[nodiscard]] u64 line_at(usize offset) const noexcept;
    // The line's first byte, O(log n). Past the last line: the text's size.
    [[nodiscard]] usize line_start(u64 line) const noexcept;
    // The line's bytes in `text` (the document's current text).
    [[nodiscard]] LineExtent line_extent(View<char> text, u64 line) const noexcept;
    // The state the line is entered in; zero past the last line.
    [[nodiscard]] LineState line_entry_state(u64 line) const noexcept;
    // A cursor at the start of `line`, for lex_line over line_extent's bytes.
    [[nodiscard]] LineCursor line_cursor(u64 line) const noexcept;

    [[nodiscard]] DocumentCounters counters() const noexcept;
    [[nodiscard]] usize storage_bytes() const noexcept;

private:
    explicit Document(detail::DocumentState* state) noexcept : state_(state) {}
    void release() noexcept;
    detail::DocumentState* state_{nullptr};
};

} // namespace stxlib::syntax
