/* locale.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.11), with
 * the Microsoft locale objects libc++ uses (Learn, "_create_locale",
 * "_free_locale", "_configthreadlocale", "setlocale, _wsetlocale").
 *
 * Categories (probe): setlocale accepts 0 to 5 and refuses 6; setting one
 * category to "de-DE" moved strcoll for 1, the multibyte length for 2
 * (with ".UTF8"), the currency symbol for 3, the decimal point for 4 and
 * the weekday names for 5. These are also the numbers libc++'s masks assume
 * (include/__locale_dir/support/windows.h, _CATMASK).
 *
 * struct lconv: Learn, "localeconv", names the members; the layout was
 * observed in "en-US" (the ten string pointers in Learn's order, then the
 * eight small numbers, then the eight wide string pointers).
 */
#ifndef STXLIB_CRT_LOCALE_H
#define STXLIB_CRT_LOCALE_H

#include "stxlib_crt_common.h"

#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
#define LC_MIN LC_ALL
#define LC_MAX LC_TIME

/* _configthreadlocale's arguments (Learn, "_configthreadlocale"). */
#define _ENABLE_PER_THREAD_LOCALE 0x1
#define _DISABLE_PER_THREAD_LOCALE 0x2

_STXLIB_CRT_BEGIN

struct lconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    wchar_t* _W_decimal_point;
    wchar_t* _W_thousands_sep;
    wchar_t* _W_int_curr_symbol;
    wchar_t* _W_currency_symbol;
    wchar_t* _W_mon_decimal_point;
    wchar_t* _W_mon_thousands_sep;
    wchar_t* _W_positive_sign;
    wchar_t* _W_negative_sign;
};

_STXLIB_UCRT char* __cdecl setlocale(int category, const char* locale);
_STXLIB_UCRT wchar_t* __cdecl _wsetlocale(int category, const wchar_t* locale);
_STXLIB_UCRT struct lconv* __cdecl localeconv(void);
_STXLIB_UCRT _locale_t __cdecl _create_locale(int category, const char* locale);
_STXLIB_UCRT _locale_t __cdecl _wcreate_locale(int category, const wchar_t* locale);
_STXLIB_UCRT void __cdecl _free_locale(_locale_t locale);
_STXLIB_UCRT _locale_t __cdecl _get_current_locale(void);
_STXLIB_UCRT int __cdecl _configthreadlocale(int mode);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_LOCALE_H */
