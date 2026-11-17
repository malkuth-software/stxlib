# setjmp's half of setjmp and longjmp: _setjmp and _setjmpex, the
# functions clang calls on x86_64-pc-windows-msvc for setjmp(buffer), with
# the caller's frame address as a second argument (clang/lib/CodeGen/
# CGBuiltin.cpp, EmitMSVCRTSetJmp: "_setjmp" on x86-64, with
# llvm.frameaddress(0)), and the register restore longjmp ends with
# (jump.cpp). Microsoft's runtime has no export by these names; its static
# objects supply them.
#
# int _setjmp(JumpBuffer* buffer, void* frame)   RCX, RDX
# Saves the caller's nonvolatile state (Microsoft Learn, "x64 calling
# convention", "Callee/caller saved registers": RBX, RBP, RDI, RSI, RSP,
# R12-R15, XMM6-XMM15, and the MXCSR and x87 control word, which the
# convention says a callee preserves) and returns 0. The buffer layout is
# jump.cpp's JumpBuffer; jmp_buf in STXlib's setjmp.h is 256 bytes, 16-byte
# aligned, to hold it. A leaf that never moves RSP needs no unwind entry.

    .text

    .globl  _setjmp
    .def    _setjmp
    .scl    2
    .type   32
    .endef
    .globl  _setjmpex
    .def    _setjmpex
    .scl    2
    .type   32
    .endef
    .p2align 4
_setjmp:
_setjmpex:
    movq    %rdx, 0(%rcx)           # Frame: the frame longjmp unwinds to
    movq    %rbx, 8(%rcx)
    leaq    8(%rsp), %rax
    movq    %rax, 16(%rcx)          # RSP after this call returns
    movq    %rbp, 24(%rcx)
    movq    %rsi, 32(%rcx)
    movq    %rdi, 40(%rcx)
    movq    %r12, 48(%rcx)
    movq    %r13, 56(%rcx)
    movq    %r14, 64(%rcx)
    movq    %r15, 72(%rcx)
    movq    (%rsp), %rax
    movq    %rax, 80(%rcx)          # RIP: the return address
    stmxcsr 88(%rcx)
    fnstcw  92(%rcx)
    movdqu  %xmm6, 96(%rcx)
    movdqu  %xmm7, 112(%rcx)
    movdqu  %xmm8, 128(%rcx)
    movdqu  %xmm9, 144(%rcx)
    movdqu  %xmm10, 160(%rcx)
    movdqu  %xmm11, 176(%rcx)
    movdqu  %xmm12, 192(%rcx)
    movdqu  %xmm13, 208(%rcx)
    movdqu  %xmm14, 224(%rcx)
    movdqu  %xmm15, 240(%rcx)
    xorl    %eax, %eax
    retq

# void stxlib_msvc_longjmp_restore(const JumpBuffer* buffer, int value)
# Reloads the saved state and resumes after the _setjmp call with VALUE
# (never 0; jump.cpp has made it 1) in EAX. Used when no frame was recorded,
# so no frame between is unwound.
    .globl  stxlib_msvc_longjmp_restore
    .def    stxlib_msvc_longjmp_restore
    .scl    2
    .type   32
    .endef
    .p2align 4
stxlib_msvc_longjmp_restore:
    movl    %edx, %eax
    movq    8(%rcx), %rbx
    movq    24(%rcx), %rbp
    movq    32(%rcx), %rsi
    movq    40(%rcx), %rdi
    movq    48(%rcx), %r12
    movq    56(%rcx), %r13
    movq    64(%rcx), %r14
    movq    72(%rcx), %r15
    ldmxcsr 88(%rcx)
    fnclex
    fldcw   92(%rcx)
    movdqu  96(%rcx), %xmm6
    movdqu  112(%rcx), %xmm7
    movdqu  128(%rcx), %xmm8
    movdqu  144(%rcx), %xmm9
    movdqu  160(%rcx), %xmm10
    movdqu  176(%rcx), %xmm11
    movdqu  192(%rcx), %xmm12
    movdqu  208(%rcx), %xmm13
    movdqu  224(%rcx), %xmm14
    movdqu  240(%rcx), %xmm15
    movq    16(%rcx), %rsp
    jmpq    *80(%rcx)
