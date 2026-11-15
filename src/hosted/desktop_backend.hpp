#pragma once

// The primitives each family's desktop module supplies
// (posix/desktop_posix.cpp, windows/desktop_windows.cpp); core/desktop.cpp
// validates every target before calling them.

#include "stxlib/detail/early_exit.hpp"
#include "core/growable.hpp"
#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"

namespace stxlib::desktop_backend {

// Whether this family's Path may name a drive ("C:/"), and whether its host
// names are bytes (POSIX) rather than WTF-8 made from 16-bit units.
#if defined(_WIN32)
inline constexpr bool drive_roots = true;
inline constexpr bool byte_names = false;
#else
inline constexpr bool drive_roots = false;
inline constexpr bool byte_names = true;
#endif

// Opens a link (an http, https or mailto URL) or an absolute Path's text,
// already checked.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN Status open_target(TextView target, bool is_link) noexcept;
// Reveals an absolute Path, already checked, whose text is Unicode.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN Status reveal(const Path& path) noexcept;
// The executable's host name ('/' separators; WTF-8 on Windows, raw bytes on
// POSIX), not yet parsed; or a failure.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Status executable_name(Growable<char>& text) noexcept;

} // namespace stxlib::desktop_backend
