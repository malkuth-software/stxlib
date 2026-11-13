#pragma once

// Ending the process (Capability::exit_tasks), replacing exit and atexit.
// Ported from stx_auxlib's include/stx_auxlib/process.hpp at b6e330a.
//
// Settled platform differences:
// - Exit statuses are 0 to 255: POSIX hosts keep only the low eight bits of
//   a wider status, so a wider one would read differently by host.
// - Exit tasks take a context, which atexit's do not.
//
// stx_auxlib's fail() and ensure() are not carried: STXlib's fault path is
// STXLIB_REQUIRE (require.hpp) and fail_fast (fail_fast.hpp), which report
// one line on standard error naming the source location and end the
// process abnormally, and are never compiled out.
//
// Modules: the task table lives in stxlib_state, so a task registered in
// any module runs once. The handler that runs them is registered with the
// C library's atexit by the module that registers the first task; on
// Windows, atexit in a DLL runs when that DLL is unloaded, which for a DLL
// loaded at start-up is during process exit.

#include "stxlib/callback.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// Ends the process with `status`: runs the exit tasks (newest first), then
// the C and C++ library's own exit handling (static destructors, atexit),
// which flushes standard streams. Call it only when no other thread can be
// using the objects that static destructors destroy.
[[noreturn]] void exit_process(u8 status) noexcept;

inline constexpr usize maximum_exit_tasks = 32;

// Registers `task` to run when the process exits normally (exit_process, or
// returning from main), newest first. A task must not throw; it may
// register further tasks, which run after it. Its context is borrowed until
// it has run. Failures: invalid_argument for an unbound task;
// limit_exceeded with HostCode::table_full when all maximum_exit_tasks
// places are taken, or the C library refuses the handler.
[[nodiscard]] Status register_exit_task(CallbackTarget<void()> task) noexcept;

} // namespace stxlib
