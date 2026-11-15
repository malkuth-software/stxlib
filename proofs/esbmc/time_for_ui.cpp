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
//
// Pointer width: run each harness twice, as above (64-bit) and with
// `--32 -D__wasm32__` added (wasm32's 32-bit usize and uptr; types.hpp
// admits 32-bit pointers only under __wasm32__, and the cores here have no
// other __wasm32__ branch). ESBMC's --32 and -D options are as its --help
// lists them; unconfirmed at the pinned ESBMC (VERIFICATION_PROGRAMME).

#include "stxlib/clocks.hpp"
#include "stxlib/rounding.hpp"

extern "C" long long __VERIFIER_nondet_longlong(void);
extern "C" double __VERIFIER_nondet_double(void);
extern "C" float __VERIFIER_nondet_float(void);
extern "C" unsigned char __VERIFIER_nondet_uchar(void);
extern "C" void __ESBMC_assume(bool);
extern "C" void __ESBMC_assert(bool, const char*);

using stxlib::Duration;
using stxlib::i64;
using stxlib::IntegerRounding;
using stxlib::IntervalTime;
using stxlib::Rounding;

// pace: for any origin, period and now (all of i64), the tick is the least
// origin + k * period strictly after now with k >= 1, and elapsed_periods is
// k - 1; or the range's end where that tick lies beyond it.
//
// The statement is in 128-bit arithmetic, which ESBMC's 32-bit target does
// not have ("__int128 is not supported on this target", M5, 2026-11-15), so
// harness_pace is compiled only where __int128 exists and the 32-bit run of
// this file checks the rounding harnesses alone. pace uses i64 only (no
// usize or uptr: clocks.hpp), so its statement does not depend on the
// pointer width and the 64-bit verdict is the 32-bit one by that argument,
// not by a run.
#if defined(__SIZEOF_INT128__)
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
#endif

Rounding nondet_rule() {
    const unsigned char choice = __VERIFIER_nondet_uchar();
    __ESBMC_assume(choice < 5);
    return static_cast<Rounding>(choice);
}

// round_to_i64 of a double: NaN, beyond the range, or an integer r with
// |x - r| < 1 that the rule selects. Every comparison below is exact:
// - |y| >= 2^52: every such double is an integer, so each rule returns y
//   itself; y converts to i64 exactly (it lies in [-2^63, 2^63)), and the
//   result is compared as an integer;
// - |y| < 2^52: |r| <= 2^52, so r, r - 1 and r + 1 are doubles exactly,
//   and so are r - 0.5 and r + 0.5 except the one beyond 2^52 in magnitude
//   when |r| = 2^52, which rounds to r; y, strictly inside 2^52, compares
//   with that rounded bound as with the exact one (y <= it holds and
//   y == it fails either way). The rule is stated by these comparisons.
// (M5, 2026-11-15: the first form compared y with r - 1.0 and r + 1.0 at
// every magnitude; at |y| >= 2^54 those round back to r, and the run refuted
// "within one" at x = -288230376386429952 (about -2^58), where r == y: a
// harness defect, not the code's.)
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
        __ESBMC_assert(got.is_above_range() ? y >= 9223372036854775808.0 : y < -9223372036854775808.0,
                       "out of range only beyond it");
        return;
    }
    __ESBMC_assert(y >= -9223372036854775808.0 && y < 9223372036854775808.0, "in range only within it");
    const i64 value = *got.value();
    const double magnitude = y < 0.0 ? -y : y;
    if (magnitude >= 4503599627370496.0) { // 2^52
        __ESBMC_assert(value == static_cast<i64>(y), "an integral value is its own rounding");
        return;
    }
    __ESBMC_assert(value >= -4503599627370496LL && value <= 4503599627370496LL, "a small value rounds near it");
    const double r = static_cast<double>(value);
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
        __ESBMC_assert(r - 0.5 <= y && y <= r + 0.5, "nearest");
        if (y == r + 0.5 || y == r - 0.5) __ESBMC_assert(y >= 0.0 ? r > y : r < y, "halves away");
        break;
    case Rounding::nearest_ties_even:
        __ESBMC_assert(r - 0.5 <= y && y <= r + 0.5, "nearest");
        if (y == r + 0.5 || y == r - 0.5) __ESBMC_assert(value % 2 == 0, "halves to even");
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
