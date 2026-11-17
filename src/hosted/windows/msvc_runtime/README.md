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

Not built by STXlib's own CMakeLists yet: the conformance suite
(`tests/msvc_runtime/`) builds it, configured with the GUI.Forms MSVC
toolchain file, and `tools/msvc_runtime/run_conformance.py` compares each
case with the same program linked with vcruntime.
