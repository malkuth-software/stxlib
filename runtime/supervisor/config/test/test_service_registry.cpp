// test_service_registry.cpp: host tests of the services registry, with a
// fake in place of the system: spawn hands out pids 100, 101, ... (or fails
// when told to), signals and events are recorded.
//
// Besides examples, a bounded exhaustive run drives one service of each
// restart policy through every sequence of up to seven events over six
// event kinds, checking the registry's invariants after every event.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../boot_config.h"
#include "../service_registry.h"
#include "test_support.h"

namespace {

using stxconfig::BootConfig;
using stxconfig::ServiceEvent;
using stxconfig::ServiceEventKind;
using stxconfig::ServiceRegistry;
using stxconfig::ServiceSignal;
using stxconfig::SpawnOutcome;
using stxconfig::StartResult;
using stxtest::check;
using stxtest::same_text;
using stxtest::text_of;

constexpr std::size_t events_max = 64;

class FakeServiceActions final : public stxconfig::ServiceActions {
public:
    FakeServiceActions() noexcept
        : next_pid_(100), fail_spawns_(false), spawns_(0), last_pid_(0), last_directory_{}, last_name_{},
          last_program_{}, last_folder_{}, last_target_(std::nullopt), last_environment_first_{},
          signals_(0), last_signal_pid_(0), last_signal_(ServiceSignal::terminate), events_{}, event_count_(0) {}

    void set_fail_spawns(bool fail) noexcept {
        fail_spawns_ = fail;
    }
    [[nodiscard]] bool fail_spawns() const noexcept {
        return fail_spawns_;
    }

    [[nodiscard]] SpawnOutcome spawn(const stxconfig::SpawnRequest& request) noexcept override {
        copy(last_name_, request.name);
        copy(last_program_, request.arguments[0]);
        copy(last_directory_, request.directory);
        copy(last_environment_first_, request.environment[0] != nullptr ? request.environment[0] : "");
        last_index_ = request.service_index;
        last_target_ = request.target;
        if (request.target.has_value()) {
            copy(last_folder_, (*request.target).folder);
            copy(last_package_, (*request.target).program);
        } else {
            last_folder_[0] = '\0';
            last_package_[0] = '\0';
        }
        if (fail_spawns_) {
            return SpawnOutcome::spawn_failed(12);
        }
        spawns_ += 1;
        last_pid_ = next_pid_;
        next_pid_ += 1;
        return SpawnOutcome::spawned(last_pid_);
    }
    void signal(int pid, ServiceSignal which) noexcept override {
        signals_ += 1;
        last_signal_pid_ = pid;
        last_signal_ = which;
    }
    void report(const ServiceEvent& event) noexcept override {
        if (event_count_ < events_max) {
            events_[event_count_] = event;
        }
        event_count_ += 1;
    }

    [[nodiscard]] std::size_t spawns() const noexcept {
        return spawns_;
    }
    [[nodiscard]] int last_pid() const noexcept {
        return last_pid_;
    }
    [[nodiscard]] const char* last_directory() const noexcept {
        return last_directory_;
    }
    [[nodiscard]] const char* last_name() const noexcept {
        return last_name_;
    }
    [[nodiscard]] const char* last_program() const noexcept {
        return last_program_;
    }
    [[nodiscard]] const char* last_folder() const noexcept {
        return last_folder_;
    }
    [[nodiscard]] const char* last_package() const noexcept {
        return last_package_;
    }
    [[nodiscard]] std::size_t last_index() const noexcept {
        return last_index_;
    }
    [[nodiscard]] const std::optional<stxconfig::SpawnTarget>& last_target() const noexcept {
        return last_target_;
    }
    [[nodiscard]] const char* last_environment_first() const noexcept {
        return last_environment_first_;
    }
    [[nodiscard]] std::size_t signals() const noexcept {
        return signals_;
    }
    [[nodiscard]] int last_signal_pid() const noexcept {
        return last_signal_pid_;
    }
    [[nodiscard]] ServiceSignal last_signal() const noexcept {
        return last_signal_;
    }
    [[nodiscard]] std::size_t event_count() const noexcept {
        return event_count_;
    }
    [[nodiscard]] const ServiceEvent& event(std::size_t index) const noexcept {
        return events_[index < events_max ? index : events_max - 1u];
    }
    [[nodiscard]] const ServiceEvent& last_event() const noexcept {
        return event(event_count_ - 1u);
    }

private:
    static void copy(char (&destination)[64], const char* source) noexcept {
        std::snprintf(destination, sizeof(destination), "%s", source);
    }

