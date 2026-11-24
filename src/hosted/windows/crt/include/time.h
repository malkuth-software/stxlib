/* time.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.27).
 *
 * time_t is 64 bits. ucrtbase.dll exports the 64-bit forms under Microsoft
 * names (_time64, _mktime64, _gmtime64, ...) and not the standard names, so
 * each standard function is an inline form over its 64-bit export (Learn,
 * "time, _time32, _time64" and the matching pages).
 *
 * clock_t is a 32-bit long and CLOCKS_PER_SEC is 1000 (Learn, "clock"). The
 * members of struct tm are in the C standard's order (probe: _gmtime64 of
 * 365 days + 3661 s gives sec 1, min 1, hour 1, mday 1, mon 0, year 71,
 * wday 5, yday 0, isdst 0).
 */
#ifndef STXLIB_CRT_TIME_H
#define STXLIB_CRT_TIME_H

#include "stxlib_crt_common.h"

#define CLOCKS_PER_SEC ((clock_t)1000)
#define TIME_UTC 1

_STXLIB_CRT_BEGIN

typedef long clock_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

/* C17 7.27.1: whole seconds and nanoseconds; with a 64-bit time_t the
 * layout is the one _timespec64_get fills. */
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

_STXLIB_UCRT clock_t __cdecl clock(void);
_STXLIB_UCRT __time64_t __cdecl _time64(__time64_t* result);
_STXLIB_UCRT double __cdecl _difftime64(__time64_t later, __time64_t earlier);
_STXLIB_UCRT __time64_t __cdecl _mktime64(struct tm* time);
_STXLIB_UCRT __time64_t __cdecl _mkgmtime64(struct tm* time);
_STXLIB_UCRT struct tm* __cdecl _gmtime64(const __time64_t* time);
_STXLIB_UCRT struct tm* __cdecl _localtime64(const __time64_t* time);
_STXLIB_UCRT errno_t __cdecl _gmtime64_s(struct tm* result, const __time64_t* time);
_STXLIB_UCRT errno_t __cdecl _localtime64_s(struct tm* result, const __time64_t* time);
_STXLIB_UCRT char* __cdecl _ctime64(const __time64_t* time);
_STXLIB_UCRT int __cdecl _timespec64_get(struct timespec* result, int base);
_STXLIB_UCRT char* __cdecl asctime(const struct tm* time);
_STXLIB_UCRT size_t __cdecl strftime(char* buffer, size_t size, const char* format, const struct tm* time);
_STXLIB_UCRT size_t __cdecl _strftime_l(char* buffer, size_t size, const char* format, const struct tm* time,
                                        _locale_t locale);
_STXLIB_UCRT size_t __cdecl wcsftime(wchar_t* buffer, size_t size, const wchar_t* format, const struct tm* time);
_STXLIB_UCRT void __cdecl _tzset(void);

_STXLIB_CRT_INLINE time_t __cdecl time(time_t* result) {
    const time_t now = _time64(result);
    return now;
}
_STXLIB_CRT_INLINE double __cdecl difftime(time_t later, time_t earlier) {
    const double seconds = _difftime64(later, earlier);
    return seconds;
}
_STXLIB_CRT_INLINE time_t __cdecl mktime(struct tm* time) {
    const time_t calendar = _mktime64(time);
    return calendar;
}
_STXLIB_CRT_INLINE struct tm* __cdecl gmtime(const time_t* time) {
    struct tm* const broken_down = _gmtime64(time);
    return broken_down;
}
_STXLIB_CRT_INLINE struct tm* __cdecl localtime(const time_t* time) {
    struct tm* const broken_down = _localtime64(time);
    return broken_down;
}
_STXLIB_CRT_INLINE char* __cdecl ctime(const time_t* time) {
    char* const text = _ctime64(time);
    return text;
}
_STXLIB_CRT_INLINE int __cdecl timespec_get(struct timespec* result, int base) {
    const int status = _timespec64_get(result, base);
    return status;
}
/* Microsoft's argument order: the result first (Learn, "gmtime_s"). */
_STXLIB_CRT_INLINE errno_t __cdecl gmtime_s(struct tm* result, const time_t* time) {
    const errno_t status = _gmtime64_s(result, time);
    return status;
}
_STXLIB_CRT_INLINE errno_t __cdecl localtime_s(struct tm* result, const time_t* time) {
    const errno_t status = _localtime64_s(result, time);
    return status;
}

/* Learn: "_daylight, _dstbias, _timezone, and _tzname" (the variables are
 * reached through __daylight, __dstbias, __timezone and __tzname, which
 * return their addresses), "_get_daylight", "_get_timezone",
 * "_get_tzname", "_get_dstbias", "asctime_s, _wasctime_s", "_strdate",
 * "_strtime". */
_STXLIB_UCRT int* __cdecl __daylight(void);
_STXLIB_UCRT long* __cdecl __dstbias(void);
_STXLIB_UCRT long* __cdecl __timezone(void);
_STXLIB_UCRT char** __cdecl __tzname(void);
#define _daylight (*__daylight())
#define _dstbias (*__dstbias())
#define _timezone (*__timezone())
#define _tzname (__tzname())
_STXLIB_UCRT errno_t __cdecl _get_daylight(int* hours);
_STXLIB_UCRT errno_t __cdecl _get_dstbias(long* seconds);
_STXLIB_UCRT errno_t __cdecl _get_timezone(long* seconds);
_STXLIB_UCRT errno_t __cdecl _get_tzname(size_t* length, char* buffer, size_t size, int index);
_STXLIB_UCRT errno_t __cdecl asctime_s(char* buffer, size_t size, const struct tm* time);
_STXLIB_UCRT char* __cdecl _strdate(char* buffer);
_STXLIB_UCRT char* __cdecl _strtime(char* buffer);
_STXLIB_UCRT errno_t __cdecl _strdate_s(char* buffer, size_t size);
_STXLIB_UCRT errno_t __cdecl _strtime_s(char* buffer, size_t size);

/* The POSIX name tzset (Learn: "_tzset"). The variables' POSIX names
 * (daylight, timezone, tzname) are not declared: ucrtbase.dll exports no
 * variable to alias, and a macro would rename struct timeb's timezone. */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX void __cdecl tzset(void);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_TIME_H */
