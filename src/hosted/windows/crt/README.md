# STXlib's C library headers for x86_64-pc-windows-msvc

The C headers that GUI.Forms, stx, STXlib and libc++ 22.1.8 need on
Windows, written clean-room (REFERENCES.md lists every source and probe)
and declaring the C runtime that every Windows 10 and 11 installation
carries, `ucrtbase.dll`. No Windows SDK, Visual C++ or MinGW header or
library is used.

## Contents

- `include/`: the headers. `assert.h`, `corecrt_startup.h`, `ctype.h`,
  `complex.h`, `conio.h`, `direct.h`, `eh.h` (C++ only), `errno.h`,
  `fcntl.h`, `fenv.h`, `float.h` (the Microsoft control part only),
  `inttypes.h`, `io.h`, `locale.h`, `malloc.h`, `math.h`, `process.h`,
  `search.h`, `setjmp.h`, `signal.h`, `stdio.h`, `stdlib.h`, `string.h`,
  `sys/locking.h`, `sys/stat.h`, `sys/timeb.h`, `sys/types.h`,
  `sys/utime.h`, `time.h`, `uchar.h`, `wchar.h`, `wctype.h`, and
  `stxlib_crt_common.h` (the target check, the linkage macros and the shared
  types).
- `ucrtbase.def`: the import list, `LIBRARY ucrtbase.dll` and one name per
  imported function (795), then the POSIX names (99) as aliases of the
  underscore exports (`open == _open`): `llvm-dlltool` makes each a weak
  alias, so a call to `open` imports `_open`. Make the import library with
  `llvm-dlltool -m i386:x86-64 -d ucrtbase.def -l ucrtbase.lib` or
  `lld-link /lib /def:ucrtbase.def /machine:x64`.
- `ucrtbase_exports.txt`: the export names of `ucrtbase.dll` 10.0.26100.9711,
  the snapshot every import is checked against.
- `REFERENCES.md`: sources, probe results and what is unconfirmed.

`tools/windows_libcxx/check_crt_headers.py` checks that every imported
declaration names an export and that `ucrtbase.def` matches the headers
(`--write` regenerates it); `gen_math_header.py` generates `math.h`'s
declarations.

## Include order and compiler

`-nostdinc`, then clang's resource headers, then `include/`, then STXlib's
Windows API headers (`src/hosted/windows/api/include`). `stdint.h`,
`stddef.h`, `limits.h`, `float.h`, `stdarg.h`, `stdbool.h` and `stdalign.h`
are clang's own; clang's `float.h` includes `include/float.h`, which adds
`_controlfp`, `_controlfp_s`, `_statusfp`, `_clearfp` and the `_MCW_` set. The target must be x86_64-pc-windows-msvc (LLP64, 16-bit
`wchar_t`, `long double` equal to `double`); any other target is an error.

Neither `_DLL` nor `_MT` is needed or read: every import is declared
`__declspec(dllimport)` unconditionally, and nothing emits a default-library
directive. Pass no `-fms-runtime-lib` (in CMake, set
`CMAKE_MSVC_RUNTIME_LIBRARY` to an empty string), or Microsoft's
`msvcrt.lib` is named in every object.

## What the headers do not supply

Declared without an import, defined by other parts of STXlib:

- the startup shim (exit table): `atexit`, `at_quick_exit`, `_onexit`;
- the runtime: the string group (`memchr`, `memcmp`, `memcpy`, `memmove`,
  `memset`, `strchr`, `strrchr`, `strstr`, `wcschr`, `wcsrchr`, `wcsstr`),
  `_setjmp` and `longjmp` (clang passes the frame address as a second
  argument on x86_64), and `unexpected`, `set_unexpected`, `_get_unexpected`.

`terminate`, `set_terminate` and `_get_terminate` are `ucrtbase.dll`'s.

Supplied inline, because `ucrtbase.dll` exports no function of that name:
the printf and scanf families (over `__stdio_common_*`), `stdin`, `stdout`,
`stderr`, `errno`, `MB_CUR_MAX`, `_sys_nerr`, the time functions (over the
64-bit exports), `fileno`, `fdopen`, `hypotf`, `fabsf`, `frexpf`, `ldexpf`,
`modff`, every `long double` function, `wmemchr` and the other `wmem*`
functions, and `mbsinit`. Each header's opening comment says how.

## POSIX names

Since STXlib 1.0.1 the headers declare the CRT's POSIX names (Microsoft's
oldnames set: `open`, `read`, `fileno`, `stat`, `fstat`, `strdup`, `getcwd`,
`spawnv` and the rest), unless `_CRT_DECLARE_NONSTDC_NAMES` is defined as 0.
Each names the export its underscore form names; `stat`, `fstat`, `utime`,
`ftime` and `strcmpi` name `_stat64i32`, `_fstat64i32`, `_utime64`,
`_ftime64` and `_stricmp` (`POSIX_TARGETS` in `check_crt_headers.py`), the
forms the SDK maps them to with a 64-bit `time_t`. `_stati64` and
`_fstati64` are macros for `_stat64` and `_fstat64`.

The POSIX data names (`daylight`, `timezone`, `tzname`, `environ`) are not
declared: an import alias cannot stand for data. Use `_get_daylight` and the
other accessors, or the `_daylight` macros.

## Not provided

`feraiseexcept` and `feupdateenv` (no export, no user in the build),
`aligned_alloc` (the UCRT has none; use `_aligned_malloc`), the `_mbs*`
multibyte-string family, the `_l` locale variants, and `conio.h`'s
formatted console input and output (`_cprintf`, `_cscanf`).

## libc++

`tools/windows_libcxx/build_libcxx.sh` builds static libc++ 22.1.8 with
`LIBCXX_NO_VCRUNTIME=ON` against these headers; its opening comment lists
what the result leaves to STXlib's runtime and startup shim.
