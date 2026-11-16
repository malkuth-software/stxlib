// config_merge.h: the configuration a boot runs, from the factory
// configuration and the user configuration's turns (C2, K18; sc2's
// C2-web.md section 7 item 3; appliance/C2_DESIGN.md, "The supervisor
// interface", item 3).
//
// The choice: the newest turn that parses, keeps the user configuration's
// rules against the factory configuration (user_config.h) and, merged with
// the factory configuration, parses again; then the older turn the same
// way; then the factory configuration alone. The last parse of the merged
// text is what catches what only the two together break: a target id,
// folder or port both use, a second foreground target, too many services
// or settings, or a merged text over config_size_max.
//
// The merge works on lines (both texts already parse, so every line is a
// header, a setting, an init command, a comment or blank):
//   - the factory's lines in order, comments and blank lines left out;
//     in its [system], a setting the user's [system] also sets is left out
//     and the user's settings follow the header; its [network] and
//     [access], when the user has the section, are replaced by the user's
//     settings (each is one coherent group: a static address belongs with
//     its gateway);
//   - then the user's [system], [network] and [access] when the factory
//     has no such section;
//   - then each user target section, its lines as written, with one line
//     added: "start = boot" when the user's [init] has "start <name>",
//     else "start = manual" (rule 6 keeps a start setting out of a user
//     target, so the line is never repeated);
//   - the user's [init] is left out: its start lines became start
//     settings, and the factory's init section is the only one.
//
// Both the supervisor (the services, targets and [access]) and stxnet
// ([system] hostname and [network]) make this choice from the same files
// at the same boot: both read the turn files before the web service can
// write a new turn (it starts after both), and the choice is a function of
// the bytes, so both arrive at the same configuration.
//
// Pure code: no system call, no allocation. Tested on the host
// (test/test_config_merge.cpp).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_CONFIG_MERGE_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_CONFIG_MERGE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../store/turn_file.h"
#include "boot_config.h"
#include "user_config.h"

namespace stxconfig {

// The merged text of factory_text and user_text into out, or nothing when
// it does not fit. user is user_text parsed (it supplies the start lines).
[[nodiscard]] std::optional<std::size_t> merge_config_text(std::span<const char> factory_text,
                                                           std::span<const char> user_text, const BootConfig& user,
                                                           std::span<char> out) noexcept;

// Why a turn was passed over.
enum class TurnRefusal : std::uint8_t {
    none,           // not tried (absent or not a valid turn file)
    parse,          // the user text does not parse
    rule,           // it breaks a user-configuration rule
    too_large,      // the merged text does not fit config_size_max
    merged_parse,   // the merged text does not parse
    // C2, sb2: a target without an id (written past config_write, which
    // issues every id: target_ids.h); appended.
    id_missing,
};
[[nodiscard]] const char* turn_refusal_name(TurnRefusal refusal) noexcept;

struct TurnTried {
    std::size_t file_index;
    std::uint32_t sequence;
    TurnRefusal refusal;
    // For parse and merged_parse: the parser's error and place; for rule:
    // the rule (its code) and place.
    std::uint8_t code;
    std::uint32_t line;
    std::uint32_t column;
};

struct BootChoice {
    // The turn used, or nothing: the factory configuration alone.
    std::optional<std::size_t> file_index;
    std::uint32_t sequence;
    // Each turn tried and passed over, newest first (at most two).
    TurnTried passed_over[2];
    std::size_t passed_over_count;
    // The text effective now holds (in out): the merged text, or the
    // factory text copied.
    std::size_t text_length;
};

// The choice above. factory_text parses into factory (the caller parsed
// it, and does not change either during the call). turns: the two files'
// views and their choice (store/turn_file.h). user_work and effective are
// the caller's (large: static storage); on return effective holds the
// configuration to run and out its text. Nothing only when the factory
// text itself does not fit out or no longer parses (the caller then takes
// the failsafe path, as for a factory configuration that does not parse).
[[nodiscard]] std::optional<BootChoice> choose_boot_config(std::span<const char> factory_text,
                                                           const BootConfig& factory,
                                                           const std::optional<stxstore::TurnView> (&turns)[2],
                                                           const stxstore::TurnChoice& choice, BootConfig& user_work,
                                                           BootConfig& effective, std::span<char> out) noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_CONFIG_MERGE_H
