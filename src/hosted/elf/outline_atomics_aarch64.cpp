// STXlib's own outline-atomics helpers for Linux and FreeBSD aarch64
// (hosted/outline_atomics_backend.hpp says why and how detection works;
// owner, 2026-11-22, projects tracks/stxlib/own_outline_atomics.md).
//
// The contract is the toolchain's: the names, arguments, results and
// register use of compiler-rt's lib/builtins/aarch64/lse.S and libgcc's
// config/aarch64/lse.S (both written from memory of those files and not
// re-read at a pin for this change; the integrator's link check,
// tools/check_outline_atomics.py, and tests/outline_atomics_tests.cpp hold
// the result to it). Per helper:
// - __aarch64_casN_ORDER(expected, desired, address), N = 1, 2, 4, 8:
//   compares the N bytes at address with expected's low N bytes, stores
//   desired's there when equal, and returns the N bytes it read;
// - __aarch64_cas16_ORDER(expected in x0:x1, desired in x2:x3, address in
//   x4): the same on 16 bytes, returning them in x0:x1;
// - __aarch64_swpN_ORDER(value, address): stores value, returns the old;
// - __aarch64_ldOPN_ORDER(value, address), OP = add, clr (and-not), eor,
//   set (or): stores old OP value, returns the old.
// ORDER is relax, acq, rel or acq_rel: acquire on the load, release on the
// store, both, or neither, as the LSE instruction's a and l forms give it.
// Arguments narrower than a register may carry anything above their N
// bytes; the helpers read only the low N bytes (the cas loop zero-extends
// expected before comparing). Each helper may change only x0 (x0 and x1 for
// cas16), x15, x16, x17 and the condition flags: compiled code relies on
// every other register surviving, so detection, which runs inside the
// first call, saves what it uses.
//
// Visibility matches the toolchain's (hidden: each module has its own
// copies), so a module that links STXlib resolves its helper calls here and
// the toolchain's archive member, with its getauxval constructor, is never
// pulled in. Every helper is in one section and one object, between
// stxlib_outline_atomics_begin and stxlib_outline_atomics_end, which is what
// the link check looks for.
//
// One deliberate difference from compiler-rt's loop (replace, not
// replicate): when cas16's comparison fails, the loop stores the 16 bytes it
// read back with a store-exclusive and retries if that fails. A 16-byte
// load-exclusive is single-copy atomic only when a store-exclusive to the
// same place succeeds after it (Arm ARM, B2.2.1), so without the store the
// value returned on failure could be torn. LLVM's own inline expansion of a
// 16-byte compare-exchange does the same.
//
// Written as file-scope assembly in a C++ file so the build needs no
// assembler language and the file compiles to nothing elsewhere. Assembler
// macros keep each helper's shape in one place; the table at the end names
// every helper with its instructions, so each line can be checked against
// the Arm ARM by reading it.

#include "hosted/outline_atomics_backend.hpp"

#if defined(STXLIB_OUTLINE_ATOMICS)

// With branch protection (-mbranch-protection=bti or standard) every
// function that can be reached by an indirect branch starts with a landing
// pad. The helpers are called directly, but a linker's range-extension
// stub reaches them with br, so they carry one, as compiler-rt's do.
#if defined(__ARM_FEATURE_BTI_DEFAULT)
#define STXLIB_OUTLINE_ATOMICS_LANDING "    hint #34\n" // bti c
#else
#define STXLIB_OUTLINE_ATOMICS_LANDING ""
#endif

