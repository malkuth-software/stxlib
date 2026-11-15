// test_api.cpp: host tests of the web interface's routes and security
// decisions (api.h), with a fake backend whose supervisor is a real
// ControlServer (control/control_server.h) over a real registry with fake
// actions and a fake system (control/test/control_fakes.h). So each request
// crosses the JSON interface, the control protocol's encoding and the
// supervisor's checks, as on the appliance.
//
// - every route and method, without a session and with a session but no
//   CSRF token: no protected route ever answers 2xx (bounded exhaustive
//   over the route table, both security conditions, three methods);
// - the first password with the setup code; login; the lockout's 429 and
//   Retry-After; Content-Type, Origin and CSRF refusals;
// - services read and operated; the user configuration by turns (409 on a
//   stale base, 422 on a refusal, with its line); targets, network and
//   access written through the editor and read back; the program commands
//   answering 501 while the host side does not implement them; uploads
//   begun and finished; reboot; a password change ending the other
//   sessions; logout.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../supervisor/config/boot_config.h"
#include "../../supervisor/config/service_registry.h"
#include "../../supervisor/config/test/test_support.h"
#include "../../supervisor/control/control_protocol.h"
#include "../../supervisor/control/control_server.h"
#include "../../supervisor/control/test/control_fakes.h"
#include "../api.h"
#include "../auth.h"
#include "../json.h"

namespace {

using namespace stxweb;
using stxtest::check;
using stxtest::text_of;

const char* const factory_text =
    "[init]\n"
    "start web\n"
    "[service-web]\n"
    "command = /usr/sbin/stxweb\n"
    "control = operate\n"
    "[service-sleeper]\n"
    "command = /bin/sleep 3600\n"
    "restart = always\n"
    "[service-plain]\n"
    "command = /bin/sleep 3600\n";

constexpr int web_pid = 100;
const char* const valid_key =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl owner@laptop";

stxconfig::BootConfig factory;
stxconfig::ServiceRegistry registry;
stxcontrol::test::FakeActions actions;
stxcontrol::test::FakeSystem fake_system;

class CountingRandom final : public RandomSource {
public:
    CountingRandom() noexcept : next_(1) {}
    [[nodiscard]] bool fill(std::span<std::uint8_t> bytes) noexcept override {
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(next_ * 131u + 7u);
            next_ += 1;
        }
        return true;
    }

private:
    std::uint32_t next_;
};

class TestBackend final : public ApiBackend {
public:
    explicit TestBackend(stxcontrol::ControlServer& server) noexcept
        : server_(server), request_{}, response_{}, password_{}, password_size_(0), password_set_(false), random_() {}

