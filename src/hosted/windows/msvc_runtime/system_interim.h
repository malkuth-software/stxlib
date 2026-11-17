/* The Windows and UCRT declarations STXlib's MSVC C++ runtime and startup
 * shim use, private to those two directories, so that both build with
 * -nostdinc against STXlib's own C headers (src/hosted/windows/crt/include)
 * and nothing from the Windows SDK or the Visual C++ tools.
 *
 * Interim: once STXlib's Windows API headers (src/hosted/windows/api/
 * include) land, the Win32 half gives way to their windows.h; the UCRT
 * half stays wherever the C headers do not cover it. Either way this file
 * is never included in the same translation unit as windows.h (checked
 * below).
 *
 * Clean room (REFERENCES.md, "system_interim.h"): every structure, constant
 * and prototype here is written from Microsoft Learn's documentation of
 * it, read 2026-11-17, and the structure sizes are checked against the
 * sizes the documentation implies. Where Learn gives a name and meaning but
 * no numeric value, the value is marked "general knowledge" and is
 * confirmed by the conformance suite or the startup probes. No Windows SDK,
 * UCRT, vcruntime, Wine, ReactOS or mingw-w64 header was read to write it.
 *
 * x86-64 only. Calling-convention keywords are spelled as the documentation
 * spells them; on x64 they all name the one Microsoft x64 convention. */

#ifndef STXLIB_HOSTED_WINDOWS_MSVC_RUNTIME_SYSTEM_INTERIM_H
#define STXLIB_HOSTED_WINDOWS_MSVC_RUNTIME_SYSTEM_INTERIM_H

#if !defined(_M_X64) && !defined(__x86_64__)
#error "system_interim.h is written for x86-64 Windows only"
#endif
#if defined(_WINDOWS_) || defined(_WINNT_)
#error "system_interim.h and windows.h declare the same names; include one"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Windows data types (Learn: "Windows Data Types") ------------------- */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long DWORD64;
typedef unsigned long long ULONG_PTR;
typedef int BOOL;
typedef BYTE BOOLEAN;
typedef void* PVOID;
typedef void* LPVOID;
typedef void* HANDLE;
/* wchar_t is a keyword in C++ and stddef.h's typedef in C; the C form is
 * spelled through the compiler's own macro so this header needs no C
 * library header. */
#ifdef __cplusplus
#define STXLIB_WCHAR wchar_t
#else
#define STXLIB_WCHAR __WCHAR_TYPE__
#endif
typedef STXLIB_WCHAR* LPWSTR;
typedef char* LPSTR;
typedef BYTE* LPBYTE;
typedef DWORD64* PDWORD64;

/* An opaque module handle, a distinct pointer type as the SDK makes it. */
struct HINSTANCE__;
typedef struct HINSTANCE__* HINSTANCE;
typedef HINSTANCE HMODULE;

#define WINAPI __stdcall
#define NTAPI __stdcall
#define CALLBACK __stdcall

#define TRUE 1
#define FALSE 0

/* Every kernel32 function here is imported from kernel32.dll. */
#define STXLIB_KERNEL32_IMPORT __declspec(dllimport)

/* ---- Structured exception handling ---------------------------------------- */

/* Learn: "EXCEPTION_RECORD structure (winnt.h)". */
#define EXCEPTION_MAXIMUM_PARAMETERS 15

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

/* ExceptionFlags. EXCEPTION_NONCONTINUABLE: Learn, "EXCEPTION_RECORD" and
 * "RaiseException". The unwind flags (Learn, "x64 exception handling",
 * language-specific handler: EXCEPTION_UNWINDING, EXCEPTION_EXIT_UNWIND,
 * EXCEPTION_TARGET_UNWIND, EXCEPTION_COLLIDED_UNWIND); their values:
 * general knowledge, the same bits LLVM's runtime tests use. */
#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_UNWINDING 0x2
#define EXCEPTION_EXIT_UNWIND 0x4
#define EXCEPTION_TARGET_UNWIND 0x20
#define EXCEPTION_COLLIDED_UNWIND 0x40

/* Status codes (Learn, "NTSTATUS Values"). */
#define STATUS_LONGJUMP 0x80000026UL
#define STATUS_UNWIND_CONSOLIDATE 0x80000029UL

