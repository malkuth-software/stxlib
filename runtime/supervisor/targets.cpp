// targets.cpp: see targets.h.

#include "targets.h"

#include <cerrno>
#include <cstring>
#include <span>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../common/console_line.h"
#include "../package/manifest.h"
#include "../package/store_layout.h"
#include "../package/stxpkg_exit.h"
#include "boot_slots_system.h"

namespace stxtargets {

using stxruntime::LineText;

const char* request_result_name(RequestResult result) noexcept {
    switch (result) {
    case RequestResult::accepted: return "accepted";
    case RequestResult::unknown_target: return "unknown_target";
    case RequestResult::busy: return "busy";
    case RequestResult::nothing_to_do: return "nothing_to_do";
    case RequestResult::failed: return "failed";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

constexpr int interrupted_retries_max = 16;
// The longest URL a request may name: runtime/net/url.h's url_length_max
// (sc2's channel allows up to 1024; the downloader refuses more than this).
constexpr std::size_t url_request_max = 255;
const char* const job_environment[] = {"PATH=/sbin:/bin:/usr/sbin:/usr/bin", nullptr};

[[nodiscard]] const char* outcome_name(JobOutcome outcome) noexcept {
    switch (outcome) {
    case JobOutcome::installed: return "installed";
    case JobOutcome::nothing_newer: return "nothing-newer";
    case JobOutcome::refused: return "refused";
    case JobOutcome::failed: return "failed";
    default: break;
    }
    return "unknown";
}

[[nodiscard]] std::optional<stxpackage::StorePath> state_path(const char* program, const char* leaf) noexcept {
    const char* const parts[] = {program, leaf};
    return stxpackage::store_path(parts, 2);
}

// Reads a program's stored state: none when there is no state file;
// nothing when it exists but cannot be read or parsed.
[[nodiscard]] std::optional<stxpackage::StoredState> read_state(const char* program) noexcept {
    const stxpackage::StoredState none{stxpackage::ProgramState::none(), 0};
    const std::optional<stxpackage::StorePath> path = state_path(program, "state");
    if (!path.has_value()) {
        return std::nullopt;
    }
    const int file = open((*path).text, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        if (errno == ENOENT) {
            const std::optional<stxpackage::StoredState> result = none;
            return result;
        }
        return std::nullopt;
    }
    char text[stxpackage::state_file_size_max + 1u] = {};
    const ssize_t got = read(file, text, sizeof(text));
    (void)close(file);
    if (got < 0) {
        return std::nullopt;
    }
    return stxpackage::parse_program_state(std::span<const char>(text, static_cast<std::size_t>(got)));
}

// A slot's installed version (its manifest, root's file), or 0.
[[nodiscard]] std::uint32_t slot_version(const char* program, stxpackage::Slot slot) noexcept {
    static char text[stxpackage::manifest_size_max];
    static stxpackage::Manifest parsed;
    const char letter[2] = {stxpackage::slot_letter(slot), '\0'};
    const char* const parts[] = {program, letter, "manifest"};
    const std::optional<stxpackage::StorePath> path = stxpackage::store_path(parts, 3);
    if (!path.has_value()) {
        return 0;
    }
    const int file = open((*path).text, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        return 0;
    }
    std::size_t total = 0;
    int interrupted = 0;
    while (total < sizeof(text)) {
        const ssize_t got = read(file, &text[total], sizeof(text) - total);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
            interrupted += 1;
            continue;
        }
        break;
    }
    (void)close(file);
    if (stxpackage::parse_manifest(std::span<const char>(text, total), parsed).has_value()) {
        return 0;
    }
    return parsed.version;
}

void begin_target_line(LineText& line, const char* name) noexcept {
    stxruntime::append_text(line, "STXSUPERVISOR target ");
    stxruntime::append_text(line, name);
}

}  // namespace

Targets::Targets(int console, stxconfig::ServiceRegistry& registry, stxconfig::ServiceActions& actions) noexcept
    : console_(console), registry_(registry), actions_(actions), config_(nullptr), entries_{}, entry_count_(0),
      job_(std::nullopt), system_slot_(std::nullopt), system_next_check_ms_(std::nullopt),
      system_interval_s_(stxconfig::update_interval_default_s), reboot_requested_(false),
      last_operation_{OperationKind::none, false, false, 0, 0, 0, nullptr}, logs_() {}

void Targets::load(const stxconfig::BootConfig& config, std::optional<stxboot::Slot> running_system_slot,
                   std::uint64_t now_ms) noexcept {
    config_ = &config;
    entry_count_ = 0;
    for (std::size_t index = 0; index < config.service_count() && entry_count_ < stxconfig::services_max;
         index += 1) {
        const stxconfig::ServiceDefinition& definition = config.service(index);
        if (!definition.target.has_value()) {
            continue;
        }
        Entry& entry = entries_[entry_count_];
        entry.service_index = index;
        entry.stored = read_state(config.text((*definition.target).program));
        entry.running = std::nullopt;
        entry.restart_pending = false;
        entry.next_check_ms = std::nullopt;
        if ((*definition.target).update_url.has_value()) {
            entry.next_check_ms = now_ms + update_first_delay_ms;
        }
        if (!entry.stored.has_value()) {
            LineText line = stxruntime::empty_line();
            begin_target_line(line, config.text(definition.name));
            stxruntime::append_text(line, " state unreadable\n");
            stxruntime::write_line(console_, line);
        }
        entry_count_ += 1;
    }
    // The system update service: [system] update-url, update-interval.
    system_slot_ = running_system_slot;
    system_next_check_ms_ = std::nullopt;
    system_interval_s_ = stxconfig::update_interval_default_s;
    const std::optional<stxconfig::TextRef> interval = config.find_setting("system", "update-interval");
    if (interval.has_value()) {
        std::uint64_t seconds = 0;
        const char* const digits = config.text(*interval);
        bool valid = digits[0] != '\0';
        for (std::size_t index = 0; valid && digits[index] != '\0'; index += 1) {
            valid = digits[index] >= '0' && digits[index] <= '9' && index < 8u;
            seconds = seconds * 10u + static_cast<std::uint64_t>(digits[index] - '0');
        }
        if (valid && seconds >= stxconfig::update_interval_min_s && seconds <= stxconfig::update_interval_max_s) {
            system_interval_s_ = static_cast<std::uint32_t>(seconds);
        }
    }
    if (config.find_setting("system", "update-url").has_value() && running_system_slot.has_value()) {
        system_next_check_ms_ = now_ms + update_first_delay_ms;
    }
}

void Targets::start_boot_targets(std::uint64_t now_ms) noexcept {
    if (config_ == nullptr) {
        return;
    }
    // The foreground target first, then the servers in file order.
    for (std::size_t pass = 0; pass < 2u; pass += 1) {
        for (std::size_t index = 0; index < entry_count_; index += 1) {
            const stxconfig::TargetSettings& target = settings(entries_[index]);
            const bool foreground = target.role == stxconfig::TargetRole::foreground;
            if (target.start != stxconfig::StartPolicy::boot || foreground != (pass == 0u)) {
                continue;
            }
            (void)registry_.start(name(entries_[index]), now_ms, actions_);
        }
    }
}

std::optional<char> Targets::slot_to_run(const stxconfig::SpawnRequest& request) noexcept {
    const std::optional<std::size_t> found = entry_of_service(request.service_index);
    if (!found.has_value() || !entries_[*found].stored.has_value()) {
        return std::nullopt;
    }
    const std::optional<stxpackage::Slot> slot = stxpackage::slot_to_run((*entries_[*found].stored).state);
    if (!slot.has_value()) {
        return std::nullopt;
    }
    const std::optional<char> letter = stxpackage::slot_letter(*slot);
    return letter;
}

void Targets::target_started(std::size_t service_index, char slot, int pid) noexcept {
    const std::optional<std::size_t> found = entry_of_service(service_index);
    const std::optional<stxpackage::Slot> started = stxpackage::slot_of_letter(slot);
    if (!found.has_value() || !started.has_value()) {
        return;
    }
    entries_[*found].running = Running{*started, pid, stxconfig::monotonic_milliseconds()};
    // A marker in the target's output ring, so a reader sees where each
    // start begins (target_log.h).
    LineText marker = stxruntime::empty_line();
    stxruntime::append_text(marker, "--- start pid=");
    stxruntime::append_decimal(marker, static_cast<std::uint64_t>(pid));
    const char letter[2] = {slot, '\0'};
    stxruntime::append_text(marker, " slot=");
    stxruntime::append_text(marker, letter);
    stxruntime::append_text(marker, "\n");
    if (!marker.truncated) {
        logs_.note(service_index, std::span<const char>(marker.bytes, marker.length));
    }
}

int Targets::prepare_output(std::size_t service_index) noexcept {
    return logs_.open_output(service_index);
}

void Targets::output_abandoned(std::size_t service_index) noexcept {
    logs_.abandon(service_index);
}

void Targets::service_event(const stxconfig::ServiceEvent& event) noexcept {
    if (event.kind != stxconfig::ServiceEventKind::exited) {
        return;
    }
    const std::optional<std::size_t> found = entry_of_service(event.service_index);
    if (!found.has_value()) {
        return;
    }
    Entry& entry = entries_[*found];
    if (!entry.running.has_value() || (*entry.running).pid != event.pid || !entry.stored.has_value()) {
        entry.running = std::nullopt;
        return;
    }
    const Running ran = *entry.running;
    entry.running = std::nullopt;
    const ExitKind kind = exit_kind(event.wait_status, event.ran_ms, settings(entry).healthy_after_s);
    const stxpackage::SlotTransition transition = after_exit((*entry.stored).state, ran.slot, kind);
    if (transition.note == stxpackage::SlotNote::fell_back) {
        // The version that failed is not installed again by the update
        // service.
        (*entry.stored).refused_version = slot_version(program(entry), ran.slot);
        // A refused start is not restarted by the registry; a fallback after
        // an early exit may be waiting for its back-off. Either way the
        // good slot starts now (outside this callback: tick).
        entry.restart_pending = true;
    }
    apply(entry, transition);
}

bool Targets::child_exited(int pid, int wait_status, std::uint64_t now_ms) noexcept {
    if (!job_.has_value() || (*job_).pid != pid) {
        return false;
    }
    const Job job = *job_;
    job_ = std::nullopt;
    finish_job(job, wait_status, now_ms);
    return true;
}

void Targets::tick(std::uint64_t now_ms) noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        Entry& entry = entries_[index];
        // A trial that has run healthy_after is kept.
        if (entry.running.has_value() && entry.stored.has_value()) {
            const stxpackage::GoodWithTrial* const trial = (*entry.stored).state.as_good_with_trial();
            const std::uint64_t healthy_ms = static_cast<std::uint64_t>(settings(entry).healthy_after_s) * 1000u;
            if (trial != nullptr && (*entry.running).slot == stxpackage::other_slot((*trial).good) &&
                now_ms >= (*entry.running).started_ms + healthy_ms) {
                apply(entry, stxpackage::after_healthy((*entry.stored).state, (*entry.running).slot));
            }
        }
        if (entry.restart_pending) {
            entry.restart_pending = false;
            (void)registry_.restart(name(entry), now_ms, actions_);
        }
    }
    if (job_.has_value()) {
        return;
    }
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        if (entries_[index].next_check_ms.has_value() && now_ms >= *entries_[index].next_check_ms) {
            start_update(index, now_ms);
            return;
        }
    }
    if (system_next_check_ms_.has_value() && now_ms >= *system_next_check_ms_ && !reboot_requested_) {
        start_system_update(now_ms);
    }
}

