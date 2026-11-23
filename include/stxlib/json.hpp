#pragma once

// JSON (RFC 8259) for stx_mcp: a bounded reader that parses one text into a
// flat document, and a writer that appends JSON text to a caller's string.
// STXlib had no JSON reader or writer, so stx_mcp carries its own (notes
// planning/studies/MCP_AUTOMATION_STUDY.md, section 6.1, public notes,
// gui_forms branch). Part of the stxlib::mcp target.
//
// The document is flat: every value is a node in one array, children are
// linked by index, and every decoded string lives in one byte arena. So a
// document is two allocations whatever its shape, and neither parsing,
// reading, copying nor destroying it recurses (house style, section 1: no
// recursion over runtime-shaped data). Depth is bounded by JsonLimits and
// checked before a level is entered.
//
// Reading rules (RFC 8259, with the limits below):
// - the whole text is one value, with insignificant whitespace around it;
// - strings must be well-formed: the input is a TextView (UTF-8 by
//   construction), control characters must be escaped, and a \u escape
//   that is an unpaired surrogate is refused (it names no Unicode scalar
//   value and could not be held in UTF-8 text);
// - an object may not repeat a name (RFC 8259 section 4 says names SHOULD
//   be unique; a repeated name is ambiguous, so it is refused);
// - numbers keep their text; a number is an integer when it has no fraction
//   and no exponent and fits i64, or when its value is a whole number of
//   magnitude at most 2^53 (JSON Schema 2020-12 counts 1.0 as an integer);
//   every number also reads as the nearest double.
// A failure is invalid_argument with code = the byte offset of the problem
// (u32_max when it does not fit), or limit_exceeded for a limit, and the
// message names the problem.
//
// Identity of values: a JsonRef is an index into its own document, valid
// for that document only and while it lives. Queries on a ref of another
// document, or an out-of-range ref, fail as requirements.

#include "stxlib/optional.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

enum class JsonKind : u8 {
    null_value = 0,
    boolean = 1,
    number = 2,
    string = 3,
    array = 4,
    object = 5,
};

// A stable lower-case name ("null", "boolean", "number", "string", "array",
// "object"), for messages.
[[nodiscard]] TextView json_kind_name(JsonKind kind) noexcept;

// A value in a JsonDocument: an index, typed so it is not confused with a
// count or a byte offset.
struct JsonRef final {
    u32 index{0};

    [[nodiscard]] constexpr bool operator==(const JsonRef other) const noexcept { return index == other.index; }
    [[nodiscard]] constexpr bool operator!=(const JsonRef other) const noexcept { return index != other.index; }
};

// Bounds on one parse, checked before the storage for each value is used.
struct JsonLimits final {
    // Bytes of input; a longer text is refused before parsing.
    usize max_bytes{4 * 1024 * 1024};
    // Nesting of arrays and objects (the root's own level counts as 1).
    u32 max_depth{64};
    // Values in the document.
    u32 max_nodes{1u << 20};
    // Members of one object (names are checked for repeats, which costs the
    // square of this per object at most).
    u32 max_object_members{1024};
};

namespace detail {

// One value of a JsonDocument. Fields that do not belong to the node's
// kind are zero. Private to json.cpp's reader and the document's queries.
struct JsonNode final {
    JsonKind kind{JsonKind::null_value};
    // boolean: its value.
    bool truth{false};
    // number: whether it reads as an integer (header comment), and its
    // integer value then.
    bool integral{false};
    // array and object: their children; a member's key is its own field.
    u32 first_child{0};
    u32 child_count{0};
    // The next child of the same parent; 0 for the last (0 is the root,
    // never a child).
    u32 next_sibling{0};
    // A member of an object: its name in the arena.
    u32 key_offset{0};
    u32 key_count{0};
    // string: its decoded text in the arena. number: its source text.
    u32 text_offset{0};
    u32 text_count{0};
    i64 integer{0};
    double real{0.0};
};

} // namespace detail

