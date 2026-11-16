// fetch.h: one HTTPS GET, the body written to a descriptor (C2: the
// downloader, appliance/C2_DESIGN.md, "Updates").
//
// FreeBSD only: sockets, the C library's resolver, and the base system's
// OpenSSL (libssl, libcrypto, linked statically). The parsing it relies on
// is pure and tested on the host (url.h, http_head.h). stxpkg calls it in a
// child that has dropped to fetch_identity and holds nothing else: the
// child can read the network and write one pipe, and nothing it receives
// is trusted until the parent has checked the package's signature and
// hashes.
//
// What it does: resolve the host (getaddrinfo), connect to each address in
// turn (connect_timeout_s each), TLS 1.2 or later with the peer's chain
// checked against ca_bundle_path and its name (or IPv4 address) against
// the URL's host, one "GET <path> HTTP/1.1" with "Connection: close",
// read the head (http_head.h), follow at most redirects_max redirects to
// https URLs, then copy exactly Content-Length bytes to the output. Reads
// and writes on the socket time out after io_timeout_s; the caller bounds
// the whole fetch (stxpkg: alarm, fetch_seconds_max).

#ifndef STXLIB_RUNTIME_NET_FETCH_H
#define STXLIB_RUNTIME_NET_FETCH_H

#include <cstdint>

namespace stxnet {

constexpr std::uint32_t connect_timeout_s = 10;
constexpr std::uint32_t io_timeout_s = 30;
constexpr std::uint32_t redirects_max = 3;
// The uid and gid a fetch runs as: no passwd entry, owns no file.
constexpr std::uint32_t fetch_identity = 29999;

enum class FetchResult : std::uint8_t {
    done,
    url_refused,
    resolve_failed,
    connect_failed,
    tls_failed,
    request_failed,
    head_refused,
    too_many_redirects,
    body_short,
    output_failed,
};

[[nodiscard]] const char* fetch_result_name(FetchResult result) noexcept;

// Fetches url (NUL-terminated) and writes its body, at most body_max
// bytes, to output. On anything but done, one line "stxfetch <result>
// <detail>" goes to diagnostics and the output may hold part of the body.
[[nodiscard]] FetchResult fetch_https(const char* url, std::uint64_t body_max, int output, int diagnostics,
                                      const char* ca_bundle) noexcept;

}  // namespace stxnet

#endif  // STXLIB_RUNTIME_NET_FETCH_H
