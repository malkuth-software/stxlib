// api.h: the web interface's routes, its JSON interface and its security
// decisions, the pure part (C2; design dev/notes/planning/stages/C2-web.md,
// sections 3 and 5).
//
// One request in (already read and checked by http.h), one response out.
// Everything the system does (ask the supervisor, check or store the
// password, read the addresses, the configuration and the pages) goes
// through ApiBackend, which tls_server.cpp implements and the host tests
// fake.
//
// Routes (every response body is JSON, except the static pages):
//
//   GET  /  /index.html  /app.js  /style.css   the pages (no login)
//   GET  /api/session                 {logged_in, setup_required[, csrf]}
//   POST /api/login      {password}   a session: cookie, and {csrf}
//   POST /api/setup      {code, password}  first password (no password yet)
//   POST /api/logout                  ends the session
//   POST /api/password   {current, new}    ends every other session
//   GET  /api/supervisor              the supervisor's state
//   GET  /api/services                every service
//   GET  /api/services/<name>         one service
//   POST /api/services/<name>/start   (also stop, restart)
//   GET  /api/slots                   A/B slot status
//   GET  /api/boot-log                the boot log's end
//   GET  /api/network                 the interfaces' addresses
//   GET  /api/config                  the boot configuration's text
//   POST /api/config/check {text}     the configuration parser's verdict
//
// Security (LuCI-style, C-appliance-design.md section 6):
//   - every /api/ route but session, login and setup needs a live session
//     (cookie "stxsession", HttpOnly, Secure, SameSite=Strict); else 401;
//   - every POST needs Content-Type application/json (415 otherwise: a
//     cross-site form cannot send it without a CORS preflight, which this
//     server never grants); an Origin field, when present, must be
//     "https://" + Host (403 otherwise);
//   - every POST that needs a session also needs X-STX-CSRF equal to the
//     session's CSRF token (403 otherwise);
//   - password and setup-code checks pass the client's lockout gate first
//     (auth.h, GateTable); a locked client gets 429 with Retry-After and no
//     password check at all;
//   - a stop of the web service itself is refused by the supervisor
//     (control_server.h), so the interface cannot cut itself off.

#ifndef STXLIB_RUNTIME_WEB_API_H
#define STXLIB_RUNTIME_WEB_API_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "../supervisor/config/boot_config.h"
#include "../supervisor/control/control_protocol.h"
#include "auth.h"
#include "http.h"
#include "json.h"

namespace stxweb {

constexpr std::size_t api_body_bytes_max = 40960;
constexpr std::size_t network_addresses_max = 32;
constexpr std::size_t interface_name_bytes_max = 16;
// Room for the longest address text: an IPv6 address, 39 characters.
constexpr std::size_t address_text_bytes_max = 40;

enum class ContentKind : std::uint8_t {
    json,
    html,
    javascript,
    css,
};

[[nodiscard]] const char* content_type_of(ContentKind kind) noexcept;

// The pages: a fixed list, served from memory (assets.h); no request path
// ever names a file.
struct AssetName {
    const char* path;
    const char* file;
    ContentKind kind;
};
constexpr std::size_t asset_count = 4;
extern const AssetName asset_names[asset_count];
// The asset a path names, or nothing.
[[nodiscard]] std::optional<std::size_t> asset_index(std::span<const char> path) noexcept;

// ---------------------------------------------------------------------------
// Addresses.

struct NetworkAddress {
    // NUL-terminated.
    char interface_name[interface_name_bytes_max + 1];
    AddressFamily family;
    // IPv4 in the first 4 bytes.
    std::uint8_t bytes[16];
    std::uint8_t prefix_length;
};

// Dotted decimal, or RFC 5952 text (lower case, the longest run of two or
// more zero groups shortened to "::", the first such run on a tie). Writes
// a NUL-terminated text into out and returns its length.
std::size_t format_address(AddressFamily family, std::span<const std::uint8_t, 16> bytes,
                           std::span<char, address_text_bytes_max> out) noexcept;

// ---------------------------------------------------------------------------
// The backend.

enum class PasswordCheck : std::uint8_t {
    matches,
    wrong,
    // No usable record, or the key derivation failed.
    unavailable,
};

struct ConfigRead {
    std::size_t size;
};
struct ConfigAbsent {
};
struct ConfigFailed {
    int error_number;
};

class ConfigOutcome {
public:
    [[nodiscard]] static ConfigOutcome read(std::size_t size) noexcept;
    [[nodiscard]] static ConfigOutcome absent() noexcept;
    [[nodiscard]] static ConfigOutcome failed(int error_number) noexcept;
    [[nodiscard]] const ConfigRead* as_read() const noexcept;
    [[nodiscard]] bool is_absent() const noexcept;
    [[nodiscard]] const ConfigFailed* as_failed() const noexcept;

private:
    using Alternatives = std::variant<ConfigRead, ConfigAbsent, ConfigFailed>;
    explicit ConfigOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

class ApiBackend {
public:
    ApiBackend() noexcept = default;
    ApiBackend(const ApiBackend&) = delete;
    ApiBackend& operator=(const ApiBackend&) = delete;

    // Sends the request on the control channel and waits for its answer.
    // Nothing when there is no channel, it timed out, or the answer was
    // malformed or not this request's. The view's payload borrows the
    // backend's storage until the next call.
    [[nodiscard]] virtual std::optional<stxcontrol::ResponseView> ask_supervisor(
        const stxcontrol::Request& request) noexcept = 0;

