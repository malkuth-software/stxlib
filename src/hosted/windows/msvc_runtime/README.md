# STXlib's MSVC C++ runtime (x86_64-pc-windows-msvc)

STXlib's own clean-room replacement for Microsoft's vcruntime on the MSVC
target, compiled by the pinned clang 22 (never the MSVC compiler). STXlib
hosts what we do not trust the host with (owner, 2026-11-16); this is the one
place in STXlib that implements exception machinery, and its own code stays
exception-free except the two files that construct and throw the C++
library's types (`cxx_hooks.cpp`, `new_delete.cpp`). The sources consulted are
in [REFERENCES.md](REFERENCES.md).

x64 only. What it supplies:

| Part | Files |
| --- | --- |
| Throwing: `_CxxThrowException`, `throw;` with per-thread in-flight records, `__uncaught_exceptions` | `throw.cpp`, `runtime_state.cpp` |
| `__CxxFrameHandler3`: two-phase search and unwind over clang's FuncInfo tables (magic 0x19930522), catch matching with bases, virtual-base displacements and qualifier rules, catch objects, catch funclets through STATUS_UNWIND_CONSOLIDATE, cleanup funclets, terminate on a throw out of a cleanup and on noexcept | `frame_handler.cpp`, `thunks_x64.S` |
| RTTI: `__RTtypeid`, `__RTDynamicCast`, `__RTCastToVoid`; the type_info vtable | `rtti.cpp`, `module_static.cpp` |
| `std::exception_ptr`'s `__ExceptionPtr*` functions (libc++ calls them on this ABI) | `exception_ptr.cpp` |
| `__std_terminate`, `_purecall`, `set_unexpected`, `_get_unexpected`, `unexpected`, the unhandled-exception filter | `terminate.cpp` |
| Throwing std::bad_alloc, bad_cast, bad_typeid | `cxx_hooks.cpp` |
| Per module: memcpy and the vcruntime string group, operator new and delete (one archive member per function, so a program may replace some of them) | `strings.cpp`, `new_delete.cpp` |
| `__C_specific_handler` (`__try`/`__except`/`__finally`), `_setjmp`, `_setjmpex`, `longjmp` (unwinding) | `structured_handler.cpp`, `setjmp_x64.S`, `jump.cpp` |
| The one include for the C library and the Windows API, shared with the startup shim: STXlib's own headers (`src/hosted/windows/crt/include`, `src/hosted/windows/api/include`) plus the three names they lack (WinMain, wWinMain, `__fastfail`), so both build with `-nostdinc` | `system_interim.h` |

The per-image pieces belong to the startup shim (`src/hosted/windows_msvc_startup/`), by the MSVC target's per-symbol owner table (2026-11-17): `__chkstk`, `_fltused`, thread-safe statics, TLS, the image entry points and the exit tables. The shared form's DLL takes its entry (`_DllMainCRTStartup`) from the shim, so `stxlib_msvc_runtime` is defined only where the `stx_msvc_startup` target exists.

Two forms, as vcruntime has: static (every file above linked into the
image) and shared (`stxlib_msvc_runtime.dll`, exports in
`stxlib_msvc_runtime.def`, plus the per-module library in each image). A
process whose exceptions cross DLL boundaries needs the shared form: one
uncaught count and one chain of active catches per thread for all images.

The C++ library is LLVM's libc++ 22 built with `LIBCXX_NO_VCRUNTIME=ON`, so
std::exception, type_info and the standard exception classes come from
libc++ and this runtime supplies only the ABI functions.

## Building

The targets are in `CMakeLists.txt` beside this file. Two builds add it:

- STXlib's root build with `-DSTXLIB_MSVC_RUNTIME=ON` (OFF by default, and
  refused on every target but x86_64-pc-windows-msvc), together with the
  startup shim (`src/hosted/windows_msvc_startup/`, whose probes follow
  `STXLIB_TESTS`);
- the conformance suite (`tests/msvc_runtime/`, its "ours" side),
  configured with the GUI.Forms MSVC toolchain file in its own-runtime
  mode (`-DGUI_FORMS_MSVC_RUNTIME=OWN`, `GUI_FORMS_OWN_RUNTIME_SOURCE` an
  STXlib source holding the crt and api headers, `GUI_FORMS_LIBCXX_PREFIX`
  a libc++ built with `_LIBCPP_NO_VCRUNTIME` against them). It adds the
  startup shim and this runtime as subprojects and makes `ucrtbase.lib`
  and `kernel32.lib` from `crt/ucrtbase.def` and `api/lib/kernel32.def`
  with `llvm-dlltool`; no Windows SDK, Visual C++ or MinGW folder is read;
  `tools/msvc_runtime/run_conformance.py` compares each case with the same
  program linked with vcruntime (the "oracle" side).

Nothing in the default build compiles these files, so MinGW (MSYS2
CLANG64) and every other host are unchanged. The two MSVC-target edits
outside these directories (`src/hosted/windows/minmallocpp_os_windows.cpp`'s
TLS callback and `src/hosted/cxx_runtime/current_exception.cpp`) are under
`_MSC_VER`, which MinGW's clang does not define.

