/* The conformance programs' entry point when linked with STXlib's runtime
 * instead of Microsoft's startup objects (the startup shim of Stages 0 and
 * 1 replaces this file once it lands): it installs the runtime's
 * unhandled-exception filter, which sends an uncaught C++ exception to
 * std::terminate, runs the image's C++ initialisers, calls main and leaves
 * through the UCRT's exit, which runs the atexit table and flushes stdio.
 * The programs take no arguments, so no argv is built.
 *
 * Microsoft Learn: "SetUnhandledExceptionFilter", "_initterm, _initterm_e",
 * "CRT initialization", "_crt_atexit, _crt_at_quick_exit", "exit, _Exit,
 * _exit". */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef void(__cdecl* initializer)(void);

__attribute__((section(".CRT$XCA"), used)) const initializer test_first_initializer = 0;
__attribute__((section(".CRT$XCZ"), used)) const initializer test_last_initializer = 0;

void __cdecl _initterm(initializer* first, initializer* last);
int __cdecl _crt_atexit(initializer function);
__declspec(noreturn) void __cdecl exit(int code);
LONG WINAPI stxlib_msvc_unhandled_exception_filter(EXCEPTION_POINTERS* pointers);
/* corecrt_startup.h: _crt_console_app is 1; it selects console reporting
 * for the UCRT's own diagnostics, as Microsoft's console startup does. */
void __cdecl _set_app_type(int type);

int main(void);

int __cdecl atexit(initializer function) {
    return _crt_atexit(function);
}

int mainCRTStartup(void) {
    _set_app_type(1);
    SetUnhandledExceptionFilter(stxlib_msvc_unhandled_exception_filter);
    _initterm((initializer*)&test_first_initializer, (initializer*)&test_last_initializer);
    const int code = main();
    exit(code);
}
