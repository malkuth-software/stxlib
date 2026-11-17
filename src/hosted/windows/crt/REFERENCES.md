# References: STXlib's C library headers for x86_64-pc-windows-msvc

Clean room. The headers in `include/` were written from the sources below
only. Never opened: the Windows SDK's or Visual C++'s header sources
(`ucrt/`, `um/`, `shared/`, `vcruntime*.h`), the UCRT sources, Wine,
ReactOS, mingw-w64. All read or observed 2026-11-17.

## Standards

- ISO/IEC 9899:2018 (C17), clauses 7.2 to 7.30: every standard name,
  signature, macro and type, and the semantics the inline forms implement
  (snprintf's and vswprintf's return values, mbstate_t's zero initial state,
  the members of `struct tm`, `div_t`, `struct timespec`).

## Microsoft Learn (C runtime library reference)

Names, signatures and stated values. Page `ms.date` where it was read:

- "errno constants" (2021-04-26): every `E*` value including the POSIX
  supplement 100 to 140, and `STRUNCATE` 80.
- "errno, _doserrno, _sys_errlist, and _sys_nerr": errno is per thread.
- "_open, _wopen" (2022-05-18): the `_O_*` and `_S_I*` names (no values), the
  headers, the variadic signature.
- "setbuf", "setvbuf" (2020-04-02): `BUFSIZ` is the buffer length; the three
  modes, and `_IOLBF` behaving as `_IOFBF` on Windows.
- "localeconv" (2020-04-02): the members of `struct lconv` and the `_W_`
  wide members.
- "fegetround, fesetround" (2018-04-05): `FE_DOWNWARD` 0x0100 and
  `FE_UPWARD` 0x0200 since Windows 10 version 14393.
- "signal" (2018-04-12): the six signal names, the handler type, `SIG_ERR`
  with `EINVAL`, `SIGABRT` raised by abort.
- "clock" (2016-11-04): `clock_t` is a 32-bit long, `CLOCKS_PER_SEC` is 1000.
- Signatures and headers, one page each: `_initterm`/`_initterm_e`,
  `_crt_atexit`/`_crt_at_quick_exit`, `_initialize_onexit_table`/
  `_register_onexit_function`/`_execute_onexit_table`,
  `_configure_narrow_argv`/`_configure_wide_argv`,
  `_initialize_narrow_environment`/`_initialize_wide_environment`,
  `_get_initial_narrow_environment`/`_get_initial_wide_environment`,
  `__p___argc`/`__p___argv`/`__p___wargv`,
  `_get_narrow_winmain_command_line`/`_get_wide_winmain_command_line`,
  `_set_app_type`, `_register_thread_local_exe_atexit_callback`,
  `_beginthreadex`, `_aligned_malloc` and family, `_msize`, `_callnewh`,
  `_create_locale`, `_free_locale`, `_configthreadlocale`, the `_l` locale
  forms, `_strdup`, `_putenv_s`, `_wgetenv`, `rand_s`, `_set_abort_behavior`,
  `_get_osfhandle`, `_setmode`, `_fseeki64`/`_ftelli64`, `_time64` and the
  other 64-bit time functions, `gmtime_s`/`localtime_s` (result first),
  `_wassert`, `strerror_s`, `_snprintf`/`_snwprintf` (the exact-fit and
  truncation results), "errno, _doserrno, _sys_errlist, and _sys_nerr"
  (declared in stdlib.h too), `wcrtomb_s`, `terminate`/`set_terminate`/
  `_get_terminate`, "Path field limits" (`_MAX_PATH` 260), "C11
  aligned_alloc" (not provided by the UCRT), "Format specification syntax".

## LLVM 22.1.8 (Apache-2.0 with LLVM exception)

Source tarball `llvm-project-22.1.8.src.tar.xz`, sha256
`922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888`.

- clang `lib/CodeGen/CGBuiltin.cpp`, `EmitMSVCRTSetJmp`: `_setjmp(buffer)`
  becomes a call `_setjmp(buffer, frame address)` on x86_64.
- clang's resource headers (`stdint.h`, `limits.h`, `float.h`,
  `inttypes.h`, `intrin.h`, `vadefs.h`, `stdatomic.h`): which of our headers
  they `#include_next` (`inttypes.h` always; `setjmp.h` from `intrin.h`).
- clang's predefined `__*_FMT*__` macros (`clang -dM -E`): the `PRI`/`SCN`
  strings.
- libc++ `include/__locale` (the `_LIBCPP_MSVCRT_LIKE` masks, and the note
  that 0x0100 and 0x8000 are used), `include/__locale_dir/support/windows.h`
  (the `_l` functions called, `_CATMASK`, `_ENABLE_PER_THREAD_LOCALE`),
  `src/support/win32/locale_win32.cpp` (`__stdio_common_vsprintf` with
  `_CRT_INTERNAL_LOCAL_PRINTF_OPTIONS | _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR`,
  `___mb_cur_max_l_func`), `src/locale.cpp` (`__pctype_func`), `include/math.h`
  and `include/__math/traits.h` (the classification overloads the C library
  provides in C++), `include/wchar.h` (`_CRT_CONST_CORRECT_OVERLOADS`),
  `src/support/runtime/exception_msvc.ipp` (the terminate declarations),
  `src/filesystem/posix_compat.h` and `file_descriptor.h` (the `O_*` flags,
  `_wopen`, `_close`, `off_t` beside `io.h`), `src/string.cpp`
  (`_snwprintf`), `src/system_error.cpp` and `src/ios.cpp` (`_sys_nerr`),
  `src/support/win32/support.cpp` (`errno` through `stdlib.h`), `include/__system_error/errc.h` (the `E*` names).

