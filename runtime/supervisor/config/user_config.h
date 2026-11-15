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
//       [service-<target>]   a target, as any service
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
//
// Pure code: no system call, no allocation.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_USER_CONFIG_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_USER_CONFIG_H

#include <cstdint>
#include <optional>

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
};
constexpr std::uint8_t user_rule_last = 3;

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

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_USER_CONFIG_H
