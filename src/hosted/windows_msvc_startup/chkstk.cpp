// __chkstk, the stack probe.
//
// Learn "x64 prolog and epilog": a function whose fixed allocation is 4 KiB
// or more (the guard page's size) calls __chkstk with the allocation's size
// in rax before subtracting it from rsp, so that the guard page below the
// committed stack is touched page by page and the stack grows; touching a
// page beyond the guard page instead raises an access violation. The probe
// must preserve every register except r10, r11 and the flags (the callers
// clang emits keep their arguments in rcx, rdx, r8 and r9 across the call)
// and leaves rsp unchanged.
//
// This one touches one byte in each 4 KiB page from the caller's stack
// pointer down to the new lowest address, in descending order, with a read
// (a read of the guard page is enough to commit it). It saves the two
// registers it uses and describes the pushes with SEH unwind directives,
// so that a stack overflow inside it unwinds correctly.
//
// On entry: [rsp] is the return address, rax the allocation in bytes. The
// caller's stack pointer before the call is rsp + 8; after the two pushes
// it is rsp + 24.

__asm__(
    "\t.text\n"
    "\t.globl __chkstk\n"
    "\t.p2align 4\n"
    "\t.def __chkstk; .scl 2; .type 32; .endef\n"
    "\t.seh_proc __chkstk\n"
    "__chkstk:\n"
    "\tpushq %rcx\n"
    "\t.seh_pushreg %rcx\n"
    "\tpushq %rax\n"
    "\t.seh_pushreg %rax\n"
    "\t.seh_endprologue\n"
    "\tleaq 24(%rsp), %rcx\n"
    "1:\n"
    "\tcmpq $0x1000, %rax\n"
    "\tjb 2f\n"
    "\tsubq $0x1000, %rcx\n"
    "\ttestq %rcx, (%rcx)\n"
    "\tsubq $0x1000, %rax\n"
    "\tjmp 1b\n"
    "2:\n"
    "\tsubq %rax, %rcx\n"
    "\ttestq %rcx, (%rcx)\n"
    "\tpopq %rax\n"
    "\tpopq %rcx\n"
    "\tret\n"
    "\t.seh_endproc\n");
