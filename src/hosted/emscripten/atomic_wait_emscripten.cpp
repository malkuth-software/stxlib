// The atomic-wait module for Emscripten (hosted/atomic_wait_backend.hpp):
// Emscripten's futex over the wasm memory, as the wake channel uses it
// (wake_emscripten.cpp).
//
// Facts at emscripten 6.0.13 (system/include/emscripten/
// threading_primitives.h:190-213, as wake_emscripten.cpp cites them):
// emscripten_futex_wait(address, value, milliseconds) waits while the word
// equals `value`, for at most the milliseconds given as a double, and
// returns 0, -ETIMEDOUT or -EWOULDBLOCK (the word already differed), or
// -ECANCELED; it works on every thread, the main browser thread included,
// where it busy-waits (site/source/docs/porting/pthreads.rst:75-117).
// emscripten_futex_wake(address, count) wakes up to `count` waiters and
// returns how many, or a negative errno.
//
// An untimed wait waits a day at a time (core waits again), as the wake
// channel's waits do. Early exit: the futex calls are wasm code and imports
// that do not throw.

#include "hosted/atomic_wait_backend.hpp"

#include "stxlib/fail_fast.hpp"

#include <cerrno>
#include <climits>

#include <emscripten/threading.h>
#include <emscripten/threading_primitives.h>

namespace stxlib::atomic_wait_backend {

namespace {

constexpr i64 nanoseconds_per_day = static_cast<i64>(86400) * 1000000000;

[[noreturn]] void defect(const TextView message, const int error) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::internal_failure, "stxlib.atomic_word",
                          ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)),
                                      .message = message});
    fail_fast(record);
}

double milliseconds_of(const Duration interval) noexcept {
    const i64 nanoseconds = interval.count_nanoseconds();
    const i64 bounded = nanoseconds < nanoseconds_per_day ? nanoseconds : nanoseconds_per_day;
    const double result = static_cast<double>(bounded) / 1000000.0;
    return result;
}

// The futex calls take a volatile address and only read the word.
void wake(const u32* const word, const int count) noexcept {
    const int result = emscripten_futex_wake(const_cast<u32*>(word), count);
    if (result < 0) defect("emscripten_futex_wake failed", -result);
}

} // namespace

bool wait(const u32* const word, const u32 expected, const bool timed, const Duration interval) noexcept {
    const Duration bound = timed ? interval : Duration::nanoseconds(nanoseconds_per_day);
    const double milliseconds = milliseconds_of(bound);
    const int result = emscripten_futex_wait(const_cast<u32*>(word), expected, milliseconds);
    if (result == 0) return false;
    if (result == -ETIMEDOUT) return timed;
    if (result == -EWOULDBLOCK || result == -ECANCELED || result == -EINTR) return false;
    defect("emscripten_futex_wait failed", -result);
}

void wake_one(const u32* const word) noexcept { wake(word, 1); }

void wake_all(const u32* const word) noexcept { wake(word, INT_MAX); }

} // namespace stxlib::atomic_wait_backend
