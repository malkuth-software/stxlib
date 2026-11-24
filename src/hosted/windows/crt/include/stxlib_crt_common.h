/* STXlib's C library headers for x86_64-pc-windows-msvc: the shared base.
 *
 * Every header in this folder declares functions exported by the Universal
 * C Runtime DLL, ucrtbase.dll (ucrtbase.def beside this folder is the import
 * list), or small inline forms over those exports where the UCRT exports a
 * different name. Sources: the C standard (C17) for the standard names, and
 * Microsoft Learn's CRT reference for the Microsoft names; values Learn does
 * not state were observed from ucrtbase.dll's behaviour (REFERENCES.md, which
 * also says which probe established each value). Clean room: no Windows SDK,
 * Visual C++ or UCRT header source, Wine, ReactOS or mingw-w64 was read.
 *
 * Target facts this folder relies on:
 * - LLP64: int and long are 32 bits, long long and pointers 64 bits;
 * - wchar_t is 16 bits and holds UTF-16 code units; wint_t is 16 bits;
 * - long double has double's representation (64 bits), so every long double
 *   function is an inline form over the double one, with no rounding change;
 * - x64 has one calling convention; __cdecl is spelled for documentation and
 *   ignored by the compiler;
 * - time_t is 64 bits: every time function maps to the UCRT's 64-bit export.
 *
 * Linkage: functions imported from ucrtbase.dll carry _STXLIB_UCRT
 * (__declspec(dllimport)). Functions every image supplies itself (STXlib's
 * startup shim and C++ runtime: atexit, the string group, setjmp/longjmp, the
 * unexpected hooks) are declared without it, so a definition in the same
 * image never conflicts with an import. Neither _DLL nor _MT changes anything
 * here: the UCRT is always the DLL.
 */
#ifndef STXLIB_CRT_COMMON_H
#define STXLIB_CRT_COMMON_H

#if !defined(_WIN64) || !defined(__x86_64__) || !defined(_MSC_VER) || defined(__MINGW32__)
#error "STXlib's C library headers are for x86_64-pc-windows-msvc only"
#endif

/* clang's own stddef.h: size_t, ptrdiff_t, wchar_t, NULL, max_align_t. */
#include <stddef.h>

#ifdef __cplusplus
#define _STXLIB_CRT_BEGIN extern "C" {
#define _STXLIB_CRT_END }
#define _STXLIB_CRT_NOTHROW noexcept
#else
#define _STXLIB_CRT_BEGIN
#define _STXLIB_CRT_END
#define _STXLIB_CRT_NOTHROW
#endif

/* An export of ucrtbase.dll. */
#define _STXLIB_UCRT __declspec(dllimport)
/* A function that ends the program and never returns. */
#define _STXLIB_NORETURN __declspec(noreturn)
/* The header's own inline form of a function the UCRT exports under another
 * name (or not at all, for long double). Internal linkage in C, so every
 * translation unit keeps its own copy and no symbol is exported. */
#ifdef __cplusplus
#define _STXLIB_CRT_INLINE inline
#else
#define _STXLIB_CRT_INLINE static __inline
#endif

/* Learn, "Standard types" and "errno_t". */
typedef int errno_t;
typedef unsigned short wint_t;
typedef unsigned short wctype_t;
typedef long long __time64_t;
typedef long long time_t;
typedef size_t rsize_t;
typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;

/* The UCRT's locale object (Learn, "_create_locale"): opaque here; only its
 * pointer crosses the boundary. */
typedef struct stxlib_ucrt_locale* _locale_t;

#ifndef _TRUNCATE
/* Learn, "_TRUNCATE": the count argument of the _s functions asking for a
 * truncating copy. */
#define _TRUNCATE ((size_t)-1)
#endif

/* A POSIX name of a Microsoft C runtime function (open for _open): Microsoft
 * documents both and deprecates the POSIX one (Learn, "Compatibility" and
 * each function's page). ucrtbase.dll exports only the Microsoft name, so
 * ucrtbase.def lists the POSIX name as an alias of it (`open == _open`):
 * llvm-dlltool writes weak aliases, and an image imports _open. Declared
 * unless _CRT_DECLARE_NONSTDC_NAMES is defined to 0. */
#define _STXLIB_UCRT_POSIX __declspec(dllimport)
#ifndef _CRT_DECLARE_NONSTDC_NAMES
#define _CRT_DECLARE_NONSTDC_NAMES 1
#endif
/* The 32-bit time type the *32 functions take (Learn, "time, _time32, _time64"). */
typedef long __time32_t;

#endif /* STXLIB_CRT_COMMON_H */
