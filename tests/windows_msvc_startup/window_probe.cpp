// WinMain: the startup shim's windows-subsystem entry point. Returns the
// length of the command line without the program name ("alpha beta": 10)
// plus one when the instance handle is the image's base.

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_command) {
    (void)previous;
    (void)show_command;
    const int length = static_cast<int>(std::strlen(command_line));
    int base_matches = 0;
    if (instance == GetModuleHandleW(nullptr)) {
        base_matches = 1;
    }
    const int result = length + base_matches;
    return result;
}