// Helper entry and end, the memo test every helper starts with, and the
// detour into detection. The memo test: bit 1 set means LSE (label 9 is the
// loop); a zero memo means not yet detected (label 7 detects, then the
// helper starts again at label 6, now with a non-zero memo). x17 keeps the
// return address across the call to detection, which preserves x17.
__asm__(".pushsection .text.stxlib_outline_atomics,\"ax\",%progbits\n"
        ".arch_extension lse\n"

        ".macro stxlib_entry name\n"
        "    .globl \\name\n"
        "    .hidden \\name\n"
        "    .type \\name, %function\n"
        "    .p2align 4\n"
        "\\name:\n" STXLIB_OUTLINE_ATOMICS_LANDING
        ".endm\n"

        ".macro stxlib_end name\n"
        "    .size \\name, . - \\name\n"
        ".endm\n"

        ".macro stxlib_memo_test\n"
        "6:  adrp x16, stxlib_outline_atomics_memo\n"
        "    ldrb w16, [x16, :lo12:stxlib_outline_atomics_memo]\n"
        "    tbz w16, #1, 9f\n"
        ".endm\n"

        ".macro stxlib_detect_and_retry\n"
        "7:  mov x17, x30\n"
        "    bl stxlib_outline_atomics_detect\n"
        "    mov x30, x17\n"
        "    b 6b\n"
        ".endm\n"

        // casN: r is the register prefix (w or x), ext zero-extends
        // expected's low N bytes into the comparison register (uxtb, uxth,
        // or mov for 4 and 8 bytes).
        ".macro stxlib_cas name, r, ext, lse, load, store\n"
        "stxlib_entry \\name\n"
        "stxlib_memo_test\n"
        "    \\lse \\r\\()0, \\r\\()1, [x2]\n"
        "    ret\n"
        "9:  cbz w16, 7f\n"
        "    \\ext \\r\\()16, \\r\\()0\n"
        "0:  \\load \\r\\()0, [x2]\n"
        "    cmp \\r\\()0, \\r\\()16\n"
        "    b.ne 1f\n"
        "    \\store w17, \\r\\()1, [x2]\n"
        "    cbnz w17, 0b\n"
        "1:  ret\n"
        "stxlib_detect_and_retry\n"
        "stxlib_end \\name\n"
        ".endm\n"

        // cas16: on a mismatch the loop stores back what it read (label 2;
        // the file's header says why).
        ".macro stxlib_cas16 name, lse, load, store\n"
        "stxlib_entry \\name\n"
        "stxlib_memo_test\n"
        "    \\lse x0, x1, x2, x3, [x4]\n"
        "    ret\n"
        "9:  cbz w16, 7f\n"
        "    mov x16, x0\n"
        "    mov x17, x1\n"
        "0:  \\load x0, x1, [x4]\n"
        "    cmp x0, x16\n"
        "    ccmp x1, x17, #0, eq\n"
        "    b.ne 2f\n"
        "    \\store w15, x2, x3, [x4]\n"
        "    cbnz w15, 0b\n"
        "    ret\n"
        "2:  \\store w15, x0, x1, [x4]\n"
        "    cbnz w15, 0b\n"
        "    ret\n"
        "stxlib_detect_and_retry\n"
        "stxlib_end \\name\n"
        ".endm\n"

        ".macro stxlib_swp name, r, lse, load, store\n"
        "stxlib_entry \\name\n"
        "stxlib_memo_test\n"
        "    \\lse \\r\\()0, \\r\\()0, [x1]\n"
        "    ret\n"
        "9:  cbz w16, 7f\n"
        "    mov \\r\\()16, \\r\\()0\n"
        "0:  \\load \\r\\()0, [x1]\n"
        "    \\store w17, \\r\\()16, [x1]\n"
        "    cbnz w17, 0b\n"
        "    ret\n"
        "stxlib_detect_and_retry\n"
        "stxlib_end \\name\n"
        ".endm\n"

        // ldOP: op is the loop's data operation (add, bic for clr, eor, orr
        // for set), computing the new value from the old (r0) and the
        // argument (r16).
        ".macro stxlib_ldop name, r, op, lse, load, store\n"
        "stxlib_entry \\name\n"
        "stxlib_memo_test\n"
        "    \\lse \\r\\()0, \\r\\()0, [x1]\n"
        "    ret\n"
        "9:  cbz w16, 7f\n"
        "    mov \\r\\()16, \\r\\()0\n"
        "0:  \\load \\r\\()0, [x1]\n"
        "    \\op \\r\\()17, \\r\\()0, \\r\\()16\n"
        "    \\store w15, \\r\\()17, [x1]\n"
        "    cbnz w15, 0b\n"
        "    ret\n"
        "stxlib_detect_and_retry\n"
        "stxlib_end \\name\n"
        ".endm\n"

        ".globl stxlib_outline_atomics_begin\n"
        ".hidden stxlib_outline_atomics_begin\n"
        ".p2align 4\n"
        "stxlib_outline_atomics_begin:\n");

