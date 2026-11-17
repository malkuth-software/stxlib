// The image's TLS directory, dynamic TLS initialization and thread_local
// destructors.
//
// The TLS directory (Learn "PE Format", section ".tls"; winnt.h
// IMAGE_TLS_DIRECTORY64): the template's start and end, the address the
// loader writes the module's TLS index to, and a null-terminated array of
// TLS callbacks. The linker finds it by the name _tls_used. The template
// is everything in the .tls subsections, which the linker sorts by the
// name after '$': _tls_start sits in plain .tls, before clang's thread_local
// data (.tls$), and _tls_end in .tls$ZZZ after it. The callbacks lie
// between the null sentinels __xl_a (.CRT$XLA) and __xl_z (.CRT$XLZ);
// AddressOfCallBacks names the slot after __xl_a, and __xl_z ends the
// array. STXlib's own callbacks sit in .CRT$XLY.
//
// The directory is written in assembly because its fields are addresses
// (VAs with base relocations), which a C++ initializer could only express
// through integer casts the compiler may turn into dynamic initialization:
// a TLS directory filled at run time would be read by the loader before it
// was written.
//
// Dynamic initialization (MicrosoftCXXABI.cpp, EmitThreadLocalInitFuncs and
// EmitThreadLocalVarDeclLValue): clang puts each thread_local's initializer
// in .CRT$XDU, references __dyn_tls_init with /include, and before each use
// of such a variable tests the per-thread byte __tls_guard and calls
// __dyn_tls_on_demand_init while it is zero. Here the initializers run
// once per thread: eagerly for a thread that starts while the image is
// loaded (the __dyn_tls_init callback, DLL_THREAD_ATTACH), from the entry
// point for the thread that loads the image, and on demand otherwise
// (threads that already existed when a DLL was loaded).
//
// Destructors (emitGlobalDtorWithTLRegDtor): clang registers a
// thread_local's destructor stub with __tlregdtor on its first
// initialization. Each thread keeps its stubs in a list on the heap and
// runs them in reverse at its DLL_THREAD_DETACH (Learn "DllMain": sent on
// the exiting thread when it ends cleanly; never for TerminateThread or at
// process termination). The program's main thread runs them from the UCRT's
// exit through _register_thread_local_exe_atexit_callback (process.h); a
// DLL's loading thread runs them at its DLL_PROCESS_DETACH (entry_dll.cpp).

#include "hosted/windows_msvc_startup/startup.hpp"

#include <process.h>
#include <stdint.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {

// The loader writes the module's TLS index here (AddressOfIndex).
constinit unsigned long _tls_index = 0;

__attribute__((used, section(".tls"))) constinit char _tls_start = 0;
__attribute__((used, section(".tls$ZZZ"))) constinit char _tls_end = 0;

__attribute__((used, section(".CRT$XLA"))) extern const PIMAGE_TLS_CALLBACK __xl_a = nullptr;
__attribute__((used, section(".CRT$XLZ"))) extern const PIMAGE_TLS_CALLBACK __xl_z = nullptr;

__attribute__((used, section(".CRT$XDA"))) extern const _PVFV __xd_a = nullptr;
__attribute__((used, section(".CRT$XDZ"))) extern const _PVFV __xd_z = nullptr;

// Nonzero once the calling thread's dynamic TLS initializers have run.
constinit thread_local unsigned char __tls_guard = 0;

void NTAPI __dyn_tls_init(PVOID module, DWORD reason, PVOID reserved);
void NTAPI stx_msvc_startup_tls_detach(PVOID module, DWORD reason, PVOID reserved);

__attribute__((used, section(".CRT$XLC"))) extern const PIMAGE_TLS_CALLBACK __dyn_tls_init_callback = &__dyn_tls_init;
__attribute__((used, section(".CRT$XLD"))) extern const PIMAGE_TLS_CALLBACK stx_msvc_startup_tls_detach_callback =
    &stx_msvc_startup_tls_detach;

}  // extern "C"

// IMAGE_TLS_DIRECTORY64: StartAddressOfRawData, EndAddressOfRawData,
// AddressOfIndex, AddressOfCallBacks (8-byte VAs), SizeOfZeroFill,
// Characteristics (4 bytes each); 40 bytes in all.
static_assert(sizeof(IMAGE_TLS_DIRECTORY64) == 40, "the TLS directory below is written for winnt.h's 40-byte layout");
__asm__(
    "\t.section .rdata$T,\"dr\"\n"
    "\t.globl _tls_used\n"
    "\t.p2align 3\n"
    "_tls_used:\n"
    "\t.quad _tls_start\n"
    "\t.quad _tls_end\n"
    "\t.quad _tls_index\n"
    "\t.quad __xl_a+8\n"
    "\t.long 0\n"
    "\t.long 0\n"
    "\t.text\n");

