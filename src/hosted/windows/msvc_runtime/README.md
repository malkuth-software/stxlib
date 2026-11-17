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
7. The owner's choice to make it the default for the MSVC target.