// Detection: stores the module's memo, 1 (no LSE) or 3 (LSE). Called with
// bl from a helper's first call or from stxlib_outline_atomics_lse; changes
// only x16 and the flags (x17 holds the helper's return address).
#if defined(__linux__)
// Linux: HWCAP_CPUID first (outline_atomics_backend.hpp). /proc/self/auxv
// is the process's auxiliary vector as 16-byte (type, value) pairs ending
// at AT_NULL (0); AT_HWCAP is 16. The system calls are the kernel's
// generic table for arm64 (include/uapi/asm-generic/unistd.h: openat 56,
// close 57, read 63); AT_FDCWD is -100, O_CLOEXEC 0x80000 (asm-generic
// fcntl.h), and a call returns -errno in x0 and changes no other register.
// A read interrupted by a signal (-EINTR, -4) is repeated; any other
// failure or short read ends the search with what was found. These are
// raw system calls, not the C library's wrappers, so no cancellation point
// and no foreign code is reached (AGENTS.md, early exits): a helper may run
// under any lock.
__asm__(".p2align 4\n"
        ".type stxlib_outline_atomics_detect, %function\n"
        "stxlib_outline_atomics_detect:\n" STXLIB_OUTLINE_ATOMICS_LANDING
        "    sub sp, sp, #64\n"
        "    stp x0, x1, [sp]\n"
        "    stp x2, x3, [sp, #16]\n"
        "    stp x8, x15, [sp, #32]\n"
        "    mov x15, xzr\n"                // AT_HWCAP's value, 0 until found
        "    mov x0, #-100\n"               // AT_FDCWD
        "    adrp x1, stxlib_outline_atomics_auxv\n"
        "    add x1, x1, :lo12:stxlib_outline_atomics_auxv\n"
        "    mov x2, #0x80000\n"            // O_RDONLY | O_CLOEXEC
        "    mov x3, xzr\n"
        "    mov x8, #56\n"                 // openat
        "    svc #0\n"
        "    tbnz x0, #63, 3f\n"            // -errno: no vector, so no HWCAP_CPUID
        "    mov x3, x0\n"                  // the descriptor
        "1:  mov x0, x3\n"
        "    add x1, sp, #48\n"             // one (type, value) pair
        "    mov x2, #16\n"
        "    mov x8, #63\n"                 // read
        "    svc #0\n"
        "    cmn x0, #4\n"                  // -EINTR
        "    b.eq 1b\n"
        "    cmp x0, #16\n"
        "    b.ne 2f\n"
        "    ldr x16, [sp, #48]\n"          // the type
        "    cbz x16, 2f\n"                 // AT_NULL
        "    cmp x16, #16\n"                // AT_HWCAP
        "    b.ne 1b\n"
        "    ldr x15, [sp, #56]\n"          // its value
        "2:  mov x0, x3\n"
        "    mov x8, #57\n"                 // close
        "    svc #0\n"
        "3:  mov w16, #1\n"
        "    tbz x15, #11, 4f\n"            // HWCAP_CPUID clear: the read below would trap
        "    mrs x0, ID_AA64ISAR0_EL1\n"
        "    ubfx x0, x0, #20, #4\n"        // Atomic
        "    cmp x0, #2\n"
        "    b.lo 4f\n"
        "    mov w16, #3\n"
        "4:  adrp x0, stxlib_outline_atomics_memo\n"
        "    strb w16, [x0, :lo12:stxlib_outline_atomics_memo]\n"
        "    ldp x8, x15, [sp, #32]\n"
        "    ldp x2, x3, [sp, #16]\n"
        "    ldp x0, x1, [sp]\n"
        "    add sp, sp, #64\n"
        "    ret\n"
        ".size stxlib_outline_atomics_detect, . - stxlib_outline_atomics_detect\n");