std::optional<std::uint64_t> Targets::next_due_ms() const noexcept {
    std::optional<std::uint64_t> due;
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        const Entry& entry = entries_[index];
        std::optional<std::uint64_t> at;
        if (entry.restart_pending) {
            at = 0;
        } else if (entry.running.has_value() && entry.stored.has_value() &&
                   (*entry.stored).state.as_good_with_trial() != nullptr) {
            at = (*entry.running).started_ms + static_cast<std::uint64_t>(settings(entry).healthy_after_s) * 1000u;
        }
        if (!job_.has_value() && entry.next_check_ms.has_value() &&
            (!at.has_value() || *entry.next_check_ms < *at)) {
            at = entry.next_check_ms;
        }
        if (at.has_value() && (!due.has_value() || *at < *due)) {
            due = at;
        }
    }
    if (!job_.has_value() && system_next_check_ms_.has_value() &&
        (!due.has_value() || *system_next_check_ms_ < *due)) {
        due = system_next_check_ms_;
    }
    return due;
}

bool Targets::reboot_requested() const noexcept {
    return reboot_requested_;
}

RequestResult Targets::request_update(const char* target_name_text, std::uint64_t now_ms) noexcept {
    const std::optional<std::size_t> found = entry_of_name(target_name_text);
    if (!found.has_value()) {
        return RequestResult::unknown_target;
    }
    if (!settings(entries_[*found]).update_url.has_value()) {
        return RequestResult::nothing_to_do;
    }
    if (job_.has_value()) {
        return RequestResult::busy;
    }
    start_update(*found, now_ms);
    return job_.has_value() ? RequestResult::accepted : RequestResult::failed;
}

