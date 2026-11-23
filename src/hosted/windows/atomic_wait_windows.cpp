// The atomic-wait module for Windows (hosted/atomic_wait_backend.hpp):
// WaitOnAddress, WakeByAddressSingle and WakeByAddressAll (Windows 8 and
// later), the calls under the Microsoft STL's std::atomic wait on Windows 8
// and later, and under libc++'s on Windows.
//
// Microsoft's "WaitOnAddress function" page (from the published page, not
// re-read in this batch): it sleeps while the AddressSize bytes at Address
// equal those at CompareAddress, compared atomically with going to sleep;
// it returns TRUE when woken (or at once when the values differ, or
// spuriously), and FALSE with GetLastError() == ERROR_TIMEOUT when
// dwMilliseconds passed. WakeByAddressSingle wakes one waiter on the
// address, WakeByAddressAll every one; neither fails. The waits are
// process-private, as atomic_word.hpp requires. The three live in the API
// set api-ms-win-core-synch-l1-2-0 (import library synchronization).
//
// Milliseconds are rounded up so a wait never ends before its interval,
// and kept below INFINITE (which would never time out); a longer wait ends
// early and core waits again.

#include "hosted/atomic_wait_backend.hpp"

#include "stxlib/fail_fast.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib::atomic_wait_backend {

namespace {

[[noreturn]] void defect(const TextView message, const DWORD error) noexcept {
    const ErrorRecord record = ErrorRecord::make(
        ErrorKind::internal_failure, "stxlib.atomic_word",
        ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(error)),
                    .message = message});
    fail_fast(record);
}

DWORD milliseconds_of(const Duration interval) noexcept {
    const i64 nanoseconds = interval.count_nanoseconds();
    const i64 whole = nanoseconds / 1000000;
    const i64 rounded = nanoseconds % 1000000 == 0 ? whole : whole + 1;
    constexpr i64 longest = static_cast<i64>(INFINITE) - 1;
    const DWORD result = static_cast<DWORD>(rounded > longest ? longest : rounded);
    return result;
}

} // namespace

// The system only reads the word and the comparand; the calls take
// non-const addresses.
bool wait(const u32* const word, const u32 expected, const bool timed, const Duration interval) noexcept {
    u32 comparand = expected;
    const DWORD milliseconds = timed ? milliseconds_of(interval) : INFINITE;
    if (WaitOnAddress(const_cast<u32*>(word), &comparand, sizeof comparand, milliseconds)) return false;
    const DWORD error = GetLastError();
    if (error == ERROR_TIMEOUT) return true;
    defect("WaitOnAddress failed", error);
}

void wake_one(const u32* const word) noexcept { WakeByAddressSingle(const_cast<u32*>(word)); }

void wake_all(const u32* const word) noexcept { WakeByAddressAll(const_cast<u32*>(word)); }

} // namespace stxlib::atomic_wait_backend
