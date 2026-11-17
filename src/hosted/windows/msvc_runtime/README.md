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
| Per module: memcpy and the vcruntime string group, operator new and delete, `__chkstk`, thread-safe statics | `strings.cpp`, `new_delete.cpp`, `chkstk_x64.S`, `thread_statics.cpp` |
| Per image without Microsoft's startup objects: TLS directory, `_fltused`; the DLL entry and atexit | `image_support.c`, `dll_entry.cpp` |

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
  configured with the GUI.Forms MSVC toolchain file;
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
| vcruntime (`vcruntime140.dll` / `libvcruntime.lib`: exception throwing and `__CxxFrameHandler3`, RTTI, terminate, `exception_ptr`'s functions, memcpy and the string group) | this runtime (stage 2) | never compiled; only the suite's vcruntime side has |
| the STL's runtime (`msvcp140.dll`, for `exception_ptr`) | libc++ 22 with `LIBCXX_NO_VCRUNTIME` plus this runtime | as above |

The UCRT stays: it is the one host C library on this target, and STXlib
sideloads alongside it as it does alongside glibc or musl. This is not the
mingw-w64 CRT: MinGW builds keep mingw-w64's startup objects and its C
runtime import libraries, and are untouched by either part.

## External pieces, and where each comes from

Owner direction (2026-11-16, through the coordinator): the pathway takes
what it needs from the UCRT or from clang and does not use the Windows SDK,
so that it is free of the GPL and of any EULA. Today both stages still lean
on the SDK and on the Visual C++ tools:

| Piece | Used by | Comes from |
| --- | --- | --- |
| `windows.h` (`um`, `shared`: winnt.h's EXCEPTION_RECORD, CONTEXT, DISPATCHER_CONTEXT, RUNTIME_FUNCTION, IMAGE_TLS_DIRECTORY64, IMAGE_LOAD_CONFIG_DIRECTORY64; kernel32 declarations) | 17 files of both stages and both suites, and `minmallocpp_os_windows.cpp`'s MSVC form | Windows SDK |
| `corecrt_startup.h`, `stdlib.h`, `stdio.h`, `malloc.h`, `process.h`, `eh.h` (UCRT; they include `vcruntime.h`) | startup shim, `new_delete.cpp`, the probes and cases | Windows SDK (`ucrt`) and the Visual C++ tools (`vcruntime.h`, `eh.h`) |
| `limits.h`, `stdint.h`, `stddef.h` | runtime and shim | clang's resource headers, which `#include_next` the tools' and the UCRT's copies when hosted |
| libc++ 22's headers (`<atomic>`, `<new>`, `<exception>`, `<typeinfo>`, `<stdexcept>`) | runtime hooks, cases | LLVM; they include the UCRT's and the tools' C headers on this target |
| `kernel32.lib` (TlsAlloc/GetValue/SetValue/Free, RaiseException, RtlUnwindEx, RtlPcToFileHeader, SetUnhandledExceptionFilter, SRW locks and condition variables, QueryPerformanceCounter, GetCurrentProcessId/ThreadId, GetStartupInfoW, GetModuleHandleW, CreateThread, WaitForSingleObject, CloseHandle, FreeLibrary, ExitProcess) | both stages | Windows SDK (`um/x64`), found by lld-link through the toolchain file's SDK root |
| `ucrt.lib` (`_initterm`, `_initterm_e`, the onexit table functions, `_crt_atexit`, `_crt_at_quick_exit`, argv and environment configuration, `_set_app_type`, `_register_thread_local_exe_atexit_callback`, `malloc`, `free`, `_aligned_malloc`, `_aligned_free`, `_callnewh`, `terminate`, `abort`, `exit`, `printf`) | both stages | Windows SDK (`ucrt/x64`); the DLL itself (`ucrtbase.dll`) is part of Windows |
| `vcruntime.lib` | the startup probes (stage 1 keeps vcruntime for exceptions) | Visual C++ tools |
| vcruntime140.dll, msvcp140.dll, msvcrt.lib, libcmt.lib | the conformance suite's oracle side only (a binary oracle) | Visual C++ tools |
| vswhere.exe and the SDK registry key | GUI.Forms' toolchain file `cmake/llvm22-windows-msvc.cmake` (on the Windows side's branch) | Visual Studio installer, SDK installer |
| clang, lld-link, llvm-ar, llvm-lib, llvm-rc, llvm-mt, llvm-readobj, llvm-nm | build and inspection | LLVM 22.1.8 (Apache-2.0 with LLVM exceptions) |
| `/guard:cf` symbols (`__guard_*`), `_load_config_used`, `__security_check_cookie`, `__report_gsfailure` | startup shim | defined by the shim itself; lld-link fills the guard tables |

No mingw-w64 piece is used: no header, import library, startup object or
tool. rc.exe and mt.exe are not used (llvm-rc and llvm-mt are).

Proposed first-party replacement (opt-in, behind the same option):

1. **Declarations.** A private header set under
   `src/hosted/windows/msvc_abi/`: the structures above, written from
   Microsoft Learn's documented layouts and checked by `static_assert` on
   sizes and offsets, and the ~35 kernel32/ntdll and ~30 UCRT entry points
   declared with their documented types (`__declspec(dllimport)`,
   `__stdcall` is the x64 default). The runtime and shim include it in place
   of `windows.h`, `corecrt_startup.h` and the other C headers, and build
   with `-nostdinc` plus clang's resource directory (`-ffreestanding` so
   `stdint.h` and `limits.h` stop at clang's own).
2. **Import libraries.** `.def` files for `kernel32.dll` and `ucrtbase.dll`
   listing only the names used, written from the system DLLs' export tables
   (`llvm-readobj --coff-exports C:\Windows\System32\kernel32.dll`; names
   only, as REFERENCES.md already records for vcruntime140.dll), turned
   into `kernel32.lib` and `ucrtbase.lib` at build time by
   `llvm-dlltool -m i386:x86-64 -d <def> -l <lib>`. Importing `ucrtbase.dll`
   directly rather than the `api-ms-win-crt-*` sets is supported from
   Windows 10, the floor GUI.Forms already has.
3. **Toolchain file.** An SDK-free variant that passes no
   `-Xmicrosoft-windows-sdk-root` or tools root, links with `/NODEFAULTLIB`
   and `-fms-omit-default-lib`, and adds the generated import libraries.
4. **The open part: libc++.** libc++'s headers include a C library's
   headers. On this target those are the UCRT's (SDK) and `vcruntime.h`
   (Visual C++ tools). Freeing the C++ library needs a C header set for it:
   our own UCRT-compatible declarations (as in 1, but the whole surface
   libc++ includes), or LLVM's libc headers. This is the largest item and
   an owner choice.
5. The stage-1 probes link this runtime instead of `vcruntime.lib`, and the
   oracle side of the conformance suite stays the one place that links
   Microsoft's runtime, as a test oracle only (or is run only where the
   tools are already installed).