RequestResult Targets::request_install(const char* target_name_text, const char* package_path,
                                       std::uint64_t now_ms) noexcept {
    const std::optional<std::size_t> found = entry_of_name(target_name_text);
    if (!found.has_value()) {
        return RequestResult::unknown_target;
    }
    if (job_.has_value()) {
        return RequestResult::busy;
    }
    Entry& entry = entries_[*found];
    if (!entry.stored.has_value()) {
        return RequestResult::failed;
    }
    const char letter[2] = {stxpackage::slot_letter(stxpackage::install_slot((*entry.stored).state)), '\0'};
    const char* const arguments[] = {stxpackage::stxpkg_path, "install", program(entry), letter, package_path,
                                     nullptr};
    return start_job(JobKind::install, *found, arguments, OperationKind::program_install, true, now_ms)
               ? RequestResult::accepted
               : RequestResult::failed;
}

RequestResult Targets::request_update_from(const char* target_name_text, const char* url,
                                           std::uint64_t now_ms) noexcept {
    const std::optional<std::size_t> found = entry_of_name(target_name_text);
    if (!found.has_value()) {
        return RequestResult::unknown_target;
    }
    const char prefix[] = "https://";
    const std::size_t length = url == nullptr ? 0u : std::strlen(url);
    if (length <= sizeof(prefix) - 1u || length > url_request_max || std::strncmp(url, prefix, sizeof(prefix) - 1u) != 0) {
        return RequestResult::nothing_to_do;
    }
    if (job_.has_value()) {
        return RequestResult::busy;
    }
    Entry& entry = entries_[*found];
    if (!entry.stored.has_value()) {
        return RequestResult::failed;
    }
    const char letter[2] = {stxpackage::slot_letter(stxpackage::install_slot((*entry.stored).state)), '\0'};
    const char* const arguments[] = {stxpackage::stxpkg_path, "update", program(entry), letter, url, nullptr};
    return start_job(JobKind::update, *found, arguments, OperationKind::program_install, true, now_ms)
               ? RequestResult::accepted
               : RequestResult::failed;
}

