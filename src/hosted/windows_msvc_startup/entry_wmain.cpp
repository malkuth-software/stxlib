// wmainCRTStartup: the entry point for a program that defines wmain.

#include "hosted/windows_msvc_startup/program_startup.hpp"

#include <stdlib.h>

extern "C" int wmain(int argument_count, wchar_t** arguments, wchar_t** environment);

extern "C" __attribute__((no_stack_protector)) int wmainCRTStartup(void) {
    stxlib::msvc_startup::start_program(stxlib::msvc_startup::ArgumentForm::wide, _crt_console_app);
    const int result = wmain(*__p___argc(), *__p___wargv(), _get_initial_wide_environment());
    exit(result);
}