/* Learn: "CONTEXT structure (winnt.h)", x64. Nothing here reads its
 * fields: the runtime only passes contexts to the system. The members the
 * documentation lists come to 1232 bytes (six home quadwords, ContextFlags
 * and MxCsr, six segment words and EFlags, six debug registers, sixteen
 * integer registers and Rip, the 512-byte legacy floating-point save area,
 * 26 vector registers of 16 bytes, VectorControl and five debug-control
 * quadwords), aligned to 16. */
typedef struct __attribute__((aligned(16))) _CONTEXT {
    DWORD64 P1Home;
    DWORD64 P2Home;
    DWORD64 P3Home;
    DWORD64 P4Home;
    DWORD64 P5Home;
    DWORD64 P6Home;
    DWORD ContextFlags;
    DWORD MxCsr;
    WORD SegCs;
    WORD SegDs;
    WORD SegEs;
    WORD SegFs;
    WORD SegGs;
    WORD SegSs;
    DWORD EFlags;
    DWORD64 Dr0;
    DWORD64 Dr1;
    DWORD64 Dr2;
    DWORD64 Dr3;
    DWORD64 Dr6;
    DWORD64 Dr7;
    DWORD64 Rax;
    DWORD64 Rcx;
    DWORD64 Rdx;
    DWORD64 Rbx;
    DWORD64 Rsp;
    DWORD64 Rbp;
    DWORD64 Rsi;
    DWORD64 Rdi;
    DWORD64 R8;
    DWORD64 R9;
    DWORD64 R10;
    DWORD64 R11;
    DWORD64 R12;
    DWORD64 R13;
    DWORD64 R14;
    DWORD64 R15;
    DWORD64 Rip;
    BYTE FloatSave[512];
    BYTE VectorRegister[26][16];
    DWORD64 VectorControl;
    DWORD64 DebugControl;
    DWORD64 LastBranchToRip;
    DWORD64 LastBranchFromRip;
    DWORD64 LastExceptionToRip;
    DWORD64 LastExceptionFromRip;
} CONTEXT, *PCONTEXT;

/* Learn: "EXCEPTION_POINTERS structure (winnt.h)". */
typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

/* Learn: "x64 exception handling", RUNTIME_FUNCTION. */
typedef struct _RUNTIME_FUNCTION {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
} RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;

/* Learn: "x64 exception handling", language-specific handler: the
 * dispositions a handler returns, in this order. */
typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution = 0,
    ExceptionContinueSearch = 1,
    ExceptionNestedException = 2,
    ExceptionCollidedUnwind = 3
} EXCEPTION_DISPOSITION;

typedef EXCEPTION_DISPOSITION(__cdecl* PEXCEPTION_ROUTINE)(PEXCEPTION_RECORD ExceptionRecord, PVOID EstablisherFrame,
                                                          PCONTEXT ContextRecord, PVOID DispatcherContext);

/* Opaque here: the runtime only passes the system's own pointer on. */
typedef struct _UNWIND_HISTORY_TABLE UNWIND_HISTORY_TABLE, *PUNWIND_HISTORY_TABLE;
typedef struct _KNONVOLATILE_CONTEXT_POINTERS KNONVOLATILE_CONTEXT_POINTERS, *PKNONVOLATILE_CONTEXT_POINTERS;

/* Learn: "x64 exception handling", language-specific handler,
 * DISPATCHER_CONTEXT. */
typedef struct _DISPATCHER_CONTEXT {
    DWORD64 ControlPc;
    DWORD64 ImageBase;
    PRUNTIME_FUNCTION FunctionEntry;
    DWORD64 EstablisherFrame;
    DWORD64 TargetIp;
    PCONTEXT ContextRecord;
    PEXCEPTION_ROUTINE LanguageHandler;
    PVOID HandlerData;
    PUNWIND_HISTORY_TABLE HistoryTable;
    DWORD ScopeIndex;
    DWORD Fill0;
} DISPATCHER_CONTEXT, *PDISPATCHER_CONTEXT;

/* Learn: "UnhandledExceptionFilter" and "SetUnhandledExceptionFilter";
 * the filter results (Learn, "try-except statement"). */
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

typedef LONG(WINAPI* LPTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS ExceptionInfo);

/* Learn: "RaiseException function (errhandlingapi.h)". */
STXLIB_KERNEL32_IMPORT void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
                                                  const ULONG_PTR* lpArguments);
