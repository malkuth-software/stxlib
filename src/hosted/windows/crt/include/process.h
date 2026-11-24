/* process.h for x86_64-pc-windows-msvc over ucrtbase.dll: the Microsoft
 * thread and process functions our sources use (Learn, "_beginthread,
 * _beginthreadex", "_endthread, _endthreadex", "_getpid",
 * "_register_thread_local_exe_atexit_callback").
 */
#ifndef STXLIB_CRT_PROCESS_H
#define STXLIB_CRT_PROCESS_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

/* _beginthreadex's thread body: its result becomes the thread's exit code. */
typedef unsigned(__stdcall* _beginthreadex_proc_type)(void* argument);
typedef void(__cdecl* _beginthread_proc_type)(void* argument);
/* The executable's thread_local destructor callback, called on thread and
 * process exit with the loader's DllMain-style arguments. */
typedef void(__stdcall* _tls_callback_type)(void* module, unsigned long reason, void* reserved);

_STXLIB_UCRT uintptr_t __cdecl _beginthreadex(void* security, unsigned stack_size, _beginthreadex_proc_type start_address,
                                              void* argument, unsigned initial_flags, unsigned* thread_identifier);
_STXLIB_UCRT uintptr_t __cdecl _beginthread(_beginthread_proc_type start_address, unsigned stack_size, void* argument);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl _endthreadex(unsigned exit_code);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl _endthread(void);
_STXLIB_UCRT int __cdecl _getpid(void);
_STXLIB_UCRT void __cdecl _register_thread_local_exe_atexit_callback(_tls_callback_type callback);

_STXLIB_UCRT _STXLIB_NORETURN void __cdecl exit(int status);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl _exit(int status);
_STXLIB_UCRT _STXLIB_NORETURN void __cdecl abort(void);
_STXLIB_UCRT int __cdecl system(const char* command);
_STXLIB_UCRT int __cdecl _wsystem(const wchar_t* command);

/* Learn: "_exec, _wexec functions", "_spawn, _wspawn functions" (the mode
 * values) and "_cwait". The l forms end their argument list with a null
 * pointer. */
#define _P_WAIT 0
#define _P_NOWAIT 1
#define _P_OVERLAY 2
#define _OLD_P_OVERLAY _P_OVERLAY
#define _P_NOWAITO 3
#define _P_DETACH 4
#define P_WAIT _P_WAIT
#define P_NOWAIT _P_NOWAIT
#define P_OVERLAY _P_OVERLAY
#define P_NOWAITO _P_NOWAITO
#define P_DETACH _P_DETACH
#define _WAIT_CHILD 0
#define _WAIT_GRANDCHILD 1
#define WAIT_CHILD _WAIT_CHILD
#define WAIT_GRANDCHILD _WAIT_GRANDCHILD

_STXLIB_UCRT intptr_t __cdecl _cwait(int* status, intptr_t process, int action);
_STXLIB_UCRT intptr_t __cdecl _execl(const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _execle(const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _execlp(const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _execlpe(const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _execv(const char* path, const char* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _execve(const char* path, const char* const* arguments, const char* const* environment);
_STXLIB_UCRT intptr_t __cdecl _execvp(const char* path, const char* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _execvpe(const char* path, const char* const* arguments, const char* const* environment);
_STXLIB_UCRT intptr_t __cdecl _spawnl(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _spawnle(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _spawnlp(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _spawnlpe(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _spawnv(int mode, const char* path, const char* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _spawnve(int mode, const char* path, const char* const* arguments, const char* const* environment);
_STXLIB_UCRT intptr_t __cdecl _spawnvp(int mode, const char* path, const char* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _spawnvpe(int mode, const char* path, const char* const* arguments, const char* const* environment);
_STXLIB_UCRT intptr_t __cdecl _wexecl(const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wexecle(const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wexeclp(const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wexeclpe(const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wexecv(const wchar_t* path, const wchar_t* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _wexecve(const wchar_t* path, const wchar_t* const* arguments, const wchar_t* const* environment);
_STXLIB_UCRT intptr_t __cdecl _wexecvp(const wchar_t* path, const wchar_t* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _wexecvpe(const wchar_t* path, const wchar_t* const* arguments, const wchar_t* const* environment);
_STXLIB_UCRT intptr_t __cdecl _wspawnl(int mode, const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wspawnle(int mode, const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wspawnlp(int mode, const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wspawnlpe(int mode, const wchar_t* path, const wchar_t* argument, ...);
_STXLIB_UCRT intptr_t __cdecl _wspawnv(int mode, const wchar_t* path, const wchar_t* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _wspawnve(int mode, const wchar_t* path, const wchar_t* const* arguments, const wchar_t* const* environment);
_STXLIB_UCRT intptr_t __cdecl _wspawnvp(int mode, const wchar_t* path, const wchar_t* const* arguments);
_STXLIB_UCRT intptr_t __cdecl _wspawnvpe(int mode, const wchar_t* path, const wchar_t* const* arguments, const wchar_t* const* environment);

/* The POSIX names (Learn: each function's page, "The POSIX name ... is
 * deprecated"), imported under the Microsoft names (stxlib_crt_common.h). */
#if _CRT_DECLARE_NONSTDC_NAMES
_STXLIB_UCRT_POSIX intptr_t __cdecl cwait(int* status, intptr_t process, int action);
_STXLIB_UCRT_POSIX int __cdecl getpid(void);
_STXLIB_UCRT_POSIX intptr_t __cdecl execl(const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl execle(const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl execlp(const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl execlpe(const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl execv(const char* path, const char* const* arguments);
_STXLIB_UCRT_POSIX intptr_t __cdecl execve(const char* path, const char* const* arguments, const char* const* environment);
_STXLIB_UCRT_POSIX intptr_t __cdecl execvp(const char* path, const char* const* arguments);
_STXLIB_UCRT_POSIX intptr_t __cdecl execvpe(const char* path, const char* const* arguments, const char* const* environment);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnl(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnle(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnlp(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnlpe(int mode, const char* path, const char* argument, ...);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnv(int mode, const char* path, const char* const* arguments);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnve(int mode, const char* path, const char* const* arguments, const char* const* environment);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnvp(int mode, const char* path, const char* const* arguments);
_STXLIB_UCRT_POSIX intptr_t __cdecl spawnvpe(int mode, const char* path, const char* const* arguments, const char* const* environment);
#endif

_STXLIB_CRT_END

#endif /* STXLIB_CRT_PROCESS_H */
