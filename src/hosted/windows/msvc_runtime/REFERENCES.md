# MSVC C++ runtime: references

The clean-room record for `src/hosted/windows/msvc_runtime/` and its conformance
suite (`tests/msvc_runtime/`, `tools/msvc_runtime/`). The runtime was written
without reading Microsoft's ehdata or vcruntime headers or sources, Wine's or
ReactOS's code. Wine and ReactOS bug lists may serve as test ideas only; none
was used in this round. Microsoft's vcruntime was used only as a binary
oracle: the conformance programs run against it and against this runtime,
and their outputs are compared.

Every source consulted, with the date it was read:

## LLVM 22.1.8 (Apache-2.0 with LLVM exceptions), tag llvmorg-22.1.8

Read 2026-11-16 from the release source archive
(`llvm-project-22.1.8.src.tar.xz`):

| File | What it gave |
| --- | --- |
| `llvm/lib/CodeGen/AsmPrinter/WinException.cpp` | FuncInfo, unwind map, try map, handler type, ip-to-state layouts (`emitCXXFrameHandler3Table`); the x64 UnwindHelp field; ParentFrameOffset; image-relative references; funclet entry and state numbering |
| `clang/lib/CodeGen/MicrosoftCXXABI.cpp` | ThrowInfo, CatchableTypeArray, CatchableType, TypeDescriptor and their flags; RTTI CompleteObjectLocator, ClassHierarchyDescriptor, BaseClassDescriptor and attribute bits; `__RTDynamicCast`, `__RTtypeid`, `__RTCastToVoid` signatures; the type_info vtable symbol; thread-safe statics (`_Init_thread_header`, `_footer`, `_abort`, `_Init_thread_epoch`, the N2325 algorithm) |
| `llvm/lib/CodeGen/WinEHPrepare.cpp` | state numbering of try, catch and cleanup regions; catch-all versus typed handlers |
| `llvm/include/llvm/CodeGen/WinEHFuncInfo.h` | the state tables' fields |
| `llvm/lib/Target/X86/X86FrameLowering.cpp` | funclet prologues (RDX homing, establisher frame), UnwindHelp initialisation to -2, `__chkstk` probe calls |
| `llvm/lib/Target/X86/X86MCInstLower.cpp` | the nop after calls in functions with Windows EH |
| `llvm/lib/Target/X86/X86ISelLowering.cpp` | catchret lowering (continuation address in RAX) |
| `libcxx/` (whole library, read for the ABI it expects) | `_LIBCPP_NO_VCRUNTIME`; std::type_info layout; `src/support/runtime/exception_msvc.ipp` and `exception_pointer_msvc.ipp` (the `__ExceptionPtr*` declarations, `__uncaught_exceptions`, `set_unexpected`, `_get_unexpected`); `src/new.cpp` and `src/new_handler.cpp` (operator new left to the platform under the vcruntime ABI); `src/CMakeLists.txt` (`LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS`) |

## Microsoft Learn (public documentation)

Read 2026-11-16:

- x64 exception handling: <https://learn.microsoft.com/cpp/build/exception-handling-x64>
  (RUNTIME_FUNCTION, UNWIND_INFO, UNW_FLAG_EHANDLER and UNW_FLAG_UHANDLER,
  language handlers, DISPATCHER_CONTEXT, leaf functions)
- x64 calling convention: <https://learn.microsoft.com/cpp/build/x64-calling-convention>
- x64 prolog and epilog: <https://learn.microsoft.com/cpp/build/prolog-and-epilog> (`__chkstk`)
- RaiseException: <https://learn.microsoft.com/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception>
- RtlUnwindEx: <https://learn.microsoft.com/windows/win32/api/winnt/nf-winnt-rtlunwindex>
- RtlPcToFileHeader: <https://learn.microsoft.com/windows/win32/api/winnt/nf-winnt-rtlpctofileheader>
- SetUnhandledExceptionFilter: <https://learn.microsoft.com/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter>
- Thread local storage, TlsAlloc, TlsGetValue, TlsSetValue: <https://learn.microsoft.com/windows/win32/procthread/thread-local-storage>
- Slim reader/writer locks and condition variables: <https://learn.microsoft.com/windows/win32/sync/slim-reader-writer--srw--locks>,
  <https://learn.microsoft.com/windows/win32/sync/condition-variables>
- __fastfail: <https://learn.microsoft.com/cpp/intrinsics/fastfail>
- terminate, set_terminate, _get_terminate (UCRT): <https://learn.microsoft.com/cpp/c-runtime-library/reference/set-terminate-crt>
- _initterm, _initterm_e: <https://learn.microsoft.com/cpp/c-runtime-library/initterm-initterm-e>
- CRT initialization: <https://learn.microsoft.com/cpp/c-runtime-library/crt-initialization>
- _crt_atexit, _register_onexit_function, _initialize_onexit_table, _execute_onexit_table:
  <https://learn.microsoft.com/cpp/c-runtime-library/reference/crt-atexit-crt-at-quick-exit>,
  <https://learn.microsoft.com/cpp/c-runtime-library/reference/execute-onexit-table-initialize-onexit-table-register-onexit-function>
- _aligned_malloc, _aligned_free: <https://learn.microsoft.com/cpp/c-runtime-library/reference/aligned-malloc>
- PE format (TLS section, TLS callbacks, TLS directory): <https://learn.microsoft.com/windows/win32/debug/pe-format>

## Windows SDK public headers (10.0.28000)

Read 2026-11-16 for declarations only: `winnt.h` (EXCEPTION_RECORD,
CONTEXT, DISPATCHER_CONTEXT, RUNTIME_FUNCTION, EXCEPTION_UNWINDING and the
other ExceptionFlags bits, STATUS_UNWIND_CONSOLIDATE, NT_TIB,
IMAGE_TLS_DIRECTORY64), `errhandlingapi.h`, the UCRT's `corecrt_startup.h`
and `corecrt_terminate.h` (declarations of `_initterm`, the onexit table,
`terminate`, `set_terminate`). The tools' `ehdata*.h`, `vcruntime*.h` and the
vcruntime sources were not opened.

## Standards and papers

- ISO C++ ([except], [except.handle], [except.terminate], [new.delete],
  [replacement.functions], [expr.dynamic.cast], [expr.typeid]), working draft.
- WG21 N2325, "Dynamic Initialization and Destruction with Concurrency"
  (the epoch algorithm, as clang's comment names it).

## Binary observation (no source read)

2026-11-16: `llvm-nm` over the libc++ archives built for this target (which
symbols libc++ leaves undefined, and their decorated names); `llvm-readobj
--coff-imports` over the conformance programs; the export list of
`vcruntime140.dll` (names only, `llvm-readobj --coff-exports`), to know which
functions a replacement has to supply.

## General knowledge, to be confirmed by the conformance suite

- STATUS_UNWIND_CONSOLIDATE's dispatch: RtlUnwindEx with that code calls
  ExceptionInformation[0] on the target frame's stack with the exception
  record and continues at the address it returns. Confirmed when the catch
  cases match the oracle.
- The x64 magic numbers 0x19930520 (exception parameters) and 0x19930522
  (FuncInfo) appear in LLVM's emitter; the exception code 0xE06D7363 likewise
  (`MicrosoftCXXABI.cpp`, `WinException.cpp`).
- The Old New Thing, "Decoding the parameters of a thrown C++ exception
  (0xE06D7363)", 2010-07-30:
  <https://devblogs.microsoft.com/oldnewthing/20100730-00/?p=13273>, read
  2026-11-16 (the four exception parameters on x64).
