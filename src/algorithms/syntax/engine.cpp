// stxlib::syntax's engine (include/stxlib/syntax.hpp): one lexer for every language, driven
// by the language's Grammar (grammar.hpp).
//
// A line is lexed token by token from the cursor. Every token is found whole before it is
// written, so the cursor always stands at a token boundary and a full buffer loses nothing.
//
// LineState layout: bits 0-3 the mode, bits 4-7 a rule (the string rule's index, or 1 for a
// documentation comment), bits 8-63 a payload:
//   block comment  the nesting depth;
//   string         1 when opened by three quotes;
//   C++ raw string the delimiter (delimiter_payload);
//   Rust raw string the number of #.
// Zero is ordinary code.

#include "grammar.hpp"

namespace stxlib::syntax {

namespace detail {

namespace {

enum class Mode : u8 {
    code = 0,
    block_comment = 1,
    line_comment = 2,
    string = 3,
    cpp_raw = 4,
    rust_raw = 5,
};

// LineCursor::context: what the last word asks of the next token; bit 7 says the line has
// had a token (a directive must be the first).
constexpr u8 expect_none = 0;
constexpr u8 expect_type = 1;
constexpr u8 expect_function = 2;
constexpr u8 expect_include = 3;
constexpr u8 expect_mask = 0x7f;
constexpr u8 line_started = 0x80;

[[nodiscard]] constexpr u64 pack(const Mode mode, const u8 rule, const u64 payload) noexcept {
    return static_cast<u64>(mode) | (static_cast<u64>(rule & 15u) << 4) | (payload << 8);
}
[[nodiscard]] constexpr Mode mode_of(const u64 state) noexcept { return static_cast<Mode>(state & 15u); }
[[nodiscard]] constexpr u8 rule_of(const u64 state) noexcept { return static_cast<u8>((state >> 4) & 15u); }
[[nodiscard]] constexpr u64 payload_of(const u64 state) noexcept { return state >> 8; }

// A C++ raw string's delimiter in 56 bits: its length in the low five, then the bytes
// themselves at seven bits each when there are at most seven (exact), otherwise 51 bits of
// their FNV-1a hash. Two delimiters over seven bytes with the same length and hash would be
// taken for each other; nothing else depends on the hash.
[[nodiscard]] u64 delimiter_payload(const char* const bytes, const usize length) noexcept {
    u64 payload = static_cast<u64>(length);
    if (length <= 7) {
        for (usize k = 0; k < length; ++k) payload = payload | (static_cast<u64>(static_cast<u8>(bytes[k]) & 0x7fu) << (5 + 7 * k));
        return payload;
    }
    u64 hash = 14695981039346656037ull;
    for (usize k = 0; k < length; ++k) hash = (hash ^ static_cast<u8>(bytes[k])) * 1099511628211ull;
    payload = payload | ((hash & ((u64{1} << 51) - 1)) << 5);
    return payload;
}

struct Scan final {
    const Grammar& grammar;
    const char* text;
    u32 count;
    u32 position;
    u64 state;
    u8 expect;
    bool started;
};

[[nodiscard]] ByteClass class_at(const Scan& scan, const u32 index) noexcept {
    return scan.grammar.classes[static_cast<u8>(scan.text[index])];
}

[[nodiscard]] bool starts_with(const Scan& scan, const u32 index, const TextView text) noexcept {
    const usize length = text.count();
    if (length == 0 || scan.count - index < length) return false;
    for (usize k = 0; k < length; ++k) {
        if (scan.text[index + k] != text.data()[k]) return false;
    }
    return true;
}

[[nodiscard]] constexpr char folded(const char byte) noexcept {
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte;
}

[[nodiscard]] const WordSlot* find_word(const Grammar& grammar, const char* const text, const usize length, const u32 hash) noexcept {
    if (grammar.slots == nullptr || length > 255) return nullptr;
    usize slot = hash & grammar.slot_mask;
    for (;;) {
        const WordSlot& candidate = grammar.slots[slot];
        if (candidate.length == 0) return nullptr;
        if (candidate.length == length) {
            bool same = true;
            for (usize k = 0; k < length && same; ++k) {
                const char byte = grammar.spec.fold_case ? folded(text[k]) : text[k];
                same = byte == candidate.text[k];
            }
            if (same) return &candidate;
        }
        slot = (slot + 1) & grammar.slot_mask;
    }
}

[[nodiscard]] u32 skip_spaces(const Scan& scan, u32 index) noexcept {
    while (index < scan.count && class_at(scan, index) == ByteClass::space) ++index;
    return index;
}

// The end of a block comment entered at `index` with `depth` open; writes the depth left
// (0: closed).
[[nodiscard]] u32 block_comment_end(const Scan& scan, u32 index, u64& depth) noexcept {
    const GrammarSpec& spec = scan.grammar.spec;
    while (index < scan.count) {
        if (starts_with(scan, index, spec.block_close)) {
            index = index + static_cast<u32>(spec.block_close.count());
            depth = depth - 1;
            if (depth == 0) return index;
            continue;
        }
        if (spec.block_nests && starts_with(scan, index, spec.block_open)) {
            index = index + static_cast<u32>(spec.block_open.count());
            depth = depth + 1;
            continue;
        }
        ++index;
    }
    return scan.count;
}

struct StringEnd final {
    u32 end{0};
    bool closed{false};
    // The line ended on an escape byte.
    bool spliced{false};
};

// The end of a string body entered at `index`.
[[nodiscard]] StringEnd string_end(const Scan& scan, u32 index, const StringRule& rule, const bool triple) noexcept {
    StringEnd result;
    while (index < scan.count) {
        const char byte = scan.text[index];
        if (rule.escape != 0 && byte == rule.escape) {
            if (index + 1 >= scan.count) {
                result.spliced = true;
                break;
            }
            index = index + 2;
            continue;
        }
        if (byte == rule.quote) {
            if (triple) {
                if (scan.count - index >= 3 && scan.text[index + 1] == rule.quote && scan.text[index + 2] == rule.quote) {
                    result.end = index + 3;
                    result.closed = true;
                    return result;
                }
                ++index;
                continue;
            }
            if (rule.doubled && index + 1 < scan.count && scan.text[index + 1] == rule.quote) {
                index = index + 2;
                continue;
            }
            result.end = index + 1;
            result.closed = true;
            return result;
        }
        ++index;
    }
    result.end = scan.count;
    return result;
}

// The end of a C++ raw string body entered at `index`: after )delimiter".
[[nodiscard]] StringEnd cpp_raw_end(const Scan& scan, u32 index, const u64 payload) noexcept {
    StringEnd result;
    const u32 length = static_cast<u32>(payload & 31u);
    while (index < scan.count) {
        if (scan.text[index] == ')' && scan.count - index >= length + 2 && scan.text[index + 1 + length] == '"' &&
            delimiter_payload(scan.text + index + 1, length) == payload) {
            result.end = index + length + 2;
            result.closed = true;
            return result;
        }
        ++index;
    }
    result.end = scan.count;
    return result;
}

// The end of a Rust raw string body entered at `index`: after " and `hashes` #.
[[nodiscard]] StringEnd rust_raw_end(const Scan& scan, u32 index, const u64 hashes) noexcept {
    StringEnd result;
    while (index < scan.count) {
        if (scan.text[index] == '"') {
            u64 found = 0;
            while (found < hashes && index + 1 + found < scan.count && scan.text[index + 1 + found] == '#') ++found;
            if (found == hashes) {
                result.end = index + 1 + static_cast<u32>(hashes);
                result.closed = true;
                return result;
            }
        }
        ++index;
    }
    result.end = scan.count;
    return result;
}

[[nodiscard]] u8 string_rule_index(const Grammar& grammar, const char quote) noexcept {
    for (u8 k = 0; k < string_rule_limit; ++k) {
        if (grammar.spec.strings[k].quote == quote) return k;
    }
    return 0;
}

void emit(LineToken& token, const u32 start, const u32 end, const TokenKind kind) noexcept {
    token.column = start;
    token.length = end - start;
    token.kind = kind;
}

// Lexes a string whose token starts at `start` (a prefix, or the quote) and whose opening
// quote is at `quote_at`.
void lex_string(Scan& scan, const u32 start, const u32 quote_at, LineToken& token) noexcept {
    const Grammar& grammar = scan.grammar;
    const u8 index = string_rule_index(grammar, scan.text[quote_at]);
    const StringRule& rule = grammar.spec.strings[index];
    const bool triple = rule.triple && scan.count - quote_at >= 3 && scan.text[quote_at + 1] == rule.quote && scan.text[quote_at + 2] == rule.quote;
    const u32 body = quote_at + (triple ? 3u : 1u);
    const StringEnd end = string_end(scan, body, rule, triple);
    TokenKind kind = rule.kind;
    scan.state = 0;
    if (!end.closed && (triple || rule.multiline || (end.spliced && grammar.spec.line_splice))) {
        scan.state = pack(Mode::string, index, triple ? 1 : 0);
    }
    if (end.closed && grammar.spec.string_keys) {
        const u32 after = skip_spaces(scan, end.end);
        if (after < scan.count && scan.text[after] == ':') kind = TokenKind::property;
    }
    emit(token, start, end.end, kind);
    scan.position = end.end;
}

// A quote at `position` that opens a character rule: a literal when it closes within a few
// bytes, a label where the language has them, otherwise one byte of punctuation.
void lex_character(Scan& scan, LineToken& token) noexcept {
    const Grammar& grammar = scan.grammar;
    const u32 start = scan.position;
    const StringRule& rule = grammar.spec.strings[string_rule_index(grammar, scan.text[start])];
    const StringEnd end = string_end(scan, start + 1, rule, false);
    const bool near = end.closed && end.end - start <= 12;
    const bool label_shaped = grammar.spec.quote_labels && start + 1 < scan.count && class_at(scan, start + 1) == ByteClass::word &&
                              !(near && end.end - start == 3);
    if (label_shaped) {
        u32 index = start + 1;
        while (index < scan.count && (class_at(scan, index) == ByteClass::word || class_at(scan, index) == ByteClass::digit)) ++index;
        // 'a' is a character; 'a (no quote after the name) is a label.
        if (!(index < scan.count && scan.text[index] == rule.quote)) {
            emit(token, start, index, TokenKind::label);
            scan.position = index;
            return;
        }
    }
    if (near) {
        emit(token, start, end.end, rule.kind);
        scan.position = end.end;
        return;
    }
    emit(token, start, start + 1, TokenKind::punctuation);
    scan.position = start + 1;
}

void lex_number(Scan& scan, LineToken& token) noexcept {
    const u32 start = scan.position;
    const char separator = scan.grammar.spec.number_separator;
    u32 index = start;
    bool hex = false;
    if (scan.text[index] == '0' && index + 1 < scan.count && (scan.text[index + 1] == 'x' || scan.text[index + 1] == 'X')) {
        hex = true;
        index = index + 2;
    }
    while (index < scan.count) {
        const char byte = scan.text[index];
        const ByteClass kind = class_at(scan, index);
        const bool has_next = index + 1 < scan.count;
        if (kind == ByteClass::digit || kind == ByteClass::word) {
            const bool exponent = hex ? (byte == 'p' || byte == 'P') : (byte == 'e' || byte == 'E');
            if (exponent && has_next && (scan.text[index + 1] == '+' || scan.text[index + 1] == '-')) {
                index = index + 2;
                continue;
            }
            ++index;
            continue;
        }
        if (byte == '.' && has_next && class_at(scan, index + 1) == ByteClass::digit) {
            ++index;
            continue;
        }
        if (separator != 0 && byte == separator && has_next &&
            (class_at(scan, index + 1) == ByteClass::digit || class_at(scan, index + 1) == ByteClass::word)) {
            ++index;
            continue;
        }
        break;
    }
    emit(token, start, index, TokenKind::number);
    scan.position = index;
}

// A raw prefix followed by what opens the language's raw string, at `after` (the byte
// after the prefix). True when a raw string was lexed.
[[nodiscard]] bool lex_raw_string(Scan& scan, const u32 start, const u32 after, LineToken& token) noexcept {
    const GrammarSpec& spec = scan.grammar.spec;
    if (spec.cpp_raw && after < scan.count && scan.text[after] == '"') {
        // R"delimiter( : at most 16 delimiter bytes, none of them a space, a parenthesis,
        // a backslash or a quote.
        u32 index = after + 1;
        while (index < scan.count && index - (after + 1) <= 16) {
            const char byte = scan.text[index];
            if (byte == '(') {
                const u64 payload = delimiter_payload(scan.text + after + 1, index - (after + 1));
                const StringEnd end = cpp_raw_end(scan, index + 1, payload);
                scan.state = end.closed ? 0 : pack(Mode::cpp_raw, 0, payload);
                emit(token, start, end.end, TokenKind::string);
                scan.position = end.end;
                return true;
            }
            if (byte == ' ' || byte == ')' || byte == '\\' || byte == '"' || byte == '\t') break;
            ++index;
        }
        emit(token, start, scan.count, TokenKind::invalid);
        scan.position = scan.count;
        return true;
    }
    if (spec.rust_raw && after < scan.count && (scan.text[after] == '"' || scan.text[after] == '#')) {
        u32 index = after;
        while (index < scan.count && scan.text[index] == '#') ++index;
        if (index < scan.count && scan.text[index] == '"') {
            const u64 hashes = index - after;
            const StringEnd end = rust_raw_end(scan, index + 1, hashes);
            scan.state = end.closed ? 0 : pack(Mode::rust_raw, 0, hashes);
            emit(token, start, end.end, TokenKind::string);
            scan.position = end.end;
            return true;
        }
    }
    return false;
}

void lex_word(Scan& scan, LineToken& token) noexcept {
    const Grammar& grammar = scan.grammar;
    const GrammarSpec& spec = grammar.spec;
    const u32 start = scan.position;
    u32 index = start;
    u32 hash = word_hash_seed;
    bool lower = false;
    while (index < scan.count) {
        const ByteClass kind = class_at(scan, index);
        if (kind != ByteClass::word && kind != ByteClass::digit) break;
        const char byte = scan.text[index];
        lower = lower || (byte >= 'a' && byte <= 'z');
        hash = word_hash_step(hash, static_cast<u8>(spec.fold_case ? folded(byte) : byte));
        ++index;
    }
    const u8 expected = scan.expect;
    scan.expect = expect_none;
    const WordSlot* word = find_word(grammar, scan.text + start, index - start, hash);
    // A directive's word (include) is a word of the table only after the directive byte.
    if (word != nullptr && ((*word).flags & word_include) != 0) word = nullptr;
    if (word != nullptr) {
        const u8 flags = (*word).flags;
        if ((flags & word_raw_prefix) != 0 && lex_raw_string(scan, start, index, token)) return;
        if ((flags & (word_string_prefix | word_raw_prefix)) != 0) {
            const bool quoted = index < scan.count && class_at(scan, index) == ByteClass::quote &&
                                !spec.strings[string_rule_index(grammar, scan.text[index])].character;
            if (quoted) {
                lex_string(scan, start, index, token);
                return;
            }
        }
        if ((*word).kind != TokenKind::string) {
            if ((flags & word_declares_type) != 0) scan.expect = expect_type;
            if ((flags & word_declares_function) != 0) scan.expect = expect_function;
            emit(token, start, index, (*word).kind);
            scan.position = index;
            return;
        }
        // A string prefix with no string after it is an ordinary name.
    }
    TokenKind kind = TokenKind::identifier;
    const char first = scan.text[start];
    const bool first_upper = first >= 'A' && first <= 'Z';
    const u32 after = skip_spaces(scan, index);
    if (expected == expect_type) {
        kind = TokenKind::type;
    } else if (expected == expect_function) {
        kind = TokenKind::function;
    } else if (spec.macro_bang && index < scan.count && scan.text[index] == '!' && !(index + 1 < scan.count && scan.text[index + 1] == '=')) {
        emit(token, start, index + 1, TokenKind::macro);
        scan.position = index + 1;
        return;
    } else if (spec.upper_constants && first_upper && !lower && index - start >= 2) {
        kind = TokenKind::constant;
    } else if (spec.calls_before_capitals && spec.call_functions && after < scan.count && scan.text[after] == '(') {
        kind = TokenKind::function;
    } else if (spec.capital_types && first_upper && lower) {
        kind = TokenKind::type;
    } else if (spec.suffix_t_types && index - start > 2 && scan.text[index - 2] == '_' && scan.text[index - 1] == 't') {
        kind = TokenKind::type;
    } else if (spec.call_functions && after < scan.count && scan.text[after] == '(') {
        kind = TokenKind::function;
    }
    emit(token, start, index, kind);
    scan.position = index;
}

// A directive as the line's first token: the byte, spaces, and the directive's word.
void lex_directive(Scan& scan, LineToken& token) noexcept {
    const u32 start = scan.position;
    u32 index = skip_spaces(scan, start + 1);
    const u32 word_start = index;
    u32 hash = word_hash_seed;
    while (index < scan.count && (class_at(scan, index) == ByteClass::word || class_at(scan, index) == ByteClass::digit)) {
        hash = word_hash_step(hash, static_cast<u8>(scan.text[index]));
        ++index;
    }
    const WordSlot* const word = find_word(scan.grammar, scan.text + word_start, index - word_start, hash);
    if (word != nullptr && ((*word).flags & word_include) != 0) scan.expect = expect_include;
    emit(token, start, index, TokenKind::directive);
    scan.position = index;
}

// Lexes the next token at or after the cursor. False when the line has no more.
[[nodiscard]] bool next_token(Scan& scan, LineToken& token) noexcept {
    const Grammar& grammar = scan.grammar;
    const GrammarSpec& spec = grammar.spec;
    const Mode mode = mode_of(scan.state);
    if (mode != Mode::code) {
        // A construct the line before left open: it runs from the line's start.
        if (scan.position >= scan.count && scan.count != 0) return false;
        const u32 start = scan.position;
        if (mode == Mode::block_comment) {
            u64 depth = payload_of(scan.state);
            const u8 rule = rule_of(scan.state);
            const u32 end = block_comment_end(scan, start, depth);
            scan.state = depth == 0 ? 0 : pack(Mode::block_comment, rule, depth);
            emit(token, start, end, rule == 1 ? TokenKind::documentation : TokenKind::comment);
            scan.position = end;
        } else if (mode == Mode::line_comment) {
            const u8 rule = rule_of(scan.state);
            const bool spliced = spec.line_splice && scan.count != 0 && scan.text[scan.count - 1] == '\\';
            scan.state = spliced ? scan.state : 0;
            emit(token, start, scan.count, rule == 1 ? TokenKind::documentation : TokenKind::comment);
            scan.position = scan.count;
        } else if (mode == Mode::string) {
            const u8 index = rule_of(scan.state);
            const StringRule& rule = spec.strings[index];
            const bool triple = payload_of(scan.state) == 1;
            const StringEnd end = string_end(scan, start, rule, triple);
            const bool open = !end.closed && (triple || rule.multiline || (end.spliced && spec.line_splice));
            scan.state = open ? scan.state : 0;
            emit(token, start, end.end, rule.kind);
            scan.position = end.end;
        } else if (mode == Mode::cpp_raw) {
            const StringEnd end = cpp_raw_end(scan, start, payload_of(scan.state));
            scan.state = end.closed ? 0 : scan.state;
            emit(token, start, end.end, TokenKind::string);
            scan.position = end.end;
        } else {
            const StringEnd end = rust_raw_end(scan, start, payload_of(scan.state));
            scan.state = end.closed ? 0 : scan.state;
            emit(token, start, end.end, TokenKind::string);
            scan.position = end.end;
        }
        scan.started = true;
        // An empty line inside an open construct has no token to show.
        return token.length != 0 || scan.position < scan.count;
    }
    scan.position = skip_spaces(scan, scan.position);
    if (scan.position >= scan.count) return false;
    const u32 start = scan.position;
    const char byte = scan.text[start];
    const bool first = !scan.started;
    scan.started = true;
    if (first && spec.directive != 0 && byte == spec.directive) {
        lex_directive(scan, token);
        return true;
    }
    if (scan.expect == expect_include && byte == '<') {
        u32 index = start + 1;
        while (index < scan.count && scan.text[index] != '>') ++index;
        if (index < scan.count) ++index;
        scan.expect = expect_none;
        emit(token, start, index, TokenKind::string);
        scan.position = index;
        return true;
    }
    if (starts_with(scan, start, spec.block_open)) {
        const u32 body = start + static_cast<u32>(spec.block_open.count());
        // /** and /*! are documentation; /**/ is an empty comment.
        const bool documentation = spec.documentation && body < scan.count && (scan.text[body] == '*' || scan.text[body] == '!') &&
                                   !(body + 1 < scan.count && scan.text[body + 1] == '/');
        const u8 rule = documentation ? 1 : 0;
        u64 depth = 1;
        const u32 end = block_comment_end(scan, body, depth);
        scan.state = depth == 0 ? 0 : pack(Mode::block_comment, rule, depth);
        scan.expect = expect_none;
        emit(token, start, end, documentation ? TokenKind::documentation : TokenKind::comment);
        scan.position = end;
        return true;
    }
    for (usize k = 0; k < 2; ++k) {
        if (!starts_with(scan, start, spec.line_comment[k])) continue;
        const u32 body = start + static_cast<u32>(spec.line_comment[k].count());
        // /// and //! are documentation; //// is a ruler.
        const bool documentation = spec.documentation && body < scan.count && (scan.text[body] == '/' || scan.text[body] == '!') &&
                                   !(body + 1 < scan.count && scan.text[body + 1] == '/');
        const bool spliced = spec.line_splice && scan.text[scan.count - 1] == '\\';
        scan.state = spliced ? pack(Mode::line_comment, documentation ? 1 : 0, 0) : 0;
        scan.expect = expect_none;
        emit(token, start, scan.count, documentation ? TokenKind::documentation : TokenKind::comment);
        scan.position = scan.count;
        return true;
    }
    const ByteClass kind = grammar.classes[static_cast<u8>(byte)];
    if (kind == ByteClass::word) {
        lex_word(scan, token);
        return true;
    }
    scan.expect = expect_none;
    if (kind == ByteClass::digit || (byte == '.' && start + 1 < scan.count && class_at(scan, start + 1) == ByteClass::digit)) {
        lex_number(scan, token);
        return true;
    }
    if (kind == ByteClass::quote) {
        if (spec.strings[string_rule_index(grammar, byte)].character) {
            lex_character(scan, token);
        } else {
            lex_string(scan, start, start, token);
        }
        return true;
    }
    if (spec.attribute != 0 && byte == spec.attribute && start + 1 < scan.count && class_at(scan, start + 1) == ByteClass::word) {
        u32 index = start + 1;
        while (index < scan.count && (class_at(scan, index) == ByteClass::word || class_at(scan, index) == ByteClass::digit)) ++index;
        emit(token, start, index, TokenKind::attribute);
        scan.position = index;
        return true;
    }
    if (spec.bracket_attribute && byte == '#' && start + 1 < scan.count &&
        (scan.text[start + 1] == '[' || (scan.text[start + 1] == '!' && start + 2 < scan.count && scan.text[start + 2] == '['))) {
        u32 index = start + 1;
        u32 depth = 0;
        while (index < scan.count) {
            if (scan.text[index] == '[') depth = depth + 1;
            if (scan.text[index] == ']') {
                depth = depth - 1;
                if (depth == 0) {
                    ++index;
                    break;
                }
            }
            ++index;
        }
        emit(token, start, index, TokenKind::attribute);
        scan.position = index;
        return true;
    }
    if (kind == ByteClass::punctuation) {
        u32 index = start + 1;
        while (index < scan.count && class_at(scan, index) == ByteClass::punctuation &&
               !(scan.text[index] == '.' && index + 1 < scan.count && class_at(scan, index + 1) == ByteClass::digit)) {
            ++index;
        }
        emit(token, start, index, TokenKind::punctuation);
        scan.position = index;
        return true;
    }
    if (kind == ByteClass::operator_symbol) {
        // A run of operator bytes, stopped where a comment begins inside it.
        u32 index = start + 1;
        while (index < scan.count && class_at(scan, index) == ByteClass::operator_symbol && !starts_with(scan, index, spec.block_open) &&
               !starts_with(scan, index, spec.line_comment[0]) && !starts_with(scan, index, spec.line_comment[1])) {
            ++index;
        }
        emit(token, start, index, TokenKind::operator_symbol);
        scan.position = index;
        return true;
    }
    // A byte the language gives no class: plain, one byte.
    token.length = 0;
    scan.position = start + 1;
    return true;
}

} // namespace

} // namespace detail

usize lex_line(const Language language, const View<char> line, LineCursor& cursor, const MutableView<LineToken> tokens) noexcept {
    if (cursor.done) return 0;
    const detail::Grammar& grammar = detail::grammar_of(language);
    if (language == Language::plain || line.count() > line_byte_limit) {
        cursor.position = static_cast<u32>(line.count() > line_byte_limit ? 0 : line.count());
        cursor.done = true;
        return 0;
    }
    detail::Scan scan{grammar,
                      line.data(),
                      static_cast<u32>(line.count()),
                      cursor.position,
                      cursor.state.value,
                      static_cast<u8>(cursor.context & detail::expect_mask),
                      (cursor.context & detail::line_started) != 0};
    usize written = 0;
    LineToken* const out = tokens.data();
    while (written < tokens.count()) {
        LineToken token;
        if (!detail::next_token(scan, token)) {
            cursor.done = true;
            break;
        }
        if (token.length != 0) {
            out[written] = token;
            written = written + 1;
        }
        // A line an open construct fills ends here; so does an empty line inside one.
        if (detail::mode_of(scan.state) != detail::Mode::code && scan.position >= scan.count) {
            cursor.done = true;
            break;
        }
    }
    cursor.position = scan.position;
    cursor.state.value = scan.state;
    cursor.context = static_cast<u8>(scan.expect | (scan.started ? detail::line_started : 0));
    return written;
}

LineState line_exit_state(const Language language, const View<char> line, const LineState entry) noexcept {
    LineCursor cursor = LineCursor::at_line_start(entry);
    LineToken scratch[32];
    while (!cursor.done) {
        const usize written = lex_line(language, line, cursor, MutableView<LineToken>(scratch, 32));
        (void)written;
    }
    return cursor.state;
}

} // namespace stxlib::syntax
