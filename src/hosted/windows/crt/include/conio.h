/* conio.h for x86_64-pc-windows-msvc over ucrtbase.dll: console input and
 * output (Learn, "_getch, _getwch", "_getche, _getwche", "_kbhit",
 * "_putch, _putwch", "_ungetch, _ungetwch", "_cputs, _cputws") and their
 * POSIX names. The formatted console functions (_cprintf and the rest) are
 * not declared: ucrtbase.dll exports them only through __conio_common_*.
 */
#ifndef STXLIB_CRT_CONIO_H
#define STXLIB_CRT_CONIO_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

_STXLIB_UCRT int __cdecl _getch(void);
_STXLIB_UCRT wint_t __cdecl _getwch(void);
_STXLIB_UCRT int __cdecl _getche(void);
_STXLIB_UCRT wint_t __cdecl _getwche(void);
_STXLIB_UCRT int __cdecl _kbhit(void);
_STXLIB_UCRT int __cdecl _putch(int character);
_STXLIB_UCRT wint_t __cdecl _putwch(wchar_t character);
_STXLIB_UCRT int __cdecl _ungetch(int character);
_STXLIB_UCRT wint_t __cdecl _ungetwch(wint_t character);
_STXLIB_UCRT int __cdecl _cputs(const char* text);
_STXLIB_UCRT int __cdecl _cputws(const wchar_t* text);

/* The POSIX names (Learn: each function's page, "The POSIX name ... is
 * deprecated"), imported under the Microsoft names (stxlib_crt_common.h). */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX int __cdecl getch(void);
_STXLIB_UCRT_POSIX int __cdecl getche(void);
_STXLIB_UCRT_POSIX int __cdecl kbhit(void);
_STXLIB_UCRT_POSIX int __cdecl putch(int character);
_STXLIB_UCRT_POSIX int __cdecl ungetch(int character);
_STXLIB_UCRT_POSIX int __cdecl cputs(const char* text);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_CONIO_H */