    [[nodiscard]] std::optional<stxcontrol::ResponseView> ask_supervisor(
        const stxcontrol::Request& request) noexcept override {
        const std::optional<std::size_t> size = stxcontrol::encode_request(request, request_);
        if (!size.has_value()) {
            return std::nullopt;
        }
        stxcontrol::ControlFacts facts{};
        facts.uptime_ms = 1000;
        facts.init_done = true;
        const stxcontrol::ControlHandled handled =
            server_.handle(std::span<const std::uint8_t>(request_, *size), web_pid, facts, 2000, response_);
        const stxcontrol::ResponseOutcome outcome =
            stxcontrol::decode_response(std::span<const std::uint8_t>(response_, handled.response_size));
        const stxcontrol::ResponseView* const view = outcome.as_view();
        if (view == nullptr) {
            return std::nullopt;
        }
        return *view;
    }
    [[nodiscard]] bool password_is_set() noexcept override {
        return password_set_;
    }
    [[nodiscard]] PasswordCheck verify_password(std::span<const char> password) noexcept override {
        if (!password_set_) {
            return PasswordCheck::unavailable;
        }
        const bool same = password.size() == password_size_ && std::memcmp(password.data(), password_, password_size_) == 0;
        return same ? PasswordCheck::matches : PasswordCheck::wrong;
    }
    [[nodiscard]] bool set_password(std::span<const char> password) noexcept override {
        if (password.size() > sizeof(password_)) {
            return false;
        }
        std::memcpy(password_, password.data(), password.size());
        password_size_ = password.size();
        password_set_ = true;
        return true;
    }
    [[nodiscard]] std::optional<std::span<const char>> asset(std::size_t index) noexcept override {
        const char* const pages[asset_count] = {"<!doctype html>", "<!doctype html>", "'use strict';", "body{}"};
        if (index >= asset_count) {
            return std::nullopt;
        }
        return text_of(pages[index]);
    }
    [[nodiscard]] std::optional<std::size_t> list_addresses(std::span<NetworkAddress> out) noexcept override {
        if (out.empty()) {
            return std::size_t{0};
        }
        NetworkAddress address{};
        std::memcpy(address.interface_name, "genet0", 7);
        address.family = AddressFamily::ipv4;
        address.bytes[0] = 192;
        address.bytes[1] = 168;
        address.bytes[2] = 1;
        address.bytes[3] = 20;
        address.prefix_length = 24;
        out[0] = address;
        return std::size_t{1};
    }
    [[nodiscard]] ConfigOutcome read_config(std::span<char> out) noexcept override {
        const std::size_t size = std::strlen(factory_text);
        if (size > out.size()) {
            return ConfigOutcome::failed(27);
        }
        std::memcpy(out.data(), factory_text, size);
        return ConfigOutcome::read(size);
    }
    [[nodiscard]] const char* config_path() noexcept override {
        return "/etc/stx/appliance.conf";
    }
    [[nodiscard]] RandomSource& random() noexcept override {
        return random_;
    }

private:
    stxcontrol::ControlServer& server_;
    std::uint8_t request_[stxcontrol::request_frame_bytes_max];
    std::uint8_t response_[stxcontrol::response_frame_bytes_max];
    char password_[password_bytes_max];
    std::size_t password_size_;
    bool password_set_;
    CountingRandom random_;
};

Api* api_pointer = nullptr;
JsonDocument document;

// A client's state: its address, cookie and CSRF token.
struct Client {
    ClientKey key;
    char token[token_text_bytes + 1];
    char csrf[token_text_bytes + 1];
    bool has_session;
};

[[nodiscard]] Client client_at(std::uint8_t last) {
    Client client{};
    const std::uint8_t address[4] = {192, 168, 1, last};
    client.key = client_key_ipv4(std::span<const std::uint8_t, 4>(address));
    return client;
}

struct Answer {
    std::uint16_t status;
    char body[api_body_bytes_max + 1];
    std::size_t size;
    char set_cookie[160];
    std::optional<std::uint32_t> retry_after_s;
};

Answer answer;
char cookie_field[token_text_bytes + 16];
std::uint64_t now_ms = 10000;

void keep(const ApiResponse& response) {
    answer.status = response.status;
    answer.size = response.body.size() <= api_body_bytes_max ? response.body.size() : api_body_bytes_max;
    std::memcpy(answer.body, response.body.data(), answer.size);
    answer.body[answer.size] = '\0';
    answer.set_cookie[0] = '\0';
    if (response.set_cookie != nullptr) {
        std::snprintf(answer.set_cookie, sizeof(answer.set_cookie), "%s", response.set_cookie);
    }
    answer.retry_after_s = response.retry_after_s;
}

[[nodiscard]] ApiRequest request_of(const Client& client, Method method, const char* path, const char* body,
                                    bool with_csrf) {
    ApiRequest request{};
    request.method = method;
    request.path = text_of(path);
    request.host = text_of("appliance");
    if (client.has_session) {
        std::snprintf(cookie_field, sizeof(cookie_field), "stxsession=%s", client.token);
        request.cookie = text_of(cookie_field);
        if (with_csrf) {
            request.csrf_token = text_of(client.csrf);
        }
    }
    if (method == Method::post) {
        request.content_type = text_of("application/json");
    }
    request.body = text_of(body);
    request.client = client.key;
    now_ms += 10;
    request.now_ms = now_ms;
    return request;
}

std::uint16_t send(const Client& client, Method method, const char* path, const char* body = "{}",
                   bool with_csrf = true) {
    keep((*api_pointer).handle(request_of(client, method, path, body, with_csrf)));
    return answer.status;
}