## Binary observation of ucrtbase.dll

- Export names: `llvm-readobj --coff-exports C:/Windows/System32/ucrtbase.dll`
  (file version 10.0.26100.9711, Windows 11 10.0.26200), saved as
  `ucrtbase_exports.txt`. It decides which names are imports and which need
  inline forms (no `printf`, `snprintf`, `time`, `gmtime`, `fileno`, `fdopen`,
  `hypotf`, `fabsf`, `frexpf`, `ldexpf`, long double functions, `wmem*`,
  `mbsinit`, `feraiseexcept`, `feupdateenv`, `atexit`, `_onexit`).
- Behaviour probes: four small programs (`D:/gw_crt_work/probe/probe*.c`,
  hand-written prototypes, linked only against an import library made from
  a `.def`, run on this machine). What each established:

| Value | Observation |
| --- | --- |
| `__acrt_iob_func` | `_fileno` of the results for 0, 1, 2 is 0, 1, 2 |
| `_errno` | reads back the value `_set_errno(42)` stored |
| printf options | 4-element buffer, 6-character result: 0 truncates, terminates, returns -2; bit 0 leaves it unterminated (-1); bit 1 returns 6 (C's snprintf); bits 2-7 no change; same for the wide entry; options 0 make wide `%s` narrow; scanf options 0 read `%s` without a size; count `SIZE_MAX` with bit 1 formats normally |
| ctype bits | `__pctype_func()` in "C": 0x20 for 1, 0x28 for `\t` and `\n`, 0x48 for space, 0x10 `!`, 0x84 `0`, 0x81 `A`, 0x01 `G`, 0x82 `a`, 0x02 `g` |
| `FE_*` exceptions | 1/0 → 0x8, 0/0 → 0x10, overflow → 0x5, underflow → 0x3, 1/3 → 0x1 |
| `FE_*` rounding | 0, 0x100, 0x200, 0x300 accepted with nearest, down, up, toward-zero results for ±1/3; 0x400 refused |
| `fenv_t`, `mbstate_t`, `fpos_t` | `fegetenv`, `mbrtowc`, `fgetpos` write 8 bytes |
| `LC_*` | 0-5 accepted, 6 refused; "de-DE" per category moved strcoll (1), the UTF-8 length (2), the currency (3), the decimal point (4), weekday names (5) |
| `struct lconv` | "en-US": ten strings in Learn's order, eight numbers (2 2 1 0 1 0 3 0), eight wide strings |
| `struct tm`, `div_t` | `_gmtime64(365 d + 3661 s)` and `div(7, 2)` field order |
| `_O_*` | read/write behaviour of 0, 1, 2; 0x8 appends; 0x100 creates; 0x200 alone refused; 0x40 deletes on close; 0x400 with 0x100 on an existing file fails with EEXIST; `_setmode` accepts 0x4000-0x40000, default 0x4000 |
| `_S_IWRITE` | only 0x80 made a created file writable |
| `_IO*`, `BUFSIZ` | `setvbuf` accepts 0, 4, 0x40; 4 writes through; `setbuf` uses 512 bytes |
| `WEOF` | `fgetwc` at end of file returns 0xFFFF |
| signals | accepted 2, 4, 6, 8, 11, 15, 21, 22; abort raises 22; `SIG_IGN` 1 reads back; `SIG_DFL` 0 initially |
| `_crt_argv_mode` | `_configure_narrow_argv` accepts 0-2, refuses 3 |
| `_configthreadlocale` | starts at 2; 1 and 2 accepted and read back |
| `FP_ILOGB0`, `FP_ILOGBNAN` | `ilogb(0)` INT_MIN, `ilogb(NaN)` INT_MAX |
| `math_errhandling` | `log(-1)` sets EDOM and FE_INVALID |
| `TIME_UTC` | `_timespec64_get(…, 1)` returns 1, base 0 returns 0 |
| `RAND_MAX` | maximum of 200000 `rand()` results is 32767 |
| `_set_abort_behavior` | current flags in a release process are 0x2 |
| `__sys_nerr`, `__sys_errlist` | return addresses: `*__sys_nerr()` is 43, `__sys_errlist()[2]` equals `strerror(2)` |
| `_snwprintf` (option bit 0) | `%d` of 1234 into 4 returns 4 and leaves element 4 alone; 12345 returns -1; 123 returns 3, terminated |

## Unconfirmed

- Which accepted signal numbers are SIGINT, SIGILL, SIGFPE, SIGSEGV and
  SIGTERM (only SIGABRT = 22 was observed); the traditional numbers are used.
- `_S_IREAD` 0x100 (accepted, no observable effect on Windows).
- `_WRITE_ABORT_MSG` 0x1 and `_CALL_REPORTFAULT` 0x2: only the release
  default 0x2 was observed.
- `_ALPHA`'s 0x0100 bit (no ASCII character carries it in "C"; libc++'s note).
- `_crt_app_type` values (unknown 0, console 1, GUI 2): `_set_app_type`
  returns nothing observable.
- `_onexit_table_t` as three pointers: the layout the onexit functions read
  (Learn names them; the size was not probed).
- `FOPEN_MAX` 20.