namespace stxlib::msvc_startup {

namespace {

// The calling thread's registered destructor stubs: `entries` holds
// `capacity` slots, of which the first `count` are live, in registration
// order. Owned by the thread; released when they have run.
struct ThreadDestructorList {
    _PVFV* entries;
    size_t count;
    size_t capacity;
};

constinit thread_local ThreadDestructorList thread_destructors{.entries = nullptr, .count = 0, .capacity = 0};

constexpr size_t first_destructor_capacity = 8;

// The .CRT$XD table's bounds as integers: the entries between the two
// sentinels belong to other objects, so they are reached by address, not
// by indexing the sentinel.
void run_thread_local_initializers() noexcept {
    const uintptr_t first = reinterpret_cast<uintptr_t>(&__xd_a) + sizeof(_PVFV);
    const uintptr_t last = reinterpret_cast<uintptr_t>(&__xd_z);
    for (uintptr_t address = first; address < last; address += sizeof(_PVFV)) {
        const _PVFV initializer = *reinterpret_cast<const _PVFV*>(address);
        if (initializer != nullptr) {
            initializer();
        }
    }
}

void NTAPI program_thread_local_exit(void* module, unsigned long reason, void* reserved) {
    static_cast<void>(module);
    static_cast<void>(reason);
    static_cast<void>(reserved);
    run_thread_local_destructors();
}

}  // namespace

void initialize_thread_locals() noexcept {
    if (__tls_guard != 0) {
        return;
    }
    // Set first: an initializer that uses another thread_local of this
    // image tests the guard and must not start the table again.
    __tls_guard = 1;
    run_thread_local_initializers();
}

void run_thread_local_destructors() noexcept {
    // A destructor may register further destructors (a thread_local first
    // used inside another's destructor); the count is read again after
    // each call, and the list may have been reallocated.
    while (thread_destructors.count > 0) {
        thread_destructors.count = thread_destructors.count - 1;
        const _PVFV destructor = thread_destructors.entries[thread_destructors.count];
        destructor();
    }
    free(thread_destructors.entries);
    thread_destructors.entries = nullptr;
    thread_destructors.capacity = 0;
}

void register_program_thread_local_exit() noexcept {
    _register_thread_local_exe_atexit_callback(&program_thread_local_exit);
}

}  // namespace stxlib::msvc_startup

extern "C" {

void NTAPI __dyn_tls_init(PVOID module, DWORD reason, PVOID reserved) {
    static_cast<void>(module);
    static_cast<void>(reserved);
    if (reason == DLL_THREAD_ATTACH) {
        stxlib::msvc_startup::initialize_thread_locals();
    }
}

void NTAPI stx_msvc_startup_tls_detach(PVOID module, DWORD reason, PVOID reserved) {
    static_cast<void>(module);
    static_cast<void>(reserved);
    if (reason == DLL_THREAD_DETACH) {
        stxlib::msvc_startup::run_thread_local_destructors();
    }
}

void __cdecl __dyn_tls_on_demand_init(void) noexcept {
    stxlib::msvc_startup::initialize_thread_locals();
}

// Registers a thread_local's destructor stub for the calling thread.
// Returns 0. Running out of memory here ends the process (abort): the
// destructor could not otherwise be kept, and a thread_local whose
// destructor silently never runs is worse than a stop.
int __cdecl __tlregdtor(_PVFV destructor) noexcept {
    stxlib::msvc_startup::ThreadDestructorList& list = stxlib::msvc_startup::thread_destructors;
    if (list.count == list.capacity) {
        size_t new_capacity = stxlib::msvc_startup::first_destructor_capacity;
        if (list.capacity != 0) {
            if (list.capacity > SIZE_MAX / (2 * sizeof(_PVFV))) {
                abort();
            }
            new_capacity = list.capacity * 2;
        }
        void* const grown = realloc(list.entries, new_capacity * sizeof(_PVFV));
        if (grown == nullptr) {
            abort();
        }
        list.entries = static_cast<_PVFV*>(grown);
        list.capacity = new_capacity;
    }
    list.entries[list.count] = destructor;
    list.count = list.count + 1;
    return 0;
}

}  // extern "C"
