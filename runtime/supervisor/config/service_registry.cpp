// service_registry.cpp: the services registry (service_registry.h).

#include "service_registry.h"

#include <cassert>
#include <new>
#include <type_traits>

namespace stxconfig {

// ---------------------------------------------------------------------------
// SpawnOutcome and ServiceState: factories and queries.

SpawnOutcome::SpawnOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

SpawnOutcome SpawnOutcome::spawned(int pid) noexcept {
    Spawned state{};
    state.pid = pid;
    const SpawnOutcome outcome(Alternatives(std::in_place_type<Spawned>, state));
    return outcome;
}

SpawnOutcome SpawnOutcome::spawn_failed(int error_number) noexcept {
    SpawnFailed state{};
    state.error_number = error_number;
    const SpawnOutcome outcome(Alternatives(std::in_place_type<SpawnFailed>, state));
    return outcome;
}

const Spawned* SpawnOutcome::as_spawned() const noexcept {
    return std::get_if<Spawned>(&alternatives_);
}

const SpawnFailed* SpawnOutcome::as_spawn_failed() const noexcept {
    return std::get_if<SpawnFailed>(&alternatives_);
}

ServiceState::ServiceState(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

ServiceState ServiceState::stopped() noexcept {
    const ServiceState state(Alternatives(std::in_place_type<ServiceStopped>));
    return state;
}

ServiceState ServiceState::running(int pid, std::uint64_t started_ms) noexcept {
    ServiceRunning running{};
    running.pid = pid;
    running.started_ms = started_ms;
    const ServiceState state(Alternatives(std::in_place_type<ServiceRunning>, running));
    return state;
}

ServiceState ServiceState::waiting(std::uint64_t start_at_ms) noexcept {
    ServiceWaiting waiting{};
    waiting.start_at_ms = start_at_ms;
    const ServiceState state(Alternatives(std::in_place_type<ServiceWaiting>, waiting));
    return state;
}

ServiceState ServiceState::stopping(int pid, std::uint64_t started_ms, std::uint64_t kill_at_ms,
                                    bool kill_sent) noexcept {
    ServiceStopping stopping{};
    stopping.pid = pid;
    stopping.started_ms = started_ms;
    stopping.kill_at_ms = kill_at_ms;
    stopping.kill_sent = kill_sent;
    const ServiceState state(Alternatives(std::in_place_type<ServiceStopping>, stopping));
    return state;
}

bool ServiceState::is_stopped() const noexcept {
    return std::holds_alternative<ServiceStopped>(alternatives_);
}

const ServiceRunning* ServiceState::as_running() const noexcept {
    return std::get_if<ServiceRunning>(&alternatives_);
}

const ServiceWaiting* ServiceState::as_waiting() const noexcept {
    return std::get_if<ServiceWaiting>(&alternatives_);
}

const ServiceStopping* ServiceState::as_stopping() const noexcept {
    return std::get_if<ServiceStopping>(&alternatives_);
}

// ---------------------------------------------------------------------------
// Back-off and exit status.

std::uint64_t service_backoff_ms(std::uint32_t quick_exits_in_row) noexcept {
    if (quick_exits_in_row == 0) {
        return 0;
    }
    std::uint64_t delay = service_backoff_first_ms;
    // At most service_quick_exits_counted_max - 1 doublings, and the loop
    // stops at the cap, so the shift never overflows.
    for (std::uint32_t doubling = 1; doubling < quick_exits_in_row; doubling += 1) {
        delay *= 2u;
        if (delay >= service_backoff_max_ms) {
            return service_backoff_max_ms;
        }
    }
    return delay;
}

bool wait_status_is_success(int wait_status) noexcept {
    const unsigned int bits = static_cast<unsigned int>(wait_status);
    return (bits & 0x7fu) == 0u && ((bits >> 8u) & 0xffu) == 0u;
}

namespace {

[[nodiscard]] bool policy_restarts(RestartPolicy policy, int wait_status) noexcept {
    switch (policy) {
    case RestartPolicy::never:
        return false;
    case RestartPolicy::on_failure:
        return !wait_status_is_success(wait_status);
    case RestartPolicy::always:
        return true;
    default:
        break;
    }
    // Fallback: an unknown policy does not restart.
    return false;
}

// Counts one exit (or failed restart) after a run of ran_ms.
void account_exit(RestartAccount& account, std::uint64_t ran_ms) noexcept {
    if (ran_ms >= service_stable_run_ms) {
        account.quick_exits_in_row = 1;
    } else if (account.quick_exits_in_row < service_quick_exits_counted_max) {
        account.quick_exits_in_row += 1;
    }
}

[[nodiscard]] bool c_strings_equal(const char* left, const char* right) noexcept {
    std::size_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index += 1;
    }
    return left[index] == right[index];
}

const char* const empty_environment[] = {nullptr};

}  // namespace

// ---------------------------------------------------------------------------
// ServiceRegistry.



ServiceRegistry::ServiceRegistry() noexcept
    : config_(nullptr), environment_(empty_environment), entry_storage_{}, entry_count_(0) {}

static_assert(std::is_trivially_destructible_v<ServiceState>, "load drops entries without destructors");

ServiceRegistry::Entry& ServiceRegistry::entry(std::size_t index) noexcept {
    assert(index < entry_count_);
    unsigned char* const slot = &entry_storage_[index * sizeof(Entry)];
    return *std::launder(reinterpret_cast<Entry*>(slot));
}

const ServiceRegistry::Entry& ServiceRegistry::entry(std::size_t index) const noexcept {
    assert(index < entry_count_);
    const unsigned char* const slot = &entry_storage_[index * sizeof(Entry)];
    return *std::launder(reinterpret_cast<const Entry*>(slot));
}

void ServiceRegistry::load(const BootConfig& config) noexcept {
    config_ = &config;
    entry_count_ = 0;
    const std::size_t count = config.service_count();
    // The configuration holds at most services_max services.
    assert(count <= services_max);
    for (std::size_t index = 0; index < count && index < services_max; index += 1) {
        RestartAccount account{};
        account.restart_count = 0;
        account.quick_exits_in_row = 0;
        unsigned char* const slot = &entry_storage_[index * sizeof(Entry)];
        new (slot) Entry{index, ServiceState::stopped(), account};
        entry_count_ += 1;
    }
}

std::size_t ServiceRegistry::service_count() const noexcept {
    return entry_count_;
}

const ServiceState& ServiceRegistry::state(std::size_t index) const noexcept {
    return entry(index).state;
}

const RestartAccount& ServiceRegistry::account(std::size_t index) const noexcept {
    return entry(index).account;
}

void ServiceRegistry::set_environment(const char* const* environment) noexcept {
    environment_ = environment != nullptr ? environment : empty_environment;
}

ServiceEvent ServiceRegistry::event_for(const Entry& item, ServiceEventKind kind) const noexcept {
    const ServiceDefinition& definition = (*config_).service(item.definition_index);
    ServiceEvent event{};
    event.kind = kind;
    event.name = (*config_).text(definition.name);
    event.pid = 0;
    event.restart_count = item.account.restart_count;
    event.wait_status = 0;
    event.error_number = 0;
    event.ran_ms = 0;
    event.next_in_ms = std::nullopt;
    return event;
}

// Spawns the item's command. On success the item runs; on failure a first
// start ends stopped, and a restart waits again with the next back-off.
void ServiceRegistry::spawn_entry(Entry& item, std::uint64_t now_ms, ServiceActions& actions) noexcept {
    const ServiceDefinition& definition = (*config_).service(item.definition_index);
    const char* arguments[run_arguments_max + 1u];
    for (std::uint32_t index = 0; index < definition.argument_count; index += 1) {
        arguments[index] = (*config_).text(definition.arguments[index]);
    }
    arguments[definition.argument_count] = nullptr;
    const char* const directory = definition.directory.has_value() ? (*config_).text(*definition.directory) : "/";
    const bool is_restart = item.state.as_waiting() != nullptr;
    const SpawnOutcome outcome = actions.spawn(arguments, environment_, directory);
    const Spawned* const spawned = outcome.as_spawned();
    if (spawned != nullptr) {
        item.state = ServiceState::running((*spawned).pid, now_ms);
        ServiceEvent event = event_for(item, ServiceEventKind::started);
        event.pid = (*spawned).pid;
        actions.report(event);
        return;
    }
    const SpawnFailed* const failed = outcome.as_spawn_failed();
    ServiceEvent event = event_for(item, ServiceEventKind::spawn_failed);
    event.error_number = failed != nullptr ? (*failed).error_number : 0;
    if (is_restart && definition.restart != RestartPolicy::never) {
        account_exit(item.account, 0);
        const std::uint64_t delay_ms = service_backoff_ms(item.account.quick_exits_in_row);
        item.state = ServiceState::waiting(now_ms + delay_ms);
        event.next_in_ms = delay_ms;
    } else {
        item.state = ServiceState::stopped();
    }
    actions.report(event);
}

StartResult ServiceRegistry::start(const char* name, std::uint64_t now_ms, ServiceActions& actions) noexcept {
    if (config_ == nullptr) {
        return StartResult::unknown_service;
    }
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        Entry& candidate = entry(index);
        const ServiceDefinition& definition = (*config_).service(candidate.definition_index);
        if (!c_strings_equal((*config_).text(definition.name), name)) {
            continue;
        }
        if (!candidate.state.is_stopped()) {
            return StartResult::already_running;
        }
        // A start by request begins a fresh account.
        candidate.account.quick_exits_in_row = 0;
        spawn_entry(candidate, now_ms, actions);
        return candidate.state.as_running() != nullptr ? StartResult::started : StartResult::spawn_failed;
    }
    return StartResult::unknown_service;
}

