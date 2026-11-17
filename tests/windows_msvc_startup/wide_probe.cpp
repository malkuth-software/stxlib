// wmain: the startup shim's wide entry point. Returns the argument count
// (the program and two arguments: 3), or 100 when the second argument did
// not survive with its space.

#include <cwchar>

int wmain(int argument_count, wchar_t** arguments) {
    if (argument_count == 3 && std::wcscmp(arguments[2], L"two words") != 0) {
        return 100;
    }
    return argument_count;
}