/* Learn: "SetUnhandledExceptionFilter function (errhandlingapi.h)". */
STXLIB_KERNEL32_IMPORT LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
/* Learn: "RtlUnwindEx function (winnt.h)". */
STXLIB_KERNEL32_IMPORT void NTAPI RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord,
                                              PVOID ReturnValue, PCONTEXT ContextRecord, PUNWIND_HISTORY_TABLE HistoryTable);
/* Learn: "RtlPcToFileHeader function (winnt.h)". */
STXLIB_KERNEL32_IMPORT PVOID NTAPI RtlPcToFileHeader(PVOID PcValue, PVOID* BaseOfImage);
/* Learn: "RtlLookupFunctionEntry function (winnt.h)". */
STXLIB_KERNEL32_IMPORT PRUNTIME_FUNCTION NTAPI RtlLookupFunctionEntry(DWORD64 ControlPc, PDWORD64 ImageBase,
                                                                     PUNWIND_HISTORY_TABLE HistoryTable);
/* Learn: "RtlVirtualUnwind function (winnt.h)". */
STXLIB_KERNEL32_IMPORT PEXCEPTION_ROUTINE NTAPI RtlVirtualUnwind(DWORD HandlerType, DWORD64 ImageBase, DWORD64 ControlPc,
                                                                PRUNTIME_FUNCTION FunctionEntry, PCONTEXT ContextRecord,
                                                                PVOID* HandlerData, PDWORD64 EstablisherFrame,
                                                                PKNONVOLATILE_CONTEXT_POINTERS ContextPointers);
/* Learn: "RtlCaptureContext function (winnt.h)", "RtlRestoreContext
 * function (winnt.h)". */
STXLIB_KERNEL32_IMPORT void NTAPI RtlCaptureContext(PCONTEXT ContextRecord);
STXLIB_KERNEL32_IMPORT void NTAPI RtlRestoreContext(PCONTEXT ContextRecord, PEXCEPTION_RECORD ExceptionRecord);

/* Learn: "__fastfail" (an intrinsic clang declares in its intrin.h; it is
 * declared here so no intrinsics header is needed). The codes: Learn,
 * "__fastfail", and "/GS (Buffer Security Check)"; values general
 * knowledge (FAST_FAIL_STACK_COOKIE_CHECK_FAILURE 2, FAST_FAIL_FATAL_APP_EXIT
 * 7, the latter the documented code abort reports). */
__attribute__((noreturn)) void __fastfail(unsigned int code);
#define FAST_FAIL_STACK_COOKIE_CHECK_FAILURE 2
#define FAST_FAIL_FATAL_APP_EXIT 7

/* ---- Thread-local storage (Learn: "TlsAlloc", "TlsFree", "TlsGetValue",
 * "TlsSetValue"; TLS_OUT_OF_INDEXES is TlsAlloc's documented failure
 * value, 0xFFFFFFFF) -------------------------------------------------------- */

#define TLS_OUT_OF_INDEXES 0xFFFFFFFFUL
STXLIB_KERNEL32_IMPORT DWORD WINAPI TlsAlloc(void);
STXLIB_KERNEL32_IMPORT BOOL WINAPI TlsFree(DWORD dwTlsIndex);
STXLIB_KERNEL32_IMPORT LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex);
STXLIB_KERNEL32_IMPORT BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue);

/* ---- Heap (Learn: "GetProcessHeap", "HeapAlloc", "HeapFree") ------------ */

STXLIB_KERNEL32_IMPORT HANDLE WINAPI GetProcessHeap(void);
STXLIB_KERNEL32_IMPORT LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, ULONG_PTR dwBytes);
STXLIB_KERNEL32_IMPORT BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);

/* ---- Synchronization (Learn: "Slim Reader/Writer (SRW) Locks",
 * "Condition Variables", "SleepConditionVariableSRW"; SRWLOCK_INIT and
 * CONDITION_VARIABLE_INIT are documented as the static initialisers, a
 * single null pointer) ------------------------------------------------------ */

typedef struct _RTL_SRWLOCK {
    PVOID Ptr;
} SRWLOCK, *PSRWLOCK;
typedef struct _RTL_CONDITION_VARIABLE {
    PVOID Ptr;
} CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define SRWLOCK_INIT {0}
#define CONDITION_VARIABLE_INIT {0}
#define INFINITE 0xFFFFFFFFUL

