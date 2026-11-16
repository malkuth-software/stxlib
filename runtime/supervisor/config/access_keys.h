// access_keys.h: the [access] section's SSH keys (C2, K11; sc2's
// C2-web.md section 3.3; appliance/C2_DESIGN.md, "The supervisor
// interface", item 4).
//
//   [access]
//   ssh-key-1 = ssh-ed25519 AAAAC3Nz... owner@laptop
//   ...
//   ssh-key-8 = ...
//
// Each value is one public key as an authorized_keys line has it, without
// options: "<type> <base64>[ <comment>]". The type is one of
// ssh_key_types; the key is base64 ([A-Za-z0-9+/], then at most two '='
// and nothing after them), 16 bytes at least; the comment, when present,
// is printable ASCII. Options (command=, from=, ...) cannot be written:
// the line must begin with a type. The supervisor writes the valid keys,
// one per line, to access_keys_path at boot (sshd reads it beside the
// provisioned keys, appliance/disk/sshd_appliance.conf); the web service
// checks the same form before it asks (runtime/web/api_pages.cpp).
//
// The form's check (access_key_number, access_key_value_valid) is the user
// configuration's rule 7, so it lives in user_config.h, which the web
// service links too. Pure code: no system call, no allocation. Tested on
// the host (test/test_access_keys.cpp).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_ACCESS_KEYS_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_ACCESS_KEYS_H

#include <cstddef>
#include <optional>
#include <span>

#include "boot_config.h"
#include "user_config.h"

namespace stxconfig {

// The file the supervisor writes (by access_keys_temporary_path, fsync and
// a rename) on the data partition: root's, mode 0600.
constexpr const char* access_keys_path = "/data/ssh/authorized_keys.access";
constexpr const char* access_keys_temporary_path = "/data/ssh/authorized_keys.access.new";

// The authorized_keys text of a configuration's [access] keys: each valid
// key on its own line, in setting order, into out. Settings of other names
// and invalid values are left out (check_user_config refuses them in a
// user configuration before this runs). The length written, or nothing
// when out is too small.
[[nodiscard]] std::optional<std::size_t> access_keys_text(const BootConfig& config, std::span<char> out) noexcept;

// out's size that always suffices: access_keys_max lines of a line's
// length.
constexpr std::size_t access_keys_text_max = access_keys_max * (line_length_max + 1u);

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_ACCESS_KEYS_H
