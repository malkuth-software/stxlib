/* stdlib.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.22), with
 * the Microsoft additions our sources and libc++ use.
 *
 * Module-local, not imported: atexit, at_quick_exit and _onexit belong to
 * each image's exit table (STXlib's startup shim, exit_table.cpp, which
 * forwards to _crt_atexit or _register_onexit_function); ucrtbase.dll does
 * not export them.
 */
#ifndef STXLIB_CRT_STDLIB_H
#define STXLIB_CRT_STDLIB_H

#include "stxlib_crt_common.h"
/* errno: Learn ("errno, _doserrno, _sys_errlist, and _sys_nerr") names
 * stdlib.h as well as errno.h; libc++'s support.cpp relies on that. */
#include "errno.h"

_STXLIB_CRT_BEGIN

/* C17 7.22.6.2: quotient first, then remainder (probe: div(7,2) = {3,1}). */
typedef struct _div_t {
    int quot;
    int rem;
} div_t;
typedef struct _ldiv_t {
    long quot;
    long rem;
} ldiv_t;
typedef struct _lldiv_t {
    long long quot;
    long long rem;
} lldiv_t;

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
/* Learn, "rand"; probe: the largest of 200000 results is 32767. */
#define RAND_MAX 0x7fff

/* The current locale's longest multibyte character (Learn, "MB_CUR_MAX"). */
_STXLIB_UCRT int __cdecl ___mb_cur_max_func(void);
_STXLIB_UCRT int __cdecl ___mb_cur_max_l_func(_locale_t locale);
#define MB_CUR_MAX ___mb_cur_max_func()

/* Numeric conversions (C17 7.22.1). */
_STXLIB_UCRT double __cdecl atof(const char* text);
_STXLIB_UCRT int __cdecl atoi(const char* text);
_STXLIB_UCRT long __cdecl atol(const char* text);
_STXLIB_UCRT long long __cdecl atoll(const char* text);
_STXLIB_UCRT double __cdecl strtod(const char* text, char** end);
_STXLIB_UCRT float __cdecl strtof(const char* text, char** end);
_STXLIB_UCRT long double __cdecl strtold(const char* text, char** end);
_STXLIB_UCRT long __cdecl strtol(const char* text, char** end, int base);
_STXLIB_UCRT long long __cdecl strtoll(const char* text, char** end, int base);
_STXLIB_UCRT unsigned long __cdecl strtoul(const char* text, char** end, int base);
_STXLIB_UCRT unsigned long long __cdecl strtoull(const char* text, char** end, int base);
_STXLIB_UCRT long long __cdecl _strtoi64(const char* text, char** end, int base);
_STXLIB_UCRT unsigned long long __cdecl _strtoui64(const char* text, char** end, int base);
/* Locale-explicit forms (Learn, "strtod, _strtod_l, ..."). */
_STXLIB_UCRT double __cdecl _strtod_l(const char* text, char** end, _locale_t locale);
_STXLIB_UCRT float __cdecl _strtof_l(const char* text, char** end, _locale_t locale);
_STXLIB_UCRT long double __cdecl _strtold_l(const char* text, char** end, _locale_t locale);
_STXLIB_UCRT long __cdecl _strtol_l(const char* text, char** end, int base, _locale_t locale);
_STXLIB_UCRT long long __cdecl _strtoll_l(const char* text, char** end, int base, _locale_t locale);
_STXLIB_UCRT unsigned long __cdecl _strtoul_l(const char* text, char** end, int base, _locale_t locale);
_STXLIB_UCRT unsigned long long __cdecl _strtoull_l(const char* text, char** end, int base, _locale_t locale);

/* Pseudo-random numbers (C17 7.22.2) and Learn's rand_s (declared whether
 * or not _CRT_RAND_S is defined). */
_STXLIB_UCRT int __cdecl rand(void);
_STXLIB_UCRT void __cdecl srand(unsigned seed);
_STXLIB_UCRT errno_t __cdecl rand_s(unsigned* value);

/* Memory (C17 7.22.3). aligned_alloc is not provided by the UCRT (Learn,
 * "C11 aligned_alloc"); _aligned_malloc is in malloc.h. */
_STXLIB_UCRT void* __cdecl malloc(size_t size);
_STXLIB_UCRT void* __cdecl calloc(size_t count, size_t size);
_STXLIB_UCRT void* __cdecl realloc(void* block, size_t size);
_STXLIB_UCRT void __cdecl free(void* block);

