// The sequence every program entry point shares, and the unhandled
// exception filter.
//
// Order, from Learn "CRT initialization" and the UCRT's corecrt_startup.h
// (the study's section 5): the security cookie first, before anything that
// may be stack-protected; then the application type, which decides where
// the UCRT reports errors; the arguments and environment; the C
// initializers, any of which may refuse the start; the C++ initializers;
// the main thread's thread_local initializers and their exit hook. The
// entry point then calls the user's main and passes its result to exit(),
// which runs the exit table, the main thread's thread_local destructors
// first.
//
// Failures before main: a refused C initializer or an argument
// configuration the UCRT could not make ends the process with _exit(255)
// (no exit functions run: none are registered yet but the UCRT's own).

#include "hosted/windows_msvc_startup/program_startup.hpp"

#include <stdlib.h>

// The load configuration directory (load_config.cpp) is found by the
// linker only by name; every image with an entry point here keeps it.
#pragma comment(linker, "/INCLUDE:_load_config_used")

namespace stxlib::msvc_startup {

namespace {

// The code RaiseException carries for a C++ exception thrown on this ABI
// (the study's section 1.3: 'msc' with the customer bit set).
constexpr DWORD cxx_exception_code = 0xE06D7363;

LONG WINAPI filter_unhandled_exception(EXCEPTION_POINTERS* const pointers) {
    const DWORD code = (*(*pointers).ExceptionRecord).ExceptionCode;
    if (code == cxx_exception_code) {
        // std::terminate's host form; it calls the terminate handler, which
        // by default aborts. It does not return.
        terminate();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_unhandled_exception_filter() noexcept {
    static_cast<void>(SetUnhandledExceptionFilter(&filter_unhandled_exception));
}

void start_program(const ArgumentForm form, const _crt_app_type application_type) noexcept {
    __security_init_cookie();
    set_image_kind(ImageKind::program);
    _set_app_type(application_type);
    install_unhandled_exception_filter();

    int configured = 0;
    if (form == ArgumentForm::wide) {
        configured = _configure_wide_argv(_crt_argv_unexpanded_arguments);
        if (configured == 0) {
            configured = _initialize_wide_environment();
        }
    } else {
        configured = _configure_narrow_argv(_crt_argv_unexpanded_arguments);
        if (configured == 0) {
            configured = _initialize_narrow_environment();
        }
    }
    if (configured != 0) {
        _exit(255);
    }

    const int refused = run_c_initializers();
    if (refused != 0) {
        _exit(255);
    }
    run_cxx_initializers();
    register_program_thread_local_exit();
    initialize_thread_locals();
}

}  // namespace stxlib::msvc_startup
