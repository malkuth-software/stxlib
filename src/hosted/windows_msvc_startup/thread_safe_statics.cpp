// Thread-safe initialization of function-local statics: the protocol clang
// emits on the MSVC target (MicrosoftCXXABI.cpp, EmitGuardedInit, "the
// algorithm is almost identical to what can be found in the appendix found
// in N2325").
//
// Each guarded static has an int guard, zero before initialization. Clang's
// fast path compares the guard with the calling thread's _Init_thread_epoch
// and skips everything when the guard is not greater. Otherwise it calls
// _Init_thread_header(&guard); if the guard then reads -1, this thread
// initializes the object and calls _Init_thread_footer(&guard), or
// _Init_thread_abort(&guard) if the initializer throws.
//
// Guard values: 0 uninitialized; -1 being initialized by some thread; any
// other value is the global epoch at which initialization completed. The
// global epoch starts at INT_MIN and grows by one per completed
// initialization, skipping -1 and 0 so that a completed guard never reads
// as either. A thread's epoch is the global epoch it last observed under
// the lock; a guard greater than it may have completed since, and the
// header then just refreshes the thread's epoch. Exhausting the epoch
// (about four billion completed initializations in one image) ends the
// process: a wrapped epoch would let threads skip initializations they
// never observed.
//
// One SRW lock and one condition variable per image guard all statics
// (kernel32, Learn "Slim Reader/Writer (SRW) Locks" and "Condition
// Variables"; both statically initializable). Waiters wait on the
// condition variable; footer and abort wake them all, and each re-reads
// its own guard.

#include "hosted/windows_msvc_startup/startup.hpp"

#include <limits.h>
#include <stdlib.h>

namespace {

constexpr int guard_uninitialized = 0;
constexpr int guard_in_progress = -1;

constinit SRWLOCK guard_lock = SRWLOCK_INIT;
constinit CONDITION_VARIABLE guard_completed = CONDITION_VARIABLE_INIT;
// Guarded by guard_lock.
constinit int global_epoch = INT_MIN;

int next_epoch(const int epoch) noexcept {
    if (epoch == INT_MAX) {
        abort();
    }
    int next = epoch + 1;
    if (next == guard_in_progress) {
        next = 1;
    }
    return next;
}

}  // namespace

extern "C" {

constinit thread_local int _Init_thread_epoch = INT_MIN;

void __cdecl _Init_thread_header(int* const guard) noexcept {
    AcquireSRWLockExclusive(&guard_lock);
    for (;;) {
        if (*guard == guard_uninitialized) {
            *guard = guard_in_progress;
            break;
        }
        if (*guard == guard_in_progress) {
            static_cast<void>(SleepConditionVariableSRW(&guard_completed, &guard_lock, INFINITE, 0));
            continue;
        }
        _Init_thread_epoch = global_epoch;
        break;
    }
    ReleaseSRWLockExclusive(&guard_lock);
}

void __cdecl _Init_thread_footer(int* const guard) noexcept {
    AcquireSRWLockExclusive(&guard_lock);
    global_epoch = next_epoch(global_epoch);
    *guard = global_epoch;
    _Init_thread_epoch = global_epoch;
    ReleaseSRWLockExclusive(&guard_lock);
    WakeAllConditionVariable(&guard_completed);
}

void __cdecl _Init_thread_abort(int* const guard) noexcept {
    AcquireSRWLockExclusive(&guard_lock);
    *guard = guard_uninitialized;
    ReleaseSRWLockExclusive(&guard_lock);
    WakeAllConditionVariable(&guard_completed);
}

}  // extern "C"
