// control_channels.cpp: the control channels on FreeBSD
// (control_channels.h).

#include "control_channels.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../common/console_line.h"
#include "../store/turn_file_system.h"
#include "boot_slots_system.h"
#include "config/user_config.h"
#include "program_answers.h"
#include "target_id_files.h"

namespace stxsupervisor {

namespace {

using stxruntime::LineText;

constexpr int interrupted_retries_max = 16;

[[nodiscard]] std::uint64_t unsigned_of(int value) noexcept {
    return static_cast<std::uint64_t>(static_cast<unsigned int>(value));
}

void report_errno(int console, const char* prefix, int error_number) noexcept {
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, prefix);
    stxruntime::append_decimal(line, unsigned_of(error_number));
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

[[nodiscard]] bool add_close_on_exec(int descriptor) noexcept {
    const int flags = fcntl(descriptor, F_GETFD);
    return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

[[nodiscard]] bool add_non_blocking(int descriptor) noexcept {
    const int flags = fcntl(descriptor, F_GETFL);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

ControlChannels::ControlChannels(int console) noexcept
    : console_(console), queue_(-1), channels_{}, channel_count_(0), pending_parent_(-1), pending_child_(-1),
      request_{}, response_{} {}

ControlChannels::~ControlChannels() {
    close_pending();
    while (channel_count_ > 0u) {
        close_channel(channel_count_ - 1u);
    }
    if (queue_ >= 0) {
        (void)close(queue_);
        queue_ = -1;
    }
}

bool ControlChannels::open(std::span<const int> signal_numbers) noexcept {
    if (queue_ >= 0) {
        return true;
    }
    if (signal_numbers.size() > control_signals_max) {
        return false;
    }
    const int queue = kqueue();
    if (queue < 0) {
        report_errno(console_, "STXSUPERVISOR control queue failed errno=", errno);
        return false;
    }
    // A kqueue is not inherited across fork (kqueue(2)); close-on-exec is
    // set as well, so no exec'ed program can hold it.
    if (!add_close_on_exec(queue)) {
        report_errno(console_, "STXSUPERVISOR control queue failed errno=", errno);
        (void)close(queue);
        return false;
    }
    struct kevent changes[control_signals_max]{};
    for (std::size_t index = 0; index < signal_numbers.size(); index += 1) {
        EV_SET(&changes[index], static_cast<uintptr_t>(signal_numbers[index]), EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
    }
    if (kevent(queue, changes, static_cast<int>(signal_numbers.size()), nullptr, 0, nullptr) != 0) {
        report_errno(console_, "STXSUPERVISOR control queue failed errno=", errno);
        (void)close(queue);
        return false;
    }
    queue_ = queue;
    return true;
}

bool ControlChannels::is_open() const noexcept {
    return queue_ >= 0;
}

void ControlChannels::close_pending() noexcept {
    if (pending_parent_ >= 0) {
        (void)close(pending_parent_);
        pending_parent_ = -1;
    }
    if (pending_child_ >= 0) {
        (void)close(pending_child_);
        pending_child_ = -1;
    }
}

int ControlChannels::prepare_channel() noexcept {
    // A prepare without its started or abandoned is a caller defect; its
    // descriptors are not leaked.
    close_pending();
    if (queue_ < 0) {
        return -1;
    }
    if (channel_count_ >= control_channels_max) {
        LineText line = stxruntime::empty_line();
        stxruntime::append_text(line, "STXSUPERVISOR control channel unavailable reason=full\n");
        stxruntime::write_line(console_, line);
        return -1;
    }
    int pair[2] = {-1, -1};
    // Close-on-exec on both ends: the child's end reaches the service only
    // through dup2 to descriptor 3, which clears the flag on the copy.
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
        report_errno(console_, "STXSUPERVISOR control channel unavailable reason=errno=", errno);
        return -1;
    }
    // Only the supervisor's end is non-blocking: the flag belongs to the
    // open file, which the service's end does not share.
    if (!add_non_blocking(pair[0])) {
        report_errno(console_, "STXSUPERVISOR control channel unavailable reason=errno=", errno);
        (void)close(pair[0]);
        (void)close(pair[1]);
        return -1;
    }
    pending_parent_ = pair[0];
    pending_child_ = pair[1];
    return pending_child_;
}

void ControlChannels::channel_started(int pid) noexcept {
    if (pending_parent_ < 0) {
        return;
    }
    // The service holds its own copy now.
    (void)close(pending_child_);
    pending_child_ = -1;
    struct kevent change{};
    EV_SET(&change, static_cast<uintptr_t>(pending_parent_), EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (channel_count_ >= control_channels_max || kevent(queue_, &change, 1, nullptr, 0, nullptr) != 0) {
        report_errno(console_, "STXSUPERVISOR control channel unavailable reason=errno=", errno);
        close_pending();
        return;
    }
    Channel channel{};
    channel.descriptor = pending_parent_;
    channel.pid = pid;
    channels_[channel_count_] = channel;
    channel_count_ += 1;
    pending_parent_ = -1;
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR control channel pid=");
    stxruntime::append_decimal(line, unsigned_of(pid));
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console_, line);
}

void ControlChannels::channel_abandoned() noexcept {
    close_pending();
}

void ControlChannels::close_channel(std::size_t index) noexcept {
    if (index >= channel_count_) {
        return;
    }
    // Closing the descriptor removes its kqueue note (kqueue(2)).
    (void)close(channels_[index].descriptor);
    // Keep the table dense: the last entry takes the closed one's place.
    channels_[index] = channels_[channel_count_ - 1u];
    channel_count_ -= 1;
}

void ControlChannels::watch(std::span<const int> descriptors) noexcept {
    if (queue_ < 0 || descriptors.empty()) {
        return;
    }
    struct kevent changes[control_channels_max]{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < descriptors.size() && count < control_channels_max; index += 1) {
        if (descriptors[index] < 0) {
            continue;
        }
        EV_SET(&changes[count], static_cast<uintptr_t>(descriptors[index]), EVFILT_READ, EV_ADD, 0, 0, nullptr);
        count += 1;
    }
    if (count == 0u) {
        return;
    }
    // A failure leaves those pipes unwatched: their output then waits for
    // the next wake, and a target that fills its pipe (64 KiB) waits too.
    (void)kevent(queue_, changes, static_cast<int>(count), nullptr, 0, nullptr);
}

void ControlChannels::wait(std::uint64_t timeout_ms) noexcept {
    if (queue_ < 0) {
        return;
    }
    struct timespec timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000u);
    timeout.tv_nsec = static_cast<long>((timeout_ms % 1000u) * stxruntime::nanoseconds_per_millisecond);
    // The events themselves are not needed: the caller takes pending
    // signals and serve() looks at every channel.
    struct kevent events[control_channels_max + control_signals_max]{};
    const int count = kevent(queue_, nullptr, 0, events, static_cast<int>(sizeof(events) / sizeof(events[0])),
                             &timeout);
    (void)count;
}

void ControlChannels::report_request(const stxcontrol::ControlHandled& handled, int pid,
                                     const stxconfig::ServiceRegistry& registry) noexcept {
    const bool operated = handled.command.has_value() && stxcontrol::command_operates(*handled.command);
    if (!operated && handled.status != stxcontrol::Status::malformed &&
        handled.status != stxcontrol::Status::not_permitted) {
        return;
    }
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR control ");
    if (handled.requester.has_value() && *handled.requester < registry.service_count()) {
        stxruntime::append_text(line, registry.service_name(*handled.requester));
    } else {
        stxruntime::append_text(line, "pid=");
        stxruntime::append_decimal(line, unsigned_of(pid));
    }
    stxruntime::append_text(line, " ");
    stxruntime::append_text(line, handled.command.has_value() ? stxcontrol::command_name(*handled.command)
                                                              : "malformed");
    if (handled.command.has_value() && stxcontrol::command_takes_name(*handled.command)) {
        // The target is named when the response carries its record (it
        // exists); otherwise the status says why not.
        const stxcontrol::ResponseOutcome response = stxcontrol::decode_response(
            std::span<const std::uint8_t>(response_, handled.response_size));
        const stxcontrol::ResponseView* const view = response.as_view();
        std::optional<stxcontrol::ServiceName> named;
        if (view != nullptr && (*view).payload.size() == stxcontrol::service_record_bytes) {
            const std::optional<stxcontrol::ServiceRecord> record = stxcontrol::decode_service_record((*view).payload);
            if (record.has_value()) {
                named = (*record).name;
            }
        }
        if (view != nullptr && (*view).payload.size() == stxcontrol::program_record_bytes) {
            const std::optional<stxcontrol::ProgramRecord> record = stxcontrol::decode_program_record((*view).payload);
            if (record.has_value()) {
                named = (*record).name;
            }
        }
        if (named.has_value()) {
            char target[stxcontrol::service_name_bytes_max + 1];
            stxcontrol::service_name_c_string(*named, target);
            stxruntime::append_text(line, " ");
            stxruntime::append_text(line, target);
        }
    }
    stxruntime::append_text(line, " status=");
    stxruntime::append_text(line, stxcontrol::status_name(handled.status));
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console_, line);
}

void ControlChannels::serve(stxcontrol::ControlServer& server, const stxcontrol::ControlFacts& facts,
                            std::uint64_t now_ms, const stxconfig::ServiceRegistry& registry) noexcept {
    std::size_t index = 0;
    // Each pass either keeps the channel (index advances) or closes it
    // (the count drops), so the loop ends within channel_count_ passes.
    while (index < channel_count_) {
        const Channel channel = channels_[index];
        bool keep = true;
        int interrupted = 0;
        int requests = 0;
        while (keep && requests < control_requests_per_round) {
            const ssize_t got = recv(channel.descriptor, request_, sizeof(request_), 0);
            if (got == 0) {
                // The service's end is closed: it exited.
                keep = false;
                break;
            }
            if (got < 0) {
                const int error_number = errno;
                if (error_number == EINTR && interrupted < interrupted_retries_max) {
                    interrupted += 1;
                    continue;
                }
                keep = error_number == EAGAIN || error_number == EWOULDBLOCK;
                break;
            }
            requests += 1;
            const std::span<const std::uint8_t> frame(request_, static_cast<std::size_t>(got));
            const std::span<std::uint8_t> response(response_, sizeof(response_));
            const stxcontrol::ControlHandled handled = server.handle(frame, channel.pid, facts, now_ms, response);
            report_request(handled, channel.pid, registry);
            // One request is outstanding at a time and a response is smaller
            // than the socket's buffer (64 KiB at the pin;
            // control_protocol.h), so the send completes or the service has
            // broken the protocol (or gone): either way a failed send closes
            // the channel. MSG_NOSIGNAL: no SIGPIPE.
            const ssize_t sent = send(channel.descriptor, response_, handled.response_size, MSG_NOSIGNAL);
            if (sent < 0 || static_cast<std::size_t>(sent) != handled.response_size) {
                keep = false;
            }
        }
        if (keep) {
            index += 1;
        } else {
            close_channel(index);
        }
    }
}

void ControlChannels::sweep(const stxconfig::ServiceRegistry& registry) noexcept {
    std::size_t index = 0;
    while (index < channel_count_) {
        if (registry.find_running_pid(channels_[index].pid).has_value()) {
            index += 1;
        } else {
            close_channel(index);
        }
    }
}

// ---------------------------------------------------------------------------
// SystemControl.

namespace {

// Closes a descriptor on every exit.
class DescriptorOwner {
public:
    explicit DescriptorOwner(int descriptor) noexcept : descriptor_(descriptor) {}
    DescriptorOwner(const DescriptorOwner&) = delete;
    DescriptorOwner& operator=(const DescriptorOwner&) = delete;
    ~DescriptorOwner() {
        if (descriptor_ >= 0) {
            (void)close(descriptor_);
        }
    }
    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

}  // namespace

stxcontrol::BootLogRead SystemControl::read_boot_log_end(std::span<char> destination) noexcept {
    const DescriptorOwner file(::open(stxboot::boot_log_path, O_RDONLY | O_CLOEXEC));
    if (file.descriptor() < 0) {
        const int error_number = errno;
        if (error_number == ENOENT) {
            return stxcontrol::BootLogRead::absent();
        }
        return stxcontrol::BootLogRead::failed(error_number);
    }
    struct stat status{};
    if (fstat(file.descriptor(), &status) != 0) {
        return stxcontrol::BootLogRead::failed(errno);
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        return stxcontrol::BootLogRead::failed(EFTYPE);
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(status.st_size);
    const std::uint64_t window = file_size < destination.size() ? file_size : destination.size();
    const std::uint64_t start = file_size - window;
    std::size_t total = 0;
    int interrupted = 0;
    // Each successful pread adds at least one byte; a short file (truncated
    // meanwhile) ends the loop at end of file.
    while (total < window) {
        const ssize_t got = pread(file.descriptor(), destination.data() + total,
                                  static_cast<std::size_t>(window) - total, static_cast<off_t>(start + total));
        if (got < 0) {
            const int error_number = errno;
            if (error_number == EINTR && interrupted < interrupted_retries_max) {
                interrupted += 1;
                continue;
            }
            return stxcontrol::BootLogRead::failed(error_number);
        }
        if (got == 0) {
            break;
        }
        total += static_cast<std::size_t>(got);
    }
    return stxcontrol::BootLogRead::text(total, start > 0u);
}

// ---------------------------------------------------------------------------
// SystemControl: the user configuration, the reboot, the host side's part.

namespace {

const stxstore::TurnPaths user_config_paths = {
    stxconfig::user_config_directory,
    {stxconfig::user_config_path_0, stxconfig::user_config_path_1},
};

}  // namespace

SystemControl::SystemControl() noexcept
    : reboot_requested_(false), targets_(nullptr), ledger_(), ledger_text_{}, turn_buffers_{} {}

stxcontrol::UserConfigRead SystemControl::read_user_config(std::span<char> destination) noexcept {
    const stxstore::TurnState state =
        stxstore::read_turns(user_config_paths, stxcontrol::config_text_max,
                             std::span<char>(turn_buffers_[0], sizeof(turn_buffers_[0])),
                             std::span<char>(turn_buffers_[1], sizeof(turn_buffers_[1])));
    if (!state.choice.newest.has_value()) {
        // Neither file holds a turn: absent, unless reading one failed
        // for another reason than its absence.
        const int error_number = state.errors[0] != 0 ? state.errors[0] : state.errors[1];
        if (error_number != 0) {
            return stxcontrol::UserConfigRead::failed(error_number);
        }
        return stxcontrol::UserConfigRead::absent();
    }
    const std::size_t newest = *state.choice.newest;
    const stxstore::TurnView& turn = *state.turns[newest];
    if (turn.body.size() > destination.size()) {
        return stxcontrol::UserConfigRead::failed(EFBIG);
    }
    for (std::size_t index = 0; index < turn.body.size(); index += 1) {
        destination[index] = turn.body[index];
    }
    const stxcontrol::ConfigSource source = newest == 0u ? stxcontrol::ConfigSource::file_0
                                                         : stxcontrol::ConfigSource::file_1;
    return stxcontrol::UserConfigRead::text(source, turn.sequence, turn.body.size());
}

stxcontrol::Status SystemControl::write_user_config(std::span<const char> text,
                                                    stxcontrol::ConfigWritten& written) noexcept {
    // Choose from the files as they are now, not as an earlier read saw
    // them.
    const stxstore::TurnState state =
        stxstore::read_turns(user_config_paths, stxcontrol::config_text_max,
                             std::span<char>(turn_buffers_[0], sizeof(turn_buffers_[0])),
                             std::span<char>(turn_buffers_[1], sizeof(turn_buffers_[1])));
    const stxstore::TurnWriteOutcome outcome = stxstore::write_turn(user_config_paths, state.choice, text, 0600u);
    const stxstore::TurnWritten* const done = outcome.as_written();
    if (done == nullptr) {
        return stxcontrol::Status::store_failed;
    }
    written.sequence = (*done).sequence;
    written.file_index = static_cast<std::uint8_t>((*done).file_index);
    return stxcontrol::Status::ok;
}

stxcontrol::IdsIssued SystemControl::issue_target_ids(const stxconfig::BootConfig& user,
                                                      const stxconfig::BootConfig& factory, std::span<const char> text,
                                                      std::span<char> out) noexcept {
    stxcontrol::IdsIssued answer{stxcontrol::IdsIssuedKind::failed, 0, 0, 0};
    // A ledger that cannot be read is never taken as empty.
    if (read_target_id_ledger(ledger_, std::span<char>(ledger_text_, sizeof(ledger_text_))) == LedgerRead::failed) {
        return answer;
    }
    const stxconfig::IdIssueOutcome outcome = stxconfig::issue_target_ids(user, factory, text, ledger_, out);
    switch (outcome.kind) {
    case stxconfig::IdIssueKind::issued:
        break;
    case stxconfig::IdIssueKind::refused:
        answer.kind = stxcontrol::IdsIssuedKind::not_issued;
        answer.line = outcome.line;
        answer.column = outcome.column;
        return answer;
    case stxconfig::IdIssueKind::too_large:
        answer.kind = stxcontrol::IdsIssuedKind::too_large;
        return answer;
    case stxconfig::IdIssueKind::ledger_full:
    default:
        return answer;
    }
    // The ledger first: an id issued for a write that then fails is spent.
    // Unchanged, it is not written (a configuration without targets needs
    // no /data).
    if (outcome.ledger_changed &&
        (!ensure_target_id_directory() ||
         !write_target_id_ledger(ledger_, std::span<char>(ledger_text_, sizeof(ledger_text_))))) {
        return answer;
    }
    answer.kind = stxcontrol::IdsIssuedKind::issued;
    answer.length = outcome.length;
    return answer;
}

void SystemControl::boot_target_ids(const stxconfig::BootConfig& running, int console) noexcept {
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR target-ids ");
    if (!ensure_target_id_directory()) {
        stxruntime::append_text(line, "folder-unavailable\n");
        stxruntime::write_line(console, line);
        return;
    }
    const LedgerRead read = read_target_id_ledger(ledger_, std::span<char>(ledger_text_, sizeof(ledger_text_)));
    if (read == LedgerRead::failed) {
        stxruntime::append_text(line, "ledger-unreadable\n");
        stxruntime::write_line(console, line);
        return;
    }
    static stxconfig::BootIdPlan plan;
    stxconfig::plan_boot_ids(running, ledger_, plan);
    std::size_t removed = 0;
    std::size_t failed = 0;
    for (std::size_t index = 0; index < plan.removal_count; index += 1) {
        const stxconfig::LedgerEntry& entry = ledger_.entry(plan.removals[index]);
        char folder[stxconfig::line_length_max + 1u] = {};
        std::memcpy(folder, entry.folder, entry.folder_length);
        // Tried once: the entry is removed whatever the result, so a later
        // target in the same folder never loses its files to a retry.
        if (remove_target_folder(folder)) {
            removed += 1;
        } else {
            failed += 1;
        }
        ledger_.set_state(plan.removals[index], stxconfig::LedgerState::removed);
    }
    const bool written = (!plan.changed && plan.removal_count == 0u) ||
                         write_target_id_ledger(ledger_, std::span<char>(ledger_text_, sizeof(ledger_text_)));
    stxruntime::append_text(line, "entries=");
    stxruntime::append_decimal(line, ledger_.count());
    stxruntime::append_text(line, " folders-removed=");
    stxruntime::append_decimal(line, removed);
    stxruntime::append_text(line, " remove-failed=");
    stxruntime::append_decimal(line, failed);
    stxruntime::append_text(line, " conflicts=");
    stxruntime::append_decimal(line, plan.conflicts);
    stxruntime::append_text(line, written ? "\n" : " write-failed\n");
    stxruntime::write_line(console, line);
}

void SystemControl::request_reboot() noexcept {
    reboot_requested_ = true;
}

bool SystemControl::reboot_requested() const noexcept {
    return reboot_requested_;
}

// The host side's part: the targets layer (targets.h) carries the program
// commands out; program_answers.h says what they answer.
void SystemControl::set_targets(stxtargets::Targets* targets) noexcept {
    targets_ = targets;
}

namespace {

// Whether path names a regular file (not a link): the staged package or
// image the web page stored (runtime/web/api.h, begin_upload).
[[nodiscard]] bool staged_file_present(const char* path) noexcept {
    struct stat status{};
    return lstat(path, &status) == 0 && S_ISREG(status.st_mode);
}

}  // namespace

stxcontrol::ProgramAnswer SystemControl::answer_for(const stxcontrol::ServiceName& target,
                                                    stxcontrol::Status status) const noexcept {
    stxcontrol::ProgramAnswer answer = stxcontrol::program_answer(status);
    if (targets_ == nullptr) {
        return answer;
    }
    char name[stxcontrol::service_name_bytes_max + 1] = {};
    stxcontrol::service_name_c_string(target, name);
    const std::optional<stxtargets::TargetStatus> found = (*targets_).status(name);
    if (found.has_value()) {
        answer.record = program_record(target, *found);
    }
    return answer;
}

stxcontrol::ProgramAnswer SystemControl::program_state(const stxcontrol::ServiceName& target) noexcept {
    if (targets_ == nullptr) {
        return stxcontrol::program_answer(stxcontrol::Status::unavailable);
    }
    const stxcontrol::ProgramAnswer answer = answer_for(target, stxcontrol::Status::ok);
    if (!answer.record.has_value()) {
        return stxcontrol::program_answer(stxcontrol::Status::unknown_service);
    }
    return answer;
}

stxcontrol::ProgramAnswer SystemControl::program_install(const stxcontrol::ServiceName& target,
                                                         const stxcontrol::InstallSource& source) noexcept {
    if (targets_ == nullptr) {
        return stxcontrol::program_answer(stxcontrol::Status::unavailable);
    }
    char name[stxcontrol::service_name_bytes_max + 1] = {};
    stxcontrol::service_name_c_string(target, name);
    const std::uint64_t now_ms = stxconfig::monotonic_milliseconds();
    if (source.kind == stxcontrol::InstallKind::staged) {
        if (!(*targets_).status(name).has_value()) {
            return stxcontrol::program_answer(stxcontrol::Status::unknown_service);
        }
        if (!staged_file_present(stxcontrol::staged_package_path)) {
            return answer_for(target, stxcontrol::Status::source_unavailable);
        }
        const stxtargets::RequestResult result =
            (*targets_).request_install(name, stxcontrol::staged_package_path, now_ms);
        return answer_for(target, request_status(result, ProgramCommand::install_staged));
    }
    if (source.kind != stxcontrol::InstallKind::url || source.url.size() > stxcontrol::install_url_bytes_max) {
        return stxcontrol::program_answer(stxcontrol::Status::malformed);
    }
    char url[stxcontrol::install_url_bytes_max + 1] = {};
    for (std::size_t index = 0; index < source.url.size(); index += 1) {
        url[index] = source.url[index];
    }
    url[source.url.size()] = '\0';
    const stxtargets::RequestResult result = (*targets_).request_update_from(name, url, now_ms);
    return answer_for(target, request_status(result, ProgramCommand::install_url));
}

stxcontrol::ProgramAnswer SystemControl::program_rollback(const stxcontrol::ServiceName& target) noexcept {
    if (targets_ == nullptr) {
        return stxcontrol::program_answer(stxcontrol::Status::unavailable);
    }
    char name[stxcontrol::service_name_bytes_max + 1] = {};
    stxcontrol::service_name_c_string(target, name);
    const stxtargets::RequestResult result = (*targets_).rollback(name, stxconfig::monotonic_milliseconds());
    return answer_for(target, request_status(result, ProgramCommand::rollback));
}

stxcontrol::ProgramAnswer SystemControl::program_check(const stxcontrol::ServiceName& target) noexcept {
    if (targets_ == nullptr) {
        return stxcontrol::program_answer(stxcontrol::Status::unavailable);
    }
    char name[stxcontrol::service_name_bytes_max + 1] = {};
    stxcontrol::service_name_c_string(target, name);
    const stxtargets::RequestResult result = (*targets_).request_update(name, stxconfig::monotonic_milliseconds());
    return answer_for(target, request_status(result, ProgramCommand::check));
}

stxcontrol::Status SystemControl::system_update() noexcept {
    if (targets_ == nullptr) {
        return stxcontrol::Status::unavailable;
    }
    if (!staged_file_present(stxcontrol::staged_system_path)) {
        return stxcontrol::Status::source_unavailable;
    }
    const stxtargets::RequestResult result =
        (*targets_).request_system_install(stxcontrol::staged_system_path, stxconfig::monotonic_milliseconds());
    return request_status(result, ProgramCommand::system_update);
}

stxcontrol::OperationRecord SystemControl::operation_state() noexcept {
    if (targets_ == nullptr) {
        return stxcontrol::operation_none();
    }
    return operation_record((*targets_).last_operation(), stxconfig::monotonic_milliseconds());
}

stxcontrol::BootLogRead SystemControl::read_target_log(const stxcontrol::ServiceName& target,
                                                       std::span<char> destination) noexcept {
    if (targets_ == nullptr) {
        return stxcontrol::BootLogRead::absent();
    }
    char name[stxcontrol::service_name_bytes_max + 1] = {};
    stxcontrol::service_name_c_string(target, name);
    // What is waiting in the pipe first, so the answer is current.
    (*targets_).drain_logs();
    const std::optional<stxtargets::TargetLogEnd> end = (*targets_).log_end(name, destination);
    if (!end.has_value() || ((*end).size == 0u && !(*end).began_inside)) {
        return stxcontrol::BootLogRead::absent();
    }
    return stxcontrol::BootLogRead::text((*end).size, (*end).began_inside);
}

}  // namespace stxsupervisor