/* Environment and termination (C17 7.22.4). */
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl abort(void);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl exit(int status);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl _Exit(int status);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl _exit(int status);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl quick_exit(int status);
int __cdecl atexit(void(__cdecl* function)(void));
int __cdecl at_quick_exit(void(__cdecl* function)(void));
typedef int(__cdecl* _onexit_t)(void);
_onexit_t __cdecl _onexit(_onexit_t function);
_STXLIB_UCRT char* __cdecl getenv(const char* name);
_STXLIB_UCRT wchar_t* __cdecl _wgetenv(const wchar_t* name);
_STXLIB_UCRT errno_t __cdecl _putenv_s(const char* name, const char* value);
_STXLIB_UCRT errno_t __cdecl _wputenv_s(const wchar_t* name, const wchar_t* value);
_STXLIB_UCRT int __cdecl system(const char* command);
_STXLIB_UCRT int __cdecl _wsystem(const wchar_t* command);

/* Learn, "_set_abort_behavior": the flags and the function. */
#define _WRITE_ABORT_MSG 0x1
#define _CALL_REPORTFAULT 0x2
_STXLIB_UCRT unsigned int __cdecl _set_abort_behavior(unsigned int flags, unsigned int mask);

/* Searching and sorting (C17 7.22.5). The comparison receives two elements. */
_STXLIB_UCRT void* __cdecl bsearch(const void* key, const void* base, size_t count, size_t size,
                                   int(__cdecl* compare)(const void*, const void*));
_STXLIB_UCRT void __cdecl qsort(void* base, size_t count, size_t size, int(__cdecl* compare)(const void*, const void*));

/* Integer arithmetic (C17 7.22.6). */
_STXLIB_UCRT int __cdecl abs(int value);
_STXLIB_UCRT long __cdecl labs(long value);
_STXLIB_UCRT long long __cdecl llabs(long long value);
_STXLIB_UCRT long long __cdecl _abs64(long long value);
_STXLIB_UCRT div_t __cdecl div(int numerator, int denominator);
_STXLIB_UCRT ldiv_t __cdecl ldiv(long numerator, long denominator);
_STXLIB_UCRT lldiv_t __cdecl lldiv(long long numerator, long long denominator);

/* Byte order reversal (Microsoft Learn, "_byteswap_uint64, _byteswap_ulong,
 * _byteswap_ushort"). Clang treats these as builtins once declared, so they
 * carry no import and generate a bswap instruction; stx's skia uses
 * _byteswap_ulong. */
unsigned short __cdecl _byteswap_ushort(unsigned short value);
unsigned long __cdecl _byteswap_ulong(unsigned long value);
unsigned long long __cdecl _byteswap_uint64(unsigned long long value);

/* Multibyte and wide characters (C17 7.22.7, 7.22.8). */
_STXLIB_UCRT int __cdecl mblen(const char* text, size_t count);
_STXLIB_UCRT int __cdecl mbtowc(wchar_t* wide, const char* text, size_t count);
_STXLIB_UCRT int __cdecl _mbtowc_l(wchar_t* wide, const char* text, size_t count, _locale_t locale);
_STXLIB_UCRT int __cdecl wctomb(char* text, wchar_t wide);
_STXLIB_UCRT size_t __cdecl mbstowcs(wchar_t* wide, const char* text, size_t count);
_STXLIB_UCRT size_t __cdecl wcstombs(char* text, const wchar_t* wide, size_t count);

/* Wide numeric conversions (C17 7.29.4.1, declared by the UCRT here too). */
_STXLIB_UCRT double __cdecl wcstod(const wchar_t* text, wchar_t** end);
_STXLIB_UCRT float __cdecl wcstof(const wchar_t* text, wchar_t** end);
_STXLIB_UCRT long double __cdecl wcstold(const wchar_t* text, wchar_t** end);
_STXLIB_UCRT long __cdecl wcstol(const wchar_t* text, wchar_t** end, int base);
_STXLIB_UCRT long long __cdecl wcstoll(const wchar_t* text, wchar_t** end, int base);
_STXLIB_UCRT unsigned long __cdecl wcstoul(const wchar_t* text, wchar_t** end, int base);
_STXLIB_UCRT unsigned long long __cdecl wcstoull(const wchar_t* text, wchar_t** end, int base);

