// boot_slots_system.h: the A/B slots, the boot log and the network check
// on FreeBSD (C1 step S4; the pure part is boot_slots.h, the design
// appliance/disk/LAYOUT.md).
//
// System calls: kenv(2), nmount(2), unmount(2), open/read/pwrite/fsync/
// rename/close, getifaddrs(3). Hosted on FreeBSD's libc for C1.
//
// Console lines:
//   STXSUPERVISOR boot slot=<A|B|none> tries=<n> fallback=<0|1>
//   STXSUPERVISOR boot good slot=<A|B> tries=<reset|kept> slot-file=<written|kept>
//   STXSUPERVISOR boot mark-failed step=<name> errno=<n>
//   STXSUPERVISOR boot-log written event=<event> | failed step=<name> errno=<n>

#ifndef STXLIB_RUNTIME_SUPERVISOR_BOOT_SLOTS_SYSTEM_H
#define STXLIB_RUNTIME_SUPERVISOR_BOOT_SLOTS_SYSTEM_H

#include <cstdint>

#include "boot_slots.h"

namespace stxboot {

// Mount points in the read-only root (created when the root is built).
constexpr const char* bootctl_device = "/dev/ufs/STXBOOTCTL";
constexpr const char* bootctl_directory = "/stx/bootctl";
constexpr const char* esp_device = "/dev/msdosfs/STXESP";
constexpr const char* esp_directory = "/stx/esp";
constexpr const char* user_device = "/dev/msdosfs/STXUSER";
constexpr const char* user_directory = "/user";
constexpr const char* boot_log_path = "/user/stx/boot.log";
constexpr const char* failsafe_config_path = "/etc/stx/failsafe.conf";

// Reads stx.boot.slot, stx.boot.tries and stx.boot.fallback from the kernel
// environment, and prints the boot line.
[[nodiscard]] BootFacts read_boot_facts(int console) noexcept;

// Whether a non-loopback interface that is up has an IPv4 address.
[[nodiscard]] bool network_up() noexcept;

// Marks this boot good (LAYOUT.md): mounts the boot-control partition and
// the ESP read-write only for the writes, writes what mark_good_plan says,
// unmounts. Prints one line.
void mark_boot_good(int console, const BootFacts& facts) noexcept;

// Appends "uptime_ms=<n> slot=<s> tries=<n> fallback=<0|1> event=<event>\n"
// to the boot log, mounting the user partition first when it is not yet
// mounted. Prints one line.
void append_boot_log(int console, const BootFacts& facts, const char* event, std::uint64_t uptime_ms) noexcept;

// On a fallback boot: appends the event
//   "fallback tried=<A|B|none> loader-reason=<r> tried-last-event=<e|none>"
// where tried and loader-reason are the loader's stx.boot.tried and
// stx.boot.reason, and tried-last-event is the last event the tried slot
// wrote to the boot log (its failsafe entry), read from the log's last
// boot_log_tail_bytes; none when it wrote none (it failed before userspace).
constexpr std::size_t boot_log_tail_bytes = 4096;
void append_fallback_log(int console, const BootFacts& facts, std::uint64_t uptime_ms) noexcept;

}  // namespace stxboot

#endif  // STXLIB_RUNTIME_SUPERVISOR_BOOT_SLOTS_SYSTEM_H
