// program_answers.cpp: what the program commands answer
// (program_answers.h).

#include "program_answers.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include <sys/wait.h>

#include "../package/stxpkg_exit.h"

namespace stxsupervisor {

namespace {

[[nodiscard]] stxcontrol::SlotCode slot_code(std::optional<stxpackage::Slot> slot) noexcept {
    if (!slot.has_value()) {
        return stxcontrol::SlotCode::none;
    }
    switch (*slot) {
    case stxpackage::Slot::a: return stxcontrol::SlotCode::a;
    case stxpackage::Slot::b: return stxcontrol::SlotCode::b;
    default: break;
    }
    // Fallback: a slot this code does not know is none.
    return stxcontrol::SlotCode::none;
}

[[nodiscard]] stxcontrol::ProgramVersion version_text(std::uint32_t version) noexcept {
    stxcontrol::ProgramVersion text{};
    text.length = 0;
    if (version == 0u) {
        return text;
    }
    char reversed[10] = {};
    std::size_t count = 0;
    std::uint32_t rest = version;
    // At most 10 decimal digits in 32 bits.
    for (int digit = 0; digit < 10 && rest != 0u; digit += 1) {
        reversed[count] = static_cast<char>('0' + rest % 10u);
        rest /= 10u;
        count += 1;
    }
    for (std::size_t index = 0; index < count; index += 1) {
        text.bytes[index] = reversed[count - 1u - index];
    }
    text.length = static_cast<std::uint8_t>(count);
    return text;
}

[[nodiscard]] std::uint32_t saturated_elapsed(std::uint64_t from_ms, std::uint64_t to_ms) noexcept {
    if (to_ms <= from_ms) {
        return 0;
    }
    const std::uint64_t elapsed = to_ms - from_ms;
    return elapsed > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(elapsed);
}

}  // namespace

stxcontrol::Status request_status(stxtargets::RequestResult result, ProgramCommand command) noexcept {
    switch (result) {
    case stxtargets::RequestResult::accepted: return stxcontrol::Status::ok;
    case stxtargets::RequestResult::unknown_target: return stxcontrol::Status::unknown_service;
    case stxtargets::RequestResult::busy: return stxcontrol::Status::busy;
    case stxtargets::RequestResult::nothing_to_do:
        return command == ProgramCommand::install_url ? stxcontrol::Status::source_unavailable
                                                      : stxcontrol::Status::unavailable;
    case stxtargets::RequestResult::failed: return stxcontrol::Status::unavailable;
    default: break;
    }
    // Fallback: a result this code does not know.
    return stxcontrol::Status::unavailable;
}

stxcontrol::Status job_status(bool install, int wait_status) noexcept {
    if (!WIFEXITED(wait_status)) {
        return stxcontrol::Status::unavailable;
    }
    const int code = WEXITSTATUS(wait_status);
    if (code == stxpackage::exit_done) {
        return stxcontrol::Status::ok;
    }
    if (code == stxpackage::exit_not_newer || code == stxpackage::exit_nothing_newer) {
        return install ? stxcontrol::Status::unavailable : stxcontrol::Status::ok;
    }
    if (code == stxpackage::exit_refused) {
        return stxcontrol::Status::signature_refused;
    }
    if (code == stxpackage::exit_fetch_failed) {
        return stxcontrol::Status::source_unavailable;
    }
    return stxcontrol::Status::unavailable;
}

stxcontrol::OperationRecord operation_record(const stxtargets::OperationStatus& status,
                                             std::uint64_t now_ms) noexcept {
    stxcontrol::OperationRecord record = stxcontrol::operation_none();
    bool names_target = false;
    switch (status.kind) {
    case stxtargets::OperationKind::none: return record;
    case stxtargets::OperationKind::system_update: record.kind = stxcontrol::OperationKind::system_update; break;
    case stxtargets::OperationKind::program_install:
        record.kind = stxcontrol::OperationKind::program_install;
        names_target = true;
        break;
    case stxtargets::OperationKind::program_check:
        record.kind = stxcontrol::OperationKind::program_check;
        names_target = true;
        break;
    default: return record;  // Fallback: an unknown kind is none.
    }
    if (names_target) {
        const std::size_t length = status.target == nullptr ? 0u : strnlen(status.target, 64u);
        const std::optional<stxcontrol::ServiceName> name =
            stxcontrol::service_name_of(std::span<const char>(status.target == nullptr ? "" : status.target, length));
        if (!name.has_value()) {
            return stxcontrol::operation_none();
        }
        record.name = *name;
    }
    if (status.running) {
        record.state = stxcontrol::OperationState::running;
        record.result = stxcontrol::Status::ok;
        record.elapsed_ms = saturated_elapsed(status.started_ms, now_ms);
        return record;
    }
    record.result = job_status(status.install, status.wait_status);
    record.state = record.result == stxcontrol::Status::ok ? stxcontrol::OperationState::succeeded
                                                           : stxcontrol::OperationState::failed;
    record.elapsed_ms = saturated_elapsed(status.started_ms, status.ended_ms);
    return record;
}

stxcontrol::ProgramRecord program_record(const stxcontrol::ServiceName& name,
                                         const stxtargets::TargetStatus& status) noexcept {
    stxcontrol::ProgramRecord record{};
    record.name = name;
    record.active = stxcontrol::SlotCode::none;
    record.last_good = stxcontrol::SlotCode::none;
    record.on_trial = false;
    record.update_available = false;
    record.failed_starts = 0;
    std::optional<stxpackage::Slot> active = status.running_slot;
    if (status.stored.has_value()) {
        const stxpackage::ProgramState& state = (*status.stored).state;
        if (!active.has_value()) {
            active = stxpackage::slot_to_run(state);
        }
        const stxpackage::GoodOnly* const good_only = state.as_good_only();
        const stxpackage::GoodWithTrial* const trial = state.as_good_with_trial();
        const stxpackage::GoodWithPrevious* const previous = state.as_good_with_previous();
        if (good_only != nullptr) {
            record.last_good = slot_code((*good_only).good);
        } else if (trial != nullptr) {
            record.last_good = slot_code((*trial).good);
            record.failed_starts = (*trial).failures;
            record.on_trial = active.has_value() && *active == stxpackage::other_slot((*trial).good);
        } else if (previous != nullptr) {
            record.last_good = slot_code((*previous).good);
        }
    }
    record.active = slot_code(active);
    record.version_a = version_text(status.version_a);
    record.version_b = version_text(status.version_b);
    return record;
}

}  // namespace stxsupervisor
