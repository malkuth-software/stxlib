// control_channels.cpp: the control channels on FreeBSD
// (control_channels.h).

#include "control_channels.h"

#include <cerrno>

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

SystemControl::SystemControl() noexcept : reboot_requested_(false), turn_buffers_{} {}

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

void SystemControl::request_reboot() noexcept {
    reboot_requested_ = true;
}

bool SystemControl::reboot_requested() const noexcept {
    return reboot_requested_;
}

// The host side's part (sb2's C2 note: targets K11, packages K12 and K13,
// downloads K14, the system updater K5) replaces these.
stxcontrol::ProgramAnswer SystemControl::program_state(const stxcontrol::ServiceName& target) noexcept {
    (void)target;
    return stxcontrol::program_answer(stxcontrol::Status::unsupported);
}

stxcontrol::ProgramAnswer SystemControl::program_install(const stxcontrol::ServiceName& target,
                                                         const stxcontrol::InstallSource& source) noexcept {
    (void)target;
    (void)source;
    return stxcontrol::program_answer(stxcontrol::Status::unsupported);
}

stxcontrol::ProgramAnswer SystemControl::program_rollback(const stxcontrol::ServiceName& target) noexcept {
    (void)target;
    return stxcontrol::program_answer(stxcontrol::Status::unsupported);
}

stxcontrol::ProgramAnswer SystemControl::program_check(const stxcontrol::ServiceName& target) noexcept {
    (void)target;
    return stxcontrol::program_answer(stxcontrol::Status::unsupported);
}

stxcontrol::Status SystemControl::system_update() noexcept {
    return stxcontrol::Status::unsupported;
}

stxcontrol::OperationRecord SystemControl::operation_state() noexcept {
    return stxcontrol::operation_none();
}

}  // namespace stxsupervisor
