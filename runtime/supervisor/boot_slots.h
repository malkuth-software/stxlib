// boot_slots.h: the supervisor's pure part of the A/B root slots, the
// good-boot rule and the failsafe decision (C1 step S4; design
// appliance/disk/LAYOUT.md). No system calls: boot_slots_system.cpp reads
// the kernel environment and writes the files.

#ifndef STXLIB_RUNTIME_SUPERVISOR_BOOT_SLOTS_H
#define STXLIB_RUNTIME_SUPERVISOR_BOOT_SLOTS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace stxboot {

enum class Slot : std::uint8_t { a, b };

[[nodiscard]] char slot_letter(Slot slot) noexcept;
[[nodiscard]] std::optional<Slot> slot_from_letter(char letter) noexcept;

// The ESP's /stx/slots.conf: "good=<A|B>\ntry=<A|B>\n". Parsing accepts the
// two settings in either order, once each, nothing else but blank lines;
// anything else is refused (nullopt).
struct SlotFile {
    Slot good;
    Slot try_slot;
};
constexpr std::size_t slot_file_size_max = 256;
[[nodiscard]] std::optional<SlotFile> parse_slot_file(std::span<const char> text) noexcept;
// Exactly 14 bytes.
constexpr std::size_t slot_file_text_size = 14;
[[nodiscard]] std::array<char, slot_file_text_size> format_slot_file(const SlotFile& file) noexcept;

// The boot-control partition's /stx/tries: 16 bytes, "tries=<d>", blanks,
// "\n" (the loader rewrites it in place, appliance/boot/local.lua).
constexpr std::size_t tries_file_size = 16;
constexpr std::uint32_t tries_max = 3;
[[nodiscard]] std::optional<std::uint32_t> parse_tries(std::span<const char> text) noexcept;
[[nodiscard]] std::array<char, tries_file_size> format_tries(std::uint32_t count) noexcept;

// What the loader told this boot (kenv stx.boot.*). Absent values (a boot
// without our loader) are nullopt / false.
struct BootFacts {
    std::optional<Slot> slot;
    std::uint32_t tries;
    bool fallback;
};
[[nodiscard]] BootFacts boot_facts(const char* slot, const char* tries, const char* fallback) noexcept;

// The boot log (/user/stx/boot.log): lines "... slot=<X> ... event=<e>".
// The event of the last line whose slot is the given one, as a span into
// text (up to the next blank or newline); nullopt when there is none. Used
// to say, on a fallback, why the tried slot failed: its own last failsafe
// entry, or none when it failed before its supervisor could write one.
[[nodiscard]] std::optional<std::span<const char>> last_event_of_slot(std::span<const char> text, Slot slot) noexcept;

// The failsafe decision.
enum class ConfigState : std::uint8_t { loaded, absent, refused, unreadable, not_read };
enum class FailsafeReason : std::uint8_t { none, config_absent, config_refused, config_unreadable,
                                           restart_limit };
constexpr std::uint64_t failsafe_restart_limit = 3;
// restart_count: stxinit's count for this supervisor start, nullopt when
// unknown (taken as 0). The restart limit is reached at exactly the third
// restart (later restarts do not take the path again: it is already up),
// and wins over the configuration, which is read on start 0 only. A
// fallback boot is not a failsafe reason: the last good slot runs the
// appliance; the failed update is logged and cancelled when this boot
// proves good (mark_good_plan).
[[nodiscard]] FailsafeReason failsafe_reason(ConfigState config, std::optional<std::uint64_t> restart_count) noexcept;
[[nodiscard]] const char* failsafe_reason_name(FailsafeReason reason) noexcept;

// The good-boot rule (LAYOUT.md, "When a boot is good").
constexpr std::uint64_t good_boot_settle_ms = 60000;
struct GoodBootInputs {
    bool registered;            // with the shim
    bool init_done;             // the init section ran on this supervisor start
    bool failsafe_taken;
    bool services_healthy;      // services_healthy() below
    bool network_up;            // an Ethernet interface has an IPv4 address
    std::uint64_t init_done_ms; // monotonic time the init section ended
    std::uint64_t now_ms;
};
[[nodiscard]] bool boot_is_good(const GoodBootInputs& inputs) noexcept;

// Services: each running or waiting to restart, or stopped with the
// policy "never" (it finished or was never started).
enum class ServiceStateKind : std::uint8_t { stopped, running, waiting, stopping };
struct ServiceHealth {
    ServiceStateKind state;
    bool restart_never;
};
[[nodiscard]] bool services_healthy(std::span<const ServiceHealth> services) noexcept;

// What marking this boot good writes. Nothing for a boot without a known
// slot.
struct MarkGoodPlan {
    bool write_tries_zero;
    std::optional<SlotFile> new_slot_file;
};
// current_tries: the tries file's count (nullopt: unreadable, rewritten).
// current_slot_file: nullopt when unreadable (rewritten with this slot).
// The slot file is rewritten (good = try = this slot) when its good slot is
// not this one; on a fallback boot also when try is not this one (the
// failed update is cancelled). Otherwise an update pending in try, written
// during this boot, is kept.
[[nodiscard]] MarkGoodPlan mark_good_plan(const BootFacts& facts, std::optional<std::uint32_t> current_tries,
                                          std::optional<SlotFile> current_slot_file) noexcept;

}  // namespace stxboot

#endif  // STXLIB_RUNTIME_SUPERVISOR_BOOT_SLOTS_H
