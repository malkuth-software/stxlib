// fetch.cpp: see fetch.h.

#include "fetch.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "../supervisor/config/boot_config.h"
#include "http_head.h"
#include "url.h"

namespace stxnet {

const char* fetch_result_name(FetchResult result) noexcept {
    switch (result) {
    case FetchResult::done: return "done";
    case FetchResult::url_refused: return "url_refused";
    case FetchResult::resolve_failed: return "resolve_failed";
    case FetchResult::connect_failed: return "connect_failed";
    case FetchResult::tls_failed: return "tls_failed";
    case FetchResult::request_failed: return "request_failed";
    case FetchResult::head_refused: return "head_refused";
    case FetchResult::too_many_redirects: return "too_many_redirects";
    case FetchResult::body_short: return "body_short";
    case FetchResult::output_failed: return "output_failed";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

constexpr std::size_t transfer_buffer_bytes = 16384;
constexpr std::size_t request_bytes_max = 512 + url_length_max + host_length_max;
constexpr int interrupted_retries_max = 16;

void report(int diagnostics, FetchResult result, const char* detail) noexcept {
    char line[256] = {};
    std::size_t used = 0;
    const char* const parts[] = {"stxfetch ", fetch_result_name(result), " ", detail, "\n"};
    for (const char* const part : parts) {
        for (std::size_t index = 0; part[index] != '\0' && used + 1u < sizeof(line); index += 1) {
            line[used] = part[index];
            used += 1;
        }
    }
    (void)write(diagnostics, line, used);
}

// Writes all of bytes to descriptor. False on an error.
[[nodiscard]] bool write_all(int descriptor, const char* bytes, std::size_t length) noexcept {
    std::size_t written = 0;
    int interrupted = 0;
    while (written < length) {
        const ssize_t put = write(descriptor, &bytes[written], length - written);
        if (put > 0) {
            written += static_cast<std::size_t>(put);
            continue;
        }
        if (put < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
            interrupted += 1;
            continue;
        }
        return false;
    }
    return true;
}

// The decimal of value into out; the length.
std::size_t decimal_text(std::uint32_t value, char (&out)[12]) noexcept {
    char digits[12] = {};
    std::size_t count = 0;
    std::uint32_t rest = value;
    do {
        digits[count] = static_cast<char>('0' + static_cast<char>(rest % 10u));
        rest /= 10u;
        count += 1;
    } while (rest > 0u && count < 10u);
    for (std::size_t index = 0; index < count; index += 1) {
        out[index] = digits[count - 1u - index];
    }
    out[count] = '\0';
    return count;
}

// Connects to host:port, trying each address; the socket or -1.
[[nodiscard]] int connect_to(const HttpsUrl& url) noexcept {
    char port[12] = {};
    (void)decimal_text(url.port, port);
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* addresses = nullptr;
    if (getaddrinfo(url.host, port, &hints, &addresses) != 0 || addresses == nullptr) {
        return -2;
    }
    int connected = -1;
    // The resolver's list is finite; each pass tries one address.
    for (struct addrinfo* address = addresses; address != nullptr && connected < 0;
         address = (*address).ai_next) {
        const int socket_descriptor =
            socket((*address).ai_family, (*address).ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                   (*address).ai_protocol);
        if (socket_descriptor < 0) {
            continue;
        }
        bool ready = connect(socket_descriptor, (*address).ai_addr, (*address).ai_addrlen) == 0;
        if (!ready && errno == EINPROGRESS) {
            struct pollfd wait{};
            wait.fd = socket_descriptor;
            wait.events = POLLOUT;
            const int polled = poll(&wait, 1, static_cast<int>(connect_timeout_s * 1000u));
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            ready = polled == 1 &&
                    getsockopt(socket_descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) == 0 &&
                    socket_error == 0;
        }
        if (!ready) {
            (void)close(socket_descriptor);
            continue;
        }
        // Blocking again, with time limits on every read and write.
        const int flags = fcntl(socket_descriptor, F_GETFL);
        struct timeval limit{};
        limit.tv_sec = static_cast<time_t>(io_timeout_s);
        if (flags < 0 || fcntl(socket_descriptor, F_SETFL, flags & ~O_NONBLOCK) != 0 ||
            setsockopt(socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) != 0 ||
            setsockopt(socket_descriptor, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) != 0) {
            (void)close(socket_descriptor);
            continue;
        }
        connected = socket_descriptor;
    }
    freeaddrinfo(addresses);
    return connected;
}

// The OpenSSL objects of one connection; released by the destructor.
class TlsConnection {
public:
    TlsConnection() noexcept : context_(nullptr), session_(nullptr), socket_(-1) {}
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;
    ~TlsConnection() {
        if (session_ != nullptr) {
            SSL_free(session_);
        }
        if (context_ != nullptr) {
            SSL_CTX_free(context_);
        }
        if (socket_ >= 0) {
            (void)close(socket_);
        }
    }

    // Connects and completes the handshake; the result's detail in detail.
    [[nodiscard]] FetchResult open(const HttpsUrl& url, const char* ca_bundle, const char*& detail) noexcept {
        socket_ = connect_to(url);
        if (socket_ == -2) {
            socket_ = -1;
            detail = url.host;
            return FetchResult::resolve_failed;
        }
        if (socket_ < 0) {
            detail = url.host;
            return FetchResult::connect_failed;
        }
        detail = "handshake";
        context_ = SSL_CTX_new(TLS_client_method());
        if (context_ == nullptr || SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION) != 1 ||
            SSL_CTX_load_verify_locations(context_, ca_bundle, nullptr) != 1) {
            detail = "context";
            return FetchResult::tls_failed;
        }
        SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
        session_ = SSL_new(context_);
        if (session_ == nullptr || SSL_set_fd(session_, socket_) != 1) {
            return FetchResult::tls_failed;
        }
        // A dotted IPv4 host is checked against the certificate's IP
        // addresses and gets no SNI; a name against its DNS names.
        const bool is_address =
            stxconfig::ipv4_of(url.host, std::strlen(url.host)).has_value();
        if (is_address) {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(session_), url.host) != 1) {
                return FetchResult::tls_failed;
            }
        } else if (SSL_set_tlsext_host_name(session_, url.host) != 1 || SSL_set1_host(session_, url.host) != 1) {
            return FetchResult::tls_failed;
        }
        if (SSL_connect(session_) != 1) {
            detail = SSL_get_verify_result(session_) != X509_V_OK ? "certificate" : "handshake";
            return FetchResult::tls_failed;
        }
        return FetchResult::done;
    }

