// http.h: the web interface's HTTP/1.1 request reader and response head
// writer (C2; design dev/notes/planning/stages/C2-web.md, section 2).
//
// Pure code: no system call, no operating-system header, no allocation.
//
// One request per connection: every response says "Connection: close" and
// the connection ends after it (RFC 9112 section 9.6 lets a server close).
// The reader is strict and bounded; what it refuses it names, with the
// status the server answers:
//
//   head at most http_head_bytes_max bytes, else 431
//   at most http_header_count_max header fields, else 431
//   request-target at most http_target_bytes_max bytes, else 414
//   body (Content-Length) at most http_body_bytes_max bytes, else 413;
//     below http_stream_path_prefix, at most http_stream_bytes_max (the
//     server streams such a body instead of holding it: uploads)
//   method GET, HEAD or POST; another valid token, 501; else 400
//   version HTTP/1.1; another HTTP/x.y, 505; else 400
//   lines end in CRLF; a bare LF or CR is refused (400); no empty line may
//     precede the request line (400)
//   request-target in origin form only: "/" path ["?" query], RFC 3986
//     pchar, "/" and "?", "%" with two hex digits (not decoded); the
//     absolute and asterisk forms are refused (400)
//   field lines name ":" OWS value OWS: the name a token with no blank
//     before the colon; the value visible ASCII, space and tab only (no
//     obs-text, no control byte); obs-fold refused (400)
//   exactly one Host, non-empty, host characters only (400)
//   at most one of Host, Cookie, Content-Type, Origin, X-STX-CSRF,
//     Content-Length (400)
//   Content-Length: 1 to 10 digits within 32 bits (400); GET and HEAD with
//     a body are refused (400)
//   Transfer-Encoding of any kind: 501 (chunked request bodies are not
//     read; every client of this interface sends Content-Length)
//   Expect of any kind: 417
//
// The response head writer emits the status line, the fields the caller
// names, and fixed security fields on every response.

#ifndef STXLIB_RUNTIME_WEB_HTTP_H
#define STXLIB_RUNTIME_WEB_HTTP_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "json.h"

namespace stxweb {

constexpr std::size_t http_head_bytes_max = 8192;
constexpr std::size_t http_header_count_max = 32;
constexpr std::size_t http_target_bytes_max = 2048;
// The largest request body: a JSON document (json.h).
constexpr std::size_t http_body_bytes_max = json_document_bytes_max;
// Requests below this path carry bodies the server streams to a file (the
// upload routes, api.h), up to http_stream_bytes_max bytes; the assembler
// stops after their head (AssemblyState::streaming).
constexpr const char* http_stream_path_prefix = "/api/upload/";
constexpr std::uint32_t http_stream_bytes_max = 128u * 1024u * 1024u;
constexpr std::size_t http_request_bytes_max = http_head_bytes_max + http_body_bytes_max;
// A response head never exceeds this (the fixed fields plus the variable
// ones the writer accepts).
constexpr std::size_t http_response_head_bytes_max = 1536;

enum class Method : std::uint8_t {
    get,
    head,
    post,
};

[[nodiscard]] const char* method_name(Method method) noexcept;

// A run of bytes in the request buffer.
struct ByteRange {
    std::uint32_t offset;
    std::uint32_t length;
};

struct RequestHead {
    Method method;
    ByteRange path;
    // After the "?", when there is one.
    std::optional<ByteRange> query;
    std::uint32_t header_count;
    ByteRange host;
    std::optional<ByteRange> cookie;
    std::optional<ByteRange> content_type;
    std::optional<ByteRange> origin;
    std::optional<ByteRange> csrf_token;
    // 0 when absent.
    std::uint32_t content_length;
    // The head's bytes, its final CRLF CRLF included.
    std::uint32_t head_length;
};

struct HeadRefused {
    // The status to answer: 400, 413, 414, 417, 431, 501 or 505.
    std::uint16_t status;
    // A short name for the console and the error body ("bare_lf", ...).
    const char* reason;
};

class HeadOutcome {
public:
    [[nodiscard]] static HeadOutcome parsed(const RequestHead& head) noexcept;
    [[nodiscard]] static HeadOutcome refused(std::uint16_t status, const char* reason) noexcept;
    [[nodiscard]] const RequestHead* as_head() const noexcept;
    [[nodiscard]] const HeadRefused* as_refused() const noexcept;

private:
    using Alternatives = std::variant<RequestHead, HeadRefused>;
    explicit HeadOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// Parses a complete head: bytes from the request line to the CRLF CRLF
// that ends it, inclusive (head.size() is the head's length, at most
// http_head_bytes_max). Line ends have been checked by the assembler but
// are checked again here.
[[nodiscard]] HeadOutcome parse_request_head(std::span<const char> head) noexcept;

// ---------------------------------------------------------------------------
// Assembling a request from the bytes a connection receives.

enum class AssemblyState : std::uint8_t {
    receiving,
    complete,
    refused,
    // The head of a request below http_stream_path_prefix is complete;
    // its body is the server's to read (streamed_prefix() holds the part
    // that arrived with the head).
    streaming,
};

// Whether a request-target's path is below http_stream_path_prefix.
[[nodiscard]] bool is_stream_path(std::span<const char> path) noexcept;

// Owns one request's bytes (http_request_bytes_max). Not copyable; a
// connection owns one and resets it per request.
class RequestAssembler {
public:
    RequestAssembler() noexcept;
    RequestAssembler(const RequestAssembler&) = delete;
    RequestAssembler& operator=(const RequestAssembler&) = delete;