RequestResult Targets::request_system_update(std::uint64_t now_ms) noexcept {
    if (config_ == nullptr || !(*config_).find_setting("system", "update-url").has_value() ||
        !system_slot_.has_value() || reboot_requested_) {
        return RequestResult::nothing_to_do;
    }
    if (job_.has_value()) {
        return RequestResult::busy;
    }
    start_system_update(now_ms);
    return job_.has_value() ? RequestResult::accepted : RequestResult::failed;
}

RequestResult Targets::request_system_install(const char* package_path, std::uint64_t now_ms) noexcept {
    if (!system_slot_.has_value() || reboot_requested_ || package_path == nullptr || package_path[0] != '/') {
        return RequestResult::nothing_to_do;
    }
    if (job_.has_value()) {
        return RequestResult::busy;
    }
    const stxboot::Slot inactive = *system_slot_ == stxboot::Slot::a ? stxboot::Slot::b : stxboot::Slot::a;
    const char letter[2] = {inactive == stxboot::Slot::a ? 'a' : 'b', '\0'};
    const char* const arguments[] = {stxpackage::stxpkg_path, "system-install", letter, package_path, nullptr};
    return start_job(JobKind::system_update, 0, arguments, OperationKind::system_update, true, now_ms)
               ? RequestResult::accepted
               : RequestResult::failed;
}

