// targets_plan.h: the pure decisions of the supervisor's targets layer
// (C2, appliance/C2_DESIGN.md, "Targets" and "Updates"): what a target's
// exit means for its program's slots, and when the update service looks
// next. Tested on the host (test/test_targets_plan.cpp), exhaustively over
// its small domains.

#ifndef STXLIB_RUNTIME_SUPERVISOR_TARGETS_PLAN_H
#define STXLIB_RUNTIME_SUPERVISOR_TARGETS_PLAN_H

#include <cstdint>

#include "../package/program_slots.h"

namespace stxtargets {

// The update service's first look after the supervisor starts, and the
// longest wait before a retry after a failed fetch or a failed install.
constexpr std::uint64_t update_first_delay_ms = 300000;
constexpr std::uint64_t update_retry_max_ms = 3600000;

// How a target's process ended, as the registry reported it.
enum class ExitKind : std::uint8_t {
    refused,      // exit status package_refused_exit_status: the launcher refused the package
    early,        // any other end before healthy_after
    after_healthy,  // any other end at or after healthy_after
};

// wait_status as waitpid gives it, ran_ms how long the process ran.
[[nodiscard]] ExitKind exit_kind(int wait_status, std::uint64_t ran_ms, std::uint32_t healthy_after_s) noexcept;

// The program-slot transition an exit makes for the slot it ran on.
[[nodiscard]] stxpackage::SlotTransition after_exit(const stxpackage::ProgramState& state, stxpackage::Slot ran,
                                                    ExitKind kind) noexcept;

// What an update or install job's exit status means.
enum class JobOutcome : std::uint8_t {
    installed,      // 0
    nothing_newer,  // 3 or 6: look again after the interval
    refused,        // 4: the offered package is bad; look again after the interval
    failed,         // anything else (5, 7, a signal): retry sooner
};

[[nodiscard]] JobOutcome job_outcome(int wait_status) noexcept;

// When to look next after a job ended so, at now_ms, for a target whose
// interval is interval_s: installed, nothing_newer and refused wait the
// interval; failed waits min(interval, update_retry_max_ms).
[[nodiscard]] std::uint64_t next_check_ms(JobOutcome outcome, std::uint64_t now_ms, std::uint32_t interval_s) noexcept;

}  // namespace stxtargets

#endif  // STXLIB_RUNTIME_SUPERVISOR_TARGETS_PLAN_H