class JsonDocument final {
public:
    // The empty document: one null root.
    JsonDocument() noexcept = default;
    JsonDocument(JsonDocument&&) noexcept = default;
    JsonDocument& operator=(JsonDocument&&) noexcept = default;
    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;
    ~JsonDocument() = default;

    // Parses `text` as one JSON value. The document owns copies of its
    // strings; `text` is not borrowed past the call. Failures: header
    // comment; out_of_memory.
    [[nodiscard]] static OperationResult<JsonDocument> parse(TextView text, const JsonLimits& limits) noexcept;
    [[nodiscard]] static OperationResult<JsonDocument> parse(TextView text) noexcept;

    [[nodiscard]] JsonRef root() const noexcept { return JsonRef{}; }
    [[nodiscard]] usize node_count() const noexcept { return nodes_.count(); }

    [[nodiscard]] JsonKind kind(JsonRef value) const noexcept;
    [[nodiscard]] bool is(JsonRef value, JsonKind expected) const noexcept;

    // Each answers its value when `value` has that kind, else none.
    [[nodiscard]] Optional<bool> boolean(JsonRef value) const noexcept;
    // An integer number (header comment), else none.
    [[nodiscard]] Optional<i64> integer(JsonRef value) const noexcept;
    // Any number, as the nearest double.
    [[nodiscard]] Optional<double> number(JsonRef value) const noexcept;
    // A string's decoded text, borrowed from this document (valid while it
    // lives and is not moved from or assigned to).
    [[nodiscard]] Optional<TextView> text(JsonRef value) const& noexcept;
    Optional<TextView> text(JsonRef value) const&& = delete;
    // A number's source text ("1.50e3"), borrowed as text().
    [[nodiscard]] Optional<TextView> number_text(JsonRef value) const& noexcept;
    Optional<TextView> number_text(JsonRef value) const&& = delete;

    // Children of an array or object, in source order; 0 for other kinds.
    [[nodiscard]] usize child_count(JsonRef value) const noexcept;
    // The first child, or none when there is none or `value` is not a
    // container.
    [[nodiscard]] Optional<JsonRef> first_child(JsonRef value) const noexcept;
    // The next child of the same container, or none after the last.
    [[nodiscard]] Optional<JsonRef> next_sibling(JsonRef child) const noexcept;
    // An array's element at `position` (counted, so linear), or none.
    [[nodiscard]] Optional<JsonRef> element(JsonRef array, usize position) const noexcept;
    // An object's member named `name`, or none (also when `value` is not an
    // object).
    [[nodiscard]] Optional<JsonRef> member(JsonRef object, TextView name) const noexcept;
    // The name of an object's member (a child of an object), borrowed as
    // text(); empty for any other value.
    [[nodiscard]] TextView key(JsonRef child) const& noexcept;
    TextView key(JsonRef child) const&& = delete;

    // Convenience on member(): the member's text, integer, boolean, or none
    // when absent or of another kind.
    [[nodiscard]] Optional<TextView> member_text(JsonRef object, TextView name) const& noexcept;
    Optional<TextView> member_text(JsonRef object, TextView name) const&& = delete;
    [[nodiscard]] Optional<i64> member_integer(JsonRef object, TextView name) const noexcept;
    [[nodiscard]] Optional<bool> member_boolean(JsonRef object, TextView name) const noexcept;

private:
    friend class JsonParser;

    [[nodiscard]] const detail::JsonNode& node(JsonRef value) const noexcept;
    [[nodiscard]] TextView arena_text(u32 offset, u32 count) const noexcept;

    Vector<detail::JsonNode> nodes_{};
    Vector<char> arena_{};
};

// The parser's working state, named so the document can befriend it
// (src/mcp/json.cpp). Not for use outside it.
class JsonParser;

// The most nesting a JsonWriter tracks: room for a reply's own levels
// around a tool schema of stx_mcp's deepest (32 schema levels, two JSON
// levels each).
inline constexpr u32 json_writer_depth_limit = 128;

