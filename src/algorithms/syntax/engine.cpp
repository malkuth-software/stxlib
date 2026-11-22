// stxlib::syntax's C-like engine and the public line calls (include/stxlib/syntax.hpp): one
// lexer for every language of the family, driven by the language's Grammar (grammar.hpp).
//
// A line is lexed token by token from the cursor. Every token is found whole before it is
// written, so the cursor always stands at a token boundary and a full buffer loses nothing.
//
// LineState (scan.hpp) for this engine, by mode:
//   code           zero, or inside a string's interpolation: the rule is the string's, the
//                  payload's bit 0 says the string was opened by three quotes, bit 1 is set,
//                  and the bits above count the open nesting bytes plus one (zero: the
//                  opener itself is the next token);
//   block comment  the rule is 1 for documentation; the payload is the nesting depth;
//   line comment   continued by a backslash; the rule is 1 for documentation;
//   string         the rule is the string rule's index; the payload is 1 when opened by
//                  three quotes;
//   C++ raw string the payload is the delimiter (delimiter_payload);
//   Rust raw string the payload is the number of #;
//   long bracket   the payload is the number of = (Lua).
//
// One level of interpolation is carried. A string or comment left open inside an
// interpolation, or an interpolating string inside one, forgets the outer string: the
// lines after it are lexed as code until the next quote. That is a limit of the 64-bit
// state, stated here so nobody takes the nesting for exact.

#include "scan.hpp"

