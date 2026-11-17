/* stdio.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.21), with the
 * Microsoft additions our sources and libc++ use.
 *
 * Three things the UCRT does not export under their standard names, and how
 * this header supplies them (REFERENCES.md has each observation):
 *
 * 1. stdin, stdout, stderr. ucrtbase.dll exports __acrt_iob_func(index),
 *    which returns the stream for index 0, 1 or 2 (observed: _fileno of the
 *    three results is 0, 1 and 2).
 *
 * 2. The printf and scanf families. ucrtbase.dll exports only the "common"
 *    entry points __stdio_common_v{f,s}printf, __stdio_common_v{f,s}wprintf,
 *    __stdio_common_v{f,s}scanf and __stdio_common_v{f,s}wscanf; every named
 *    function is an inline form here. Each common entry takes an options
 *    word first, then its stream or buffer, the format, a locale (null for
 *    the thread's locale) and the argument list. Option bits, as observed
 *    from ucrtbase.dll (probe: a 4-element buffer and a 6-character result):
 *      0  (no bit)  the result is written and null-terminated as far as it
 *                   fits; a truncated result returns a negative value;
 *      bit 0 (1)    a truncated result is not null-terminated;
 *      bit 1 (2)    C's snprintf: the result is truncated and terminated and
 *                   the full length is returned.
 *    Bits 2 to 7 changed nothing in that probe. No Microsoft-legacy option is
 *    used here: _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS and
 *    _CRT_INTERNAL_LOCAL_SCANF_OPTIONS are 0, so %s in the wide functions
 *    takes a narrow string as C requires (observed), and scanf's %s takes no
 *    size argument. libc++ (src/support/win32/locale_win32.cpp) calls
 *    __stdio_common_vsprintf itself with these names.
 *
 * 3. POSIX names (fileno, fdopen): inline forms over the exported _fileno and
 *    _fdopen.
 *
 * Streams are opaque: no member of FILE is used; only FILE* crosses.
 */
#ifndef STXLIB_CRT_STDIO_H
#define STXLIB_CRT_STDIO_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

/* The tag is FILE, as in STXlib's Windows API headers (stxwin_types.h),
 * which may complete it with a placeholder member; nothing here reads it. */
typedef struct FILE FILE;
/* A file position: 8 bytes (probe: fgetpos writes 8 bytes, the offset). */
typedef long long fpos_t;

/* Probe: setbuf's buffer is used through its 512th byte and no further. */
#define BUFSIZ 512
#define EOF (-1)
/* Learn, "Path field limits": _MAX_PATH. */
#define FILENAME_MAX 260
#define FOPEN_MAX 20
/* Probe: setvbuf accepts exactly 0, 4 and 0x40; 4 writes through at once,
 * 0 holds the data until the buffer fills; 0x40 behaves as 0 (Learn,
 * "setvbuf": _IOLBF is full buffering on Windows). */
#define _IOFBF 0x0000
#define _IOLBF 0x0040
#define _IONBF 0x0004
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

_STXLIB_UCRT FILE* __cdecl __acrt_iob_func(unsigned index);
#define stdin (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))

#define _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS (0ULL)
#define _CRT_INTERNAL_LOCAL_SCANF_OPTIONS (0ULL)
#define _CRT_INTERNAL_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION (1ULL)
#define _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR (2ULL)

_STXLIB_UCRT int __cdecl __stdio_common_vfprintf(unsigned long long options, FILE* stream, const char* format,
                                                 _locale_t locale, __builtin_va_list arguments);
_STXLIB_UCRT int __cdecl __stdio_common_vsprintf(unsigned long long options, char* buffer, size_t count,
                                                 const char* format, _locale_t locale, __builtin_va_list arguments);
_STXLIB_UCRT int __cdecl __stdio_common_vfscanf(unsigned long long options, FILE* stream, const char* format,
                                                _locale_t locale, __builtin_va_list arguments);
_STXLIB_UCRT int __cdecl __stdio_common_vsscanf(unsigned long long options, const char* buffer, size_t count,
                                                const char* format, _locale_t locale, __builtin_va_list arguments);
_STXLIB_UCRT int __cdecl __stdio_common_vfwprintf(unsigned long long options, FILE* stream, const wchar_t* format,
                                                  _locale_t locale, __builtin_va_list arguments);
_STXLIB_UCRT int __cdecl __stdio_common_vswprintf(unsigned long long options, wchar_t* buffer, size_t count,
                                                  const wchar_t* format, _locale_t locale,
                                                  __builtin_va_list arguments);
