# STXlib's MSVC C++ runtime: the two thunks through which the runtime calls
# foreign code while it handles an exception (frame_handler.cpp). Each has
# its own unwind entry and a language handler, so an exception passing
# through the thunk's frame reaches the runtime's guard.
#
# Win64 calling convention and unwind data: Microsoft Learn, "x64 calling
# convention" (the 32-byte home area the caller reserves, RCX, RDX, R8, R9)
# and "x64 exception handling" (UNWIND_INFO, UNW_FLAG_EHANDLER and
# UNW_FLAG_UHANDLER, which .seh_handler's @except and @unwind set). Each call
# is followed by a nop so the return address lies inside the body, not at
# the epilogue, where no handler is called (LLVM's X86MCInstLower.cpp,
# maybe EmitNopAfterCallForWindowsEH, gives the same rule).

    .text

# void stxlib_msvc_call_guarded(target, first, second, third):
# calls target(first, second, third) under stxlib_msvc_terminate_guard.
    .globl  stxlib_msvc_call_guarded
    .def    stxlib_msvc_call_guarded
    .scl    2
    .type   32
    .endef
    .p2align 4
stxlib_msvc_call_guarded:
.seh_proc stxlib_msvc_call_guarded
    .seh_handler stxlib_msvc_terminate_guard, @except
    subq    $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movq    %rcx, %rax
    movq    %rdx, %rcx
    movq    %r8, %rdx
    movq    %r9, %r8
    callq   *%rax
    nop
    addq    $40, %rsp
    retq
.seh_endproc

# void* stxlib_msvc_call_catch(funclet, parent_establisher, record):
# calls the catch funclet with RDX = the parent's establisher frame and
# returns its continuation address (RAX), under stxlib_msvc_catch_guard.
# Frame (RSP after the prologue): [0, 32) the funclet's home area, which
# receives its homed RDX at +8; +32 the CatchRecord; +40 the cookie
# 0x5354584C43415443 ("STXLCATC", runtime_state.hpp); +48 padding to keep
# RSP 16-byte aligned at the call.
    .globl  stxlib_msvc_call_catch
    .def    stxlib_msvc_call_catch
    .scl    2
    .type   32
    .endef
    .p2align 4
stxlib_msvc_call_catch:
.seh_proc stxlib_msvc_call_catch
    .seh_handler stxlib_msvc_catch_guard, @except, @unwind
    subq    $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    movq    %r8, 32(%rsp)
    movabsq $0x5354584C43415443, %rax
    movq    %rax, 40(%rsp)
    movq    %rcx, %rax
    xorl    %ecx, %ecx
    callq   *%rax
    nop
    addq    $56, %rsp
    retq
.seh_endproc
