// ESBMC harnesses: frame pacing (include/stxlib/clocks.hpp, pace; M2 C1)
// and rounding to integers (include/stxlib/rounding.hpp; M2 C6). Both are
// pure, constant-time and loop-free, so no unwinding bound is needed.
//
// Not yet run. Intended command (the pinned ESBMC of
// notes planning/VERIFICATION_PROGRAMME.md, from the repository root):
//
//   esbmc proofs/esbmc/time_for_ui.cpp --std c++20 -I include \
//     --floatbv --function <harness>
//
// --floatbv makes the double harnesses bit-precise (SMT floating-point
// theory). The rounding harnesses state the result against arithmetic on
// the value itself (no host function), for every double and every float.

#include "stxlib/clocks.hpp"
#include "stxlib/rounding.hpp"

extern "C" long long __VERIFIER_nondet_longlong(void);
extern "C" double __VERIFIER_nondet_double(void);
extern "C" float __VERIFIER_nondet_float(void);
extern "C" unsigned char __VERIFIER_nondet_uchar(void);
extern "C" void __ESBMC_assume(_Bool);
extern "C" void __ESBMC_assert(_Bool, const char*);

using stxlib::Duration;
using stxlib::i64;
using stxlib::IntegerRounding;
using stxlib::IntervalTime;
using stxlib::Rounding;

// pace: for any origin, period and now (all of i64), the tick is the least
// origin + k * period strictly after now with k >= 1, and elapsed_periods is
// k - 1; or the range's end where that tick lies beyond it.
void harness_pace() {
    const i64 origin = __VERIFIER_nondet_longlong();
    const i64 period = __VERIFIER_nondet_longlong();
    const i64 now = __VERIFIER_nondet_longlong();
    const stxlib::Pacing paced =
        stxlib::pace(IntervalTime::from_nanoseconds(origin), Duration::nanoseconds(period), IntervalTime::from_nanoseconds(now));
    if (period <= 0) {
        __ESBMC_assert(paced.next.count_nanoseconds() == now && paced.elapsed_periods == 0, "no period: now");
        return;
    }
    if (now < origin) {
        __ESBMC_assert(paced.next.count_nanoseconds() == origin && paced.elapsed_periods == 0, "before the origin");
        return;
    }
    // In 128-bit arithmetic, so the statement itself cannot overflow.
    const __int128 k = static_cast<__int128>(paced.elapsed_periods);
    const __int128 span = static_cast<__int128>(now) - static_cast<__int128>(origin);
    __ESBMC_assert(k >= 0, "periods are not negative");
    if (span <= static_cast<__int128>(stxlib::i64_max)) {
        __ESBMC_assert(k * period <= span && span < (k + 1) * period, "elapsed_periods is the floor");
    }
    const __int128 tick = static_cast<__int128>(origin) + (k + 1) * period;
    if (span <= static_cast<__int128>(stxlib::i64_max) && tick <= static_cast<__int128>(stxlib::i64_max)) {
        __ESBMC_assert(static_cast<__int128>(paced.next.count_nanoseconds()) == tick, "the next tick");
        __ESBMC_assert(paced.next.count_nanoseconds() > now, "strictly after now");
    } else {
        __ESBMC_assert(paced.next.count_nanoseconds() == stxlib::i64_max, "saturates at the end");
    }
}

Rounding nondet_rule() {
    const unsigned char choice = __VERIFIER_nondet_uchar();
    __ESBMC_assume(choice < 5);
    return static_cast<Rounding>(choice);
}

// round_to_i64 of a double: NaN, beyond the range, or an integer r with
// |x - r| < 1 that the rule selects (stated with the floating-point
// comparisons alone, every one exact here: r is an integer of magnitude at
// most 2^63 and x - r is exact by Sterbenz's lemma where both are large).
void harness_round_double() {
    const double x = __VERIFIER_nondet_double();
    const Rounding rule = nondet_rule();
    const IntegerRounding got = stxlib::round_to_i64(x, rule);
    if (x != x) {
        __ESBMC_assert(got.is_not_a_number(), "NaN");
        return;
    }
    const bool subnormal = x != 0.0 && x > -2.2250738585072014e-308 && x < 2.2250738585072014e-308;
    const double y = subnormal ? 0.0 : x;
    if (!got.in_range()) {
        __ESBMC_assert(got.is_above_range() ? y >= 9223372036854775807.0 : y < -9223372036854775808.0,
                       "out of range only beyond it");
        return;
    }
    const double r = static_cast<double>(*got.value());
    __ESBMC_assert(r - 1.0 < y && y < r + 1.0, "within one");
    switch (rule) {
    case Rounding::toward_zero:
        __ESBMC_assert(y >= 0.0 ? (r <= y && y < r + 1.0) : (r - 1.0 < y && y <= r), "toward zero");
        break;
    case Rounding::down:
        __ESBMC_assert(r <= y && y < r + 1.0, "down");
        break;
    case Rounding::up:
        __ESBMC_assert(r - 1.0 < y && y <= r, "up");
        break;
    case Rounding::nearest_ties_away:
        __ESBMC_assert(y - r <= 0.5 && r - y <= 0.5, "nearest");
        if (y - r == 0.5 || r - y == 0.5) __ESBMC_assert(y >= 0.0 ? r > y : r < y, "halves away");
        break;
    case Rounding::nearest_ties_even:
        __ESBMC_assert(y - r <= 0.5 && r - y <= 0.5, "nearest");
        if (y - r == 0.5 || r - y == 0.5) {
            const i64 value = *got.value();
            __ESBMC_assert(value % 2 == 0, "halves to even");
        }
        break;
    }
}

// The float overload agrees with the double overload on the same value
// (every float converts to double exactly, and a float subnormal is a
// normal double, so the float rule is checked on its own: a subnormal float
// rounds as zero).
void harness_round_float() {
    const float x = __VERIFIER_nondet_float();
    const Rounding rule = nondet_rule();
    const IntegerRounding got = stxlib::round_to_i64(x, rule);
    const bool subnormal = x != 0.0f && x > -1.17549435e-38f && x < 1.17549435e-38f;
    const double widened = subnormal ? 0.0 : static_cast<double>(x);
    __ESBMC_assert(got == stxlib::round_to_i64(widened, rule), "float agrees with double");
}
