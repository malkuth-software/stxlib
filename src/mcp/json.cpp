// JSON reading and writing (include/stxlib/json.hpp).
//
// The reader is one loop over the text with an explicit stack of open
// containers (at most JsonLimits::max_depth frames, checked before a frame
// is pushed), so it never recurses. Nodes are appended in document order;
// each container's children are linked through next_sibling, the frame
// remembering the last child so linking is constant time.
//
// Storage: the node array grows by push, each push checked against
// max_nodes first; the arena is sized once to the input's length (decoded
// strings and copied number texts together never exceed the input: an
// escape decodes to at most its own length, and every byte is copied at
// most once), then cut to what was used.

#include "stxlib/json.hpp"

#include "stxlib/numeric_text.hpp"

namespace stxlib {

namespace {

constexpr Operation parse_operation = "stxlib.json.parse";
constexpr Operation write_operation = "stxlib.json.write";

// The null root of an empty document.
constexpr detail::JsonNode empty_root{};

constexpr u32 no_node = 0; // 0 is the root, never a child or a sibling

ErrorRecord parse_failure(const usize offset, const TextView message) noexcept {
    const u32 code = offset <= u32_max ? static_cast<u32>(offset) : u32_max;
    const ErrorRecord result =
        ErrorRecord::make(ErrorKind::invalid_argument, parse_operation, ErrorDetail{.code = code, .message = message});
    return result;
}

ErrorRecord parse_limit(const TextView message) noexcept {
    const ErrorRecord result =
        ErrorRecord::make(ErrorKind::limit_exceeded, parse_operation, ErrorDetail{.message = message});
    return result;
}

bool is_whitespace(const char byte) noexcept { return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r'; }
bool is_digit(const char byte) noexcept { return byte >= '0' && byte <= '9'; }

// The value of one hexadecimal digit, or 16 for another byte.
u32 hex_value(const char byte) noexcept {
    if (byte >= '0' && byte <= '9') return static_cast<u32>(byte - '0');
    if (byte >= 'a' && byte <= 'f') return static_cast<u32>(byte - 'a' + 10);
    if (byte >= 'A' && byte <= 'F') return static_cast<u32>(byte - 'A' + 10);
    return 16;
}

// Bytes of `code_point` in UTF-8 written at `out` (room for 4). Requirement:
// a scalar value.
usize encode_utf8(const u32 code_point, char* const out) noexcept {
    if (code_point < 0x80) {
        out[0] = static_cast<char>(code_point);
        return 1;
    }
    if (code_point < 0x800) {
        out[0] = static_cast<char>(0xC0 | (code_point >> 6));
        out[1] = static_cast<char>(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (code_point >> 12));
        out[1] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (code_point & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (code_point >> 18));
    out[1] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (code_point & 0x3F));
    return 4;
}

constexpr double integer_exact_limit = 9007199254740992.0; // 2^53

} // namespace

TextView json_kind_name(const JsonKind kind) noexcept {
    switch (kind) {
    case JsonKind::null_value:
        return "null";
    case JsonKind::boolean:
        return "boolean";
    case JsonKind::number:
        return "number";
    case JsonKind::string:
        return "string";
    case JsonKind::array:
        return "array";
    case JsonKind::object:
        return "object";
    }
    return "unknown";
}

// ---- the parser -------------------------------------------------------------

class JsonParser final {
public:
    JsonParser(const TextView text, const JsonLimits& limits, JsonDocument& document) noexcept
        : text_(text), bytes_(text.data()), count_(text.count()), limits_(limits), document_(document) {}

    [[nodiscard]] OperationStatus run() noexcept;

private:
    struct Frame final {
        u32 node;
        u32 last_child;
        bool object;
    };

    // The pending member name of an object, attached to the next value.
    struct PendingKey final {
        u32 offset;
        u32 count;
    };

    void skip_whitespace() noexcept {
        while (position_ < count_ && is_whitespace(bytes_[position_])) position_ = position_ + 1;
    }
    [[nodiscard]] bool at_end() const noexcept { return position_ >= count_; }
    [[nodiscard]] char peek() const noexcept { return bytes_[position_]; }

    // Appends a node for the next value and links it under the open
    // container (with the pending key when it is a member). Fails on the
    // node limit, out_of_memory or a repeated name.
    [[nodiscard]] OperationResult<u32> add_node(JsonKind kind) noexcept;
    // Decodes a string at position_ (on its opening quote) into the arena.
    [[nodiscard]] OperationResult<PendingKey> read_string() noexcept;
    // Reads a number at position_ into `node`.
    [[nodiscard]] OperationStatus read_number(u32 node) noexcept;
    // Reads the literal `word` at position_.
    [[nodiscard]] bool read_word(TextView word) noexcept;
    // One value's start: a scalar is read whole; a container's node is made
    // and its frame pushed. `closed_now` is set when the container is empty
    // and closed at once.
    [[nodiscard]] OperationStatus begin_value() noexcept;
    [[nodiscard]] TextView arena_view(u32 offset, u32 count) const noexcept;

