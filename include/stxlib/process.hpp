#pragma once

// Exit tasks (Capability::exit_tasks), replacing atexit.
// Ported from stx_auxlib's include/stx_auxlib/process.hpp at b6e330a.
//
// Settled platform difference: exit tasks take a context, which atexit's do
// not.
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

// STXlib does not end the process normally (AGENTS.md, rule 3; owner
// decision 2026-11-15, notes decisions/2026-11-15-audit-e-decisions.md, 3,
// which removed exit_process). A program ends by returning from main or by
// calling the C library's exit itself; either way the exit tasks run.

inline constexpr usize maximum_exit_tasks = 32;

// Registers `task` to run when the process exits normally (returning from
// main, or the C library's exit; from one atexit handler), newest first. A task must not throw or longjmp out
// (AGENTS.md, early exits); it may
// register further tasks, which run after it. Its context is borrowed until
// it has run. Failures: invalid_argument for an unbound task;
// limit_exceeded with HostCode::table_full when all maximum_exit_tasks
// places are taken, or the C library refuses the handler.
[[nodiscard]] Status register_exit_task(CallbackTarget<void()> task) noexcept;

} // namespace stxlib