## What the two parts replace

On the MSVC target an image built by clang otherwise links three Microsoft
pieces besides the UCRT:

| Microsoft piece | Replaced by | State |
| --- | --- | --- |
| vcstartup, the static startup objects in `msvcrt.lib` / `libcmt.lib` (entry points, `.CRT$X*` initialiser tables, TLS directory and dynamic TLS, thread-safe statics, `/GS` cookie, load configuration, `__chkstk`, `_fltused`, atexit) | the startup shim, `src/hosted/windows_msvc_startup/` (stage 1) | compiles; its probes have not run |
| vcruntime (`vcruntime140.dll` / `libvcruntime.lib`: exception throwing and `__CxxFrameHandler3`, RTTI, terminate, `exception_ptr`'s functions, memcpy and the string group) | this runtime (stage 2) | compiles against STXlib's headers; the suite has not yet linked |
| the STL's runtime (`msvcp140.dll`, for `exception_ptr`) | libc++ 22 with `LIBCXX_NO_VCRUNTIME` plus this runtime | as above |

The UCRT stays: it is the one host C library on this target, and STXlib
sideloads alongside it as it does alongside glibc or musl. This is not the
mingw-w64 CRT: MinGW builds keep mingw-w64's startup objects and its C
runtime import libraries, and are untouched by either part.

## External pieces, and where each comes from

Owner direction (2026-11-16 and the no-MinGW switch, 2026-11-17): the
pathway takes what it needs from the UCRT or from clang and uses no
Windows SDK, Visual C++ or MinGW header or library, so that it is free of
the GPL and of any EULA.

| Piece | Used by | Comes from |
| --- | --- | --- |
| C library headers (`corecrt_startup.h`, `stdlib.h`, `malloc.h`, `process.h`, `eh.h` and the rest libc++ includes) | runtime, startup shim, cases | STXlib, `src/hosted/windows/crt/include` |
| Windows API headers (`windows.h`: EXCEPTION_RECORD, CONTEXT, DISPATCHER_CONTEXT, RUNTIME_FUNCTION, the kernel32 declarations) | runtime, startup shim, cases | STXlib, `src/hosted/windows/api/include` |
| `stddef.h`, `stdint.h`, `limits.h`, the intrinsics | runtime and shim | clang's resource headers |
| libc++ 22 (headers and static archives, `_LIBCPP_NO_VCRUNTIME`) | runtime hooks, cases | LLVM, built against STXlib's headers (`tools/windows_libcxx`) |
| `ucrtbase.lib`, `kernel32.lib` | every image | made at build time from STXlib's export lists by `llvm-dlltool`; the DLLs are part of Windows |
| vcruntime140.dll, msvcp140.dll, msvcrt.lib, the SDK | the conformance suite's oracle side only (a binary oracle) | Visual C++ tools and the Windows SDK |
| clang, lld-link, llvm-ar, llvm-dlltool, llvm-readobj, llvm-nm | build and inspection | LLVM 22.1.8 (Apache-2.0 with LLVM exceptions) |
| `/guard:cf` symbols (`__guard_*`), `_load_config_used`, `__security_check_cookie`, `__report_gsfailure` | startup shim | defined by the shim itself; lld-link fills the guard tables |

The UCRT stays: it is the one host C library on this target, and STXlib
sideloads alongside it as it does alongside glibc or musl. No mingw-w64
piece is used: no header, import library, startup object or tool.

## Before it can be the default

1. Run the 18 conformance cases, static and shared, against the vcruntime
   oracle, all matching (`run_conformance.py`). State 2026-11-17: every
   file compiles against STXlib's headers (single-file probes and the
   suite's own-runtime tree); the first link found the `__ExceptionPtr*`
   definitions decorated with top-level const (fixed, names checked against
   libc++'s references); the suite has not yet linked and run.
2. Run the startup probes (`tests/windows_msvc_startup/`, six CTest
   entries: start-up, exit order, wide entry, window entry, `/GS` overrun,
   uncaught exception).
3. Done (2026-11-17): the startup shim owns the per-image set, by the
   MSVC target's owner table; the runtime's duplicates were removed.
4. Build STXlib's own suite and GUI.Forms (its C ABI DLL included) on the
   MSVC target with both parts and no Microsoft C++ runtime library, under
   the sanitizers that target has.
5. Proof entries: each module in scope gets its proof report entry (proved,
   assumed or not yet proved), as AGENTS.md requires; the frame handler's
   table walk and the N2325 guard protocol are the first candidates.
6. Performance against vcruntime within the twice-as-slow ceiling (throw and
   catch, dynamic_cast, guarded statics).
7. Done in part (2026-11-17): the SDK-free pathway (STXlib's C library and
   Windows API headers, import libraries made from export lists, the
   toolchain's own-runtime mode, libc++ built against them); it holds once
   item 1 runs in that mode.
8. The owner's choice to make it the default for the MSVC target.