STXLIB_KERNEL32_IMPORT void WINAPI AcquireSRWLockExclusive(PSRWLOCK SRWLock);
STXLIB_KERNEL32_IMPORT void WINAPI ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
STXLIB_KERNEL32_IMPORT BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE ConditionVariable, PSRWLOCK SRWLock,
                                                             DWORD dwMilliseconds, ULONG Flags);
STXLIB_KERNEL32_IMPORT void WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE ConditionVariable);

/* ---- Time and identity (Learn: "QueryPerformanceCounter", "LARGE_INTEGER
 * union", "GetSystemTimePreciseAsFileTime", "FILETIME structure",
 * "GetCurrentProcessId", "GetCurrentThreadId") ----------------------------- */

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER;
typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

STXLIB_KERNEL32_IMPORT BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
STXLIB_KERNEL32_IMPORT void WINAPI GetSystemTimePreciseAsFileTime(FILETIME* lpSystemTimeAsFileTime);
STXLIB_KERNEL32_IMPORT DWORD WINAPI GetCurrentProcessId(void);
STXLIB_KERNEL32_IMPORT DWORD WINAPI GetCurrentThreadId(void);

/* ---- Process start (Learn: "STARTUPINFOW structure", "GetStartupInfoW",
 * "WinMain function", "DllMain entry point"; STARTF_USESHOWWINDOW 1 and
 * SW_SHOWDEFAULT 10: Learn, "STARTUPINFOW" and "ShowWindow") -------------- */

typedef struct _STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOW;
#define STARTF_USESHOWWINDOW 0x00000001UL
#define SW_SHOWDEFAULT 10

STXLIB_KERNEL32_IMPORT void WINAPI GetStartupInfoW(STARTUPINFOW* lpStartupInfo);

/* The program's own entry functions, which the startup calls. */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd);
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd);

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3

/* ---- Image structures (Learn: "PE Format": the TLS directory, TLS
 * callbacks; the DOS header only by name, for the linker's __ImageBase) --- */

typedef void(NTAPI* PIMAGE_TLS_CALLBACK)(PVOID DllHandle, DWORD Reason, PVOID Reserved);

typedef struct _IMAGE_TLS_DIRECTORY64 {
    ULONGLONG StartAddressOfRawData;
    ULONGLONG EndAddressOfRawData;
    ULONGLONG AddressOfIndex;
    ULONGLONG AddressOfCallBacks;
    DWORD SizeOfZeroFill;
    DWORD Characteristics;
} IMAGE_TLS_DIRECTORY64;

struct _IMAGE_DOS_HEADER;
typedef struct _IMAGE_DOS_HEADER IMAGE_DOS_HEADER;

/* ---- UCRT entry points beyond the C standard headers (ucrtbase.dll) -------
 * Learn: "_initterm, _initterm_e"; "_crt_atexit, _crt_at_quick_exit";
 * "_execute_onexit_table, _initialize_onexit_table,
 * _register_onexit_function"; "_set_app_type"; "_configure_narrow_argv,
 * _configure_wide_argv" and "_initialize_narrow_environment,
 * _initialize_wide_environment" and "_get_initial_narrow_environment,
 * _get_initial_wide_environment" and "__p___argc, __p___argv, __p___wargv"
 * and "_get_narrow_winmain_command_line, _get_wide_winmain_command_line"
 * (the "Internal CRT globals and functions" pages); "_aligned_malloc",
 * "_aligned_free", "_callnewh", "_exit", "terminate (CRT)". Imported
 * (dllimport) when built for the UCRT DLL (_DLL), as the C headers declare
 * them, so a translation unit that also includes those headers sees
 * matching declarations. */

#if defined(_DLL)
#define STXLIB_UCRT_IMPORT __declspec(dllimport)
#else
#define STXLIB_UCRT_IMPORT
#endif

typedef void(__cdecl* _PVFV)(void);
typedef int(__cdecl* _PIFV)(void);
typedef int(__cdecl* _onexit_t)(void);

typedef struct _onexit_table_t {
    _PVFV* _first;
    _PVFV* _last;
    _PVFV* _end;
} _onexit_table_t;

/* The values: general knowledge (unknown, console, GUI application; and no,
 * expanded, unexpanded arguments), confirmed by the startup probes. */