    TextView text_;
    const char* bytes_;
    usize count_;
    usize position_{0};
    JsonLimits limits_;
    JsonDocument& document_;
    u32 arena_used_{0};
    Frame frames_[256]{};
    u32 depth_{0};
    bool key_pending_{false};
    PendingKey key_{};
};

TextView JsonParser::arena_view(const u32 offset, const u32 count) const noexcept {
    const TextView result = detail::TextViewAccess::adopt_validated(document_.arena_.begin() + offset, count);
    return result;
}

OperationResult<u32> JsonParser::add_node(const JsonKind kind) noexcept {
    const usize index = document_.nodes_.count();
    if (index >= limits_.max_nodes) return OperationResult<u32>::failure(parse_limit("too many JSON values"));
    detail::JsonNode fresh{};
    fresh.kind = kind;
    if (depth_ > 0) {
        Frame& parent = frames_[depth_ - 1];
        detail::JsonNode& container = document_.nodes_[parent.node];
        if (parent.object) {
            if (container.child_count >= limits_.max_object_members) {
                return OperationResult<u32>::failure(parse_limit("too many members in one JSON object"));
            }
            // A repeated name is refused (header comment): compare with
            // each earlier member, at most max_object_members of them.
            const TextView name = arena_view(key_.offset, key_.count);
            u32 sibling = container.child_count == 0 ? no_node : container.first_child;
            for (u32 seen = 0; seen < container.child_count; ++seen) {
                const detail::JsonNode& earlier = document_.nodes_[sibling];
                if (arena_view(earlier.key_offset, earlier.key_count) == name) {
                    return OperationResult<u32>::failure(parse_failure(position_, "a repeated name in a JSON object"));
                }
                sibling = earlier.next_sibling;
            }
            fresh.key_offset = key_.offset;
            fresh.key_count = key_.count;
            key_pending_ = false;
        }
    }
    const OperationStatus pushed = document_.nodes_.push(static_cast<detail::JsonNode&&>(fresh));
    if (pushed.failed()) return OperationResult<u32>::failure(*pushed.error());
    const u32 node = static_cast<u32>(index);
    if (depth_ > 0) {
        Frame& parent = frames_[depth_ - 1];
        detail::JsonNode& container = document_.nodes_[parent.node];
        if (container.child_count == 0) {
            container.first_child = node;
        } else {
            document_.nodes_[parent.last_child].next_sibling = node;
        }
        container.child_count = container.child_count + 1;
        parent.last_child = node;
    }
    return OperationResult<u32>::success(node);
}

bool JsonParser::read_word(const TextView word) noexcept {
    if (count_ - position_ < word.count()) return false;
    for (usize index = 0; index < word.count(); ++index) {
        if (bytes_[position_ + index] != word.data()[index]) return false;
    }
    position_ = position_ + word.count();
    return true;
}

OperationResult<JsonParser::PendingKey> JsonParser::read_string() noexcept {
    // position_ is on the opening quote.
    const usize start = position_;
    position_ = position_ + 1;
    const u32 offset = arena_used_;
    char* const arena = document_.arena_.begin();
    for (;;) {
        if (at_end()) return OperationResult<PendingKey>::failure(parse_failure(start, "an unterminated JSON string"));
        const char byte = bytes_[position_];
        if (byte == '"') {
            position_ = position_ + 1;
            const PendingKey result{offset, arena_used_ - offset};
            return OperationResult<PendingKey>::success(result);
        }
        if (static_cast<unsigned char>(byte) < 0x20) {
            return OperationResult<PendingKey>::failure(
                parse_failure(position_, "a control character in a JSON string"));
        }
        if (byte != '\\') {
            // The input is UTF-8 (a TextView), so its bytes copy as they are.
            arena[arena_used_] = byte;
            arena_used_ = arena_used_ + 1;
            position_ = position_ + 1;
            continue;
        }
        if (count_ - position_ < 2) {
            return OperationResult<PendingKey>::failure(parse_failure(position_, "an unfinished JSON escape"));
        }
        const char escape = bytes_[position_ + 1];
        char simple = 0;
        switch (escape) {
        case '"':
            simple = '"';
            break;
        case '\\':
            simple = '\\';
            break;
        case '/':
            simple = '/';
            break;
        case 'b':
            simple = '\b';
            break;
        case 'f':
            simple = '\f';
            break;
        case 'n':
            simple = '\n';
            break;
        case 'r':
            simple = '\r';
            break;
        case 't':
            simple = '\t';
            break;
        case 'u':
            break;
        default:
            return OperationResult<PendingKey>::failure(parse_failure(position_, "an unknown JSON escape"));
        }
        if (escape != 'u') {
            arena[arena_used_] = simple;
            arena_used_ = arena_used_ + 1;
            position_ = position_ + 2;
            continue;
        }
        // \uXXXX, and a second one for a surrogate pair. Each escape is 6
        // input bytes and decodes to at most 3 (or 4 for a pair's 12), so
        // the arena never runs ahead of the input.
        u32 units[2] = {0, 0};
        usize unit_count = 0;
        for (; unit_count < 2; ++unit_count) {
            if (count_ - position_ < 6 || bytes_[position_] != '\\' || bytes_[position_ + 1] != 'u') break;
            u32 unit = 0;
            for (usize digit = 0; digit < 4; ++digit) {
                const u32 value = hex_value(bytes_[position_ + 2 + digit]);
                if (value == 16) {
                    return OperationResult<PendingKey>::failure(parse_failure(position_, "a bad \\u escape in JSON"));
                }
                unit = (unit << 4) | value;
            }
            units[unit_count] = unit;
            position_ = position_ + 6;
            const bool high = unit >= 0xD800 && unit <= 0xDBFF;
            if (!high) {
                unit_count = unit_count + 1;
                break;
            }
        }
        u32 code_point = units[0];
        if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            return OperationResult<PendingKey>::failure(parse_failure(position_, "an unpaired surrogate in JSON"));
        }
        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            if (unit_count < 2 || units[1] < 0xDC00 || units[1] > 0xDFFF) {
                return OperationResult<PendingKey>::failure(parse_failure(position_, "an unpaired surrogate in JSON"));
            }
            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (units[1] - 0xDC00);
        } else if (unit_count == 2) {
            // The loop read a second escape only after a high surrogate.
            return OperationResult<PendingKey>::failure(parse_failure(position_, "an unpaired surrogate in JSON"));
        }
        const usize written = encode_utf8(code_point, arena + arena_used_);
        arena_used_ = arena_used_ + static_cast<u32>(written);
    }
}