/* The deprecated message table (Learn, "errno, _doserrno, _sys_errlist,
 * and _sys_nerr"). ucrtbase.dll exports them as functions returning their
 * addresses (probe: *__sys_nerr() is 43 and __sys_errlist()[2] equals
 * strerror(2)). libc++ reads _sys_nerr. */
_STXLIB_UCRT int* __cdecl __sys_nerr(void);
_STXLIB_UCRT char** __cdecl __sys_errlist(void);
#define _sys_nerr (*__sys_nerr())
#define _sys_errlist (__sys_errlist())

/* Learn: "_itoa, _itow ...", "_itoa_s ...", "_ecvt", "_fcvt", "_gcvt",
 * "_ecvt_s", "_fcvt_s", "_gcvt_s", "_putenv, _wputenv", "_swab",
 * "_fullpath, _wfullpath", "_splitpath, _wsplitpath",
 * "_makepath, _wmakepath", "_searchenv, _wsearchenv", "getenv_s, _wgetenv_s",
 * "_dupenv_s, _wdupenv_s", "bsearch_s", "qsort_s", "mbstowcs_s",
 * "wcstombs_s", "wctomb_s", "_wtoi, _wtol ...", "_wtof", "_sleep" is not
 * declared (Learn: obsolete). */
#define _MAX_PATH 260
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256

_STXLIB_UCRT char* __cdecl _itoa(int value, char* buffer, int radix);
_STXLIB_UCRT char* __cdecl _ltoa(long value, char* buffer, int radix);
_STXLIB_UCRT char* __cdecl _ultoa(unsigned long value, char* buffer, int radix);
_STXLIB_UCRT char* __cdecl _i64toa(long long value, char* buffer, int radix);
_STXLIB_UCRT char* __cdecl _ui64toa(unsigned long long value, char* buffer, int radix);
_STXLIB_UCRT wchar_t* __cdecl _itow(int value, wchar_t* buffer, int radix);
_STXLIB_UCRT wchar_t* __cdecl _ltow(long value, wchar_t* buffer, int radix);
_STXLIB_UCRT wchar_t* __cdecl _ultow(unsigned long value, wchar_t* buffer, int radix);
_STXLIB_UCRT wchar_t* __cdecl _i64tow(long long value, wchar_t* buffer, int radix);
_STXLIB_UCRT wchar_t* __cdecl _ui64tow(unsigned long long value, wchar_t* buffer, int radix);
_STXLIB_UCRT errno_t __cdecl _itoa_s(int value, char* buffer, size_t size, int radix);
_STXLIB_UCRT errno_t __cdecl _ltoa_s(long value, char* buffer, size_t size, int radix);
_STXLIB_UCRT errno_t __cdecl _ultoa_s(unsigned long value, char* buffer, size_t size, int radix);
_STXLIB_UCRT errno_t __cdecl _i64toa_s(long long value, char* buffer, size_t size, int radix);
_STXLIB_UCRT errno_t __cdecl _ui64toa_s(unsigned long long value, char* buffer, size_t size, int radix);
_STXLIB_UCRT char* __cdecl _ecvt(double value, int digits, int* decimal, int* sign);
_STXLIB_UCRT char* __cdecl _fcvt(double value, int digits, int* decimal, int* sign);
_STXLIB_UCRT char* __cdecl _gcvt(double value, int digits, char* buffer);
_STXLIB_UCRT errno_t __cdecl _ecvt_s(char* buffer, size_t size, double value, int digits, int* decimal, int* sign);
_STXLIB_UCRT errno_t __cdecl _fcvt_s(char* buffer, size_t size, double value, int digits, int* decimal, int* sign);
_STXLIB_UCRT errno_t __cdecl _gcvt_s(char* buffer, size_t size, double value, int digits);
_STXLIB_UCRT int __cdecl _putenv(const char* assignment);
_STXLIB_UCRT int __cdecl _wputenv(const wchar_t* assignment);
_STXLIB_UCRT void __cdecl _swab(char* source, char* destination, int count);
_STXLIB_UCRT char* __cdecl _fullpath(char* absolute, const char* relative, size_t size);
_STXLIB_UCRT wchar_t* __cdecl _wfullpath(wchar_t* absolute, const wchar_t* relative, size_t size);
_STXLIB_UCRT void __cdecl _splitpath(const char* path, char* drive, char* directory, char* name, char* extension);
_STXLIB_UCRT void __cdecl _wsplitpath(const wchar_t* path, wchar_t* drive, wchar_t* directory, wchar_t* name,
                                      wchar_t* extension);
