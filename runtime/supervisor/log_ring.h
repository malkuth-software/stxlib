// log_ring.h: the last bytes a target wrote (K17, sb2; target_log.h):
// a ring of log_ring_bytes that keeps the newest output and counts what
// it dropped.
//
// Pure code: no system call, no allocation. Tested on the host
// (test/test_log_ring.cpp), exhaustively over append sequences (bounds in
// the test's header).

#ifndef STXLIB_RUNTIME_SUPERVISOR_LOG_RING_H
#define STXLIB_RUNTIME_SUPERVISOR_LOG_RING_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace stxsupervisor {

// The control channel's log text limit (control_protocol.h,
// boot_log_text_max, 3996) fits; a page shows the end, not all of it.
constexpr std::size_t log_ring_bytes = 4096;

struct LogRingEnd {
    // Bytes placed at the front of the destination.
    std::size_t size;
    // Whether older bytes than these exist (dropped by the ring, or left
    // out because the destination was smaller).
    bool began_inside;
};

class LogRing final {
public:
    LogRing() noexcept;

    // Appends bytes; the oldest are dropped beyond log_ring_bytes.
    void append(std::span<const char> bytes) noexcept;
    // The newest min(kept, destination.size()) bytes, oldest first.
    [[nodiscard]] LogRingEnd copy_end(std::span<char> destination) const noexcept;
    // Everything written so far (saturating).
    [[nodiscard]] std::uint64_t total() const noexcept;
    void clear() noexcept;

private:
    char bytes_[log_ring_bytes];
    // Where the next byte goes.
    std::size_t next_;
    // How many bytes the ring holds (at most log_ring_bytes).
    std::size_t kept_;
    std::uint64_t total_;
};

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_LOG_RING_H
