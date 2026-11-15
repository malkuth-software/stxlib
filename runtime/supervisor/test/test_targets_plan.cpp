// test_targets_plan.cpp: host tests of the targets layer's pure decisions
// and of the device plan.
//
// Exhaustive over their domains:
// - exit_kind: every normal exit status 0..255 and every signal 1..127
//   (with and without the core flag), each at ran_ms h-1, h and h+1 for
//   healthy_after 1, 30 and 3600 s (and 0 ms): refused exactly for exit
//   125, else early exactly when ran_ms < healthy_after;
// - after_exit: all 11 slot states x both slots x the three kinds, against
//   program_slots' own transitions;
// - job_outcome: every exit status and signal;
// - next_check_ms: every outcome at intervals 300, 3599, 3600, 3601, 86400
//   and 2592000 s.
// The device plan: hide all first, the base patterns, then the foreground
// target's devices to its group with mode 0660, and no other target's.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../supervisor/config/boot_config.h"
#include "../../supervisor/config/test/test_support.h"
#include "../device_rules.h"
#include "../targets_plan.h"

namespace {

using stxpackage::ProgramState;
using stxpackage::Slot;
using stxtargets::ExitKind;
using stxtargets::JobOutcome;
using stxtest::check;

[[nodiscard]] int exited(int status) {
    return status << 8;
}

void test_exit_kind() {
    const std::uint32_t healthy[] = {1u, 30u, 3600u};
    std::size_t cases = 0;
    std::size_t failures = 0;
    for (const std::uint32_t healthy_after : healthy) {
        const std::uint64_t h = static_cast<std::uint64_t>(healthy_after) * 1000u;
        const std::uint64_t times[] = {0u, h - 1u, h, h + 1u};
        for (const std::uint64_t ran : times) {
            for (int status = 0; status < 256; status += 1) {
                const ExitKind kind = stxtargets::exit_kind(exited(status), ran, healthy_after);
                const ExitKind expected = status == 125 ? ExitKind::refused
                                                        : (ran < h ? ExitKind::early : ExitKind::after_healthy);
                failures += kind == expected ? 0u : 1u;
                cases += 1;
            }
            for (int signal_number = 1; signal_number < 128; signal_number += 1) {
                for (int core = 0; core < 2; core += 1) {
                    const int wait_status = signal_number | (core != 0 ? 0x80 : 0);
                    const ExitKind kind = stxtargets::exit_kind(wait_status, ran, healthy_after);
                    const ExitKind expected = ran < h ? ExitKind::early : ExitKind::after_healthy;
                    failures += kind == expected ? 0u : 1u;
                    cases += 1;
                }
            }
        }
    }
    std::printf("exit kinds: %zu cases, %zu failures\n", cases, failures);
    check(failures == 0u && cases == 3u * 4u * (256u + 254u), "exit kinds", "every status and time");
}

void test_after_exit() {
    const ProgramState states[] = {
        ProgramState::none(),
        ProgramState::good_only(Slot::a),
        ProgramState::good_only(Slot::b),
        ProgramState::good_with_trial(Slot::a, 0),
        ProgramState::good_with_trial(Slot::a, 1),
        ProgramState::good_with_trial(Slot::a, 2),
        ProgramState::good_with_trial(Slot::b, 0),
        ProgramState::good_with_trial(Slot::b, 1),
        ProgramState::good_with_trial(Slot::b, 2),
        ProgramState::good_with_previous(Slot::a),
        ProgramState::good_with_previous(Slot::b),
    };
    const Slot slots[] = {Slot::a, Slot::b};
    std::size_t cases = 0;
    std::size_t failures = 0;
    for (const ProgramState& state : states) {
        for (const Slot slot : slots) {
            const stxpackage::SlotTransition refused = stxtargets::after_exit(state, slot, ExitKind::refused);
            const stxpackage::SlotTransition early = stxtargets::after_exit(state, slot, ExitKind::early);
            const stxpackage::SlotTransition late = stxtargets::after_exit(state, slot, ExitKind::after_healthy);
            const stxpackage::SlotTransition expected_refused = stxpackage::after_refused(state, slot);
            const stxpackage::SlotTransition expected_early = stxpackage::after_early_exit(state, slot);
            failures += refused.next == expected_refused.next && refused.note == expected_refused.note ? 0u : 1u;
            failures += early.next == expected_early.next && early.note == expected_early.note ? 0u : 1u;
            failures += late.next == state && late.note == stxpackage::SlotNote::unchanged ? 0u : 1u;
            cases += 3;
        }
    }
    check(failures == 0u && cases == 66u, "after exit", "as program_slots, all 66 cases");
}

void test_job_outcome() {
    std::size_t failures = 0;
    for (int status = 0; status < 256; status += 1) {
        const JobOutcome outcome = stxtargets::job_outcome(exited(status));
        JobOutcome expected = JobOutcome::failed;
        if (status == 0) {
            expected = JobOutcome::installed;
        } else if (status == 3 || status == 6) {
            expected = JobOutcome::nothing_newer;
        } else if (status == 4) {
            expected = JobOutcome::refused;
        }
        failures += outcome == expected ? 0u : 1u;
    }
    for (int signal_number = 1; signal_number < 128; signal_number += 1) {
        failures += stxtargets::job_outcome(signal_number) == JobOutcome::failed ? 0u : 1u;
    }
    check(failures == 0u, "job outcome", "every status and signal");
}

void test_next_check() {
    const std::uint32_t intervals[] = {300u, 3599u, 3600u, 3601u, 86400u, 2592000u};
    const JobOutcome outcomes[] = {JobOutcome::installed, JobOutcome::nothing_newer, JobOutcome::refused,
                                   JobOutcome::failed};
    std::size_t failures = 0;
    const std::uint64_t now = 123456789u;
    for (const std::uint32_t interval : intervals) {
        for (const JobOutcome outcome : outcomes) {
            const std::uint64_t interval_ms = static_cast<std::uint64_t>(interval) * 1000u;
            std::uint64_t expected = now + interval_ms;
            if (outcome == JobOutcome::failed && interval_ms > 3600000u) {
                expected = now + 3600000u;
            }
            failures += stxtargets::next_check_ms(outcome, now, interval) == expected ? 0u : 1u;
        }
    }
    check(failures == 0u, "next check", "every outcome and interval");
}

stxconfig::BootConfig config;

void test_device_plan() {
    const char* const text =
        "[target-show]\nrole = foreground\nid = 7\ncommand = bin/show\nfolder = /data/targets/show\n"
        "devices = fb0 \"input/event*\" ttyv0\n"
        "[target-radio]\nrole = server\nid = 8\ncommand = bin/radio\nfolder = /data/targets/radio\n";
    const bool parsed = stxconfig::parse_boot_config(stxtest::text_of(text), config).is_success();
    check(parsed, "device plan", "parses");
    if (!parsed) {
        return;
    }
    const stxdevices::DevicePlan plan = stxdevices::plan_device_rules(config);
    check(plan.count == 1u + stxdevices::base_pattern_count + 3u, "device plan", "hide, base, three devices");
    check(plan.rules[0].action == stxdevices::RuleAction::hide_all, "device plan", "hide all first");
    bool base_in_order = true;
    for (std::size_t index = 0; index < stxdevices::base_pattern_count; index += 1) {
        base_in_order = base_in_order && plan.rules[1u + index].action == stxdevices::RuleAction::show &&
                        std::strcmp(plan.rules[1u + index].pattern, stxdevices::base_patterns[index]) == 0 &&
                        std::strlen(stxdevices::base_patterns[index]) < stxdevices::pattern_size;
    }
    check(base_in_order, "device plan", "the base patterns, in order");
    const stxdevices::DeviceRule& last = plan.rules[plan.count - 1u];
    const stxdevices::DeviceRule& second = plan.rules[plan.count - 2u];
    check(last.action == stxdevices::RuleAction::show_to_group && std::strcmp(last.pattern, "ttyv0") == 0 &&
              last.group == 30007u && last.mode == 0660u && std::strcmp(second.pattern, "input/event*") == 0,
          "device plan", "the foreground target's devices to group 30007, 0660");
    const char* const no_foreground =
        "[target-radio]\nrole = server\nid = 8\ncommand = bin/radio\nfolder = /data/targets/radio\n";
    check(stxconfig::parse_boot_config(stxtest::text_of(no_foreground), config).is_success() &&
              stxdevices::plan_device_rules(config).count == 1u + stxdevices::base_pattern_count,
          "device plan", "no foreground target: base only");
}

}  // namespace

int main() {
    test_exit_kind();
    test_after_exit();
    test_job_outcome();
    test_next_check();
    test_device_plan();
    return stxtest::finish("test_targets_plan");
}
