// http.cpp: the HTTP/1.1 request reader and response head writer (http.h).

#include "http.h"

#include <cassert>

namespace stxweb {

const char* method_name(Method method) noexcept {
    switch (method) {
    case Method::get: return "GET";
    case Method::head: return "HEAD";
    case Method::post: return "POST";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "UNKNOWN";
}

HeadOutcome::HeadOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

HeadOutcome HeadOutcome::parsed(const RequestHead& head) noexcept {
    const HeadOutcome outcome(Alternatives(std::in_place_type<RequestHead>, head));
    return outcome;
}

HeadOutcome HeadOutcome::refused(std::uint16_t status, const char* reason) noexcept {
    HeadRefused refusal{};
    refusal.status = status;
    refusal.reason = reason;
    const HeadOutcome outcome(Alternatives(std::in_place_type<HeadRefused>, refusal));
    return outcome;
}

const RequestHead* HeadOutcome::as_head() const noexcept {
    return std::get_if<RequestHead>(&alternatives_);
}

const HeadRefused* HeadOutcome::as_refused() const noexcept {
    return std::get_if<HeadRefused>(&alternatives_);
}

std::span<const char> range_text(std::span<const char> bytes, const ByteRange& range) noexcept {
    assert(range.offset <= bytes.size() && range.length <= bytes.size() - range.offset);
    return bytes.subspan(range.offset, range.length);
}

// ---------------------------------------------------------------------------
// Character classes (RFC 9110 section 5.6.2, RFC 3986 section 3.3).

namespace {

[[nodiscard]] bool is_alpha(char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

[[nodiscard]] bool is_digit(char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

[[nodiscard]] bool is_hex(char byte) noexcept {
    return is_digit(byte) || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

// tchar: "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." / "^" /
// "_" / "`" / "|" / "~" / DIGIT / ALPHA
[[nodiscard]] bool is_tchar(char byte) noexcept {
    if (is_alpha(byte) || is_digit(byte)) {
        return true;
    }
    switch (byte) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        break;
    }
    return false;
}

// pchar without pct-encoded: unreserved / sub-delims / ":" / "@"
[[nodiscard]] bool is_pchar_plain(char byte) noexcept {
    if (is_alpha(byte) || is_digit(byte)) {
        return true;
    }
    switch (byte) {
    case '-': case '.': case '_': case '~':
    case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';': case '=':
    case ':': case '@':
        return true;
    default:
        break;
    }
    return false;
}

// Visible ASCII, space and tab: what a field value may hold here.
[[nodiscard]] bool is_field_byte(char byte) noexcept {
    const unsigned int value = static_cast<unsigned int>(static_cast<unsigned char>(byte));
    return (value >= 0x21u && value <= 0x7eu) || byte == ' ' || byte == '\t';
}

// What a Host value may hold: reg-name characters, digits, ".", ":" and
// IPv6 literal brackets.
[[nodiscard]] bool is_host_byte(char byte) noexcept {
    return is_alpha(byte) || is_digit(byte) || byte == '.' || byte == '-' || byte == ':' || byte == '[' ||
           byte == ']' || byte == '_' || byte == '~';
}

[[nodiscard]] char lower_of(char byte) noexcept {
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
}

[[nodiscard]] ByteRange range_of(std::size_t offset, std::size_t length) noexcept {
    ByteRange range{};
    range.offset = static_cast<std::uint32_t>(offset);
    range.length = static_cast<std::uint32_t>(length);
    return range;
}

}  // namespace

bool equals_lower(std::span<const char> bytes, const char* lower) noexcept {
    std::size_t index = 0;
    for (; index < bytes.size(); index += 1) {
        if (lower[index] == '\0' || lower_of(bytes[index]) != lower[index]) {
            return false;
        }
    }
    return lower[index] == '\0';
}

bool equals_exact(std::span<const char> bytes, const char* text) noexcept {
    std::size_t index = 0;
    for (; index < bytes.size(); index += 1) {
        if (text[index] == '\0' || bytes[index] != text[index]) {
            return false;
        }
    }
    return text[index] == '\0';
}

// ---------------------------------------------------------------------------
// The head.

namespace {

// The fields the reader looks at, and whether each was seen.
enum class KnownField : std::uint8_t {
    host,
    cookie,
    content_type,
    origin,
    csrf_token,
    content_length,
    transfer_encoding,
    expect,
    other,
};

[[nodiscard]] KnownField known_field(std::span<const char> name) noexcept {
    if (equals_lower(name, "host")) {
        return KnownField::host;
    }
    if (equals_lower(name, "cookie")) {
        return KnownField::cookie;
    }
    if (equals_lower(name, "content-type")) {
        return KnownField::content_type;
    }
    if (equals_lower(name, "origin")) {
        return KnownField::origin;
    }
    if (equals_lower(name, "x-stx-csrf")) {
        return KnownField::csrf_token;
    }
    if (equals_lower(name, "content-length")) {
        return KnownField::content_length;
    }
    if (equals_lower(name, "transfer-encoding")) {
        return KnownField::transfer_encoding;
    }
    if (equals_lower(name, "expect")) {
        return KnownField::expect;
    }
    return KnownField::other;
}

// The request-target's checks; returns the refusal or nothing.
[[nodiscard]] std::optional<HeadRefused> check_target(std::span<const char> target) noexcept {
    HeadRefused refusal{};
    refusal.status = 400;
    if (target.size() > http_target_bytes_max) {
        refusal.status = 414;
        refusal.reason = "target_too_long";
        const std::optional<HeadRefused> result = refusal;
        return result;
    }
    if (target.empty() || target[0] != '/') {
        refusal.reason = "target_not_origin_form";
        const std::optional<HeadRefused> result = refusal;
        return result;
    }
    std::size_t index = 0;
    // Each pass consumes one byte or a three-byte escape.
    while (index < target.size()) {
        const char byte = target[index];
        if (byte == '%') {
            if (index + 2u >= target.size() || !is_hex(target[index + 1u]) || !is_hex(target[index + 2u])) {
                refusal.reason = "target_escape_malformed";
                const std::optional<HeadRefused> result = refusal;
                return result;
            }
            index += 3;
            continue;
        }
        if (!is_pchar_plain(byte) && byte != '/' && byte != '?') {
            refusal.reason = "target_byte";
            const std::optional<HeadRefused> result = refusal;
            return result;
        }
        index += 1;
    }
    return std::nullopt;
}

// The index of the CRLF that ends the line starting at begin, or nothing.
[[nodiscard]] std::optional<std::size_t> line_end(std::span<const char> head, std::size_t begin) noexcept {
    for (std::size_t index = begin; index + 1u < head.size(); index += 1) {
        if (head[index] == '\r') {
            if (head[index + 1u] != '\n') {
                return std::nullopt;
            }
            const std::optional<std::size_t> found = index;
            return found;
        }
        if (head[index] == '\n') {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

HeadOutcome parse_request_head(std::span<const char> head) noexcept {
    if (head.size() > http_head_bytes_max) {
        return HeadOutcome::refused(431, "head_too_large");
    }
    if (head.size() < 4u || head[head.size() - 4u] != '\r' || head[head.size() - 3u] != '\n' ||
        head[head.size() - 2u] != '\r' || head[head.size() - 1u] != '\n') {
        return HeadOutcome::refused(400, "head_unterminated");
    }
    RequestHead request{};
    request.head_length = static_cast<std::uint32_t>(head.size());
    request.header_count = 0;
    request.content_length = 0;

    // The request line.
    const std::optional<std::size_t> first_end = line_end(head, 0);
    if (!first_end.has_value()) {
        return HeadOutcome::refused(400, "line_end");
    }
    if (*first_end == 0u) {
        return HeadOutcome::refused(400, "empty_line_first");
    }
    const std::span<const char> line = head.subspan(0, *first_end);
    std::size_t method_end = 0;
    while (method_end < line.size() && is_tchar(line[method_end])) {
        method_end += 1;
    }
    if (method_end == 0u || method_end >= line.size() || line[method_end] != ' ') {
        return HeadOutcome::refused(400, "request_line");
    }
    const std::span<const char> method = line.subspan(0, method_end);
    std::size_t target_end = method_end + 1u;
    while (target_end < line.size() && line[target_end] != ' ') {
        target_end += 1;
    }
    if (target_end >= line.size()) {
        return HeadOutcome::refused(400, "request_line");
    }
    const std::span<const char> target = line.subspan(method_end + 1u, target_end - method_end - 1u);
    const std::span<const char> version = line.subspan(target_end + 1u);
    if (!equals_exact(version, "HTTP/1.1")) {
        const bool is_http_version = version.size() == 8u && equals_exact(version.subspan(0, 5), "HTTP/") &&
                                     is_digit(version[5]) && version[6] == '.' && is_digit(version[7]);
        return is_http_version ? HeadOutcome::refused(505, "version") : HeadOutcome::refused(400, "request_line");
    }
    if (equals_exact(method, "GET")) {
        request.method = Method::get;
    } else if (equals_exact(method, "HEAD")) {
        request.method = Method::head;
    } else if (equals_exact(method, "POST")) {
        request.method = Method::post;
    } else {
        return HeadOutcome::refused(501, "method");
    }
    const std::optional<HeadRefused> target_refusal = check_target(target);
    if (target_refusal.has_value()) {
        return HeadOutcome::refused((*target_refusal).status, (*target_refusal).reason);
    }
    const std::size_t target_offset = method_end + 1u;
    std::size_t question = 0;
    while (question < target.size() && target[question] != '?') {
        question += 1;
    }
    request.path = range_of(target_offset, question);
    if (question < target.size()) {
        request.query = range_of(target_offset + question + 1u, target.size() - question - 1u);
    }

    // The field lines, up to the empty line.
    bool host_seen = false;
    bool content_length_seen = false;
    std::size_t begin = *first_end + 2u;
    // Each pass consumes one line of at least two bytes.
    for (;;) {
        const std::optional<std::size_t> end = line_end(head, begin);
        if (!end.has_value()) {
            return HeadOutcome::refused(400, "line_end");
        }
        if (*end == begin) {
            break;
        }
        const std::span<const char> field = head.subspan(begin, *end - begin);
        if (field[0] == ' ' || field[0] == '\t') {
            return HeadOutcome::refused(400, "obs_fold");
        }
        if (request.header_count >= http_header_count_max) {
            return HeadOutcome::refused(431, "too_many_fields");
        }
        request.header_count += 1;
        std::size_t name_end = 0;
        while (name_end < field.size() && is_tchar(field[name_end])) {
            name_end += 1;
        }
        if (name_end == 0u || name_end >= field.size() || field[name_end] != ':') {
            return HeadOutcome::refused(400, "field_name");
        }
        std::size_t value_begin = name_end + 1u;
        std::size_t value_end = field.size();
        for (std::size_t index = value_begin; index < value_end; index += 1) {
            if (!is_field_byte(field[index])) {
                return HeadOutcome::refused(400, "field_value");
            }
        }
        while (value_begin < value_end && (field[value_begin] == ' ' || field[value_begin] == '\t')) {
            value_begin += 1;
        }
        while (value_end > value_begin && (field[value_end - 1u] == ' ' || field[value_end - 1u] == '\t')) {
            value_end -= 1;
        }
        const std::span<const char> value = field.subspan(value_begin, value_end - value_begin);
        const ByteRange value_range = range_of(begin + value_begin, value.size());
        switch (known_field(field.subspan(0, name_end))) {
        case KnownField::host:
            if (host_seen) {
                return HeadOutcome::refused(400, "host_repeated");
            }
            host_seen = true;
            if (value.empty()) {
                return HeadOutcome::refused(400, "host_empty");
            }
            for (const char byte : value) {
                if (!is_host_byte(byte)) {
                    return HeadOutcome::refused(400, "host_byte");
                }
            }
            request.host = value_range;
            break;
        case KnownField::cookie:
            if (request.cookie.has_value()) {
                return HeadOutcome::refused(400, "cookie_repeated");
            }
            request.cookie = value_range;
            break;
        case KnownField::content_type:
            if (request.content_type.has_value()) {
                return HeadOutcome::refused(400, "content_type_repeated");
            }
            request.content_type = value_range;
            break;
        case KnownField::origin:
            if (request.origin.has_value()) {
                return HeadOutcome::refused(400, "origin_repeated");
            }
            request.origin = value_range;
            break;
        case KnownField::csrf_token:
            if (request.csrf_token.has_value()) {
                return HeadOutcome::refused(400, "csrf_repeated");
            }
            request.csrf_token = value_range;
            break;
        case KnownField::content_length: {
            if (content_length_seen) {
                return HeadOutcome::refused(400, "content_length_repeated");
            }
            content_length_seen = true;
            if (value.empty() || value.size() > 10u) {
                return HeadOutcome::refused(400, "content_length");
            }
            std::uint64_t length = 0;
            for (const char byte : value) {
                if (!is_digit(byte)) {
                    return HeadOutcome::refused(400, "content_length");
                }
                length = length * 10u + static_cast<std::uint64_t>(byte - '0');
            }
            if (length > 0xffffffffu) {
                return HeadOutcome::refused(400, "content_length");
            }
            request.content_length = static_cast<std::uint32_t>(length);
            break;
        }
        case KnownField::transfer_encoding:
            return HeadOutcome::refused(501, "transfer_encoding");
        case KnownField::expect:
            return HeadOutcome::refused(417, "expect");
        case KnownField::other:
            break;
        default:
            break;
        }
        begin = *end + 2u;
    }
    if (!host_seen) {
        return HeadOutcome::refused(400, "host_missing");
    }
    if (request.method != Method::post && request.content_length != 0u) {
        return HeadOutcome::refused(400, "body_not_allowed");
    }
    if (request.content_length > http_body_bytes_max) {
        return HeadOutcome::refused(413, "body_too_large");
    }
    return HeadOutcome::parsed(request);
}

// ---------------------------------------------------------------------------
// RequestAssembler.

RequestAssembler::RequestAssembler() noexcept
    : buffer_{}, received_(0), scanned_(0), state_(AssemblyState::receiving), head_(), refusal_{} {
    refusal_.status = 0;
    refusal_.reason = "";
}

void RequestAssembler::reset() noexcept {
    received_ = 0;
    scanned_ = 0;
    state_ = AssemblyState::receiving;
    head_.reset();
    refusal_.status = 0;
    refusal_.reason = "";
}

std::span<char> RequestAssembler::free_space() noexcept {
    if (state_ != AssemblyState::receiving) {
        return std::span<char>(buffer_, 0);
    }
    std::size_t limit = http_head_bytes_max;
    if (head_.has_value()) {
        limit = static_cast<std::size_t>((*head_).head_length) + (*head_).content_length;
    }
    assert(received_ <= limit && limit <= http_request_bytes_max);
    return std::span<char>(buffer_ + received_, limit - received_);
}

void RequestAssembler::refuse(std::uint16_t status, const char* reason) noexcept {
    state_ = AssemblyState::refused;
    refusal_.status = status;
    refusal_.reason = reason;
}

// Checks the head's new bytes for line ends and looks for its end.
void RequestAssembler::scan_head() noexcept {
    for (std::size_t index = scanned_; index < received_; index += 1) {
        const char byte = buffer_[index];
        if (byte == '\n') {
            if (index == 0u || buffer_[index - 1u] != '\r') {
                refuse(400, "bare_lf");
                return;
            }
            if (index == 1u) {
                refuse(400, "empty_line_first");
                return;
            }
            if (index >= 3u && buffer_[index - 2u] == '\n' && buffer_[index - 3u] == '\r') {
                // The head ends here.
                const std::span<const char> head_bytes(buffer_, index + 1u);
                const HeadOutcome outcome = parse_request_head(head_bytes);
                const HeadRefused* const refused = outcome.as_refused();
                if (refused != nullptr) {
                    refuse((*refused).status, (*refused).reason);
                    return;
                }
                head_ = *outcome.as_head();
                scanned_ = index + 1u;
                return;
            }
            continue;
        }
        if (byte == '\r') {
            if (index + 1u >= received_) {
                // Decided when the next byte arrives.
                scanned_ = index;
                return;
            }
            if (buffer_[index + 1u] != '\n') {
                refuse(400, "bare_cr");
                return;
            }
        }
    }
    scanned_ = received_;
}

AssemblyState RequestAssembler::commit(std::size_t count) noexcept {
    assert(count <= free_space().size());
    if (state_ != AssemblyState::receiving) {
        return state_;
    }
    received_ += count;
    if (!head_.has_value()) {
        scan_head();
        if (state_ != AssemblyState::receiving) {
            return state_;
        }
        if (!head_.has_value()) {
            if (received_ >= http_head_bytes_max) {
                refuse(431, "head_too_large");
            }
            return state_;
        }
    }
    const std::size_t need = static_cast<std::size_t>((*head_).head_length) + (*head_).content_length;
    if (received_ >= need) {
        state_ = AssemblyState::complete;
    }
    return state_;
}

AssemblyState RequestAssembler::state() const noexcept {
    return state_;
}

const RequestHead* RequestAssembler::head() const noexcept {
    if (state_ != AssemblyState::complete || !head_.has_value()) {
        return nullptr;
    }
    return &*head_;
}

std::span<const char> RequestAssembler::body() const noexcept {
    if (state_ != AssemblyState::complete || !head_.has_value()) {
        return std::span<const char>(buffer_, 0);
    }
    return std::span<const char>(buffer_ + (*head_).head_length, (*head_).content_length);
}

const HeadRefused* RequestAssembler::refusal() const noexcept {
    return state_ == AssemblyState::refused ? &refusal_ : nullptr;
}

std::span<const char> RequestAssembler::bytes() const noexcept {
    return std::span<const char>(buffer_, received_);
}

std::size_t RequestAssembler::received() const noexcept {
    return received_;
}

// ---------------------------------------------------------------------------
// Cookies.

std::optional<std::span<const char>> cookie_value(std::span<const char> field, const char* name) noexcept {
    std::size_t name_length = 0;
    while (name[name_length] != '\0') {
        name_length += 1;
    }
    std::optional<std::span<const char>> found;
    std::size_t begin = 0;
    // Each pass consumes one pair and its separator.
    while (begin < field.size()) {
        std::size_t end = begin;
        while (end < field.size() && field[end] != ';') {
            end += 1;
        }
        std::size_t pair_begin = begin;
        while (pair_begin < end && field[pair_begin] == ' ') {
            pair_begin += 1;
        }
        std::size_t pair_end = end;
        while (pair_end > pair_begin && field[pair_end - 1u] == ' ') {
            pair_end -= 1;
        }
        const std::span<const char> pair = field.subspan(pair_begin, pair_end - pair_begin);
        if (pair.size() > name_length && pair[name_length] == '=' &&
            equals_exact(pair.subspan(0, name_length), name)) {
            if (found.has_value()) {
                // Twice: ambiguous, so neither counts.
                return std::nullopt;
            }
            found = pair.subspan(name_length + 1u);
        }
        begin = end + 1u;
    }
    return found;
}

// ---------------------------------------------------------------------------
// Response heads.

const char* status_reason(std::uint16_t status) noexcept {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: break;
    }
    // Fallback: a status this server does not send.
    return "Unknown";
}

namespace {

// Appends to a fixed buffer; a write that does not fit marks it full.
class HeadText {
public:
    explicit HeadText(std::span<char> out) noexcept : out_(out), size_(0), full_(false) {}

    void text(const char* value) noexcept {
        std::size_t length = 0;
        while (value[length] != '\0') {
            length += 1;
        }
        if (full_ || length > out_.size() - size_) {
            full_ = true;
            return;
        }
        for (std::size_t index = 0; index < length; index += 1) {
            out_[size_ + index] = value[index];
        }
        size_ += length;
    }

    void decimal(std::uint64_t value) noexcept {
        char digits[21];
        std::size_t count = 0;
        std::uint64_t remaining = value;
        do {
            digits[19u - count] = static_cast<char>('0' + static_cast<int>(remaining % 10u));
            remaining /= 10u;
            count += 1;
        } while (remaining != 0u);
        digits[20] = '\0';
        text(&digits[20u - count]);
    }

    [[nodiscard]] std::optional<std::size_t> result() const noexcept {
        if (full_) {
            return std::nullopt;
        }
        const std::optional<std::size_t> size = size_;
        return size;
    }

private:
    std::span<char> out_;
    std::size_t size_;
    bool full_;
};

// A field value written as given must not split the head.
[[nodiscard]] bool is_safe_value(const char* value) noexcept {
    for (std::size_t index = 0; value[index] != '\0'; index += 1) {
        if (!is_field_byte(value[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<std::size_t> write_response_head(const ResponseHeadFields& fields, std::span<char> out) noexcept {
    if ((fields.content_type != nullptr && !is_safe_value(fields.content_type)) ||
        (fields.set_cookie != nullptr && !is_safe_value(fields.set_cookie)) ||
        (fields.allow != nullptr && !is_safe_value(fields.allow))) {
        return std::nullopt;
    }
    HeadText head(out);
    head.text("HTTP/1.1 ");
    head.decimal(fields.status);
    head.text(" ");
    head.text(status_reason(fields.status));
    head.text("\r\n");
    if (fields.content_type != nullptr) {
        head.text("Content-Type: ");
        head.text(fields.content_type);
        head.text("\r\n");
    }
    head.text("Content-Length: ");
    head.decimal(fields.content_length);
    head.text("\r\n");
    if (fields.set_cookie != nullptr) {
        head.text("Set-Cookie: ");
        head.text(fields.set_cookie);
        head.text("\r\n");
    }
    if (fields.retry_after_s.has_value()) {
        head.text("Retry-After: ");
        head.decimal(*fields.retry_after_s);
        head.text("\r\n");
    }
    if (fields.allow != nullptr) {
        head.text("Allow: ");
        head.text(fields.allow);
        head.text("\r\n");
    }
    head.text(fields.cacheable ? "Cache-Control: no-cache\r\n" : "Cache-Control: no-store\r\n");
    // No Date field: the appliance has no reliable clock (no RTC, no NTP),
    // and RFC 9110 section 6.6.1 says such a server must not send one.
    head.text("Connection: close\r\n"
              "X-Content-Type-Options: nosniff\r\n"
              "X-Frame-Options: DENY\r\n"
              "Referrer-Policy: no-referrer\r\n"
              "Cross-Origin-Opener-Policy: same-origin\r\n"
              "Cross-Origin-Resource-Policy: same-origin\r\n"
              "Content-Security-Policy: default-src 'self'; base-uri 'none'; form-action 'self'; "
              "frame-ancestors 'none'\r\n"
              "\r\n");
    return head.result();
}

}  // namespace stxweb
