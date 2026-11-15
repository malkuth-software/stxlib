// stxpkg_exit.h: stxpkg's exit statuses, shared with the supervisor's
// targets layer, which runs stxpkg and acts on how it ended (stxpkg.cpp
// states what each means).

#ifndef STXLIB_RUNTIME_PACKAGE_STXPKG_EXIT_H
#define STXLIB_RUNTIME_PACKAGE_STXPKG_EXIT_H

#include "../supervisor/config/service_registry.h"

namespace stxpackage {

constexpr int exit_done = 0;
constexpr int exit_usage = 2;
constexpr int exit_not_newer = 3;
constexpr int exit_refused = 4;
constexpr int exit_io = 5;
constexpr int exit_nothing_newer = 6;
constexpr int exit_fetch_failed = 7;
constexpr int exit_run_refused = stxconfig::package_refused_exit_status;

// The program that runs packages, installs and updates (system_actions.h
// names it as the target launcher too).
constexpr const char* stxpkg_path = "/sbin/stxpkg";

}  // namespace stxpackage

#endif  // STXLIB_RUNTIME_PACKAGE_STXPKG_EXIT_H
