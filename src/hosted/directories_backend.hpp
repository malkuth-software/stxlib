#pragma once

// The primitives behind known_directory (files.hpp) each family supplies:
// posix/home_posix.cpp (the user database's home) and
// windows/known_folders_windows.cpp (SHGetKnownFolderPath). core/
// known_directories.cpp applies the family's convention over them and the
// environment.

#include "core/growable.hpp"
#include "stxlib/files.hpp"
#include "stxlib/result.hpp"

namespace stxlib::directories_backend {

// Which convention core applies (files.hpp, known_directory).
enum class Convention : u8 {
    apple = 0,            // HOME, Library/Application Support, Library/Caches
    xdg = 1,              // the XDG Base Directory Specification
    windows_folders = 2,  // SHGetKnownFolderPath
};

#if defined(_WIN32)
inline constexpr Convention convention = Convention::windows_folders;
#elif defined(__APPLE__)
inline constexpr Convention convention = Convention::apple;
#else
inline constexpr Convention convention = Convention::xdg;
#endif

// POSIX hosts: replaces `text` with the calling user's home directory from
// the user database (getpwuid_r), as host bytes; not_found with
// HostCode::no_home when it has none. Windows: unsupported.
[[nodiscard]] OperationStatus user_database_home(Growable<char>& text) noexcept;

// Windows: replaces `text` with the known folder for `which`, as WTF-8
// with '/' separators. POSIX hosts: unsupported.
[[nodiscard]] OperationStatus known_folder(KnownDirectory which, Growable<char>& text) noexcept;

} // namespace stxlib::directories_backend
