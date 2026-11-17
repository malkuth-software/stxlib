// The stack-protector cookie (clang's -fstack-protector on the MSVC
// target, the counterpart of /GS).
//
// Clang stores __security_cookie, combined with the frame address, in each
// protected frame and calls __security_check_cookie with the combined
// value on the way out. Learn "__security_init_cookie" (2016-11-04): the
// cookie must be set before any protected function runs, and an image that
// supplies its own entry point calls __security_init_cookie itself; every
// entry point here calls it first, so this file and the entry points are
// built without the stack protector.
//
// The initial value is the conventional one the loader recognizes as not
// yet chosen (0x00002B992DDFA232; unconfirmed against a Microsoft page:
// the load configuration's SecurityCookie field points the loader at this
// variable, and Windows replaces the default with a random value at load).
// __security_init_cookie keeps a value the loader has already chosen and
// otherwise derives one from the performance counter, the system time,
// the process and thread identities and a stack address, mixed by the
// SplitMix64 finalizer (Steele, Lea and Flood, "Fast splittable
// pseudorandom number generators", 2014). The top 16 bits are kept clear,
// as the conventional value's are, so that a cookie never looks like a
// canonical address (a hardening convention, not a requirement).
//
// A mismatch ends the process at once with __fastfail
// (FAST_FAIL_STACK_COOKIE_CHECK_FAILURE, winnt.h; Learn "__fastfail"):
// no handler, no unwinding, a Windows Error Reporting record with
// STATUS_STACK_BUFFER_OVERRUN.

#include "hosted/windows_msvc_startup/startup.hpp"

#include <stdint.h>

namespace {

constexpr uint64_t default_security_cookie = 0x00002B992DDFA232ULL;
constexpr uint64_t security_cookie_mask = 0x0000FFFFFFFFFFFFULL;

uint64_t mix_seed(const uint64_t value) noexcept {
    uint64_t mixed = value + 0x9E3779B97F4A7C15ULL;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
    mixed = mixed ^ (mixed >> 31);
    return mixed;
}

}  // namespace

extern "C" {

constinit uintptr_t __security_cookie = default_security_cookie;
constinit uintptr_t __security_cookie_complement = ~default_security_cookie;

__attribute__((no_stack_protector)) void __cdecl __security_init_cookie(void) {
    if (__security_cookie != default_security_cookie) {
        __security_cookie_complement = ~__security_cookie;
        return;
    }
    LARGE_INTEGER counter{};
    static_cast<void>(QueryPerformanceCounter(&counter));
    FILETIME system_time{};
    GetSystemTimePreciseAsFileTime(&system_time);
    const uint64_t time_value = (static_cast<uint64_t>(system_time.dwHighDateTime) << 32) | system_time.dwLowDateTime;
    const uint64_t identities = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) | GetCurrentThreadId();
    int stack_marker = 0;
    const uint64_t stack_address = reinterpret_cast<uint64_t>(&stack_marker);

    uint64_t cookie = mix_seed(static_cast<uint64_t>(counter.QuadPart));
    cookie = mix_seed(cookie ^ time_value);
    cookie = mix_seed(cookie ^ identities);
    cookie = mix_seed(cookie ^ stack_address);
    cookie = cookie & security_cookie_mask;
    if (cookie == default_security_cookie || cookie == 0) {
        cookie = default_security_cookie ^ 0x0000174A00000001ULL;
    }
    __security_cookie = cookie;
    __security_cookie_complement = ~cookie;
}

__attribute__((noreturn, no_stack_protector)) void __cdecl __report_gsfailure(uintptr_t cookie) {
    static_cast<void>(cookie);
    __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

__attribute__((no_stack_protector)) void __cdecl __security_check_cookie(uintptr_t cookie) {
    if (cookie != __security_cookie) {
        __report_gsfailure(cookie);
    }
}

}  // extern "C"
