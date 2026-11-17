// atexit, _onexit and at_quick_exit, per image.
//
// A program registers with the UCRT's process-wide tables (_crt_atexit and
// _crt_at_quick_exit, corecrt_startup.h), which exit() and quick_exit()
// run. A DLL keeps its own tables (_onexit_table_t, filled by
// _register_onexit_function) and runs the exit table at its
// DLL_PROCESS_DETACH, after its DllMain, so that its static objects are
// destroyed while the DLL is still mapped, whether it is unloaded by
// FreeLibrary or by the process ending. The quick-exit table of a DLL is
// kept for at_quick_exit's contract but run by nobody: quick_exit runs the
// process table only, and a DLL's quick-exit functions are lost, as the
// standard allows for functions registered from a module that is not the
// program (unconfirmed against Microsoft's behaviour; not observable to
// GUI.Forms, which registers none).
//
// The tables are touched by the image's loading thread during its
// initialization and by any thread calling atexit later; the UCRT's
// _register_onexit_function locks the table itself (corecrt_startup.h
// declares no other contract; unconfirmed beyond that).

#include "hosted/windows_msvc_startup/startup.hpp"

#include <stdlib.h>

namespace stxlib::msvc_startup {

namespace {

constinit _onexit_table_t module_exit_table{._first = nullptr, ._last = nullptr, ._end = nullptr};
constinit _onexit_table_t module_quick_exit_table{._first = nullptr, ._last = nullptr, ._end = nullptr};

}  // namespace

bool initialize_module_exit_tables() noexcept {
    const int exit_status = _initialize_onexit_table(&module_exit_table);
    const int quick_status = _initialize_onexit_table(&module_quick_exit_table);
    const bool initialized = exit_status == 0 && quick_status == 0;
    return initialized;
}

void run_module_exit_table() noexcept {
    static_cast<void>(_execute_onexit_table(&module_exit_table));
}

}  // namespace stxlib::msvc_startup

extern "C" {

// Foreign-interface contract for the casts below: UCRT's tables store
// _onexit_t (int (*)(void)) while atexit and at_quick_exit take _PVFV
// (void (*)(void)). UCRT calls each entry and discards its result, and on
// x64 both types use the same call sequence (no arguments, result in RAX
// left unread), so either pointer type is called safely through the other.

// Returns 0 when registered, nonzero otherwise (the C standard's atexit).
int __cdecl atexit(_PVFV function) {
    int status = 0;
    if (stxlib::msvc_startup::image_kind() == stxlib::msvc_startup::ImageKind::library) {
        status = _register_onexit_function(&stxlib::msvc_startup::module_exit_table, reinterpret_cast<_onexit_t>(function));
    } else {
        status = _crt_atexit(function);
    }
    return status;
}

// Microsoft's _onexit: returns the function when registered, null
// otherwise.
_onexit_t __cdecl _onexit(_onexit_t function) {
    int status = 0;
    if (stxlib::msvc_startup::image_kind() == stxlib::msvc_startup::ImageKind::library) {
        status = _register_onexit_function(&stxlib::msvc_startup::module_exit_table, function);
    } else {
        status = _crt_atexit(reinterpret_cast<_PVFV>(function));
    }
    _onexit_t registered = nullptr;
    if (status == 0) {
        registered = function;
    }
    return registered;
}

int __cdecl at_quick_exit(_PVFV function) {
    int status = 0;
    if (stxlib::msvc_startup::image_kind() == stxlib::msvc_startup::ImageKind::library) {
        status = _register_onexit_function(&stxlib::msvc_startup::module_quick_exit_table,
                                           reinterpret_cast<_onexit_t>(function));
    } else {
        status = _crt_at_quick_exit(function);
    }
    return status;
}

}  // extern "C"
