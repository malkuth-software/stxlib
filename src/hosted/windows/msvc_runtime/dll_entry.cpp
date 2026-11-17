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
// entry name (STXLIB_MSVC_DLL_ENTRY). The startup shim's entry_dll.cpp does
// the same for every other DLL; an image links one or the other.

#include "system_interim.h"

#include "stxlib/types.hpp"

#ifndef STXLIB_MSVC_DLL_ENTRY
#define STXLIB_MSVC_DLL_ENTRY stxlib_msvc_runtime_entry
#endif

namespace {

const _PVFV first_initializer __attribute__((section(".CRT$XCA"), used)) = nullptr;
const _PVFV last_initializer __attribute__((section(".CRT$XCZ"), used)) = nullptr;

_onexit_table_t module_exit_table = {nullptr, nullptr, nullptr};

} // namespace

extern "C" {

// The table's entries are called as int(void) functions whose result is
// ignored (the documented _onexit_t); a void(void) function called so on
// x64 leaves RAX unread.
int __cdecl atexit(_PVFV const function) {
    const int registered = _register_onexit_function(&module_exit_table, reinterpret_cast<_onexit_t>(function));
    return registered == 0 ? 0 : -1;
}

BOOL WINAPI STXLIB_MSVC_DLL_ENTRY(HINSTANCE const instance, DWORD const reason, void* const reserved) {
    static_cast<void>(instance);
    static_cast<void>(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        if (_initialize_onexit_table(&module_exit_table) != 0) return FALSE;
        // _initterm takes the table's bounds as writable pointers; it only
        // reads them.
        _initterm(const_cast<_PVFV*>(&first_initializer), const_cast<_PVFV*>(&last_initializer));
    }
    if (reason == DLL_PROCESS_DETACH) {
        _execute_onexit_table(&module_exit_table);
    }
    return TRUE;
}

} // extern "C"