// Appends JSON text to a Utf8String it borrows (house style, section 6: the
// caller holds the storage and sees its size). The writer checks structure:
// a key only inside an object and before each member's value, values
// separated by commas, containers closed in order. The first failure is
// kept and every later call does nothing; status() reports it:
// - invalid_argument for a structural mistake (a value where a key is due,
//   end_object inside an array, a non-finite number, finishing with an open
//   container), naming it;
// - limit_exceeded when more than json_writer_depth_limit containers are
//   open, or the text would pass the writer's byte limit;
// - out_of_memory when the string cannot grow.
// After a failure the string holds an unspecified prefix of the text.
//
// Strings are escaped per RFC 8259: '"', '\\' and the control characters
// U+0000 to U+001F (as \n, \r, \t, \b, \f or \u00XX), and U+007F, U+2028 and
// U+2029 as \u escapes, so the text never holds a line break and fits on one
// line of a newline-delimited stream.
//
// Borrows `output`: declare the writer after the string.
class JsonWriter final {
public:
    // Appends to `output` (which may already hold text: a writer begins a
    // fresh value at its end) up to `byte_limit` bytes in total.
    explicit JsonWriter(Utf8String& output, usize byte_limit = usize_max) noexcept;
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

    void begin_object() noexcept;
    void end_object() noexcept;
    void begin_array() noexcept;
    void end_array() noexcept;
    // The next member's name. Only directly inside an object, before a value.
    void key(TextView name) noexcept;

    void null_value() noexcept;
    void boolean(bool value) noexcept;
    void integer(i64 value) noexcept;
    void unsigned_integer(u64 value) noexcept;
    // The shortest text that reads back as `value`. A NaN or an infinity is
    // refused (JSON has neither).
    void number(double value) noexcept;
    void string(TextView value) noexcept;
    // `bytes` as one string of standard base64 (RFC 4648 section 4, with
    // padding), for MCP's image and blob content.
    void base64(View<Byte> bytes) noexcept;
    // A copy of `value` from `document`, written in source order without
    // recursion (an explicit stack bounded by json_writer_depth_limit).
    void copy(const JsonDocument& document, JsonRef value) noexcept;
    // Text that is already one JSON value, appended as it is. Requirement on
    // the caller: `json` is one valid JSON value on one line (stx_mcp uses it
    // for request ids it parsed and wrote itself).
    void raw(TextView json) noexcept;

    // key() then the value, for the common member.
    void member_string(TextView name, TextView value) noexcept;
    void member_integer(TextView name, i64 value) noexcept;
    void member_boolean(TextView name, bool value) noexcept;
    void member_number(TextView name, double value) noexcept;

    // Starts over: forgets the structure written so far and any failure,
    // so the next value is a fresh top-level value. The string is not
    // touched (its owner clears it when the text is to go too).
    void reset() noexcept;

    // Whether one whole value has been written (every container closed).
    [[nodiscard]] bool complete() const noexcept;
    // Success once complete() with no failure; otherwise the first failure,
    // or invalid_argument when a container is still open.
    [[nodiscard]] OperationStatus status() const noexcept;
    [[nodiscard]] bool failed() const noexcept { return failure_.present(); }

private:
    enum class Frame : u8 {
        object_awaiting_key = 0,
        object_awaiting_value = 1,
        array = 2,
    };

    // Before a value: the comma and structural check. False when the value
    // must not be written (failure recorded).
    [[nodiscard]] bool before_value() noexcept;
    void after_value() noexcept;
    void append(TextView text) noexcept;
    void append_escaped(TextView text) noexcept;
    void fail(ErrorKind kind, TextView message) noexcept;

    Utf8String& output_;
    usize byte_limit_;
    Frame frames_[json_writer_depth_limit]{};
    u32 depth_{0};
    // Whether the current container (or the top level) already holds a
    // value, so the next one needs a comma (containers) or is refused (the
    // top level holds one value).
    bool has_value_[json_writer_depth_limit + 1]{};
    // The first failure; absent while none.
    Optional<ErrorRecord> failure_{};
};

} // namespace stxlib