    int next_pid_;
    bool fail_spawns_;
    std::size_t spawns_;
    int last_pid_;
    char last_directory_[64];
    char last_name_[64];
    char last_program_[64];
    char last_folder_[64];
    char last_package_[64] = {};
    std::size_t last_index_ = 0;
    std::optional<stxconfig::SpawnTarget> last_target_;
    char last_environment_first_[64];
    std::size_t signals_;
    int last_signal_pid_;
    ServiceSignal last_signal_;
    ServiceEvent events_[events_max];
    std::size_t event_count_;
};

BootConfig config;

const char* const configuration =
    "[service-web]\n"
    "command = /bin/web --port 1\n"
    "directory = /srv/web\n"
    "restart = on-failure\n"
    "[service-always]\n"
    "command = /bin/always\n"
    "restart = always\n"
    "[service-once]\n"
    "command = /bin/once\n"
    "restart = never\n";

constexpr int exit_status_0 = 0;
constexpr int exit_status_1 = 1 << 8;
constexpr int killed_by_9 = 9;

[[nodiscard]] bool load(ServiceRegistry& registry) {
    const bool parsed = stxconfig::parse_boot_config(text_of(configuration), config).is_success();
    if (parsed) {
        registry.load(config);
    }
    return parsed;
}

void test_start_and_lookup() {
    const char* const test = "start";
    ServiceRegistry registry;
    FakeServiceActions actions;
    check(load(registry) && registry.service_count() == 3, test, "three services");
    check(registry.start("absent", 0, actions) == StartResult::unknown_service, test, "unknown service");
    const char* const environment[] = {"PATH=/bin", nullptr};
    registry.set_environment(environment);
    check(registry.start("web", 1000, actions) == StartResult::started, test, "started");
    check(registry.state(0).as_running() != nullptr && (*registry.state(0).as_running()).pid == 100, test,
          "running pid 100");
    check(same_text(actions.last_directory(), "/srv/web") && same_text(actions.last_environment_first(), "PATH=/bin"),
          test, "directory and environment passed");
    check(actions.event_count() == 1 && actions.last_event().kind == ServiceEventKind::started &&
              same_text(actions.last_event().name, "web") && actions.last_event().pid == 100,
          test, "started event");
    check(registry.start("web", 1001, actions) == StartResult::already_running && actions.spawns() == 1, test,
          "second start changes nothing");
    check(registry.start("once", 1002, actions) == StartResult::started && same_text(actions.last_directory(), "/"),
          test, "default directory /");
    check(!registry.child_exited(4242, exit_status_0, 1003, actions), test, "unknown pid not the registry's");
}

void test_backoff_sequence() {
    const char* const test = "back-off";
    ServiceRegistry registry;
    FakeServiceActions actions;
    check(load(registry), test, "loaded");
    std::uint64_t now = 0;
    check(registry.start("web", now, actions) == StartResult::started, test, "started");
    const std::uint64_t expected[] = {100, 200, 400, 800, 1600, 3200, 6400, 10000, 10000};
    for (const std::uint64_t delay : expected) {
        now += 5;
        const int pid = actions.last_pid();
        check(registry.child_exited(pid, exit_status_1, now, actions), test, "exit owned");
        const ServiceEvent& exited = actions.last_event();
        check(exited.kind == ServiceEventKind::exited && exited.next_in_ms.has_value() && *exited.next_in_ms == delay,
              test, "next_in_ms doubles to the cap");
        check(registry.next_due_ms().has_value() && *registry.next_due_ms() == now + delay, test, "due time");
        registry.tick(now + delay - 1u, actions);
        check(registry.state(0).as_waiting() != nullptr, test, "not before the due time");
        now += delay;
        registry.tick(now, actions);
        check(registry.state(0).as_running() != nullptr, test, "restarted at the due time");
    }
    check(registry.account(0).restart_count == 9, test, "restart count");
    // A run of 30 s resets the back-off to its first delay.
    now += stxconfig::service_stable_run_ms;
    check(registry.child_exited(actions.last_pid(), exit_status_1, now, actions), test, "stable exit");
    check(actions.last_event().next_in_ms.has_value() && *actions.last_event().next_in_ms == 100, test,
          "stable run resets to 100 ms");
}

void test_policies() {
    const char* const test = "policies";
    ServiceRegistry registry;
    FakeServiceActions actions;
    check(load(registry), test, "loaded");
    check(registry.start("web", 0, actions) == StartResult::started, test, "web");
    check(registry.child_exited(actions.last_pid(), exit_status_0, 10, actions) && registry.state(0).is_stopped() &&
              !actions.last_event().next_in_ms.has_value(),
          test, "on-failure: clean exit stops");
    check(registry.start("web", 20, actions) == StartResult::started, test, "web again");
    check(registry.child_exited(actions.last_pid(), killed_by_9, 30, actions) && registry.state(0).as_waiting() != nullptr,
          test, "on-failure: death by signal restarts");
    check(registry.start("always", 40, actions) == StartResult::started, test, "always");
    check(registry.child_exited(actions.last_pid(), exit_status_0, 50, actions) &&
              registry.state(1).as_waiting() != nullptr,
          test, "always: clean exit restarts");
    check(registry.start("once", 60, actions) == StartResult::started, test, "once");
    check(registry.child_exited(actions.last_pid(), exit_status_1, 70, actions) && registry.state(2).is_stopped(), test,
          "never: failure stops");
}

void test_stop_all() {
    const char* const test = "stop all";
    ServiceRegistry registry;
    FakeServiceActions actions;
    check(load(registry), test, "loaded");
    check(registry.start("web", 0, actions) == StartResult::started, test, "web");
    const int web_pid = actions.last_pid();
    check(registry.start("always", 0, actions) == StartResult::started, test, "always");
    check(registry.child_exited(actions.last_pid(), exit_status_0, 10, actions), test, "always exits, waits");
    registry.stop_all(1000, actions);
    check(actions.signals() == 1 && actions.last_signal_pid() == web_pid &&
              actions.last_signal() == ServiceSignal::terminate,
          test, "SIGTERM to the running one only");
    check(registry.state(0).as_stopping() != nullptr && registry.state(1).is_stopped(), test,
          "running -> stopping, waiting -> stopped");
    check(registry.next_due_ms().has_value() && *registry.next_due_ms() == 1000 + stxconfig::service_stop_grace_ms,
          test, "due at the kill time");
    check(!registry.all_stopped(), test, "not all stopped yet");
    registry.tick(1000 + stxconfig::service_stop_grace_ms, actions);
    check(actions.signals() == 2 && actions.last_signal() == ServiceSignal::kill, test, "SIGKILL after the grace");
    registry.tick(5000, actions);
    check(actions.signals() == 2 && !registry.next_due_ms().has_value(), test, "SIGKILL once");
    check(registry.child_exited(web_pid, killed_by_9, 5001, actions) && registry.all_stopped() &&
              !actions.last_event().next_in_ms.has_value(),
          test, "exit while stopping: stopped, no restart");
}

// stop and restart by request (C2).
void test_stop_and_restart() {
    const char* const test = "stop and restart";
    ServiceRegistry registry;
    FakeServiceActions actions;
    check(load(registry), test, "loaded");
    check(registry.stop("absent", 0, actions) == stxconfig::StopResult::unknown_service &&
              registry.restart("absent", 0, actions) == stxconfig::RestartResult::unknown_service,
          test, "unknown service");
    check(registry.stop("web", 0, actions) == stxconfig::StopResult::not_running && actions.signals() == 0, test,
          "stop of a stopped service changes nothing");
    check(registry.restart("web", 10, actions) == stxconfig::RestartResult::started &&
              registry.state(0).as_running() != nullptr && registry.account(0).restart_count == 0,
          test, "restart of a stopped service starts it, uncounted");
    check(registry.find("web").has_value() && *registry.find("web") == 0 && !registry.find("absent").has_value(), test,
          "find");
    const int first_pid = actions.last_pid();
    check(registry.find_running_pid(first_pid).has_value() && !registry.find_running_pid(4242).has_value(), test,
          "find_running_pid");
    check(registry.restart("web", 20, actions) == stxconfig::RestartResult::restarting && actions.signals() == 1 &&
              actions.last_signal() == ServiceSignal::terminate && actions.last_signal_pid() == first_pid,
          test, "restart of a running service sends SIGTERM");
    const stxconfig::ServiceStopping* const stopping = registry.state(0).as_stopping();
    check(stopping != nullptr && (*stopping).start_after && !registry.find_running_pid(first_pid).has_value(), test,
          "stopping with start_after; no longer running");
    check(registry.child_exited(first_pid, exit_status_0, 30, actions), test, "exit owned");
    check(registry.state(0).as_running() != nullptr && actions.last_pid() == first_pid + 1 &&
              registry.account(0).restart_count == 1 && registry.account(0).quick_exits_in_row == 0,
          test, "started again after the exit, counted, fresh account");
    check(actions.event_count() >= 2 && actions.event(actions.event_count() - 2u).kind == ServiceEventKind::exited &&
              actions.last_event().kind == ServiceEventKind::started,
          test, "exited then started");
    const int second_pid = actions.last_pid();
    check(registry.restart("web", 40, actions) == stxconfig::RestartResult::restarting, test, "restart again");
    check(registry.stop("web", 41, actions) == stxconfig::StopResult::stopping && actions.signals() == 2, test,
          "stop while stopping: no second signal");
    check(registry.state(0).as_stopping() != nullptr && !(*registry.state(0).as_stopping()).start_after, test,
          "stop cancels the pending restart");
    check(registry.child_exited(second_pid, exit_status_0, 50, actions) && registry.state(0).is_stopped() &&
              actions.spawns() == 2,
          test, "exit after stop: stopped, no spawn");
    // A failing service waiting for its back-off: stop cancels the restart.
    check(registry.start("always", 60, actions) == StartResult::started, test, "always started");
    check(registry.child_exited(actions.last_pid(), exit_status_1, 61, actions) &&
              registry.state(1).as_waiting() != nullptr,
          test, "always waits");
    check(registry.stop("always", 62, actions) == stxconfig::StopResult::stopped && registry.state(1).is_stopped(),
          test, "stop of a waiting service: stopped");
    registry.tick(100000, actions);
    check(registry.state(1).is_stopped(), test, "and stays stopped");
    // Waiting, then restart: started at once, the count unchanged.
    check(registry.start("always", 70, actions) == StartResult::started, test, "always again");
    check(registry.child_exited(actions.last_pid(), exit_status_1, 71, actions), test, "waits again");
    const std::uint64_t counted = registry.account(1).restart_count;
    check(registry.restart("always", 72, actions) == stxconfig::RestartResult::started &&
              registry.state(1).as_running() != nullptr && registry.account(1).restart_count == counted,
          test, "restart of a waiting service: started now, not counted twice");
    // stop_all cancels a pending restart too.
    const int always_pid = actions.last_pid();
    check(registry.restart("always", 80, actions) == stxconfig::RestartResult::restarting, test, "restart pending");
    registry.stop_all(81, actions);
    check(registry.child_exited(always_pid, exit_status_0, 82, actions) && registry.state(1).is_stopped(), test,
          "stop_all cancels it");
    // A failed spawn after the exit leaves the service stopped.
    check(registry.start("web", 90, actions) == StartResult::started, test, "web again");
    const int third_pid = actions.last_pid();
    check(registry.restart("web", 91, actions) == stxconfig::RestartResult::restarting, test, "restart pending");
    actions.set_fail_spawns(true);
    check(registry.child_exited(third_pid, exit_status_0, 92, actions) && registry.state(0).is_stopped() &&
              actions.last_event().kind == ServiceEventKind::spawn_failed,
          test, "failed start after the exit: stopped");
    check(registry.restart("web", 93, actions) == stxconfig::RestartResult::spawn_failed &&
              registry.state(0).is_stopped(),
          test, "restart of a stopped service that cannot spawn");
}

void test_spawn_failures() {
    const char* const test = "spawn failure";
    ServiceRegistry registry;
    FakeServiceActions actions;
    check(load(registry), test, "loaded");
    actions.set_fail_spawns(true);
    check(registry.start("web", 0, actions) == StartResult::spawn_failed && registry.state(0).is_stopped(), test,
          "first start fails: stopped");
    check(actions.last_event().kind == ServiceEventKind::spawn_failed && actions.last_event().error_number == 12 &&
              !actions.last_event().next_in_ms.has_value(),
          test, "reported, no retry");
    actions.set_fail_spawns(false);
    check(registry.start("web", 10, actions) == StartResult::started, test, "started");
    check(registry.child_exited(actions.last_pid(), exit_status_1, 20, actions), test, "fails, waits 100");
    actions.set_fail_spawns(true);
    registry.tick(120, actions);
    check(registry.state(0).as_waiting() != nullptr && actions.last_event().kind == ServiceEventKind::spawn_failed &&
              actions.last_event().next_in_ms.has_value() && *actions.last_event().next_in_ms == 200,
          test, "failed restart waits with the next back-off");
}

// ---------------------------------------------------------------------------
// Bounded exhaustive run.

enum class Event : std::uint8_t {
    start,
    exit_clean,
    exit_failed,
    tick_later,
    stop_all,
    toggle_spawn_failure,
    stop,
    restart,
};
constexpr std::size_t event_kinds = 8;

[[nodiscard]] bool advance(std::size_t* digits, std::size_t length, std::size_t base) {
    for (std::size_t position = 0; position < length; position += 1) {
        digits[position] += 1;
        if (digits[position] < base) {
            return true;
        }
        digits[position] = 0;
    }
    return false;
}

// Drives service index `service` (name `name`) through one sequence.
// Invariants after each event:
// - a running or stopping service's pid is the last pid spawned for it;
// - after stop_all or stop, no spawn happens until the next start or
//   restart;
// - an own exit while stopping with start_after, spawns succeeding, leaves
//   the service running;
// - next_due_ms is present exactly when the service waits or is stopping
//   without its SIGKILL sent, and equals that due time;
// - all_stopped holds exactly when the service is stopped;
// - a signal sent by this event went to the service's own pid, which is
//   then stopping.
void run_sequence(const char* name, std::size_t service, const std::size_t* digits, std::size_t length,
                  ServiceRegistry& registry) {
    FakeServiceActions actions;
    registry.load(config);
    std::uint64_t now = 0;
    bool stopped_by_request = false;
    std::size_t spawns_at_stop = 0;
    int own_pid = 0;
    for (std::size_t position = 0; position < length; position += 1) {
        const Event event = static_cast<Event>(digits[position]);
        const std::size_t signals_before = actions.signals();
        const stxconfig::ServiceStopping* const stopping_before = registry.state(service).as_stopping();
        const bool restart_due = stopping_before != nullptr && (*stopping_before).start_after;
        now += 1;
        switch (event) {
        case Event::start: {
            const StartResult result = registry.start(name, now, actions);
            (void)result;
            stopped_by_request = false;
            break;
        }
        case Event::exit_clean:
        case Event::exit_failed: {
            const int status = event == Event::exit_clean ? exit_status_0 : exit_status_1;
            const stxconfig::ServiceRunning* const running = registry.state(service).as_running();
            const stxconfig::ServiceStopping* const stopping = registry.state(service).as_stopping();
            if (running != nullptr || stopping != nullptr) {
                const int pid = running != nullptr ? (*running).pid : (*stopping).pid;
                check(registry.child_exited(pid, status, now, actions), "exhaustive registry", "own exit accepted");
            } else {
                check(!registry.child_exited(9999, status, now, actions), "exhaustive registry", "stray exit refused");
            }
            break;
        }
        case Event::tick_later:
            now += stxconfig::service_backoff_max_ms;
            registry.tick(now, actions);
            break;
        case Event::stop_all:
            registry.stop_all(now, actions);
            stopped_by_request = true;
            spawns_at_stop = actions.spawns();
            break;
        case Event::toggle_spawn_failure:
            actions.set_fail_spawns(actions.spawns() % 2u == 0u);
            break;
        case Event::stop: {
            const stxconfig::StopResult result = registry.stop(name, now, actions);
            (void)result;
            stopped_by_request = true;
            spawns_at_stop = actions.spawns();
            break;
        }
        case Event::restart: {
            const stxconfig::RestartResult result = registry.restart(name, now, actions);
            (void)result;
            stopped_by_request = false;
            break;
        }
        default:
            break;
        }
        if ((event == Event::exit_clean || event == Event::exit_failed) && restart_due && !actions.fail_spawns()) {
            check(registry.state(service).as_running() != nullptr, "exhaustive registry",
                  "restart by request runs after the exit");
        }
        const stxconfig::ServiceState& state = registry.state(service);
        if (state.as_running() != nullptr || state.as_stopping() != nullptr) {
            const int pid = state.as_running() != nullptr ? (*state.as_running()).pid : (*state.as_stopping()).pid;
            own_pid = actions.last_pid();
            check(pid == own_pid, "exhaustive registry", "pid is the last spawned");
        }
        if (stopped_by_request) {
            check(actions.spawns() == spawns_at_stop, "exhaustive registry", "no spawn after stop_all");
        }
        const std::optional<std::uint64_t> due = registry.next_due_ms();
        const stxconfig::ServiceWaiting* const waiting = state.as_waiting();
        const stxconfig::ServiceStopping* const stopping = state.as_stopping();
        if (waiting != nullptr) {
            check(due.has_value() && *due == (*waiting).start_at_ms, "exhaustive registry", "due when waiting");
        } else if (stopping != nullptr && !(*stopping).kill_sent) {
            check(due.has_value() && *due == (*stopping).kill_at_ms, "exhaustive registry", "due when stopping");
        } else {
            check(!due.has_value(), "exhaustive registry", "nothing due otherwise");
        }
        check(registry.all_stopped() == state.is_stopped(), "exhaustive registry", "all_stopped matches");
        if (actions.signals() != signals_before) {
            check(actions.signals() == signals_before + 1u && stopping != nullptr &&
                      (*stopping).pid == actions.last_signal_pid() && (*stopping).pid == own_pid,
                  "exhaustive registry", "one signal, to the own pid, now stopping");
        }
    }
}

void test_exhaustive() {
    const char* const names[] = {"web", "always", "once"};
    constexpr std::size_t length_max = 7;
    std::size_t sequences = 0;
    const std::size_t failures_before = stxtest::tally.failures;
    ServiceRegistry registry;
    for (std::size_t service = 0; service < 3u; service += 1) {
        for (std::size_t length = 0; length <= length_max; length += 1) {
            std::size_t digits[length_max] = {};
            for (;;) {
                run_sequence(names[service], service, digits, length, registry);
                sequences += 1;
                if (stxtest::tally.failures != failures_before) {
                    std::printf("  first failing sequence: service %s, length %zu\n", names[service], length);
                    return;
                }
                if (!advance(digits, length, event_kinds)) {
                    break;
                }
            }
        }
    }
    std::printf("exhaustive registry: %zu sequences (0-%zu events of %zu kinds, 3 policies)\n", sequences, length_max,
                event_kinds);
}

// A target's start carries its identity, package and folder, its index, and
// the folder as its directory; a service's carries none. A target that
// exits with the launcher's refusal status is not restarted, whatever its
// policy; any other failure is.
void test_target_request() {
    const char* const test = "target request";
    const char* const text =
        "[service-plain]\n"
        "command = /bin/plain\n"
        "[target-show]\n"
        "role = foreground\n"
        "id = 7\n"
        "program = show\n"
        "command = bin/show -v\n"
        "folder = /data/targets/show\n"
        "restart = always\n";
    BootConfig target_config;
    const bool parsed = stxconfig::parse_boot_config(text_of(text), target_config).is_success();
    check(parsed, test, "parses");
    if (!parsed) {
        return;
    }
    ServiceRegistry registry;
    registry.load(target_config);
    FakeServiceActions actions;
    check(registry.start("show", 10, actions) == StartResult::started, test, "starts");
    const std::optional<stxconfig::SpawnTarget>& target = actions.last_target();
    check(target.has_value() && (*target).identity == stxconfig::target_identity_base + 7u, test,
          "a target, identity 30007");
    check(actions.last_index() == 1u, test, "its index");
    check(same_text(actions.last_name(), "show") && same_text(actions.last_program(), "bin/show") &&
              same_text(actions.last_directory(), "/data/targets/show") &&
              same_text(actions.last_folder(), "/data/targets/show") && same_text(actions.last_package(), "show"),
          test, "name, relative file, folder as directory, package");
    const int first = actions.last_pid();
    check(registry.child_exited(first, exit_status_1, 20, actions) && registry.state(1).as_waiting() != nullptr, test,
          "an ordinary failure: restarted");
    registry.tick(100000, actions);
    const int second = actions.last_pid();
    check(second != first && registry.state(1).as_running() != nullptr, test, "running again");
    constexpr int refused_status = stxconfig::package_refused_exit_status << 8;
    check(registry.child_exited(second, refused_status, 100001, actions) && registry.state(1).is_stopped() &&
              !actions.last_event().next_in_ms.has_value(),
          test, "exit 125: stopped, no restart");
    check(registry.start("plain", 100002, actions) == StartResult::started, test, "the service starts");
    check(!actions.last_target().has_value() && same_text(actions.last_directory(), "/"), test, "no target, in /");
}

}  // namespace

int main() {
    test_target_request();
    test_start_and_lookup();
    test_backoff_sequence();
    test_policies();
    test_stop_all();
    test_stop_and_restart();
    test_spawn_failures();
    test_exhaustive();
    return stxtest::finish("test_service_registry");
}
