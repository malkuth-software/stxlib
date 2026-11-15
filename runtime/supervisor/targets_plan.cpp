// targets_plan.cpp: see targets_plan.h.

#include "targets_plan.h"

#include "../package/stxpkg_exit.h"
#include "config/service_registry.h"

namespace stxtargets {

ExitKind exit_kind(int wait_status, std::uint64_t ran_ms, std::uint32_t healthy_after_s) noexcept {
    if (stxconfig::wait_status_is_exit(wait_status, stxconfig::package_refused_exit_status)) {
        return ExitKind::refused;
    }
    const std::uint64_t healthy_after_ms = static_cast<std::uint64_t>(healthy_after_s) * 1000u;
    return ran_ms < healthy_after_ms ? ExitKind::early : ExitKind::after_healthy;
}

stxpackage::SlotTransition after_exit(const stxpackage::ProgramState& state, stxpackage::Slot ran,
                                      ExitKind kind) noexcept {
    switch (kind) {
    case ExitKind::refused:
        return stxpackage::after_refused(state, ran);
    case ExitKind::early:
        return stxpackage::after_early_exit(state, ran);
    case ExitKind::after_healthy:
        // A trial that ran its healthy time was kept by the health tick
        // already; an end after that changes nothing.
        return stxpackage::SlotTransition{state, stxpackage::SlotNote::unchanged};
    default:
        break;
    }
    // Fallback: a kind outside the enumeration changes nothing.
    return stxpackage::SlotTransition{state, stxpackage::SlotNote::unchanged};
}

JobOutcome job_outcome(int wait_status) noexcept {
    if (stxconfig::wait_status_is_exit(wait_status, stxpackage::exit_done)) {
        return JobOutcome::installed;
    }
    if (stxconfig::wait_status_is_exit(wait_status, stxpackage::exit_not_newer) ||
        stxconfig::wait_status_is_exit(wait_status, stxpackage::exit_nothing_newer)) {
        return JobOutcome::nothing_newer;
    }
    if (stxconfig::wait_status_is_exit(wait_status, stxpackage::exit_refused)) {
        return JobOutcome::refused;
    }
    return JobOutcome::failed;
}

std::uint64_t next_check_ms(JobOutcome outcome, std::uint64_t now_ms, std::uint32_t interval_s) noexcept {
    const std::uint64_t interval_ms = static_cast<std::uint64_t>(interval_s) * 1000u;
    if (outcome == JobOutcome::failed) {
        return now_ms + (interval_ms < update_retry_max_ms ? interval_ms : update_retry_max_ms);
    }
    return now_ms + interval_ms;
}

}  // namespace stxtargets