// The body's string member into out; false when absent.
[[nodiscard]] bool body_member(const char* key, char* out, std::size_t capacity) {
    if (!parse_json(std::span<const char>(answer.body, answer.size), document).is_parsed()) {
        return false;
    }
    const std::optional<std::size_t> member = document.member(0, key);
    if (!member.has_value() || document.token(*member).kind != JsonKind::string) {
        return false;
    }
    const std::optional<std::size_t> size = document.decode_string(*member, std::span<char>(out, capacity - 1u));
    if (!size.has_value()) {
        return false;
    }
    out[*size] = '\0';
    return true;
}

[[nodiscard]] bool body_has(const char* text) {
    return std::strstr(answer.body, text) != nullptr;
}

// Takes the session from the last answer's cookie and body.
[[nodiscard]] bool take_session(Client& client) {
    const char* const head = "stxsession=";
    if (std::strncmp(answer.set_cookie, head, std::strlen(head)) != 0) {
        return false;
    }
    std::memcpy(client.token, answer.set_cookie + std::strlen(head), token_text_bytes);
    client.token[token_text_bytes] = '\0';
    client.has_session = body_member("csrf", client.csrf, sizeof(client.csrf));
    return client.has_session;
}

void test_routes_guarded(Client& owner) {
    const char* const paths[] = {
        "/api/supervisor",        "/api/services",           "/api/services/sleeper", "/api/services/sleeper/stop",
        "/api/services/web/start", "/api/slots",              "/api/boot-log",         "/api/config",
        "/api/reboot",            "/api/settings",           "/api/settings/check",   "/api/targets",
        "/api/targets/player",    "/api/targets/player/delete", "/api/targets/player/program",
        "/api/targets/player/install", "/api/targets/player/rollback", "/api/targets/player/check-update",
        "/api/targets/plain/log",
        "/api/system/update",     "/api/operation",          "/api/network",          "/api/access",
        "/api/logout",            "/api/password",           "/api/upload/package",   "/api/upload/system"};
    const Method methods[] = {Method::get, Method::head, Method::post};
    Client stranger = client_at(77);
    bool never_open = true;
    bool never_without_csrf = true;
    for (const char* const path : paths) {
        for (const Method method : methods) {
            const std::uint16_t without_session = send(stranger, method, path);
            never_open = never_open && (without_session < 200u || without_session > 299u);
            if (method == Method::post) {
                const std::uint16_t without_csrf = send(owner, method, path, "{}", false);
                never_without_csrf = never_without_csrf && (without_csrf < 200u || without_csrf > 299u);
            }
        }
    }
    check(never_open, "guarded", "no protected route answers 2xx without a session");
    check(never_without_csrf, "guarded", "no POST answers 2xx without the CSRF token");
    check(fake_system.reboots() == 0, "guarded", "no refused request reached the supervisor's reboot");
}

void test_setup_and_login(Client& owner) {
    check(send(owner, Method::get, "/") == 200u && send(owner, Method::get, "/app.js") == 200u, "login",
          "pages served without a session");
    check(send(owner, Method::get, "/nothing") == 404u, "login", "an unknown page is 404");
    check(send(owner, Method::get, "/api/session") == 200u && body_has("\"setup_required\":true"), "login",
          "setup required at first");
    check(send(owner, Method::post, "/api/login", "{\"password\":\"whatever1\"}") == 409u, "login",
          "login before setup refused");
    std::uint8_t random[setup_code_characters] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const SetupCode code = setup_code_of(random);
    char code_text[setup_code_text_bytes + 1];
    format_setup_code(code, std::span<char, setup_code_text_bytes>(code_text, setup_code_text_bytes));
    code_text[setup_code_text_bytes] = '\0';
    (*api_pointer).set_setup_code(code);
    check(send(owner, Method::post, "/api/setup", "{\"code\":\"00000-00000\",\"password\":\"first-pass\"}") == 401u,
          "login", "wrong setup code refused");
    char body[128];
    std::snprintf(body, sizeof(body), "{\"code\":\"%s\",\"password\":\"short\"}", code_text);
    check(send(owner, Method::post, "/api/setup", body) == 400u, "login", "a short first password refused");
    std::snprintf(body, sizeof(body), "{\"code\":\"%s\",\"password\":\"first-pass\"}", code_text);
    check(send(owner, Method::post, "/api/setup", body) == 200u && take_session(owner), "login",
          "first password set, session given");
    check(std::strstr(answer.set_cookie, "HttpOnly") != nullptr && std::strstr(answer.set_cookie, "Secure") != nullptr &&
              std::strstr(answer.set_cookie, "SameSite=Strict") != nullptr,
          "login", "cookie HttpOnly, Secure, SameSite=Strict");
    check(send(owner, Method::post, "/api/setup", body) != 200u, "login", "setup closed once a password is set");

    Client guesser = client_at(66);
    // Each failure before the lock is a 401; the failure that locks the
    // client is answered as locked already (429, api.cpp handle_login).
    std::uint32_t refused = 0;
    for (std::uint32_t attempt = 0; attempt + 1u < gate_failures_before_lock; attempt += 1) {
        if (send(guesser, Method::post, "/api/login", "{\"password\":\"wrong-pass\"}") == 401u) {
            refused += 1;
        }
    }
    check(refused + 1u == gate_failures_before_lock, "login", "wrong passwords refused");
    check(send(guesser, Method::post, "/api/login", "{\"password\":\"wrong-pass\"}") == 429u &&
              answer.retry_after_s.has_value(),
          "login", "the failure that locks is answered 429");
    check(send(guesser, Method::post, "/api/login", "{\"password\":\"first-pass\"}") == 429u &&
              answer.retry_after_s.has_value() && *answer.retry_after_s > 0u,
          "login", "a locked client gets 429 with Retry-After, even with the right password");
    Client second = client_at(21);
    check(send(second, Method::post, "/api/login", "{\"password\":\"first-pass\"}") == 200u && take_session(second),
          "login", "another client logs in");
}