## Before it can be the default

1. Compile the runtime (never done) and run the 17 conformance cases,
   static and shared, against the vcruntime oracle, all matching
   (`run_conformance.py`).
2. Run the startup probes (`tests/windows_msvc_startup/`, six CTest
   entries: start-up, exit order, wide entry, window entry, `/GS` overrun,
   uncaught exception).
3. Decide which per-image set an image links. Both parts supply operator new
   and delete, `__chkstk`, thread-safe statics, the TLS directory and a DLL
   entry with its atexit table (`new_delete.cpp`, `chkstk_x64.S`,
   `thread_statics.cpp`, `image_support.c`, `dll_entry.cpp` here; the shim's
   `new_delete.cpp`, `chkstk.cpp`, `thread_safe_statics.cpp`, `tls.cpp`,
   `entry_dll.cpp`). The runtime's set was written "until the startup shim
   supplies them"; the shim's probes then link against this runtime in place
   of `vcruntime.lib`, and the duplicates are removed.
4. Build STXlib's own suite and GUI.Forms (its C ABI DLL included) on the
   MSVC target with both parts and no Microsoft C++ runtime library, under
   the sanitizers that target has.
5. Proof entries: each module in scope gets its proof report entry (proved,
   assumed or not yet proved), as AGENTS.md requires; the frame handler's
   table walk and the N2325 guard protocol are the first candidates.
6. Performance against vcruntime within the twice-as-slow ceiling (throw and
   catch, dynamic_cast, guarded statics).
7. The SDK-free pathway above (declarations, generated import libraries,
   the toolchain variant, and a C header set for libc++).
8. The owner's choice to make it the default for the MSVC target.
