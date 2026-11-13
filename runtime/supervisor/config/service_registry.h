// service_registry.h: the supervisor's services registry, first form (C1
// step S3).
//
// Services are defined in the boot configuration (boot_config.h, sections
// [service-<name>]) and started by the init section's start command (later
// also by the services console and the web interface). The registry holds
// each service's state and decides what happens when one exits; a
// ServiceActions object does the spawning and signalling. The decisions are
// pure code, tested on the host (test/test_service_registry.cpp); the
// supervisor passes SystemServiceActions (system_actions.h).
//
// Per service, a state machine:
//
//   stopped   --start-->                       running (or stopped, spawn failed)
//   running   --exit, policy restarts-->       waiting (back-off)
//   running   --exit, policy does not-->       stopped
//   waiting   --tick at or after the due time-> running (or stopped, spawn failed)
//   running   --stop_all-->                    stopping (SIGTERM sent)
//   waiting   --stop_all-->                    stopped
//   stopping  --exit-->                        stopped
//   stopping  --tick past the deadline-->      stopping (SIGKILL sent once)
//
// Any other pair: start on a running, waiting or stopping service is
// accepted as "already running" and changes nothing; an exit for a pid no
// service owns is reported back as not the registry's; stop_all on a
// stopped or stopping service changes nothing.
//
// Restart policy: never; on-failure (anything but a normal exit with status
// 0); always. Back-off as stxinit's: 100 ms doubling to 10 s; the count of
// quick exits in a row is reset by a run of 30 s or more.
//
// Times are milliseconds of a monotonic clock, passed in by the caller. The
// registry allocates nothing and borrows the BootConfig it was loaded from,
// which must outlive it (the supervisor's are both static).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_SERVICE_REGISTRY_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_SERVICE_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "boot_config.h"

namespace stxconfig {

constexpr std::uint64_t service_backoff_first_ms = 100;
constexpr std::uint64_t service_backoff_max_ms = 10000;
constexpr std::uint64_t service_stable_run_ms = 30000;
constexpr std::uint32_t service_quick_exits_counted_max = 16;
// Between SIGTERM and SIGKILL when the registry stops a service.
constexpr std::uint64_t service_stop_grace_ms = 500;

// ---------------------------------------------------------------------------
// What the registry asks of the system.

struct Spawned {
    int pid;
};
struct SpawnFailed {
    int error_number;
};

class SpawnOutcome {
public:
    [[nodiscard]] static SpawnOutcome spawned(int pid) noexcept;
    [[nodiscard]] static SpawnOutcome spawn_failed(int error_number) noexcept;
    [[nodiscard]] const Spawned* as_spawned() const noexcept;
    [[nodiscard]] const SpawnFailed* as_spawn_failed() const noexcept;

private:
    using Alternatives = std::variant<Spawned, SpawnFailed>;
    explicit SpawnOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

enum class ServiceSignal : std::uint8_t {
    terminate,
    kill,
};

// What the registry reports, one event per state change.
enum class ServiceEventKind : std::uint8_t {
    started,       // pid, restart_count
    spawn_failed,  // error_number
    exited,        // pid, wait_status, ran_ms, then next_in_ms if restarting
    stopping,      // pid (SIGTERM sent)
    killed,        // pid (SIGKILL sent)
};

struct ServiceEvent {
    ServiceEventKind kind;
    // The service's name, borrowed from the BootConfig.
    const char* name;
    int pid;
    std::uint64_t restart_count;
    int wait_status;
    int error_number;
    std::uint64_t ran_ms;
    // For exited: the delay before the restart, or nothing (no restart).
    std::optional<std::uint64_t> next_in_ms;
};

class ServiceActions {
public:
    ServiceActions() noexcept = default;
    ServiceActions(const ServiceActions&) = delete;
    ServiceActions& operator=(const ServiceActions&) = delete;

