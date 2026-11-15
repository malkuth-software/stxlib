// device_rules.h: the devfs ruleset the supervisor applies to /dev after
// the init section (C2; owner, roadmap item 17: stock fbd, vt and evdev,
// opened only to the foreground target's group). appliance/C2_DESIGN.md,
// "Devices".
//
// Subtraction: every device node is hidden, then only those the appliance
// uses are shown again, and the foreground target's declared devices are
// shown with its group and mode 0660. Hidden nodes cannot be opened by
// anyone, root included, so the list below is the appliance's whole device
// surface; the VM check compares /dev against it (appliance/vm/c2_m4.sh).
//
// devfs rules (devfs.rules(5), sys/fs/devfs/devfs_rule.c at the pin): a
// rule with no condition matches every node; a path pattern matches by
// fnmatch(3) with FNM_PATHNAME, so "pts/*" is needed beside "pts". Rules
// apply in order and a later one overrides an earlier one's action.
//
// The plan is pure (tested on the host, test/test_device_rules.cpp); the
// system part (device_rules_system.cpp) adds the rules as ruleset
// device_ruleset and makes /dev use it.

#ifndef STXLIB_RUNTIME_SUPERVISOR_DEVICE_RULES_H
#define STXLIB_RUNTIME_SUPERVISOR_DEVICE_RULES_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "config/boot_config.h"

namespace stxdevices {

constexpr std::uint16_t device_ruleset = 10;
// devfs's pattern buffer (DEVFS_MAXPTRNLEN) with its NUL.
constexpr std::size_t pattern_size = 200;

// The nodes every appliance shows: the C library's and the shell's basics,
// pseudo-terminals for sshd, bpf for dhclient, the shim, the labelled
// partitions and the disks they are on (the updater writes root slots raw),
// the console.
constexpr const char* const base_patterns[] = {
    "null", "zero", "random", "urandom", "stdin", "stdout", "stderr", "fd", "fd/*",
    "console", "tty", "ptmx", "pts", "pts/*", "bpf", "bpf*", "stxshim",
    "ufs", "ufs/*", "gpt", "gpt/*", "msdosfs", "msdosfs/*",
    "vtbd*", "nda*", "ada*", "da*", "mmcsd*", "md*",
};
constexpr std::size_t base_pattern_count = sizeof(base_patterns) / sizeof(base_patterns[0]);
constexpr std::size_t rules_max = 1u + base_pattern_count + stxconfig::devices_max;

enum class RuleAction : std::uint8_t {
    hide_all,
    show,
    show_to_group,
};

struct DeviceRule {
    RuleAction action;
    // NUL-terminated; empty for hide_all.
    char pattern[pattern_size];
    // For show_to_group: the group and mode.
    std::uint32_t group;
    std::uint16_t mode;
};

struct DevicePlan {
    DeviceRule rules[rules_max];
    std::size_t count;
};

// The rules for config: hide all, the base patterns, then the foreground
// target's devices (if any) to its group, mode 0660. Patterns were checked
// by the parser (shorter than pattern_size).
[[nodiscard]] DevicePlan plan_device_rules(const stxconfig::BootConfig& config) noexcept;

}  // namespace stxdevices

#endif  // STXLIB_RUNTIME_SUPERVISOR_DEVICE_RULES_H
