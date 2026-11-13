#pragma once

// The process environment (Capability::environment): named UTF-8 values
// inherited from the parent and passed to child processes. Ported from
// stx_auxlib's include/stx_auxlib/environment.hpp at b6e330a.
//
// Settled platform differences:
// - Names are limited to the portable set: an ASCII letter or '_', then
//   letters, digits and '_'. Other names are refused, so a name means the
//   same on every host. Hosts still differ in case: Windows treats "Path"
//   and "PATH" as one variable, POSIX hosts as two; use upper-case names.
// - Values are UTF-8 on every host. Windows reads and writes the wide
//   Win32 environment block, not the C runtime's ANSI copy (which getenv
//   reads and which child processes do not inherit).
// - The host's getenv and setenv race when called concurrently; these
//   functions serialize with one another, in every module (the lock is in
//   stxlib_state). Code that still calls the host's functions directly is
//   not covered.

#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// Bounds every value read or written, in bytes; Windows allows at most
// 32,767 UTF-16 code units.
inline constexpr usize maximum_environment_bytes = 32767;

[[nodiscard]] bool is_portable_variable_name(TextView name) noexcept;

// The variable's value. Failures: invalid_argument with
// HostCode::invalid_name; not_found when it is not set; limit_exceeded with
// HostCode::too_long for a value of more than `limit` bytes (refused before
// copying); invalid_argument with HostCode::invalid_text when the host's
// value is not UTF-8; out_of_memory.
[[nodiscard]] Result<Utf8String> read_variable(TextView name, usize limit) noexcept;
// Sets the variable, replacing any value it had. Failures: invalid_name;
// limit_exceeded (more than maximum_environment_bytes); invalid_argument
// with HostCode::invalid_value for a value containing NUL; foreign_failure
// when the host refuses.
[[nodiscard]] Status set_variable(TextView name, TextView value) noexcept;
// Removes the variable; succeeds when it was not set.
[[nodiscard]] Status remove_variable(TextView name) noexcept;

} // namespace stxlib
