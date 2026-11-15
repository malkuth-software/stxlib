#pragma once

// Holding POSIX thread cancellation off over a section (owner decision
// 2026-11-15, notes decisions/2026-11-15-audit-e-decisions.md, 2; audit E,
// sections 3 and 4). STXlib acts on a cancellation only outside its locked
// sections: wherever a cancellation point would otherwise be reached while
// an STXlib lock is held, or while a trampoline visit is open in a unit
// without exceptions, a CancellationHold keeps cancellation off, and a
// cancellation requested meanwhile is acted on at the thread's next
// cancellation point after the section. Declared before the section's lock,
// so it ends after the lock is released. On Windows, which has no thread
// cancellation, it does nothing.

#include "stxlib/detail/early_exit.hpp"

namespace stxlib::detail {

// Turns cancellation off for the calling thread; returns the previous
// state, for restore_cancellation.
[[nodiscard]] int cancellation_off() noexcept;
void restore_cancellation(int previous) noexcept;

class STXLIB_EARLY_EXIT_NO_CANCEL CancellationHold final {
public:
    CancellationHold() noexcept : previous_(cancellation_off()) {}
    ~CancellationHold() { restore_cancellation(previous_); }
    CancellationHold(const CancellationHold&) = delete;
    CancellationHold& operator=(const CancellationHold&) = delete;

private:
    int previous_;
};

} // namespace stxlib::detail