    // Starts arguments[0] (absolute) with that argument vector and
    // environment (both null-terminated) in directory.
    [[nodiscard]] virtual SpawnOutcome spawn(const char* const* arguments, const char* const* environment,
                                             const char* directory) noexcept = 0;
    virtual void signal(int pid, ServiceSignal which) noexcept = 0;
    virtual void report(const ServiceEvent& event) noexcept = 0;

protected:
    ~ServiceActions() = default;
};

// ---------------------------------------------------------------------------
// Service states.

struct ServiceStopped {
};
struct ServiceRunning {
    int pid;
    std::uint64_t started_ms;
};
struct ServiceWaiting {
    std::uint64_t start_at_ms;
};
struct ServiceStopping {
    int pid;
    std::uint64_t started_ms;
    std::uint64_t kill_at_ms;
    bool kill_sent;
};

class ServiceState {
public:
    [[nodiscard]] static ServiceState stopped() noexcept;
    [[nodiscard]] static ServiceState running(int pid, std::uint64_t started_ms) noexcept;
    [[nodiscard]] static ServiceState waiting(std::uint64_t start_at_ms) noexcept;
    [[nodiscard]] static ServiceState stopping(int pid, std::uint64_t started_ms, std::uint64_t kill_at_ms,
                                               bool kill_sent) noexcept;

    [[nodiscard]] bool is_stopped() const noexcept;
    [[nodiscard]] const ServiceRunning* as_running() const noexcept;
    [[nodiscard]] const ServiceWaiting* as_waiting() const noexcept;
    [[nodiscard]] const ServiceStopping* as_stopping() const noexcept;

private:
    using Alternatives = std::variant<ServiceStopped, ServiceRunning, ServiceWaiting, ServiceStopping>;
    explicit ServiceState(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

struct RestartAccount {
    // Starts after the first; saturates.
    std::uint64_t restart_count;
    std::uint32_t quick_exits_in_row;
};

// The delay before the next start after quick_exits_in_row quick exits.
[[nodiscard]] std::uint64_t service_backoff_ms(std::uint32_t quick_exits_in_row) noexcept;

// Whether a waitpid status is a normal exit with status 0 (FreeBSD's
// encoding, spelled out so that this pure code needs no system header).
[[nodiscard]] bool wait_status_is_success(int wait_status) noexcept;

// ---------------------------------------------------------------------------
// The registry.

enum class StartResult : std::uint8_t {
    started,
    already_running,
    unknown_service,
    spawn_failed,
};

class ServiceRegistry {
public:
    ServiceRegistry() noexcept;
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    // One stopped entry per service of config, which the registry then
    // borrows. Any earlier entries are dropped (the caller has stopped
    // them).
    void load(const BootConfig& config) noexcept;

    [[nodiscard]] std::size_t service_count() const noexcept;
    [[nodiscard]] const ServiceState& state(std::size_t index) const noexcept;
    [[nodiscard]] const RestartAccount& account(std::size_t index) const noexcept;

    // The environment for every later spawn (restarts included): a
    // null-terminated vector the caller keeps valid while the registry
    // lives. Until set, services get an empty environment.
    void set_environment(const char* const* environment) noexcept;

    [[nodiscard]] StartResult start(const char* name, std::uint64_t now_ms, ServiceActions& actions) noexcept;

    // A child exited. False when no service owns pid (the caller's own
    // child, or an orphan the supervisor reaped).
    [[nodiscard]] bool child_exited(int pid, int wait_status, std::uint64_t now_ms, ServiceActions& actions) noexcept;

    // Starts waiting services that are due and kills stopping ones past
    // their grace.
    void tick(std::uint64_t now_ms, ServiceActions& actions) noexcept;

    // The earliest time tick has work, or nothing.
    [[nodiscard]] std::optional<std::uint64_t> next_due_ms() const noexcept;

    // Asks every running service to stop (SIGTERM) and cancels waiting
    // restarts. Exits after this never restart.
    void stop_all(std::uint64_t now_ms, ServiceActions& actions) noexcept;

    [[nodiscard]] bool all_stopped() const noexcept;

private:
    struct Entry {
        std::size_t definition_index;
        ServiceState state;
        RestartAccount account;
    };

    void spawn_entry(Entry& item, std::uint64_t now_ms, ServiceActions& actions) noexcept;
    [[nodiscard]] ServiceEvent event_for(const Entry& item, ServiceEventKind kind) const noexcept;

    const BootConfig* config_;
    const char* const* environment_;
    // Entry has no default constructor (ServiceState has none): raw storage,
    // constructed by load.
    alignas(Entry) unsigned char entry_storage_[services_max * sizeof(Entry)];
    std::size_t entry_count_;

    [[nodiscard]] Entry& entry(std::size_t index) noexcept;
    [[nodiscard]] const Entry& entry(std::size_t index) const noexcept;
};

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_SERVICE_REGISTRY_H
