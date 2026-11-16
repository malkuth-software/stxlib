// test_program_answers.cpp: host tests of what the program commands answer
// (program_answers.h).
//
// Exhaustive over their domains:
// - request_status: every RequestResult x every ProgramCommand (25), each
//   against a written table;
// - job_status: every 16-bit wait status (65536) x install and look, against
//   a reference read from the wait-status encoding FreeBSD and the host
//   share (low 7 bits 0: exited, code in bits 8..15);
// - program_record: all 11 program states and the unreadable state x no
//   process, slot A, slot B running x versions {0, 7, 4294967295} for each
//   slot (324), against a written table of each state's slot to run, good
//   slot and trial.
// Over a stated set (sampled, not the whole domain): operation_record for
// every kind x install x running x 7 wait statuses x 4 time triples x 3
// target names (1344), each checked against the expected fields and
// operation_record_valid.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include "../../package/stxpkg_exit.h"
#include "../../supervisor/config/test/test_support.h"
#include "../program_answers.h"

namespace {

using stxcontrol::Status;
using stxpackage::ProgramState;
using stxpackage::Slot;
using stxsupervisor::ProgramCommand;
using stxtargets::RequestResult;
using stxtest::check;

static_assert(stxpackage::exit_done == 0 && stxpackage::exit_not_newer == 3 && stxpackage::exit_refused == 4 &&
              stxpackage::exit_io == 5 && stxpackage::exit_nothing_newer == 6 && stxpackage::exit_fetch_failed == 7);

void test_request_status() {
    const RequestResult results[] = {RequestResult::accepted, RequestResult::unknown_target, RequestResult::busy,
                                     RequestResult::nothing_to_do, RequestResult::failed};
    const ProgramCommand commands[] = {ProgramCommand::install_staged, ProgramCommand::install_url,
                                       ProgramCommand::rollback, ProgramCommand::check,
                                       ProgramCommand::system_update};
    // Rows: results; columns: commands.
    const Status expected[5][5] = {
        {Status::ok, Status::ok, Status::ok, Status::ok, Status::ok},
        {Status::unknown_service, Status::unknown_service, Status::unknown_service, Status::unknown_service,
         Status::unknown_service},
        {Status::busy, Status::busy, Status::busy, Status::busy, Status::busy},
        {Status::unavailable, Status::source_unavailable, Status::unavailable, Status::unavailable,
         Status::unavailable},
        {Status::unavailable, Status::unavailable, Status::unavailable, Status::unavailable, Status::unavailable},
    };
    std::size_t wrong = 0;
    for (std::size_t row = 0; row < 5u; row += 1) {
        for (std::size_t column = 0; column < 5u; column += 1) {
            wrong += stxsupervisor::request_status(results[row], commands[column]) == expected[row][column] ? 0u : 1u;
        }
    }
    check(wrong == 0u, "request_status", "every result and command");
}

[[nodiscard]] Status expected_job(bool install, int wait_status) {
    if ((wait_status & 0x7f) != 0) {
        return Status::unavailable;
    }
    const int code = (wait_status >> 8) & 0xff;
    switch (code) {
    case 0: return Status::ok;
    case 3:
    case 6: return install ? Status::unavailable : Status::ok;
    case 4: return Status::signature_refused;
    case 7: return Status::source_unavailable;
    default: break;
    }
    return Status::unavailable;
}

void test_job_status() {
    std::size_t wrong = 0;
    std::size_t cases = 0;
    for (int wait_status = 0; wait_status < 65536; wait_status += 1) {
        for (int install = 0; install < 2; install += 1) {
            wrong += stxsupervisor::job_status(install == 1, wait_status) == expected_job(install == 1, wait_status)
                         ? 0u
                         : 1u;
            cases += 1;
        }
    }
    check(cases == 131072u && wrong == 0u, "job_status", "every 16-bit wait status, install and look");
}

struct StateRow {
    std::optional<ProgramState> state;
    // What the state runs next, its good slot, its trial slot.
    std::optional<Slot> runs;
    std::optional<Slot> good;
    std::optional<Slot> trial;
    std::uint8_t failures;
};

[[nodiscard]] stxcontrol::SlotCode code_of(std::optional<Slot> slot) {
    if (!slot.has_value()) {
        return stxcontrol::SlotCode::none;
    }
    return *slot == Slot::a ? stxcontrol::SlotCode::a : stxcontrol::SlotCode::b;
}

[[nodiscard]] bool version_is(const stxcontrol::ProgramVersion& version, const char* text) {
    const std::size_t length = std::strlen(text);
    return version.length == length && std::memcmp(version.bytes, text, length) == 0;
}

void test_program_record() {
    const StateRow rows[] = {
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt, 0},
        {ProgramState::none(), std::nullopt, std::nullopt, std::nullopt, 0},
        {ProgramState::good_only(Slot::a), Slot::a, Slot::a, std::nullopt, 0},
        {ProgramState::good_only(Slot::b), Slot::b, Slot::b, std::nullopt, 0},
        {ProgramState::good_with_trial(Slot::a, 0), Slot::b, Slot::a, Slot::b, 0},
        {ProgramState::good_with_trial(Slot::a, 1), Slot::b, Slot::a, Slot::b, 1},
        {ProgramState::good_with_trial(Slot::a, 2), Slot::b, Slot::a, Slot::b, 2},
        {ProgramState::good_with_trial(Slot::b, 0), Slot::a, Slot::b, Slot::a, 0},
        {ProgramState::good_with_trial(Slot::b, 1), Slot::a, Slot::b, Slot::a, 1},
        {ProgramState::good_with_trial(Slot::b, 2), Slot::a, Slot::b, Slot::a, 2},
        {ProgramState::good_with_previous(Slot::a), Slot::a, Slot::a, std::nullopt, 0},
        {ProgramState::good_with_previous(Slot::b), Slot::b, Slot::b, std::nullopt, 0},
    };
    const std::optional<Slot> running[] = {std::nullopt, Slot::a, Slot::b};
    const std::uint32_t versions[] = {0u, 7u, 4294967295u};
    const char* const version_texts[] = {"", "7", "4294967295"};
    const std::optional<stxcontrol::ServiceName> name =
        stxcontrol::service_name_of(std::span<const char>("player", 6));
    check(name.has_value(), "program_record", "the test's name");
    std::size_t cases = 0;
    std::size_t wrong = 0;
    for (std::size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row += 1) {
        for (std::size_t run = 0; run < 3u; run += 1) {
            for (std::size_t a = 0; a < 3u; a += 1) {
                for (std::size_t b = 0; b < 3u; b += 1) {
                    stxtargets::TargetStatus status{};
                    status.service_index = 2;
                    status.stored = std::nullopt;
                    if (rows[row].state.has_value()) {
                        status.stored = stxpackage::StoredState{*rows[row].state, 0};
                    }
                    status.running_slot = running[run];
                    status.version_a = versions[a];
                    status.version_b = versions[b];
                    const stxcontrol::ProgramRecord record = stxsupervisor::program_record(*name, status);
                    const std::optional<Slot> active = running[run].has_value() ? running[run] : rows[row].runs;
                    const bool on_trial = rows[row].trial.has_value() && active == rows[row].trial;
                    const bool good = stxcontrol::service_names_equal(record.name, *name) &&
                                      record.active == code_of(active) && record.last_good == code_of(rows[row].good) &&
                                      record.on_trial == on_trial && !record.update_available &&
                                      record.failed_starts == rows[row].failures &&
                                      version_is(record.version_a, version_texts[a]) &&
                                      version_is(record.version_b, version_texts[b]);
                    wrong += good ? 0u : 1u;
                    cases += 1;
                }
            }
        }
    }
    check(cases == 324u && wrong == 0u, "program_record", "every state, running slot and version set");
}