OperationStatus JsonParser::read_number(const u32 node) noexcept {
    const usize start = position_;
    if (!at_end() && peek() == '-') position_ = position_ + 1;
    if (at_end() || !is_digit(peek())) return OperationStatus::failure(parse_failure(start, "a bad JSON number"));
    if (peek() == '0') {
        position_ = position_ + 1;
    } else {
        while (!at_end() && is_digit(peek())) position_ = position_ + 1;
    }
    bool whole = true;
    if (!at_end() && peek() == '.') {
        whole = false;
        position_ = position_ + 1;
        if (at_end() || !is_digit(peek())) return OperationStatus::failure(parse_failure(start, "a bad JSON number"));
        while (!at_end() && is_digit(peek())) position_ = position_ + 1;
    }
    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
        whole = false;
        position_ = position_ + 1;
        if (!at_end() && (peek() == '+' || peek() == '-')) position_ = position_ + 1;
        if (at_end() || !is_digit(peek())) return OperationStatus::failure(parse_failure(start, "a bad JSON number"));
        while (!at_end() && is_digit(peek())) position_ = position_ + 1;
    }
    // The number's text is ASCII, so any slice of it is UTF-8; it is copied
    // into the arena so the document does not borrow the input.
    const usize length = position_ - start;
    const u32 offset = arena_used_;
    char* const arena = document_.arena_.begin();
    for (usize index = 0; index < length; ++index) arena[offset + index] = bytes_[start + index];
    arena_used_ = arena_used_ + static_cast<u32>(length);
    const TextView number_text = arena_view(offset, static_cast<u32>(length));
    const Parsed<double> real = parse_double(number_text);
    if (!real.succeeded()) return OperationStatus::failure(parse_failure(start, "a JSON number out of range"));
    detail::JsonNode& target = document_.nodes_[node];
    target.text_offset = offset;
    target.text_count = static_cast<u32>(length);
    target.real = *real.value();
    if (whole) {
        const Parsed<i64> integer = parse_signed(number_text);
        if (integer.succeeded()) {
            target.integral = true;
            target.integer = *integer.value();
            return OperationStatus::success();
        }
    }
    // A whole value of magnitude at most 2^53 counts as an integer
    // (JSON Schema 2020-12: 1.0 is an integer). Exact: the comparisons and
    // the conversion of a whole double in range are exact operations.
    const double value = target.real;
    if (value >= -integer_exact_limit && value <= integer_exact_limit) {
        const i64 truncated = static_cast<i64>(value);
        if (static_cast<double>(truncated) == value) {
            target.integral = true;
            target.integer = truncated;
        }
    }
    return OperationStatus::success();
}

