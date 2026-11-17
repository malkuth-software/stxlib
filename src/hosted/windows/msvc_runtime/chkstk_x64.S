# __chkstk, per module: clang calls it from the prologue of every function
# whose frame exceeds a page (LLVM's X86FrameLowering.cpp, emitStackProbeCall
# on a Windows MSVC target), with the frame's size in RAX, before the
# prologue moves RSP. It touches each page between the caller's stack
# pointer and the new frame's end, in order, so the stack grows one guard
# page at a time (Microsoft Learn, "x64 prolog and epilog": "__chkstk",
# and "Thread Stack Size"). It moves no register the caller depends on:
# RAX, RSP and every nonvolatile register are kept; R10, R11 and the flags
# are clobbered.
#
# The stack's current low end is the thread's NT_TIB StackLimit, at GS:0x10
# on x64 (the Windows SDK's winnt.h, NT_TIB: ExceptionList, StackBase,
# StackLimit; the TEB starts with it and GS addresses the TEB). Pages above
# it are committed already and are not touched.
#
# A leaf that never moves RSP needs no unwind entry (Microsoft Learn, "x64
# exception handling": leaf functions).

    .text
    .globl  __chkstk
    .def    __chkstk
    .scl    2
    .type   32
    .endef
    .p2align 4
__chkstk:
    leaq    8(%rsp), %r10           # the caller's RSP, before its call
    subq    %rax, %r10              # the lowest address the frame reaches
    jae     1f
    xorl    %r10d, %r10d            # a size beyond the address space
1:
    movq    %gs:16, %r11            # NT_TIB.StackLimit
    cmpq    %r11, %r10
    jae     3f                      # within committed pages: nothing to do
    andq    $-4096, %r10            # the page holding the lowest address
2:
    subq    $4096, %r11
    testb   $0, (%r11)              # a read: commits the guard page below
    cmpq    %r10, %r11
    ja      2b
3:
    retq
