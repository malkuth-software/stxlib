// tls_server.h: the web service's listener, its TLS connections and its
// one loop (C2; design dev/notes/planning/stages/C2-web.md, section 4).
//
// One thread, one poll(2) loop over the listener, the control channel and
// at most tls_connections_max connections. Each connection carries one
// request (http.h: every response closes):
//
//   handshake   TLS 1.3 only, base OpenSSL, the identity of crypto_system.h;
//               no session tickets and no session cache (nothing to resume
//               across a restart, nothing kept per client)
//   reading     the request into the connection's RequestAssembler
//   streaming   an upload's body (api.h, begin_upload) into its staging
//               file, hashed with SHA-256 on the way; one upload at a time
//   writing     the response head and body; then close_notify and close
//
// Deadlines, from the phase's start: the handshake and the request
// together tls_request_ms; the response tls_response_ms; an upload's body
// no gap longer than tls_upload_idle_ms and all of it within
// tls_upload_total_ms. A connection past its deadline is closed without an
// answer. A connection beyond tls_connections_max is accepted and closed at
// once (so the listener does not stay readable).
//
// The listener: AF_INET6 with IPV6_V6ONLY off (both families), falling back
// to AF_INET when the kernel has no INET6 (the Pi kernel's STXLIB-PI4 has
// none); SO_REUSEADDR, so a restarted service binds at once.
//
// The loop ends when the control channel closes (the supervisor went away:
// C2-web.md, owner question 4), or on SIGTERM (the supervisor's stop).
//
// System calls: socket, setsockopt, bind, listen, accept4, getpeername,
// poll, close, open, write, fsync, unlink, mkdir, clock_gettime, and
// OpenSSL's reads and writes on the sockets.

#ifndef STXLIB_RUNTIME_WEB_TLS_SERVER_H
#define STXLIB_RUNTIME_WEB_TLS_SERVER_H

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "api.h"
#include "control_client.h"
#include "crypto_system.h"
#include "http.h"

struct ssl_st;
struct ssl_ctx_st;
struct evp_md_ctx_st;

namespace stxweb {

constexpr std::size_t tls_connections_max = 8;
constexpr int tls_listen_backlog = 16;
constexpr std::uint64_t tls_request_ms = 10u * 1000u;
constexpr std::uint64_t tls_response_ms = 10u * 1000u;
constexpr std::uint64_t tls_upload_idle_ms = 10u * 1000u;
constexpr std::uint64_t tls_upload_total_ms = 15u * 60u * 1000u;
// Bytes of an upload read per pass of the loop before the other
// connections get their turn.
constexpr std::size_t tls_upload_chunk_bytes = 16384;
constexpr std::size_t tls_upload_chunks_per_pass = 16;

[[nodiscard]] std::uint64_t monotonic_now_ms() noexcept;

enum class ConnectionPhase : std::uint8_t {
    idle,
    handshake,
    reading,
    streaming,
    writing,
};

struct Connection {
    ConnectionPhase phase;
    int descriptor;
    ssl_st* ssl;
    ClientKey client;
    // The last TLS call wants the socket writable (else readable).
    bool want_write;
    std::uint64_t deadline_ms;
    RequestAssembler assembler;
    char head[http_response_head_bytes_max];
    std::size_t head_size;
    // A JSON body is copied here (the Api's storage is reused by the next
    // request); a page's body is borrowed from the backend.
    char body_copy[api_body_bytes_max];
    std::span<const char> body;
    // Bytes of head, then body, written.
    std::size_t sent;
};

// Why run() returned.
enum class ServerExit : std::uint8_t {
    channel_closed,
    terminated,
    failed,
};

[[nodiscard]] const char* server_exit_name(ServerExit exit) noexcept;

// Large (eight connections of about 115 KiB): the caller owns one, with
// static storage.
class TlsServer {
public:
    // Borrows api and client; console is the descriptor for event lines.
    TlsServer(Api& api, ControlClient& client, int console) noexcept;
    TlsServer(const TlsServer&) = delete;
    TlsServer& operator=(const TlsServer&) = delete;
    ~TlsServer();

    // The TLS context for identity (borrowed for the server's life) and
    // the listener on port. False, with a console line, when either fails.
    [[nodiscard]] bool start(const TlsIdentity& identity, std::uint16_t port) noexcept;

    // Serves until the channel closes or *terminate turns true (set by the
    // SIGTERM handler; poll returns EINTR).
    [[nodiscard]] ServerExit run(const volatile std::sig_atomic_t* terminate) noexcept;

private:
    struct Upload {
        bool active;
        std::size_t connection;
        UploadKind kind;
        int descriptor;
        std::uint64_t expected;
        std::uint64_t received;
        evp_md_ctx_st* hash;
        std::uint64_t started_ms;
        std::uint64_t last_progress_ms;
    };

    void accept_connections(std::uint64_t now_ms) noexcept;
    void step(std::size_t index, std::uint64_t now_ms) noexcept;
    void step_handshake(Connection& connection, std::uint64_t now_ms) noexcept;
    void step_reading(std::size_t index, Connection& connection, std::uint64_t now_ms) noexcept;
    void step_streaming(std::size_t index, Connection& connection, std::uint64_t now_ms) noexcept;
    void step_writing(Connection& connection) noexcept;
    // A complete request (or a refused head) into a response.
    void answer_request(Connection& connection, std::uint64_t now_ms) noexcept;
    // An upload's head: begin it, or answer the refusal.
    void begin_streaming(std::size_t index, Connection& connection, std::uint64_t now_ms) noexcept;
    // Writes bytes to the staging file and the hash; false on failure.
    [[nodiscard]] bool store_upload_bytes(std::span<const char> bytes) noexcept;
    // Ends the upload: synced and answered (stored), or removed.
    void finish_upload(Connection& connection, bool body_complete, int error_number, std::uint64_t now_ms) noexcept;
    void abandon_upload() noexcept;
    void respond(Connection& connection, const ApiResponse& response, Method method, std::uint64_t now_ms) noexcept;
    void respond_plain_error(Connection& connection, std::uint16_t status, const char* reason,
                             std::uint64_t now_ms) noexcept;
    void close_connection(Connection& connection) noexcept;
    void report_event(const Connection& connection, const ApiEvent& event) noexcept;
    [[nodiscard]] ApiRequest request_of(const Connection& connection, std::uint64_t now_ms) const noexcept;
    [[nodiscard]] int poll_timeout_ms(std::uint64_t now_ms) const noexcept;
    [[nodiscard]] bool has_buffered_input(const Connection& connection) const noexcept;

    Api& api_;
    ControlClient& client_;
    int console_;
    ssl_ctx_st* context_;
    int listener_;
    Connection connections_[tls_connections_max];
    Upload upload_;
    char upload_buffer_[tls_upload_chunk_bytes];
    char error_body_[96];
};

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_TLS_SERVER_H