_STXLIB_UCRT errno_t __cdecl _splitpath_s(const char* path, char* drive, size_t drive_size, char* directory,
                                          size_t directory_size, char* name, size_t name_size, char* extension,
                                          size_t extension_size);
_STXLIB_UCRT errno_t __cdecl _wsplitpath_s(const wchar_t* path, wchar_t* drive, size_t drive_size,
                                           wchar_t* directory, size_t directory_size, wchar_t* name,
                                           size_t name_size, wchar_t* extension, size_t extension_size);
_STXLIB_UCRT void __cdecl _makepath(char* path, const char* drive, const char* directory, const char* name,
                                    const char* extension);
_STXLIB_UCRT void __cdecl _wmakepath(wchar_t* path, const wchar_t* drive, const wchar_t* directory,
                                     const wchar_t* name, const wchar_t* extension);
_STXLIB_UCRT errno_t __cdecl _makepath_s(char* path, size_t size, const char* drive, const char* directory,
                                         const char* name, const char* extension);
_STXLIB_UCRT errno_t __cdecl _wmakepath_s(wchar_t* path, size_t size, const wchar_t* drive,
                                          const wchar_t* directory, const wchar_t* name, const wchar_t* extension);
_STXLIB_UCRT void __cdecl _searchenv(const char* file, const char* variable, char* found);
_STXLIB_UCRT void __cdecl _wsearchenv(const wchar_t* file, const wchar_t* variable, wchar_t* found);
_STXLIB_UCRT errno_t __cdecl getenv_s(size_t* required, char* buffer, rsize_t size, const char* name);
_STXLIB_UCRT errno_t __cdecl _wgetenv_s(size_t* required, wchar_t* buffer, size_t size, const wchar_t* name);
_STXLIB_UCRT errno_t __cdecl _dupenv_s(char** buffer, size_t* size, const char* name);
_STXLIB_UCRT errno_t __cdecl _wdupenv_s(wchar_t** buffer, size_t* size, const wchar_t* name);
_STXLIB_UCRT void* __cdecl bsearch_s(const void* key, const void* base, rsize_t count, rsize_t size,
                                     int(__cdecl* compare)(void* context, const void* key, const void* element),
                                     void* context);
_STXLIB_UCRT void __cdecl qsort_s(void* base, rsize_t count, rsize_t size,
                                  int(__cdecl* compare)(void* context, const void* left, const void* right),
                                  void* context);
_STXLIB_UCRT errno_t __cdecl mbstowcs_s(size_t* converted, wchar_t* destination, size_t size, const char* source,
                                        size_t count);
_STXLIB_UCRT errno_t __cdecl wcstombs_s(size_t* converted, char* destination, size_t size, const wchar_t* source,
                                        size_t count);
_STXLIB_UCRT errno_t __cdecl wctomb_s(int* written, char* destination, rsize_t size, wchar_t character);
_STXLIB_UCRT int __cdecl _wtoi(const wchar_t* text);
_STXLIB_UCRT long __cdecl _wtol(const wchar_t* text);
_STXLIB_UCRT long long __cdecl _wtoll(const wchar_t* text);
_STXLIB_UCRT long long __cdecl _wtoi64(const wchar_t* text);
_STXLIB_UCRT double __cdecl _wtof(const wchar_t* text);

/* The POSIX names (Learn: each function's page, "The POSIX name ... is
 * deprecated"), imported under the Microsoft names (stxlib_crt_common.h). */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX char* __cdecl ecvt(double value, int digits, int* decimal, int* sign);
_STXLIB_UCRT_POSIX char* __cdecl fcvt(double value, int digits, int* decimal, int* sign);
_STXLIB_UCRT_POSIX char* __cdecl gcvt(double value, int digits, char* buffer);
_STXLIB_UCRT_POSIX char* __cdecl itoa(int value, char* buffer, int radix);
_STXLIB_UCRT_POSIX char* __cdecl ltoa(long value, char* buffer, int radix);
_STXLIB_UCRT_POSIX char* __cdecl ultoa(unsigned long value, char* buffer, int radix);
_STXLIB_UCRT_POSIX int __cdecl putenv(const char* assignment);
_STXLIB_UCRT_POSIX void __cdecl swab(char* source, char* destination, int count);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_STDLIB_H */