RequestResult Targets::rollback(const char* target_name_text, std::uint64_t now_ms) noexcept {
    (void)now_ms;
    const std::optional<std::size_t> found = entry_of_name(target_name_text);
    if (!found.has_value()) {
        return RequestResult::unknown_target;
    }
    Entry& entry = entries_[*found];
    if (!entry.stored.has_value() || job_.has_value()) {
        return job_.has_value() ? RequestResult::busy : RequestResult::failed;
    }
    const stxpackage::SlotTransition transition = stxpackage::after_rollback((*entry.stored).state);
    if (transition.note == stxpackage::SlotNote::rollback_refused) {
        return RequestResult::nothing_to_do;
    }
    apply(entry, transition);
    entry.restart_pending = true;
    return RequestResult::accepted;
}

std::optional<TargetStatus> Targets::status(const char* target_name_text) const noexcept {
    const std::optional<std::size_t> found = entry_of_name(target_name_text);
    if (!found.has_value()) {
        return std::nullopt;
    }
    const Entry& entry = entries_[*found];
    TargetStatus result{};
    result.service_index = entry.service_index;
    result.stored = entry.stored;
    result.running_slot = std::nullopt;
    if (entry.running.has_value()) {
        result.running_slot = (*entry.running).slot;
    }
    result.version_a = slot_version(program(entry), stxpackage::Slot::a);
    result.version_b = slot_version(program(entry), stxpackage::Slot::b);
    result.job_running = job_.has_value() && (*job_).kind != JobKind::system_update && (*job_).entry == *found;
    result.next_check_ms = entry.next_check_ms;
    const std::optional<TargetStatus> answer = result;
    return answer;
}

OperationStatus Targets::last_operation() const noexcept {
    return last_operation_;
}

std::optional<TargetLogEnd> Targets::log_end(const char* target_name_text, std::span<char> destination) const noexcept {
    if (target_name_text == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::size_t> found = entry_of_name(target_name_text);
    if (!found.has_value()) {
        return std::nullopt;
    }
    const std::optional<TargetLogEnd> end = logs_.read_end(entries_[*found].service_index, destination);
    return end;
}

void Targets::drain_logs() noexcept {
    logs_.drain();
}

std::size_t Targets::log_descriptors(std::span<int> out) const noexcept {
    return logs_.descriptors(out);
}

std::size_t Targets::target_count() const noexcept {
    return entry_count_;
}

const char* Targets::target_name(std::size_t index) const noexcept {
    return name(entries_[index < entry_count_ ? index : 0u]);
}

// ---------------------------------------------------------------------------
// Private.

std::optional<std::size_t> Targets::entry_of_service(std::size_t service_index) const noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        if (entries_[index].service_index == service_index) {
            const std::optional<std::size_t> found = index;
            return found;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Targets::entry_of_name(const char* target_name_text) const noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        if (std::strcmp(name(entries_[index]), target_name_text) == 0) {
            const std::optional<std::size_t> found = index;
            return found;
        }
    }
    return std::nullopt;
}

