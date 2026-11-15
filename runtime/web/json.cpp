// json.cpp: the strict JSON reader and the writer (json.h).

#include "json.h"

#include <cassert>

namespace stxweb {

// ---------------------------------------------------------------------------
// Names and outcomes.

const char* json_error_name(JsonError error) noexcept {
    switch (error) {
    case JsonError::empty: return "empty";
    case JsonError::too_large: return "too_large";
    case JsonError::byte_order_mark: return "byte_order_mark";
    case JsonError::unexpected_byte: return "unexpected_byte";
    case JsonError::unexpected_end: return "unexpected_end";
    case JsonError::too_deep: return "too_deep";
    case JsonError::too_many_tokens: return "too_many_tokens";
    case JsonError::literal_malformed: return "literal_malformed";
    case JsonError::number_malformed: return "number_malformed";
    case JsonError::number_not_integer: return "number_not_integer";
    case JsonError::number_out_of_range: return "number_out_of_range";
    case JsonError::string_unterminated: return "string_unterminated";
    case JsonError::string_control_byte: return "string_control_byte";
    case JsonError::escape_malformed: return "escape_malformed";
    case JsonError::surrogate_unpaired: return "surrogate_unpaired";
    case JsonError::utf8_malformed: return "utf8_malformed";
    case JsonError::key_repeated: return "key_repeated";
    case JsonError::trailing_bytes: return "trailing_bytes";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

JsonOutcome::JsonOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

JsonOutcome JsonOutcome::parsed() noexcept {
    const JsonOutcome outcome(Alternatives(std::in_place_type<JsonParsed>));
    return outcome;
}

JsonOutcome JsonOutcome::refused(JsonError error, std::size_t offset) noexcept {
    JsonRefused refusal{};
    refusal.error = error;
    refusal.offset = offset;
    const JsonOutcome outcome(Alternatives(std::in_place_type<JsonRefused>, refusal));
    return outcome;
}

bool JsonOutcome::is_parsed() const noexcept {
    return std::holds_alternative<JsonParsed>(alternatives_);
}

const JsonRefused* JsonOutcome::as_refused() const noexcept {
    return std::get_if<JsonRefused>(&alternatives_);
}

// ---------------------------------------------------------------------------
// UTF-8 and escapes.

namespace {

[[nodiscard]] unsigned int byte_value(char byte) noexcept {
    return static_cast<unsigned int>(static_cast<unsigned char>(byte));
}

[[nodiscard]] bool in_range(unsigned int value, unsigned int low, unsigned int high) noexcept {
    return value >= low && value <= high;
}

}  // namespace

std::size_t utf8_sequence_length(std::span<const char> bytes) noexcept {
    if (bytes.empty()) {
        return 0;
    }
    const unsigned int first = byte_value(bytes[0]);
    if (first <= 0x7fu) {
        return 1;
    }
    // The second byte's range by the first byte (table 3-7); the rest are
    // 80..BF.
    std::size_t length = 0;
    unsigned int second_low = 0x80u;
    unsigned int second_high = 0xbfu;
    if (in_range(first, 0xc2u, 0xdfu)) {
        length = 2;
    } else if (first == 0xe0u) {
        length = 3;
        second_low = 0xa0u;
    } else if (in_range(first, 0xe1u, 0xecu) || in_range(first, 0xeeu, 0xefu)) {
        length = 3;
    } else if (first == 0xedu) {
        length = 3;
        second_high = 0x9fu;
    } else if (first == 0xf0u) {
        length = 4;
        second_low = 0x90u;
    } else if (in_range(first, 0xf1u, 0xf3u)) {
        length = 4;
    } else if (first == 0xf4u) {
        length = 4;
        second_high = 0x8fu;
    } else {
        return 0;
    }
    if (bytes.size() < length || !in_range(byte_value(bytes[1]), second_low, second_high)) {
        return 0;
    }
    for (std::size_t index = 2; index < length; index += 1) {
        if (!in_range(byte_value(bytes[index]), 0x80u, 0xbfu)) {
            return 0;
        }
    }
    return length;
}

namespace {

[[nodiscard]] int hex_digit(char byte) noexcept {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

// Four hex digits at text[at..at+4), or nothing (not hex, or cut off).
[[nodiscard]] std::optional<unsigned int> hex4(std::span<const char> text, std::size_t at) noexcept {
    if (at > text.size() || text.size() - at < 4u) {
        return std::nullopt;
    }
    unsigned int value = 0;
    for (std::size_t index = 0; index < 4u; index += 1) {
        const int digit = hex_digit(text[at + index]);
        if (digit < 0) {
            return std::nullopt;
        }
        value = value * 16u + static_cast<unsigned int>(digit);
    }
    const std::optional<unsigned int> found = value;
    return found;
}

[[nodiscard]] bool is_high_surrogate(unsigned int unit) noexcept {
    return in_range(unit, 0xd800u, 0xdbffu);
}

[[nodiscard]] bool is_low_surrogate(unsigned int unit) noexcept {
    return in_range(unit, 0xdc00u, 0xdfffu);
}

// Encodes a scalar value (not a surrogate, at most 0x10FFFF) as UTF-8.
// Returns the byte count, 1..4.
[[nodiscard]] std::size_t encode_utf8(unsigned int code_point, char (&out)[4]) noexcept {
    if (code_point <= 0x7fu) {
        out[0] = static_cast<char>(code_point);
        return 1;
    }
    if (code_point <= 0x7ffu) {
        out[0] = static_cast<char>(0xc0u | (code_point >> 6u));
        out[1] = static_cast<char>(0x80u | (code_point & 0x3fu));
        return 2;
    }
    if (code_point <= 0xffffu) {
        out[0] = static_cast<char>(0xe0u | (code_point >> 12u));
        out[1] = static_cast<char>(0x80u | ((code_point >> 6u) & 0x3fu));
        out[2] = static_cast<char>(0x80u | (code_point & 0x3fu));
        return 3;
    }
    out[0] = static_cast<char>(0xf0u | (code_point >> 18u));
    out[1] = static_cast<char>(0x80u | ((code_point >> 12u) & 0x3fu));
    out[2] = static_cast<char>(0x80u | ((code_point >> 6u) & 0x3fu));
    out[3] = static_cast<char>(0x80u | (code_point & 0x3fu));
    return 4;
}

// Yields the unescaped bytes of a string the parser has validated (the
// bytes between its quotes), one at a time.
class StringCursor {
public:
    explicit StringCursor(std::span<const char> raw) noexcept
        : raw_(raw), position_(0), pending_{}, pending_count_(0), pending_index_(0) {}

    // The next byte, or false at the end.
    [[nodiscard]] bool next(char& byte) noexcept {
        if (pending_index_ < pending_count_) {
            byte = pending_[pending_index_];
            pending_index_ += 1;
            return true;
        }
        if (position_ >= raw_.size()) {
            return false;
        }
        const char current = raw_[position_];
        if (current != '\\') {
            byte = current;
            position_ += 1;
            return true;
        }
        // Validated: an escape is complete and well formed.
        const char kind = position_ + 1u < raw_.size() ? raw_[position_ + 1u] : '\0';
        position_ += 2;
        switch (kind) {
        case '"': byte = '"'; return true;
        case '\\': byte = '\\'; return true;
        case '/': byte = '/'; return true;
        case 'b': byte = '\b'; return true;
        case 'f': byte = '\f'; return true;
        case 'n': byte = '\n'; return true;
        case 'r': byte = '\r'; return true;
        case 't': byte = '\t'; return true;
        case 'u': break;
        default:
            // Fallback: the parser admits no other escape; end the string.
            position_ = raw_.size();
            return false;
        }
        const std::optional<unsigned int> unit = hex4(raw_, position_);
        position_ += 4;
        unsigned int code_point = unit.has_value() ? *unit : 0xfffdu;
        if (is_high_surrogate(code_point)) {
            // Validated: "\uDC00".."\uDFFF" follows.
            const std::optional<unsigned int> low = hex4(raw_, position_ + 2u);
            position_ += 6;
            const unsigned int low_unit = low.has_value() ? *low : 0xdc00u;
            code_point = 0x10000u + ((code_point - 0xd800u) << 10u) + (low_unit - 0xdc00u);
        }
        pending_count_ = encode_utf8(code_point, pending_);
        pending_index_ = 1;
        byte = pending_[0];
        return true;
    }

private:
    std::span<const char> raw_;
    std::size_t position_;
    char pending_[4];
    std::size_t pending_count_;
    std::size_t pending_index_;
};

[[nodiscard]] bool cursors_equal(StringCursor left, StringCursor right) noexcept {
    // Each step consumes at least one byte of each, so the loop ends.
    for (;;) {
        char left_byte = '\0';
        char right_byte = '\0';
        const bool left_more = left.next(left_byte);
        const bool right_more = right.next(right_byte);
        if (left_more != right_more) {
            return false;
        }
        if (!left_more) {
            return true;
        }
        if (left_byte != right_byte) {
            return false;
        }
    }
}

[[nodiscard]] bool is_whitespace(char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

[[nodiscard]] bool is_digit(char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

}  // namespace

// ---------------------------------------------------------------------------
// JsonDocument.

JsonDocument::JsonDocument() noexcept : text_(), tokens_{}, token_count_(0) {}

std::size_t JsonDocument::token_count() const noexcept {
    return token_count_;
}

const JsonToken& JsonDocument::token(std::size_t index) const noexcept {
    assert(index < token_count_);
    return tokens_[index];
}

std::span<const char> JsonDocument::text() const noexcept {
    return text_;
}

std::optional<std::size_t> JsonDocument::member(std::size_t object_index, std::span<const char> key) const noexcept {
    if (object_index >= token_count_ || tokens_[object_index].kind != JsonKind::object) {
        return std::nullopt;
    }
    std::size_t key_index = object_index + 1u;
    for (std::uint32_t member_number = 0; member_number < tokens_[object_index].count; member_number += 1) {
        // Each member is a key token, then its value's tokens.
        if (key_index + 1u >= token_count_) {
            return std::nullopt;
        }
        if (string_equals(key_index, key)) {
            const std::optional<std::size_t> value = key_index + 1u;
            return value;
        }
        key_index = tokens_[key_index + 1u].end;
    }
    return std::nullopt;
}

std::optional<std::size_t> JsonDocument::member(std::size_t object_index, const char* key) const noexcept {
    std::size_t length = 0;
    while (key[length] != '\0') {
        length += 1;
    }
    const std::span<const char> bytes(key, length);
    return member(object_index, bytes);
}

std::optional<std::size_t> JsonDocument::decode_string(std::size_t index, std::span<char> destination) const noexcept {
    if (index >= token_count_ || tokens_[index].kind != JsonKind::string) {
        return std::nullopt;
    }
    StringCursor cursor(text_.subspan(tokens_[index].offset, tokens_[index].length));
    std::size_t size = 0;
    char byte = '\0';
    while (cursor.next(byte)) {
        if (size >= destination.size()) {
            return std::nullopt;
        }
        destination[size] = byte;
        size += 1;
    }
    const std::optional<std::size_t> decoded = size;
    return decoded;
}

bool JsonDocument::string_equals(std::size_t index, std::span<const char> bytes) const noexcept {
    if (index >= token_count_ || tokens_[index].kind != JsonKind::string) {
        return false;
    }
    StringCursor cursor(text_.subspan(tokens_[index].offset, tokens_[index].length));
    std::size_t position = 0;
    char byte = '\0';
    while (cursor.next(byte)) {
        if (position >= bytes.size() || bytes[position] != byte) {
            return false;
        }
        position += 1;
    }
    return position == bytes.size();
}

// ---------------------------------------------------------------------------
// The parser: one pass, an explicit stack of open containers.

class JsonParser {
public:
    JsonParser(std::span<const char> text, JsonDocument& document) noexcept
        : text_(text), document_(document), position_(0), stack_{}, depth_(0) {}

    [[nodiscard]] JsonOutcome run() noexcept;

private:
    // What the next non-blank byte may be.
    enum class Expect : std::uint8_t {
        value,
        value_or_array_end,
        key,
        key_or_object_end,
        after_value,
    };

    [[nodiscard]] static std::optional<JsonRefused> refuse(JsonError error, std::size_t offset) noexcept {
        JsonRefused refusal{};
        refusal.error = error;
        refusal.offset = offset;
        const std::optional<JsonRefused> result = refusal;
        return result;
    }

    void skip_whitespace() noexcept {
        while (position_ < text_.size() && is_whitespace(text_[position_])) {
            position_ += 1;
        }
    }

    [[nodiscard]] bool at_end() const noexcept {
        return position_ >= text_.size();
    }

    // Appends a token at the current position; nothing on overflow.
    [[nodiscard]] std::optional<std::size_t> add_token(JsonKind kind) noexcept {
        if (document_.token_count_ >= json_tokens_max) {
            return std::nullopt;
        }
        const std::size_t index = document_.token_count_;
        JsonToken token{};
        token.kind = kind;
        token.offset = static_cast<std::uint32_t>(position_);
        token.length = 0;
        token.end = static_cast<std::uint32_t>(index + 1u);
        token.count = 0;
        token.integer = 0;
        document_.tokens_[index] = token;
        document_.token_count_ += 1;
        const std::optional<std::size_t> added = index;
        return added;
    }

    [[nodiscard]] std::optional<JsonRefused> read_value(Expect& expect) noexcept;
    [[nodiscard]] std::optional<JsonRefused> read_string() noexcept;
    [[nodiscard]] std::optional<JsonRefused> read_number(std::size_t index) noexcept;
    [[nodiscard]] std::optional<JsonRefused> read_literal(std::size_t index) noexcept;
    [[nodiscard]] std::optional<JsonRefused> close_container(Expect& expect) noexcept;
    [[nodiscard]] bool keys_distinct(std::size_t object_index) const noexcept;

    std::span<const char> text_;
    JsonDocument& document_;
    std::size_t position_;
    // Token indices of the open containers, outermost first.
    std::size_t stack_[json_depth_max];
    std::size_t depth_;
};

// A string token from the opening quote at position_; position_ ends after
// the closing quote.
std::optional<JsonRefused> JsonParser::read_string() noexcept {
    const std::optional<std::size_t> index = add_token(JsonKind::string);
    if (!index.has_value()) {
        return refuse(JsonError::too_many_tokens, position_);
    }
    position_ += 1;
    const std::size_t begin = position_;
    // Each pass consumes at least one byte, so the loop ends.
    for (;;) {
        if (at_end()) {
            return refuse(JsonError::string_unterminated, position_);
        }
        const char byte = text_[position_];
        if (byte == '"') {
            document_.tokens_[*index].offset = static_cast<std::uint32_t>(begin);
            document_.tokens_[*index].length = static_cast<std::uint32_t>(position_ - begin);
            position_ += 1;
            return std::nullopt;
        }
        if (byte_value(byte) < 0x20u) {
            return refuse(JsonError::string_control_byte, position_);
        }
        if (byte == '\\') {
            if (position_ + 1u >= text_.size()) {
                return refuse(JsonError::string_unterminated, position_);
            }
            const char kind = text_[position_ + 1u];
            if (kind == '"' || kind == '\\' || kind == '/' || kind == 'b' || kind == 'f' || kind == 'n' ||
                kind == 'r' || kind == 't') {
                position_ += 2;
                continue;
            }
            if (kind != 'u') {
                return refuse(JsonError::escape_malformed, position_);
            }
            const std::optional<unsigned int> unit = hex4(text_, position_ + 2u);
            if (!unit.has_value()) {
                return refuse(JsonError::escape_malformed, position_);
            }
            if (is_low_surrogate(*unit)) {
                return refuse(JsonError::surrogate_unpaired, position_);
            }
            if (!is_high_surrogate(*unit)) {
                position_ += 6;
                continue;
            }
            // A high surrogate needs "\u" and a low surrogate next.
            const std::size_t next = position_ + 6u;
            if (next + 1u >= text_.size() || text_[next] != '\\' || text_[next + 1u] != 'u') {
                return refuse(JsonError::surrogate_unpaired, position_);
            }
            const std::optional<unsigned int> low = hex4(text_, next + 2u);
            if (!low.has_value()) {
                return refuse(JsonError::escape_malformed, next);
            }
            if (!is_low_surrogate(*low)) {
                return refuse(JsonError::surrogate_unpaired, position_);
            }
            position_ += 12;
            continue;
        }
        if (byte_value(byte) >= 0x80u) {
            const std::size_t length = utf8_sequence_length(text_.subspan(position_));
            if (length == 0u) {
                return refuse(JsonError::utf8_malformed, position_);
            }
            position_ += length;
            continue;
        }
        position_ += 1;
    }
}

std::optional<JsonRefused> JsonParser::read_number(std::size_t index) noexcept {
    const std::size_t begin = position_;
    bool negative = false;
    if (text_[position_] == '-') {
        negative = true;
        position_ += 1;
    }
    if (at_end() || !is_digit(text_[position_])) {
        return refuse(JsonError::number_malformed, position_);
    }
    std::uint64_t magnitude = 0;
    const std::uint64_t limit = static_cast<std::uint64_t>(json_integer_max);
    if (text_[position_] == '0') {
        position_ += 1;
        if (!at_end() && is_digit(text_[position_])) {
            return refuse(JsonError::number_malformed, position_);
        }
    } else {
        while (!at_end() && is_digit(text_[position_])) {
            const std::uint64_t digit = static_cast<std::uint64_t>(text_[position_] - '0');
            if (magnitude > (limit - digit) / 10u) {
                return refuse(JsonError::number_out_of_range, begin);
            }
            magnitude = magnitude * 10u + digit;
            position_ += 1;
        }
    }
    if (!at_end() && (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E')) {
        return refuse(JsonError::number_not_integer, position_);
    }
    const std::int64_t value = static_cast<std::int64_t>(magnitude);
    document_.tokens_[index].integer = negative ? -value : value;
    document_.tokens_[index].length = static_cast<std::uint32_t>(position_ - begin);
    return std::nullopt;
}

std::optional<JsonRefused> JsonParser::read_literal(std::size_t index) noexcept {
    const char* literal = nullptr;
    JsonKind kind = JsonKind::null;
    switch (text_[position_]) {
    case 't':
        literal = "true";
        kind = JsonKind::boolean_true;
        break;
    case 'f':
        literal = "false";
        kind = JsonKind::boolean_false;
        break;
    case 'n':
        literal = "null";
        kind = JsonKind::null;
        break;
    default:
        return refuse(JsonError::unexpected_byte, position_);
    }
    const std::size_t begin = position_;
    std::size_t length = 0;
    while (literal[length] != '\0') {
        if (position_ >= text_.size() || text_[position_] != literal[length]) {
            return refuse(JsonError::literal_malformed, begin);
        }
        position_ += 1;
        length += 1;
    }
    document_.tokens_[index].kind = kind;
    document_.tokens_[index].length = static_cast<std::uint32_t>(length);
    return std::nullopt;
}

// A value at position_ (not blank, not the end). A container is opened and
// expect becomes its first expectation; a scalar is read whole and expect
// becomes after_value.
std::optional<JsonRefused> JsonParser::read_value(Expect& expect) noexcept {
    const char byte = text_[position_];
    // The value is an element of the array it is in.
    if (depth_ > 0u && document_.tokens_[stack_[depth_ - 1u]].kind == JsonKind::array) {
        document_.tokens_[stack_[depth_ - 1u]].count += 1;
    }
    if (byte == '{' || byte == '[') {
        if (depth_ >= json_depth_max) {
            return refuse(JsonError::too_deep, position_);
        }
        const JsonKind kind = byte == '{' ? JsonKind::object : JsonKind::array;
        const std::optional<std::size_t> index = add_token(kind);
        if (!index.has_value()) {
            return refuse(JsonError::too_many_tokens, position_);
        }
        stack_[depth_] = *index;
        depth_ += 1;
        position_ += 1;
        expect = kind == JsonKind::object ? Expect::key_or_object_end : Expect::value_or_array_end;
        return std::nullopt;
    }
    expect = Expect::after_value;
    if (byte == '"') {
        return read_string();
    }
    if (byte == '-' || is_digit(byte)) {
        const std::optional<std::size_t> index = add_token(JsonKind::number);
        if (!index.has_value()) {
            return refuse(JsonError::too_many_tokens, position_);
        }
        return read_number(*index);
    }
    if (byte == 't' || byte == 'f' || byte == 'n') {
        const std::optional<std::size_t> index = add_token(JsonKind::null);
        if (!index.has_value()) {
            return refuse(JsonError::too_many_tokens, position_);
        }
        return read_literal(*index);
    }
    return refuse(JsonError::unexpected_byte, position_);
}

// Whether the object's keys are pairwise distinct after unescaping.
bool JsonParser::keys_distinct(std::size_t object_index) const noexcept {
    // At most json_tokens_max / 2 members.
    std::size_t keys[json_tokens_max / 2u];
    std::size_t key_count = 0;
    std::size_t key_index = object_index + 1u;
    const std::uint32_t members = document_.tokens_[object_index].count;
    for (std::uint32_t member_number = 0; member_number < members && key_count < json_tokens_max / 2u;
         member_number += 1) {
        keys[key_count] = key_index;
        key_count += 1;
        key_index = document_.tokens_[key_index + 1u].end;
    }
    for (std::size_t first = 0; first < key_count; first += 1) {
        const JsonToken& left = document_.tokens_[keys[first]];
        for (std::size_t second = first + 1u; second < key_count; second += 1) {
            const JsonToken& right = document_.tokens_[keys[second]];
            const StringCursor left_cursor(text_.subspan(left.offset, left.length));
            const StringCursor right_cursor(text_.subspan(right.offset, right.length));
            if (cursors_equal(left_cursor, right_cursor)) {
                return false;
            }
        }
    }
    return true;
}

// Closes the innermost container at its closing bracket (position_).
std::optional<JsonRefused> JsonParser::close_container(Expect& expect) noexcept {
    const std::size_t index = stack_[depth_ - 1u];
    JsonToken& token = document_.tokens_[index];
    token.end = static_cast<std::uint32_t>(document_.token_count_);
    token.length = static_cast<std::uint32_t>(position_ + 1u - token.offset);
    if (token.kind == JsonKind::object && !keys_distinct(index)) {
        return refuse(JsonError::key_repeated, position_);
    }
    depth_ -= 1;
    position_ += 1;
    expect = Expect::after_value;
    return std::nullopt;
}

JsonOutcome JsonParser::run() noexcept {
    document_.text_ = text_;
    document_.token_count_ = 0;
    if (text_.size() > json_document_bytes_max) {
        return JsonOutcome::refused(JsonError::too_large, json_document_bytes_max);
    }
    if (text_.size() >= 3u && byte_value(text_[0]) == 0xefu && byte_value(text_[1]) == 0xbbu &&
        byte_value(text_[2]) == 0xbfu) {
        return JsonOutcome::refused(JsonError::byte_order_mark, 0);
    }
    skip_whitespace();
    if (at_end()) {
        return JsonOutcome::refused(JsonError::empty, position_);
    }
    Expect expect = Expect::value;
    // Each pass consumes at least one byte or ends, so the loop ends within
    // text_.size() + 1 passes.
    for (;;) {
        skip_whitespace();
        std::optional<JsonRefused> refusal;
        if (expect == Expect::after_value && depth_ == 0u) {
            if (at_end()) {
                return JsonOutcome::parsed();
            }
            refusal = refuse(JsonError::trailing_bytes, position_);
        } else if (at_end()) {
            refusal = refuse(JsonError::unexpected_end, position_);
        } else {
            const char byte = text_[position_];
            switch (expect) {
            case Expect::value_or_array_end:
                if (byte == ']') {
                    refusal = close_container(expect);
                    break;
                }
                refusal = read_value(expect);
                break;
            case Expect::value:
                refusal = read_value(expect);
                break;
            case Expect::key_or_object_end:
                if (byte == '}') {
                    refusal = close_container(expect);
                    break;
                }
                [[fallthrough]];
            case Expect::key: {
                if (byte != '"') {
                    refusal = refuse(JsonError::unexpected_byte, position_);
                    break;
                }
                document_.tokens_[stack_[depth_ - 1u]].count += 1;
                refusal = read_string();
                if (refusal.has_value()) {
                    break;
                }
                skip_whitespace();
                if (at_end()) {
                    refusal = refuse(JsonError::unexpected_end, position_);
                    break;
                }
                if (text_[position_] != ':') {
                    refusal = refuse(JsonError::unexpected_byte, position_);
                    break;
                }
                position_ += 1;
                expect = Expect::value;
                break;
            }
            case Expect::after_value: {
                const bool in_object = document_.tokens_[stack_[depth_ - 1u]].kind == JsonKind::object;
                if (byte == ',') {
                    position_ += 1;
                    expect = in_object ? Expect::key : Expect::value;
                } else if ((in_object && byte == '}') || (!in_object && byte == ']')) {
                    refusal = close_container(expect);
                } else {
                    refusal = refuse(JsonError::unexpected_byte, position_);
                }
                break;
            }
            default:
                // Fallback: no other expectation exists.
                refusal = refuse(JsonError::unexpected_byte, position_);
                break;
            }
        }
        if (refusal.has_value()) {
            document_.token_count_ = 0;
            return JsonOutcome::refused((*refusal).error, (*refusal).offset);
        }
    }
}

JsonOutcome parse_json(std::span<const char> text, JsonDocument& document) noexcept {
    JsonParser parser(text, document);
    return parser.run();
}

// ---------------------------------------------------------------------------
// JsonWriter.

JsonWriter::JsonWriter(std::span<char> storage) noexcept
    : storage_(storage), size_(0), failed_(false), top_written_(false), frames_{}, has_element_{}, depth_(0) {}

bool JsonWriter::put(char byte) noexcept {
    if (failed_ || size_ >= storage_.size()) {
        failed_ = true;
        return false;
    }
    storage_[size_] = byte;
    size_ += 1;
    return true;
}

bool JsonWriter::put_text(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    if (failed_ || length > storage_.size() - size_) {
        failed_ = true;
        return false;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        storage_[size_ + index] = text[index];
    }
    size_ += length;
    return true;
}

bool JsonWriter::begin_value() noexcept {
    if (failed_) {
        return false;
    }
    if (depth_ == 0u) {
        if (top_written_) {
            failed_ = true;
            return false;
        }
        top_written_ = true;
        return true;
    }
    const std::size_t level = depth_ - 1u;
    switch (frames_[level]) {
    case Frame::object_expect_value:
        frames_[level] = Frame::object_expect_key;
        return true;
    case Frame::array:
        if (has_element_[level] && !put(',')) {
            return false;
        }
        has_element_[level] = true;
        return true;
    case Frame::object_expect_key:
        break;
    default:
        break;
    }
    // A value where a key belongs, or an unknown frame.
    failed_ = true;
    return false;
}

void JsonWriter::begin_object() noexcept {
    if (!begin_value()) {
        return;
    }
    if (depth_ >= json_depth_max) {
        failed_ = true;
        return;
    }
    if (!put('{')) {
        return;
    }
    frames_[depth_] = Frame::object_expect_key;
    has_element_[depth_] = false;
    depth_ += 1;
}

void JsonWriter::end_object() noexcept {
    if (failed_ || depth_ == 0u || frames_[depth_ - 1u] != Frame::object_expect_key) {
        failed_ = true;
        return;
    }
    if (put('}')) {
        depth_ -= 1;
    }
}

void JsonWriter::begin_array() noexcept {
    if (!begin_value()) {
        return;
    }
    if (depth_ >= json_depth_max) {
        failed_ = true;
        return;
    }
    if (!put('[')) {
        return;
    }
    frames_[depth_] = Frame::array;
    has_element_[depth_] = false;
    depth_ += 1;
}

void JsonWriter::end_array() noexcept {
    if (failed_ || depth_ == 0u || frames_[depth_ - 1u] != Frame::array) {
        failed_ = true;
        return;
    }
    if (put(']')) {
        depth_ -= 1;
    }
}

void JsonWriter::key(const char* name) noexcept {
    if (failed_ || depth_ == 0u || frames_[depth_ - 1u] != Frame::object_expect_key) {
        failed_ = true;
        return;
    }
    const std::size_t level = depth_ - 1u;
    if (has_element_[level] && !put(',')) {
        return;
    }
    has_element_[level] = true;
    std::size_t length = 0;
    while (name[length] != '\0') {
        length += 1;
    }
    if (!put('"')) {
        return;
    }
    put_string_body(std::span<const char>(name, length));
    if (put('"') && put(':')) {
        frames_[level] = Frame::object_expect_value;
    }
}

void JsonWriter::put_string_body(std::span<const char> bytes) noexcept {
    static const char hex[] = "0123456789abcdef";
    std::size_t position = 0;
    // Each pass consumes at least one byte.
    while (position < bytes.size() && !failed_) {
        const char byte = bytes[position];
        const unsigned int value = byte_value(byte);
        if (value >= 0x80u) {
            const std::size_t length = utf8_sequence_length(bytes.subspan(position));
            if (length == 0u) {
                // Not well-formed UTF-8: U+FFFD for this byte.
                (void)put_text("\xef\xbf\xbd");
                position += 1;
                continue;
            }
            for (std::size_t index = 0; index < length; index += 1) {
                (void)put(bytes[position + index]);
            }
            position += length;
            continue;
        }
        position += 1;
        switch (byte) {
        case '"': (void)put_text("\\\""); continue;
        case '\\': (void)put_text("\\\\"); continue;
        case '\n': (void)put_text("\\n"); continue;
        case '\r': (void)put_text("\\r"); continue;
        case '\t': (void)put_text("\\t"); continue;
        case '\b': (void)put_text("\\b"); continue;
        case '\f': (void)put_text("\\f"); continue;
        default: break;
        }
        if (value < 0x20u) {
            char escape[7] = {'\\', 'u', '0', '0', hex[(value >> 4u) & 0xfu], hex[value & 0xfu], '\0'};
            (void)put_text(escape);
            continue;
        }
        (void)put(byte);
    }
}

void JsonWriter::string(std::span<const char> bytes) noexcept {
    if (!begin_value()) {
        return;
    }
    if (!put('"')) {
        return;
    }
    put_string_body(bytes);
    (void)put('"');
}

void JsonWriter::string(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    string(std::span<const char>(text, length));
}

void JsonWriter::unsigned_integer(std::uint64_t value) noexcept {
    if (!begin_value()) {
        return;
    }
    char digits[21];
    std::size_t count = 0;
    std::uint64_t remaining = value;
    do {
        digits[19u - count] = static_cast<char>('0' + static_cast<int>(remaining % 10u));
        remaining /= 10u;
        count += 1;
    } while (remaining != 0u);
    digits[20] = '\0';
    (void)put_text(&digits[20u - count]);
}

void JsonWriter::integer(std::int64_t value) noexcept {
    if (value >= 0) {
        unsigned_integer(static_cast<std::uint64_t>(value));
        return;
    }
    if (!begin_value()) {
        return;
    }
    // The magnitude of the most negative value is representable unsigned.
    const std::uint64_t magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1u;
    char digits[21];
    std::size_t count = 0;
    std::uint64_t remaining = magnitude;
    do {
        digits[19u - count] = static_cast<char>('0' + static_cast<int>(remaining % 10u));
        remaining /= 10u;
        count += 1;
    } while (remaining != 0u);
    digits[20] = '\0';
    if (put('-')) {
        (void)put_text(&digits[20u - count]);
    }
}

void JsonWriter::boolean(bool value) noexcept {
    if (begin_value()) {
        (void)put_text(value ? "true" : "false");
    }
}

void JsonWriter::null() noexcept {
    if (begin_value()) {
        (void)put_text("null");
    }
}

bool JsonWriter::ok() const noexcept {
    return !failed_ && depth_ == 0u && top_written_;
}

std::size_t JsonWriter::size() const noexcept {
    return size_;
}

}  // namespace stxweb
