// control_server.h: the supervisor's side of the control channel, its pure
// part (C2; design dev/notes/planning/stages/C2-web.md, section 4).
//
// One request frame in, one response frame out. The server decides who
// may do what, carries out the request on the services registry, and
// encodes the answer; control_channels.h moves the packets.
//
// Authority, checked in this order for every request:
//   1. The frame decodes (control_protocol.h); otherwise status malformed.
//   2. The channel's process is a running service: the registry's entry
//      whose running pid is the channel's. A stopping, waiting or exited
//      service has no authority left; status not_permitted.
//   3. That service's definition grants control: read for queries,
//      operate for every command that changes something (start, stop,
//      restart, config_write, reboot, the program commands but
//      program_state, system_update: command_operates; config/boot_config.h,
//      ControlAccess); otherwise not_permitted.
//   4. A service may not stop itself (status refused_self): the web
//      interface would cut off the only way to start it again. It may
//      restart itself.
// The authority is read from the registry and the configuration at each
// request, never cached, so it ends the moment the service stops running.
//
// config_write is checked here before anything is written: the text must
// parse (config/boot_config.h) and keep the user configuration's rules
// against the factory configuration the supervisor booted with
// (config/user_config.h); otherwise status config_refused with a
// ConfigRefusal. Only then does ControlSystem write the next turn.
//
// The program and system-update commands are passed to ControlSystem as
// they are; the host side carries them out (sb2's C2 note) and answers.
//
// Pure code: no system call; files, the reboot and the host side's
// operations go through ControlSystem.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_SERVER_H
#define STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_SERVER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "../boot_slots.h"
#include "../config/boot_config.h"
#include "../config/service_registry.h"
#include "control_protocol.h"

