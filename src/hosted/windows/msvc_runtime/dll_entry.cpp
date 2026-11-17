// The shared runtime's entry point (stxlib_msvc_runtime.dll), named by
// /ENTRY at its link, and the image's atexit. At process attach it runs the
// image's own C++ initialisers, those of the static C++ library linked into
// it, through the UCRT's _initterm (Microsoft Learn, "_initterm,
// _initterm_e"; "CRT initialization": the linker orders .CRT$XCA, the
// compiler's .CRT$XCU entries and .CRT$XCZ alphabetically). Destructors those
// initialisers register through atexit go to a table of this module's own
// (Microsoft Learn, "_register_onexit_function, _initialize_onexit_table,
// _execute_onexit_table"), run at process detach in reverse order, as a DLL
// linked with Microsoft's startup objects runs them. The runtime's own state
// needs no initialiser (constant initialisation and TLS slots allocated on
// first use, runtime_state.cpp).
//
// The conformance suite's test DLL compiles this same file under another
// entry name (STXLIB_MSVC_DLL_ENTRY), until the startup shim supplies one.

#include "stxlib/types.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifndef STXLIB_MSVC_DLL_ENTRY
#define STXLIB_MSVC_DLL_ENTRY stxlib_msvc_runtime_entry
#endif

namespace {

using Initializer = void(__cdecl*)();

const Initializer first_initializer __attribute__((section(".CRT$XCA"), used)) = nullptr;
const Initializer last_initializer __attribute__((section(".CRT$XCZ"), used)) = nullptr;

} // namespace

extern "C" {

// UCRT (corecrt_startup.h, _onexit_table_t): three pointers, the first and
// last entry and the end of a heap block the UCRT grows.
struct ExitTable {
    Initializer* first;
    Initializer* last;
    Initializer* end;
};

using OnExit = int(__cdecl*)();

void __cdecl _initterm(Initializer* first, Initializer* last);
int __cdecl _initialize_onexit_table(ExitTable* table);
int __cdecl _register_onexit_function(ExitTable* table, OnExit function);
int __cdecl _execute_onexit_table(ExitTable* table);

} // extern "C"

namespace {

ExitTable module_exit_table = {nullptr, nullptr, nullptr};

} // namespace

extern "C" {

// The table's entries are called as int(void) functions whose result is
// ignored (the documented _onexit_t); a void(void) function called so on
// x64 leaves RAX unread.
int __cdecl atexit(void(__cdecl* const function)()) {
    const int registered = _register_onexit_function(&module_exit_table, reinterpret_cast<OnExit>(function));
    return registered == 0 ? 0 : -1;
}

BOOL WINAPI STXLIB_MSVC_DLL_ENTRY(HINSTANCE const instance, DWORD const reason, void* const reserved) {
    static_cast<void>(instance);
    static_cast<void>(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        if (_initialize_onexit_table(&module_exit_table) != 0) return FALSE;
        _initterm(const_cast<Initializer*>(&first_initializer), const_cast<Initializer*>(&last_initializer));
    }
    if (reason == DLL_PROCESS_DETACH) {
        _execute_onexit_table(&module_exit_table);
    }
    return TRUE;
}

} // extern "C"
