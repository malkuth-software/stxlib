#pragma once

// Internal preconditions of STXlib's own types (house style, section 2:
// assert internal preconditions). A failed requirement is a defect in the
// caller: it is reported on standard error as
//
//   stxlib: requirement failed: <message> (<file>:<line>)
//
// and the process ends abnormally. Requirements are never compiled out, so
// a header template behaves the same in every translation unit that
// instantiates it, whatever its build flags (no one-definition-rule split
// between a checked and an unchecked copy). External input is validated
// with a result, never with a requirement.

namespace stxlib::detail {

// Defined in src/core/require.cpp. Allocates nothing.
[[noreturn]] void requirement_failed(const char* message, const char* file, int line) noexcept;

} // namespace stxlib::detail

#define STXLIB_REQUIRE(condition, message)                                                    \
    do {                                                                                      \
        if (!(condition)) ::stxlib::detail::requirement_failed((message), __FILE__, __LINE__); \
    } while (false)
