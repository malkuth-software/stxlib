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
//   GET  /api/config                  the factory configuration's text
//   POST /api/reboot                  stop the services, reboot (applies
//                                     the user configuration)
//
// The 1.0 pages (C2-web.md, section 3.3), on the user configuration
// (config/user_config.h), which takes effect at the next boot:
//
//   GET  /api/settings                {source, sequence, pending, text}
//   POST /api/settings {text, base_sequence}   the whole text, written as
//                                     the next turn; 409 when another write
//                                     came first, 422 when it is refused
//   POST /api/settings/check {text}   parse and user rules, nothing written
//   GET  /api/targets                 every target: its settings, enabled
//                                     at boot, its service, its program
//   POST /api/targets/<name> {settings, enabled}   add or replace one
//   POST /api/targets/<name>/delete
//   GET  /api/targets/<name>/program  its program's slots and versions
//   POST /api/targets/<name>/install {source: "upload"|"url", url}
//   POST /api/targets/<name>/rollback
//   POST /api/targets/<name>/check-update
//   POST /api/upload/package          (application/octet-stream, streamed
//   POST /api/upload/system            to the staging files; begin_upload)
//   POST /api/system/update           the staged image to the other slot
//   GET  /api/operation               the long operation's progress
//   GET  /api/network                 addresses, and the network settings
//   POST /api/network {hostname, mode, address, gateway, dns}
//   GET  /api/access                  the SSH keys
//   POST /api/access {keys}
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
//     (control_server.h), so the interface cannot cut itself off; and the
//     user configuration cannot touch the factory configuration, where the
//     web service and sshd are defined (config/user_config.h).

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
#include "config_edit.h"
#include "http.h"
#include "json.h"

namespace stxweb {

// The largest response body: the targets page with every target's
// settings (a 16 KiB configuration, escaped) and its service and program.
constexpr std::size_t api_body_bytes_max = 65536;
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
// Value rules for the settings pages. The host side gives the settings
// their meaning (sb2's C2 note, K4); these keep values the pages write to
// forms it can read.

// RFC 1123 host name: labels of 1..63 letters, digits and '-', not
// starting or ending with '-', joined by '.', at most 253 bytes.
[[nodiscard]] bool hostname_valid(std::span<const char> text) noexcept;
// Dotted-decimal IPv4: four decimal numbers 0..255 without leading zeros.
[[nodiscard]] bool ipv4_text_valid(std::span<const char> text) noexcept;
// IPv4 with a prefix length: "a.b.c.d/n", n 0..32 without leading zeros.
[[nodiscard]] bool ipv4_prefix_text_valid(std::span<const char> text) noexcept;
// An OpenSSH public key line: a key type sshd accepts for key login
// (ssh-ed25519, ecdsa-sha2-nistp256/384/521, sk-ssh-ed25519@openssh.com,
// sk-ecdsa-sha2-nistp256@openssh.com), one space, base64 (A-Z a-z 0-9 + /,
// '=' padding only at the end), then optionally a space and a comment of
// visible ASCII and spaces. RSA keys are longer than a configuration line
// and are refused by length.
[[nodiscard]] bool ssh_key_valid(std::span<const char> text) noexcept;
constexpr std::size_t ssh_keys_max = 8;

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

