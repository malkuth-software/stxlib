// user_config.h: the user configuration, the part of the machine's
// configuration that the web interface edits (C2; design
// dev/notes/planning/stages/C2-web.md, section 4.3).
//
// The appliance has two configurations, both in the boot configuration's
// format (boot_config.h):
//   - the factory configuration, in the read-only root (config_path): the
//     init section (mounts, the network's programs, sshd, the web service)
//     and the system's own services. A user configuration can never change
//     it, so no edit made on the web page can stop the web page or SSH from
//     coming up;
//   - the user configuration, on the user partition, in two files written
//     by turns (store/turn_file.h): user_config_paths. It holds the
//     machine's settings and its targets:
//
//       [system]      hostname = ...
//       [network]     mode = dhcp|static, address, gateway, dns
//       [access]      ssh-key-1 = ... (up to ssh-key-8)
//       [service-<target>]   a target: K11's target settings (boot_config.h;
//                     a service section with target settings is a target)
//       [init]        start <target>    (a target enabled at boot)
//
// What the settings mean, and when the targets start, is the host side's
// (sb2's C2 note, K4 and K11): the supervisor reads the user configuration
// at boot, after the factory init section has mounted the user partition,
// taking the newest turn that parses and keeps the rules below, then the
// older, then none. This file states the rules both sides check: the
// supervisor before it writes a turn (control/control_server.h,
// config_write) and the web service before it asks (runtime/web/api.h).
//
// The rules, beyond parsing:
//   1. Sections: system, network, access, init and service-<name> only.
//   2. Init commands: start only. Mounting, running programs and waiting
//      belong to the factory configuration.
//   3. A target's name is not a service of the factory configuration.
//   4. A target has control none: only factory services talk to the
//      supervisor.
//   5. Every service-<name> section is a target (it has target settings):
//      a user configuration never defines a service, which would run as
//      root with any command (C2, sb2; appliance/C2_DESIGN.md, "The
//      supervisor interface", item 3).
//   6. A target has no start setting: whether it starts at boot is its
//      start line in [init] (the web page's "enabled"), one place only.
//   7. [access] holds ssh-key-1 to ssh-key-8 only, each a valid key
//      (access_keys.h).
//
// How the next boot uses the result (the newest turn that parses, keeps
// these rules and merges with the factory configuration, then the older
// turn, then the factory configuration alone) is config_merge.h.
//
// Pure code: no system call, no allocation.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_USER_CONFIG_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_USER_CONFIG_H

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>

#include "boot_config.h"

namespace stxconfig {

// The two turn files, on the user partition (FAT32; the factory init
// section mounts it at /user).
constexpr const char* user_config_directory = "/user/stx";
constexpr const char* user_config_path_0 = "/user/stx/config.0";
constexpr const char* user_config_path_1 = "/user/stx/config.1";

// The sections a user configuration may hold, besides service-<name>.
constexpr const char* user_section_system = "system";
constexpr const char* user_section_network = "network";
constexpr const char* user_section_access = "access";
constexpr const char* user_section_init = "init";

enum class UserRule : std::uint8_t {
    section_not_allowed,
    command_not_allowed,
    target_name_taken,
    target_control_not_allowed,
    // C2, sb2 (appended: the codes above keep their values on the wire).
    service_not_target,
    target_start_not_allowed,
    access_setting_not_allowed,
    // A target's id that the supervisor did not issue to that target
    // (target_ids.h): checked by config_write against the id ledger on
    // /data, not by check_user_config (the web service has no ledger).
    target_id_not_issued,
};
constexpr std::uint8_t user_rule_last = 7;

[[nodiscard]] const char* user_rule_name(UserRule rule) noexcept;
[[nodiscard]] std::optional<UserRule> user_rule_from_code(std::uint8_t code) noexcept;

// Where a parsed user configuration breaks a rule: 1-based line and column.
struct UserRuleBroken {
    UserRule rule;
    std::uint32_t line;
    std::uint32_t column;
};

// The first rule user breaks (in file order: sections, then init commands,
// then targets), or nothing. user and factory are parsed configurations;
// factory may be empty (a failsafe boot, or no factory configuration).
[[nodiscard]] std::optional<UserRuleBroken> check_user_config(const BootConfig& user,
                                                              const BootConfig& factory) noexcept;

// Rule 7's form: an [access] value is one public key without options,
// "<type> <base64>[ <comment>]": the type one of ssh_key_types; the key
// base64 ([A-Za-z0-9+/], then at most two '=' and nothing after them), at
// least access_key_base64_min bytes; the comment, when present, one space
// then printable ASCII.
constexpr std::size_t access_keys_max = 8;
constexpr std::size_t access_key_base64_min = 16;
// The key types, as the web service lists them.
constexpr const char* const ssh_key_types[] = {
    "ssh-ed25519",
    "ecdsa-sha2-nistp256",
    "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521",
    "sk-ssh-ed25519@openssh.com",
    "sk-ecdsa-sha2-nistp256@openssh.com",
};

// Which ssh-key-<n> a setting name is: 1..access_keys_max, or nothing.
[[nodiscard]] std::optional<std::size_t> access_key_number(const char* name) noexcept;

// Whether a value has the form above.
[[nodiscard]] bool access_key_value_valid(std::span<const char> value) noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_USER_CONFIG_H