#else
// FreeBSD: the kernel always emulates the read (outline_atomics_backend.hpp).
__asm__(".p2align 4\n"
        ".type stxlib_outline_atomics_detect, %function\n"
        "stxlib_outline_atomics_detect:\n" STXLIB_OUTLINE_ATOMICS_LANDING
        "    str x0, [sp, #-16]!\n"
        "    mrs x16, ID_AA64ISAR0_EL1\n"
        "    ubfx x16, x16, #20, #4\n"      // Atomic
        "    cmp x16, #2\n"
        "    mov w16, #1\n"
        "    b.lo 1f\n"
        "    mov w16, #3\n"
        "1:  adrp x0, stxlib_outline_atomics_memo\n"
        "    strb w16, [x0, :lo12:stxlib_outline_atomics_memo]\n"
        "    ldr x0, [sp], #16\n"
        "    ret\n"
        ".size stxlib_outline_atomics_detect, . - stxlib_outline_atomics_detect\n");
#endif

// stxlib_outline_atomics_lse (outline_atomics_backend.hpp): an ordinary
// function, so it may change x16, x17 and x30's copy freely.
__asm__("stxlib_entry stxlib_outline_atomics_lse\n"
        "    adrp x16, stxlib_outline_atomics_memo\n"
        "    ldrb w16, [x16, :lo12:stxlib_outline_atomics_memo]\n"
        "    cbnz w16, 1f\n"
        "    mov x17, x30\n"
        "    bl stxlib_outline_atomics_detect\n"
        "    mov x30, x17\n"
        "    adrp x16, stxlib_outline_atomics_memo\n"
        "    ldrb w16, [x16, :lo12:stxlib_outline_atomics_memo]\n"
        "1:  ubfx w0, w16, #1, #1\n"
        "    ret\n"
        "stxlib_end stxlib_outline_atomics_lse\n");

