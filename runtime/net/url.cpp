// url.cpp: see url.h.

#include "url.h"

namespace stxnet {

UrlOutcome::UrlOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

UrlOutcome UrlOutcome::parsed(const HttpsUrl& url) noexcept {
    return UrlOutcome(Alternatives(url));
}

UrlOutcome UrlOutcome::refused(UrlRefusal reason) noexcept {
    return UrlOutcome(Alternatives(reason));
}

const HttpsUrl* UrlOutcome::as_parsed() const noexcept {
    return std::get_if<HttpsUrl>(&alternatives_);
}

const UrlRefusal* UrlOutcome::as_refused() const noexcept {
    return std::get_if<UrlRefusal>(&alternatives_);
}

const char* url_refusal_name(UrlRefusal reason) noexcept {
    switch (reason) {
    case UrlRefusal::too_long: return "too_long";
    case UrlRefusal::scheme_not_https: return "scheme_not_https";
    case UrlRefusal::host_malformed: return "host_malformed";
    case UrlRefusal::user_information: return "user_information";
    case UrlRefusal::port_malformed: return "port_malformed";
    case UrlRefusal::path_malformed: return "path_malformed";
    case UrlRefusal::fragment: return "fragment";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

[[nodiscard]] bool is_alphanumeric(char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_hex_digit(char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

// The host [begin, end): labels as url.h states.
[[nodiscard]] bool host_valid(std::span<const char> text, std::size_t begin, std::size_t end) noexcept {
    if (end == begin || end - begin > host_length_max) {
        return false;
    }
    std::size_t label_begin = begin;
    for (std::size_t index = begin; index <= end; index += 1) {
        if (index < end && text[index] != '.') {
            if (!is_alphanumeric(text[index]) && text[index] != '-') {
                return false;
            }
            continue;
        }
        const std::size_t length = index - label_begin;
        if (length == 0u || length > label_length_max || text[label_begin] == '-' || text[index - 1u] == '-') {
            return false;
        }
        label_begin = index + 1u;
    }
    return true;
}

}  // namespace

bool is_path_byte(char byte) noexcept {
    if (is_alphanumeric(byte)) {
        return true;
    }
    switch (byte) {
    // unreserved
    case '-': case '.': case '_': case '~':
    // sub-delims
    case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';': case '=':
    // pchar, path and query
    case ':': case '@': case '/': case '?':
        return true;
    default:
        break;
    }
    return false;
}

UrlOutcome parse_https_url(std::span<const char> text) noexcept {
    if (text.size() > url_length_max) {
        return UrlOutcome::refused(UrlRefusal::too_long);
    }
    const char scheme[] = "https://";
    const std::size_t scheme_length = sizeof(scheme) - 1u;
    if (text.size() < scheme_length) {
        return UrlOutcome::refused(UrlRefusal::scheme_not_https);
    }
    for (std::size_t index = 0; index < scheme_length; index += 1) {
        if (text[index] != scheme[index]) {
            return UrlOutcome::refused(UrlRefusal::scheme_not_https);
        }
    }
    // The authority runs to the first '/', '?' or '#', or the end.
    std::size_t authority_end = scheme_length;
    while (authority_end < text.size() && text[authority_end] != '/' && text[authority_end] != '?' &&
           text[authority_end] != '#') {
        if (text[authority_end] == '@') {
            return UrlOutcome::refused(UrlRefusal::user_information);
        }
        authority_end += 1;
    }
    std::size_t host_end = scheme_length;
    while (host_end < authority_end && text[host_end] != ':') {
        host_end += 1;
    }
    if (!host_valid(text, scheme_length, host_end)) {
        return UrlOutcome::refused(UrlRefusal::host_malformed);
    }
    HttpsUrl url{};
    url.port = https_default_port;
    if (host_end < authority_end) {
        const std::size_t digits_begin = host_end + 1u;
        const std::size_t digits = authority_end - digits_begin;
        if (digits == 0u || digits > 5u || text[digits_begin] == '0') {
            return UrlOutcome::refused(UrlRefusal::port_malformed);
        }
        std::uint32_t port = 0;
        for (std::size_t index = digits_begin; index < authority_end; index += 1) {
            if (text[index] < '0' || text[index] > '9') {
                return UrlOutcome::refused(UrlRefusal::port_malformed);
            }
            port = port * 10u + static_cast<std::uint32_t>(text[index] - '0');
        }
        if (port > 65535u) {
            return UrlOutcome::refused(UrlRefusal::port_malformed);
        }
        url.port = static_cast<std::uint16_t>(port);
    }
    // The path: absent, or '/' then path bytes and escapes. A '?' straight
    // after the authority starts a query with an empty path.
    std::size_t path_length = 0;
    if (authority_end < text.size() && text[authority_end] == '?') {
        url.path[0] = '/';
        path_length = 1;
    }
    for (std::size_t index = authority_end; index < text.size(); index += 1) {
        const char byte = text[index];
        if (byte == '#') {
            return UrlOutcome::refused(UrlRefusal::fragment);
        }
        if (byte == '%') {
            if (index + 2u >= text.size() || !is_hex_digit(text[index + 1u]) || !is_hex_digit(text[index + 2u])) {
                return UrlOutcome::refused(UrlRefusal::path_malformed);
            }
        } else if (!is_path_byte(byte)) {
            return UrlOutcome::refused(UrlRefusal::path_malformed);
        }
        // At most url_length_max bytes, plus the '/' added before a query.
        if (path_length >= url_length_max) {
            return UrlOutcome::refused(UrlRefusal::too_long);
        }
        url.path[path_length] = byte;
        path_length += 1;
    }
    if (path_length == 0u) {
        url.path[0] = '/';
        path_length = 1;
    }
    url.path[path_length] = '\0';
    const std::size_t host_length = host_end - scheme_length;
    for (std::size_t index = 0; index < host_length; index += 1) {
        url.host[index] = text[scheme_length + index];
    }
    url.host[host_length] = '\0';
    return UrlOutcome::parsed(url);
}

}  // namespace stxnet
