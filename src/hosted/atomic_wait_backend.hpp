#pragma once

// The primitives each family's atomic-wait module supplies
// (linux/atomic_wait_linux.cpp, macos/atomic_wait_macos.cpp,
// freebsd/atomic_wait_freebsd.cpp, windows/atomic_wait_windows.cpp,
// emscripten/atomic_wait_emscripten.cpp); exactly one is linked.
// core/atomic_word.cpp builds include/stxlib/atomic_word.hpp's waits on
// them.
//
// `word` is an aligned u32 in this process's memory that other threads
// change with atomic operations. The host compares it with `expected`
// atomically with going to sleep. Defects (a host error other than the
// ones named here) are reported by the family through fail_fast; these
// functions return only on success. None is a cancellation point: the host
// waits are direct system calls (Linux, macOS, FreeBSD), a kernel32 wait
// (Windows) or a wasm wait (Emscripten).

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/types.hpp"

namespace stxlib::atomic_wait_backend {

// Sleeps while *word == expected: returns when woken by a wake below, when
// the host wakes it spuriously or is interrupted, at once when the word
// already differs, and (when `timed`) after at most about `interval`
// (positive), which may be cut short. True only when the host reports that
// the interval passed; core decides by IntervalTime whether its deadline
// did.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT bool wait(const u32* word, u32 expected, bool timed,
                                                                         Duration interval) noexcept;
// Wakes at least one thread sleeping in wait on `word`, or every one.
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void wake_one(const u32* word) noexcept;
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void wake_all(const u32* word) noexcept;

} // namespace stxlib::atomic_wait_backend
