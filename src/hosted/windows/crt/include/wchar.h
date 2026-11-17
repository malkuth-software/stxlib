/* wchar.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.29), with the
 * Microsoft additions our sources and libc++ use.
 *
 * wchar_t is a 16-bit UTF-16 code unit; wint_t is 16 bits and WEOF is
 * 0xFFFF (probe: fgetwc at end of file).
 *
 * Not exported by ucrtbase.dll and therefore inline here: wmemchr, wmemcmp,
 * wmemcpy, wmemmove, wmemset (over the string group), mbsinit, and the wide
 * printf and scanf families (over the __stdio_common_* entries, stdio.h
 * comment 2; for vswprintf the options word is 0, which truncates,
 * terminates and returns a negative value when the result does not fit,
 * exactly as C's vswprintf must: observed). wcschr, wcsrchr and wcsstr are
 * STXlib runtime's string group (string.h), declared without an import.
 */
#ifndef STXLIB_CRT_WCHAR_H
#define STXLIB_CRT_WCHAR_H

#include "stxlib_crt_common.h"
#include "stdio.h"
#include "string.h"

_STXLIB_CRT_BEGIN

/* Conversion state between calls of the restartable functions: 8 bytes
 * (probe: mbrtowc writes the first 8 bytes of a larger state and no more;
 * a partial UTF-8 sequence leaves nonzero bytes there). All bytes zero is
 * the initial state (C17 7.29.6: a zero-valued mbstate_t object is initial). */
typedef struct stxlib_ucrt_mbstate {
    unsigned int _Part[2];
} mbstate_t;

#define WEOF ((wint_t)0xFFFF)

/* Module-local (STXlib's runtime). */
wchar_t* __cdecl wcschr(const wchar_t* text, wchar_t value);
wchar_t* __cdecl wcsrchr(const wchar_t* text, wchar_t value);
wchar_t* __cdecl wcsstr(const wchar_t* text, const wchar_t* pattern);

/* Wide strings (C17 7.29.4). */
_STXLIB_UCRT size_t __cdecl wcslen(const wchar_t* text);
_STXLIB_UCRT size_t __cdecl wcsnlen(const wchar_t* text, size_t maximum);
_STXLIB_UCRT wchar_t* __cdecl wcscpy(wchar_t* destination, const wchar_t* source);
_STXLIB_UCRT wchar_t* __cdecl wcsncpy(wchar_t* destination, const wchar_t* source, size_t count);
_STXLIB_UCRT wchar_t* __cdecl wcscat(wchar_t* destination, const wchar_t* source);
_STXLIB_UCRT wchar_t* __cdecl wcsncat(wchar_t* destination, const wchar_t* source, size_t count);
_STXLIB_UCRT int __cdecl wcscmp(const wchar_t* left, const wchar_t* right);
_STXLIB_UCRT int __cdecl wcsncmp(const wchar_t* left, const wchar_t* right, size_t count);
_STXLIB_UCRT int __cdecl wcscoll(const wchar_t* left, const wchar_t* right);
_STXLIB_UCRT size_t __cdecl wcsxfrm(wchar_t* destination, const wchar_t* source, size_t count);
_STXLIB_UCRT size_t __cdecl wcsspn(const wchar_t* text, const wchar_t* accept);
_STXLIB_UCRT size_t __cdecl wcscspn(const wchar_t* text, const wchar_t* reject);
_STXLIB_UCRT wchar_t* __cdecl wcspbrk(const wchar_t* text, const wchar_t* accept);
_STXLIB_UCRT wchar_t* __cdecl wcstok(wchar_t* text, const wchar_t* delimiters, wchar_t** context);
_STXLIB_UCRT wchar_t* __cdecl wcstok_s(wchar_t* text, const wchar_t* delimiters, wchar_t** context);
_STXLIB_UCRT errno_t __cdecl wcscpy_s(wchar_t* destination, rsize_t size, const wchar_t* source);
_STXLIB_UCRT errno_t __cdecl wcsncpy_s(wchar_t* destination, rsize_t size, const wchar_t* source, rsize_t count);
_STXLIB_UCRT errno_t __cdecl wcscat_s(wchar_t* destination, rsize_t size, const wchar_t* source);
_STXLIB_UCRT wchar_t* __cdecl _wcsdup(const wchar_t* text);
_STXLIB_UCRT int __cdecl _wcsicmp(const wchar_t* left, const wchar_t* right);
_STXLIB_UCRT int __cdecl _wcsnicmp(const wchar_t* left, const wchar_t* right, size_t count);
_STXLIB_UCRT int __cdecl _wcscoll_l(const wchar_t* left, const wchar_t* right, _locale_t locale);
_STXLIB_UCRT size_t __cdecl _wcsxfrm_l(wchar_t* destination, const wchar_t* source, size_t count, _locale_t locale);

