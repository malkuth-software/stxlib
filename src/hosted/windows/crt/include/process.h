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

_STXLIB_CRT_END

#endif /* STXLIB_CRT_PROCESS_H */