const stxconfig::TargetSettings& Targets::settings(const Entry& entry) const noexcept {
    // Every entry was made from a definition with a target.
    return *(*config_).service(entry.service_index).target;
}

const char* Targets::program(const Entry& entry) const noexcept {
    return (*config_).text(settings(entry).program);
}

const char* Targets::name(const Entry& entry) const noexcept {
    return (*config_).text((*config_).service(entry.service_index).name);
}

void Targets::apply(Entry& entry, const stxpackage::SlotTransition& transition) noexcept {
    if (transition.note == stxpackage::SlotNote::unchanged || !entry.stored.has_value()) {
        return;
    }
    (*entry.stored).state = transition.next;
    const bool stored = store(entry);
    LineText line = stxruntime::empty_line();
    begin_target_line(line, name(entry));
    const std::optional<stxpackage::Slot> run = stxpackage::slot_to_run(transition.next);
    const char letter[2] = {run.has_value() ? stxpackage::slot_letter(*run) : '-', '\0'};
    stxruntime::append_text(line, " slot=");
    stxruntime::append_text(line, letter);
    stxruntime::append_text(line, " note=");
    stxruntime::append_text(line, stxpackage::slot_note_name(transition.note));
    if (transition.note == stxpackage::SlotNote::fell_back) {
        stxruntime::append_text(line, " refused_version=");
        stxruntime::append_decimal(line, (*entry.stored).refused_version);
    }
    stxruntime::append_text(line, stored ? "\n" : " write-failed\n");
    stxruntime::write_line(console_, line);
}

// The state file by a new file and a rename; fsync both.
bool Targets::store(Entry& entry) noexcept {
    if (!entry.stored.has_value()) {
        return false;
    }
    const std::optional<stxpackage::StorePath> path = state_path(program(entry), "state");
    const std::optional<stxpackage::StorePath> fresh = state_path(program(entry), "state.new");
    const char* const directory_parts[] = {program(entry)};
    const std::optional<stxpackage::StorePath> directory = stxpackage::store_path(directory_parts, 1);
    if (!path.has_value() || !fresh.has_value() || !directory.has_value()) {
        return false;
    }
    char text[stxpackage::state_file_size_max] = {};
    const std::size_t length =
        stxpackage::format_program_state(*entry.stored, std::span<char, stxpackage::state_file_size_max>(text));
    const int file = open((*fresh).text, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (file < 0) {
        return false;
    }
    const bool written = write(file, text, length) == static_cast<ssize_t>(length) && fsync(file) == 0;
    (void)close(file);
    if (!written || rename((*fresh).text, (*path).text) != 0) {
        return false;
    }
    const int folder = open((*directory).text, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder >= 0) {
        (void)fsync(folder);
        (void)close(folder);
    }
    return true;
}

bool Targets::start_job(JobKind kind, std::size_t entry_index, const char* const* arguments,
                        OperationKind operation, bool install, std::uint64_t now_ms) noexcept {
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        (void)setsid();
        sigset_t none{};
        sigemptyset(&none);
        (void)sigprocmask(SIG_SETMASK, &none, nullptr);
        closefrom(3);
        (void)chdir("/");
        (void)execve(arguments[0], const_cast<char* const*>(arguments), const_cast<char* const*>(job_environment));
        _exit(127);
    }
    job_ = Job{kind, static_cast<int>(child), entry_index};
    last_operation_ = OperationStatus{operation, install, true, 0, now_ms, now_ms,
                                      kind == JobKind::system_update ? nullptr : name(entries_[entry_index])};
    LineText line = stxruntime::empty_line();
    if (kind == JobKind::system_update) {
        stxruntime::append_text(line, "STXSUPERVISOR system job=update");
    } else {
        begin_target_line(line, name(entries_[entry_index]));
        stxruntime::append_text(line, kind == JobKind::update ? " job=update" : " job=install");
    }
    stxruntime::append_text(line, " pid=");
    stxruntime::append_decimal(line, static_cast<std::uint64_t>(child));
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console_, line);
    return true;
}

