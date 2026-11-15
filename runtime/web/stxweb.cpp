// stxweb.cpp: the appliance's web interface, a service the supervisor
// starts from the factory configuration (C2; design
// dev/notes/planning/stages/C2-web.md, and appliance/web/README.md).
//
//   stxweb [--port N] [--state DIR] [--assets DIR] [--config PATH]
//
//   --port     the HTTPS port (default 443, the one declared listener)
//   --state    the password and TLS identity's turn files (default
//              /user/stx/web)
//   --assets   index.html, app.js and style.css (default /usr/share/stx/web)
//   --config   the factory configuration shown on the settings page
//              (default /etc/stx/appliance.conf)
//
// At start, on the console (the supervisor's standard output):
//   STXWEB fingerprint sha256=AB:CD:...   (every start; "made" when new)
//   STXWEB setup code=XXXXX-XXXXX         (only while no password is set)
//   STXWEB listening port=N
// and one line per security event (login, setup, password, operation,
// refusal) while it runs. It exits when the supervisor's end of the
// control channel closes, or at SIGTERM.

#include <csignal>
#include <cstdint>
#include <cstdlib>

#include <signal.h>
#include <unistd.h>

#include "../common/console_line.h"
#include "../supervisor/config/system_actions.h"
#include "api.h"
#include "auth.h"
#include "control_client.h"
#include "crypto_system.h"
#include "system_backend.h"
#include "tls_server.h"

namespace {

using stxruntime::LineText;

constexpr const char* default_state_directory = "/user/stx/web";
constexpr const char* default_assets_directory = "/usr/share/stx/web";
constexpr std::uint16_t default_port = 443;

volatile std::sig_atomic_t terminate_requested = 0;

extern "C" void note_terminate(int signal_number) noexcept {
    (void)signal_number;
    terminate_requested = 1;
}

// Static storage: the server's connections, the Api's checker and the
// backend's pages together are about 2 MiB, and the service has one of
// each. Construction order is declaration order (one translation unit).
stxweb::StatePaths state_paths{};
stxweb::ControlClient control_client(-1);
stxweb::PasswordStore password_store(state_paths);
stxweb::SystemBackend backend(control_client, password_store);
stxweb::Api api(backend);
stxweb::TlsServer server(api, control_client, STDOUT_FILENO);
stxweb::TlsIdentity identity{};

void say(const char* first, const char* second) noexcept {
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXWEB ");
    stxruntime::append_text(line, first);
    stxruntime::append_text(line, second);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(STDOUT_FILENO, line);
}

[[nodiscard]] bool same_text(const char* left, const char* right) noexcept {
    std::size_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index += 1;
    }
    return left[index] == right[index];
}