OperationStatus JsonParser::begin_value() noexcept {
    if (at_end()) return OperationStatus::failure(parse_failure(position_, "a JSON value is missing"));
    const char byte = peek();
    if (byte == '{' || byte == '[') {
        const bool object = byte == '{';
        if (depth_ >= limits_.max_depth || depth_ >= sizeof(frames_) / sizeof(frames_[0])) {
            return OperationStatus::failure(parse_limit("JSON nested too deeply"));
        }
        OperationResult<u32> node = add_node(object ? JsonKind::object : JsonKind::array);
        if (node.failed()) return node.status();
        frames_[depth_] = Frame{*node.value(), no_node, object};
        depth_ = depth_ + 1;
        position_ = position_ + 1;
        return OperationStatus::success();
    }
    if (byte == '"') {
        OperationResult<u32> node = add_node(JsonKind::string);
        if (node.failed()) return node.status();
        const u32 index = *node.value();
        OperationResult<PendingKey> text = read_string();
        if (text.failed()) return text.status();
        detail::JsonNode& target = document_.nodes_[index];
        target.text_offset = (*text.value()).offset;
        target.text_count = (*text.value()).count;
        return OperationStatus::success();
    }
    if (byte == '-' || is_digit(byte)) {
        OperationResult<u32> node = add_node(JsonKind::number);
        if (node.failed()) return node.status();
        return read_number(*node.value());
    }
    const usize start = position_;
    if (read_word("true") || read_word("false")) {
        OperationResult<u32> node = add_node(JsonKind::boolean);
        if (node.failed()) return node.status();
        document_.nodes_[*node.value()].truth = byte == 't';
        return OperationStatus::success();
    }
    if (read_word("null")) {
        OperationResult<u32> node = add_node(JsonKind::null_value);
        if (node.failed()) return node.status();
        return OperationStatus::success();
    }
    return OperationStatus::failure(parse_failure(start, "not a JSON value"));
}

OperationStatus JsonParser::run() noexcept {
    // Sized once: decoded strings and number texts never exceed the input.
    if (count_ > 0) {
        const OperationStatus sized = document_.arena_.resize(count_);
        if (sized.failed()) return sized;
    }
    // The states of the loop: a value is due (at the top, after '[', ',' or
    // ':'), or a value has just ended (a scalar was read or a container
    // closed).
    bool value_due = true;
    for (;;) {
        skip_whitespace();
        if (value_due) {
            // Inside an object a value is due only after a key; a key is
            // due after '{' or ','.
            if (depth_ > 0 && frames_[depth_ - 1].object && !key_pending_) {
                if (at_end() || peek() != '"') {
                    return OperationStatus::failure(parse_failure(position_, "a JSON object member needs a name"));
                }
                OperationResult<PendingKey> name = read_string();
                if (name.failed()) return name.status();
                key_ = *name.value();
                key_pending_ = true;
                skip_whitespace();
                if (at_end() || peek() != ':') {
                    return OperationStatus::failure(parse_failure(position_, "a ':' is missing after a JSON name"));
                }
                position_ = position_ + 1;
                continue;
            }
            const u32 depth_before = depth_;
            const OperationStatus begun = begin_value();
            if (begun.failed()) return begun;
            if (depth_ > depth_before) {
                // A container opened: an immediate close makes it empty.
                skip_whitespace();
                const char closer = frames_[depth_ - 1].object ? '}' : ']';
                if (!at_end() && peek() == closer) {
                    position_ = position_ + 1;
                    depth_ = depth_ - 1;
                    value_due = false;
                }
                continue;
            }
            value_due = false;
            continue;
        }
        // A value has ended.
        if (depth_ == 0) {
            if (!at_end()) return OperationStatus::failure(parse_failure(position_, "text after the JSON value"));
            break;
        }
        if (at_end()) return OperationStatus::failure(parse_failure(position_, "an unclosed JSON container"));
        const Frame& open = frames_[depth_ - 1];
        const char byte = peek();
        if (byte == ',') {
            position_ = position_ + 1;
            value_due = true;
            continue;
        }
        const char closer = open.object ? '}' : ']';
        if (byte != closer) return OperationStatus::failure(parse_failure(position_, "a ',' or a close is missing"));
        position_ = position_ + 1;
        depth_ = depth_ - 1;
    }
    document_.arena_.truncate(arena_used_);
    return OperationStatus::success();
}