_STXLIB_UCRT int __cdecl __stdio_common_vswscanf(unsigned long long options, const wchar_t* buffer, size_t count,
                                                 const wchar_t* format, _locale_t locale,
                                                 __builtin_va_list arguments);

/* File access (C17 7.21.5). */
_STXLIB_UCRT FILE* __cdecl fopen(const char* path, const char* mode);
_STXLIB_UCRT errno_t __cdecl fopen_s(FILE** stream, const char* path, const char* mode);
_STXLIB_UCRT FILE* __cdecl freopen(const char* path, const char* mode, FILE* stream);
_STXLIB_UCRT int __cdecl fclose(FILE* stream);
_STXLIB_UCRT int __cdecl fflush(FILE* stream);
_STXLIB_UCRT void __cdecl setbuf(FILE* stream, char* buffer);
_STXLIB_UCRT int __cdecl setvbuf(FILE* stream, char* buffer, int mode, size_t size);
_STXLIB_UCRT FILE* __cdecl _wfopen(const wchar_t* path, const wchar_t* mode);
_STXLIB_UCRT errno_t __cdecl _wfopen_s(FILE** stream, const wchar_t* path, const wchar_t* mode);
_STXLIB_UCRT FILE* __cdecl _fdopen(int descriptor, const char* mode);
_STXLIB_UCRT int __cdecl _fileno(FILE* stream);
_STXLIB_UCRT FILE* __cdecl tmpfile(void);

/* Character and direct I/O (C17 7.21.7, 7.21.8). */
_STXLIB_UCRT int __cdecl fgetc(FILE* stream);
_STXLIB_UCRT int __cdecl getc(FILE* stream);
_STXLIB_UCRT int __cdecl getchar(void);
_STXLIB_UCRT char* __cdecl fgets(char* buffer, int count, FILE* stream);
_STXLIB_UCRT int __cdecl fputc(int character, FILE* stream);
_STXLIB_UCRT int __cdecl putc(int character, FILE* stream);
_STXLIB_UCRT int __cdecl putchar(int character);
_STXLIB_UCRT int __cdecl fputs(const char* text, FILE* stream);
_STXLIB_UCRT int __cdecl puts(const char* text);
_STXLIB_UCRT int __cdecl ungetc(int character, FILE* stream);
_STXLIB_UCRT size_t __cdecl fread(void* buffer, size_t size, size_t count, FILE* stream);
_STXLIB_UCRT size_t __cdecl fwrite(const void* buffer, size_t size, size_t count, FILE* stream);

/* Positioning and errors (C17 7.21.9, 7.21.10). */
_STXLIB_UCRT int __cdecl fseek(FILE* stream, long offset, int origin);
_STXLIB_UCRT long __cdecl ftell(FILE* stream);
_STXLIB_UCRT int __cdecl _fseeki64(FILE* stream, long long offset, int origin);
_STXLIB_UCRT long long __cdecl _ftelli64(FILE* stream);
_STXLIB_UCRT void __cdecl rewind(FILE* stream);
_STXLIB_UCRT int __cdecl fgetpos(FILE* stream, fpos_t* position);
_STXLIB_UCRT int __cdecl fsetpos(FILE* stream, const fpos_t* position);
_STXLIB_UCRT void __cdecl clearerr(FILE* stream);
_STXLIB_UCRT int __cdecl feof(FILE* stream);
_STXLIB_UCRT int __cdecl ferror(FILE* stream);
_STXLIB_UCRT void __cdecl perror(const char* message);

/* Operations on files (C17 7.21.4). */
_STXLIB_UCRT int __cdecl remove(const char* path);
_STXLIB_UCRT int __cdecl rename(const char* old_path, const char* new_path);
_STXLIB_UCRT int __cdecl _wremove(const wchar_t* path);
_STXLIB_UCRT int __cdecl _wrename(const wchar_t* old_path, const wchar_t* new_path);

/* POSIX names over the Microsoft exports. */
_STXLIB_CRT_INLINE int __cdecl fileno(FILE* stream) {
    const int descriptor = _fileno(stream);
    return descriptor;
}
_STXLIB_CRT_INLINE FILE* __cdecl fdopen(int descriptor, const char* mode) {
    FILE* const stream = _fdopen(descriptor, mode);
    return stream;
}

