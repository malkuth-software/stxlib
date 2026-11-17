// wWinMainCRTStartup: the entry point for a program that defines wWinMain
// (windows subsystem by default).
//
// WinMain's arguments (Learn "WinMain function"): the image's base as its
// instance handle, a null previous instance, the command line without the
// program name (the UCRT's _get_wide_winmain_command_line), and the show
// command from the process's startup information when it carries one
// (STARTF_USESHOWWINDOW), SW_SHOWDEFAULT otherwise.

#include "hosted/windows_msvc_startup/program_startup.hpp"

#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" __attribute__((no_stack_protector)) int wWinMainCRTStartup(void) {
    stxlib::msvc_startup::start_program(stxlib::msvc_startup::ArgumentForm::wide, _crt_gui_app);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    int show_command = SW_SHOWDEFAULT;
    if ((startup.dwFlags & STARTF_USESHOWWINDOW) != 0) {
        show_command = startup.wShowWindow;
    }
    HINSTANCE const instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    const int result = wWinMain(instance, nullptr, _get_wide_winmain_command_line(), show_command);
    exit(result);
}
