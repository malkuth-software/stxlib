// url.h: the https URLs the appliance fetches (C2: the update service's
// latest documents and packages, appliance/C2_DESIGN.md, "Updates").
//
// Pure code: no system call, no operating-system header, no allocation.
// Tested on the host (test/test_url.cpp), exhaustively over a bounded
// domain against a reference written another way.
//
// Accepted, and nothing else (a subset of RFC 3986's https URLs):
//
//   https://<host>[:<port>][<path>]
//
// - The scheme exactly "https://" (lower case).
// - host: a DNS name of 1..host_length_max bytes, labels of 1..63 bytes of
//   [A-Za-z0-9-] that neither begin nor end with '-', separated by single
//   dots, no trailing dot. A dotted IPv4 address is such a name and is
//   accepted as one. No user information ('@'), no IPv6 literal ('[').
// - port: 1..65535 in decimal without a leading zero; 443 when absent.
// - path: '/' followed by bytes that RFC 3986 allows in a path and query
//   (unreserved, sub-delims, ':', '@', '/', '?'), and '%' followed by two
//   hexadecimal digits; "/" when absent. No fragment ('#').
// - At most url_length_max bytes in all.

#ifndef STXLIB_RUNTIME_NET_URL_H
#define STXLIB_RUNTIME_NET_URL_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace stxnet {

constexpr std::size_t url_length_max = 255;
constexpr std::size_t host_length_max = 253;
constexpr std::size_t label_length_max = 63;
constexpr std::uint16_t https_default_port = 443;

struct HttpsUrl {
    // NUL-terminated copies.
    char host[host_length_max + 1u];
    // The request target: the path and query, "/" when the URL has none.
    char path[url_length_max + 1u];
    std::uint16_t port;
};

enum class UrlRefusal : std::uint8_t {
    too_long,
    scheme_not_https,
    host_malformed,
    user_information,
    port_malformed,
    path_malformed,
    fragment,
};

class UrlOutcome {
public:
    [[nodiscard]] static UrlOutcome parsed(const HttpsUrl& url) noexcept;
    [[nodiscard]] static UrlOutcome refused(UrlRefusal reason) noexcept;

    [[nodiscard]] const HttpsUrl* as_parsed() const noexcept;
    [[nodiscard]] const UrlRefusal* as_refused() const noexcept;

private:
    using Alternatives = std::variant<UrlRefusal, HttpsUrl>;
    explicit UrlOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

[[nodiscard]] const char* url_refusal_name(UrlRefusal reason) noexcept;

// text is the URL's bytes (no NUL needed, none allowed inside).
[[nodiscard]] UrlOutcome parse_https_url(std::span<const char> text) noexcept;

// Whether byte may appear in a path or query unescaped (RFC 3986 pchar,
// '/' and '?', without '%').
[[nodiscard]] bool is_path_byte(char byte) noexcept;

}  // namespace stxnet

#endif  // STXLIB_RUNTIME_NET_URL_H