/* The formatted family over the common entry points (comment 2 above). */
__attribute__((__format__(__printf__, 2, 0))) _STXLIB_CRT_INLINE int __cdecl vfprintf(FILE* stream, const char* format,
                                                                                      __builtin_va_list arguments) {
    const int result =
        __stdio_common_vfprintf(_CRT_INTERNAL_LOCAL_PRINTF_OPTIONS, stream, format, (_locale_t)0, arguments);
    return result;
}
__attribute__((__format__(__printf__, 1, 0))) _STXLIB_CRT_INLINE int __cdecl vprintf(const char* format,
                                                                                     __builtin_va_list arguments) {
    const int result = vfprintf(stdout, format, arguments);
    return result;
}
__attribute__((__format__(__printf__, 2, 3))) _STXLIB_CRT_INLINE int __cdecl fprintf(FILE* stream, const char* format,
                                                                                     ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vfprintf(stream, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
__attribute__((__format__(__printf__, 1, 2))) _STXLIB_CRT_INLINE int __cdecl printf(const char* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vfprintf(stdout, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
/* C's vsnprintf: truncates, terminates when count > 0, returns the full
 * length, or a negative value on an encoding error. */
__attribute__((__format__(__printf__, 3, 0))) _STXLIB_CRT_INLINE int __cdecl vsnprintf(char* buffer, size_t count,
                                                                                       const char* format,
                                                                                       __builtin_va_list arguments) {
    const int result =
        __stdio_common_vsprintf(_CRT_INTERNAL_LOCAL_PRINTF_OPTIONS | _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR,
                                buffer, count, format, (_locale_t)0, arguments);
    if (result < 0) return -1;
    return result;
}
__attribute__((__format__(__printf__, 3, 4))) _STXLIB_CRT_INLINE int __cdecl snprintf(char* buffer, size_t count,
                                                                                      const char* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vsnprintf(buffer, count, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
/* C's vsprintf has no bound: the largest count stands for "unbounded". */
__attribute__((__format__(__printf__, 2, 0))) _STXLIB_CRT_INLINE int __cdecl vsprintf(char* buffer, const char* format,
                                                                                      __builtin_va_list arguments) {
    const int result = vsnprintf(buffer, (size_t)-1, format, arguments);
    return result;
}
__attribute__((__format__(__printf__, 2, 3))) _STXLIB_CRT_INLINE int __cdecl sprintf(char* buffer, const char* format,
                                                                                     ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vsnprintf(buffer, (size_t)-1, format, arguments);
    __builtin_va_end(arguments);
    return result;
}

__attribute__((__format__(__scanf__, 2, 0))) _STXLIB_CRT_INLINE int __cdecl vfscanf(FILE* stream, const char* format,
                                                                                    __builtin_va_list arguments) {
    const int result =
        __stdio_common_vfscanf(_CRT_INTERNAL_LOCAL_SCANF_OPTIONS, stream, format, (_locale_t)0, arguments);
    return result;
}
__attribute__((__format__(__scanf__, 1, 0))) _STXLIB_CRT_INLINE int __cdecl vscanf(const char* format,
                                                                                   __builtin_va_list arguments) {
    const int result = vfscanf(stdin, format, arguments);
    return result;
}
__attribute__((__format__(__scanf__, 2, 3))) _STXLIB_CRT_INLINE int __cdecl fscanf(FILE* stream, const char* format,
                                                                                   ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vfscanf(stream, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
__attribute__((__format__(__scanf__, 1, 2))) _STXLIB_CRT_INLINE int __cdecl scanf(const char* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vfscanf(stdin, format, arguments);
    __builtin_va_end(arguments);
    return result;
}
/* The buffer is a null-terminated string: the largest count stands for "read
 * to the terminator". */
__attribute__((__format__(__scanf__, 2, 0))) _STXLIB_CRT_INLINE int __cdecl vsscanf(const char* buffer,
                                                                                    const char* format,
                                                                                    __builtin_va_list arguments) {
    const int result = __stdio_common_vsscanf(_CRT_INTERNAL_LOCAL_SCANF_OPTIONS, buffer, (size_t)-1, format,
                                              (_locale_t)0, arguments);
    return result;
}
__attribute__((__format__(__scanf__, 2, 3))) _STXLIB_CRT_INLINE int __cdecl sscanf(const char* buffer,
                                                                                   const char* format, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format);
    const int result = vsscanf(buffer, format, arguments);
    __builtin_va_end(arguments);
    return result;
}

_STXLIB_CRT_END

#endif /* STXLIB_CRT_STDIO_H */
