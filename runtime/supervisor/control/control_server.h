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
//      operate for start, stop and restart (config/boot_config.h,
//      ControlAccess); otherwise not_permitted.
//   4. A service may not stop itself (status refused_self): the web
//      interface would cut off the only way to start it again. It may
//      restart itself.
// The authority is read from the registry and the configuration at each
// request, never cached, so it ends the moment the service stops running.
//
// Pure code: no system call; the boot log is read through ControlSystem.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_SERVER_H
#define STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_SERVER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "../boot_slots.h"
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

// What the server asks of the system.
class ControlSystem {
public:
    ControlSystem() noexcept = default;
    ControlSystem(const ControlSystem&) = delete;
    ControlSystem& operator=(const ControlSystem&) = delete;

    // Reads the boot log's last destination.size() bytes (or all of it,
    // when shorter) into the front of destination.
    [[nodiscard]] virtual BootLogRead read_boot_log_end(std::span<char> destination) noexcept = 0;

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

class ControlServer {
public:
    // Borrows the registry, the actions it spawns and signals through, and
    // the system; all three must outlive the server.
    ControlServer(stxconfig::ServiceRegistry& registry, stxconfig::ServiceActions& actions,
                  ControlSystem& system) noexcept;
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

    stxconfig::ServiceRegistry& registry_;
    stxconfig::ServiceActions& actions_;
    ControlSystem& system_;
    // The payload is built here, then copied behind the header.
    std::uint8_t payload_[response_payload_max];
};

}  // namespace stxcontrol

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_SERVER_H
