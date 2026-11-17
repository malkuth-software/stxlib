/* signal.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.14).
 *
 * Learn, "signal" (ms.date 2018-04-12), lists SIGABRT, SIGFPE, SIGILL,
 * SIGINT, SIGSEGV and SIGTERM without values. Probe: signal() accepts
 * exactly 2, 4, 6, 8, 11, 15, 21 and 22; a handler installed on all of them
 * receives 22 when abort() runs, so SIGABRT is 22. The others carry the
 * traditional numbers (SIGINT 2, SIGILL 4, SIGFPE 8, SIGSEGV 11, SIGTERM
 * 15); 6 is the older abort number and 21 the console break; which accepted
 * number is which of these is not separately observable (unconfirmed).
 * SIG_DFL is 0 (probe: the initial handler of every signal) and SIG_ERR is
 * -1 (probe: the result for a refused number); SIG_IGN is 1 (probe3).
 */
#ifndef STXLIB_CRT_SIGNAL_H
#define STXLIB_CRT_SIGNAL_H

#include "stxlib_crt_common.h"

#define SIGINT 2
#define SIGILL 4
#define SIGABRT_COMPAT 6
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15
#define SIGBREAK 21
#define SIGABRT 22

_STXLIB_CRT_BEGIN

typedef int sig_atomic_t;
typedef void(__cdecl* _crt_signal_t)(int);

#define SIG_DFL ((_crt_signal_t)0)
#define SIG_IGN ((_crt_signal_t)1)
#define SIG_ERR ((_crt_signal_t)-1)

_STXLIB_UCRT _crt_signal_t __cdecl signal(int number, _crt_signal_t handler);
_STXLIB_UCRT int __cdecl raise(int number);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_SIGNAL_H */
