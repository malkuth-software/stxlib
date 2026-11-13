#pragma once

// Desktop integration (Capability::desktop_open, file_reveal and
// current_executable). Ported from stx_auxlib's
// include/stx_auxlib/desktop.hpp and src/hosted/desktop_<host>.cpp at
// b6e330a.
//
// Sealing: open_with_desktop accepts only http, https and mailto links and
// absolute paths, without control characters, so input can never become a
// command-line option or an arbitrary protocol handler. The desktop's helper
// (open on macOS, xdg-open elsewhere on POSIX, ShellExecuteW and explorer on
// Windows) runs detached, with no shell and its output discarded; neither
// function waits for the desktop to act.
//
// Failures: invalid_argument with HostCode::invalid_value for a refused
// target or a relative path; unsupported when the desktop's helper is
// missing (a POSIX host without xdg-open) or, for current_executable, on a
// host STXlib cannot ask; Child::start's failures otherwise; io_failure when
// ShellExecuteW refuses.

#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"

namespace stxlib {

// Opens a link or an absolute path with the desktop's default handler.
[[nodiscard]] Status open_with_desktop(TextView target) noexcept;
// Shows `path` selected in the desktop's file manager (on hosts that cannot
// select it, its directory opened). `path` must be absolute; a host name
// that is not valid Unicode (path.hpp) reaches the file manager as the host
// spells it (children.hpp).
[[nodiscard]] Status reveal_in_file_manager(const Path& path) noexcept;
// The running program's executable, as an absolute Path (its host name
// carried losslessly, path.hpp).
[[nodiscard]] Result<Path> current_executable() noexcept;

} // namespace stxlib
