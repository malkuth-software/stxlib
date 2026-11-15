// target_ids.h: the supervisor issues target ids (C2, sb2; coordinator's
// design call, 2026-11-15).
//
// A target runs as uid and gid target_identity_base + id (boot_config.h),
// so an id given to a second target would let it inherit the first one's
// files. The supervisor therefore issues every id and never issues one
// twice. It keeps a ledger of every id it has issued or adopted, on the
// data partition (target_id_ledger_path, root only), and:
// - config_write: a user target written without an id keeps the id the
//   ledger gives its name while it is active, else gets the lowest id never
//   used before (not in the ledger, not a factory target's); a target
//   written with an id must hold the id the ledger gives that name, else
//   the write is refused (UserRule::target_id_not_issued, at the id line).
//   A target in the ledger that the new configuration no longer holds is
//   marked deleted. The ledger is written before the configuration (an id
//   issued for a write that then fails is spent, never reused);
// - at boot, after the init section mounted /data: every target the
//   chosen configuration runs is adopted (factory targets, and any written
//   before this ledger existed); a deleted target that is back in the
//   chosen configuration with its own id (an older turn chosen) is active
//   again; every other deleted target's folder is removed, once, and its
//   entry kept as removed: the id stays spent.
//
// Ledger text, one entry per line, ids ascending:
//
//   stx-target-ids 1
//   <id> active <name> <folder>
//   <id> deleted <name> <folder>
//   <id> removed
//
// Pure code: no system call, no allocation (target_id_files.h does the
// files). Tested on the host (test/test_target_ids.cpp).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_TARGET_IDS_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_TARGET_IDS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "boot_config.h"

namespace stxconfig {

constexpr const char* target_id_ledger_directory = "/data/stx";
constexpr const char* target_id_ledger_path = "/data/stx/target_ids";
constexpr const char* target_id_ledger_temporary_path = "/data/stx/target_ids.new";
constexpr const char* target_id_ledger_header = "stx-target-ids 1\n";

// Every id 1..target_id_max can have an entry.
constexpr std::size_t target_id_entries_max = target_id_max;
// One entry line at most: id (3), state (7), name, folder, three spaces
// and LF.
constexpr std::size_t target_id_line_max = 3u + 1u + 7u + 1u + name_length_max + 1u + line_length_max + 1u;
constexpr std::size_t target_id_ledger_text_max = 17u + target_id_entries_max * target_id_line_max;

enum class LedgerState : std::uint8_t {
    active,
    deleted,
    removed,
};

struct LedgerEntry {
    std::uint32_t id;
    LedgerState state;
    // Empty for removed.
    std::uint8_t name_length;
    char name[name_length_max + 1u];
    std::uint16_t folder_length;
    char folder[line_length_max + 1u];
};

// Large (target_id_entries_max entries): static storage.
class TargetIdLedger final {
public:
    TargetIdLedger() noexcept;
    TargetIdLedger(const TargetIdLedger&) = delete;
    TargetIdLedger& operator=(const TargetIdLedger&) = delete;

    void clear() noexcept;
    // Reads the ledger text; false (and empty) when it is not one: a
    // ledger that cannot be read is never taken as empty by the callers.
    [[nodiscard]] bool parse(std::span<const char> text) noexcept;
    // The text into out (target_id_ledger_text_max bytes are enough).
    [[nodiscard]] std::optional<std::size_t> format(std::span<char> out) const noexcept;

    [[nodiscard]] std::size_t count() const noexcept;
    [[nodiscard]] const LedgerEntry& entry(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_id(std::uint32_t id) const noexcept;
    // Adds an entry (kept in id order); false when the id has one or the
    // values do not fit.
    [[nodiscard]] bool add(std::uint32_t id, LedgerState state, std::span<const char> name,
                           std::span<const char> folder) noexcept;
    void set_state(std::size_t index, LedgerState state) noexcept;

private:
    LedgerEntry entries_[target_id_entries_max];
    std::size_t count_;
};

enum class IdIssueKind : std::uint8_t {
    issued,        // out holds the text, ids added; the ledger was changed
    refused,       // a given id is not the one issued to that target (line, column)
    ledger_full,   // every id 1..target_id_max is spent
    too_large,     // the text with the ids added is over config_size_max
};

struct IdIssueOutcome {
    IdIssueKind kind;
    std::size_t length;
    std::uint32_t line;
    std::uint32_t column;
};

// config_write's part. user is text parsed (and passed check_user_config);
// factory the factory configuration (may be empty). On issued, ledger holds
// the new entries and states; on any other outcome its contents are
// undefined (the caller reads it again from its file).
[[nodiscard]] IdIssueOutcome issue_target_ids(const BootConfig& user, const BootConfig& factory,
                                              std::span<const char> text, TargetIdLedger& ledger,
                                              std::span<char> out) noexcept;

// The boot's part, on the configuration the supervisor runs: adopts its
// targets, makes active again a deleted entry it holds with that id, and
// lists in removals the indices of the deleted entries left, whose folders
// the caller removes once and then marks removed. conflicts counts targets
// whose id the ledger gives another name or has removed (a configuration
// not written through config_write); the caller reports them.
struct BootIdPlan {
    std::size_t removals[target_id_entries_max];
    std::size_t removal_count;
    std::size_t conflicts;
    bool changed;
};
void plan_boot_ids(const BootConfig& running, TargetIdLedger& ledger, BootIdPlan& plan) noexcept;

// Whether every target in config has an id (the boot refuses a user turn
// that has not, config_merge.h).
[[nodiscard]] bool every_target_has_id(const BootConfig& config) noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_TARGET_IDS_H
