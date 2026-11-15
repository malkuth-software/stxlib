// target_log.h: each target's own output, kept by the supervisor (K17,
// sb2; sc2's C2-web.md section 7 item 9; appliance/C2_DESIGN.md,
// "The supervisor interface", item 9).
//
// A target's standard output and standard error go into a pipe the
// supervisor reads: the write end is the child's descriptors 1 and 2
// (system_actions.cpp, become_target), the read end stays in the
// supervisor, non-blocking and close-on-exec. What arrives is kept in a
// ring of the newest log_ring_bytes per target (log_ring.h), in memory
// only: no file on any partition, so a target that writes without end
// cannot fill the disk, and nothing on disk can be swapped for a link. A
// line "--- start pid=<n> slot=<s>" marks each start. The web page reads
// the end through the control channel (target_log_tail). A supervisor
// restart loses the rings; a target whose pipe has no reader any more
// gets SIGPIPE on its next write (targets are not adopted by a restarted
// supervisor, appliance/C2_DESIGN.md, "Known limits").
//
// System calls: pipe2, fcntl, read, close. Hosted on FreeBSD's libc.

#ifndef STXLIB_RUNTIME_SUPERVISOR_TARGET_LOG_H
#define STXLIB_RUNTIME_SUPERVISOR_TARGET_LOG_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "config/boot_config.h"
#include "log_ring.h"

namespace stxtargets {

using TargetLogEnd = stxsupervisor::LogRingEnd;

// Reads per pipe per round: at most this many reads of log_ring_bytes, so
// one busy target cannot hold the loop (the rest waits for the next round;
// the pipe holds 64 KiB, and a full pipe only blocks that target's writes).
constexpr int target_log_reads_per_round = 16;

class TargetLogs final {
public:
    TargetLogs() noexcept;
    ~TargetLogs();
    TargetLogs(const TargetLogs&) = delete;
    TargetLogs& operator=(const TargetLogs&) = delete;

    // A new pipe for the service at index (its previous pipe, if any, is
    // read out and closed first): the write end, close-on-exec, for the
    // child to take as 1 and 2; -1 when no pipe can be made (the target
    // then writes to the console).
    [[nodiscard]] int open_output(std::size_t service_index) noexcept;
    // The start was abandoned: the read end is closed.
    void abandon(std::size_t service_index) noexcept;
    // A line into the service's ring (the start marker).
    void note(std::size_t service_index, std::span<const char> line) noexcept;
    // Reads what is waiting on every pipe; a pipe at end of file is closed.
    void drain() noexcept;
    [[nodiscard]] std::optional<TargetLogEnd> read_end(std::size_t service_index,
                                                       std::span<char> destination) const noexcept;
    // The open read ends into out; how many.
    [[nodiscard]] std::size_t descriptors(std::span<int> out) const noexcept;

private:
    void drain_one(std::size_t service_index) noexcept;

    stxsupervisor::LogRing rings_[stxconfig::services_max];
    int read_ends_[stxconfig::services_max];
};

}  // namespace stxtargets

#endif  // STXLIB_RUNTIME_SUPERVISOR_TARGET_LOG_H
