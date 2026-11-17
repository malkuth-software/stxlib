// mainCRTStartup: the entry point the linker selects for a program that
// defines main (console subsystem by default).

#include "hosted/windows_msvc_startup/program_startup.hpp"

#include <stdlib.h>

// The program's main, by its symbol: C++ forbids declaring or calling main
// itself, so it is named here through an assembly label. main may be
// defined with no parameters, two or three; on x64 the caller passes three
// and a shorter definition ignores the rest. Its symbol is unmangled on
// this target.
extern "C" int stx_msvc_startup_program_main(int argument_count, char** arguments, char** environment)
    __asm__("main");

extern "C" __attribute__((no_stack_protector)) int mainCRTStartup(void) {
    stxlib::msvc_startup::start_program(stxlib::msvc_startup::ArgumentForm::narrow, _crt_console_app);
    const int result = stx_msvc_startup_program_main(*__p___argc(), *__p___argv(), _get_initial_narrow_environment());
    exit(result);
}