void test_post_checks(Client& owner) {
    ApiRequest request = request_of(owner, Method::post, "/api/services/sleeper/stop", "{}", true);
    request.content_type = text_of("text/plain");
    keep((*api_pointer).handle(request));
    check(answer.status == 415u, "post_checks", "a POST without JSON content refused (415)");
    request = request_of(owner, Method::post, "/api/services/sleeper/stop", "{}", true);
    request.origin = text_of("https://evil.example");
    keep((*api_pointer).handle(request));
    check(answer.status == 403u, "post_checks", "a foreign Origin refused (403)");
    request = request_of(owner, Method::post, "/api/services/sleeper/stop", "{}", true);
    request.origin = text_of("https://appliance");
    keep((*api_pointer).handle(request));
    check(answer.status == 200u, "post_checks", "the page's own Origin accepted");
}

void test_services(Client& owner) {
    check(send(owner, Method::get, "/api/services") == 200u && body_has("\"sleeper\"") && body_has("\"web\""),
          "services", "listed");
    check(send(owner, Method::post, "/api/services/sleeper/start") == 200u || answer.status == 409u, "services",
          "start answered");
    check(send(owner, Method::post, "/api/services/web/stop") == 409u, "services",
          "the web service's own stop refused");
    check(send(owner, Method::get, "/api/supervisor") == 200u && body_has("\"init_done\":true"), "services",
          "supervisor state");
    check(send(owner, Method::get, "/api/boot-log") == 200u && body_has("second line"), "services", "boot log");
}

void test_settings(Client& owner) {
    check(send(owner, Method::get, "/api/settings") == 200u && body_has("\"sequence\":0"), "settings",
          "empty at first");
    check(send(owner, Method::post, "/api/settings",
               "{\"text\":\"[system]\\nhostname = stx\\n\",\"base_sequence\":0}") == 200u &&
              body_has("\"sequence\":1"),
          "settings", "written as turn 1");
    check(send(owner, Method::post, "/api/settings", "{\"text\":\"[system]\\nhostname = b\\n\",\"base_sequence\":0}") ==
              409u,
          "settings", "a stale base refused (409)");
    check(send(owner, Method::post, "/api/settings",
               "{\"text\":\"[system]\\nhostname = b\\n[service-web]\\ncommand = /bin/sh\\n\",\"base_sequence\":1}") ==
                  422u &&
              body_has("\"line\":3"),
          "settings", "a factory name refused (422) with its line");
    check(send(owner, Method::post, "/api/settings/check", "{\"text\":\"[system\\n\"}") == 200u &&
              body_has("\"ok\":false"),
          "settings", "check reports a parse error");
}