/* Wide numeric conversions (C17 7.29.4.1). */
_STXLIB_UCRT double __cdecl wcstod(const wchar_t* text, wchar_t** end);
_STXLIB_UCRT float __cdecl wcstof(const wchar_t* text, wchar_t** end);
_STXLIB_UCRT long double __cdecl wcstold(const wchar_t* text, wchar_t** end);
_STXLIB_UCRT long __cdecl wcstol(const wchar_t* text, wchar_t** end, int base);
_STXLIB_UCRT long long __cdecl wcstoll(const wchar_t* text, wchar_t** end, int base);
_STXLIB_UCRT unsigned long __cdecl wcstoul(const wchar_t* text, wchar_t** end, int base);
_STXLIB_UCRT unsigned long long __cdecl wcstoull(const wchar_t* text, wchar_t** end, int base);

/* Multibyte conversion (C17 7.29.6). */
_STXLIB_UCRT wint_t __cdecl btowc(int character);
_STXLIB_UCRT int __cdecl wctob(wint_t wide);
_STXLIB_UCRT size_t __cdecl mbrlen(const char* text, size_t count, mbstate_t* state);
_STXLIB_UCRT size_t __cdecl mbrtowc(wchar_t* wide, const char* text, size_t count, mbstate_t* state);
_STXLIB_UCRT size_t __cdecl wcrtomb(char* text, wchar_t wide, mbstate_t* state);
_STXLIB_UCRT errno_t __cdecl wcrtomb_s(size_t* written, char* text, size_t size, wchar_t wide, mbstate_t* state);
_STXLIB_UCRT size_t __cdecl mbsrtowcs(wchar_t* wide, const char** text, size_t count, mbstate_t* state);
_STXLIB_UCRT size_t __cdecl wcsrtombs(char* text, const wchar_t** wide, size_t count, mbstate_t* state);
_STXLIB_CRT_INLINE int __cdecl mbsinit(const mbstate_t* state) {
    if (state == 0) return 1;
    const int initial = (*state)._Part[0] == 0 && (*state)._Part[1] == 0;
    return initial;
}

/* Wide character I/O (C17 7.29.3). */
_STXLIB_UCRT wint_t __cdecl fgetwc(FILE* stream);
_STXLIB_UCRT wint_t __cdecl getwc(FILE* stream);
_STXLIB_UCRT wint_t __cdecl getwchar(void);
_STXLIB_UCRT wint_t __cdecl fputwc(wchar_t wide, FILE* stream);
_STXLIB_UCRT wint_t __cdecl putwc(wchar_t wide, FILE* stream);
_STXLIB_UCRT wint_t __cdecl putwchar(wchar_t wide);
_STXLIB_UCRT wint_t __cdecl ungetwc(wint_t wide, FILE* stream);
_STXLIB_UCRT wchar_t* __cdecl fgetws(wchar_t* buffer, int count, FILE* stream);
_STXLIB_UCRT int __cdecl fputws(const wchar_t* text, FILE* stream);

/* Time (C17 7.29.5). struct tm is time.h's. */
struct tm;
_STXLIB_UCRT size_t __cdecl wcsftime(wchar_t* buffer, size_t size, const wchar_t* format, const struct tm* time);

/* Low-level file I/O on a wide path (Learn, "_open, _wopen": also wchar.h). */
_STXLIB_UCRT int __cdecl _wopen(const wchar_t* path, int flags, ...);

