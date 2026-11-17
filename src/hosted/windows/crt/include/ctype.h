/* ctype.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.4), with the
 * Microsoft classification table and locale-explicit forms.
 *
 * __pctype_func() returns the current locale's classification table: one
 * unsigned short per character, indexable from -1 (EOF) to 255. libc++'s
 * ctype<char>::classic_table() returns it, and libc++'s ctype_base masks are
 * these macros (include/__locale, the _LIBCPP_MSVCRT_LIKE branch), so the
 * bit values must be the table's. Observed in the "C" locale: control 0x20,
 * space 0x08 (with blank 0x40 for ' ' and '\t'), punctuation 0x10, digit
 * 0x04 (with hex 0x80), upper 0x01, lower 0x02, hex letters add 0x80.
 * _ALPHA adds 0x0100 to upper and lower: the bit a locale sets for letters
 * that are neither (libc++ include/__locale: "0x8000 and 0x0100 and 0x00ff
 * are used"); the "C" locale sets it for no ASCII character.
 */
#ifndef STXLIB_CRT_CTYPE_H
#define STXLIB_CRT_CTYPE_H

#include "stxlib_crt_common.h"

#define _UPPER 0x0001
#define _LOWER 0x0002
#define _DIGIT 0x0004
#define _SPACE 0x0008
#define _PUNCT 0x0010
#define _CONTROL 0x0020
#define _BLANK 0x0040
#define _HEX 0x0080
#define _LEADBYTE 0x8000
#define _ALPHA (0x0100 | _UPPER | _LOWER)

_STXLIB_CRT_BEGIN

_STXLIB_UCRT const unsigned short* __cdecl __pctype_func(void);
_STXLIB_UCRT const wctype_t* __cdecl __pwctype_func(void);

_STXLIB_UCRT int __cdecl isalnum(int character);
_STXLIB_UCRT int __cdecl isalpha(int character);
_STXLIB_UCRT int __cdecl isblank(int character);
_STXLIB_UCRT int __cdecl iscntrl(int character);
_STXLIB_UCRT int __cdecl isdigit(int character);
_STXLIB_UCRT int __cdecl isgraph(int character);
_STXLIB_UCRT int __cdecl islower(int character);
_STXLIB_UCRT int __cdecl isprint(int character);
_STXLIB_UCRT int __cdecl ispunct(int character);
_STXLIB_UCRT int __cdecl isspace(int character);
_STXLIB_UCRT int __cdecl isupper(int character);
_STXLIB_UCRT int __cdecl isxdigit(int character);
_STXLIB_UCRT int __cdecl tolower(int character);
_STXLIB_UCRT int __cdecl toupper(int character);

_STXLIB_UCRT int __cdecl _isctype(int character, int mask);
_STXLIB_UCRT int __cdecl _isctype_l(int character, int mask, _locale_t locale);
_STXLIB_UCRT int __cdecl _isalnum_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isalpha_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isblank_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _iscntrl_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isdigit_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isgraph_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _islower_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isprint_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _ispunct_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isspace_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isupper_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _isxdigit_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _tolower_l(int character, _locale_t locale);
_STXLIB_UCRT int __cdecl _toupper_l(int character, _locale_t locale);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_CTYPE_H */
