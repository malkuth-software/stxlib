// test_url.cpp: host tests of the https URL parser.
//
// Examples with their refusal reasons, then an exhaustive run: "https://"
// followed by every string of 0 to 6 bytes over 12 symbols chosen to reach
// every rule (3,257,437 strings), each judged by a reference that splits
// the URL with different code (find the delimiters first, then check the
// pieces); the parser must accept exactly what the reference accepts and
// return the same host, port and path.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../url.h"
#include "../../supervisor/config/test/test_support.h"

namespace {

using stxnet::HttpsUrl;
using stxnet::UrlOutcome;
using stxnet::UrlRefusal;
using stxtest::check;
using stxtest::same_text;
using stxtest::text_of;

[[nodiscard]] bool refused_with(const char* text, UrlRefusal reason) {
    const UrlOutcome outcome = stxnet::parse_https_url(text_of(text));
    return outcome.as_refused() != nullptr && *outcome.as_refused() == reason;
}

void test_examples() {
    const char* const test = "url examples";
    const UrlOutcome plain = stxnet::parse_https_url(text_of("https://updates.example.com/radio/latest"));
    const HttpsUrl* const parsed = plain.as_parsed();
    check(parsed != nullptr && same_text((*parsed).host, "updates.example.com") && (*parsed).port == 443u &&
              same_text((*parsed).path, "/radio/latest"),
          test, "host, default port, path");
    const UrlOutcome ported = stxnet::parse_https_url(text_of("https://10.0.2.2:8443"));
    check(ported.as_parsed() != nullptr && (*ported.as_parsed()).port == 8443u &&
              same_text((*ported.as_parsed()).path, "/"),
          test, "IPv4 host, port, empty path is /");
    const UrlOutcome query = stxnet::parse_https_url(text_of("https://a.b?x=1"));
    check(query.as_parsed() != nullptr && same_text((*query.as_parsed()).path, "/?x=1"), test,
          "a query straight after the host gets /");
    const UrlOutcome escaped = stxnet::parse_https_url(text_of("https://a/%2Fb%7e"));
    check(escaped.as_parsed() != nullptr && same_text((*escaped.as_parsed()).path, "/%2Fb%7e"), test,
          "escapes kept as written");
    check(refused_with("http://a/", UrlRefusal::scheme_not_https), test, "http");
    check(refused_with("HTTPS://a/", UrlRefusal::scheme_not_https), test, "upper-case scheme");
    check(refused_with("https:/a", UrlRefusal::scheme_not_https), test, "one slash");
    check(refused_with("https://user@a/", UrlRefusal::user_information), test, "user information");
    check(refused_with("https://[::1]/", UrlRefusal::host_malformed), test, "IPv6 literal");
    check(refused_with("https:///x", UrlRefusal::host_malformed), test, "empty host");
    check(refused_with("https://a..b/", UrlRefusal::host_malformed), test, "empty label");
    check(refused_with("https://a./", UrlRefusal::host_malformed), test, "trailing dot");
    check(refused_with("https://-a/", UrlRefusal::host_malformed), test, "label starting with -");
    check(refused_with("https://a_b/", UrlRefusal::host_malformed), test, "underscore");
    check(refused_with("https://a:/", UrlRefusal::port_malformed), test, "empty port");
    check(refused_with("https://a:0/", UrlRefusal::port_malformed), test, "port 0");
    check(refused_with("https://a:065/", UrlRefusal::port_malformed), test, "leading zero");
    check(refused_with("https://a:65536/", UrlRefusal::port_malformed), test, "port above 65535");
    check(refused_with("https://a/b c", UrlRefusal::path_malformed), test, "space in path");
    check(refused_with("https://a/%4", UrlRefusal::path_malformed), test, "short escape");
    check(refused_with("https://a/%zz", UrlRefusal::path_malformed), test, "bad escape");
    check(refused_with("https://a/b#c", UrlRefusal::fragment), test, "fragment");
    char label[80] = {};
    std::memset(label, 'a', 64);
    char long_label[100] = {};
    std::snprintf(long_label, sizeof(long_label), "https://%s/", label);
    check(refused_with(long_label, UrlRefusal::host_malformed), test, "64-byte label");
    label[63] = '\0';
    std::snprintf(long_label, sizeof(long_label), "https://%s/", label);
    check(stxnet::parse_https_url(text_of(long_label)).as_parsed() != nullptr, test, "63-byte label");
    char too_long[300] = {};
    std::memcpy(too_long, "https://a/", 10);
    std::memset(&too_long[10], 'x', 246);
    check(refused_with(too_long, UrlRefusal::too_long), test, "256 bytes");
    too_long[255] = '\0';
    check(stxnet::parse_https_url(text_of(too_long)).as_parsed() != nullptr, test, "255 bytes");
}

// The reference, written from the rules in url.h without sharing code.
struct Reference {
    bool accepted;
    char host[300];
    std::uint32_t port;
    char path[300];
};

[[nodiscard]] bool ref_alnum(char byte) {
    return std::strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", byte) != nullptr &&
           byte != '\0';
}

[[nodiscard]] bool ref_hex(char byte) {
    return byte != '\0' && std::strchr("0123456789abcdefABCDEF", byte) != nullptr;
}

[[nodiscard]] Reference reference(const char* rest, std::size_t length) {
    Reference result{};
    result.accepted = false;
    // Delimiters first.
    std::size_t authority_end = length;
    for (std::size_t index = 0; index < length; index += 1) {
        if (rest[index] == '/' || rest[index] == '?' || rest[index] == '#') {
            authority_end = index;
            break;
        }
    }
    for (std::size_t index = 0; index < authority_end; index += 1) {
        if (rest[index] == '@') {
            return result;
        }
    }
    std::size_t colon = authority_end;
    for (std::size_t index = 0; index < authority_end; index += 1) {
        if (rest[index] == ':') {
            colon = index;
            break;
        }
    }
    // Host: labels.
    if (colon == 0u || colon > 253u) {
        return result;
    }
    std::size_t label_length = 0;
    for (std::size_t index = 0; index <= colon; index += 1) {
        if (index == colon || rest[index] == '.') {
            if (label_length == 0u || label_length > 63u || rest[index - label_length] == '-' ||
                rest[index - 1u] == '-') {
                return result;
            }
            label_length = 0;
            continue;
        }
        if (!ref_alnum(rest[index]) && rest[index] != '-') {
            return result;
        }
        label_length += 1;
    }
    // Port.
    result.port = 443;
    if (colon < authority_end) {
        const std::size_t digits = authority_end - colon - 1u;
        if (digits == 0u || digits > 5u || rest[colon + 1u] == '0') {
            return result;
        }
        std::uint32_t port = 0;
        for (std::size_t index = colon + 1u; index < authority_end; index += 1) {
            if (rest[index] < '0' || rest[index] > '9') {
                return result;
            }
            port = port * 10u + static_cast<std::uint32_t>(rest[index] - '0');
        }
        if (port < 1u || port > 65535u) {
            return result;
        }
        result.port = port;
    }
    // Path.
    const char* const allowed = "-._~!$&'()*+,;=:@/?";
    for (std::size_t index = authority_end; index < length; index += 1) {
        const char byte = rest[index];
        if (byte == '%') {
            if (index + 2u >= length || !ref_hex(rest[index + 1u]) || !ref_hex(rest[index + 2u])) {
                return result;
            }
            continue;
        }
        if (!ref_alnum(byte) && std::strchr(allowed, byte) == nullptr) {
            return result;
        }
    }
    std::memcpy(result.host, rest, colon);
    result.host[colon] = '\0';
    if (authority_end == length) {
        std::snprintf(result.path, sizeof(result.path), "/");
    } else if (rest[authority_end] == '?') {
        std::snprintf(result.path, sizeof(result.path), "/%.*s", static_cast<int>(length - authority_end),
                      &rest[authority_end]);
    } else {
        std::snprintf(result.path, sizeof(result.path), "%.*s", static_cast<int>(length - authority_end),
                      &rest[authority_end]);
    }
    result.accepted = true;
    return result;
}

void test_exhaustive() {
    const char alphabet[] = {'a', '-', '.', ':', '1', '0', '/', '?', '@', '%', '#', ' '};
    constexpr std::size_t symbols = sizeof(alphabet);
    char url[32] = "https://";
    constexpr std::size_t scheme_length = 8;
    std::size_t values = 0;
    std::size_t accepted = 0;
    for (std::size_t length = 0; length <= 6u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= symbols;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                url[scheme_length + position] = alphabet[rest % symbols];
                rest /= symbols;
            }
            url[scheme_length + length] = '\0';
            const Reference expected = reference(&url[scheme_length], length);
            const UrlOutcome outcome = stxnet::parse_https_url(text_of(url));
            const HttpsUrl* const parsed = outcome.as_parsed();
            bool same = (parsed != nullptr) == expected.accepted;
            if (same && parsed != nullptr) {
                same = same_text((*parsed).host, expected.host) && (*parsed).port == expected.port &&
                       same_text((*parsed).path, expected.path);
            }
            if (!same) {
                check(false, "exhaustive urls", "as the reference");
                std::printf("  url \"%s\"\n", url);
            }
            accepted += expected.accepted ? 1u : 0u;
            values += 1;
        }
    }
    std::printf("exhaustive urls: %zu values, %zu accepted\n", values, accepted);
    check(values == 3257437u, "exhaustive urls", "the stated domain was covered");
}

}  // namespace

int main() {
    test_examples();
    test_exhaustive();
    return stxtest::finish("test_url");
}
