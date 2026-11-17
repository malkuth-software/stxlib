/* uchar.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.28).
 * char16_t holds UTF-16 code units and char32_t UTF-32 code points; the
 * conversions use the current locale's multibyte encoding (Learn, "c16rtomb,
 * c32rtomb", "mbrtoc16, mbrtoc32").
 */
#ifndef STXLIB_CRT_UCHAR_H
#define STXLIB_CRT_UCHAR_H

#include "stxlib_crt_common.h"
#include "wchar.h"

_STXLIB_CRT_BEGIN

#ifndef __cplusplus
typedef unsigned short char16_t;
typedef unsigned int char32_t;
#endif

_STXLIB_UCRT size_t __cdecl c16rtomb(char* text, char16_t unit, mbstate_t* state);
_STXLIB_UCRT size_t __cdecl c32rtomb(char* text, char32_t point, mbstate_t* state);
_STXLIB_UCRT size_t __cdecl mbrtoc16(char16_t* unit, const char* text, size_t count, mbstate_t* state);
_STXLIB_UCRT size_t __cdecl mbrtoc32(char32_t* point, const char* text, size_t count, mbstate_t* state);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_UCHAR_H */
