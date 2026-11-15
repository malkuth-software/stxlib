// tls_server.cpp: the web service's listener, TLS connections and loop
// (tls_server.h).

#include "tls_server.h"

#include <cerrno>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include "../common/console_line.h"

namespace stxweb {

namespace {

constexpr int interrupted_retries_max = 16;
// The loop wakes at least this often (deadlines are checked each pass).
constexpr int poll_period_ms = 1000;
// Transitions one step may make (handshake, reading, streaming, writing).
constexpr int step_transitions_max = 4;
constexpr std::size_t staging_path_bytes_max = 128;

using stxruntime::LineText;

[[nodiscard]] ClientKey client_key_of(const struct sockaddr_storage& address) noexcept {
    if (address.ss_family == AF_INET6) {
        const struct sockaddr_in6* const inet6 = reinterpret_cast<const struct sockaddr_in6*>(&address);
        const std::uint8_t* const bytes = reinterpret_cast<const std::uint8_t*>(&(*inet6).sin6_addr);
        // An IPv4 client on the dual-stack listener: ::ffff:a.b.c.d.
        bool mapped = bytes[10] == 0xffu && bytes[11] == 0xffu;
        for (std::size_t index = 0; index < 10u; index += 1) {
            mapped = mapped && bytes[index] == 0u;
        }
        if (mapped) {
            const std::uint8_t four[4] = {bytes[12], bytes[13], bytes[14], bytes[15]};
            return client_key_ipv4(std::span<const std::uint8_t, 4>(four));
        }
        std::uint8_t sixteen[16];
        for (std::size_t index = 0; index < 16u; index += 1) {
            sixteen[index] = bytes[index];
        }
        return client_key_ipv6(std::span<const std::uint8_t, 16>(sixteen));
    }
    const struct sockaddr_in* const inet = reinterpret_cast<const struct sockaddr_in*>(&address);
    const std::uint8_t* const bytes = reinterpret_cast<const std::uint8_t*>(&(*inet).sin_addr);
    const std::uint8_t four[4] = {bytes[0], bytes[1], bytes[2], bytes[3]};
    return client_key_ipv4(std::span<const std::uint8_t, 4>(four));
}

[[nodiscard]] const char* event_kind_name(ApiEventKind kind) noexcept {
    switch (kind) {
    case ApiEventKind::none:
        return "none";
    case ApiEventKind::login_ok:
        return "login_ok";
    case ApiEventKind::login_failed:
        return "login_failed";
    case ApiEventKind::login_locked:
        return "login_locked";
    case ApiEventKind::setup_done:
        return "setup_done";
    case ApiEventKind::setup_failed:
        return "setup_failed";
    case ApiEventKind::password_changed:
        return "password_changed";
    case ApiEventKind::logout:
        return "logout";
    case ApiEventKind::operated:
        return "operated";
    case ApiEventKind::refused:
        return "refused";
    default:
        return "unknown";
    }
}

void write_console(int console, const char* first, const char* second) noexcept {
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXWEB ");
    stxruntime::append_text(line, first);
    stxruntime::append_text(line, second);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

// Opens the listener on port: both families when the kernel has INET6.
[[nodiscard]] int open_listener(std::uint16_t port) noexcept {
    const int on = 1;
    const int off = 0;
    const int inet6 = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (inet6 >= 0) {
        struct sockaddr_in6 address{};
        address.sin6_len = sizeof(address);
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        address.sin6_addr = in6addr_any;
        if (setsockopt(inet6, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0 &&
            setsockopt(inet6, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0 &&
            bind(inet6, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(inet6, tls_listen_backlog) == 0) {
            return inet6;
        }
        (void)close(inet6);
    }
    const int inet = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (inet < 0) {
        return -1;
    }
    struct sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (setsockopt(inet, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0 &&
        bind(inet, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) == 0 &&
        listen(inet, tls_listen_backlog) == 0) {
        return inet;
    }
    (void)close(inet);
    return -1;
}

// Makes each directory above path (one per '/' after the first byte);
// existing ones are fine. False when one cannot be made.
[[nodiscard]] bool make_parent_directories(const char* path) noexcept {
    char prefix[staging_path_bytes_max];
    for (std::size_t index = 1; path[index] != '\0' && index < sizeof(prefix); index += 1) {
        if (path[index] != '/') {
            continue;
        }
        for (std::size_t copy = 0; copy < index; copy += 1) {
            prefix[copy] = path[copy];
        }
        prefix[index] = '\0';
        if (mkdir(prefix, 0700) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool write_all(int descriptor, std::span<const char> bytes) noexcept {
    std::size_t done = 0;
    int interrupted = 0;
    // Each pass writes at least one byte or counts an interruption.
    while (done < bytes.size()) {
        const ssize_t wrote = write(descriptor, bytes.data() + done, bytes.size() - done);
        if (wrote < 0) {
            if (errno == EINTR && interrupted < interrupted_retries_max) {
                interrupted += 1;
                continue;
            }
            return false;
        }
        if (wrote == 0) {
            errno = EIO;
            return false;
        }
        done += static_cast<std::size_t>(wrote);
    }
    return true;
}

[[nodiscard]] std::uint64_t remaining_ms(std::uint64_t deadline_ms, std::uint64_t now_ms) noexcept {
    return deadline_ms > now_ms ? deadline_ms - now_ms : 0u;
}

}  // namespace

std::uint64_t monotonic_now_ms() noexcept {
    struct timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u + static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

const char* server_exit_name(ServerExit exit) noexcept {
    switch (exit) {
    case ServerExit::channel_closed:
        return "channel_closed";
    case ServerExit::terminated:
        return "terminated";
    case ServerExit::failed:
        return "failed";
    default:
        return "unknown";
    }
}

TlsServer::TlsServer(Api& api, ControlClient& client, int console) noexcept
    : api_(api),
      client_(client),
      console_(console),
      context_(nullptr),
      listener_(-1),
      connections_{},
      upload_{false, 0, UploadKind::package, -1, 0, 0, nullptr, 0, 0},
      upload_buffer_{},
      error_body_{} {
    for (Connection& connection : connections_) {
        connection.phase = ConnectionPhase::idle;
        connection.descriptor = -1;
        connection.ssl = nullptr;
    }
}

TlsServer::~TlsServer() {
    for (Connection& connection : connections_) {
        close_connection(connection);
    }
    if (listener_ >= 0) {
        (void)close(listener_);
    }
    SSL_CTX_free(context_);
}

bool TlsServer::start(const TlsIdentity& identity, std::uint16_t port) noexcept {
    SSL_CTX* const context = SSL_CTX_new(TLS_server_method());
    if (context == nullptr) {
        write_console(console_, "tls context failed", "");
        return false;
    }
    context_ = context;
    // TLS 1.3 only: every browser that this interface serves has it, and
    // it leaves no older cipher suite or renegotiation to configure.
    const bool versions = SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) == 1 &&
                          SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) == 1;
    (void)SSL_CTX_set_options(context, SSL_OP_NO_TICKET | SSL_OP_NO_RENEGOTIATION);
    // No resumption: nothing per client outlives its connection.
    (void)SSL_CTX_set_num_tickets(context, 0);
    (void)SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
    (void)SSL_CTX_set_mode(context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                                        SSL_MODE_RELEASE_BUFFERS);
    if (!versions || SSL_CTX_use_certificate(context, identity.certificate) != 1 ||
        SSL_CTX_use_PrivateKey(context, identity.key) != 1 || SSL_CTX_check_private_key(context) != 1) {
        ERR_clear_error();
        write_console(console_, "tls context refused the identity", "");
        return false;
    }
    listener_ = open_listener(port);
    if (listener_ < 0) {
        write_console(console_, "listen failed", "");
        return false;
    }
    return true;
}

ServerExit TlsServer::run(const volatile std::sig_atomic_t* terminate) noexcept {
    struct pollfd watches[2u + tls_connections_max];
    // The service's loop: it runs for the service's life and ends only on
    // the conditions at its top or a failed poll.
    for (;;) {
        if (*terminate != 0) {
            return ServerExit::terminated;
        }
        if (client_.closed()) {
            return ServerExit::channel_closed;
        }
        std::uint64_t now_ms = monotonic_now_ms();
        for (std::size_t index = 0; index < tls_connections_max; index += 1) {
            Connection& connection = connections_[index];
            if (connection.phase == ConnectionPhase::idle) {
                continue;
            }
            const bool streaming_idle = connection.phase == ConnectionPhase::streaming && upload_.active &&
                                        upload_.connection == index &&
                                        now_ms >= upload_.last_progress_ms + tls_upload_idle_ms;
            if (now_ms >= connection.deadline_ms || streaming_idle) {
                if (connection.phase == ConnectionPhase::streaming) {
                    finish_upload(connection, false, 0, now_ms);
                } else {
                    close_connection(connection);
                }
            }
        }
        watches[0].fd = listener_;
        watches[0].events = POLLIN;
        watches[0].revents = 0;
        watches[1].fd = client_.descriptor();
        watches[1].events = POLLIN;
        watches[1].revents = 0;
        for (std::size_t index = 0; index < tls_connections_max; index += 1) {
            const Connection& connection = connections_[index];
            struct pollfd& watch = watches[2u + index];
            watch.fd = connection.phase == ConnectionPhase::idle ? -1 : connection.descriptor;
            watch.events = static_cast<short>(connection.want_write ? POLLOUT : POLLIN);
            watch.revents = 0;
        }
        const int ready = poll(watches, static_cast<nfds_t>(2u + tls_connections_max), poll_timeout_ms(now_ms));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ServerExit::failed;
        }
        now_ms = monotonic_now_ms();
        if ((watches[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            client_.check_closed();
        }
        if ((watches[0].revents & POLLIN) != 0) {
            accept_connections(now_ms);
        }
        for (std::size_t index = 0; index < tls_connections_max; index += 1) {
            const Connection& connection = connections_[index];
            if (connection.phase == ConnectionPhase::idle) {
                continue;
            }
            if (watches[2u + index].revents != 0 || has_buffered_input(connection)) {
                step(index, now_ms);
            }
        }
    }
}

int TlsServer::poll_timeout_ms(std::uint64_t now_ms) const noexcept {
    std::uint64_t timeout = static_cast<std::uint64_t>(poll_period_ms);
    for (std::size_t index = 0; index < tls_connections_max; index += 1) {
        const Connection& connection = connections_[index];
        if (connection.phase == ConnectionPhase::idle) {
            continue;
        }
        if (has_buffered_input(connection)) {
            return 0;
        }
        const std::uint64_t left = remaining_ms(connection.deadline_ms, now_ms);
        timeout = left < timeout ? left : timeout;
        if (connection.phase == ConnectionPhase::streaming && upload_.active && upload_.connection == index) {
            const std::uint64_t idle_left = remaining_ms(upload_.last_progress_ms + tls_upload_idle_ms, now_ms);
            timeout = idle_left < timeout ? idle_left : timeout;
        }
    }
    return static_cast<int>(timeout);
}

bool TlsServer::has_buffered_input(const Connection& connection) const noexcept {
    const bool reads = connection.phase == ConnectionPhase::handshake || connection.phase == ConnectionPhase::reading ||
                       connection.phase == ConnectionPhase::streaming;
    return reads && connection.ssl != nullptr && SSL_has_pending(connection.ssl) == 1;
}

void TlsServer::accept_connections(std::uint64_t now_ms) noexcept {
    // At most one more than the slots per wake: the rest wait in the
    // backlog for the next pass.
    for (std::size_t attempt = 0; attempt <= tls_connections_max; attempt += 1) {
        struct sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int descriptor =
            accept4(listener_, reinterpret_cast<struct sockaddr*>(&address), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (descriptor < 0) {
            return;
        }
        Connection* free_slot = nullptr;
        for (Connection& connection : connections_) {
            if (connection.phase == ConnectionPhase::idle) {
                free_slot = &connection;
                break;
            }
        }
        if (free_slot == nullptr) {
            (void)close(descriptor);
            continue;
        }
        SSL* const ssl = SSL_new(context_);
        if (ssl == nullptr || SSL_set_fd(ssl, descriptor) != 1) {
            SSL_free(ssl);
            ERR_clear_error();
            (void)close(descriptor);
            continue;
        }
        Connection& connection = *free_slot;
        connection.phase = ConnectionPhase::handshake;
        connection.descriptor = descriptor;
        connection.ssl = ssl;
        connection.client = client_key_of(address);
        connection.want_write = false;
        connection.deadline_ms = now_ms + tls_request_ms;
        connection.assembler.reset();
        connection.head_size = 0;
        connection.body = std::span<const char>();
        connection.sent = 0;
    }
}

void TlsServer::step(std::size_t index, std::uint64_t now_ms) noexcept {
    Connection& connection = connections_[index];
    for (int transition = 0; transition < step_transitions_max; transition += 1) {
        const ConnectionPhase before = connection.phase;
        switch (connection.phase) {
        case ConnectionPhase::handshake:
            step_handshake(connection, now_ms);
            break;
        case ConnectionPhase::reading:
            step_reading(index, connection, now_ms);
            break;
        case ConnectionPhase::streaming:
            step_streaming(index, connection, now_ms);
            break;
        case ConnectionPhase::writing:
            step_writing(connection);
            break;
        case ConnectionPhase::idle:
            return;
        default:
            close_connection(connection);
            return;
        }
        if (connection.phase == before) {
            return;
        }
    }
}

void TlsServer::step_handshake(Connection& connection, std::uint64_t now_ms) noexcept {
    (void)now_ms;
    ERR_clear_error();
    const int result = SSL_accept(connection.ssl);
    if (result == 1) {
        connection.phase = ConnectionPhase::reading;
        connection.want_write = false;
        return;
    }
    const int error = SSL_get_error(connection.ssl, result);
    if (error == SSL_ERROR_WANT_READ) {
        connection.want_write = false;
        return;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
        connection.want_write = true;
        return;
    }
    // A client that does not speak TLS 1.3, or plain HTTP on this port.
    ERR_clear_error();
    close_connection(connection);
}

void TlsServer::step_reading(std::size_t index, Connection& connection, std::uint64_t now_ms) noexcept {
    // Each pass commits at least one byte or returns; the assembler's
    // buffer bounds the bytes.
    for (;;) {
        const std::span<char> space = connection.assembler.free_space();
        if (space.empty()) {
            close_connection(connection);
            return;
        }
        std::size_t got = 0;
        ERR_clear_error();
        const int result = SSL_read_ex(connection.ssl, space.data(), space.size(), &got);
        if (result != 1) {
            const int error = SSL_get_error(connection.ssl, result);
            if (error == SSL_ERROR_WANT_READ) {
                connection.want_write = false;
                return;
            }
            if (error == SSL_ERROR_WANT_WRITE) {
                connection.want_write = true;
                return;
            }
            ERR_clear_error();
            close_connection(connection);
            return;
        }
        const AssemblyState state = connection.assembler.commit(got);
        switch (state) {
        case AssemblyState::receiving:
            continue;
        case AssemblyState::streaming:
            begin_streaming(index, connection, now_ms);
            return;
        case AssemblyState::complete:
        case AssemblyState::refused:
            answer_request(connection, now_ms);
            return;
        default:
            close_connection(connection);
            return;
        }
    }
}

ApiRequest TlsServer::request_of(const Connection& connection, std::uint64_t now_ms) const noexcept {
    const RequestHead& head = *connection.assembler.head();
    const std::span<const char> bytes = connection.assembler.bytes();
    ApiRequest request{};
    request.method = head.method;
    request.path = range_text(bytes, head.path);
    request.host = range_text(bytes, head.host);
    if (head.cookie.has_value()) {
        request.cookie = range_text(bytes, *head.cookie);
    }
    if (head.content_type.has_value()) {
        request.content_type = range_text(bytes, *head.content_type);
    }
    if (head.origin.has_value()) {
        request.origin = range_text(bytes, *head.origin);
    }
    if (head.csrf_token.has_value()) {
        request.csrf_token = range_text(bytes, *head.csrf_token);
    }
    request.body = connection.assembler.state() == AssemblyState::complete ? connection.assembler.body()
                                                                            : std::span<const char>();
    request.client = connection.client;
    request.now_ms = now_ms;
    return request;
}

void TlsServer::answer_request(Connection& connection, std::uint64_t now_ms) noexcept {
    const HeadRefused* const refusal = connection.assembler.refusal();
    if (connection.assembler.state() == AssemblyState::refused || refusal != nullptr) {
        respond_plain_error(connection, refusal != nullptr ? (*refusal).status : 400,
                            refusal != nullptr ? (*refusal).reason : "bad_request", now_ms);
        return;
    }
    const Method method = (*connection.assembler.head()).method;
    const ApiResponse response = api_.handle(request_of(connection, now_ms));
    respond(connection, response, method, now_ms);
}

void TlsServer::respond(Connection& connection, const ApiResponse& response, Method method,
                        std::uint64_t now_ms) noexcept {
    if (response.event.kind != ApiEventKind::none) {
        report_event(connection, response.event);
    }
    ResponseHeadFields fields{};
    fields.status = response.status;
    fields.content_type = response.body.empty() ? nullptr : content_type_of(response.kind);
    fields.content_length = response.body.size();
    fields.cacheable = response.cacheable;
    fields.set_cookie = response.set_cookie;
    fields.retry_after_s = response.retry_after_s;
    fields.allow = response.allow;
    const std::optional<std::size_t> head_size =
        write_response_head(fields, std::span<char>(connection.head, sizeof(connection.head)));
    if (!head_size.has_value()) {
        close_connection(connection);
        return;
    }
    connection.head_size = *head_size;
    connection.sent = 0;
    if (method == Method::head || response.body.empty()) {
        connection.body = std::span<const char>();
    } else if (response.kind == ContentKind::json) {
        // The Api's storage is reused by the next request on any
        // connection: copy.
        const std::size_t size =
            response.body.size() <= sizeof(connection.body_copy) ? response.body.size() : sizeof(connection.body_copy);
        for (std::size_t index = 0; index < size; index += 1) {
            connection.body_copy[index] = response.body[index];
        }
        connection.body = std::span<const char>(connection.body_copy, size);
    } else {
        // A page: the backend's storage, for its life.
        connection.body = response.body;
    }
    connection.phase = ConnectionPhase::writing;
    connection.want_write = true;
    connection.deadline_ms = now_ms + tls_response_ms;
}

void TlsServer::respond_plain_error(Connection& connection, std::uint16_t status, const char* reason,
                                    std::uint64_t now_ms) noexcept {
    JsonWriter writer(std::span<char>(error_body_, sizeof(error_body_)));
    writer.begin_object();
    writer.key("error");
    writer.string(reason);
    writer.end_object();
    ApiResponse response{};
    response.status = status;
    response.kind = ContentKind::json;
    response.body = std::span<const char>(error_body_, writer.ok() ? writer.size() : 0u);
    response.cacheable = false;
    response.set_cookie = nullptr;
    response.allow = nullptr;
    response.event.kind = ApiEventKind::none;
    response.event.detail = "";
    response.event.service[0] = '\0';
    response.event.status = "";
    respond(connection, response, Method::post, now_ms);
}

void TlsServer::step_writing(Connection& connection) noexcept {
    // Each pass writes at least one byte or returns.
    for (;;) {
        std::span<const char> part;
        if (connection.sent < connection.head_size) {
            part = std::span<const char>(connection.head + connection.sent, connection.head_size - connection.sent);
        } else if (connection.sent - connection.head_size < connection.body.size()) {
            part = connection.body.subspan(connection.sent - connection.head_size);
        } else {
            // All written: close_notify (once, not waited for), then close.
            ERR_clear_error();
            (void)SSL_shutdown(connection.ssl);
            ERR_clear_error();
            close_connection(connection);
            return;
        }
        std::size_t written = 0;
        ERR_clear_error();
        const int result = SSL_write_ex(connection.ssl, part.data(), part.size(), &written);
        if (result == 1) {
            connection.sent += written;
            continue;
        }
        const int error = SSL_get_error(connection.ssl, result);
        if (error == SSL_ERROR_WANT_WRITE) {
            connection.want_write = true;
            return;
        }
        if (error == SSL_ERROR_WANT_READ) {
            connection.want_write = false;
            return;
        }
        ERR_clear_error();
        close_connection(connection);
        return;
    }
}

void TlsServer::begin_streaming(std::size_t index, Connection& connection, std::uint64_t now_ms) noexcept {
    const RequestHead& head = *connection.assembler.head();
    if (upload_.active) {
        respond_plain_error(connection, 409, "upload_busy", now_ms);
        return;
    }
    const UploadDecision decision = api_.begin_upload(request_of(connection, now_ms), head.content_length);
    const ApiResponse* const refused = decision.as_refused();
    const UploadAccepted* const accepted = decision.as_accepted();
    if (refused != nullptr || accepted == nullptr) {
        if (refused != nullptr) {
            respond(connection, *refused, head.method, now_ms);
        } else {
            respond_plain_error(connection, 500, "upload_refused", now_ms);
        }
        return;
    }
    const char* const path = upload_path_of((*accepted).kind);
    upload_.active = true;
    upload_.connection = index;
    upload_.kind = (*accepted).kind;
    upload_.descriptor = -1;
    upload_.expected = head.content_length;
    upload_.received = 0;
    upload_.hash = nullptr;
    upload_.started_ms = now_ms;
    upload_.last_progress_ms = now_ms;
    connection.phase = ConnectionPhase::streaming;
    connection.deadline_ms = now_ms + tls_upload_total_ms;
    write_console(console_, "upload begin path=", path);
    if (!make_parent_directories(path)) {
        finish_upload(connection, false, errno != 0 ? errno : EIO, now_ms);
        return;
    }
    int descriptor = -1;
    for (int attempt = 0; attempt <= interrupted_retries_max; attempt += 1) {
        descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (descriptor >= 0 || errno != EINTR) {
            break;
        }
    }
    if (descriptor < 0) {
        finish_upload(connection, false, errno != 0 ? errno : EIO, now_ms);
        return;
    }
    upload_.descriptor = descriptor;
    EVP_MD_CTX* const hash = EVP_MD_CTX_new();
    if (hash == nullptr || EVP_DigestInit_ex(hash, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(hash);
        ERR_clear_error();
        finish_upload(connection, false, ENOMEM, now_ms);
        return;
    }
    upload_.hash = hash;
    const std::span<const char> prefix = connection.assembler.streamed_prefix();
    if (!store_upload_bytes(prefix)) {
        finish_upload(connection, false, errno != 0 ? errno : EIO, now_ms);
        return;
    }
    upload_.received = prefix.size();
    if (upload_.received >= upload_.expected) {
        finish_upload(connection, true, 0, now_ms);
    }
}

bool TlsServer::store_upload_bytes(std::span<const char> bytes) noexcept {
    if (bytes.empty()) {
        return true;
    }
    if (!write_all(upload_.descriptor, bytes)) {
        return false;
    }
    if (EVP_DigestUpdate(upload_.hash, bytes.data(), bytes.size()) != 1) {
        ERR_clear_error();
        errno = EIO;
        return false;
    }
    return true;
}

void TlsServer::step_streaming(std::size_t index, Connection& connection, std::uint64_t now_ms) noexcept {
    if (!upload_.active || upload_.connection != index) {
        close_connection(connection);
        return;
    }
    for (std::size_t chunk = 0; chunk < tls_upload_chunks_per_pass; chunk += 1) {
        if (upload_.received >= upload_.expected) {
            finish_upload(connection, true, 0, now_ms);
            return;
        }
        const std::uint64_t remaining = upload_.expected - upload_.received;
        const std::size_t wanted =
            remaining < sizeof(upload_buffer_) ? static_cast<std::size_t>(remaining) : sizeof(upload_buffer_);
        std::size_t got = 0;
        ERR_clear_error();
        const int result = SSL_read_ex(connection.ssl, upload_buffer_, wanted, &got);
        if (result != 1) {
            const int error = SSL_get_error(connection.ssl, result);
            if (error == SSL_ERROR_WANT_READ) {
                connection.want_write = false;
                return;
            }
            if (error == SSL_ERROR_WANT_WRITE) {
                connection.want_write = true;
                return;
            }
            // The client stopped sending.
            ERR_clear_error();
            finish_upload(connection, false, 0, now_ms);
            return;
        }
        if (!store_upload_bytes(std::span<const char>(upload_buffer_, got))) {
            finish_upload(connection, false, errno != 0 ? errno : EIO, now_ms);
            return;
        }
        upload_.received += got;
        upload_.last_progress_ms = now_ms;
    }
    if (upload_.received >= upload_.expected) {
        finish_upload(connection, true, 0, now_ms);
    }
}

void TlsServer::finish_upload(Connection& connection, bool body_complete, int error_number,
                              std::uint64_t now_ms) noexcept {
    UploadSummary summary{};
    summary.stored = false;
    summary.bytes = upload_.received;
    summary.sha256[0] = '\0';
    summary.error_number = error_number;
    bool stored = body_complete && upload_.descriptor >= 0 && upload_.hash != nullptr;
    if (stored && fsync(upload_.descriptor) != 0) {
        summary.error_number = errno;
        stored = false;
    }
    if (upload_.descriptor >= 0 && close(upload_.descriptor) != 0 && stored) {
        summary.error_number = errno;
        stored = false;
    }
    upload_.descriptor = -1;
    if (stored) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(upload_.hash, digest, &length) == 1 && length == 32u) {
            const char digits[] = "0123456789abcdef";
            for (unsigned int index = 0; index < length; index += 1) {
                summary.sha256[2u * index] = digits[digest[index] >> 4u];
                summary.sha256[2u * index + 1u] = digits[digest[index] & 0x0fu];
            }
            summary.sha256[64] = '\0';
        } else {
            ERR_clear_error();
            summary.error_number = EIO;
            stored = false;
        }
    }
    EVP_MD_CTX_free(upload_.hash);
    upload_.hash = nullptr;
    const char* const path = upload_path_of(upload_.kind);
    if (!stored) {
        // A part-written staging file is never left for an install to see.
        (void)unlink(path);
    }
    summary.stored = stored;
    upload_.active = false;
    write_console(console_, stored ? "upload stored path=" : "upload failed path=", path);
    const ApiResponse response = api_.finish_upload(upload_.kind, summary);
    if (!stored && summary.error_number == 0) {
        // The client stopped sending: nobody to answer.
        report_event(connection, response.event);
        close_connection(connection);
        return;
    }
    respond(connection, response, Method::post, now_ms);
}

void TlsServer::abandon_upload() noexcept {
    if (upload_.descriptor >= 0) {
        (void)close(upload_.descriptor);
        upload_.descriptor = -1;
    }
    EVP_MD_CTX_free(upload_.hash);
    upload_.hash = nullptr;
    (void)unlink(upload_path_of(upload_.kind));
    upload_.active = false;
    write_console(console_, "upload abandoned path=", upload_path_of(upload_.kind));
}

void TlsServer::close_connection(Connection& connection) noexcept {
    if (connection.phase == ConnectionPhase::idle) {
        return;
    }
    if (upload_.active && &connections_[upload_.connection] == &connection) {
        abandon_upload();
    }
    SSL_free(connection.ssl);
    connection.ssl = nullptr;
    if (connection.descriptor >= 0) {
        (void)close(connection.descriptor);
    }
    connection.descriptor = -1;
    connection.phase = ConnectionPhase::idle;
    connection.want_write = false;
    connection.body = std::span<const char>();
    connection.head_size = 0;
    connection.sent = 0;
}

void TlsServer::report_event(const Connection& connection, const ApiEvent& event) noexcept {
    char address[address_text_bytes_max];
    const std::span<const std::uint8_t, 16> bytes(connection.client.bytes);
    (void)format_address(connection.client.family, bytes, std::span<char, address_text_bytes_max>(address));
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXWEB event=");
    stxruntime::append_text(line, event_kind_name(event.kind));
    stxruntime::append_text(line, " client=");
    stxruntime::append_text(line, address);
    if (event.detail != nullptr && event.detail[0] != '\0') {
        stxruntime::append_text(line, " detail=");
        stxruntime::append_text(line, event.detail);
    }
    if (event.service[0] != '\0') {
        stxruntime::append_text(line, " service=");
        stxruntime::append_text(line, event.service);
    }
    if (event.status != nullptr && event.status[0] != '\0') {
        stxruntime::append_text(line, " status=");
        stxruntime::append_text(line, event.status);
    }
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console_, line);
}

}  // namespace stxweb
