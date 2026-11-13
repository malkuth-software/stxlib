#pragma once

// Writing a failure report to standard error piece by piece, allocating
// nothing (require.cpp, fail_fast.cpp). Each piece is written as it comes;
// a report interleaved with another thread's output stays line-shaped only
// as far as the host's writes are.

#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib::report {

// A NUL-terminated foreign string, at most `limit` bytes of it.
void c_text(const char* text, usize limit) noexcept;
void text(TextView text) noexcept;
void unsigned_number(u64 value) noexcept;
void signed_number(i64 value) noexcept;
// "0x" and lower-case hexadecimal digits.
void hexadecimal(u64 value) noexcept;

} // namespace stxlib::report
