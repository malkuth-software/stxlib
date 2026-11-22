// stxlib::syntax's markup engine (scan.hpp): XML and HTML.
//
// After SwiftEdit's XML lexer (src/highlight/xml_language.cpp at 1c68d8b): text between
// tags is plain; a tag is its brackets (punctuation), its name (tag), attribute names
// (property), `=` (operator) and values (string); comments, CDATA (string), processing
// instructions and <!DOCTYPE ...> (directive) and entities (constant) are whole tokens.
//
// LineState: the mode below; the rule is the open attribute value's quote (1 for ", 2
// for '); the payload's bits are tag_* flags, kept through a tag and its values.
//
// HTML: the content of <script> and <style> runs to the matching close tag and is plain.
// Lexing it in its own language needs a second state beside this one and is not done.

#include "scan.hpp"

namespace stxlib::syntax::detail {

namespace {

// Mode 0 is text.
constexpr u8 mode_comment = 1;
constexpr u8 mode_cdata = 2;
constexpr u8 mode_instruction = 3;
constexpr u8 mode_declaration = 4;
constexpr u8 mode_tag = 5;
constexpr u8 mode_value = 6;
constexpr u8 mode_raw_text = 7;

// Payload bits inside a tag.
constexpr u64 tag_script = 1;        // an opening <script>
constexpr u64 tag_style = 2;         // an opening <style>
constexpr u64 tag_name_pending = 4;  // the tag's name is the next token
constexpr u64 tag_closing = 8;       // </name
constexpr u64 tag_after_equals = 16; // the next token is an attribute's value

[[nodiscard]] bool name_byte(const char byte) noexcept {
    const u8 value = static_cast<u8>(byte);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || is_digit(byte) || byte == '-' || byte == '_' || byte == ':' || byte == '.' ||
           value >= 0x80;
}

[[nodiscard]] u32 name_end(const Scan& scan, u32 index) noexcept {
    while (index < scan.count && name_byte(scan.text[index])) ++index;
    return index;
}

// Lexes a construct that runs to `close`: [start, the end of close) or the line's end.
void run_to(Scan& scan, LineToken& token, const u32 start, u32 index, const TextView close, const u8 mode, const TokenKind kind) noexcept {
    while (index < scan.count && !starts_with(scan, index, close)) ++index;
    const bool closed = index < scan.count;
    scan.state = closed ? 0 : pack_state(mode, 0, 0);
    take(scan, token, start, closed ? index + static_cast<u32>(close.count()) : scan.count, kind);
}

void value_body(Scan& scan, LineToken& token, const u32 start, u32 index, const char quote, const u64 flags) noexcept {
    while (index < scan.count && scan.text[index] != quote) ++index;
    const bool closed = index < scan.count;
    scan.state = closed ? pack_state(mode_tag, 0, flags) : pack_state(mode_value, quote == '"' ? 1 : 2, flags);
    take(scan, token, start, closed ? index + 1 : scan.count, TokenKind::string);
}

void tag_token(Scan& scan, LineToken& token, const u32 start, const u64 flags) noexcept {
    const GrammarSpec& spec = scan.grammar.spec;
    const char byte = scan.text[start];
    if ((flags & tag_name_pending) != 0 && name_byte(byte)) {
        const u32 end = name_end(scan, start);
        u64 next = flags & tag_closing;
        if (spec.html && (flags & tag_closing) == 0) {
            if (end - start == 6 && starts_with_folded(scan, start, "script")) next = next | tag_script;
            if (end - start == 5 && starts_with_folded(scan, start, "style")) next = next | tag_style;
        }
        scan.state = pack_state(mode_tag, 0, next);
        take(scan, token, start, end, TokenKind::tag);
        return;
    }
    const u64 kept = flags & (tag_script | tag_style | tag_closing);
    if (byte == '>') {
        const u64 raw = flags & (tag_script | tag_style);
        scan.state = raw != 0 ? pack_state(mode_raw_text, 0, raw) : 0;
        take(scan, token, start, start + 1, TokenKind::punctuation);
        return;
    }
    if (byte == '/' && start + 1 < scan.count && scan.text[start + 1] == '>') {
        scan.state = 0;
        take(scan, token, start, start + 2, TokenKind::punctuation);
        return;
    }
    if (byte == '"' || byte == '\'') {
        value_body(scan, token, start, start + 1, byte, kept);
        return;
    }
    if (byte == '=') {
        scan.state = pack_state(mode_tag, 0, kept | tag_after_equals);
        take(scan, token, start, start + 1, TokenKind::operator_symbol);
        return;
    }
    if ((flags & tag_after_equals) != 0) {
        // An unquoted value: to the next space or the tag's end.
        u32 index = start;
        while (index < scan.count && class_at(scan, index) != ByteClass::space && scan.text[index] != '>') ++index;
        scan.state = pack_state(mode_tag, 0, kept);
        take(scan, token, start, index, TokenKind::string);
        return;
    }
    if (name_byte(byte)) {
        scan.state = pack_state(mode_tag, 0, kept);
        take(scan, token, start, name_end(scan, start), TokenKind::property);
        return;
    }
    if (byte == '<') {
        // A new tag before this one closed: the text is malformed; start over from it.
        scan.state = 0;
        return;
    }
    scan.state = pack_state(mode_tag, 0, kept);
    scan.position = start + 1;
}

void text_token(Scan& scan, LineToken& token) noexcept {
    u32 index = scan.position;
    while (index < scan.count && scan.text[index] != '<' && scan.text[index] != '&') ++index;
    scan.position = index;
    if (index >= scan.count) return;
    const u32 start = index;
    if (scan.text[start] == '&') {
        // &name; or &#number; within a few bytes; otherwise a plain ampersand.
        u32 end = start + 1;
        while (end < scan.count && end - start < 12 && (name_byte(scan.text[end]) || scan.text[end] == '#')) ++end;
        if (end < scan.count && scan.text[end] == ';' && end > start + 1) {
            take(scan, token, start, end + 1, TokenKind::constant);
            return;
        }
        scan.position = start + 1;
        return;
    }
    if (starts_with(scan, start, "<!--")) {
        run_to(scan, token, start, start + 4, "-->", mode_comment, TokenKind::comment);
        return;
    }
    if (starts_with(scan, start, "<![CDATA[")) {
        run_to(scan, token, start, start + 9, "]]>", mode_cdata, TokenKind::string);
        return;
    }
    if (starts_with(scan, start, "<?")) {
        run_to(scan, token, start, start + 2, "?>", mode_instruction, TokenKind::directive);
        return;
    }
    if (starts_with(scan, start, "<!")) {
        run_to(scan, token, start, start + 2, ">", mode_declaration, TokenKind::directive);
        return;
    }
    const bool closing = start + 1 < scan.count && scan.text[start + 1] == '/';
    const u32 name = start + (closing ? 2u : 1u);
    if (name < scan.count && name_byte(scan.text[name])) {
        scan.state = pack_state(mode_tag, 0, tag_name_pending | (closing ? tag_closing : 0));
        take(scan, token, start, name, TokenKind::punctuation);
        return;
    }
    // A '<' that opens nothing.
    scan.position = start + 1;
}

} // namespace

bool markup_token(Scan& scan, LineToken& token) noexcept {
    token.length = 0;
    if (scan.position >= scan.count) return false;
    const u8 mode = state_mode(scan.state);
    const u64 flags = state_payload(scan.state);
    const u32 start = scan.position;
    switch (mode) {
    case mode_comment:
        run_to(scan, token, start, start, "-->", mode_comment, TokenKind::comment);
        return true;
    case mode_cdata:
        run_to(scan, token, start, start, "]]>", mode_cdata, TokenKind::string);
        return true;
    case mode_instruction:
        run_to(scan, token, start, start, "?>", mode_instruction, TokenKind::directive);
        return true;
    case mode_declaration:
        run_to(scan, token, start, start, ">", mode_declaration, TokenKind::directive);
        return true;
    case mode_value:
        value_body(scan, token, start, start, state_rule(scan.state) == 1 ? '"' : '\'', flags);
        return true;
    case mode_tag: {
        scan.position = skip_spaces(scan, start);
        if (scan.position >= scan.count) return false;
        tag_token(scan, token, scan.position, flags);
        return true;
    }
    case mode_raw_text: {
        // To </script or </style, whichever opened it.
        const TextView close = (flags & tag_script) != 0 ? TextView("</script") : TextView("</style");
        u32 index = start;
        while (index < scan.count && !starts_with_folded(scan, index, close)) ++index;
        scan.position = index;
        if (index < scan.count) scan.state = 0;
        return index < scan.count;
    }
    default:
        break;
    }
    text_token(scan, token);
    return scan.position < scan.count || token.length != 0;
}

} // namespace stxlib::syntax::detail
