/* What every image linked without Microsoft's startup objects needs from
 * them besides an entry point: the TLS directory and _fltused. Linked into
 * the runtime's own DLL (stxlib_msvc_runtime.dll) and, until the startup
 * shim supplies them, the conformance programs.
 *
 * TLS directory. Without it the loader allocates no static TLS block, and
 * every thread_local the compiler addresses through _tls_index
 * (thread_statics.cpp's _Init_thread_epoch, libc++'s own) would read
 * another module's slot.
 *
 * Layout (Microsoft Learn, "PE Format", "The .tls Section" and "TLS
 * Callback Functions"; the Windows SDK's winnt.h, IMAGE_TLS_DIRECTORY64):
 * the raw data template runs from _tls_start to _tls_end, the compiler's
 * thread_local initialisers lying in .tls$ between them (the linker sorts
 * grouped sections by the text after '$'); AddressOfIndex names the slot
 * the loader writes the module's TLS index to; AddressOfCallBacks names a
 * null-terminated array, empty here. lld-link points the image's TLS data
 * directory at the symbol _tls_used. */

#include "system_interim.h"

unsigned long _tls_index = 0;

__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;

__attribute__((section(".CRT$XLA"))) const PIMAGE_TLS_CALLBACK stxlib_msvc_tls_callbacks_first = 0;
__attribute__((section(".CRT$XLZ"))) const PIMAGE_TLS_CALLBACK stxlib_msvc_tls_callbacks_last = 0;

__attribute__((section(".rdata$T"), used)) const IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG)&_tls_start,
    (ULONGLONG)&_tls_end,
    (ULONGLONG)&_tls_index,
    (ULONGLONG)(&stxlib_msvc_tls_callbacks_first + 1),
    0,
    0,
};

/* _fltused: clang references it from objects that use floating point on
 * this target (observed: libc++.lib built for it leaves _fltused undefined,
 * llvm-nm, 2026-11-16; the emitter was not read), a marker some runtime
 * must define; nothing reads its value. */
int _fltused = 0x9875;