OperationResult<JsonDocument> JsonDocument::parse(const TextView text, const JsonLimits& limits) noexcept {
    usize byte_limit = limits.max_bytes;
    if (byte_limit > u32_max) byte_limit = u32_max;
    if (text.count() > byte_limit) {
        return OperationResult<JsonDocument>::failure(parse_limit("the JSON text is longer than its limit"));
    }
    JsonDocument document;
    JsonParser parser(text, limits, document);
    const OperationStatus parsed = parser.run();
    if (parsed.failed()) return OperationResult<JsonDocument>::failure(*parsed.error());
    return OperationResult<JsonDocument>::success(static_cast<JsonDocument&&>(document));
}

OperationResult<JsonDocument> JsonDocument::parse(const TextView text) noexcept {
    const JsonLimits limits{};
    OperationResult<JsonDocument> result = parse(text, limits);
    return result;
}

// ---- document queries ---------------------------------------------------------

const detail::JsonNode& JsonDocument::node(const JsonRef value) const noexcept {
    if (nodes_.empty()) {
        STXLIB_REQUIRE(value.index == 0, "JsonDocument: a reference outside the document");
        return empty_root;
    }
    STXLIB_REQUIRE(value.index < nodes_.count(), "JsonDocument: a reference outside the document");
    return nodes_[value.index];
}

TextView JsonDocument::arena_text(const u32 offset, const u32 count) const noexcept {
    if (count == 0) return TextView();
    const TextView result = detail::TextViewAccess::adopt_validated(arena_.begin() + offset, count);
    return result;
}

JsonKind JsonDocument::kind(const JsonRef value) const noexcept { return node(value).kind; }

bool JsonDocument::is(const JsonRef value, const JsonKind expected) const noexcept {
    const bool result = node(value).kind == expected;
    return result;
}

Optional<bool> JsonDocument::boolean(const JsonRef value) const noexcept {
    const detail::JsonNode& found = node(value);
    if (found.kind != JsonKind::boolean) return Optional<bool>::none();
    return Optional<bool>::of(found.truth);
}

Optional<i64> JsonDocument::integer(const JsonRef value) const noexcept {
    const detail::JsonNode& found = node(value);
    if (found.kind != JsonKind::number || !found.integral) return Optional<i64>::none();
    return Optional<i64>::of(found.integer);
}

Optional<double> JsonDocument::number(const JsonRef value) const noexcept {
    const detail::JsonNode& found = node(value);
    if (found.kind != JsonKind::number) return Optional<double>::none();
    return Optional<double>::of(found.real);
}

Optional<TextView> JsonDocument::text(const JsonRef value) const& noexcept {
    const detail::JsonNode& found = node(value);
    if (found.kind != JsonKind::string) return Optional<TextView>::none();
    return Optional<TextView>::of(arena_text(found.text_offset, found.text_count));
}

Optional<TextView> JsonDocument::number_text(const JsonRef value) const& noexcept {
    const detail::JsonNode& found = node(value);
    if (found.kind != JsonKind::number) return Optional<TextView>::none();
    return Optional<TextView>::of(arena_text(found.text_offset, found.text_count));
}

usize JsonDocument::child_count(const JsonRef value) const noexcept {
    const detail::JsonNode& found = node(value);
    if (found.kind != JsonKind::array && found.kind != JsonKind::object) return 0;
    return found.child_count;
}

Optional<JsonRef> JsonDocument::first_child(const JsonRef value) const noexcept {
    if (child_count(value) == 0) return Optional<JsonRef>::none();
    return Optional<JsonRef>::of(JsonRef{node(value).first_child});
}

Optional<JsonRef> JsonDocument::next_sibling(const JsonRef child) const noexcept {
    const u32 next = node(child).next_sibling;
    if (next == no_node) return Optional<JsonRef>::none();
    return Optional<JsonRef>::of(JsonRef{next});
}

Optional<JsonRef> JsonDocument::element(const JsonRef array, const usize position) const noexcept {
    const detail::JsonNode& found = node(array);
    if (found.kind != JsonKind::array || position >= found.child_count) return Optional<JsonRef>::none();
    u32 index = found.first_child;
    for (usize step = 0; step < position; ++step) index = nodes_[index].next_sibling;
    return Optional<JsonRef>::of(JsonRef{index});
}

Optional<JsonRef> JsonDocument::member(const JsonRef object, const TextView name) const noexcept {
    const detail::JsonNode& found = node(object);
    if (found.kind != JsonKind::object) return Optional<JsonRef>::none();
    u32 index = found.first_child;
    for (u32 step = 0; step < found.child_count; ++step) {
        const detail::JsonNode& child = nodes_[index];
        if (arena_text(child.key_offset, child.key_count) == name) return Optional<JsonRef>::of(JsonRef{index});
        index = child.next_sibling;
    }
    return Optional<JsonRef>::none();
}

