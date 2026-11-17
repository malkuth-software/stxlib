// Termination and the vcruntime-shaped hooks around it: __std_terminate
// (what clang calls when an exception reaches a noexcept boundary,
// clang/lib/CodeGen/MicrosoftCXXABI.cpp and CGException.cpp on this
// target), _purecall (a pure or deleted virtual call), set_unexpected,
// _get_unexpected and unexpected (libc++'s exception_msvc.ipp calls the
// first two), and the unhandled-exception filter a startup installs so that
// an exception no handler takes ends in std::terminate ([except.handle]p9).
//
// terminate itself, set_terminate and _get_terminate are the UCRT's,
// documented exports of ucrtbase (Microsoft Learn, "terminate (CRT)",
// "set_terminate (CRT)"); libc++ routes std::set_terminate there too, so
// one handler serves both.

#include "runtime_state.hpp"

#include "stxlib/detail/early_exit.hpp"

#include <atomic>

namespace stxlib::msvc_runtime {

void terminate_now() noexcept {
    const early_exit::NoExit terminate_marker{"std::terminate ends the process; a handler that returns is followed by abort"};
    ::terminate();
}

// FAST_FAIL_FATAL_APP_EXIT (winnt.h): the process ends at once, no handler
// or filter runs, and Windows Error Reporting records the code.
void fail_fast() noexcept {
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

namespace {

using UnexpectedHandler = void(__cdecl*)();

// Process-wide, like the UCRT's terminate handler. One copy per runtime
// instance: the shared runtime has one for the program.
static_assert(std::atomic<UnexpectedHandler>::is_always_lock_free, "atomic<UnexpectedHandler> must be lock-free");
constinit std::atomic<UnexpectedHandler> unexpected_handler{nullptr};

} // namespace

} // namespace stxlib::msvc_runtime

extern "C" {

[[noreturn]] void __std_terminate() {
    stxlib::msvc_runtime::terminate_now();
}

int _purecall() {
    stxlib::msvc_runtime::terminate_now();
}

void(__cdecl* set_unexpected(void(__cdecl* const handler)()))() {
    using stxlib::msvc_runtime::UnexpectedHandler;
    const UnexpectedHandler previous = stxlib::msvc_runtime::unexpected_handler.exchange(handler, std::memory_order_acq_rel);
    return previous;
}

void(__cdecl* _get_unexpected())() {
    using stxlib::msvc_runtime::UnexpectedHandler;
    const UnexpectedHandler current = stxlib::msvc_runtime::unexpected_handler.load(std::memory_order_acquire);
    return current;
}

[[noreturn]] void unexpected() {
    using stxlib::msvc_runtime::UnexpectedHandler;
    const UnexpectedHandler current = stxlib::msvc_runtime::unexpected_handler.load(std::memory_order_acquire);
    if (current != nullptr) {
        const stxlib::early_exit::Whole handler_marker{"the runtime holds no state across the handler"};
        current();
    }
    stxlib::msvc_runtime::terminate_now();
}

// For a startup's SetUnhandledExceptionFilter (Microsoft Learn,
// "SetUnhandledExceptionFilter function"): a C++ exception that reached no
// handler terminates; anything else goes on to the system's default.
LONG WINAPI stxlib_msvc_unhandled_exception_filter(EXCEPTION_POINTERS* const pointers) {
    const EXCEPTION_RECORD* const record = (*pointers).ExceptionRecord;
    if (record != nullptr && stxlib::msvc_runtime::is_cxx_exception(*record)) stxlib::msvc_runtime::terminate_now();
    return EXCEPTION_CONTINUE_SEARCH;
}

} // extern "C"