// A decimal port 1..65535.
[[nodiscard]] bool parse_port(const char* text, std::uint16_t& port) noexcept {
    std::uint32_t value = 0;
    std::size_t index = 0;
    for (; text[index] != '\0'; index += 1) {
        if (text[index] < '0' || text[index] > '9' || index >= 5u) {
            return false;
        }
        value = value * 10u + static_cast<std::uint32_t>(text[index] - '0');
    }
    if (index == 0u || value == 0u || value > 65535u) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

struct Options {
    std::uint16_t port;
    const char* state;
    const char* assets;
    const char* config;
};

[[nodiscard]] bool parse_options(int count, char** arguments, Options& options) noexcept {
    options.port = default_port;
    options.state = default_state_directory;
    options.assets = default_assets_directory;
    options.config = stxconfig::config_path;
    for (int index = 1; index < count; index += 2) {
        if (index + 1 >= count) {
            return false;
        }
        const char* const name = arguments[index];
        const char* const value = arguments[index + 1];
        if (same_text(name, "--port")) {
            if (!parse_port(value, options.port)) {
                return false;
            }
        } else if (same_text(name, "--state")) {
            options.state = value;
        } else if (same_text(name, "--assets")) {
            options.assets = value;
        } else if (same_text(name, "--config")) {
            options.config = value;
        } else {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool install_signals() noexcept {
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    (void)sigemptyset(&ignore.sa_mask);
    // A peer that closes mid-write: EPIPE, not a signal (OpenSSL writes
    // with write(2); the control channel sends with MSG_NOSIGNAL).
    if (sigaction(SIGPIPE, &ignore, nullptr) != 0) {
        return false;
    }
    struct sigaction stop{};
    stop.sa_handler = note_terminate;
    (void)sigemptyset(&stop.sa_mask);
    // No SA_RESTART: poll returns EINTR and the loop sees the flag.
    stop.sa_flags = 0;
    return sigaction(SIGTERM, &stop, nullptr) == 0 && sigaction(SIGINT, &stop, nullptr) == 0;
}

void show_setup_code() noexcept {
    std::uint8_t random[stxweb::setup_code_characters];
    if (!backend.random().fill(random)) {
        say("setup code unavailable: no random bytes", "");
        api.set_setup_code(std::nullopt);
        return;
    }
    const stxweb::SetupCode code = stxweb::setup_code_of(random);
    char text[stxweb::setup_code_text_bytes + 1u];
    stxweb::format_setup_code(code, std::span<char, stxweb::setup_code_text_bytes>(text, stxweb::setup_code_text_bytes));
    text[stxweb::setup_code_text_bytes] = '\0';
    api.set_setup_code(code);
    say("setup code=", text);
}

}  // namespace

int main(int argument_count, char** argument_vector) {
    Options options{};
    if (!parse_options(argument_count, argument_vector, options)) {
        say("usage: stxweb [--port N] [--state DIR] [--assets DIR] [--config PATH]", "");
        return EXIT_FAILURE;
    }
    if (!install_signals()) {
        say("signals failed", "");
        return EXIT_FAILURE;
    }
    const int channel = stxweb::ControlClient::inherited_descriptor();
    if (channel < 0) {
        // Without the supervisor there is nothing to operate, and nothing
        // would end this process with the supervisor.
        say("no control channel on descriptor 3", "");
        return EXIT_FAILURE;
    }
    control_client.adopt(channel);
    if (!stxweb::make_state_paths(options.state, state_paths)) {
        say("state directory too long: ", options.state);
        return EXIT_FAILURE;
    }
    backend.set_config_path(options.config);
    stxweb::AssetLoad loads[stxweb::asset_count];
    if (!backend.load_assets(options.assets, std::span<stxweb::AssetLoad, stxweb::asset_count>(loads))) {
        say("assets directory too long: ", options.assets);
        return EXIT_FAILURE;
    }
    for (std::size_t index = 0; index < stxweb::asset_count; index += 1) {
        if (loads[index] != stxweb::AssetLoad::loaded) {
            LineText line = stxruntime::empty_line();
            stxruntime::append_text(line, "STXWEB asset ");
            stxruntime::append_text(line, stxweb::asset_names[index].file);
            stxruntime::append_text(line, " ");
            stxruntime::append_text(line, stxweb::asset_load_name(loads[index]));
            stxruntime::append_text(line, "\n");
            stxruntime::write_line(STDOUT_FILENO, line);
        }
    }
    password_store.load();
    if (!stxweb::load_or_make_identity(state_paths, backend.random(), identity)) {
        say("tls identity failed", "");
        return EXIT_FAILURE;
    }
    say(identity.made_now ? "fingerprint made sha256=" : "fingerprint sha256=", identity.fingerprint);
    if (!password_store.is_set()) {
        show_setup_code();
    }
    if (!server.start(identity, options.port)) {
        stxweb::free_identity(identity);
        return EXIT_FAILURE;
    }
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXWEB listening port=");
    stxruntime::append_decimal(line, options.port);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(STDOUT_FILENO, line);
    const stxweb::ServerExit exit = server.run(&terminate_requested);
    say("exit reason=", stxweb::server_exit_name(exit));
    // The server's destructor (static) frees the TLS context, which holds
    // its own references to the identity.
    stxweb::free_identity(identity);
    return exit == stxweb::ServerExit::terminated ? EXIT_SUCCESS : EXIT_FAILURE;
}
