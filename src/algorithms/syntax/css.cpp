// stxlib::syntax's style-sheet engine (scan.hpp): CSS, with nested rules.
//
// The method is SwiftEdit's CSS lexer's (src/highlight/css_language.cpp at 1c68d8b): a
// context per statement (selector, declaration value, at-rule prelude) carried in the line
// state with the block depth. One thing differs: whether a statement is a rule or a
// declaration is decided by looking ahead on the line for `{` before `;` or `}`, so a nested
// rule (`&:hover {`, `.child {`) needs no table of which at-rules hold rules.
//
// LineState: mode 0 code, 1 comment, 2 string (the rule is 1 for " and 2 for '). The payload
// keeps bits 0-2 the context and bits 3-10 the block depth (saturating), in every mode.
//
// Kinds: selectors are tag (a type) and selector (class, identity, pseudo); a declaration's
// name is property; a custom property is variable; at-keywords are directive; value words
// of the language's table are constant; !important is keyword; #rgb is color.

#include "scan.hpp"

namespace stxlib::syntax::detail {

namespace {

constexpr u8 mode_code = 0;
constexpr u8 mode_comment = 1;
constexpr u8 mode_string = 2;

constexpr u64 context_start = 0;
constexpr u64 context_selector = 1;
constexpr u64 context_value = 2;
constexpr u64 context_prelude = 3;

constexpr u64 depth_limit = 255;

[[nodiscard]] constexpr u64 payload_of(const u64 context, const u64 depth) noexcept { return context | (depth << 3); }
[[nodiscard]] constexpr u64 context_of(const u64 payload) noexcept { return payload & 7u; }
[[nodiscard]] constexpr u64 depth_of(const u64 payload) noexcept { return (payload >> 3) & 255u; }

[[nodiscard]] bool name_byte(const char byte) noexcept {
    const u8 value = static_cast<u8>(byte);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || is_digit(byte) || byte == '-' || byte == '_' || byte == '\\' || value >= 0x80;
}

// A name starts with a letter, '_', a non-ASCII byte, an escape, or '-' before one of those
// or another '-'.
[[nodiscard]] bool name_starts(const Scan& scan, const u32 index) noexcept {
    if (index >= scan.count) return false;
    const char byte = scan.text[index];
    if (is_digit(byte)) return false;
    if (byte == '-') {
        if (index + 1 >= scan.count) return false;
        const char next = scan.text[index + 1];
        return next == '-' || (name_byte(next) && !is_digit(next));
    }
    return name_byte(byte);
}

[[nodiscard]] u32 name_end(const Scan& scan, u32 index) noexcept {
    while (index < scan.count && name_byte(scan.text[index])) {
        index = index + (scan.text[index] == '\\' && index + 1 < scan.count ? 2u : 1u);
    }
    return index < scan.count ? index : scan.count;
}

[[nodiscard]] bool number_starts(const Scan& scan, const u32 index) noexcept {
    if (index >= scan.count) return false;
    const char byte = scan.text[index];
    if (is_digit(byte)) return true;
    const bool has_next = index + 1 < scan.count;
    if (byte == '.') return has_next && is_digit(scan.text[index + 1]);
    if (byte == '+' || byte == '-') {
        if (!has_next) return false;
        const char next = scan.text[index + 1];
        return is_digit(next) || (next == '.' && index + 2 < scan.count && is_digit(scan.text[index + 2]));
    }
    return false;
}

// A number with its unit: 12, -1.5e3, 90ms, 50%.
[[nodiscard]] u32 number_end(const Scan& scan, u32 index) noexcept {
    if (scan.text[index] == '+' || scan.text[index] == '-') ++index;
    while (index < scan.count && (is_digit(scan.text[index]) || (scan.text[index] == '.' && index + 1 < scan.count && is_digit(scan.text[index + 1])))) ++index;
    if (index < scan.count && scan.text[index] == '%') return index + 1;
    while (index < scan.count && name_byte(scan.text[index]) && scan.text[index] != '\\') ++index;
    return index;
}

// The first of `{`, `;` and `}` at or after `index` outside strings and parentheses on this
// line; 0 when the line has none.
[[nodiscard]] char statement_end(const Scan& scan, u32 index) noexcept {
    u32 parentheses = 0;
    char quote = 0;
    while (index < scan.count) {
        const char byte = scan.text[index];
        if (quote != 0) {
            if (byte == '\\') {
                index = index + 2;
                continue;
            }
            if (byte == quote) quote = 0;
        } else if (byte == '"' || byte == '\'') {
            quote = byte;
        } else if (byte == '/' && index + 1 < scan.count && scan.text[index + 1] == '*') {
            return 0;
        } else if (byte == '(') {
            parentheses = parentheses + 1;
        } else if (byte == ')') {
            parentheses = parentheses == 0 ? 0 : parentheses - 1;
        } else if (parentheses == 0 && (byte == '{' || byte == ';' || byte == '}')) {
            return byte;
        }
        ++index;
    }
    return 0;
}

// Opens or closes a block at `start` (one byte) and returns to a statement's start.
void block_byte(Scan& scan, LineToken& token, const u32 start, const u64 depth) noexcept {
    const char byte = scan.text[start];
    u64 next = depth;
    if (byte == '{') next = depth < depth_limit ? depth + 1 : depth_limit;
    if (byte == '}') next = depth == 0 ? 0 : depth - 1;
    scan.state = pack_state(mode_code, 0, payload_of(context_start, next));
    take(scan, token, start, start + 1, TokenKind::punctuation);
}

void lex_string_body(Scan& scan, LineToken& token, const u32 start, u32 index, const char quote, const u64 payload) noexcept {
    bool spliced = false;
    while (index < scan.count) {
        const char byte = scan.text[index];
        if (byte == '\\') {
            spliced = index + 1 >= scan.count;
            index = index + 2;
            continue;
        }
        if (byte == quote) {
            scan.state = pack_state(mode_code, 0, payload);
            take(scan, token, start, index + 1, TokenKind::string);
            return;
        }
        ++index;
    }
    // A backslash at the line's end continues the string; otherwise the line's end closes it.
    scan.state = spliced ? pack_state(mode_string, quote == '"' ? 1 : 2, payload) : pack_state(mode_code, 0, payload);
    take(scan, token, start, scan.count, TokenKind::string);
}

void lex_comment_body(Scan& scan, LineToken& token, const u32 start, u32 index, const u64 payload) noexcept {
    while (index + 1 < scan.count) {
        if (scan.text[index] == '*' && scan.text[index + 1] == '/') {
            scan.state = pack_state(mode_code, 0, payload);
            take(scan, token, start, index + 2, TokenKind::comment);
            return;
        }
        ++index;
    }
    scan.state = pack_state(mode_comment, 0, payload);
    take(scan, token, start, scan.count, TokenKind::comment);
}

[[nodiscard]] bool operator_byte(const char byte) noexcept {
    return byte == '>' || byte == '+' || byte == '~' || byte == '*' || byte == '/' || byte == '=' || byte == '|' || byte == '^' || byte == '$' ||
           byte == '&' || byte == '-' || byte == '<';
}

// A value's or prelude's token at `start`; `prelude` says which.
void value_token(Scan& scan, LineToken& token, const u32 start, const u64 depth, const bool prelude) noexcept {
    const char byte = scan.text[start];
    // The unquoted argument of url( ... ).
    if (start >= 4 && scan.text[start - 1] == '(' && starts_with_folded(scan, start - 4, "url") && byte != ')') {
        u32 index = start;
        while (index < scan.count && scan.text[index] != ')') ++index;
        take(scan, token, start, index, TokenKind::string);
        return;
    }
    if (number_starts(scan, start)) {
        take(scan, token, start, number_end(scan, start), TokenKind::number);
        return;
    }
    if (byte == '#') {
        u32 index = start + 1;
        while (index < scan.count && is_hex_digit(scan.text[index])) ++index;
        const u32 digits = index - (start + 1);
        const bool shaped = (digits == 3 || digits == 4 || digits == 6 || digits == 8) && !(index < scan.count && name_byte(scan.text[index]));
        if (!shaped) index = name_end(scan, start + 1);
        take(scan, token, start, index > start + 1 ? index : start + 1, shaped ? TokenKind::color : TokenKind::invalid);
        return;
    }
    if (byte == '!' && name_starts(scan, skip_spaces(scan, start + 1))) {
        take(scan, token, start, name_end(scan, skip_spaces(scan, start + 1)), TokenKind::keyword);
        return;
    }
    if (name_starts(scan, start)) {
        const u32 end = name_end(scan, start);
        TokenKind kind = TokenKind::identifier;
        if (end - start >= 2 && scan.text[start] == '-' && scan.text[start + 1] == '-') {
            kind = TokenKind::variable;
        } else if (end < scan.count && scan.text[end] == '(') {
            kind = TokenKind::function;
        } else {
            const WordSlot* const word = find_word_at(scan, start, end);
            if (word != nullptr && (prelude ? (*word).kind == TokenKind::keyword : (*word).kind == TokenKind::constant)) kind = (*word).kind;
        }
        take(scan, token, start, end, kind);
        return;
    }
    if (byte == '{' || byte == '}') {
        block_byte(scan, token, start, depth);
        return;
    }
    if (byte == ';') {
        scan.state = pack_state(mode_code, 0, payload_of(context_start, depth));
        take(scan, token, start, start + 1, TokenKind::punctuation);
        return;
    }
    if (byte == ',' || byte == '(' || byte == ')' || byte == ':' || byte == '[' || byte == ']') {
        take(scan, token, start, start + 1, TokenKind::punctuation);
        return;
    }
    if (operator_byte(byte)) {
        take(scan, token, start, start + 1, TokenKind::operator_symbol);
        return;
    }
    token.length = 0;
    scan.position = start + 1;
}

void selector_token(Scan& scan, LineToken& token, const u32 start, const u64 depth) noexcept {
    const char byte = scan.text[start];
    if (name_starts(scan, start)) {
        take(scan, token, start, name_end(scan, start), TokenKind::tag);
        return;
    }
    if ((byte == '.' || byte == '#') && name_starts(scan, start + 1)) {
        take(scan, token, start, name_end(scan, start + 1), TokenKind::selector);
        return;
    }
    if (byte == ':') {
        const u32 name = start + (start + 1 < scan.count && scan.text[start + 1] == ':' ? 2u : 1u);
        if (name_starts(scan, name)) {
            take(scan, token, start, name_end(scan, name), TokenKind::selector);
            return;
        }
    }
    if (number_starts(scan, start) && is_digit(byte)) {
        take(scan, token, start, number_end(scan, start), TokenKind::number);
        return;
    }
    if (byte == '{' || byte == '}') {
        block_byte(scan, token, start, depth);
        return;
    }
    if (byte == ';') {
        scan.state = pack_state(mode_code, 0, payload_of(context_start, depth));
        take(scan, token, start, start + 1, TokenKind::punctuation);
        return;
    }
    if (byte == ',' || byte == '(' || byte == ')' || byte == '[' || byte == ']' || byte == ':') {
        take(scan, token, start, start + 1, TokenKind::punctuation);
        return;
    }
    if (operator_byte(byte)) {
        take(scan, token, start, start + 1, TokenKind::operator_symbol);
        return;
    }
    token.length = 0;
    scan.position = start + 1;
}

} // namespace

bool css_token(Scan& scan, LineToken& token) noexcept {
    token.length = 0;
    if (scan.position >= scan.count) return false;
    const u8 mode = state_mode(scan.state);
    const u64 payload = state_payload(scan.state);
    if (mode == mode_comment) {
        lex_comment_body(scan, token, scan.position, scan.position, payload);
        return true;
    }
    if (mode == mode_string) {
        lex_string_body(scan, token, scan.position, scan.position, state_rule(scan.state) == 1 ? '"' : '\'', payload);
        return true;
    }
    scan.position = skip_spaces(scan, scan.position);
    if (scan.position >= scan.count) return false;
    const u32 start = scan.position;
    const char byte = scan.text[start];
    const u64 depth = depth_of(payload);
    u64 context = context_of(payload);
    if (byte == '/' && start + 1 < scan.count && scan.text[start + 1] == '*') {
        lex_comment_body(scan, token, start, start + 2, payload);
        return true;
    }
    if (byte == '"' || byte == '\'') {
        lex_string_body(scan, token, start, start + 1, byte, payload);
        return true;
    }
    if (context == context_start) {
        if (byte == '@' && name_starts(scan, start + 1)) {
            scan.state = pack_state(mode_code, 0, payload_of(context_prelude, depth));
            take(scan, token, start, name_end(scan, start + 1), TokenKind::directive);
            return true;
        }
        if (byte == '{' || byte == '}') {
            block_byte(scan, token, start, depth);
            return true;
        }
        if (byte == ';') {
            take(scan, token, start, start + 1, TokenKind::punctuation);
            return true;
        }
        // A rule or a declaration: `{` before `;` or `}` on this line is a rule. With
        // neither, the top level holds rules, and a block holds declarations when a colon
        // follows the name.
        const char end = statement_end(scan, start);
        bool rule = end == '{';
        if (end == 0) {
            const u32 after = name_starts(scan, start) ? skip_spaces(scan, name_end(scan, start)) : start;
            rule = depth == 0 || !(after < scan.count && scan.text[after] == ':');
        }
        if (rule) {
            context = context_selector;
            scan.state = pack_state(mode_code, 0, payload_of(context_selector, depth));
        } else {
            scan.state = pack_state(mode_code, 0, payload_of(context_value, depth));
            if (name_starts(scan, start)) {
                const u32 name = name_end(scan, start);
                const bool custom = name - start >= 2 && scan.text[start] == '-' && scan.text[start + 1] == '-';
                take(scan, token, start, name, custom ? TokenKind::variable : TokenKind::property);
                return true;
            }
            context = context_value;
        }
    }
    if (context == context_selector) {
        selector_token(scan, token, start, depth);
    } else {
        value_token(scan, token, start, depth, context == context_prelude);
    }
    return true;
}

} // namespace stxlib::syntax::detail