typedef enum _crt_app_type { _crt_unknown_app = 0, _crt_console_app = 1, _crt_gui_app = 2 } _crt_app_type;
typedef enum _crt_argv_mode {
    _crt_argv_no_arguments = 0,
    _crt_argv_expanded_arguments = 1,
    _crt_argv_unexpanded_arguments = 2
} _crt_argv_mode;

STXLIB_UCRT_IMPORT void __cdecl _initterm(_PVFV* first, _PVFV* last);
STXLIB_UCRT_IMPORT int __cdecl _initterm_e(_PIFV* first, _PIFV* last);
STXLIB_UCRT_IMPORT int __cdecl _initialize_onexit_table(_onexit_table_t* table);
STXLIB_UCRT_IMPORT int __cdecl _register_onexit_function(_onexit_table_t* table, _onexit_t function);
STXLIB_UCRT_IMPORT int __cdecl _execute_onexit_table(_onexit_table_t* table);
STXLIB_UCRT_IMPORT int __cdecl _crt_atexit(_PVFV function);
STXLIB_UCRT_IMPORT int __cdecl _crt_at_quick_exit(_PVFV function);
STXLIB_UCRT_IMPORT void __cdecl _set_app_type(_crt_app_type type);
STXLIB_UCRT_IMPORT int __cdecl _configure_narrow_argv(_crt_argv_mode mode);
STXLIB_UCRT_IMPORT int __cdecl _configure_wide_argv(_crt_argv_mode mode);
STXLIB_UCRT_IMPORT int __cdecl _initialize_narrow_environment(void);
STXLIB_UCRT_IMPORT int __cdecl _initialize_wide_environment(void);
STXLIB_UCRT_IMPORT char** __cdecl _get_initial_narrow_environment(void);
STXLIB_UCRT_IMPORT STXLIB_WCHAR** __cdecl _get_initial_wide_environment(void);
STXLIB_UCRT_IMPORT int* __cdecl __p___argc(void);
STXLIB_UCRT_IMPORT char*** __cdecl __p___argv(void);
STXLIB_UCRT_IMPORT STXLIB_WCHAR*** __cdecl __p___wargv(void);
STXLIB_UCRT_IMPORT char* __cdecl _get_narrow_winmain_command_line(void);
STXLIB_UCRT_IMPORT STXLIB_WCHAR* __cdecl _get_wide_winmain_command_line(void);
STXLIB_UCRT_IMPORT void __cdecl _register_thread_local_exe_atexit_callback(PIMAGE_TLS_CALLBACK callback);
STXLIB_UCRT_IMPORT void* __cdecl _aligned_malloc(unsigned long long size, unsigned long long alignment);
STXLIB_UCRT_IMPORT void __cdecl _aligned_free(void* block);
STXLIB_UCRT_IMPORT int __cdecl _callnewh(unsigned long long size);
STXLIB_UCRT_IMPORT __declspec(noreturn) void __cdecl _exit(int status);
STXLIB_UCRT_IMPORT __declspec(noreturn) void __cdecl terminate(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Sizes the documented member lists imply. */
#ifdef __cplusplus
static_assert(sizeof(EXCEPTION_RECORD) == 152, "EXCEPTION_RECORD layout");
static_assert(sizeof(CONTEXT) == 1232 && alignof(CONTEXT) == 16, "CONTEXT layout");
static_assert(sizeof(DISPATCHER_CONTEXT) == 80, "DISPATCHER_CONTEXT layout");
static_assert(sizeof(RUNTIME_FUNCTION) == 12, "RUNTIME_FUNCTION layout");
static_assert(sizeof(STARTUPINFOW) == 104, "STARTUPINFOW layout");
static_assert(sizeof(IMAGE_TLS_DIRECTORY64) == 40, "IMAGE_TLS_DIRECTORY64 layout");
#else
_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "EXCEPTION_RECORD layout");
_Static_assert(sizeof(CONTEXT) == 1232, "CONTEXT layout");
_Static_assert(sizeof(IMAGE_TLS_DIRECTORY64) == 40, "IMAGE_TLS_DIRECTORY64 layout");
#endif

#endif /* STXLIB_HOSTED_WINDOWS_MSVC_RUNTIME_SYSTEM_INTERIM_H */
