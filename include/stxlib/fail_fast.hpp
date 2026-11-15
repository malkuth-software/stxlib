#pragma once

// Failing fast with a record: the end of a failure that cannot be carried
// as a value (survey S2, owner 2026-11-13, "Record, fail fast": a client
// callback that throws into GUI.Forms is recorded and the process fails
// fast through the fail-fast report).
//
// fail_fast writes the record as one report and ends the process
// abnormally (so the host keeps a crash report or core where it makes
// one). By default the report is STXlib's own line on standard error:
//
//   stxlib: fail fast: <operation>: <kind> [code <n>] [foreign <family> <value>]
//       [type <mangled type>]: <message>
//
// A consumer with its own fail-fast report (GUI.Forms' call_failing_fast)
// installs a reporter once; fail_fast then calls it instead of writing its
// own line, and ends the process when it returns.
//
// The reporter contract (the A2 swap-in, GUI.Forms' contract item 3.2):
// - It runs on the failing thread, with that thread's state intact: the
//   record's shadow record is still readable there.
// - It may call: read_shadow (with record.shadow()), read_shadow_stack,
//   exception_type_for_display (which allocates and frees one block),
//   write_console and write_console_line (STXlib never fails fast while a
//   thread holds the console's output lock, so these cannot deadlock on
//   their own thread), capture_stack_trace and write_stack_trace
//   (stack_trace.hpp) and describe_address (images.hpp), which allocate
//   nothing in STXlib's code and take no STXlib lock (those headers state
//   what the host's unwinder and loader do beneath them),
//   ErrorRecord's accessors, and plain code of its own that does not
//   throw.
// - It must not open a shadow frame (no gate, no in_shadow_frame), install
//   a reporter, or throw. In a development build (STXLIB_SHADOW) opening a
//   frame from the reporter fails a requirement, which ends the process
//   with STXlib's requirement line.
// - A failure inside the report (a nested fail_fast on the same thread)
//   writes STXlib's own report and ends the process; the reporter is never
//   called again on that thread.
// - Several threads may fail fast at once; each calls the reporter on its
//   own thread.
//
// STXlib's own report, when no reporter is installed, writes the record,
// its frames (kind, numbers, subject and name) and, in a development
// build, the native trace.

#include "stxlib/error.hpp"
#include "stxlib/result.hpp"

namespace stxlib {

// `report` runs once per process, on the failing thread, with no STXlib
// lock held. It must return or end the process itself; no longjmp out of
// it (AGENTS.md, early exits).
struct FailFastReporter final {
    void (*report)(void* context, const ErrorRecord& record) noexcept{nullptr};
    void* context{nullptr};
};

// Installs the process's reporter. Once only: a second installation fails
// with already_exists and leaves the first in place; an unbound reporter
// fails with invalid_argument. Install before any thread can fail fast;
// the reporter and its context must stay valid until the process ends.
[[nodiscard]] Status install_fail_fast_reporter(FailFastReporter reporter) noexcept;

// Reports `record` and ends the process abnormally. Allocates nothing.
[[noreturn]] void fail_fast(const ErrorRecord& record) noexcept;

// For a site that states it cannot handle a failure (survey D8's
// recommendation: "fail-fast only where a site states it cannot handle
// it"; notes planning/stages/A3-no-exceptions.md, section 1.5): a failure
// goes to fail_fast with its record unchanged, and success passes on. This
// is how such a site is written without exceptions, in place of letting an
// exception escape.

// Ends the process through fail_fast when `status` failed; returns
// otherwise.
inline void check_or_fail_fast(const Status& status) noexcept {
    const ErrorRecord* const failure = status.error();
    if (failure != nullptr) fail_fast(*failure);
}

// The value of a successful `result`; ends the process through fail_fast
// with the record of a failed one.
template <typename Value>
[[nodiscard]] Value take_or_fail_fast(Result<Value>&& result) noexcept {
    const ErrorRecord* const failure = result.error();
    if (failure != nullptr) fail_fast(*failure);
    Value value = static_cast<Result<Value>&&>(result).take_value();
    return value;
}

} // namespace stxlib
