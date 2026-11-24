/* sys/utime.h for x86_64-pc-windows-msvc over ucrtbase.dll: the file time
 * setters (Learn, "_utime, _utime32, _utime64, _wutime, _wutime32,
 * _wutime64" and "_futime, _futime32, _futime64"); with a 64-bit time_t,
 * _utime is _utime64 and struct _utimbuf is __utimbuf64's layout.
 */
#ifndef STXLIB_CRT_SYS_UTIME_H
#define STXLIB_CRT_SYS_UTIME_H

#include "../stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

struct __utimbuf32 {
    __time32_t actime;
    __time32_t modtime;
};
struct __utimbuf64 {
    __time64_t actime;
    __time64_t modtime;
};
struct _utimbuf {
    time_t actime;
    time_t modtime;
};
struct utimbuf {
    time_t actime;
    time_t modtime;
};
#define _utime _utime64
#define _wutime _wutime64
#define _futime _futime64

_STXLIB_UCRT int __cdecl _utime32(const char* path, struct __utimbuf32* times);
_STXLIB_UCRT int __cdecl _utime64(const char* path, struct __utimbuf64* times);
_STXLIB_UCRT int __cdecl _wutime32(const wchar_t* path, struct __utimbuf32* times);
_STXLIB_UCRT int __cdecl _wutime64(const wchar_t* path, struct __utimbuf64* times);
_STXLIB_UCRT int __cdecl _futime32(int descriptor, struct __utimbuf32* times);
_STXLIB_UCRT int __cdecl _futime64(int descriptor, struct __utimbuf64* times);

/* The POSIX name (Learn: "utime"): struct utimbuf has __utimbuf64's layout. */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX int __cdecl utime(const char* path, struct utimbuf* times);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_SYS_UTIME_H */
