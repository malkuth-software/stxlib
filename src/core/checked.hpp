#pragma once

// The one marker for intended modular arithmetic, as in stx_auxlib
// (stx_auxlib 08c8132, src/checked.hpp). The integer-sanitizer build
// (CMakeLists.txt, STXLIB_INTEGER_CHECKS) halts on any unsigned wrap,
// signed overflow or value-changing conversion outside a function marked
// STXLIB_MODULAR. Only arithmetic defined modulo 2^n carries the mark.

#if defined(__clang__)
#define STXLIB_MODULAR                                                                        \
    __attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base",          \
                               "implicit-integer-truncation")))
#else
#define STXLIB_MODULAR
#endif