void test_operation_record() {
    const stxtargets::OperationKind kinds[] = {stxtargets::OperationKind::none,
                                               stxtargets::OperationKind::system_update,
                                               stxtargets::OperationKind::program_install,
                                               stxtargets::OperationKind::program_check};
    const stxcontrol::OperationKind wire[] = {stxcontrol::OperationKind::none, stxcontrol::OperationKind::system_update,
                                              stxcontrol::OperationKind::program_install,
                                              stxcontrol::OperationKind::program_check};
    const int waits[] = {0, 3 << 8, 4 << 8, 5 << 8, 6 << 8, 7 << 8, 9};
    // started, ended, now; and the elapsed expected while running and when
    // finished.
    const std::uint64_t times[4][5] = {
        {10u, 10u, 10u, 0u, 0u},
        {10u, 25u, 40u, 30u, 15u},
        {0u, 4294967300u, 8589934592u, 4294967295u, 4294967295u},
        {100u, 100u, 50u, 0u, 0u},
    };
    const char* const targets[] = {"player", nullptr, "Bad"};
    std::size_t cases = 0;
    std::size_t wrong = 0;
    for (std::size_t kind = 0; kind < 4u; kind += 1) {
        for (int install = 0; install < 2; install += 1) {
            for (int running = 0; running < 2; running += 1) {
                for (std::size_t wait = 0; wait < sizeof(waits) / sizeof(waits[0]); wait += 1) {
                    for (std::size_t time = 0; time < 4u; time += 1) {
                        for (std::size_t target = 0; target < 3u; target += 1) {
                            const stxtargets::OperationStatus status{kinds[kind], install == 1, running == 1,
                                                                     waits[wait], times[time][0], times[time][1],
                                                                     targets[target]};
                            const stxcontrol::OperationRecord record =
                                stxsupervisor::operation_record(status, times[time][2]);
                            const bool program = kind >= 2u;
                            const bool named = target == 0u;
                            bool good = stxcontrol::operation_record_valid(record);
                            if (kind == 0u || (program && !named)) {
                                good = good && record.kind == stxcontrol::OperationKind::none &&
                                       record.state == stxcontrol::OperationState::idle;
                            } else {
                                const Status result = running == 1 ? Status::ok : expected_job(install == 1, waits[wait]);
                                const stxcontrol::OperationState state =
                                    running == 1 ? stxcontrol::OperationState::running
                                                 : (result == Status::ok ? stxcontrol::OperationState::succeeded
                                                                         : stxcontrol::OperationState::failed);
                                good = good && record.kind == wire[kind] && record.state == state &&
                                       record.result == result &&
                                       record.elapsed_ms == times[time][running == 1 ? 3u : 4u] &&
                                       record.name.length == (program ? 6u : 0u);
                            }
                            wrong += good ? 0u : 1u;
                            cases += 1;
                        }
                    }
                }
            }
        }
    }
    check(cases == 1344u && wrong == 0u, "operation_record", "every kind, flag, wait, time and name in the set");
}

}  // namespace

int main() {
    test_request_status();
    test_job_status();
    test_program_record();
    test_operation_record();
    return stxtest::finish("test_program_answers");
}