// Every helper the compilers emit: casN, cas16, swpN and ldaddN, ldclrN,
// ldeorN, ldsetN for N = 1, 2, 4, 8, each in the four orderings. Columns:
// name, register prefix, (cas: extension; ldOP: loop operation), the LSE
// instruction, the load-exclusive, the store-exclusive.
__asm__("stxlib_cas __aarch64_cas1_relax, w, uxtb, casb, ldxrb, stxrb\n"
        "stxlib_cas __aarch64_cas1_acq, w, uxtb, casab, ldaxrb, stxrb\n"
        "stxlib_cas __aarch64_cas1_rel, w, uxtb, caslb, ldxrb, stlxrb\n"
        "stxlib_cas __aarch64_cas1_acq_rel, w, uxtb, casalb, ldaxrb, stlxrb\n"
        "stxlib_cas __aarch64_cas2_relax, w, uxth, cash, ldxrh, stxrh\n"
        "stxlib_cas __aarch64_cas2_acq, w, uxth, casah, ldaxrh, stxrh\n"
        "stxlib_cas __aarch64_cas2_rel, w, uxth, caslh, ldxrh, stlxrh\n"
        "stxlib_cas __aarch64_cas2_acq_rel, w, uxth, casalh, ldaxrh, stlxrh\n"
        "stxlib_cas __aarch64_cas4_relax, w, mov, cas, ldxr, stxr\n"
        "stxlib_cas __aarch64_cas4_acq, w, mov, casa, ldaxr, stxr\n"
        "stxlib_cas __aarch64_cas4_rel, w, mov, casl, ldxr, stlxr\n"
        "stxlib_cas __aarch64_cas4_acq_rel, w, mov, casal, ldaxr, stlxr\n"
        "stxlib_cas __aarch64_cas8_relax, x, mov, cas, ldxr, stxr\n"
        "stxlib_cas __aarch64_cas8_acq, x, mov, casa, ldaxr, stxr\n"
        "stxlib_cas __aarch64_cas8_rel, x, mov, casl, ldxr, stlxr\n"
        "stxlib_cas __aarch64_cas8_acq_rel, x, mov, casal, ldaxr, stlxr\n"
        "stxlib_cas16 __aarch64_cas16_relax, casp, ldxp, stxp\n"
        "stxlib_cas16 __aarch64_cas16_acq, caspa, ldaxp, stxp\n"
        "stxlib_cas16 __aarch64_cas16_rel, caspl, ldxp, stlxp\n"
        "stxlib_cas16 __aarch64_cas16_acq_rel, caspal, ldaxp, stlxp\n"
        "stxlib_swp __aarch64_swp1_relax, w, swpb, ldxrb, stxrb\n"
        "stxlib_swp __aarch64_swp1_acq, w, swpab, ldaxrb, stxrb\n"
        "stxlib_swp __aarch64_swp1_rel, w, swplb, ldxrb, stlxrb\n"
        "stxlib_swp __aarch64_swp1_acq_rel, w, swpalb, ldaxrb, stlxrb\n"
        "stxlib_swp __aarch64_swp2_relax, w, swph, ldxrh, stxrh\n"
        "stxlib_swp __aarch64_swp2_acq, w, swpah, ldaxrh, stxrh\n"
        "stxlib_swp __aarch64_swp2_rel, w, swplh, ldxrh, stlxrh\n"
        "stxlib_swp __aarch64_swp2_acq_rel, w, swpalh, ldaxrh, stlxrh\n"
        "stxlib_swp __aarch64_swp4_relax, w, swp, ldxr, stxr\n"
        "stxlib_swp __aarch64_swp4_acq, w, swpa, ldaxr, stxr\n"
        "stxlib_swp __aarch64_swp4_rel, w, swpl, ldxr, stlxr\n"
        "stxlib_swp __aarch64_swp4_acq_rel, w, swpal, ldaxr, stlxr\n"
        "stxlib_swp __aarch64_swp8_relax, x, swp, ldxr, stxr\n"
        "stxlib_swp __aarch64_swp8_acq, x, swpa, ldaxr, stxr\n"
        "stxlib_swp __aarch64_swp8_rel, x, swpl, ldxr, stlxr\n"
        "stxlib_swp __aarch64_swp8_acq_rel, x, swpal, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldadd1_relax, w, add, ldaddb, ldxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldadd1_acq, w, add, ldaddab, ldaxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldadd1_rel, w, add, ldaddlb, ldxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldadd1_acq_rel, w, add, ldaddalb, ldaxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldadd2_relax, w, add, ldaddh, ldxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldadd2_acq, w, add, ldaddah, ldaxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldadd2_rel, w, add, ldaddlh, ldxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldadd2_acq_rel, w, add, ldaddalh, ldaxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldadd4_relax, w, add, ldadd, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldadd4_acq, w, add, ldadda, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldadd4_rel, w, add, ldaddl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldadd4_acq_rel, w, add, ldaddal, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldadd8_relax, x, add, ldadd, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldadd8_acq, x, add, ldadda, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldadd8_rel, x, add, ldaddl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldadd8_acq_rel, x, add, ldaddal, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldclr1_relax, w, bic, ldclrb, ldxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldclr1_acq, w, bic, ldclrab, ldaxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldclr1_rel, w, bic, ldclrlb, ldxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldclr1_acq_rel, w, bic, ldclralb, ldaxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldclr2_relax, w, bic, ldclrh, ldxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldclr2_acq, w, bic, ldclrah, ldaxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldclr2_rel, w, bic, ldclrlh, ldxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldclr2_acq_rel, w, bic, ldclralh, ldaxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldclr4_relax, w, bic, ldclr, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldclr4_acq, w, bic, ldclra, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldclr4_rel, w, bic, ldclrl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldclr4_acq_rel, w, bic, ldclral, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldclr8_relax, x, bic, ldclr, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldclr8_acq, x, bic, ldclra, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldclr8_rel, x, bic, ldclrl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldclr8_acq_rel, x, bic, ldclral, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldeor1_relax, w, eor, ldeorb, ldxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldeor1_acq, w, eor, ldeorab, ldaxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldeor1_rel, w, eor, ldeorlb, ldxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldeor1_acq_rel, w, eor, ldeoralb, ldaxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldeor2_relax, w, eor, ldeorh, ldxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldeor2_acq, w, eor, ldeorah, ldaxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldeor2_rel, w, eor, ldeorlh, ldxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldeor2_acq_rel, w, eor, ldeoralh, ldaxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldeor4_relax, w, eor, ldeor, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldeor4_acq, w, eor, ldeora, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldeor4_rel, w, eor, ldeorl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldeor4_acq_rel, w, eor, ldeoral, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldeor8_relax, x, eor, ldeor, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldeor8_acq, x, eor, ldeora, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldeor8_rel, x, eor, ldeorl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldeor8_acq_rel, x, eor, ldeoral, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldset1_relax, w, orr, ldsetb, ldxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldset1_acq, w, orr, ldsetab, ldaxrb, stxrb\n"
        "stxlib_ldop __aarch64_ldset1_rel, w, orr, ldsetlb, ldxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldset1_acq_rel, w, orr, ldsetalb, ldaxrb, stlxrb\n"
        "stxlib_ldop __aarch64_ldset2_relax, w, orr, ldseth, ldxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldset2_acq, w, orr, ldsetah, ldaxrh, stxrh\n"
        "stxlib_ldop __aarch64_ldset2_rel, w, orr, ldsetlh, ldxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldset2_acq_rel, w, orr, ldsetalh, ldaxrh, stlxrh\n"
        "stxlib_ldop __aarch64_ldset4_relax, w, orr, ldset, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldset4_acq, w, orr, ldseta, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldset4_rel, w, orr, ldsetl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldset4_acq_rel, w, orr, ldsetal, ldaxr, stlxr\n"
        "stxlib_ldop __aarch64_ldset8_relax, x, orr, ldset, ldxr, stxr\n"
        "stxlib_ldop __aarch64_ldset8_acq, x, orr, ldseta, ldaxr, stxr\n"
        "stxlib_ldop __aarch64_ldset8_rel, x, orr, ldsetl, ldxr, stlxr\n"
        "stxlib_ldop __aarch64_ldset8_acq_rel, x, orr, ldsetal, ldaxr, stlxr\n"
        ".globl stxlib_outline_atomics_end\n"
        ".hidden stxlib_outline_atomics_end\n"
        "stxlib_outline_atomics_end:\n"
        ".popsection\n"

        // The memo (outline_atomics_backend.hpp), zero at load.
        ".pushsection .bss.stxlib_outline_atomics_memo,\"aw\",%nobits\n"
        ".globl stxlib_outline_atomics_memo\n"
        ".hidden stxlib_outline_atomics_memo\n"
        ".type stxlib_outline_atomics_memo, %object\n"
        ".size stxlib_outline_atomics_memo, 1\n"
        "stxlib_outline_atomics_memo:\n"
        "    .zero 1\n"
        ".popsection\n"

        // Linux's detection opens this file.
        ".pushsection .rodata.stxlib_outline_atomics_auxv,\"a\",%progbits\n"
        "stxlib_outline_atomics_auxv:\n"
        "    .asciz \"/proc/self/auxv\"\n"
        ".popsection\n");

#endif