bool ServiceRegistry::child_exited(int pid, int wait_status, std::uint64_t now_ms, ServiceActions& actions) noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        Entry& candidate = entry(index);
        const ServiceRunning* const running = candidate.state.as_running();
        const ServiceStopping* const stopping = candidate.state.as_stopping();
        if (running != nullptr && (*running).pid == pid) {
            const std::uint64_t ran_ms = now_ms >= (*running).started_ms ? now_ms - (*running).started_ms : 0;
            ServiceEvent event = event_for(candidate, ServiceEventKind::exited);
            event.pid = pid;
            event.wait_status = wait_status;
            event.ran_ms = ran_ms;
            const ServiceDefinition& definition = (*config_).service(candidate.definition_index);
            if (policy_restarts(definition.restart, wait_status)) {
                account_exit(candidate.account, ran_ms);
                if (candidate.account.restart_count < UINT64_MAX) {
                    candidate.account.restart_count += 1;
                }
                const std::uint64_t delay_ms = service_backoff_ms(candidate.account.quick_exits_in_row);
                candidate.state = ServiceState::waiting(now_ms + delay_ms);
                event.next_in_ms = delay_ms;
            } else {
                candidate.state = ServiceState::stopped();
            }
            actions.report(event);
            return true;
        }
        if (stopping != nullptr && (*stopping).pid == pid) {
            const std::uint64_t ran_ms = now_ms >= (*stopping).started_ms ? now_ms - (*stopping).started_ms : 0;
            ServiceEvent event = event_for(candidate, ServiceEventKind::exited);
            event.pid = pid;
            event.wait_status = wait_status;
            event.ran_ms = ran_ms;
            candidate.state = ServiceState::stopped();
            actions.report(event);
            return true;
        }
    }
    return false;
}