namespace stxcontrol {

// What the supervisor knows about itself and this boot, at the time of the
// request.
struct ControlFacts {
    std::uint64_t uptime_ms;
    // stxinit's restart count for this supervisor start; nothing when
    // unknown.
    std::optional<std::uint64_t> restart_count;
    bool shim_registered;
    bool init_done;
    bool failsafe_taken;
    bool boot_marked_good;
    stxboot::BootFacts boot;
};

// The boot log's end, as the system read it.
struct BootLogText {
    // Bytes placed at the front of the destination.
    std::size_t size;
    // Whether the read began after the file's first byte (the window did
    // not reach the file's start).
    bool began_inside;
};
struct BootLogAbsent {
};
struct BootLogFailed {
    int error_number;
};

class BootLogRead {
public:
    [[nodiscard]] static BootLogRead text(std::size_t size, bool began_inside) noexcept;
    [[nodiscard]] static BootLogRead absent() noexcept;
    [[nodiscard]] static BootLogRead failed(int error_number) noexcept;
    [[nodiscard]] const BootLogText* as_text() const noexcept;
    [[nodiscard]] bool is_absent() const noexcept;
    [[nodiscard]] const BootLogFailed* as_failed() const noexcept;

private:
    using Alternatives = std::variant<BootLogText, BootLogAbsent, BootLogFailed>;
    explicit BootLogRead(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// The user configuration as the system read it.
struct UserConfigText {
    // file_0 or file_1.
    ConfigSource source;
    std::uint32_t sequence;
    // Bytes placed at the front of the destination.
    std::size_t size;
};
struct UserConfigAbsent {
};
struct UserConfigFailed {
    int error_number;
};

class UserConfigRead {
public:
    [[nodiscard]] static UserConfigRead text(ConfigSource source, std::uint32_t sequence, std::size_t size) noexcept;
    [[nodiscard]] static UserConfigRead absent() noexcept;
    [[nodiscard]] static UserConfigRead failed(int error_number) noexcept;
    [[nodiscard]] const UserConfigText* as_text() const noexcept;
    [[nodiscard]] bool is_absent() const noexcept;
    [[nodiscard]] const UserConfigFailed* as_failed() const noexcept;

private:
    using Alternatives = std::variant<UserConfigText, UserConfigAbsent, UserConfigFailed>;
    explicit UserConfigRead(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// A program command's answer: its status and, when the target has a
// program, its record (which must name the target asked about).
struct ProgramAnswer {
    Status status;
    std::optional<ProgramRecord> record;
};
[[nodiscard]] ProgramAnswer program_answer(Status status) noexcept;

// What the server asks of the system.
class ControlSystem {
public:
    ControlSystem() noexcept = default;
    ControlSystem(const ControlSystem&) = delete;
    ControlSystem& operator=(const ControlSystem&) = delete;

    // Reads the boot log's last destination.size() bytes (or all of it,
    // when shorter) into the front of destination.
    [[nodiscard]] virtual BootLogRead read_boot_log_end(std::span<char> destination) noexcept = 0;

    // The user configuration's newest valid turn (store/turn_file.h): its
    // text into the front of destination (config_text_max bytes). Absent
    // when neither file holds one. (Which turn the next boot uses, the
    // newest that parses and keeps the rules, then the older, is the
    // boot's choice: user_config.h.)
    [[nodiscard]] virtual UserConfigRead read_user_config(std::span<char> destination) noexcept = 0;
    // Writes text, already checked, as the next turn. ok with the record,
    // or store_failed.
    [[nodiscard]] virtual Status write_user_config(std::span<const char> text, ConfigWritten& written) noexcept = 0;

    // Stop every service, then reboot, after this request's response has
    // been sent (the supervisor's loop does it on its next round).
    virtual void request_reboot() noexcept = 0;

    // The host side's targets, packages and updater (sb2's C2 note, K5 and
    // K11 to K14). Until that side implements them they answer
    // unsupported. A long operation (install, check, update) answers ok
    // when it has started; operation_state follows it.
    [[nodiscard]] virtual ProgramAnswer program_state(const ServiceName& target) noexcept = 0;
    [[nodiscard]] virtual ProgramAnswer program_install(const ServiceName& target,
                                                        const InstallSource& source) noexcept = 0;
    [[nodiscard]] virtual ProgramAnswer program_rollback(const ServiceName& target) noexcept = 0;
    [[nodiscard]] virtual ProgramAnswer program_check(const ServiceName& target) noexcept = 0;
    [[nodiscard]] virtual Status system_update() noexcept = 0;
    [[nodiscard]] virtual OperationRecord operation_state() noexcept = 0;

    // The end of a target's own output (K17): its last destination.size()
    // bytes, or all of it when shorter, into the front of destination;
    // began_inside when older output was dropped. Absent when the service
    // is no target or has written nothing yet.
    [[nodiscard]] virtual BootLogRead read_target_log(const ServiceName& target, std::span<char> destination) noexcept = 0;

protected:
    ~ControlSystem() = default;
};

// The part of a window of a file's end that holds whole lines: everything
// when the window starts at the file's start; otherwise what follows the
// window's first line feed (nothing when it has none). A borrow of window.
[[nodiscard]] std::span<const char> whole_lines_of_end(std::span<const char> window, bool began_inside) noexcept;

// The last good slot as this boot knows it, without reading the ESP's slot
// file (appliance/disk/LAYOUT.md): the booted slot when this boot was
// marked good, is a fallback (the loader went back to the good slot), or
// had no try pending (tries 0: the loader booted good); otherwise (a trial
// boot of the try slot) the other slot. Nothing without a booted slot.
[[nodiscard]] std::optional<stxboot::Slot> last_good_slot(const stxboot::BootFacts& boot, bool marked_good) noexcept;

// What one request did, for the supervisor's console line.
struct ControlHandled {
    // The response frame's size in the response buffer (never 0).
    std::size_t response_size;
    // The decoded command, or nothing for a malformed frame.
    std::optional<Command> command;
    Status status;
    // The requesting service's index in the registry, when it is one.
    std::optional<std::size_t> requester;
};

// Large (a BootConfig to check written configurations in, about 50 KiB):
// the caller owns one, with static storage.
class ControlServer {
public:
    // Borrows the registry, the actions it spawns and signals through, the
    // system, and the factory configuration (what config_write checks
    // target names against); all must outlive the server.
    ControlServer(stxconfig::ServiceRegistry& registry, stxconfig::ServiceActions& actions, ControlSystem& system,
                  const stxconfig::BootConfig& factory) noexcept;
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Handles one request frame received on the channel of process
    // requester_pid. response must hold response_frame_bytes_max bytes
    // (asserted); frame and response must not overlap.
    [[nodiscard]] ControlHandled handle(std::span<const std::uint8_t> frame, int requester_pid,
                                        const ControlFacts& facts, std::uint64_t now_ms,
                                        std::span<std::uint8_t> response) noexcept;

private:
    [[nodiscard]] std::size_t encode_record_of(std::size_t index, std::uint64_t now_ms,
                                               std::span<std::uint8_t> out) const noexcept;
    // config_read and config_write: the status, with the payload's size.
    [[nodiscard]] Status read_config(std::size_t& payload_size) noexcept;
    [[nodiscard]] Status write_config(std::span<const std::uint8_t> text, std::size_t& payload_size) noexcept;
    // The program commands.
    [[nodiscard]] Status program(const Request& request, std::size_t& payload_size) noexcept;

    stxconfig::ServiceRegistry& registry_;
    stxconfig::ServiceActions& actions_;
    ControlSystem& system_;
    const stxconfig::BootConfig& factory_;
    // Whether a user configuration was written since this supervisor
    // started (reported with config_read).
    bool written_since_start_;
    // The payload is built here, then copied behind the header.
    std::uint8_t payload_[response_payload_max];
    stxconfig::BootConfig check_;
};

}  // namespace stxcontrol

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_SERVER_H