// The target log tail (K17): the supervisor's ring through
// target_log_tail; the fake system keeps output for "plain" only, with its
// front cut (control_fakes.h).
void test_target_log(Client& owner) {
    check(send(owner, Method::get, "/api/targets/plain/log") == 200u && body_has("\"name\":\"plain\"") &&
              body_has("\"present\":true") && body_has("\"front_cut\":true") && body_has("whole one") &&
              !body_has("half a line"),
          "target_log", "whole lines of the kept output, the cut front dropped");
    check(send(owner, Method::get, "/api/targets/sleeper/log") == 200u && body_has("\"present\":false"), "target_log",
          "no output kept: present false");
    check(send(owner, Method::head, "/api/targets/plain/log") == 200u, "target_log", "HEAD answered as GET");
    check(send(owner, Method::get, "/api/targets/absent/log") == 404u, "target_log", "unknown target is 404");
    check(send(owner, Method::post, "/api/targets/plain/log") == 405u, "target_log", "POST is 405");
    Client stranger = client_at(78);
    check(send(stranger, Method::get, "/api/targets/plain/log") == 401u, "target_log", "needs a session");
}

void test_pages(Client& owner) {
    // A user section is a target: K11's target settings (boot_config.h; the
    // command is a path inside the package), and no start setting
    // (user_config.h, rules 5 and 6).
    check(send(owner, Method::post, "/api/targets/player",
               "{\"settings\":{\"role\":\"server\",\"id\":\"1\",\"command\":\"bin/player --loop\","
               "\"folder\":\"/data/targets/player\",\"restart\":\"always\"},\"enabled\":true}") == 200u,
          "pages", "target written");
    check(send(owner, Method::post, "/api/targets/loose",
               "{\"settings\":{\"command\":\"/bin/sleep 3600\"},\"enabled\":true}") == 422u,
          "pages", "a section without target settings refused");
    check(send(owner, Method::post, "/api/targets/loose",
               "{\"settings\":{\"role\":\"server\",\"id\":\"2\",\"command\":\"bin/loose\","
               "\"folder\":\"/data/targets/loose\",\"start\":\"boot\"},\"enabled\":true}") == 422u,
          "pages", "a target with its own start setting refused");
    check(send(owner, Method::get, "/api/targets") == 200u && body_has("\"name\":\"player\"") &&
              body_has("\"enabled\":true") && body_has("\"programs_supported\":false"),
          "pages", "target read back, enabled; programs not supported yet");
    check(send(owner, Method::post, "/api/targets/web",
               "{\"settings\":{\"role\":\"server\",\"id\":\"3\",\"command\":\"bin/web\","
               "\"folder\":\"/data/targets/web\"},\"enabled\":true}") == 422u,
          "pages", "a target named after a factory service refused");
    check(send(owner, Method::post, "/api/targets/player/install", "{\"source\":\"upload\"}") == 501u, "pages",
          "install answers 501 while the host side lacks it");
    check(send(owner, Method::post, "/api/targets/player/install",
               "{\"source\":\"url\",\"url\":\"http://example.org/p.pkg\"}") == 400u,
          "pages", "a plain-HTTP URL refused");
    check(send(owner, Method::post, "/api/network",
               "{\"mode\":\"static\",\"address\":\"10.0.0.2/24\",\"gateway\":\"10.0.0.1\",\"dns\":\"10.0.0.1\","
               "\"hostname\":\"stx-box\"}") == 200u,
          "pages", "network written");
    check(send(owner, Method::get, "/api/network") == 200u && body_has("\"hostname\":\"stx-box\"") &&
              body_has("10.0.0.2/24") && body_has("192.168.1.20"),
          "pages", "network read back with the addresses");
    check(send(owner, Method::post, "/api/network", "{\"mode\":\"static\",\"address\":\"10.0.0.300/24\"}") == 400u,
          "pages", "a bad address refused");
    char body[256];
    std::snprintf(body, sizeof(body), "{\"keys\":[\"%s\"]}", valid_key);
    check(send(owner, Method::post, "/api/access", body) == 200u, "pages", "SSH key written");
    check(send(owner, Method::get, "/api/access") == 200u && body_has("owner@laptop"), "pages", "SSH key read back");
    check(send(owner, Method::post, "/api/access", "{\"keys\":[\"ssh-rsa AAAA\"]}") == 400u, "pages",
          "an RSA key refused");
    check(send(owner, Method::post, "/api/targets/player/delete") == 200u, "pages", "target deleted");
    check(send(owner, Method::get, "/api/targets") == 200u && !body_has("\"name\":\"player\""), "pages",
          "target gone");
    check(send(owner, Method::get, "/api/operation") == 200u && body_has("\"state\":\"idle\""), "pages",
          "no operation");
}

