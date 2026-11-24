/* float.h for x86_64-pc-windows-msvc over ucrtbase.dll: the Microsoft
 * floating-point control part (C17 7.7's limits are clang's).
 *
 * Include order (README.md): clang's resource float.h comes first and, on an
 * msvc target, includes the next float.h, this one. So this header adds only
 * the Microsoft names and never includes another float.h.
 *
 * Names, signatures and values: Learn, "_control87, _controlfp,
 * __control87_2" (the mask and value table), "_controlfp_s", "_statusfp,
 * _statusfp2" and "_clearfp". On x64 the precision (_MCW_PC) and infinity
 * (_MCW_IC) controls are not supported (Learn); their names are declared for
 * code that spells them. The _SW_ status bits are the _EM_ bits (Learn,
 * "_statusfp"); not probed.
 */
#ifndef STXLIB_CRT_FLOAT_H
#define STXLIB_CRT_FLOAT_H

#include "stxlib_crt_common.h"

/* Denormal control. */
#define _MCW_DN 0x03000000u
#define _DN_SAVE 0x00000000u
#define _DN_FLUSH 0x01000000u

/* Interrupt exception masks. */
#define _MCW_EM 0x0008001Fu
#define _EM_INVALID 0x00000010u
#define _EM_DENORMAL 0x00080000u
#define _EM_ZERODIVIDE 0x00000008u
#define _EM_OVERFLOW 0x00000004u
#define _EM_UNDERFLOW 0x00000002u
#define _EM_INEXACT 0x00000001u

/* Infinity control (not on x64). */
#define _MCW_IC 0x00040000u
#define _IC_AFFINE 0x00040000u
#define _IC_PROJECTIVE 0x00000000u

/* Rounding control. */
#define _MCW_RC 0x00000300u
#define _RC_CHOP 0x00000300u
#define _RC_UP 0x00000200u
#define _RC_DOWN 0x00000100u
#define _RC_NEAR 0x00000000u

/* Precision control (not on x64). */
#define _MCW_PC 0x00030000u
#define _PC_24 0x00020000u
#define _PC_53 0x00010000u
#define _PC_64 0x00000000u

/* Status word bits (_statusfp, _clearfp). */
#define _SW_INEXACT 0x00000001u
#define _SW_UNDERFLOW 0x00000002u
#define _SW_OVERFLOW 0x00000004u
#define _SW_ZERODIVIDE 0x00000008u
#define _SW_INVALID 0x00000010u
#define _SW_DENORMAL 0x00080000u

_STXLIB_CRT_BEGIN

_STXLIB_UCRT unsigned int __cdecl _controlfp(unsigned int new_value, unsigned int mask);
_STXLIB_UCRT errno_t __cdecl _controlfp_s(unsigned int* current, unsigned int new_value, unsigned int mask);
_STXLIB_UCRT unsigned int __cdecl _statusfp(void);
_STXLIB_UCRT unsigned int __cdecl _clearfp(void);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_FLOAT_H */
