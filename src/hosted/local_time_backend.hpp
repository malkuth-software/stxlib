#pragma once

// The primitive each family's local time module supplies
// (posix/local_time_posix.cpp, windows/local_time_windows.cpp); core/
// local_time.cpp computes the offset from the reading.

#include "stxlib/local_time.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib::local_time_backend {

// The host's local reading of Unix second `seconds`: every field of `local`
// but the nanosecond. Failures as local_time.hpp states.
[[nodiscard]] Status local_reading(i64 seconds, CivilTime& local) noexcept;

} // namespace stxlib::local_time_backend