    // The factory configuration's text into out (the root's
    // config_path; the user configuration comes from the supervisor).
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
// Uploads: the request's head is checked here, its body streamed by the
// server into the staging file (stxcontrol::staged_package_path or
// staged_system_path), and the result answered here.

enum class UploadKind : std::uint8_t {
    package,
    system,
};

// Sizes the staging files take at most: a program package and a root
// image (the slot is 64 MiB; appliance/disk/LAYOUT.md). The host side
// checks what it takes (sb2's C2 note, K5 and K12).
constexpr std::uint64_t upload_package_bytes_max = 64u * 1024u * 1024u;
constexpr std::uint64_t upload_system_bytes_max = 128u * 1024u * 1024u;

[[nodiscard]] std::optional<UploadKind> upload_kind_of(std::span<const char> path) noexcept;
[[nodiscard]] std::uint64_t upload_bytes_max(UploadKind kind) noexcept;
[[nodiscard]] const char* upload_path_of(UploadKind kind) noexcept;

struct UploadAccepted {
    UploadKind kind;
};

class UploadDecision {
public:
    [[nodiscard]] static UploadDecision accepted(UploadKind kind) noexcept;
    [[nodiscard]] static UploadDecision refused(const ApiResponse& response) noexcept;
    [[nodiscard]] const UploadAccepted* as_accepted() const noexcept;
    [[nodiscard]] const ApiResponse* as_refused() const noexcept;

private:
    using Alternatives = std::variant<UploadAccepted, ApiResponse>;
    explicit UploadDecision(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// What the server did with an accepted upload's body.
struct UploadSummary {
    // Written whole and synced.
    bool stored;
    std::uint64_t bytes;
    // The body's SHA-256, lower-case hex, NUL-terminated (empty unless
    // stored).
    char sha256[65];
    // stored false: errno of the failure, or 0 when the client stopped
    // sending.
    int error_number;
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

    // An upload route's head (request.body empty; content_length from the
    // head): the same checks as any POST that needs a session (octet-stream
    // instead of JSON), and the size against upload_bytes_max. Accepted:
    // the server streams the body, then calls finish_upload.
    [[nodiscard]] UploadDecision begin_upload(const ApiRequest& request, std::uint64_t content_length) noexcept;
    [[nodiscard]] ApiResponse finish_upload(UploadKind kind, const UploadSummary& summary) noexcept;

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
    void handle_reboot(ApiResponse& response) noexcept;

    // The 1.0 pages (api_pages.cpp).
    void handle_settings_read(ApiResponse& response) noexcept;
    void handle_settings_write(ApiResponse& response) noexcept;
    void handle_settings_check(ApiResponse& response) noexcept;
    void handle_targets(ApiResponse& response) noexcept;
    void handle_target_write(const stxcontrol::ServiceName& name, ApiResponse& response) noexcept;
    void handle_target_delete(const stxcontrol::ServiceName& name, ApiResponse& response) noexcept;
    void handle_program(stxcontrol::Command command, const stxcontrol::ServiceName& name,
                        ApiResponse& response) noexcept;
    void handle_system_update(ApiResponse& response) noexcept;
    void handle_operation(ApiResponse& response) noexcept;
    void handle_network_write(ApiResponse& response) noexcept;
    void handle_access(ApiResponse& response) noexcept;
    void handle_access_write(ApiResponse& response) noexcept;

    // The request's live session, when its cookie names one.
    [[nodiscard]] std::optional<std::size_t> session_of(const ApiRequest& request) noexcept;
    // A POST's Origin, when present, is "https://" + Host.
    [[nodiscard]] static bool origin_acceptable(const ApiRequest& request) noexcept;

    // The user configuration: read through the supervisor into editor_
    // (true); false with the reason in fetch_failure_ (nothing: the
    // supervisor did not answer usably).
    [[nodiscard]] bool fetch_user_config() noexcept;
    // fetch_user_config, answering the error when it fails.
    [[nodiscard]] bool load_user_config(ApiResponse& response) noexcept;
    // Parses editor_'s text into check_config_; false (answered 500) when
    // it does not parse.
    [[nodiscard]] bool parse_user_config(ApiResponse& response) noexcept;
    // Sends text as the next turn and answers.
    void write_user_config(std::span<const char> text, ApiResponse& response) noexcept;
    // An edit's result: false (answered 400) unless done.
    [[nodiscard]] bool edit_done(EditResult result, ApiResponse& response) noexcept;
    // The section's settings in check_config_ as a JSON object.
    void write_section_settings(JsonWriter& writer, const char* section) const noexcept;
    // A supervisor answer as an error, when it is not ok: false.
    [[nodiscard]] bool supervisor_ok(const std::optional<stxcontrol::ResponseView>& answer,
                                     stxcontrol::Command command, ApiResponse& response) noexcept;
    void write_program_record(JsonWriter& writer, const stxcontrol::ProgramRecord& record) const noexcept;

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
    // The factory configuration, parsed for the settings check.
    stxconfig::BootConfig factory_config_;
    NetworkAddress addresses_[network_addresses_max];
    // The user configuration being read or edited, and its turn.
    ConfigEditor editor_;
    stxcontrol::ConfigSource user_source_;
    std::uint32_t user_sequence_;
    bool user_written_since_start_;
    std::optional<stxcontrol::Status> fetch_failure_;
    // A request payload being built (config text, install source).
    std::uint8_t request_payload_[stxcontrol::request_payload_max];
};

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_API_H
