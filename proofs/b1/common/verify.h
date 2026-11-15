/* Tool-neutral vocabulary for the B1 harnesses (C and C++).
 *
 * The runner (tools/run_b1.py) defines exactly one STX_TOOL_<name> per run,
 * STX_N for rows with an input length, and STX_VACUITY for the vacuity-probe
 * variant. A harness states inputs with the nondet_ functions, assumptions
 * with STX_ASSUME and properties with STX_ASSERT, and ends its entry with
 * STX_VACUITY_PROBE(), which a correct run must report reachable.
 *
 * nondet_ functions have no body on purpose: ESBMC treats an undefined
 * function named nondet_* as returning any value of its type, and CBMC
 * treats every undefined function so. GenMC harnesses (rows 22 and 23) use
 * no nondeterministic data.
 */
#ifndef STX_B1_VERIFY_H
#define STX_B1_VERIFY_H

#if defined(__cplusplus)
#include <cassert>
#include <cstddef>
#include <cstdint>
#define STX_EXTERN_C extern "C"
#else
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STX_EXTERN_C
#endif

STX_EXTERN_C unsigned char nondet_uchar(void);
STX_EXTERN_C unsigned short nondet_ushort(void);
STX_EXTERN_C unsigned int nondet_uint(void);
STX_EXTERN_C unsigned long long nondet_ulonglong(void);
STX_EXTERN_C long long nondet_longlong(void);
STX_EXTERN_C size_t nondet_size(void);
STX_EXTERN_C bool nondet_bool(void);
STX_EXTERN_C float nondet_float(void);
STX_EXTERN_C double nondet_double(void);

/* An unbounded run (STX_UNBOUNDED) states no length bound: a harness's
 * STX_N then admits every length, and the tool's k-induction or unbounded
 * mode carries the proof. */
#if defined(STX_UNBOUNDED) && !defined(STX_N)
#define STX_N SIZE_MAX
#endif

/* STX_DECLARE_INTRINSICS is for a tool whose C++ front end does not know
 * its own assumption intrinsic without a declaration; the first smoke run
 * decides (tools/tools.json). */
#if defined(STX_TOOL_ESBMC)
#if defined(STX_DECLARE_INTRINSICS)
STX_EXTERN_C void __ESBMC_assume(bool condition);
#endif
#define STX_ASSUME(condition) __ESBMC_assume(condition)
#elif defined(STX_TOOL_CBMC)
#if defined(STX_DECLARE_INTRINSICS)
STX_EXTERN_C void __CPROVER_assume(bool condition);
#endif
#define STX_ASSUME(condition) __CPROVER_assume(condition)
#elif defined(STX_TOOL_GENMC)
/* GenMC harnesses assume nothing; a use is a harness error. */
#define STX_ASSUME(condition) STX_ASSUME_is_not_available_under_GenMC
#elif defined(STX_TOOL_CPPVERIFY)
/* cpp-verify files carry contracts, not harness entries. */
#define STX_ASSUME(condition) STX_ASSUME_is_not_available_under_cpp_verify
#else
#error "the runner defines one STX_TOOL_<name>"
#endif

/* The message is part of the asserted expression so every tool prints it
 * with the failed property. */
#define STX_ASSERT(condition, message) assert((condition) && (message))

#if defined(STX_VACUITY)
#define STX_VACUITY_PROBE() assert(0 && "vacuity probe: this point is reachable")
#else
#define STX_VACUITY_PROBE() ((void)0)
#endif

#endif /* STX_B1_VERIFY_H */
