/* The system declarations STXlib's MSVC C++ runtime and startup shim use,
 * private to those two directories. Both build with -nostdinc against
 * STXlib's own headers only: the C library's (src/hosted/windows/crt/
 * include) and the Windows API's (src/hosted/windows/api/include), after
 * clang's resource headers; nothing from the Windows SDK, the Visual C++
 * tools or MinGW.
 *
 * The UCRT startup functions and types come from the C library's
 * corecrt_startup.h, stdlib.h, malloc.h, process.h and eh.h; the Win32
 * structures, constants and kernel32 functions from the API headers'
 * windows.h. What follows the includes is the little neither declares.
 * The earlier interim form of this file declared all of it itself, from
 * Microsoft Learn (REFERENCES.md, "system_interim.h"). */

#ifndef STXLIB_HOSTED_WINDOWS_MSVC_RUNTIME_SYSTEM_INTERIM_H
#define STXLIB_HOSTED_WINDOWS_MSVC_RUNTIME_SYSTEM_INTERIM_H

#if !defined(_M_X64) && !defined(__x86_64__)
#error "the MSVC runtime and startup are written for x86-64 Windows only"
#endif

#include <corecrt_startup.h>
#include <malloc.h>
#include <process.h>
#include <stdlib.h>
#ifdef __cplusplus
#include <eh.h>
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The program's own entry functions, which the startup calls (Learn,
 * "WinMain function"; the API headers leave a program's entry points to
 * the program). */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd);
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd);

/* Learn, "__fastfail": an intrinsic clang declares in its intrin.h,
 * declared here so no intrinsics header is needed. */
__attribute__((noreturn)) void __fastfail(unsigned int code);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STXLIB_HOSTED_WINDOWS_MSVC_RUNTIME_SYSTEM_INTERIM_H */
