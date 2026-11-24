/* string.h for x86_64-pc-windows-msvc (C17 7.24), with the Microsoft
 * additions our sources and libc++ use.
 *
 * The string group (memchr, memcmp, memcpy, memmove, memset, strchr,
 * strrchr, strstr) is vcruntime's on Microsoft's toolchain; here STXlib's C++
 * runtime supplies it in every image (src/hosted/windows/msvc_runtime/
 * strings.cpp), so those carry no import and are not in ucrtbase.def. The
 * compiler's own calls to memcpy, memmove and memset resolve there too. The
 * rest is ucrtbase.dll's.
 *
 * strchr, strrchr, strstr, strpbrk and memchr have C's signatures; libc++'s
 * string.h adds the const-correct C++ overloads.
 */
#ifndef STXLIB_CRT_STRING_H
#define STXLIB_CRT_STRING_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

/* Module-local (STXlib's runtime). */
void* __cdecl memchr(const void* memory, int value, size_t count);
int __cdecl memcmp(const void* left, const void* right, size_t count);
void* __cdecl memcpy(void* destination, const void* source, size_t count);
void* __cdecl memmove(void* destination, const void* source, size_t count);
void* __cdecl memset(void* destination, int value, size_t count);
char* __cdecl strchr(const char* text, int value);
char* __cdecl strrchr(const char* text, int value);
char* __cdecl strstr(const char* text, const char* pattern);

/* ucrtbase.dll. */
_STXLIB_UCRT char* __cdecl strcpy(char* destination, const char* source);
_STXLIB_UCRT char* __cdecl strncpy(char* destination, const char* source, size_t count);
_STXLIB_UCRT char* __cdecl strcat(char* destination, const char* source);
_STXLIB_UCRT char* __cdecl strncat(char* destination, const char* source, size_t count);
_STXLIB_UCRT int __cdecl strcmp(const char* left, const char* right);
_STXLIB_UCRT int __cdecl strncmp(const char* left, const char* right, size_t count);
_STXLIB_UCRT int __cdecl strcoll(const char* left, const char* right);
_STXLIB_UCRT size_t __cdecl strxfrm(char* destination, const char* source, size_t count);
_STXLIB_UCRT size_t __cdecl strcspn(const char* text, const char* reject);
_STXLIB_UCRT size_t __cdecl strspn(const char* text, const char* accept);
_STXLIB_UCRT char* __cdecl strpbrk(const char* text, const char* accept);
_STXLIB_UCRT char* __cdecl strtok(char* text, const char* delimiters);
_STXLIB_UCRT char* __cdecl strerror(int error_number);
_STXLIB_UCRT size_t __cdecl strlen(const char* text);
_STXLIB_UCRT size_t __cdecl strnlen(const char* text, size_t maximum);

/* Bounds-checked forms (Learn, "Security features in the CRT"). */
_STXLIB_UCRT errno_t __cdecl strerror_s(char* buffer, size_t size, int error_number);
_STXLIB_UCRT errno_t __cdecl strcpy_s(char* destination, rsize_t size, const char* source);
_STXLIB_UCRT errno_t __cdecl strncpy_s(char* destination, rsize_t size, const char* source, rsize_t count);
_STXLIB_UCRT errno_t __cdecl strcat_s(char* destination, rsize_t size, const char* source);
_STXLIB_UCRT char* __cdecl strtok_s(char* text, const char* delimiters, char** context);
_STXLIB_UCRT errno_t __cdecl memcpy_s(void* destination, rsize_t size, const void* source, rsize_t count);
_STXLIB_UCRT errno_t __cdecl memmove_s(void* destination, rsize_t size, const void* source, rsize_t count);

/* Microsoft names (Learn). */
_STXLIB_UCRT char* __cdecl _strdup(const char* text);
_STXLIB_UCRT int __cdecl _stricmp(const char* left, const char* right);
_STXLIB_UCRT int __cdecl _strnicmp(const char* left, const char* right, size_t count);
_STXLIB_UCRT int __cdecl _strcoll_l(const char* left, const char* right, _locale_t locale);
_STXLIB_UCRT size_t __cdecl _strxfrm_l(char* destination, const char* source, size_t count, _locale_t locale);

/* Learn: "_memccpy", "_memicmp, _memicmp_l", "_strlwr ...", "_strupr ...",
 * "_strnset ...", "_strrev ...", "_strset ...", "_stricoll ...",
 * "_strncoll ...", "_strerror", "strncat_s". The wide forms are wchar.h's. */
_STXLIB_UCRT void* __cdecl _memccpy(void* destination, const void* source, int character, size_t count);
_STXLIB_UCRT int __cdecl _memicmp(const void* left, const void* right, size_t count);
_STXLIB_UCRT char* __cdecl _strlwr(char* text);
_STXLIB_UCRT char* __cdecl _strupr(char* text);
_STXLIB_UCRT errno_t __cdecl _strlwr_s(char* text, size_t size);
_STXLIB_UCRT errno_t __cdecl _strupr_s(char* text, size_t size);
_STXLIB_UCRT char* __cdecl _strnset(char* text, int character, size_t count);
_STXLIB_UCRT char* __cdecl _strrev(char* text);
_STXLIB_UCRT char* __cdecl _strset(char* text, int character);
_STXLIB_UCRT int __cdecl _stricoll(const char* left, const char* right);
_STXLIB_UCRT int __cdecl _strncoll(const char* left, const char* right, size_t count);
_STXLIB_UCRT int __cdecl _strnicoll(const char* left, const char* right, size_t count);
_STXLIB_UCRT char* __cdecl _strerror(const char* message);
_STXLIB_UCRT errno_t __cdecl strncat_s(char* destination, rsize_t size, const char* source, rsize_t count);

/* The POSIX names (Learn: each function's page, "The POSIX name ... is
 * deprecated"), imported under the Microsoft names (stxlib_crt_common.h). */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX void* __cdecl memccpy(void* destination, const void* source, int character, size_t count);
_STXLIB_UCRT_POSIX int __cdecl memicmp(const void* left, const void* right, size_t count);
_STXLIB_UCRT_POSIX int __cdecl strcmpi(const char* left, const char* right);
_STXLIB_UCRT_POSIX int __cdecl stricmp(const char* left, const char* right);
_STXLIB_UCRT_POSIX int __cdecl strnicmp(const char* left, const char* right, size_t count);
_STXLIB_UCRT_POSIX char* __cdecl strdup(const char* text);
_STXLIB_UCRT_POSIX char* __cdecl strlwr(char* text);
_STXLIB_UCRT_POSIX char* __cdecl strupr(char* text);
_STXLIB_UCRT_POSIX char* __cdecl strnset(char* text, int character, size_t count);
_STXLIB_UCRT_POSIX char* __cdecl strrev(char* text);
_STXLIB_UCRT_POSIX char* __cdecl strset(char* text, int character);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_STRING_H */
