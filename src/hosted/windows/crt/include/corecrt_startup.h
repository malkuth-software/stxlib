/* corecrt_startup.h for x86_64-pc-windows-msvc over ucrtbase.dll: the UCRT
 * exports a program's own startup code calls (here STXlib's startup shim,
 * src/hosted/windows_msvc_startup/).
 *
 * The header name is the one Microsoft's startup code includes; its contents
 * here are written from Learn's pages ("CRT initialization", "_initterm,
 * _initterm_e", "_crt_atexit, _crt_at_quick_exit", "_execute_onexit_table,
 * _initialize_onexit_table, _register_onexit_function", "_configure_narrow_argv,
 * _configure_wide_argv", "_initialize_narrow_environment,
 * _initialize_wide_environment", "_get_initial_narrow_environment,
 * _get_initial_wide_environment", "__p___argc, __p___argv, __p___wargv",
 * "_get_narrow_winmain_command_line, _get_wide_winmain_command_line",
 * "_set_app_type"). Values Learn does not give are in REFERENCES.md.
 */
#ifndef STXLIB_CRT_CORECRT_STARTUP_H
#define STXLIB_CRT_CORECRT_STARTUP_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

/* An initializer or terminator table entry (.CRT$XC*, .CRT$XP*, .CRT$XT*):
 * no arguments, no result. */
typedef void(__cdecl* _PVFV)(void);
/* A C initializer table entry (.CRT$XI*): nonzero ends startup with that
 * status. */
typedef int(__cdecl* _PIFV)(void);
/* An _onexit function: its int result is ignored by the table. */
typedef int(__cdecl* _onexit_t)(void);

/* A module's exit table: a growable array of _PVFV. _first is the first
 * entry, _last one past the last registered entry, _end one past the
 * allocated capacity; all three null for an empty table before
 * _initialize_onexit_table. The UCRT owns the storage once an entry is
 * registered and releases it in _execute_onexit_table. Three pointers is the
 * layout the UCRT's functions read (REFERENCES.md, "onexit table"). */
typedef struct _onexit_table_t {
    _PVFV* _first;
    _PVFV* _last;
    _PVFV* _end;
} _onexit_table_t;

/* _configure_narrow_argv's argument. ucrtbase accepts 0, 1 and 2 and refuses
 * 3 with EINVAL (probe); the names follow Learn's description: no
 * arguments, arguments as given, arguments with wildcards expanded. */
typedef enum _crt_argv_mode {
    _crt_argv_no_arguments = 0,
    _crt_argv_unexpanded_arguments = 1,
    _crt_argv_expanded_arguments = 2
} _crt_argv_mode;

/* _set_app_type's argument (REFERENCES.md: unknown, console, GUI). */
typedef enum _crt_app_type {
    _crt_unknown_app = 0,
    _crt_console_app = 1,
    _crt_gui_app = 2
} _crt_app_type;

_STXLIB_UCRT void __cdecl _initterm(_PVFV* first, _PVFV* last);
_STXLIB_UCRT int __cdecl _initterm_e(_PIFV* first, _PIFV* last);

_STXLIB_UCRT int __cdecl _crt_atexit(_PVFV function);
_STXLIB_UCRT int __cdecl _crt_at_quick_exit(_PVFV function);

_STXLIB_UCRT int __cdecl _initialize_onexit_table(_onexit_table_t* table);
_STXLIB_UCRT int __cdecl _register_onexit_function(_onexit_table_t* table, _onexit_t function);
_STXLIB_UCRT int __cdecl _execute_onexit_table(_onexit_table_t* table);

_STXLIB_UCRT errno_t __cdecl _configure_narrow_argv(_crt_argv_mode mode);
_STXLIB_UCRT errno_t __cdecl _configure_wide_argv(_crt_argv_mode mode);
_STXLIB_UCRT int __cdecl _initialize_narrow_environment(void);
_STXLIB_UCRT int __cdecl _initialize_wide_environment(void);
_STXLIB_UCRT char** __cdecl _get_initial_narrow_environment(void);
_STXLIB_UCRT wchar_t** __cdecl _get_initial_wide_environment(void);
_STXLIB_UCRT char* __cdecl _get_narrow_winmain_command_line(void);
_STXLIB_UCRT wchar_t* __cdecl _get_wide_winmain_command_line(void);

_STXLIB_UCRT int* __cdecl __p___argc(void);
_STXLIB_UCRT char*** __cdecl __p___argv(void);
_STXLIB_UCRT wchar_t*** __cdecl __p___wargv(void);

_STXLIB_UCRT void __cdecl _set_app_type(_crt_app_type type);

_STXLIB_CRT_END

#endif /* STXLIB_CRT_CORECRT_STARTUP_H */
