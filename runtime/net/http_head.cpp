// http_head.cpp: see http_head.h.

#include "http_head.h"

namespace stxnet {

HeadOutcome::HeadOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

HeadOutcome HeadOutcome::body(const HeadBody& value) noexcept {
    return HeadOutcome(Alternatives(value));
}

HeadOutcome HeadOutcome::redirect(const HeadRedirect& value) noexcept {
    return HeadOutcome(Alternatives(value));
}

HeadOutcome HeadOutcome::refused(HeadRefusal reason, std::uint16_t status) noexcept {
    return HeadOutcome(Alternatives(HeadRefused{reason, status}));
}

const HeadBody* HeadOutcome::as_body() const noexcept {
    return std::get_if<HeadBody>(&alternatives_);
}

const HeadRedirect* HeadOutcome::as_redirect() const noexcept {
    return std::get_if<HeadRedirect>(&alternatives_);
}

const HeadRefused* HeadOutcome::as_refused() const noexcept {
    return std::get_if<HeadRefused>(&alternatives_);
}

const char* head_refusal_name(HeadRefusal reason) noexcept {
    switch (reason) {
    case HeadRefusal::status_line_malformed: return "status_line_malformed";
    case HeadRefusal::header_malformed: return "header_malformed";
    case HeadRefusal::too_many_headers: return "too_many_headers";
    case HeadRefusal::transfer_encoding: return "transfer_encoding";
    case HeadRefusal::length_missing: return "length_missing";
    case HeadRefusal::length_malformed: return "length_malformed";
    case HeadRefusal::length_conflict: return "length_conflict";
    case HeadRefusal::too_large: return "too_large";
    case HeadRefusal::location_missing: return "location_missing";
    case HeadRefusal::location_repeated: return "location_repeated";
    case HeadRefusal::status_not_accepted: return "status_not_accepted";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

std::optional<std::size_t> find_head_end(std::span<const char> bytes) noexcept {
    const std::size_t limit = bytes.size() < response_head_max ? bytes.size() : response_head_max;
    for (std::size_t index = 3; index < limit; index += 1) {
        if (bytes[index - 3u] == '\r' && bytes[index - 2u] == '\n' && bytes[index - 1u] == '\r' &&
            bytes[index] == '\n') {
            const std::optional<std::size_t> end = index + 1u;
            return end;
        }
    }
    return std::nullopt;
}

namespace {

[[nodiscard]] bool is_token_byte(char byte) noexcept {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')) {
        return true;
    }
    switch (byte) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.': case '^':
    case '_': case '`': case '|': case '~':
        return true;
    default:
        break;
    }
    return false;
}

// Visible bytes, obs-text, space and tab: a field value's or reason's.
[[nodiscard]] bool is_value_byte(char byte) noexcept {
    const unsigned char code = static_cast<unsigned char>(byte);
    return code == 0x09u || (code >= 0x20u && code != 0x7fu);
}

[[nodiscard]] bool is_blank(char byte) noexcept {
    return byte == ' ' || byte == '\t';
}

[[nodiscard]] char lower(char byte) noexcept {
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
}

[[nodiscard]] bool name_is(std::span<const char> head, std::size_t begin, std::size_t end,
                           const char* literal) noexcept {
    std::size_t index = 0;
    while (literal[index] != '\0') {
        if (begin + index >= end || lower(head[begin + index]) != literal[index]) {
            return false;
        }
        index += 1;
    }
    return begin + index == end;
}

// The end of the line starting at begin: the index of its CR, which a LF
// follows; or nothing (no CRLF, or a bare CR or LF before it).
[[nodiscard]] std::optional<std::size_t> line_end(std::span<const char> head, std::size_t begin) noexcept {
    for (std::size_t index = begin; index < head.size(); index += 1) {
        if (head[index] == '\n') {
            return std::nullopt;
        }
        if (head[index] == '\r') {
            if (index + 1u < head.size() && head[index + 1u] == '\n') {
                const std::optional<std::size_t> end = index;
                return end;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// The status code from the status line [0, end), or nothing.
[[nodiscard]] std::optional<std::uint16_t> status_of(std::span<const char> head, std::size_t end) noexcept {
    const char version[] = "HTTP/1.";
    const std::size_t version_length = sizeof(version) - 1u;
    if (end < version_length + 5u) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < version_length; index += 1) {
        if (head[index] != version[index]) {
            return std::nullopt;
        }
    }
    const std::size_t minor = version_length;
    if ((head[minor] != '0' && head[minor] != '1') || head[minor + 1u] != ' ') {
        return std::nullopt;
    }
    std::uint32_t status = 0;
    for (std::size_t index = minor + 2u; index < minor + 5u; index += 1) {
        if (head[index] < '0' || head[index] > '9') {
            return std::nullopt;
        }
        status = status * 10u + static_cast<std::uint32_t>(head[index] - '0');
    }
    const std::size_t after = minor + 5u;
    if (after < end) {
        if (head[after] != ' ') {
            return std::nullopt;
        }
        for (std::size_t index = after + 1u; index < end; index += 1) {
            if (!is_value_byte(head[index])) {
                return std::nullopt;
            }
        }
    }
    const std::optional<std::uint16_t> result = static_cast<std::uint16_t>(status);
    return result;
}

struct Field {
    std::size_t name_end;
    std::size_t value_begin;
    std::size_t value_end;
};

// A header line [begin, end) split into name and trimmed value, or nothing.
[[nodiscard]] std::optional<Field> field_of(std::span<const char> head, std::size_t begin, std::size_t end) noexcept {
    std::size_t colon = begin;
    while (colon < end && is_token_byte(head[colon])) {
        colon += 1;
    }
    if (colon == begin || colon == end || head[colon] != ':') {
        return std::nullopt;
    }
    for (std::size_t index = colon + 1u; index < end; index += 1) {
        if (!is_value_byte(head[index])) {
            return std::nullopt;
        }
    }
    std::size_t value_begin = colon + 1u;
    while (value_begin < end && is_blank(head[value_begin])) {
        value_begin += 1;
    }
    std::size_t value_end = end;
    while (value_end > value_begin && is_blank(head[value_end - 1u])) {
        value_end -= 1;
    }
    const std::optional<Field> field = Field{colon, value_begin, value_end};
    return field;
}

// A Content-Length value: 1 to 19 digits (below 10^19, inside 64 bits).
[[nodiscard]] std::optional<std::uint64_t> length_of(std::span<const char> head, std::size_t begin,
                                                     std::size_t end) noexcept {
    if (end == begin || end - begin > 19u) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t index = begin; index < end; index += 1) {
        if (head[index] < '0' || head[index] > '9') {
            return std::nullopt;
        }
        value = value * 10u + static_cast<std::uint64_t>(head[index] - '0');
    }
    const std::optional<std::uint64_t> result = value;
    return result;
}

[[nodiscard]] bool is_redirect(std::uint16_t status) noexcept {
    return status == 301u || status == 302u || status == 303u || status == 307u || status == 308u;
}

}  // namespace

HeadOutcome parse_response_head(std::span<const char> head, std::uint64_t body_max) noexcept {
    if (head.size() > response_head_max) {
        return HeadOutcome::refused(HeadRefusal::header_malformed, 0);
    }
    const std::optional<std::size_t> status_end = line_end(head, 0);
    if (!status_end.has_value()) {
        return HeadOutcome::refused(HeadRefusal::status_line_malformed, 0);
    }
    const std::optional<std::uint16_t> read_status = status_of(head, *status_end);
    if (!read_status.has_value()) {
        return HeadOutcome::refused(HeadRefusal::status_line_malformed, 0);
    }
    const std::uint16_t status = *read_status;
    std::optional<std::uint64_t> length;
    std::optional<Field> location;
    std::size_t lines = 0;
    std::size_t begin = *status_end + 2u;
    // Each pass consumes one line and its CRLF, so the loop ends.
    while (true) {
        const std::optional<std::size_t> end = line_end(head, begin);
        if (!end.has_value()) {
            return HeadOutcome::refused(HeadRefusal::header_malformed, status);
        }
        if (*end == begin) {
            // The empty line: the head ends here, and nothing may follow.
            if (*end + 2u != head.size()) {
                return HeadOutcome::refused(HeadRefusal::header_malformed, status);
            }
            break;
        }
        lines += 1;
        if (lines > header_lines_max) {
            return HeadOutcome::refused(HeadRefusal::too_many_headers, status);
        }
        const std::optional<Field> field = field_of(head, begin, *end);
        if (!field.has_value()) {
            return HeadOutcome::refused(HeadRefusal::header_malformed, status);
        }
        if (name_is(head, begin, (*field).name_end, "transfer-encoding")) {
            return HeadOutcome::refused(HeadRefusal::transfer_encoding, status);
        }
        if (name_is(head, begin, (*field).name_end, "content-length")) {
            const std::optional<std::uint64_t> value = length_of(head, (*field).value_begin, (*field).value_end);
            if (!value.has_value()) {
                return HeadOutcome::refused(HeadRefusal::length_malformed, status);
            }
            if (length.has_value() && *length != *value) {
                return HeadOutcome::refused(HeadRefusal::length_conflict, status);
            }
            length = value;
        }
        if (name_is(head, begin, (*field).name_end, "location")) {
            if (location.has_value()) {
                return HeadOutcome::refused(HeadRefusal::location_repeated, status);
            }
            location = field;
        }
        begin = *end + 2u;
    }
    if (status == 200u) {
        if (!length.has_value()) {
            return HeadOutcome::refused(HeadRefusal::length_missing, status);
        }
        if (*length > body_max) {
            return HeadOutcome::refused(HeadRefusal::too_large, status);
        }
        return HeadOutcome::body(HeadBody{*length});
    }
    if (is_redirect(status)) {
        if (!location.has_value() || (*location).value_end == (*location).value_begin) {
            return HeadOutcome::refused(HeadRefusal::location_missing, status);
        }
        HeadRedirect redirect{};
        redirect.status = status;
        redirect.location_offset = (*location).value_begin;
        redirect.location_length = (*location).value_end - (*location).value_begin;
        return HeadOutcome::redirect(redirect);
    }
    return HeadOutcome::refused(HeadRefusal::status_not_accepted, status);
}

}  // namespace stxnet