/* Wide memory (C17 7.29.4.2, 7.29.4.6). The byte counts cannot overflow for
 * any count that describes real storage (count <= SIZE_MAX / 2). */
_STXLIB_CRT_INLINE wchar_t* __cdecl wmemcpy(wchar_t* destination, const wchar_t* source, size_t count) {
    memcpy(destination, source, count * sizeof(wchar_t));
    return destination;
}
_STXLIB_CRT_INLINE wchar_t* __cdecl wmemmove(wchar_t* destination, const wchar_t* source, size_t count) {
    memmove(destination, source, count * sizeof(wchar_t));
    return destination;
}
_STXLIB_CRT_INLINE wchar_t* __cdecl wmemset(wchar_t* destination, wchar_t value, size_t count) {
    for (size_t index = 0; index < count; ++index) destination[index] = value;
    return destination;
}
_STXLIB_CRT_INLINE int __cdecl wmemcmp(const wchar_t* left, const wchar_t* right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (left[index] != right[index]) return left[index] < right[index] ? -1 : 1;
    }
    return 0;
}
_STXLIB_CRT_INLINE wchar_t* __cdecl wmemchr(const wchar_t* memory, wchar_t value, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (memory[index] == value) return (wchar_t*)(memory + index);
    }
    return 0;
}

/* The wide formatted family. */
_STXLIB_CRT_INLINE int __cdecl vfwprintf(FILE* stream, const wchar_t* format, __builtin_va_list arguments) {
    const int result =
        __stdio_common_vfwprintf(_CRT_INTERNAL_LOCAL_PRINTF_OPTIONS, stream, format, (_locale_t)0, arguments);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl vwprintf(const wchar_t* format, __builtin_va_list arguments) {
    const int result = vfwprintf(stdout, format, arguments);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl fwprintf(FILE* stream, const wchar_t* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vfwprintf(stream, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl wprintf(const wchar_t* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vfwprintf(stdout, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl vswprintf(wchar_t* buffer, size_t count, const wchar_t* format,
                                         __builtin_va_list arguments) {
    const int result = __stdio_common_vswprintf(_CRT_INTERNAL_LOCAL_PRINTF_OPTIONS, buffer, count, format,
                                                (_locale_t)0, arguments);
    if (result < 0) return -1;
    return result;
}
/* Microsoft's _snwprintf and _vsnwprintf (Learn, "_snprintf, _snprintf_l,
 * _snwprintf, _snwprintf_l"): a result of exactly count characters is
 * written without a terminator and returns count; a longer one returns -1.
 * That is option bit 0 (probe: "%d" of 1234 into 4 returned 4 and left the
 * fifth element alone, 12345 returned -1, 123 returned 3 terminated).
 * libc++'s to_wstring uses _snwprintf. */
_STXLIB_CRT_INLINE int __cdecl _vsnwprintf(wchar_t* buffer, size_t count, const wchar_t* format,
                                           __builtin_va_list arguments) {
    const int result = __stdio_common_vswprintf(_CRT_INTERNAL_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION, buffer,
                                                count, format, (_locale_t)0, arguments);
    if (result < 0) return -1;
    return result;
}
_STXLIB_CRT_INLINE int __cdecl _snwprintf(wchar_t* buffer, size_t count, const wchar_t* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = _vsnwprintf(buffer, count, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl swprintf(wchar_t* buffer, size_t count, const wchar_t* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vswprintf(buffer, count, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl vswscanf(const wchar_t* buffer, const wchar_t* format, __builtin_va_list arguments) {
    const int result = __stdio_common_vswscanf(_CRT_INTERNAL_LOCAL_SCANF_OPTIONS, buffer, (size_t)-1, format,
                                               (_locale_t)0, arguments);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl swscanf(const wchar_t* buffer, const wchar_t* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vswscanf(buffer, format, arguments);
    __builtin_va_end(arguments);
    return result;
}

_STXLIB_CRT_END

#endif /* STXLIB_CRT_WCHAR_H */
