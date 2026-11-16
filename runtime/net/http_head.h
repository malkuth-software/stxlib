// http_head.h: reading an HTTP/1.1 response head (C2: the update service's
// downloads, appliance/C2_DESIGN.md, "Updates").
//
// Pure code: no system call, no operating-system header, no allocation.
// Tested on the host (test/test_http_head.cpp), exhaustively over bounded
// domains against references written another way.
//
// The downloader sends one GET per connection with "Connection: close" and
// reads the head, then exactly Content-Length bytes. So only what that
// needs is accepted (RFC 9112 sections 4 and 5, RFC 9110 section 5), and
// everything a smuggling or framing confusion would need is refused:
//
// - The status line "HTTP/1.0 " or "HTTP/1.1 ", three digits, then nothing
//   or a space and a reason of visible bytes, spaces and tabs (obs-text
//   0x80-0xFF included), then CRLF. Lines end in CRLF only; a bare LF or CR
//   is refused.
// - Header lines: a token, ':', optional blanks, a value of visible bytes,
//   spaces and tabs, optional blanks, CRLF. A line starting with a blank
//   (obs-fold) is refused; so is a name with a blank before ':'. At most
//   header_lines_max lines.
// - Any Transfer-Encoding is refused: the body is framed by Content-Length
//   only.
// - 200: exactly one Content-Length value, 1 to 19 digits, at most the
//   caller's limit; a repeated Content-Length line must repeat the value
//   exactly, and a list ("5, 5") is refused.
// - 301, 302, 303, 307, 308: exactly one Location line; its value is
//   returned for the caller to parse as an https URL (url.h).
// - Anything else is refused with its status.

#ifndef STXLIB_RUNTIME_NET_HTTP_HEAD_H
#define STXLIB_RUNTIME_NET_HTTP_HEAD_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace stxnet {

constexpr std::size_t response_head_max = 8192;
constexpr std::size_t header_lines_max = 100;

// The index just after the first CRLF CRLF in bytes, or nothing when the
// first min(size, response_head_max) bytes hold none.
[[nodiscard]] std::optional<std::size_t> find_head_end(std::span<const char> bytes) noexcept;

struct HeadBody {
    std::uint64_t content_length;
};

struct HeadRedirect {
    std::uint16_t status;
    // The Location value inside the head passed in: offset and length,
    // without surrounding blanks.
    std::size_t location_offset;
    std::size_t location_length;
};

enum class HeadRefusal : std::uint8_t {
    status_line_malformed,
    header_malformed,
    too_many_headers,
    transfer_encoding,
    length_missing,
    length_malformed,
    length_conflict,
    too_large,
    location_missing,
    location_repeated,
    status_not_accepted,
};

struct HeadRefused {
    HeadRefusal reason;
    // The status code when the status line was read, else 0.
    std::uint16_t status;
};

class HeadOutcome {
public:
    [[nodiscard]] static HeadOutcome body(const HeadBody& value) noexcept;
    [[nodiscard]] static HeadOutcome redirect(const HeadRedirect& value) noexcept;
    [[nodiscard]] static HeadOutcome refused(HeadRefusal reason, std::uint16_t status) noexcept;

    [[nodiscard]] const HeadBody* as_body() const noexcept;
    [[nodiscard]] const HeadRedirect* as_redirect() const noexcept;
    [[nodiscard]] const HeadRefused* as_refused() const noexcept;

private:
    using Alternatives = std::variant<HeadRefused, HeadBody, HeadRedirect>;
    explicit HeadOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

[[nodiscard]] const char* head_refusal_name(HeadRefusal reason) noexcept;

// head is the response's bytes up to and including the CRLF CRLF that ends
// the head (find_head_end), at most response_head_max bytes; body_max is
// the largest Content-Length the caller will read.
[[nodiscard]] HeadOutcome parse_response_head(std::span<const char> head, std::uint64_t body_max) noexcept;

}  // namespace stxnet

#endif  // STXLIB_RUNTIME_NET_HTTP_HEAD_H