    void reset() noexcept;

    // Where the next received bytes go: empty unless receiving. Before the
    // head is complete it reaches to http_head_bytes_max; after, to the
    // body's end, never beyond (bytes after the request stay unread).
    [[nodiscard]] std::span<char> free_space() noexcept;

    // count bytes were placed at the front of free_space() (count at most
    // its size, asserted). Returns the state after them.
    AssemblyState commit(std::size_t count) noexcept;

    [[nodiscard]] AssemblyState state() const noexcept;
    // Complete or streaming: the head; complete: the body. Refused: the
    // refusal.
    [[nodiscard]] const RequestHead* head() const noexcept;
    [[nodiscard]] std::span<const char> body() const noexcept;
    // Streaming: the body's bytes that arrived with the head (at most the
    // body's length).
    [[nodiscard]] std::span<const char> streamed_prefix() const noexcept;
    [[nodiscard]] const HeadRefused* refusal() const noexcept;
    // The bytes received so far (the head's ranges index into them).
    [[nodiscard]] std::span<const char> bytes() const noexcept;
    [[nodiscard]] std::size_t received() const noexcept;

private:
    void refuse(std::uint16_t status, const char* reason) noexcept;
    void scan_head() noexcept;

    char buffer_[http_request_bytes_max];
    std::size_t received_;
    // Bytes of the head already checked for line ends.
    std::size_t scanned_;
    AssemblyState state_;
    std::optional<RequestHead> head_;
    HeadRefused refusal_;
};

// The text a range names in bytes (asserted inside).
[[nodiscard]] std::span<const char> range_text(std::span<const char> bytes, const ByteRange& range) noexcept;

// ---------------------------------------------------------------------------
// Cookies and small text tests.

// The value of the cookie called name in a Cookie field's value
// ("a=1; b=2"), or nothing when it is absent or appears more than once.
// A borrow of field.
[[nodiscard]] std::optional<std::span<const char>> cookie_value(std::span<const char> field,
                                                                const char* name) noexcept;

// ASCII case-insensitive equality of bytes and a lower-case literal.
[[nodiscard]] bool equals_lower(std::span<const char> bytes, const char* lower) noexcept;
[[nodiscard]] bool equals_exact(std::span<const char> bytes, const char* text) noexcept;

// ---------------------------------------------------------------------------
// Response heads.

struct ResponseHeadFields {
    std::uint16_t status;
    // Absent: no body, Content-Length 0.
    const char* content_type;
    std::size_t content_length;
    // Static assets may be revalidated ("no-cache"); everything else is
    // "no-store".
    bool cacheable;
    // A Set-Cookie value, written as given; absent: none.
    const char* set_cookie;
    // Retry-After in seconds, for 429 and 503.
    std::optional<std::uint32_t> retry_after_s;
    // Allow, for 405.
    const char* allow;
};

[[nodiscard]] const char* status_reason(std::uint16_t status) noexcept;

// Writes the head into out. Returns its size, or nothing when it does not
// fit (out unspecified).
[[nodiscard]] std::optional<std::size_t> write_response_head(const ResponseHeadFields& fields,
                                                             std::span<char> out) noexcept;

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_HTTP_H
