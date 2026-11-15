// program_answers.h: what the control channel's program commands answer,
// from what the targets layer says (sb2; appliance/C2_DESIGN.md, "The
// supervisor interface", item 7; control/control_protocol.h for the
// records).
//
// Pure code: no system call. SystemControl (control_channels.h) asks the
// targets layer (targets.h) and answers through these. Tested on the host
// (test/test_program_answers.cpp): request_status over every result and
// command, job_status over every 16-bit wait status, program_record over
// every program state and running slot, operation_record over every kind,
// install flag and a set of wait statuses and times (bounds in the test).

#ifndef STXLIB_RUNTIME_SUPERVISOR_PROGRAM_ANSWERS_H
#define STXLIB_RUNTIME_SUPERVISOR_PROGRAM_ANSWERS_H

#include <cstdint>

#include "control/control_protocol.h"
#include "targets.h"

namespace stxsupervisor {

// The command a request's result answers.
enum class ProgramCommand : std::uint8_t {
    install_staged,  // program_install from staged_package_path
    install_url,     // program_install from an https URL
    rollback,
    check,           // program_check
    system_update,   // system_update from staged_system_path
};

// accepted -> ok; unknown_target -> unknown_service; busy -> busy;
// nothing_to_do -> source_unavailable for a URL install (not an https URL
// the targets layer takes), unavailable otherwise (nothing to roll back
// to; no update-url; no inactive system slot, or a reboot already asked
// for); failed -> unavailable.
[[nodiscard]] stxcontrol::Status request_status(stxtargets::RequestResult result, ProgramCommand command) noexcept;

// How a finished stxpkg job ended (package/stxpkg_exit.h), as a Status:
// exit 0 -> ok; 3 (not newer) or 6 (nothing newer) -> ok for a look,
// unavailable for an install; 4 (refused) -> signature_refused; 7 (fetch
// failed) -> source_unavailable; any other exit, or a signal ->
// unavailable.
[[nodiscard]] stxcontrol::Status job_status(bool install, int wait_status) noexcept;

// The OperationRecord for the last operation at now_ms: none since boot;
// running (result ok, elapsed since it began); or finished (succeeded when
// job_status is ok, failed otherwise; elapsed is how long it took). Elapsed
// saturates at 2^32 - 1 ms; a clock that reads before the start gives 0.
// A program kind whose target name is not a valid name (it always is: the
// configuration's parser checks it) is answered as none, so the record
// always keeps operation_record_valid.
[[nodiscard]] stxcontrol::OperationRecord operation_record(const stxtargets::OperationStatus& status,
                                                           std::uint64_t now_ms) noexcept;

// The ProgramRecord of a target: active is the running slot, else the slot
// the state runs next, else none; last_good the state's good slot; on
// trial when a trial exists and the active slot is the trial's; failed
// starts the trial's early exits; versions in decimal (empty for 0, an
// empty or unreadable slot); update_available false (the supervisor does
// not look for updates without installing them). No stored state (the
// file is unreadable): active none unless a process runs, last_good none.
[[nodiscard]] stxcontrol::ProgramRecord program_record(const stxcontrol::ServiceName& name,
                                                       const stxtargets::TargetStatus& status) noexcept;

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_PROGRAM_ANSWERS_H
