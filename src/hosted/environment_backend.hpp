#pragma once

// The primitives each family's environment backend supplies
// (posix/environment_posix.cpp, windows/environment_windows.cpp); exactly
// one is linked. core/environment.cpp validates names and values and
// serializes every call, so each primitive receives a portable,
// NUL-terminated name, runs alone, and may assume a value without NUL.
// Ported from stx_auxlib's src/hosted/environment_backend.hpp at b6e330a.

#include "core/growable.hpp"
#include "stxlib/error.hpp"
#include "stxlib/types.hpp"

namespace stxlib::environment_backend {

enum class Outcome : u8 {
    ok = 0,
    not_set = 1,
    too_long = 2,     // more than the limit, found before copying
    invalid_text = 3, // the host's value is not UTF-8 (or UTF-16)
    out_of_memory = 4,
    host_error = 5,
};

struct Report final {
    Outcome outcome{Outcome::ok};
    ForeignCode foreign{};
};

// Replaces `value` with the variable's raw bytes (not yet checked as UTF-8
// on POSIX hosts; core/environment.cpp checks).
[[nodiscard]] Report read(const char* name, usize limit, Growable<char>& value) noexcept;
// `value` is NUL-terminated.
[[nodiscard]] Report set(const char* name, const char* value, usize value_count) noexcept;
[[nodiscard]] Report remove(const char* name) noexcept;

} // namespace stxlib::environment_backend
