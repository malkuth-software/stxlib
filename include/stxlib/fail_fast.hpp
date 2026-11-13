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
// own line, and ends the process when it returns. A reporter must not
// throw, allocate unboundedly or call back into fail_fast; if it fails fast
// itself, the inner fail_fast writes STXlib's line and ends the process.

#include "stxlib/error.hpp"
#include "stxlib/result.hpp"

namespace stxlib {

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

} // namespace stxlib