void Targets::start_update(std::size_t entry_index, std::uint64_t now_ms) noexcept {
    Entry& entry = entries_[entry_index];
    const stxconfig::TargetSettings& target = settings(entry);
    entry.next_check_ms = std::nullopt;
    if (!target.update_url.has_value() || !entry.stored.has_value()) {
        return;
    }
    const char letter[2] = {stxpackage::slot_letter(stxpackage::install_slot((*entry.stored).state)), '\0'};
    const char* const arguments[] = {stxpackage::stxpkg_path, "update", program(entry), letter,
                                     (*config_).text(*target.update_url), nullptr};
    if (!start_job(JobKind::update, entry_index, arguments, OperationKind::program_check, false, now_ms)) {
        entry.next_check_ms = next_check_ms(JobOutcome::failed, now_ms, target.update_interval_s);
    }
}

void Targets::start_system_update(std::uint64_t now_ms) noexcept {
    system_next_check_ms_ = std::nullopt;
    const std::optional<stxconfig::TextRef> url = (*config_).find_setting("system", "update-url");
    if (!url.has_value() || !system_slot_.has_value()) {
        return;
    }
    const stxboot::Slot inactive = *system_slot_ == stxboot::Slot::a ? stxboot::Slot::b : stxboot::Slot::a;
    const char letter[2] = {inactive == stxboot::Slot::a ? 'a' : 'b', '\0'};
    const char* const arguments[] = {stxpackage::stxpkg_path, "system-update", letter, (*config_).text(*url),
                                     nullptr};
    if (!start_job(JobKind::system_update, 0, arguments, OperationKind::system_update, false, now_ms)) {
        system_next_check_ms_ = next_check_ms(JobOutcome::failed, now_ms, system_interval_s_);
    }
}

void Targets::finish_job(const Job& job, int wait_status, std::uint64_t now_ms) noexcept {
    last_operation_.running = false;
    last_operation_.wait_status = wait_status;
    last_operation_.ended_ms = now_ms < last_operation_.started_ms ? last_operation_.started_ms : now_ms;
    const JobOutcome outcome = job_outcome(wait_status);
    LineText line = stxruntime::empty_line();
    if (job.kind == JobKind::system_update) {
        stxruntime::append_text(line, "STXSUPERVISOR system job=update outcome=");
        stxruntime::append_text(line, outcome_name(outcome));
        stxruntime::append_text(line, "\n");
        stxruntime::write_line(console_, line);
        if (outcome == JobOutcome::installed && system_slot_.has_value()) {
            const stxboot::Slot inactive = *system_slot_ == stxboot::Slot::a ? stxboot::Slot::b : stxboot::Slot::a;
            if (stxboot::arm_system_update(console_, *system_slot_, inactive)) {
                reboot_requested_ = true;
                return;
            }
        }
        system_next_check_ms_ = next_check_ms(outcome, now_ms, system_interval_s_);
        return;
    }
    Entry& entry = entries_[job.entry];
    const stxconfig::TargetSettings& target = settings(entry);
    if (target.update_url.has_value()) {
        entry.next_check_ms = next_check_ms(outcome, now_ms, target.update_interval_s);
    }
    begin_target_line(line, name(entry));
    stxruntime::append_text(line, " outcome=");
    stxruntime::append_text(line, outcome_name(outcome));
    if (entry.next_check_ms.has_value()) {
        stxruntime::append_text(line, " next_check_s=");
        stxruntime::append_decimal(line, (*entry.next_check_ms - now_ms) / 1000u);
    }
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console_, line);
    if (outcome != JobOutcome::installed || !entry.stored.has_value()) {
        return;
    }
    apply(entry, stxpackage::after_installed((*entry.stored).state));
    // The new version goes on trial (or, the first install, runs) now.
    entry.restart_pending = true;
}

}  // namespace stxtargets