void ServiceRegistry::tick(std::uint64_t now_ms, ServiceActions& actions) noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        Entry& candidate = entry(index);
        const ServiceWaiting* const waiting = candidate.state.as_waiting();
        if (waiting != nullptr && now_ms >= (*waiting).start_at_ms) {
            spawn_entry(candidate, now_ms, actions);
            continue;
        }
        const ServiceStopping* const stopping = candidate.state.as_stopping();
        if (stopping != nullptr && !(*stopping).kill_sent && now_ms >= (*stopping).kill_at_ms) {
            const int pid = (*stopping).pid;
            actions.signal(pid, ServiceSignal::kill);
            candidate.state = ServiceState::stopping(pid, (*stopping).started_ms, (*stopping).kill_at_ms, true);
            ServiceEvent event = event_for(candidate, ServiceEventKind::killed);
            event.pid = pid;
            actions.report(event);
        }
    }
}

std::optional<std::uint64_t> ServiceRegistry::next_due_ms() const noexcept {
    std::optional<std::uint64_t> earliest;
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        const Entry& candidate = entry(index);
        std::optional<std::uint64_t> due;
        const ServiceWaiting* const waiting = candidate.state.as_waiting();
        if (waiting != nullptr) {
            due = (*waiting).start_at_ms;
        }
        const ServiceStopping* const stopping = candidate.state.as_stopping();
        if (stopping != nullptr && !(*stopping).kill_sent) {
            due = (*stopping).kill_at_ms;
        }
        if (due.has_value() && (!earliest.has_value() || *due < *earliest)) {
            earliest = due;
        }
    }
    return earliest;
}

void ServiceRegistry::stop_all(std::uint64_t now_ms, ServiceActions& actions) noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        Entry& candidate = entry(index);
        if (candidate.state.as_waiting() != nullptr) {
            candidate.state = ServiceState::stopped();
            continue;
        }
        const ServiceRunning* const running = candidate.state.as_running();
        if (running != nullptr) {
            const int pid = (*running).pid;
            const std::uint64_t started_ms = (*running).started_ms;
            actions.signal(pid, ServiceSignal::terminate);
            candidate.state = ServiceState::stopping(pid, started_ms, now_ms + service_stop_grace_ms, false);
            ServiceEvent event = event_for(candidate, ServiceEventKind::stopping);
            event.pid = pid;
            actions.report(event);
        }
        // Stopped or already stopping: nothing to do.
    }
}

bool ServiceRegistry::all_stopped() const noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        if (!entry(index).state.is_stopped()) {
            return false;
        }
    }
    return true;
}

}  // namespace stxconfig
