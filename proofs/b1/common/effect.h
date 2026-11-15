/* effect.h: bounded effects for B1's bound-effect configuration.
 *
 * Owner direction of 2026-11-13 (decisions/2026-11-13-root-failsafe-bound-
 * effect.md): arithmetic is an effect with stated bounds, not control flow.
 * A harness of the bound-effect configuration replaces each arithmetic
 * result on a lifted line (lift.py's `//@ abstract` directive) by any
 * value within its stated bound, so the run verifies control flow and the
 * definedness of what is left, and solver-hard arithmetic cannot make it
 * time out. Each bound is a numeric obligation, established separately
 * (exhaustively where the domain allows, by the D-stage tools otherwise)
 * and listed with the row; this run assumes it and proves nothing about
 * the arithmetic's value.
 *
 * stx_effect_*(lo, hi): any value of the type in [lo, hi] (a NaN is never
 * returned: it fails both comparisons).
 * stx_effect_*_given(pre, lo, hi): the same, after asserting `pre`, the
 * condition under which the bound is stated (the operands' own bounds).
 */
#ifndef STX_B1_EFFECT_H
#define STX_B1_EFFECT_H

#include "verify.h"

#include <cstdint>

static inline std::int64_t stx_effect_i64(const std::int64_t lo, const std::int64_t hi) {
    const std::int64_t value = static_cast<std::int64_t>(nondet_longlong());
    STX_ASSUME(value >= lo && value <= hi);
    return value;
}

static inline std::uint64_t stx_effect_u64(const std::uint64_t lo, const std::uint64_t hi) {
    const std::uint64_t value = static_cast<std::uint64_t>(nondet_ulonglong());
    STX_ASSUME(value >= lo && value <= hi);
    return value;
}

static inline double stx_effect_f64(const double lo, const double hi) {
    const double value = nondet_double();
    STX_ASSUME(value >= lo && value <= hi);
    return value;
}

static inline std::int64_t stx_effect_i64_given(const bool pre, const std::int64_t lo, const std::int64_t hi) {
    STX_ASSERT(pre, "effect: the operands are within the bounds the effect is stated for");
    return stx_effect_i64(lo, hi);
}

static inline std::uint64_t stx_effect_u64_given(const bool pre, const std::uint64_t lo, const std::uint64_t hi) {
    STX_ASSERT(pre, "effect: the operands are within the bounds the effect is stated for");
    return stx_effect_u64(lo, hi);
}

static inline double stx_effect_f64_given(const bool pre, const double lo, const double hi) {
    STX_ASSERT(pre, "effect: the operands are within the bounds the effect is stated for");
    return stx_effect_f64(lo, hi);
}

#endif