TextView JsonDocument::key(const JsonRef child) const& noexcept {
    const detail::JsonNode& found = node(child);
    const TextView result = arena_text(found.key_offset, found.key_count);
    return result;
}

Optional<TextView> JsonDocument::member_text(const JsonRef object, const TextView name) const& noexcept {
    const Optional<JsonRef> found = member(object, name);
    if (found.absent()) return Optional<TextView>::none();
    return text(*found.value());
}

Optional<i64> JsonDocument::member_integer(const JsonRef object, const TextView name) const noexcept {
    const Optional<JsonRef> found = member(object, name);
    if (found.absent()) return Optional<i64>::none();
    return integer(*found.value());
}

Optional<bool> JsonDocument::member_boolean(const JsonRef object, const TextView name) const noexcept {
    const Optional<JsonRef> found = member(object, name);
    if (found.absent()) return Optional<bool>::none();
    return boolean(*found.value());
}

// ---- the writer -----------------------------------------------------------------

JsonWriter::JsonWriter(Utf8String& output, const usize byte_limit) noexcept : output_(output), byte_limit_(byte_limit) {}

void JsonWriter::fail(const ErrorKind kind, const TextView message) noexcept {
    if (failure_.present()) return;
    failure_ = Optional<ErrorRecord>::of(ErrorRecord::make(kind, write_operation, ErrorDetail{.message = message}));
}

void JsonWriter::append(const TextView text) noexcept {
    if (failure_.present()) return;
    if (text.count() > byte_limit_ || output_.count() > byte_limit_ - text.count()) {
        fail(ErrorKind::limit_exceeded, "the JSON text would pass its byte limit");
        return;
    }
    const OperationStatus appended = output_.append(text);
    if (appended.failed()) fail(ErrorKind::out_of_memory, "the JSON text could not grow");
}

