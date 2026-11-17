/* fenv.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.6).
 *
 * Values (probe unless noted): after 1/0 fetestexcept reports 0x8, after 0/0
 * 0x10, after an overflow 0x5 (with inexact), after an underflow 0x3, after
 * 1/3 0x1. Rounding: Learn, "fegetround, fesetround", gives FE_DOWNWARD
 * 0x0100 and FE_UPWARD 0x0200 (since Windows 10 version 14393); the probe
 * confirmed both and found FE_TONEAREST 0 (the start-up mode) and
 * FE_TOWARDZERO 0x0300 (1/3 and -1/3 both rounded toward zero); 0x0400 is
 * refused. fenv_t is 8 bytes (probe: fegetenv writes 8 bytes).
 *
 * feraiseexcept and feupdateenv are not exported by ucrtbase.dll and are not
 * declared; nothing in the build calls them.
 */
#ifndef STXLIB_CRT_FENV_H
#define STXLIB_CRT_FENV_H

#include "stxlib_crt_common.h"

#define FE_INEXACT 0x01
#define FE_UNDERFLOW 0x02
#define FE_OVERFLOW 0x04
#define FE_DIVBYZERO 0x08
#define FE_INVALID 0x10
#define FE_ALL_EXCEPT (FE_DIVBYZERO | FE_INEXACT | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW)

#define FE_TONEAREST 0x0000
#define FE_DOWNWARD 0x0100
#define FE_UPWARD 0x0200
#define FE_TOWARDZERO 0x0300

_STXLIB_CRT_BEGIN

typedef unsigned long fexcept_t;
typedef struct fenv_t {
    unsigned long _Part[2];
} fenv_t;

_STXLIB_UCRT int __cdecl feclearexcept(int exceptions);
_STXLIB_UCRT int __cdecl fegetexceptflag(fexcept_t* flags, int exceptions);
_STXLIB_UCRT int __cdecl fesetexceptflag(const fexcept_t* flags, int exceptions);
_STXLIB_UCRT int __cdecl fetestexcept(int exceptions);
_STXLIB_UCRT int __cdecl fegetround(void);
_STXLIB_UCRT int __cdecl fesetround(int mode);
_STXLIB_UCRT int __cdecl fegetenv(fenv_t* environment);
_STXLIB_UCRT int __cdecl fesetenv(const fenv_t* environment);
_STXLIB_UCRT int __cdecl feholdexcept(fenv_t* environment);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_FENV_H */
