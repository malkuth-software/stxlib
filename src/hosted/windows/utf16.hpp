#pragma once

// UTF-8 and UTF-16 conversion for the Windows modules, ported from
// stx_auxlib's src/unicode.cpp (append_utf16, append_utf8) at b6e330a.
// wchar_t is 16 bits on Windows, so the UTF-16 side is wchar_t, which the
// Win32 wide calls take.

#include "core/growable.hpp"
#include "stxlib/types.hpp"

namespace stxlib::utf16 {

// Appends valid UTF-8 `bytes` as UTF-16. False when memory runs out (the
// input is valid by contract: every caller holds a TextView).
[[nodiscard]] bool append_wide(const char* bytes, usize count, Growable<wchar_t>& output) noexcept;

enum class Narrowed : u8 {
    ok = 0,
    unpaired_surrogate = 1,
    out_of_memory = 2,
};

// Appends UTF-16 `units` as UTF-8; on failure `output` is unchanged.
[[nodiscard]] Narrowed append_narrow(const wchar_t* units, usize count, Growable<char>& output) noexcept;

// Appends a terminating zero unit.
[[nodiscard]] bool terminate(Growable<wchar_t>& output) noexcept;

} // namespace stxlib::utf16