void test_uploads(Client& owner) {
    ApiRequest request = request_of(owner, Method::post, "/api/upload/package", "", true);
    request.content_type = text_of("application/octet-stream");
    const UploadDecision too_large = (*api_pointer).begin_upload(request, upload_package_bytes_max + 1u);
    check(too_large.as_refused() != nullptr && (*too_large.as_refused()).status == 413u, "uploads",
          "a package over the limit refused (413)");
    request = request_of(owner, Method::post, "/api/upload/package", "", false);
    request.content_type = text_of("application/octet-stream");
    const UploadDecision no_csrf = (*api_pointer).begin_upload(request, 1000);
    check(no_csrf.as_refused() != nullptr && (*no_csrf.as_refused()).status == 403u, "uploads",
          "an upload without the CSRF token refused");
    request = request_of(owner, Method::post, "/api/upload/system", "", true);
    request.content_type = text_of("application/octet-stream");
    const UploadDecision accepted = (*api_pointer).begin_upload(request, 1000);
    check(accepted.as_accepted() != nullptr && (*accepted.as_accepted()).kind == UploadKind::system, "uploads",
          "a system image accepted");
    UploadSummary summary{};
    summary.stored = true;
    summary.bytes = 1000;
    std::memset(summary.sha256, 'a', 64);
    summary.sha256[64] = '\0';
    keep((*api_pointer).finish_upload(UploadKind::system, summary));
    check(answer.status == 200u && body_has("\"bytes\":1000"), "uploads", "stored upload answered");
    summary.stored = false;
    summary.error_number = 28;
    keep((*api_pointer).finish_upload(UploadKind::system, summary));
    check(answer.status == 500u, "uploads", "a failed store answered 500");
}

void test_password_and_logout(Client& owner, Client& second) {
    check(send(owner, Method::post, "/api/password", "{\"current\":\"wrong-pass\",\"new\":\"second-pass\"}") == 401u,
          "password", "a wrong current password refused");
    check(send(owner, Method::post, "/api/password", "{\"current\":\"first-pass\",\"new\":\"second-pass\"}") == 200u,
          "password", "password changed");
    check(send(second, Method::get, "/api/supervisor") == 401u, "password", "the other session ended");
    check(send(owner, Method::get, "/api/supervisor") == 200u, "password", "this session kept");
    check(send(owner, Method::post, "/api/reboot") == 200u && fake_system.reboots() == 1, "password",
          "reboot asked");
    check(send(owner, Method::post, "/api/logout") == 200u && std::strstr(answer.set_cookie, "Max-Age=0") != nullptr,
          "password", "logout clears the cookie");
    check(send(owner, Method::get, "/api/supervisor") == 401u, "password", "the session ended");
}

}  // namespace

int main() {
    const stxconfig::ParseOutcome parsed = stxconfig::parse_boot_config(text_of(factory_text), factory);
    check(parsed.as_failure() == nullptr, "setup", "factory configuration parses");
    registry.load(factory);
    const char* const environment[] = {"PATH=/bin", nullptr};
    registry.set_environment(environment);
    check(registry.start("web", 0, actions) == stxconfig::StartResult::started &&
              registry.start("sleeper", 0, actions) == stxconfig::StartResult::started,
          "setup", "web and sleeper running");
    static stxcontrol::ControlServer server(registry, actions, fake_system, factory);
    static TestBackend backend(server);
    static Api api(backend);
    api_pointer = &api;
    Client owner = client_at(10);
    Client second = client_at(21);
    test_setup_and_login(owner);
    second = client_at(21);
    check(send(second, Method::post, "/api/login", "{\"password\":\"first-pass\"}") == 200u && take_session(second),
          "setup", "second client's session");
    test_routes_guarded(owner);
    test_post_checks(owner);
    test_services(owner);
    test_settings(owner);
    test_target_log(owner);
    test_pages(owner);
    test_uploads(owner);
    test_password_and_logout(owner, second);
    return stxtest::finish("test_api");
}
