// _DllMainCRTStartup: the entry point the linker selects for a DLL.
//
// Learn "DllMain entry point": the loader calls the entry point with
// DLL_PROCESS_ATTACH when the DLL is mapped (under the loader lock),
// DLL_THREAD_ATTACH and DLL_THREAD_DETACH for threads that start and end
// while it is loaded, and DLL_PROCESS_DETACH when it is unmapped (reserved
// non-null when the process is ending). A FALSE return from
// DLL_PROCESS_ATTACH makes the load fail.
//
// Process attach: the cookie, the image kind, the DLL's own exit tables,
// the C initializers (a refusal fails the load), the C++ initializers, the
// loading thread's thread_locals, then the user's DllMain. If DllMain
// refuses, the static objects already constructed are destroyed before
// FALSE is returned. Process detach: DllMain first, then the calling
// thread's thread_local destructors, then the exit table (static
// destructors and atexit functions, in reverse order of registration).
//
// A DLL that defines no DllMain gets one that accepts every call, chosen
// with the linker's /alternatename (the user's DllMain wins when it
// exists).

#include "hosted/windows_msvc_startup/startup.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma comment(linker, "/INCLUDE:_load_config_used")
#pragma comment(linker, "/alternatename:DllMain=stx_msvc_startup_default_dll_main")

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved);

extern "C" BOOL WINAPI stx_msvc_startup_default_dll_main(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

namespace {

BOOL attach_process(HINSTANCE const instance, const DWORD reason, LPVOID const reserved) noexcept {
    __security_init_cookie();
    stxlib::msvc_startup::set_image_kind(stxlib::msvc_startup::ImageKind::library);
    const bool tables_ready = stxlib::msvc_startup::initialize_module_exit_tables();
    if (!tables_ready) {
        return FALSE;
    }
    const int refused = stxlib::msvc_startup::run_c_initializers();
    if (refused != 0) {
        return FALSE;
    }
    stxlib::msvc_startup::run_cxx_initializers();
    stxlib::msvc_startup::initialize_thread_locals();
    const BOOL accepted = DllMain(instance, reason, reserved);
    if (accepted == FALSE) {
        stxlib::msvc_startup::run_thread_local_destructors();
        stxlib::msvc_startup::run_module_exit_table();
    }
    return accepted;
}

BOOL detach_process(HINSTANCE const instance, const DWORD reason, LPVOID const reserved) noexcept {
    const BOOL result = DllMain(instance, reason, reserved);
    stxlib::msvc_startup::run_thread_local_destructors();
    stxlib::msvc_startup::run_module_exit_table();
    return result;
}

}  // namespace

extern "C" __attribute__((no_stack_protector)) BOOL WINAPI _DllMainCRTStartup(HINSTANCE instance, DWORD reason,
                                                                              LPVOID reserved) {
    BOOL result = TRUE;
    if (reason == DLL_PROCESS_ATTACH) {
        result = attach_process(instance, reason, reserved);
    } else if (reason == DLL_PROCESS_DETACH) {
        result = detach_process(instance, reason, reserved);
    } else {
        result = DllMain(instance, reason, reserved);
    }
    return result;
}
