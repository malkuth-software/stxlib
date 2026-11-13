#pragma once

// The primitives each family's desktop module supplies
// (posix/desktop_posix.cpp, windows/desktop_windows.cpp); core/desktop.cpp
// validates every target before calling them.

#include "core/growable.hpp"
#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"

namespace stxlib::desktop_backend {

// Whether this family's Path may name a drive ("C:/").
#if defined(_WIN32)
inline constexpr bool drive_roots = true;
#else
inline constexpr bool drive_roots = false;
#endif

// Opens a link (an http, https or mailto URL) or an absolute Path's text,
// already checked.
[[nodiscard]] Status open_target(TextView target, bool is_link) noexcept;
// Reveals an absolute Path, already checked.
[[nodiscard]] Status reveal(const Path& path) noexcept;
// The executable's host name ('/' separators, UTF-8 on Windows, raw bytes on
// POSIX), not yet parsed; or a failure.
[[nodiscard]] Status executable_name(Growable<char>& text) noexcept;

} // namespace stxlib::desktop_backend
