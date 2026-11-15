// device_rules_system.h: applying the device plan to /dev (FreeBSD's devfs
// rule ioctls, sys/fs/devfs/devfs.h at the pin). See device_rules.h.

#ifndef STXLIB_RUNTIME_SUPERVISOR_DEVICE_RULES_SYSTEM_H
#define STXLIB_RUNTIME_SUPERVISOR_DEVICE_RULES_SYSTEM_H

#include "device_rules.h"

namespace stxdevices {

// Deletes ruleset device_ruleset's old rules, adds the plan's in order
// (rule numbers 1, 2, ...), makes /dev use the ruleset for every node
// created later and applies it to the nodes there now. Prints
//
//   STXSUPERVISOR devices ruleset=10 rules=<n> applied | failed step=<s> errno=<n>
//
// Returns whether it was applied.
[[nodiscard]] bool apply_device_plan(int console, const DevicePlan& plan) noexcept;

}  // namespace stxdevices

#endif  // STXLIB_RUNTIME_SUPERVISOR_DEVICE_RULES_SYSTEM_H
