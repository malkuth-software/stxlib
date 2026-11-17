/* wctype.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.30), with the
 * locale-explicit forms libc++ calls (include/__locale_dir/support/
 * windows.h). wctype_t is a classification mask of ctype.h's bits; wctrans_t
 * is a 16-bit transformation selector.
 */
#ifndef STXLIB_CRT_WCTYPE_H
#define STXLIB_CRT_WCTYPE_H

#include "stxlib_crt_common.h"
#include "ctype.h"

#ifndef WEOF
#define WEOF ((wint_t)0xFFFF)
#endif

_STXLIB_CRT_BEGIN

typedef unsigned short wctrans_t;

_STXLIB_UCRT int __cdecl iswalnum(wint_t wide);
_STXLIB_UCRT int __cdecl iswalpha(wint_t wide);
_STXLIB_UCRT int __cdecl iswblank(wint_t wide);
_STXLIB_UCRT int __cdecl iswcntrl(wint_t wide);
_STXLIB_UCRT int __cdecl iswdigit(wint_t wide);
_STXLIB_UCRT int __cdecl iswgraph(wint_t wide);
_STXLIB_UCRT int __cdecl iswlower(wint_t wide);
_STXLIB_UCRT int __cdecl iswprint(wint_t wide);
_STXLIB_UCRT int __cdecl iswpunct(wint_t wide);
_STXLIB_UCRT int __cdecl iswspace(wint_t wide);
_STXLIB_UCRT int __cdecl iswupper(wint_t wide);
_STXLIB_UCRT int __cdecl iswxdigit(wint_t wide);
_STXLIB_UCRT int __cdecl iswctype(wint_t wide, wctype_t type);
_STXLIB_UCRT wctype_t __cdecl wctype(const char* name);
_STXLIB_UCRT wint_t __cdecl towlower(wint_t wide);
_STXLIB_UCRT wint_t __cdecl towupper(wint_t wide);
_STXLIB_UCRT wint_t __cdecl towctrans(wint_t wide, wctrans_t transformation);
_STXLIB_UCRT wctrans_t __cdecl wctrans(const char* name);

_STXLIB_UCRT int __cdecl _iswalpha_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswblank_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswcntrl_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswdigit_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswlower_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswprint_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswpunct_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswspace_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswupper_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswxdigit_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT int __cdecl _iswctype_l(wint_t wide, wctype_t type, _locale_t locale);
_STXLIB_UCRT wint_t __cdecl _towlower_l(wint_t wide, _locale_t locale);
_STXLIB_UCRT wint_t __cdecl _towupper_l(wint_t wide, _locale_t locale);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_WCTYPE_H */
