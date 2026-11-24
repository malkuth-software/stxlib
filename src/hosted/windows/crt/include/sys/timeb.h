/* sys/timeb.h for x86_64-pc-windows-msvc over ucrtbase.dll: the current time
 * with milliseconds (Learn, "_ftime, _ftime32, _ftime64" and
 * "_ftime_s, _ftime32_s, _ftime64_s"); with a 64-bit time_t, _ftime is
 * _ftime64 and struct _timeb is __timeb64's layout.
 */
#ifndef STXLIB_CRT_SYS_TIMEB_H
#define STXLIB_CRT_SYS_TIMEB_H

#include "../stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

struct __timeb32 {
    __time32_t time;
    unsigned short millitm;
    short timezone;
    short dstflag;
};
struct __timeb64 {
    __time64_t time;
    unsigned short millitm;
    short timezone;
    short dstflag;
};
struct _timeb {
    time_t time;
    unsigned short millitm;
    short timezone;
    short dstflag;
};
struct timeb {
    time_t time;
    unsigned short millitm;
    short timezone;
    short dstflag;
};
#define _ftime _ftime64
#define _ftime_s _ftime64_s

_STXLIB_UCRT void __cdecl _ftime32(struct __timeb32* now);
_STXLIB_UCRT void __cdecl _ftime64(struct __timeb64* now);
_STXLIB_UCRT errno_t __cdecl _ftime32_s(struct __timeb32* now);
_STXLIB_UCRT errno_t __cdecl _ftime64_s(struct __timeb64* now);

/* The POSIX name (Learn: "ftime"): struct timeb has __timeb64's layout. */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX void __cdecl ftime(struct timeb* now);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_SYS_TIMEB_H */
