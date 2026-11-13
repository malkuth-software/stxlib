// json.h: the web interface's strict JSON reader and writer (C2; design
// dev/notes/planning/stages/C2-web.md, section 3).
//
// Pure code: no system call, no operating-system header, no allocation.
//
// The reader accepts RFC 8259 JSON text with these stated limits and
// strictures, and refuses everything else with a named error:
//   - at most json_document_bytes_max bytes, json_tokens_max values
//     (object, array, string, number and literal tokens; member keys
//     count as string tokens), and containers nested json_depth_max deep;
//   - whitespace is space, tab, line feed and carriage return only; no
//     byte order mark;
//   - numbers are integers only: -?(0|[1-9][0-9]*), within
//     +-(2^53 - 1) (RFC 7493, I-JSON, section 2.2: the range every JSON
//     reader holds exactly); a fraction or exponent is refused as
//     number_not_integer, so no floating point is ever parsed;
//   - strings are valid UTF-8 (no overlong form, no surrogate, nothing
//     above U+10FFFF), with no raw control character; \u escapes for
//     surrogates must form a pair;
//   - an object may not repeat a key (compared after unescaping);
//   - nothing but whitespace may follow the value.
// The document is read in one pass with an explicit stack of
// json_depth_max entries: no recursion.
//
// The result is a tape of tokens in document order. Each container token
// records where its contents end, so a value is skipped in one step.
// Strings stay in the input, escaped, until decoded on demand into the
// caller's storage.

#ifndef STXLIB_RUNTIME_WEB_JSON_H
#define STXLIB_RUNTIME_WEB_JSON_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace stxweb {

constexpr std::size_t json_document_bytes_max = 40960;
constexpr std::size_t json_tokens_max = 256;
constexpr std::size_t json_depth_max = 16;
constexpr std::int64_t json_integer_max = 9007199254740991;  // 2^53 - 1

enum class JsonKind : std::uint8_t {
    object,
    array,
    string,
    number,
    boolean_true,
    boolean_false,
    null,
};

struct JsonToken {
    JsonKind kind;
    // The token's bytes in the input: for a string, between its quotes
    // (escaped); for a number or literal, its text; for a container, from
    // its opening to its closing bracket.
    std::uint32_t offset;
    std::uint32_t length;
    // For a container: the index of the first token after its contents.
    // For a scalar: its own index plus one.
    std::uint32_t end;
    // For an object: its member count; for an array: its element count;
    // otherwise 0.
    std::uint32_t count;
    // For a number: its value; otherwise 0.
    std::int64_t integer;
};

enum class JsonError : std::uint8_t {
    empty,
    too_large,
    byte_order_mark,
    unexpected_byte,
    unexpected_end,
    too_deep,
    too_many_tokens,
    literal_malformed,
    number_malformed,
    number_not_integer,
    number_out_of_range,
    string_unterminated,
    string_control_byte,
    escape_malformed,
    surrogate_unpaired,
    utf8_malformed,
    key_repeated,
    trailing_bytes,
};

[[nodiscard]] const char* json_error_name(JsonError error) noexcept;

// A parsed document. Large (about 6 KiB); the caller owns one and reuses
// it. It borrows the input text, which must outlive it and stay unchanged.
class JsonDocument {
public:
    JsonDocument() noexcept;
    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;

    [[nodiscard]] std::size_t token_count() const noexcept;
    // index below token_count (asserted).
    [[nodiscard]] const JsonToken& token(std::size_t index) const noexcept;
    [[nodiscard]] std::span<const char> text() const noexcept;

    // In the object at object_index, the value of the member whose key
    // equals key (compared after unescaping); nothing when absent or when
    // object_index is not an object.
    [[nodiscard]] std::optional<std::size_t> member(std::size_t object_index, std::span<const char> key) const noexcept;
    [[nodiscard]] std::optional<std::size_t> member(std::size_t object_index, const char* key) const noexcept;

    // Unescapes the string at index into destination. Returns its byte
    // count, or nothing when it is not a string or does not fit.
    [[nodiscard]] std::optional<std::size_t> decode_string(std::size_t index, std::span<char> destination) const noexcept;

    // Whether the string at index, unescaped, equals bytes.
    [[nodiscard]] bool string_equals(std::size_t index, std::span<const char> bytes) const noexcept;

private:
    friend class JsonParser;

    std::span<const char> text_;
    JsonToken tokens_[json_tokens_max];
    std::size_t token_count_;
};

struct JsonParsed {
};
struct JsonRefused {
    JsonError error;
    // Byte offset where the document was refused.
    std::size_t offset;
};

class JsonOutcome {
public:
    [[nodiscard]] static JsonOutcome parsed() noexcept;
    [[nodiscard]] static JsonOutcome refused(JsonError error, std::size_t offset) noexcept;
    [[nodiscard]] bool is_parsed() const noexcept;
    [[nodiscard]] const JsonRefused* as_refused() const noexcept;

private:
    using Alternatives = std::variant<JsonParsed, JsonRefused>;
    explicit JsonOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// Parses text into document. On failure the document is empty (no tokens).
[[nodiscard]] JsonOutcome parse_json(std::span<const char> text, JsonDocument& document) noexcept;

// ---------------------------------------------------------------------------
// UTF-8, shared with the writer and the tests.

// The length of the well-formed UTF-8 sequence at the front of bytes
// (Unicode 15.0, table 3-7), or 0 when it is not well formed or is cut off.
[[nodiscard]] std::size_t utf8_sequence_length(std::span<const char> bytes) noexcept;

// ---------------------------------------------------------------------------
// The writer.

// Writes JSON text into storage the caller supplies, left to right. Every
// byte checks capacity first; a write that does not fit, or a call that
// would make the text malformed (a value where a key belongs, a key in an
// array, an end that matches no begin, more than json_depth_max levels),
// stops there and marks the writer failed, permanently; later calls write
// nothing. A failed writer's text may end in a partial value and is never
// to be sent; ok() says which.
//
// Strings are escaped: '"', '\\' and control bytes; bytes that are not
// well-formed UTF-8 are replaced by U+FFFD, so the output is always valid
// JSON whatever the input bytes.
class JsonWriter {
public:
    explicit JsonWriter(std::span<char> storage) noexcept;
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

    void begin_object() noexcept;
    void end_object() noexcept;
    void begin_array() noexcept;
    void end_array() noexcept;
    // A member's key, inside an object.
    void key(const char* name) noexcept;
    void string(std::span<const char> bytes) noexcept;
    void string(const char* text) noexcept;
    void integer(std::int64_t value) noexcept;
    void unsigned_integer(std::uint64_t value) noexcept;
    void boolean(bool value) noexcept;
    void null() noexcept;

    // Complete: no failure and every container closed, and one top-level
    // value written.
    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    enum class Frame : std::uint8_t {
        object_expect_key,
        object_expect_value,
        array,
    };

    // Before a value: the comma, and the container's state.
    [[nodiscard]] bool begin_value() noexcept;
    [[nodiscard]] bool put(char byte) noexcept;
    [[nodiscard]] bool put_text(const char* text) noexcept;
    void put_string_body(std::span<const char> bytes) noexcept;

    std::span<char> storage_;
    std::size_t size_;
    bool failed_;
    bool top_written_;
    Frame frames_[json_depth_max];
    // Whether the container at each level has had an element.
    bool has_element_[json_depth_max];
    std::size_t depth_;
};

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_JSON_H
