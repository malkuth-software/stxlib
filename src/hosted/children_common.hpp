#pragma once

// Private helpers of the child-process modules (core/children.cpp,
// posix/children_posix.cpp, windows/children_windows.cpp). Ported from
// stx_auxlib's src/hosted/children_common.hpp at b6e330a.

#include "core/growable.hpp"
#include "stxlib/children.hpp"
#include "stxlib/error.hpp"
#include "stxlib/result.hpp"

namespace stxlib::children_detail {

// A non-empty program, and a program and arguments without NUL (a PathText
// is well-formed WTF-8 already). Fails with invalid_argument and HostCode::empty_request
// or HostCode::invalid_value.
[[nodiscard]] Status check_request(const ChildRequest& request) noexcept;

// The record for a Child that cannot take the call: empty, already waited
// for, or not capturing.
[[nodiscard]] ErrorRecord invalid_state(Operation operation) noexcept;

// Appends bytes[0, count) to `output` while it stays within `limit`;
// otherwise sets `over_limit` and keeps nothing more (stx_auxlib's
// read_output loop body). False when memory runs out.
[[nodiscard]] bool keep_output(const Byte* bytes, usize count, usize limit, Growable<Byte>& output,
                               bool& over_limit) noexcept;

// The captured output as the caller's buffer, taking `output`'s block
// without a copy (`output` is empty afterwards), or limit_exceeded with
// HostCode::too_long.
[[nodiscard]] Result<Buffer<Byte>> finish_output(Growable<Byte>& output, bool over_limit,
                                                 Operation operation) noexcept;

} // namespace stxlib::children_detail
