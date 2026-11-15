// control_client.h: the web service's end of the supervisor's control
// channel (C2; the format is supervisor/control/control_protocol.h).
//
// The supervisor starts the web service with the channel as descriptor 3
// (config/system_actions.h, control_child_descriptor). The client checks
// that descriptor is an AF_UNIX SOCK_SEQPACKET socket before using it, and
// sends one request at a time: it waits for that request's response (by
// request_id) at most control_answer_timeout_ms, and drops any response
// that is not for it (a late answer to a request that timed out).
//
// When the supervisor's end closes (the supervisor exited), closed() turns
// true and stays so; stxweb then exits, and the supervisor that starts
// again starts a new web service (C2-web.md, owner question 4, built as
// recommended).
//
// System calls: fstat, getsockopt, fcntl, send, recv, poll, clock_gettime.

#ifndef STXLIB_RUNTIME_WEB_CONTROL_CLIENT_H
#define STXLIB_RUNTIME_WEB_CONTROL_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "../supervisor/control/control_protocol.h"

namespace stxweb {

// The supervisor answers within one turn of its loop; writing a
// configuration turn syncs a file on the SD card, which the 5 s allow.
constexpr int control_answer_timeout_ms = 5000;
// Stale responses dropped while waiting for one request's, at most.
constexpr int control_stale_responses_max = 4;

class ControlClient {
public:
    // descriptor: the channel, or -1 for none (every request then fails).
    explicit ControlClient(int descriptor) noexcept;
    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;

    // Descriptor 3 when it is a SOCK_SEQPACKET socket (made close-on-exec),
    // else -1.
    [[nodiscard]] static int inherited_descriptor() noexcept;

    // Sends request (its request_id replaced by the client's next) and
    // waits for its response. The view borrows the client's storage until
    // the next ask. Nothing when there is no channel, the request does not
    // encode, sending fails, the answer does not come in time, or it is
    // malformed.
    [[nodiscard]] std::optional<stxcontrol::ResponseView> ask(const stxcontrol::Request& request) noexcept;

    [[nodiscard]] int descriptor() const noexcept;
    // The supervisor's end has closed.
    [[nodiscard]] bool closed() const noexcept;
    // Reads nothing; notices a closed channel (call when poll reports the
    // descriptor readable outside ask).
    void check_closed() noexcept;

private:
    int descriptor_;
    bool closed_;
    std::uint32_t next_id_;
    std::uint8_t request_[stxcontrol::request_frame_bytes_max];
    // One more byte than a response can have: a longer packet arrives cut
    // and is refused.
    std::uint8_t response_[stxcontrol::response_frame_bytes_max + 1u];
};

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_CONTROL_CLIENT_H