namespace stxlib::syntax {

namespace detail {

namespace {

constexpr u8 mode_code = 0;
constexpr u8 mode_block_comment = 1;
constexpr u8 mode_line_comment = 2;
constexpr u8 mode_string = 3;
constexpr u8 mode_cpp_raw = 4;
constexpr u8 mode_rust_raw = 5;
constexpr u8 mode_long_bracket = 6;

constexpr u8 expect_none = 0;
constexpr u8 expect_type = 1;
constexpr u8 expect_function = 2;
constexpr u8 expect_include = 3;

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

[[nodiscard]] bool word_byte(const Scan& scan, const u32 index) noexcept {
    const ByteClass kind = class_at(scan, index);
    return kind == ByteClass::word || kind == ByteClass::digit;
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
    // `end` is where an interpolation opens.
    bool interpolated{false};
};

// The end of a string body entered at `index`. `opener` is the interpolation opener, or
// empty.
[[nodiscard]] StringEnd string_end(const Scan& scan, u32 index, const StringRule& rule, const bool triple, const TextView opener) noexcept {
    StringEnd result;
    while (index < scan.count) {
        const char byte = scan.text[index];
        if (opener.count() != 0 && byte == opener.data()[0] && starts_with(scan, index, opener)) {
            result.end = index;
            result.interpolated = true;
            return result;
        }
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

// The end of a body closed by `close`, `level` bytes of `fill`, and (when `second` is not
// zero) `second`: Rust's "## and Lua's ]==].
[[nodiscard]] StringEnd filled_end(const Scan& scan, u32 index, const char close, const char fill, const u64 level, const char second) noexcept {
    StringEnd result;
    while (index < scan.count) {
        if (scan.text[index] == close) {
            u64 found = 0;
            while (found < level && index + 1 + found < scan.count && scan.text[index + 1 + found] == fill) ++found;
            const u32 after = index + 1 + static_cast<u32>(found);
            if (found == level && (second == 0 || (after < scan.count && scan.text[after] == second))) {
                result.end = after + (second == 0 ? 0u : 1u);
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

// Lexes a string's body from `body`; the token starts at `start` (a prefix, the quote, or
// where the line or an interpolation left the string).
void finish_string(Scan& scan, const u32 start, const u32 body, const u8 index, const bool triple, LineToken& token) noexcept {
    const GrammarSpec& spec = scan.grammar.spec;
    const StringRule& rule = spec.strings[index];
    const TextView opener = rule.interpolates ? spec.interpolation_open : TextView();
    const StringEnd end = string_end(scan, body, rule, triple, opener);
    TokenKind kind = rule.kind;
    scan.state = 0;
    if (end.interpolated) {
        scan.state = pack_state(mode_code, index, (triple ? 1u : 0u) | 2u);
    } else if (!end.closed && (triple || rule.multiline || (end.spliced && spec.line_splice))) {
        scan.state = pack_state(mode_string, index, triple ? 1 : 0);
    }
    if (end.closed && spec.string_keys) {
        const u32 after = skip_spaces(scan, end.end);
        if (after < scan.count && scan.text[after] == ':') kind = TokenKind::property;
    }
    take(scan, token, start, end.end, kind);
}

// Lexes a string whose token starts at `start` and whose opening quote is at `quote_at`.
void lex_string(Scan& scan, const u32 start, const u32 quote_at, const u8 index, LineToken& token) noexcept {
    const StringRule& rule = scan.grammar.spec.strings[index];
    const bool triple = rule.triple && scan.count - quote_at >= 3 && scan.text[quote_at + 1] == rule.quote && scan.text[quote_at + 2] == rule.quote;
    finish_string(scan, start, quote_at + (triple ? 3u : 1u), index, triple, token);
}

// A quote at `position` that opens a character rule: a literal when it closes within a few
// bytes, a label where the language has them, otherwise one byte of punctuation.
void lex_character(Scan& scan, LineToken& token) noexcept {
    const Grammar& grammar = scan.grammar;
    const u32 start = scan.position;
    const StringRule& rule = grammar.spec.strings[string_rule_index(grammar, scan.text[start])];
    const StringEnd end = string_end(scan, start + 1, rule, false, TextView());
    const bool near = end.closed && end.end - start <= 12;
    // 'a' is a character; 'a with no quote after the name is a label.
    const bool label_shaped = grammar.spec.quote_labels && start + 1 < scan.count && class_at(scan, start + 1) == ByteClass::word &&
                              !(near && end.end - start == 3);
    if (label_shaped) {
        u32 index = start + 1;
        while (index < scan.count && word_byte(scan, index)) ++index;
        if (!(index < scan.count && scan.text[index] == rule.quote)) {
            take(scan, token, start, index, TokenKind::label);
            return;
        }
    }
    if (near) {
        take(scan, token, start, end.end, rule.kind);
        return;
    }
    take(scan, token, start, start + 1, TokenKind::punctuation);
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
        const bool has_next = index + 1 < scan.count;
        if (word_byte(scan, index)) {
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
        if (separator != 0 && byte == separator && has_next && word_byte(scan, index + 1)) {
            ++index;
            continue;
        }
        break;
    }
    take(scan, token, start, index, TokenKind::number);
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
                scan.state = end.closed ? 0 : pack_state(mode_cpp_raw, 0, payload);
                take(scan, token, start, end.end, TokenKind::string);
                return true;
            }
            if (byte == ' ' || byte == ')' || byte == '\\' || byte == '"' || byte == '\t') break;
            ++index;
        }
        take(scan, token, start, scan.count, TokenKind::invalid);
        return true;
    }
    if (spec.rust_raw && after < scan.count && (scan.text[after] == '"' || scan.text[after] == '#')) {
        u32 index = after;
        while (index < scan.count && scan.text[index] == '#') ++index;
        if (index < scan.count && scan.text[index] == '"') {
            const u64 hashes = index - after;
            const StringEnd end = filled_end(scan, index + 1, '"', '#', hashes, 0);
            scan.state = end.closed ? 0 : pack_state(mode_rust_raw, 0, hashes);
            take(scan, token, start, end.end, TokenKind::string);
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
    while (index < scan.count && word_byte(scan, index)) {
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
        if ((flags & (word_string_prefix | word_raw_prefix)) != 0 && index < scan.count && class_at(scan, index) == ByteClass::quote) {
            const u8 rule = string_rule_index(grammar, scan.text[index]);
            if (!spec.strings[rule].character) {
                lex_string(scan, start, index, rule, token);
                return;
            }
        }
        if ((*word).kind != TokenKind::string) {
            if ((flags & word_declares_type) != 0) scan.expect = expect_type;
            if ((flags & word_declares_function) != 0) scan.expect = expect_function;
            take(scan, token, start, index, (*word).kind);
            return;
        }
        // A string prefix with no string after it is an ordinary name.
    }
    TokenKind kind = TokenKind::identifier;
    const char first = scan.text[start];
    const bool first_upper = first >= 'A' && first <= 'Z';
    const u32 after = skip_spaces(scan, index);
    const bool called = spec.call_functions && after < scan.count && scan.text[after] == '(';
    if (expected == expect_type) {
        kind = TokenKind::type;
    } else if (expected == expect_function) {
        kind = TokenKind::function;
    } else if (spec.macro_bang && index < scan.count && scan.text[index] == '!' && !(index + 1 < scan.count && scan.text[index + 1] == '=')) {
        take(scan, token, start, index + 1, TokenKind::macro);
        return;
    } else if (spec.upper_constants && first_upper && !lower && index - start >= 2) {
        kind = TokenKind::constant;
    } else if (spec.calls_before_capitals && called) {
        kind = TokenKind::function;
    } else if (spec.capital_types && first_upper && lower) {
        kind = TokenKind::type;
    } else if (spec.suffix_t_types && index - start > 2 && scan.text[index - 2] == '_' && scan.text[index - 1] == 't') {
        kind = TokenKind::type;
    } else if (called) {
        kind = TokenKind::function;
    }
    take(scan, token, start, index, kind);
}

// A directive as the line's first token: the byte, spaces, and the directive's word.
void lex_directive(Scan& scan, LineToken& token) noexcept {
    const u32 start = scan.position;
    const u32 word_start = skip_spaces(scan, start + 1);
    u32 index = word_start;
    while (index < scan.count && word_byte(scan, index)) ++index;
    const WordSlot* const word = find_word_at(scan, word_start, index);
    if (word != nullptr && ((*word).flags & word_include) != 0) scan.expect = expect_include;
    take(scan, token, start, index, TokenKind::directive);
}

// A regular-expression literal at `position` (a '/'): true when one closes on this line.
[[nodiscard]] bool lex_regex(Scan& scan, LineToken& token) noexcept {
    const u32 start = scan.position;
    u32 index = start + 1;
    bool in_class = false;
    while (index < scan.count) {
        const char byte = scan.text[index];
        if (byte == '\\') {
            index = index + 2;
            continue;
        }
        if (byte == '[') in_class = true;
        if (byte == ']') in_class = false;
        if (byte == '/' && !in_class) {
            ++index;
            while (index < scan.count && class_at(scan, index) == ByteClass::word) ++index;
            take(scan, token, start, index, TokenKind::regex);
            return true;
        }
        ++index;
    }
    return false;
}

// A variable at `position` (the sigil): true when a name follows.
[[nodiscard]] bool lex_variable(Scan& scan, LineToken& token) noexcept {
    const u32 start = scan.position;
    if (start + 1 >= scan.count) return false;
    const char next = scan.text[start + 1];
    u32 index = start + 1;
    if (next == '{') {
        while (index < scan.count && scan.text[index] != '}') ++index;
        if (index < scan.count) ++index;
    } else if (word_byte(scan, index)) {
        while (index < scan.count && word_byte(scan, index)) ++index;
    } else if (next == '#' || next == '?' || next == '@' || next == '*' || next == '$' || next == '!' || next == '-') {
        index = start + 2;
    } else {
        return false;
    }
    take(scan, token, start, index, TokenKind::variable);
    return true;
}

// The continuation of a construct the line before (or an interpolation) left open, from the
// scan's position. False when the line has nothing for it.
[[nodiscard]] bool open_construct_token(Scan& scan, LineToken& token) noexcept {
    const GrammarSpec& spec = scan.grammar.spec;
    if (scan.position >= scan.count && scan.count != 0) return false;
    const u32 start = scan.position;
    const u8 mode = state_mode(scan.state);
    const u8 rule = state_rule(scan.state);
    const u64 payload = state_payload(scan.state);
    if (mode == mode_block_comment) {
        u64 depth = payload;
        const u32 end = block_comment_end(scan, start, depth);
        scan.state = depth == 0 ? 0 : pack_state(mode_block_comment, rule, depth);
        take(scan, token, start, end, rule == 1 ? TokenKind::documentation : TokenKind::comment);
    } else if (mode == mode_line_comment) {
        const bool spliced = spec.line_splice && scan.count != 0 && scan.text[scan.count - 1] == '\\';
        scan.state = spliced ? scan.state : 0;
        take(scan, token, start, scan.count, rule == 1 ? TokenKind::documentation : TokenKind::comment);
    } else if (mode == mode_string) {
        finish_string(scan, start, start, rule, payload == 1, token);
    } else if (mode == mode_cpp_raw) {
        const StringEnd end = cpp_raw_end(scan, start, payload);
        scan.state = end.closed ? 0 : scan.state;
        take(scan, token, start, end.end, TokenKind::string);
    } else if (mode == mode_rust_raw) {
        const StringEnd end = filled_end(scan, start, '"', '#', payload, 0);
        scan.state = end.closed ? 0 : scan.state;
        take(scan, token, start, end.end, TokenKind::string);
    } else {
        const StringEnd end = filled_end(scan, start, ']', '=', payload, ']');
        scan.state = end.closed ? 0 : scan.state;
        take(scan, token, start, end.end, TokenKind::string);
    }
    scan.started = true;
    scan.value_before = true;
    // An empty line inside an open construct has no token to show.
    return token.length != 0 || scan.position < scan.count;
}

// The next token in code.
[[nodiscard]] bool code_token(Scan& scan, LineToken& token) noexcept {
    const Grammar& grammar = scan.grammar;
    const GrammarSpec& spec = grammar.spec;
    scan.position = skip_spaces(scan, scan.position);
    if (scan.position >= scan.count) return false;
    const u32 start = scan.position;
    const char byte = scan.text[start];
    const bool first = !scan.started;
    scan.started = true;
    if (scan.state != 0) {
        // Inside an interpolation: its opener, and the bytes that nest and close it.
        const u8 rule = state_rule(scan.state);
        const u64 payload = state_payload(scan.state);
        const u64 depth = payload >> 2;
        if (depth == 0) {
            scan.state = pack_state(mode_code, rule, (payload & 3u) | (u64{1} << 2));
            take(scan, token, start, start + static_cast<u32>(spec.interpolation_open.count()), TokenKind::punctuation);
            return true;
        }
        if (byte == spec.interpolation_nest) {
            scan.state = pack_state(mode_code, rule, (payload & 3u) | ((depth + 1) << 2));
            take(scan, token, start, start + 1, TokenKind::punctuation);
            return true;
        }
        if (byte == spec.interpolation_close) {
            scan.state = depth == 1 ? pack_state(mode_string, rule, payload & 1u) : pack_state(mode_code, rule, (payload & 3u) | ((depth - 1) << 2));
            take(scan, token, start, start + 1, TokenKind::punctuation);
            return true;
        }
    }
    if (first && spec.directive != 0 && byte == spec.directive) {
        lex_directive(scan, token);
        return true;
    }
    if (scan.expect == expect_include && byte == '<') {
        u32 index = start + 1;
        while (index < scan.count && scan.text[index] != '>') ++index;
        if (index < scan.count) ++index;
        scan.expect = expect_none;
        take(scan, token, start, index, TokenKind::string);
        return true;
    }
    if (starts_with(scan, start, spec.block_open)) {
        const u32 body = start + static_cast<u32>(spec.block_open.count());
        // /** and /*! are documentation; /**/ is an empty comment.
        const bool documentation = spec.documentation && body < scan.count && (scan.text[body] == '*' || scan.text[body] == '!') &&
                                   !(body + 1 < scan.count && scan.text[body + 1] == '/');
        u64 depth = 1;
        const u32 end = block_comment_end(scan, body, depth);
        scan.state = depth == 0 ? 0 : pack_state(mode_block_comment, documentation ? 1 : 0, depth);
        scan.expect = expect_none;
        take(scan, token, start, end, documentation ? TokenKind::documentation : TokenKind::comment);
        return true;
    }
    const bool comment_allowed = !spec.comment_needs_space || start == 0 || class_at(scan, start - 1) == ByteClass::space;
    for (usize k = 0; k < 2 && comment_allowed; ++k) {
        if (!starts_with(scan, start, spec.line_comment[k])) continue;
        const u32 body = start + static_cast<u32>(spec.line_comment[k].count());
        // /// and //! are documentation; //// is a ruler.
        const bool documentation = spec.documentation && body < scan.count && (scan.text[body] == '/' || scan.text[body] == '!') &&
                                   !(body + 1 < scan.count && scan.text[body + 1] == '/');
        const bool spliced = spec.line_splice && scan.text[scan.count - 1] == '\\';
        scan.state = spliced ? pack_state(mode_line_comment, documentation ? 1 : 0, 0) : 0;
        scan.expect = expect_none;
        take(scan, token, start, scan.count, documentation ? TokenKind::documentation : TokenKind::comment);
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
    if (spec.verbatim_rule != 0 && (byte == '@' || byte == '$')) {
        // @"..." , $"..." and both together: verbatim when an @ is among them.
        u32 index = start;
        bool verbatim = false;
        while (index < scan.count && index - start < 2 && (scan.text[index] == '@' || scan.text[index] == '$')) {
            verbatim = verbatim || scan.text[index] == '@';
            ++index;
        }
        if (index < scan.count && scan.text[index] == '"') {
            lex_string(scan, start, index, verbatim ? static_cast<u8>(spec.verbatim_rule - 1) : string_rule_index(grammar, '"'), token);
            return true;
        }
    }
    if (kind == ByteClass::quote) {
        const u8 rule = string_rule_index(grammar, byte);
        if (spec.strings[rule].character) {
            lex_character(scan, token);
        } else {
            lex_string(scan, start, start, rule, token);
        }
        return true;
    }
    if (spec.variable_sigil != 0 && byte == spec.variable_sigil && lex_variable(scan, token)) return true;
    if (spec.attribute != 0 && byte == spec.attribute && start + 1 < scan.count && class_at(scan, start + 1) == ByteClass::word) {
        u32 index = start + 1;
        while (index < scan.count && word_byte(scan, index)) ++index;
        take(scan, token, start, index, TokenKind::attribute);
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
        take(scan, token, start, index, TokenKind::attribute);
        return true;
    }
    if (spec.long_brackets && byte == '[') {
        u32 index = start + 1;
        while (index < scan.count && scan.text[index] == '=') ++index;
        if (index < scan.count && scan.text[index] == '[') {
            const u64 level = index - (start + 1);
            const StringEnd end = filled_end(scan, index + 1, ']', '=', level, ']');
            scan.state = end.closed ? 0 : pack_state(mode_long_bracket, 0, level);
            take(scan, token, start, end.end, TokenKind::string);
            return true;
        }
    }
    if (spec.regex_literals && byte == '/' && !scan.value_before && lex_regex(scan, token)) return true;
    if (kind == ByteClass::punctuation) {
        // A run of punctuation; inside an interpolation its nesting bytes stand alone.
        const bool interpolating = scan.state != 0;
        u32 index = start + 1;
        while (index < scan.count && class_at(scan, index) == ByteClass::punctuation &&
               !(scan.text[index] == '.' && index + 1 < scan.count && class_at(scan, index + 1) == ByteClass::digit) &&
               !(interpolating && (scan.text[index] == spec.interpolation_nest || scan.text[index] == spec.interpolation_close))) {
            ++index;
        }
        take(scan, token, start, index, TokenKind::punctuation);
        return true;
    }
    if (kind == ByteClass::operator_symbol) {
        // A run of operator bytes, stopped where a comment begins inside it.
        u32 index = start + 1;
        while (index < scan.count && class_at(scan, index) == ByteClass::operator_symbol && !starts_with(scan, index, spec.block_open) &&
               !starts_with(scan, index, spec.line_comment[0]) && !starts_with(scan, index, spec.line_comment[1])) {
            ++index;
        }
        take(scan, token, start, index, TokenKind::operator_symbol);
        return true;
    }
    // A byte the language gives no class: plain, one byte.
    token.length = 0;
    scan.position = start + 1;
    return true;
}

// Whether a token of this kind, ending in `last`, can end a value (so a '/' after it
// divides, and does not open a regular expression).
[[nodiscard]] bool ends_value(const TokenKind kind, const char last) noexcept {
    switch (kind) {
    case TokenKind::identifier:
    case TokenKind::type:
    case TokenKind::constant:
    case TokenKind::function:
    case TokenKind::number:
    case TokenKind::string:
    case TokenKind::character:
    case TokenKind::property:
    case TokenKind::variable:
    case TokenKind::regex:
        return true;
    case TokenKind::punctuation:
        return last == ')' || last == ']';
    default:
        return false;
    }
}

} // namespace

bool c_like_token(Scan& scan, LineToken& token) noexcept {
    token.length = 0;
    if (state_mode(scan.state) != mode_code) return open_construct_token(scan, token);
    const u64 outer = scan.state;
    if (!code_token(scan, token)) return false;
    // A construct that opened and closed inside an interpolation returns to it.
    if (outer != 0 && scan.state == 0) scan.state = outer;
    if (token.length != 0) scan.value_before = ends_value(token.kind, scan.text[token.column + token.length - 1]);
    return true;
}

} // namespace detail

namespace {

constexpr u8 context_expect_mask = 0x3f;
constexpr u8 context_value_before = 0x40;
constexpr u8 context_started = 0x80;

} // namespace

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
                      static_cast<u8>(cursor.context & context_expect_mask),
                      (cursor.context & context_started) != 0,
                      (cursor.context & context_value_before) != 0};
    const detail::Family family = grammar.spec.family;
    usize written = 0;
    LineToken* const out = tokens.data();
    while (written < tokens.count()) {
        LineToken token;
        bool more = false;
        if (family == detail::Family::css) {
            more = detail::css_token(scan, token);
        } else if (family == detail::Family::markup) {
            more = detail::markup_token(scan, token);
        } else {
            more = detail::c_like_token(scan, token);
        }
        if (!more) {
            cursor.done = true;
            break;
        }
        if (token.length != 0) {
            out[written] = token;
            written = written + 1;
        }
    }
    cursor.position = scan.position;
    cursor.state.value = scan.state;
    cursor.context =
        static_cast<u8>(scan.expect | (scan.value_before ? context_value_before : 0) | (scan.started ? context_started : 0));
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
