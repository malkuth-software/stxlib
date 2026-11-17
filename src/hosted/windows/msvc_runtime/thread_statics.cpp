// Thread-safe initialisation of function-local statics, per module: the
// three functions and the per-thread epoch clang calls and reads for a
// guarded static on this target (clang's MicrosoftCXXABI.cpp, EmitGuardedInit,
// getInitThreadHeaderFn, getInitThreadFooterFn, getInitThreadAbortFn,
// getInitThreadEpochPtr). The code clang emits around each such static:
//
//   if (guard > _Init_thread_epoch) {        // unordered load of guard
//       _Init_thread_header(&guard);
//       if (guard == -1) {
//           ... initialise; on an exception: _Init_thread_abort(&guard) ...
//           _Init_thread_footer(&guard);
//       }
//   }
//
// The algorithm is the one clang's comment names, the appendix of WG21
// N2325 ("Dynamic Initialization and Destruction with Concurrency"): a
// guard is 0 before initialisation, -1 while one thread initialises, and
// afterwards the value of a global epoch that every completed
// initialisation advances. Each thread keeps the last epoch it has
// synchronised with; a guard at or below it is initialised and visible to
// that thread without a lock. Both epochs start at INT_MIN, so every
// completed guard (INT_MIN + 1 and above, far below -1) compares greater
// than a fresh thread's epoch exactly once.
//
// One lock and one condition variable per module serialise the slow paths
// (Microsoft Learn, "Slim Reader/Writer (SRW) Locks" and "Condition
// Variables"; both statically initialisable). A thread that finds another
// initialising waits; the footer and the abort wake every waiter.
//
// _Init_thread_epoch is a thread_local with a constant initialiser, so it
// lives in the image's TLS template and needs the image's TLS directory
// (image_tls.c, or the startup objects that replace it).

#include "stxlib/types.hpp"

#include <limits.h>

#include "system_interim.h"

namespace {

constexpr int guard_uninitialised = 0;
constexpr int guard_initialising = -1;

SRWLOCK guard_lock = SRWLOCK_INIT;
CONDITION_VARIABLE guard_changed = CONDITION_VARIABLE_INIT;
// Written only under guard_lock; read only under it.
int global_epoch = INT_MIN;

void store_guard(int* const guard, const int value) {
    __atomic_store_n(guard, value, __ATOMIC_RELEASE);
}

} // namespace

extern "C" {

thread_local int _Init_thread_epoch = INT_MIN;

void _Init_thread_header(int* const guard) {
    AcquireSRWLockExclusive(&guard_lock);
    for (;;) {
        const int state = __atomic_load_n(guard, __ATOMIC_RELAXED);
        if (state == guard_uninitialised) {
            store_guard(guard, guard_initialising);
            break;
        }
        if (state != guard_initialising) {
            // Initialised by another thread: this thread has now seen every
            // initialisation completed so far.
            _Init_thread_epoch = global_epoch;
            break;
        }
        SleepConditionVariableSRW(&guard_changed, &guard_lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&guard_lock);
}

void _Init_thread_footer(int* const guard) {
    AcquireSRWLockExclusive(&guard_lock);
    global_epoch = global_epoch + 1;
    store_guard(guard, global_epoch);
    _Init_thread_epoch = global_epoch;
    ReleaseSRWLockExclusive(&guard_lock);
    WakeAllConditionVariable(&guard_changed);
}

void _Init_thread_abort(int* const guard) {
    AcquireSRWLockExclusive(&guard_lock);
    store_guard(guard, guard_uninitialised);
    ReleaseSRWLockExclusive(&guard_lock);
    WakeAllConditionVariable(&guard_changed);
}

} // extern "C"
