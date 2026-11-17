/* assert.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.2).
 *
 * A failed assertion calls ucrtbase.dll's _wassert with the expression, the
 * file and the line (Learn, "_assert, _wassert"); _wassert reports and
 * aborts. The two narrow literals become wide by concatenation with L"".
 * Like every assert.h, this one has no include guard: each inclusion
 * re-reads NDEBUG.
 */
#include "stxlib_crt_common.h"

#undef assert

_STXLIB_CRT_BEGIN
_STXLIB_UCRT void __cdecl _wassert(const wchar_t* message, const wchar_t* file, unsigned line);
_STXLIB_UCRT void __cdecl _assert(const char* message, const char* file, unsigned line);
_STXLIB_CRT_END

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) \
    ((void)((!!(expression)) || (_wassert(L"" #expression, L"" __FILE__, (unsigned)(__LINE__)), 0)))
#endif

#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
