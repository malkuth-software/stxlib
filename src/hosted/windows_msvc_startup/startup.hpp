// The startup shim for the MSVC target (x86_64-pc-windows-msvc, compiled
// by the pinned clang): the pieces Microsoft's static startup library
// (vcstartup, the objects inside msvcrt.lib and libcmt.lib) supplies to
// every image, written over documented UCRT and kernel32 exports. Stage 1
// of the MSVC ABI study (projects desks/coordinator/MSVC_ABI_EH_RUNTIME_STUDY.md,
// sections 5 and 6): the entry points, the initializer sections, the TLS
// directory and dynamic TLS initialization, thread-safe statics, the
// security cookie, the load configuration, __chkstk, _fltused, atexit and
// (for a libc++ built in vcruntime mode) operator new and delete. The C++
// exception runtime, RTTI, terminate and the string group stay with
// vcruntime until Stage 2 replaces them (a separate STXlib branch).
//
// Every image (program or DLL) links this archive statically and owns its
// own copy of everything here: its initializer tables, its exit table, its
// TLS directory and its cookie. Nothing here is shared between images and
// nothing is imposed on foreign code: a DLL built by anyone else keeps its
// own runtime.
//
// References (all public): Microsoft Learn "CRT initialization" (ms.date
// 2021-08-02), "PE Format" (.tls and load configuration sections),
// "__security_init_cookie" (2016-11-04), "DllMain entry point", "x64
// prolog and epilog" (stack probes), "/GS (Buffer Security Check)",
// "Control Flow Guard"; the Windows SDK 10.0.28000.0 headers winnt.h
// (IMAGE_TLS_DIRECTORY64, IMAGE_LOAD_CONFIG_DIRECTORY64) and the UCRT's
// corecrt_startup.h; LLVM 22.1.8 clang/lib/CodeGen/MicrosoftCXXABI.cpp
// (what clang references: __dyn_tls_init, __dyn_tls_on_demand_init,
// __tls_guard, __tlregdtor, _Init_thread_header/footer/abort/epoch); and
// the N2325 appendix (Crowl, "Dynamic Initialization and Destruction with
// Concurrency") that clang's guard protocol cites. No Microsoft runtime
// source was read.

#pragma once

#if !defined(_MSC_VER) || defined(__MINGW32__)
#error "the MSVC-target startup shim is built for x86_64-pc-windows-msvc only"
#endif

#include "hosted/windows/msvc_runtime/system_interim.h"

namespace stxlib::msvc_startup {

// How the image was entered, set once by its entry point before any
// initializer runs and read by atexit: a program registers exit functions
// with the UCRT's process table, a DLL with its own module table that its
// DLL_PROCESS_DETACH runs.
enum class ImageKind : int {
    program = 0,
    library = 1,
};

void set_image_kind(ImageKind kind) noexcept;
[[nodiscard]] ImageKind image_kind() noexcept;

// The DLL's own exit and quick-exit tables, initialized by its entry point
// (exit_table.cpp).
[[nodiscard]] bool initialize_module_exit_tables() noexcept;
void run_module_exit_table() noexcept;

// The C and C++ initializer tables (sections.cpp): .CRT$XIA..XIZ return int
// (nonzero refuses start), .CRT$XCA..XCZ return nothing.
[[nodiscard]] int run_c_initializers() noexcept;
void run_cxx_initializers() noexcept;

// Dynamic TLS initialization of the calling thread (tls.cpp): runs the
// .CRT$XDA..XDZ initializers once per thread, guarded by __tls_guard.
void initialize_thread_locals() noexcept;
// Runs and forgets the calling thread's thread_local destructors in the
// reverse order of registration.
void run_thread_local_destructors() noexcept;
// The program's exit hook for the main thread's thread_local destructors,
// handed to the UCRT's _register_thread_local_exe_atexit_callback.
void register_program_thread_local_exit() noexcept;

// The program's unhandled-exception filter: a C++ exception (code
// 0xE06D7363) that no handler caught calls std::terminate's host form, the
// UCRT's terminate(); anything else continues to the system's handling.
void install_unhandled_exception_filter() noexcept;

}  // namespace stxlib::msvc_startup

extern "C" void __cdecl __security_init_cookie(void);