void JsonWriter::append_escaped(const TextView text) noexcept {
    append("\"");
    const char* const bytes = text.data();
    const usize count = text.count();
    usize run_start = 0;
    usize index = 0;
    while (index < count && !failure_.present()) {
        const unsigned char byte = static_cast<unsigned char>(bytes[index]);
        usize escaped_length = 0;
        const char* replacement = nullptr;
        char hex[7] = {'\\', 'u', '0', '0', '0', '0', 0};
        if (byte == '"') {
            replacement = "\\\"";
            escaped_length = 1;
        } else if (byte == '\\') {
            replacement = "\\\\";
            escaped_length = 1;
        } else if (byte == '\n') {
            replacement = "\\n";
            escaped_length = 1;
        } else if (byte == '\r') {
            replacement = "\\r";
            escaped_length = 1;
        } else if (byte == '\t') {
            replacement = "\\t";
            escaped_length = 1;
        } else if (byte == '\b') {
            replacement = "\\b";
            escaped_length = 1;
        } else if (byte == '\f') {
            replacement = "\\f";
            escaped_length = 1;
        } else if (byte < 0x20 || byte == 0x7F) {
            constexpr char digits[] = "0123456789abcdef";
            hex[4] = digits[byte >> 4];
            hex[5] = digits[byte & 0xF];
            replacement = hex;
            escaped_length = 1;
        } else if (byte == 0xE2 && index + 2 < count && static_cast<unsigned char>(bytes[index + 1]) == 0x80 &&
                   (static_cast<unsigned char>(bytes[index + 2]) == 0xA8 ||
                    static_cast<unsigned char>(bytes[index + 2]) == 0xA9)) {
            // U+2028 and U+2029: line separators to some readers.
            replacement = static_cast<unsigned char>(bytes[index + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
            escaped_length = 3;
        }
        if (replacement == nullptr) {
            index = index + 1;
            continue;
        }
        // The run before the escape ends at a whole sequence (it ends before
        // an ASCII byte or before U+2028's lead byte), so it is UTF-8.
        if (index > run_start) {
            append(detail::TextViewAccess::adopt_validated(bytes + run_start, index - run_start));
        }
        usize replacement_count = 0;
        while (replacement[replacement_count] != 0) replacement_count = replacement_count + 1;
        append(detail::TextViewAccess::adopt_validated(replacement, replacement_count));
        index = index + escaped_length;
        run_start = index;
    }
    if (count > run_start) append(detail::TextViewAccess::adopt_validated(bytes + run_start, count - run_start));
    append("\"");
}

bool JsonWriter::before_value() noexcept {
    if (failure_.present()) return false;
    if (depth_ == 0) {
        if (has_value_[0]) {
            fail(ErrorKind::invalid_argument, "a second top-level JSON value");
            return false;
        }
        return true;
    }
    const Frame top = frames_[depth_ - 1];
    if (top == Frame::object_awaiting_key) {
        fail(ErrorKind::invalid_argument, "a JSON value where a member name is due");
        return false;
    }
    if (top == Frame::array && has_value_[depth_]) append(",");
    return !failure_.present();
}

void JsonWriter::after_value() noexcept {
    if (failure_.present()) return;
    has_value_[depth_] = true;
    if (depth_ > 0 && frames_[depth_ - 1] == Frame::object_awaiting_value) {
        frames_[depth_ - 1] = Frame::object_awaiting_key;
    }
}

void JsonWriter::begin_object() noexcept {
    if (!before_value()) return;
    if (depth_ == json_writer_depth_limit) {
        fail(ErrorKind::limit_exceeded, "JSON nested too deeply to write");
        return;
    }
    append("{");
    frames_[depth_] = Frame::object_awaiting_key;
    depth_ = depth_ + 1;
    has_value_[depth_] = false;
}

void JsonWriter::end_object() noexcept {
    if (failure_.present()) return;
    if (depth_ == 0 || frames_[depth_ - 1] != Frame::object_awaiting_key) {
        fail(ErrorKind::invalid_argument, "end_object without an open object between members");
        return;
    }
    append("}");
    depth_ = depth_ - 1;
    after_value();
}

void JsonWriter::begin_array() noexcept {
    if (!before_value()) return;
    if (depth_ == json_writer_depth_limit) {
        fail(ErrorKind::limit_exceeded, "JSON nested too deeply to write");
        return;
    }
    append("[");
    frames_[depth_] = Frame::array;
    depth_ = depth_ + 1;
    has_value_[depth_] = false;
}

void JsonWriter::end_array() noexcept {
    if (failure_.present()) return;
    if (depth_ == 0 || frames_[depth_ - 1] != Frame::array) {
        fail(ErrorKind::invalid_argument, "end_array without an open array");
        return;
    }
    append("]");
    depth_ = depth_ - 1;
    after_value();
}

void JsonWriter::key(const TextView name) noexcept {
    if (failure_.present()) return;
    if (depth_ == 0 || frames_[depth_ - 1] != Frame::object_awaiting_key) {
        fail(ErrorKind::invalid_argument, "a JSON member name outside an object or before a value");
        return;
    }
    if (has_value_[depth_]) append(",");
    append_escaped(name);
    append(":");
    frames_[depth_ - 1] = Frame::object_awaiting_value;
}

void JsonWriter::null_value() noexcept {
    if (!before_value()) return;
    append("null");
    after_value();
}

void JsonWriter::boolean(const bool value) noexcept {
    if (!before_value()) return;
    append(value ? TextView("true") : TextView("false"));
    after_value();
}

void JsonWriter::integer(const i64 value) noexcept {
    if (!before_value()) return;
    char digits[32]{};
    const Formatted formatted = format_signed(MutableView<char>(digits), value);
    if (!formatted.succeeded()) {
        fail(ErrorKind::internal_failure, "an integer did not format");
        return;
    }
    append(*formatted.text());
    after_value();
}

void JsonWriter::unsigned_integer(const u64 value) noexcept {
    if (!before_value()) return;
    char digits[32]{};
    const Formatted formatted = format_unsigned(MutableView<char>(digits), value);
    if (!formatted.succeeded()) {
        fail(ErrorKind::internal_failure, "an integer did not format");
        return;
    }
    append(*formatted.text());
    after_value();
}

void JsonWriter::number(const double value) noexcept {
    // NaN fails both comparisons; the infinities fail one.
    const bool finite = value >= -1.7976931348623157e308 && value <= 1.7976931348623157e308;
    if (!finite) {
        fail(ErrorKind::invalid_argument, "JSON has no NaN or infinity");
        return;
    }
    if (!before_value()) return;
    char digits[64]{};
    const Formatted formatted = format_real(MutableView<char>(digits), value);
    if (!formatted.succeeded()) {
        fail(ErrorKind::internal_failure, "a number did not format");
        return;
    }
    append(*formatted.text());
    after_value();
}

void JsonWriter::string(const TextView value) noexcept {
    if (!before_value()) return;
    append_escaped(value);
    after_value();
}

void JsonWriter::base64(const View<Byte> bytes) noexcept {
    if (!before_value()) return;
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    append("\"");
    // Encoded in chunks of 48 input bytes (64 characters), so one append
    // per chunk and no allocation of our own.
    char chunk[64]{};
    usize filled = 0;
    const usize count = bytes.count();
    const Byte* const data = bytes.data();
    usize index = 0;
    while (index + 3 <= count && !failure_.present()) {
        const u32 triple = (static_cast<u32>(data[index]) << 16) | (static_cast<u32>(data[index + 1]) << 8) |
                           static_cast<u32>(data[index + 2]);
        chunk[filled] = alphabet[(triple >> 18) & 0x3F];
        chunk[filled + 1] = alphabet[(triple >> 12) & 0x3F];
        chunk[filled + 2] = alphabet[(triple >> 6) & 0x3F];
        chunk[filled + 3] = alphabet[triple & 0x3F];
        filled = filled + 4;
        index = index + 3;
        if (filled == sizeof chunk) {
            append(detail::TextViewAccess::adopt_validated(chunk, filled));
            filled = 0;
        }
    }
    const usize rest = count - index;
    if (rest > 0) {
        const u32 first = static_cast<u32>(data[index]) << 16;
        const u32 second = rest == 2 ? static_cast<u32>(data[index + 1]) << 8 : 0;
        const u32 triple = first | second;
        chunk[filled] = alphabet[(triple >> 18) & 0x3F];
        chunk[filled + 1] = alphabet[(triple >> 12) & 0x3F];
        chunk[filled + 2] = rest == 2 ? alphabet[(triple >> 6) & 0x3F] : '=';
        chunk[filled + 3] = '=';
        filled = filled + 4;
    }
    if (filled > 0) append(detail::TextViewAccess::adopt_validated(chunk, filled));
    append("\"");
    after_value();
}

void JsonWriter::raw(const TextView json) noexcept {
    if (!before_value()) return;
    append(json);
    after_value();
}

void JsonWriter::copy(const JsonDocument& document, const JsonRef value) noexcept {
    // An explicit stack of open containers: each frame holds the next child
    // to write, absent once its children are done.
    struct CopyFrame final {
        JsonRef container;
        Optional<JsonRef> next;
        bool object;
    };
    CopyFrame frames[json_writer_depth_limit]{};
    u32 depth = 0;
    JsonRef current = value;
    bool have_current = true;
    while (!failure_.present()) {
        if (have_current) {
            const JsonKind kind = document.kind(current);
            if (kind == JsonKind::object || kind == JsonKind::array) {
                if (depth == json_writer_depth_limit) {
                    fail(ErrorKind::limit_exceeded, "JSON nested too deeply to write");
                    return;
                }
                if (kind == JsonKind::object) {
                    begin_object();
                } else {
                    begin_array();
                }
                frames[depth] = CopyFrame{current, document.first_child(current), kind == JsonKind::object};
                depth = depth + 1;
            } else if (kind == JsonKind::string) {
                string(document.text(current).value_or(TextView()));
            } else if (kind == JsonKind::number) {
                raw(document.number_text(current).value_or(TextView("0")));
            } else if (kind == JsonKind::boolean) {
                boolean(document.boolean(current).value_or(false));
            } else {
                null_value();
            }
            have_current = false;
            continue;
        }
        if (depth == 0) return;
        CopyFrame& top = frames[depth - 1];
        if (top.next.present()) {
            const JsonRef child = *top.next.value();
            top.next = document.next_sibling(child);
            if (top.object) key(document.key(child));
            current = child;
            have_current = true;
            continue;
        }
        if (top.object) {
            end_object();
        } else {
            end_array();
        }
        depth = depth - 1;
    }
}

void JsonWriter::member_string(const TextView name, const TextView value) noexcept {
    key(name);
    string(value);
}

void JsonWriter::member_integer(const TextView name, const i64 value) noexcept {
    key(name);
    integer(value);
}

void JsonWriter::member_boolean(const TextView name, const bool value) noexcept {
    key(name);
    boolean(value);
}

void JsonWriter::member_number(const TextView name, const double value) noexcept {
    key(name);
    number(value);
}

void JsonWriter::reset() noexcept {
    depth_ = 0;
    for (u32 level = 0; level <= json_writer_depth_limit; ++level) has_value_[level] = false;
    failure_ = Optional<ErrorRecord>::none();
}

bool JsonWriter::complete() const noexcept {
    const bool result = !failure_.present() && depth_ == 0 && has_value_[0];
    return result;
}

OperationStatus JsonWriter::status() const noexcept {
    if (failure_.present()) return OperationStatus::failure(*failure_.value());
    if (!complete()) {
        const ErrorRecord unfinished = ErrorRecord::make(ErrorKind::invalid_argument, write_operation,
                                                         ErrorDetail{.message = "an unfinished JSON value"});
        return OperationStatus::failure(unfinished);
    }
    return OperationStatus::success();
}

} // namespace stxlib