    [[nodiscard]] virtual bool password_is_set() noexcept = 0;
    [[nodiscard]] virtual PasswordCheck verify_password(std::span<const char> password) noexcept = 0;
    // Stores a record for the password with a fresh salt. False when that
    // failed (the old record, if any, stays).
    [[nodiscard]] virtual bool set_password(std::span<const char> password) noexcept = 0;

    // The asset's bytes, or nothing when it could not be loaded. The span
    // lives as long as the backend.
    [[nodiscard]] virtual std::optional<std::span<const char>> asset(std::size_t index) noexcept = 0;

    // The interfaces' addresses into out; how many, or nothing on failure.
    [[nodiscard]] virtual std::optional<std::size_t> list_addresses(std::span<NetworkAddress> out) noexcept = 0;

    // The boot configuration's text into out.
    [[nodiscard]] virtual ConfigOutcome read_config(std::span<char> out) noexcept = 0;
    // Its path, for the answer.
    [[nodiscard]] virtual const char* config_path() noexcept = 0;

    [[nodiscard]] virtual RandomSource& random() noexcept = 0;

protected:
    ~ApiBackend() = default;
};

// ---------------------------------------------------------------------------
// Requests and responses.

struct ApiRequest {
    Method method;
    std::span<const char> path;
    std::span<const char> host;
    std::optional<std::span<const char>> cookie;
    std::optional<std::span<const char>> content_type;
    std::optional<std::span<const char>> origin;
    std::optional<std::span<const char>> csrf_token;
    std::span<const char> body;
    ClientKey client;
    std::uint64_t now_ms;
};

// What the request did, for the service's console line.
enum class ApiEventKind : std::uint8_t {
    none,
    login_ok,
    login_failed,
    login_locked,
    setup_done,
    setup_failed,
    password_changed,
    logout,
    operated,
    refused,
};

struct ApiEvent {
    ApiEventKind kind;
    // operated: the command; refused: why ("csrf", "origin", ...).
    const char* detail;
    // operated: the service (NUL-terminated); otherwise empty.
    char service[stxcontrol::service_name_bytes_max + 1];
    // operated: the supervisor's status name.
    const char* status;
};

struct ApiResponse {
    std::uint16_t status;
    ContentKind kind;
    // Borrowed: the Api's body storage (until the next handle) or an
    // asset (as long as the backend).
    std::span<const char> body;
    bool cacheable;
    // Borrowed from the Api until the next handle; null for none.
    const char* set_cookie;
    std::optional<std::uint32_t> retry_after_s;
    const char* allow;
    ApiEvent event;
};

// ---------------------------------------------------------------------------
// The interface's state and its one operation.

// Large (the configuration checker's BootConfig, about 50 KiB, and the
// body storage): the caller owns one, with static storage.
class Api {
public:
    // Borrows backend, which must outlive the Api.
    explicit Api(ApiBackend& backend) noexcept;
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    // While no password is set: the code the console shows; the setup
    // route accepts only it. Nothing once a password is set.
    void set_setup_code(const std::optional<SetupCode>& code) noexcept;

    [[nodiscard]] ApiResponse handle(const ApiRequest& request) noexcept;

    // For tests.
    [[nodiscard]] const SessionTable& sessions() const noexcept;
    [[nodiscard]] const GateTable& gates() const noexcept;

private:
    // Route handlers. session: the request's live session, when it has one.
    void route_api(const ApiRequest& request, ApiResponse& response) noexcept;
    void route_static(const ApiRequest& request, ApiResponse& response) noexcept;
    void handle_session(std::optional<std::size_t> session, ApiResponse& response) noexcept;
    void handle_login(const ApiRequest& request, ApiResponse& response) noexcept;
    void handle_setup(const ApiRequest& request, ApiResponse& response) noexcept;
    void handle_logout(std::size_t session, ApiResponse& response) noexcept;
    void handle_password(const ApiRequest& request, std::size_t session, ApiResponse& response) noexcept;
    void handle_supervisor_query(stxcontrol::Command command, ApiResponse& response) noexcept;
    void handle_service(stxcontrol::Command command, const stxcontrol::ServiceName& name,
                        ApiResponse& response) noexcept;
    void handle_network(ApiResponse& response) noexcept;
    void handle_config(ApiResponse& response) noexcept;
    void handle_config_check(ApiResponse& response) noexcept;

    // Responses.
    void respond_error(ApiResponse& response, std::uint16_t status, const char* error) noexcept;
    void respond_json(ApiResponse& response, std::uint16_t status) noexcept;
    void respond_locked(ApiResponse& response, const GateDecision& decision) noexcept;
    void set_session_cookie(ApiResponse& response, std::size_t session) noexcept;
    void clear_session_cookie(ApiResponse& response) noexcept;
    void write_csrf_body(std::size_t session) noexcept;

    // The body's top-level object's string member into destination;
    // nothing when absent, not a string or too long.
    [[nodiscard]] std::optional<std::span<const char>> body_string(const char* key,
                                                                   std::span<char> destination) const noexcept;

    ApiBackend& backend_;
    SessionTable sessions_;
    GateTable gates_;
    std::optional<SetupCode> setup_code_;
    JsonDocument document_;
    // The body being written, and its writer's result.
    char body_[api_body_bytes_max];
    std::size_t body_size_;
    char set_cookie_[160];
    // Decoded body strings.
    char password_[password_bytes_max + 1];
    char second_password_[password_bytes_max + 1];
    char config_text_[stxconfig::config_size_max + 1];
    stxconfig::BootConfig check_config_;
    NetworkAddress addresses_[network_addresses_max];
};

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_API_H