    [[nodiscard]] bool write_all(const char* bytes, std::size_t length) noexcept {
        std::size_t written = 0;
        while (written < length) {
            std::size_t put = 0;
            if (SSL_write_ex(session_, &bytes[written], length - written, &put) != 1 || put == 0u) {
                return false;
            }
            written += put;
        }
        return true;
    }

    // Reads at most capacity bytes; 0 at the end of the stream or on an
    // error.
    [[nodiscard]] std::size_t read_some(char* bytes, std::size_t capacity) noexcept {
        std::size_t got = 0;
        if (SSL_read_ex(session_, bytes, capacity, &got) != 1) {
            return 0;
        }
        return got;
    }

private:
    SSL_CTX* context_;
    SSL* session_;
    int socket_;
};

// Appends text to the request; false when it would not fit.
[[nodiscard]] bool append(char (&request)[request_bytes_max], std::size_t& used, const char* text) noexcept {
    for (std::size_t index = 0; text[index] != '\0'; index += 1) {
        if (used >= request_bytes_max) {
            return false;
        }
        request[used] = text[index];
        used += 1;
    }
    return true;
}

// One request and its response. done with the body copied; or a redirect
// (next holds the new URL, redirected set); or a failure.
[[nodiscard]] FetchResult fetch_once(const HttpsUrl& url, std::uint64_t body_max, int output, const char* ca_bundle,
                                     HttpsUrl& next, bool& redirected, const char*& detail) noexcept {
    redirected = false;
    TlsConnection connection;
    const FetchResult opened = connection.open(url, ca_bundle, detail);
    if (opened != FetchResult::done) {
        return opened;
    }
    char request[request_bytes_max] = {};
    std::size_t used = 0;
    char port[12] = {};
    (void)decimal_text(url.port, port);
    const bool built = append(request, used, "GET ") && append(request, used, url.path) &&
                       append(request, used, " HTTP/1.1\r\nHost: ") && append(request, used, url.host) &&
                       (url.port == https_default_port || (append(request, used, ":") && append(request, used, port))) &&
                       append(request, used, "\r\nUser-Agent: stxpkg/1\r\nAccept: */*\r\nConnection: close\r\n\r\n");
    if (!built || !connection.write_all(request, used)) {
        detail = "request";
        return FetchResult::request_failed;
    }
    // The head, then whatever of the body came with it.
    static char buffer[response_head_max + transfer_buffer_bytes];
    std::size_t filled = 0;
    std::optional<std::size_t> head_end;
    while (!head_end.has_value()) {
        if (filled >= response_head_max) {
            detail = "head too long";
            return FetchResult::head_refused;
        }
        const std::size_t got = connection.read_some(&buffer[filled], response_head_max - filled);
        if (got == 0u) {
            detail = "head cut short";
            return FetchResult::head_refused;
        }
        filled += got;
        head_end = find_head_end(std::span<const char>(buffer, filled));
    }
    const HeadOutcome head = parse_response_head(std::span<const char>(buffer, *head_end), body_max);
    const HeadRefused* const refused = head.as_refused();
    if (refused != nullptr) {
        detail = head_refusal_name((*refused).reason);
        return FetchResult::head_refused;
    }
    const HeadRedirect* const redirect = head.as_redirect();
    if (redirect != nullptr) {
        const UrlOutcome location = parse_https_url(
            std::span<const char>(&buffer[(*redirect).location_offset], (*redirect).location_length));
        if (location.as_parsed() == nullptr) {
            detail = "location not an https URL";
            return FetchResult::head_refused;
        }
        next = *location.as_parsed();
        redirected = true;
        return FetchResult::done;
    }
    const HeadBody* const body = head.as_body();
    if (body == nullptr) {
        // Fallback: an outcome in no known state.
        detail = "head";
        return FetchResult::head_refused;
    }
    std::uint64_t remaining = (*body).content_length;
    const std::size_t early = filled - *head_end;
    const std::size_t early_used = early < remaining ? early : static_cast<std::size_t>(remaining);
    if (!write_all(output, &buffer[*head_end], early_used)) {
        detail = "output";
        return FetchResult::output_failed;
    }
    remaining -= early_used;
    // Each pass copies at least one byte or ends the fetch.
    while (remaining > 0u) {
        const std::size_t want = remaining < transfer_buffer_bytes ? static_cast<std::size_t>(remaining)
                                                                   : transfer_buffer_bytes;
        const std::size_t got = connection.read_some(buffer, want);
        if (got == 0u) {
            detail = "body cut short";
            return FetchResult::body_short;
        }
        if (!write_all(output, buffer, got)) {
            detail = "output";
            return FetchResult::output_failed;
        }
        remaining -= got;
    }
    return FetchResult::done;
}

}  // namespace

FetchResult fetch_https(const char* url, std::uint64_t body_max, int output, int diagnostics,
                        const char* ca_bundle) noexcept {
    const UrlOutcome parsed = parse_https_url(std::span<const char>(url, std::strlen(url)));
    if (parsed.as_parsed() == nullptr) {
        report(diagnostics, FetchResult::url_refused, url_refusal_name(*parsed.as_refused()));
        return FetchResult::url_refused;
    }
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr) != 1) {
        report(diagnostics, FetchResult::tls_failed, "init");
        return FetchResult::tls_failed;
    }
    static HttpsUrl current;
    static HttpsUrl next;
    current = *parsed.as_parsed();
    for (std::uint32_t attempt = 0; attempt <= redirects_max; attempt += 1) {
        bool redirected = false;
        const char* detail = "";
        const FetchResult result = fetch_once(current, body_max, output, ca_bundle, next, redirected, detail);
        if (result != FetchResult::done) {
            report(diagnostics, result, detail);
            return result;
        }
        if (!redirected) {
            return FetchResult::done;
        }
        current = next;
    }
    report(diagnostics, FetchResult::too_many_redirects, current.host);
    return FetchResult::too_many_redirects;
}

}  // namespace stxnet
