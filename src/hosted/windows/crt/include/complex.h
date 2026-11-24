/* complex.h for x86_64-pc-windows-msvc over ucrtbase.dll: C17 7.3 as
 * Microsoft provides it (Learn, "C complex math support"): the complex types
 * are structures, _Dcomplex, _Fcomplex and _Lcomplex, not the _Complex
 * keyword's types; _Cbuild and the rest build and multiply them. norm is
 * Microsoft's (the squared magnitude). In C++ libc++'s complex.h comes first.
 */
#ifndef STXLIB_CRT_COMPLEX_H
#define STXLIB_CRT_COMPLEX_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

typedef struct _C_double_complex {
    double _Val[2];
} _Dcomplex;
typedef struct _C_float_complex {
    float _Val[2];
} _Fcomplex;
typedef struct _C_ldouble_complex {
    long double _Val[2];
} _Lcomplex;

#define _Complex_I _FCbuild(0.0f, 1.0f)
#define I _Complex_I

_STXLIB_UCRT _Dcomplex __cdecl _Cbuild(double real, double imaginary);
_STXLIB_UCRT _Dcomplex __cdecl _Cmulcc(_Dcomplex x, _Dcomplex y);
_STXLIB_UCRT _Dcomplex __cdecl _Cmulcr(_Dcomplex x, double y);
_STXLIB_UCRT _Fcomplex __cdecl _FCbuild(float real, float imaginary);
_STXLIB_UCRT _Fcomplex __cdecl _FCmulcc(_Fcomplex x, _Fcomplex y);
_STXLIB_UCRT _Fcomplex __cdecl _FCmulcr(_Fcomplex x, float y);
_STXLIB_UCRT _Lcomplex __cdecl _LCbuild(long double real, long double imaginary);
_STXLIB_UCRT _Lcomplex __cdecl _LCmulcc(_Lcomplex x, _Lcomplex y);
_STXLIB_UCRT _Lcomplex __cdecl _LCmulcr(_Lcomplex x, long double y);
_STXLIB_UCRT double __cdecl cabs(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl cacos(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl cacosh(_Dcomplex z);
_STXLIB_UCRT double __cdecl carg(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl casin(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl casinh(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl catan(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl catanh(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl ccos(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl ccosh(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl cexp(_Dcomplex z);
_STXLIB_UCRT double __cdecl cimag(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl clog(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl clog10(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl conj(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl cpow(_Dcomplex x, _Dcomplex y);
_STXLIB_UCRT _Dcomplex __cdecl cproj(_Dcomplex z);
_STXLIB_UCRT double __cdecl creal(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl csin(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl csinh(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl csqrt(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl ctan(_Dcomplex z);
_STXLIB_UCRT _Dcomplex __cdecl ctanh(_Dcomplex z);
_STXLIB_UCRT double __cdecl norm(_Dcomplex z);
_STXLIB_UCRT float __cdecl cabsf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl cacosf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl cacoshf(_Fcomplex z);
_STXLIB_UCRT float __cdecl cargf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl casinf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl casinhf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl catanf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl catanhf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl ccosf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl ccoshf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl cexpf(_Fcomplex z);
_STXLIB_UCRT float __cdecl cimagf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl clogf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl clog10f(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl conjf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl cpowf(_Fcomplex x, _Fcomplex y);
_STXLIB_UCRT _Fcomplex __cdecl cprojf(_Fcomplex z);
_STXLIB_UCRT float __cdecl crealf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl csinf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl csinhf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl csqrtf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl ctanf(_Fcomplex z);
_STXLIB_UCRT _Fcomplex __cdecl ctanhf(_Fcomplex z);
_STXLIB_UCRT float __cdecl normf(_Fcomplex z);
_STXLIB_UCRT long double __cdecl cabsl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl cacosl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl cacoshl(_Lcomplex z);
_STXLIB_UCRT long double __cdecl cargl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl casinl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl casinhl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl catanl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl catanhl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl ccosl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl ccoshl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl cexpl(_Lcomplex z);
_STXLIB_UCRT long double __cdecl cimagl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl clogl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl clog10l(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl conjl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl cpowl(_Lcomplex x, _Lcomplex y);
_STXLIB_UCRT _Lcomplex __cdecl cprojl(_Lcomplex z);
_STXLIB_UCRT long double __cdecl creall(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl csinl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl csinhl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl csqrtl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl ctanl(_Lcomplex z);
_STXLIB_UCRT _Lcomplex __cdecl ctanhl(_Lcomplex z);
_STXLIB_UCRT long double __cdecl norml(_Lcomplex z);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_COMPLEX_H */
