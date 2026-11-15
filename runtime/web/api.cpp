// api.cpp: the web interface's routes and security decisions (api.h).

#include "api.h"

#include <cassert>

#include "api_internal.h"

namespace stxweb {

// ---------------------------------------------------------------------------
// Content kinds and assets.

const char* content_type_of(ContentKind kind) noexcept {
    switch (kind) {
    case ContentKind::json: return "application/json";
    case ContentKind::html: return "text/html; charset=utf-8";
    case ContentKind::javascript: return "text/javascript; charset=utf-8";
    case ContentKind::css: return "text/css; charset=utf-8";
    default: break;
    }
    // Fallback: unknown bytes are never sniffed (nosniff is always sent).
    return "application/octet-stream";
}

const AssetName asset_names[asset_count] = {
    {"/", "index.html", ContentKind::html},
    {"/index.html", "index.html", ContentKind::html},
    {"/app.js", "app.js", ContentKind::javascript},
    {"/style.css", "style.css", ContentKind::css},
};

std::optional<std::size_t> asset_index(std::span<const char> path) noexcept {
    for (std::size_t index = 0; index < asset_count; index += 1) {
        if (equals_exact(path, asset_names[index].path)) {
            const std::optional<std::size_t> found = index;
            return found;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Addresses.

namespace {

// Appends a NUL-terminated text, keeping room for the final NUL.
void append(std::span<char> out, std::size_t& size, const char* text) noexcept {
    for (std::size_t index = 0; text[index] != '\0' && size + 1u < out.size(); index += 1) {
        out[size] = text[index];
        size += 1;
    }
    out[size] = '\0';
}

void append_decimal(std::span<char> out, std::size_t& size, unsigned int value) noexcept {
    char digits[11];
    std::size_t count = 0;
    unsigned int remaining = value;
    do {
        digits[9u - count] = static_cast<char>('0' + static_cast<int>(remaining % 10u));
        remaining /= 10u;
        count += 1;
    } while (remaining != 0u);
    digits[10] = '\0';
    append(out, size, &digits[10u - count]);
}

void append_hex_group(std::span<char> out, std::size_t& size, unsigned int group) noexcept {
    static const char digits[] = "0123456789abcdef";
    char text[5] = {};
    std::size_t count = 0;
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const unsigned int digit = (group >> static_cast<unsigned int>(shift)) & 0xfu;
        if (digit != 0u || started || shift == 0) {
            text[count] = digits[digit];
            count += 1;
            started = true;
        }
    }
    text[count] = '\0';
    append(out, size, text);
}

}  // namespace

std::size_t format_address(AddressFamily family, std::span<const std::uint8_t, 16> bytes,
                           std::span<char, address_text_bytes_max> out) noexcept {
    const std::span<char> text(out.data(), out.size());
    std::size_t size = 0;
    text[0] = '\0';
    if (family == AddressFamily::ipv4) {
        for (std::size_t index = 0; index < 4u; index += 1) {
            if (index > 0u) {
                append(text, size, ".");
            }
            append_decimal(text, size, bytes[index]);
        }
        return size;
    }
    unsigned int groups[8];
    for (std::size_t index = 0; index < 8u; index += 1) {
        groups[index] = (static_cast<unsigned int>(bytes[2u * index]) << 8u) | bytes[2u * index + 1u];
    }
    // The longest run of zero groups, two or more long; the first on a tie
    // (RFC 5952 section 4.2).
    std::size_t best_start = 8;
    std::size_t best_length = 0;
    std::size_t index = 0;
    while (index < 8u) {
        if (groups[index] != 0u) {
            index += 1;
            continue;
        }
        std::size_t end = index;
        while (end < 8u && groups[end] == 0u) {
            end += 1;
        }
        if (end - index > best_length) {
            best_start = index;
            best_length = end - index;
        }
        index = end;
    }
    if (best_length < 2u) {
        best_start = 8;
        best_length = 0;
    }
    for (std::size_t group = 0; group < 8u; group += 1) {
        if (group == best_start) {
            append(text, size, "::");
            group += best_length - 1u;
            continue;
        }
        if (group > 0u && group != best_start + best_length) {
            append(text, size, ":");
        }
        append_hex_group(text, size, groups[group]);
    }
    return size;
}

// ---------------------------------------------------------------------------
// ConfigOutcome.

ConfigOutcome::ConfigOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

ConfigOutcome ConfigOutcome::read(std::size_t size) noexcept {
    ConfigRead state{};
    state.size = size;
    const ConfigOutcome outcome(Alternatives(std::in_place_type<ConfigRead>, state));
    return outcome;
}

ConfigOutcome ConfigOutcome::absent() noexcept {
    const ConfigOutcome outcome(Alternatives(std::in_place_type<ConfigAbsent>));
    return outcome;
}

ConfigOutcome ConfigOutcome::failed(int error_number) noexcept {
    ConfigFailed state{};
    state.error_number = error_number;
    const ConfigOutcome outcome(Alternatives(std::in_place_type<ConfigFailed>, state));
    return outcome;
}

const ConfigRead* ConfigOutcome::as_read() const noexcept {
    return std::get_if<ConfigRead>(&alternatives_);
}

bool ConfigOutcome::is_absent() const noexcept {
    return std::holds_alternative<ConfigAbsent>(alternatives_);
}

const ConfigFailed* ConfigOutcome::as_failed() const noexcept {
    return std::get_if<ConfigFailed>(&alternatives_);
}

// ---------------------------------------------------------------------------
// Routes.

namespace {

enum class Route : std::uint8_t {
    session,
    login,
    setup,
    logout,
    password,
    supervisor,
    services,
    service,
    service_action,
    slots,
    boot_log,
    network,
    network_write,
    config,
    reboot,
    settings,
    settings_write,
    settings_check,
    targets,
    target_write,
    target_delete,
    program,
    program_action,
    target_log,
    upload,
    system_update,
    operation,
    access,
    access_write,
    not_found,
};

struct RouteMatch {
    Route route;
    // GET routes also take HEAD.
    Method method;
    stxcontrol::ServiceName name;
    stxcontrol::Command action;
};

[[nodiscard]] bool starts_with(std::span<const char> bytes, const char* prefix) noexcept {
    std::size_t index = 0;
    for (; prefix[index] != '\0'; index += 1) {
        if (index >= bytes.size() || bytes[index] != prefix[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t c_length(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length;
}

// A path below prefix: "<name>" or "<name>/<rest>". Sets name and rest
// (empty for none, with has_rest false); false when the name is not one.
[[nodiscard]] bool named_path(std::span<const char> path, const char* prefix, stxcontrol::ServiceName& name,
                              std::span<const char>& rest, bool& has_rest) noexcept {
    if (!starts_with(path, prefix)) {
        return false;
    }
    const std::span<const char> after = path.subspan(c_length(prefix));
    std::size_t slash = 0;
    while (slash < after.size() && after[slash] != '/') {
        slash += 1;
    }
    const std::optional<stxcontrol::ServiceName> found = stxcontrol::service_name_of(after.subspan(0, slash));
    if (!found.has_value()) {
        return false;
    }
    name = *found;
    has_rest = slash < after.size();
    rest = has_rest ? after.subspan(slash + 1u) : after.subspan(after.size());
    return true;
}

[[nodiscard]] RouteMatch route_of(std::span<const char> path) noexcept {
    RouteMatch match{};
    match.route = Route::not_found;
    match.method = Method::get;
    match.name.length = 0;
    match.action = stxcontrol::Command::service_state;
    struct Fixed {
        const char* path;
        Route route;
        Method method;
    };
    // A path that two routes share (GET and POST) is listed once per
    // method; the first entry with the request's method wins, and a path
    // known only under the other method answers 405.
    static const Fixed fixed[] = {
        {"/api/session", Route::session, Method::get},
        {"/api/login", Route::login, Method::post},
        {"/api/setup", Route::setup, Method::post},
        {"/api/logout", Route::logout, Method::post},
        {"/api/password", Route::password, Method::post},
        {"/api/supervisor", Route::supervisor, Method::get},
        {"/api/services", Route::services, Method::get},
        {"/api/slots", Route::slots, Method::get},
        {"/api/boot-log", Route::boot_log, Method::get},
        {"/api/network", Route::network, Method::get},
        {"/api/network", Route::network_write, Method::post},
        {"/api/config", Route::config, Method::get},
        {"/api/reboot", Route::reboot, Method::post},
        {"/api/settings", Route::settings, Method::get},
        {"/api/settings", Route::settings_write, Method::post},
        {"/api/settings/check", Route::settings_check, Method::post},
        {"/api/targets", Route::targets, Method::get},
        {"/api/upload/package", Route::upload, Method::post},
        {"/api/upload/system", Route::upload, Method::post},
        {"/api/system/update", Route::system_update, Method::post},
        {"/api/operation", Route::operation, Method::get},
        {"/api/access", Route::access, Method::get},
        {"/api/access", Route::access_write, Method::post},
    };
    for (const Fixed& item : fixed) {
        if (equals_exact(path, item.path)) {
            match.route = item.route;
            match.method = item.method;
            return match;
        }
    }
    std::span<const char> rest;
    bool has_rest = false;
    if (named_path(path, "/api/services/", match.name, rest, has_rest)) {
        if (!has_rest) {
            match.route = Route::service;
            match.method = Method::get;
            return match;
        }
        if (equals_exact(rest, "start")) {
            match.action = stxcontrol::Command::start;
        } else if (equals_exact(rest, "stop")) {
            match.action = stxcontrol::Command::stop;
        } else if (equals_exact(rest, "restart")) {
            match.action = stxcontrol::Command::restart;
        } else {
            return match;
        }
        match.route = Route::service_action;
        match.method = Method::post;
        return match;
    }
    if (named_path(path, "/api/targets/", match.name, rest, has_rest)) {
        if (!has_rest) {
            match.route = Route::target_write;
            match.method = Method::post;
            return match;
        }
        if (equals_exact(rest, "delete")) {
            match.route = Route::target_delete;
            match.method = Method::post;
            return match;
        }
        if (equals_exact(rest, "program")) {
            match.route = Route::program;
            match.method = Method::get;
            match.action = stxcontrol::Command::program_state;
            return match;
        }
        if (equals_exact(rest, "log")) {
            match.route = Route::target_log;
            match.method = Method::get;
            return match;
        }
        if (equals_exact(rest, "install")) {
            match.action = stxcontrol::Command::program_install;
        } else if (equals_exact(rest, "rollback")) {
            match.action = stxcontrol::Command::program_rollback;
        } else if (equals_exact(rest, "check-update")) {
            match.action = stxcontrol::Command::program_check;
        } else {
            return match;
        }
        match.route = Route::program_action;
        match.method = Method::post;
        return match;
    }
    return match;
}

// The route for path under method, when a route of that path takes it;
// else the path's first route (its method then differs: 405).
[[nodiscard]] RouteMatch route_for(std::span<const char> path, Method method) noexcept {
    const RouteMatch first = route_of(path);
    if (first.route == Route::not_found) {
        return first;
    }
    const Method wanted = method == Method::head ? Method::get : method;
    if (first.method == wanted) {
        return first;
    }
    // The fixed table lists a shared path's GET route before its POST
    // route; the named routes have one method each.
    if (wanted == Method::post) {
        if (first.route == Route::network) {
            RouteMatch second = first;
            second.route = Route::network_write;
            second.method = Method::post;
            return second;
        }
        if (first.route == Route::settings) {
            RouteMatch second = first;
            second.route = Route::settings_write;
            second.method = Method::post;
            return second;
        }
        if (first.route == Route::access) {
            RouteMatch second = first;
            second.route = Route::access_write;
            second.method = Method::post;
            return second;
        }
    }
    return first;
}

[[nodiscard]] bool route_needs_session(Route route) noexcept {
    switch (route) {
    case Route::session:
    case Route::login:
    case Route::setup:
    case Route::not_found:
        return false;
    case Route::logout:
    case Route::password:
    case Route::supervisor:
    case Route::services:
    case Route::service:
    case Route::service_action:
    case Route::slots:
    case Route::boot_log:
    case Route::network:
    case Route::network_write:
    case Route::config:
    case Route::reboot:
    case Route::settings:
    case Route::settings_write:
    case Route::settings_check:
    case Route::targets:
    case Route::target_write:
    case Route::target_delete:
    case Route::program:
    case Route::program_action:
    case Route::target_log:
    case Route::upload:
    case Route::system_update:
    case Route::operation:
    case Route::access:
    case Route::access_write:
        return true;
    default:
        break;
    }
    // Fallback: an unknown route needs a session.
    return true;
}

[[nodiscard]] bool content_type_is_json(std::span<const char> value) noexcept {
    return equals_lower(value, "application/json") || equals_lower(value, "application/json; charset=utf-8") ||
           equals_lower(value, "application/json;charset=utf-8");
}

// Whether origin equals "https://" + host.
[[nodiscard]] bool origin_matches(std::span<const char> origin, std::span<const char> host) noexcept {
    const char* const scheme = "https://";
    const std::size_t scheme_length = c_length(scheme);
    if (origin.size() != scheme_length + host.size() || !starts_with(origin, scheme)) {
        return false;
    }
    // Host names compare without case (RFC 3986 section 3.2.2).
    for (std::size_t index = 0; index < host.size(); index += 1) {
        char left = origin[scheme_length + index];
        char right = host[index];
        left = left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
        right = right >= 'A' && right <= 'Z' ? static_cast<char>(right - 'A' + 'a') : right;
        if (left != right) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t seconds_up(std::uint64_t milliseconds) noexcept {
    const std::uint64_t seconds = (milliseconds + 999u) / 1000u;
    return seconds < 0xffffffffu ? static_cast<std::uint32_t>(seconds) : 0xffffffffu;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared with api_pages.cpp (api_internal.h).

namespace api_detail {

std::uint16_t http_status_of(stxcontrol::Status status) noexcept {
    switch (status) {
    case stxcontrol::Status::ok: return 200;
    case stxcontrol::Status::unknown_service: return 404;
    case stxcontrol::Status::already_running: return 409;
    case stxcontrol::Status::not_running: return 409;
    case stxcontrol::Status::refused_self: return 409;
    case stxcontrol::Status::spawn_failed: return 500;
    case stxcontrol::Status::not_permitted: return 403;
    case stxcontrol::Status::malformed: return 500;
    case stxcontrol::Status::unavailable: return 503;
    case stxcontrol::Status::config_refused: return 422;
    case stxcontrol::Status::unsupported: return 501;
    case stxcontrol::Status::store_failed: return 500;
    case stxcontrol::Status::busy: return 409;
    case stxcontrol::Status::signature_refused: return 422;
    case stxcontrol::Status::source_unavailable: return 502;
    default: break;
    }
    // Fallback: an unknown status is the supervisor being unavailable.
    return 503;
}

void write_slot(JsonWriter& writer, stxcontrol::SlotCode code) noexcept {
    if (code == stxcontrol::SlotCode::none) {
        writer.null();
        return;
    }
    writer.string(stxcontrol::slot_code_name(code));
}

void write_service_record(JsonWriter& writer, const stxcontrol::ServiceRecord& record) noexcept {
    char name[stxcontrol::service_name_bytes_max + 1];
    stxcontrol::service_name_c_string(record.name, name);
    writer.begin_object();
    writer.key("name");
    writer.string(name);
    writer.key("state");
    writer.string(stxcontrol::service_state_code_name(record.state));
    writer.key("restart");
    writer.string(stxcontrol::restart_code_name(record.restart));
    writer.key("control");
    writer.string(stxcontrol::access_code_name(record.control));
    writer.key("restart_pending");
    writer.boolean(record.restart_pending);
    writer.key("stopped_by_request");
    writer.boolean(record.stopped_by_request);
    writer.key("pid");
    writer.integer(record.pid);
    writer.key("restart_count");
    writer.unsigned_integer(record.restart_count);
    writer.key("state_ms");
    writer.unsigned_integer(record.state_ms);
    writer.end_object();
}

const char empty_object[] = "{}";

ApiResponse blank_response(std::span<const char> body) noexcept {
    ApiResponse response{};
    response.status = 500;
    response.kind = ContentKind::json;
    response.body = body;
    response.cacheable = false;
    response.set_cookie = nullptr;
    response.retry_after_s = std::nullopt;
    response.allow = nullptr;
    response.event.kind = ApiEventKind::none;
    response.event.detail = "";
    response.event.service[0] = '\0';
    response.event.status = "";
    return response;
}

std::size_t copy_empty_object(std::span<char> body) noexcept {
    std::size_t size = 0;
    for (std::size_t index = 0; empty_object[index] != '\0' && index < body.size(); index += 1) {
        body[index] = empty_object[index];
        size += 1;
    }
    return size;
}

}  // namespace api_detail

namespace {

using api_detail::empty_object;
using api_detail::http_status_of;
using api_detail::write_service_record;
using api_detail::write_slot;

}  // namespace

// ---------------------------------------------------------------------------
// Api.

Api::Api(ApiBackend& backend) noexcept
    : backend_(backend), sessions_(), gates_(), setup_code_(), document_(), body_{}, body_size_(0), set_cookie_{},
      password_{}, second_password_{}, config_text_{}, check_config_(), factory_config_(), addresses_{}, editor_(),
      user_source_(stxcontrol::ConfigSource::none), user_sequence_(0), user_written_since_start_(false),
      fetch_failure_(), request_payload_{} {}

std::optional<std::size_t> Api::session_of(const ApiRequest& request) noexcept {
    if (!request.cookie.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::span<const char>> token = cookie_value(*request.cookie, session_cookie_name);
    if (!token.has_value()) {
        return std::nullopt;
    }
    return sessions_.find(*token, request.now_ms);
}

bool Api::origin_acceptable(const ApiRequest& request) noexcept {
    return !request.origin.has_value() || origin_matches(*request.origin, request.host);
}

void Api::set_setup_code(const std::optional<SetupCode>& code) noexcept {
    setup_code_ = code;
}

const SessionTable& Api::sessions() const noexcept {
    return sessions_;
}

const GateTable& Api::gates() const noexcept {
    return gates_;
}

void Api::respond_json(ApiResponse& response, std::uint16_t status) noexcept {
    response.status = status;
    response.kind = ContentKind::json;
    response.body = std::span<const char>(body_, body_size_);
}

void Api::respond_error(ApiResponse& response, std::uint16_t status, const char* error) noexcept {
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("error");
    writer.string(error);
    writer.end_object();
    // A one-member object always fits the body storage.
    assert(writer.ok());
    body_size_ = writer.ok() ? writer.size() : 0u;
    respond_json(response, status);
}

void Api::respond_locked(ApiResponse& response, const GateDecision& decision) noexcept {
    const std::uint32_t seconds = seconds_up(decision.retry_after_ms);
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("error");
    writer.string(decision.verdict == GateVerdict::busy ? "busy" : "locked");
    writer.key("retry_after_s");
    writer.unsigned_integer(seconds);
    writer.end_object();
    body_size_ = writer.ok() ? writer.size() : 0u;
    respond_json(response, 429);
    response.retry_after_s = seconds;
    response.event.kind = ApiEventKind::login_locked;
}

void Api::set_session_cookie(ApiResponse& response, std::size_t session) noexcept {
    const char* const head = "stxsession=";
    const char* const tail = "; Path=/; Secure; HttpOnly; SameSite=Strict";
    std::size_t size = 0;
    const std::span<char> out(set_cookie_, sizeof(set_cookie_));
    append(out, size, head);
    char token[token_text_bytes + 1];
    for (std::size_t index = 0; index < token_text_bytes; index += 1) {
        token[index] = sessions_.session(session).token.bytes[index];
    }
    token[token_text_bytes] = '\0';
    append(out, size, token);
    append(out, size, tail);
    response.set_cookie = set_cookie_;
}

void Api::clear_session_cookie(ApiResponse& response) noexcept {
    std::size_t size = 0;
    append(std::span<char>(set_cookie_, sizeof(set_cookie_)), size,
           "stxsession=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict");
    response.set_cookie = set_cookie_;
}

void Api::write_csrf_body(std::size_t session) noexcept {
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("logged_in");
    writer.boolean(true);
    writer.key("setup_required");
    writer.boolean(false);
    writer.key("csrf");
    writer.string(std::span<const char>(sessions_.session(session).csrf.bytes, token_text_bytes));
    writer.key("idle_timeout_s");
    writer.unsigned_integer(session_idle_ms / 1000u);
    writer.end_object();
    body_size_ = writer.ok() ? writer.size() : 0u;
}

std::optional<std::span<const char>> Api::body_string(const char* key, std::span<char> destination) const noexcept {
    if (document_.token_count() == 0u) {
        return std::nullopt;
    }
    const std::optional<std::size_t> value = document_.member(0, key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::size_t> size = document_.decode_string(*value, destination);
    if (!size.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::span<const char>> text = std::span<const char>(destination.data(), *size);
    return text;
}

ApiResponse Api::handle(const ApiRequest& request) noexcept {
    ApiResponse response = api_detail::blank_response(std::span<const char>(body_, 0));
    body_size_ = 0;
    if (starts_with(request.path, "/api/") || equals_exact(request.path, "/api")) {
        route_api(request, response);
    } else {
        route_static(request, response);
    }
    return response;
}

void Api::route_static(const ApiRequest& request, ApiResponse& response) noexcept {
    const std::optional<std::size_t> index = asset_index(request.path);
    if (!index.has_value()) {
        respond_error(response, 404, "not_found");
        return;
    }
    if (request.method == Method::post) {
        respond_error(response, 405, "method_not_allowed");
        response.allow = "GET, HEAD";
        return;
    }
    const std::optional<std::span<const char>> bytes = backend_.asset(*index);
    if (!bytes.has_value()) {
        respond_error(response, 503, "asset_unavailable");
        return;
    }
    response.status = 200;
    response.kind = asset_names[*index].kind;
    response.body = *bytes;
    response.cacheable = true;
}

void Api::route_api(const ApiRequest& request, ApiResponse& response) noexcept {
    const RouteMatch match = route_for(request.path, request.method);
    if (match.route == Route::not_found) {
        respond_error(response, 404, "not_found");
        return;
    }
    const bool method_fits = request.method == match.method ||
                             (match.method == Method::get && request.method == Method::head);
    if (!method_fits) {
        respond_error(response, 405, "method_not_allowed");
        response.allow = match.method == Method::get ? "GET, HEAD" : "POST";
        return;
    }
    // An upload's body is streamed, never handed here (begin_upload); a
    // request that reaches here on an upload route is refused.
    if (match.route == Route::upload) {
        respond_error(response, 400, "upload_not_streamed");
        return;
    }

    const std::optional<std::size_t> session = session_of(request);

    if (request.method == Method::post) {
        if (!request.content_type.has_value() || !content_type_is_json(*request.content_type)) {
            respond_error(response, 415, "unsupported_media_type");
            response.event.kind = ApiEventKind::refused;
            response.event.detail = "content-type";
            return;
        }
        if (!origin_acceptable(request)) {
            respond_error(response, 403, "origin_refused");
            response.event.kind = ApiEventKind::refused;
            response.event.detail = "origin";
            return;
        }
        const std::span<const char> body =
            request.body.empty() ? std::span<const char>(empty_object, sizeof(empty_object) - 1u) : request.body;
        const JsonOutcome parsed = parse_json(body, document_);
        const JsonRefused* const refused = parsed.as_refused();
        if (refused != nullptr) {
            JsonWriter writer(std::span<char>(body_, sizeof(body_)));
            writer.begin_object();
            writer.key("error");
            writer.string("body_malformed");
            writer.key("detail");
            writer.string(json_error_name((*refused).error));
            writer.key("offset");
            writer.unsigned_integer((*refused).offset);
            writer.end_object();
            body_size_ = writer.ok() ? writer.size() : 0u;
            respond_json(response, 400);
            return;
        }
        if (document_.token(0).kind != JsonKind::object) {
            respond_error(response, 400, "body_not_object");
            return;
        }
    }

    if (route_needs_session(match.route)) {
        if (!session.has_value()) {
            respond_error(response, 401, "not_logged_in");
            return;
        }
        if (request.method == Method::post &&
            (!request.csrf_token.has_value() || !sessions_.csrf_matches(*session, *request.csrf_token))) {
            respond_error(response, 403, "csrf_failed");
            response.event.kind = ApiEventKind::refused;
            response.event.detail = "csrf";
            return;
        }
    }

    switch (match.route) {
    case Route::session:
        handle_session(session, response);
        return;
    case Route::login:
        handle_login(request, response);
        return;
    case Route::setup:
        handle_setup(request, response);
        return;
    case Route::logout:
        handle_logout(*session, response);
        return;
    case Route::password:
        handle_password(request, *session, response);
        return;
    case Route::supervisor:
        handle_supervisor_query(stxcontrol::Command::supervisor_state, response);
        return;
    case Route::services:
        handle_supervisor_query(stxcontrol::Command::list_services, response);
        return;
    case Route::service:
        handle_service(stxcontrol::Command::service_state, match.name, response);
        return;
    case Route::service_action:
        handle_service(match.action, match.name, response);
        return;
    case Route::slots:
        handle_supervisor_query(stxcontrol::Command::slot_status, response);
        return;
    case Route::boot_log:
        handle_supervisor_query(stxcontrol::Command::boot_log_tail, response);
        return;
    case Route::network:
        handle_network(response);
        return;
    case Route::network_write:
        handle_network_write(response);
        return;
    case Route::config:
        handle_config(response);
        return;
    case Route::reboot:
        handle_reboot(response);
        return;
    case Route::settings:
        handle_settings_read(response);
        return;
    case Route::settings_write:
        handle_settings_write(response);
        return;
    case Route::settings_check:
        handle_settings_check(response);
        return;
    case Route::targets:
        handle_targets(response);
        return;
    case Route::target_write:
        handle_target_write(match.name, response);
        return;
    case Route::target_delete:
        handle_target_delete(match.name, response);
        return;
    case Route::program:
    case Route::program_action:
        handle_program(match.action, match.name, response);
        return;
    case Route::target_log:
        handle_target_log(match.name, response);
        return;
    case Route::system_update:
        handle_system_update(response);
        return;
    case Route::operation:
        handle_operation(response);
        return;
    case Route::access:
        handle_access(response);
        return;
    case Route::access_write:
        handle_access_write(response);
        return;
    case Route::upload:
    case Route::not_found:
        break;
    default:
        break;
    }
    // Fallback: answered as not found.
    respond_error(response, 404, "not_found");
}

void Api::handle_session(std::optional<std::size_t> session, ApiResponse& response) noexcept {
    if (session.has_value()) {
        write_csrf_body(*session);
        respond_json(response, 200);
        return;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("logged_in");
    writer.boolean(false);
    writer.key("setup_required");
    writer.boolean(!backend_.password_is_set());
    writer.end_object();
    body_size_ = writer.ok() ? writer.size() : 0u;
    respond_json(response, 200);
}

void Api::handle_login(const ApiRequest& request, ApiResponse& response) noexcept {
    if (!backend_.password_is_set()) {
        respond_error(response, 409, "setup_required");
        return;
    }
    const std::optional<std::size_t> member = document_.member(0, "password");
    if (!member.has_value() || document_.token(*member).kind != JsonKind::string) {
        respond_error(response, 400, "password_missing");
        return;
    }
    const GateDecision decision = gates_.check(request.client, request.now_ms);
    if (decision.verdict != GateVerdict::allowed) {
        respond_locked(response, decision);
        return;
    }
    // A password longer than any accepted one cannot match; it counts as a
    // wrong password, so its length tells nothing.
    const std::optional<std::span<const char>> password =
        body_string("password", std::span<char>(password_, password_bytes_max + 1u));
    PasswordCheck check = PasswordCheck::wrong;
    if (password.has_value() && (*password).size() <= password_bytes_max) {
        check = backend_.verify_password(*password);
    }
    if (check == PasswordCheck::unavailable) {
        respond_error(response, 503, "password_unavailable");
        return;
    }
    if (check != PasswordCheck::matches) {
        const GateDecision after = gates_.record_failure(request.client, request.now_ms);
        if (after.verdict != GateVerdict::allowed) {
            respond_locked(response, after);
        } else {
            respond_error(response, 401, "login_failed");
            response.event.kind = ApiEventKind::login_failed;
        }
        return;
    }
    gates_.record_success(request.client);
    const std::optional<std::size_t> session = sessions_.create(request.now_ms, backend_.random());
    if (!session.has_value()) {
        respond_error(response, 500, "random_unavailable");
        return;
    }
    write_csrf_body(*session);
    respond_json(response, 200);
    set_session_cookie(response, *session);
    response.event.kind = ApiEventKind::login_ok;
}

void Api::handle_setup(const ApiRequest& request, ApiResponse& response) noexcept {
    if (backend_.password_is_set() || !setup_code_.has_value()) {
        respond_error(response, 409, "setup_not_available");
        return;
    }
    const std::optional<std::span<const char>> password =
        body_string("password", std::span<char>(password_, password_bytes_max + 1u));
    if (!password.has_value()) {
        respond_error(response, 400, "password_missing");
        return;
    }
    const PasswordRule rule = check_new_password(*password);
    if (rule != PasswordRule::acceptable) {
        respond_error(response, 400, password_rule_name(rule));
        return;
    }
    const GateDecision decision = gates_.check(request.client, request.now_ms);
    if (decision.verdict != GateVerdict::allowed) {
        respond_locked(response, decision);
        return;
    }
    char typed[64];
    const std::optional<std::span<const char>> code_text = body_string("code", std::span<char>(typed, sizeof(typed)));
    std::optional<SetupCode> code;
    if (code_text.has_value()) {
        code = read_setup_code(*code_text);
    }
    if (!code.has_value() || !setup_codes_equal(*code, *setup_code_)) {
        const GateDecision after = gates_.record_failure(request.client, request.now_ms);
        if (after.verdict != GateVerdict::allowed) {
            respond_locked(response, after);
        } else {
            respond_error(response, 401, "setup_code_wrong");
            response.event.kind = ApiEventKind::setup_failed;
        }
        return;
    }
    if (!backend_.set_password(*password)) {
        respond_error(response, 500, "password_store_failed");
        return;
    }
    gates_.record_success(request.client);
    setup_code_.reset();
    const std::optional<std::size_t> session = sessions_.create(request.now_ms, backend_.random());
    if (!session.has_value()) {
        respond_error(response, 500, "random_unavailable");
        return;
    }
    write_csrf_body(*session);
    respond_json(response, 200);
    set_session_cookie(response, *session);
    response.event.kind = ApiEventKind::setup_done;
}

void Api::handle_logout(std::size_t session, ApiResponse& response) noexcept {
    sessions_.remove(session);
    body_size_ = api_detail::copy_empty_object(std::span<char>(body_, sizeof(body_)));
    respond_json(response, 200);
    clear_session_cookie(response);
    response.event.kind = ApiEventKind::logout;
}

void Api::handle_password(const ApiRequest& request, std::size_t session, ApiResponse& response) noexcept {
    const std::optional<std::span<const char>> fresh =
        body_string("new", std::span<char>(second_password_, password_bytes_max + 1u));
    const std::optional<std::size_t> current_member = document_.member(0, "current");
    if (!fresh.has_value() || !current_member.has_value() ||
        document_.token(*current_member).kind != JsonKind::string) {
        respond_error(response, 400, "password_missing");
        return;
    }
    const PasswordRule rule = check_new_password(*fresh);
    if (rule != PasswordRule::acceptable) {
        respond_error(response, 400, password_rule_name(rule));
        return;
    }
    const GateDecision decision = gates_.check(request.client, request.now_ms);
    if (decision.verdict != GateVerdict::allowed) {
        respond_locked(response, decision);
        return;
    }
    const std::optional<std::span<const char>> current =
        body_string("current", std::span<char>(password_, password_bytes_max + 1u));
    PasswordCheck check = PasswordCheck::wrong;
    if (current.has_value() && (*current).size() <= password_bytes_max) {
        check = backend_.verify_password(*current);
    }
    if (check == PasswordCheck::unavailable) {
        respond_error(response, 503, "password_unavailable");
        return;
    }
    if (check != PasswordCheck::matches) {
        const GateDecision after = gates_.record_failure(request.client, request.now_ms);
        if (after.verdict != GateVerdict::allowed) {
            respond_locked(response, after);
        } else {
            respond_error(response, 401, "current_password_wrong");
            response.event.kind = ApiEventKind::login_failed;
        }
        return;
    }
    gates_.record_success(request.client);
    if (!backend_.set_password(*fresh)) {
        respond_error(response, 500, "password_store_failed");
        return;
    }
    sessions_.remove_all_except(session);
    body_size_ = api_detail::copy_empty_object(std::span<char>(body_, sizeof(body_)));
    respond_json(response, 200);
    response.event.kind = ApiEventKind::password_changed;
}

void Api::handle_supervisor_query(stxcontrol::Command command, ApiResponse& response) noexcept {
    const stxcontrol::Request request = stxcontrol::request_without_name(command, 0);
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    if (!answer.has_value() || (*answer).command != command) {
        respond_error(response, 503, "supervisor_unavailable");
        return;
    }
    if ((*answer).status != stxcontrol::Status::ok) {
        respond_error(response, http_status_of((*answer).status), stxcontrol::status_name((*answer).status));
        return;
    }
    const std::span<const std::uint8_t> payload = (*answer).payload;
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    bool decoded = false;
    switch (command) {
    case stxcontrol::Command::supervisor_state: {
        const std::optional<stxcontrol::SupervisorStateRecord> record = stxcontrol::decode_supervisor_state(payload);
        if (!record.has_value()) {
            break;
        }
        decoded = true;
        writer.begin_object();
        writer.key("uptime_ms");
        writer.unsigned_integer((*record).uptime_ms);
        writer.key("restart_count");
        if ((*record).restart_count.has_value()) {
            writer.unsigned_integer(*(*record).restart_count);
        } else {
            writer.null();
        }
        writer.key("shim_registered");
        writer.boolean((*record).shim_registered);
        writer.key("init_done");
        writer.boolean((*record).init_done);
        writer.key("failsafe_taken");
        writer.boolean((*record).failsafe_taken);
        writer.key("boot_marked_good");
        writer.boolean((*record).boot_marked_good);
        writer.key("service_count");
        writer.unsigned_integer((*record).service_count);
        writer.end_object();
        break;
    }
    case stxcontrol::Command::list_services: {
        const std::optional<std::uint8_t> count = stxcontrol::decode_service_list_count(payload);
        if (!count.has_value()) {
            break;
        }
        decoded = true;
        writer.begin_object();
        writer.key("services");
        writer.begin_array();
        for (std::size_t index = 0; index < *count; index += 1) {
            const std::optional<stxcontrol::ServiceRecord> record =
                stxcontrol::decode_service_record(stxcontrol::service_list_record(payload, index));
            if (!record.has_value()) {
                decoded = false;
                break;
            }
            write_service_record(writer, *record);
        }
        writer.end_array();
        writer.end_object();
        break;
    }
    case stxcontrol::Command::slot_status: {
        const std::optional<stxcontrol::SlotRecord> record = stxcontrol::decode_slot_record(payload);
        if (!record.has_value()) {
            break;
        }
        decoded = true;
        writer.begin_object();
        writer.key("booted");
        write_slot(writer, (*record).booted);
        writer.key("tries");
        writer.unsigned_integer((*record).tries);
        writer.key("fallback");
        writer.boolean((*record).fallback);
        writer.key("marked_good");
        writer.boolean((*record).marked_good);
        writer.key("last_good");
        write_slot(writer, (*record).last_good);
        writer.end_object();
        break;
    }
    case stxcontrol::Command::boot_log_tail: {
        const std::optional<stxcontrol::BootLogTailView> tail = stxcontrol::decode_boot_log_tail(payload);
        if (!tail.has_value()) {
            break;
        }
        decoded = true;
        writer.begin_object();
        writer.key("present");
        writer.boolean((*tail).present);
        writer.key("front_cut");
        writer.boolean((*tail).front_cut);
        writer.key("text");
        // The writer replaces bytes that are not UTF-8, so any log text
        // gives valid JSON.
        writer.string(std::span<const char>(reinterpret_cast<const char*>((*tail).text.data()), (*tail).text.size()));
        writer.end_object();
        break;
    }
    case stxcontrol::Command::service_state:
    case stxcontrol::Command::start:
    case stxcontrol::Command::stop:
    case stxcontrol::Command::restart:
    default:
        // Fallback: these go through handle_service.
        break;
    }
    if (!decoded || !writer.ok()) {
        respond_error(response, 503, "supervisor_unavailable");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

void Api::handle_service(stxcontrol::Command command, const stxcontrol::ServiceName& name,
                         ApiResponse& response) noexcept {
    const stxcontrol::Request request = stxcontrol::request_for_service(command, 0, name);
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    if (stxcontrol::command_operates(command)) {
        response.event.kind = ApiEventKind::operated;
        response.event.detail = stxcontrol::command_name(command);
        stxcontrol::service_name_c_string(name, response.event.service);
        response.event.status = answer.has_value() ? stxcontrol::status_name((*answer).status) : "unavailable";
    }
    if (!answer.has_value() || (*answer).command != command) {
        respond_error(response, 503, "supervisor_unavailable");
        return;
    }
    const stxcontrol::Status status = (*answer).status;
    std::optional<stxcontrol::ServiceRecord> record;
    if (!(*answer).payload.empty()) {
        record = stxcontrol::decode_service_record((*answer).payload);
        if (!record.has_value() || !stxcontrol::service_names_equal((*record).name, name)) {
            respond_error(response, 503, "supervisor_unavailable");
            return;
        }
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    if (status == stxcontrol::Status::ok) {
        writer.key("result");
        writer.string("ok");
    } else {
        writer.key("error");
        writer.string(stxcontrol::status_name(status));
    }
    if (record.has_value()) {
        writer.key("service");
        write_service_record(writer, *record);
    }
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, http_status_of(status));
}

void Api::handle_config(ApiResponse& response) noexcept {
    const ConfigOutcome outcome =
        backend_.read_config(std::span<char>(config_text_, stxconfig::config_size_max));
    const ConfigRead* const read = outcome.as_read();
    if (read == nullptr) {
        respond_error(response, outcome.is_absent() ? 404 : 503,
                      outcome.is_absent() ? "config_absent" : "config_unreadable");
        return;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("path");
    writer.string(backend_.config_path());
    writer.key("text");
    writer.string(std::span<const char>(config_text_, (*read).size <= stxconfig::config_size_max
                                                          ? (*read).size
                                                          : stxconfig::config_size_max));
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

void Api::handle_reboot(ApiResponse& response) noexcept {
    const stxcontrol::Request request = stxcontrol::request_without_name(stxcontrol::Command::reboot, 0);
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    response.event.kind = ApiEventKind::operated;
    response.event.detail = "reboot";
    response.event.status = answer.has_value() ? stxcontrol::status_name((*answer).status) : "unavailable";
    if (!supervisor_ok(answer, stxcontrol::Command::reboot, response)) {
        return;
    }
    body_size_ = api_detail::copy_empty_object(std::span<char>(body_, sizeof(body_)));
    respond_json(response, 200);
}

}  // namespace stxweb
