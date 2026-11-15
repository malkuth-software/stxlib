#pragma once

// Rounding a floating-point value to an integer with a stated result for
// every input (M2 C6; audit C, section 9). Replaces std::lround,
// std::llround, std::lrint and the float-to-integer cast where the value can
// be out of range or NaN, as GUI.Forms' layout code rounds values that a bad
// division can make huge or NaN (102 call sites at 401001da7).
//
// What the host gives instead:
// - Outside the target's range, or NaN, lround and llround return an
//   unspecified value and raise FE_INVALID (C17 7.12.9.7), and the cast is
//   undefined behaviour (C++20 [conv.fpint]). In practice x86-64's
//   conversions return the "integer indefinite" value (the most negative
//   integer) and AArch64's saturate (unconfirmed at a pin), so one program
//   lays out differently by architecture.
// - lround returns long, which is 64 bits on Linux, macOS and FreeBSD and 32
//   bits on Windows (LLP64), so the same call overflows at 2^31 on one host
//   family only.
// - lrint and nearbyint follow the floating-point environment's rounding
//   mode, which any library the program loads may change.
// - math_errhandling differs (errno written on glibc and the UCRT, not on
//   musl or FreeBSD; C-59 to C-63), so the call may write errno.
//
// Here every rule is named, the floating-point environment is neither read
// nor changed, errno is never written, and the out-of-range and NaN cases
// are states of the result, with a saturated reading for callers that want
// one. Everything is constexpr and works on the value's bits (no host call).
//
// Subnormals: as in the rest of STXlib's numerics (math.hpp, "No
// subnormals"; owner decision 2026-11-13), a subnormal argument is read as
// zero of its sign, so rounding it up gives 0, not 1.
//
// In proof scope: tests/rounding_tests.cpp checks every float exhaustively
// (with --exhaustive) and doubles at every binade boundary against the host's
// own functions where the host is defined; proofs/esbmc/time_for_ui.cpp
// states the bit-precise harness.

#include "stxlib/types.hpp"

namespace stxlib {

enum class Rounding : u8 {
    nearest_ties_away = 0, // lround's rule: halves away from zero
    nearest_ties_even = 1, // the default IEEE rule, as lrint in the default mode
    toward_zero = 2,       // trunc, and the cast
    down = 3,              // floor: toward negative infinity
    up = 4,                // ceil: toward positive infinity
};

// The result of a rounding: an integer in range, or why there is none. One
// state at a time (house style, section 2).
class IntegerRounding final {
public:
    [[nodiscard]] static constexpr IntegerRounding of(const i64 rounded) noexcept {
        return IntegerRounding(State::in_range, rounded);
    }
    [[nodiscard]] static constexpr IntegerRounding above_range() noexcept { return IntegerRounding(State::above, 0); }
    [[nodiscard]] static constexpr IntegerRounding below_range() noexcept { return IntegerRounding(State::below, 0); }
    [[nodiscard]] static constexpr IntegerRounding not_a_number() noexcept {
        return IntegerRounding(State::nan, 0);
    }

    [[nodiscard]] constexpr bool in_range() const noexcept { return state_ == State::in_range; }
    [[nodiscard]] constexpr bool is_above_range() const noexcept { return state_ == State::above; }
    [[nodiscard]] constexpr bool is_below_range() const noexcept { return state_ == State::below; }
    [[nodiscard]] constexpr bool is_not_a_number() const noexcept { return state_ == State::nan; }

    // The integer, or null outside the range. Borrowed from this value.
    [[nodiscard]] constexpr const i64* value() const& noexcept {
        if (state_ != State::in_range) return nullptr;
        return &value_;
    }
    const i64* value() const&& = delete;

    // The integer; the range's ends for a value beyond them; `when_nan` for
    // NaN (0 unless the caller says otherwise). `lowest` and `highest` are
    // the target range's ends (round_to_i32's results use the 32-bit ends).
    [[nodiscard]] constexpr i64 saturated(const i64 lowest, const i64 highest, const i64 when_nan = 0) const noexcept {
        if (state_ == State::above) return highest;
        if (state_ == State::below) return lowest;
        if (state_ == State::nan) return when_nan;
        return value_;
    }

    [[nodiscard]] constexpr bool operator==(const IntegerRounding& other) const noexcept {
        return state_ == other.state_ && value_ == other.value_;
    }

private:
    enum class State : u8 {
        in_range = 0,
        above = 1,
        below = 2,
        nan = 3,
    };

    constexpr IntegerRounding(const State rounding_state, const i64 rounded) noexcept
        : state_(rounding_state), value_(rounded) {}

    State state_;
    i64 value_;
};

namespace detail {

inline constexpr i64 i32_highest = 2147483647LL;
inline constexpr i64 i32_lowest = -2147483647LL - 1;

// Where the discarded fraction lies relative to one half.
enum class Fraction : u8 {
    zero = 0,
    below_half = 1,
    half = 2,
    above_half = 3,
};

// Whether the magnitude's integer part goes up by one under `rule`.
[[nodiscard]] constexpr bool rounds_up(const Rounding rule, const bool negative, const u64 whole,
                                       const Fraction fraction) noexcept {
    if (fraction == Fraction::zero) return false;
    switch (rule) {
    case Rounding::nearest_ties_away:
        return fraction == Fraction::half || fraction == Fraction::above_half;
    case Rounding::nearest_ties_even:
        return fraction == Fraction::above_half || (fraction == Fraction::half && (whole & 1u) != 0);
    case Rounding::toward_zero:
        return false;
    case Rounding::down:
        return negative;
    case Rounding::up:
        return !negative;
    }
    return false;
}

// The rounding of sign * (significand * 2^(exponent - fraction_bits)), for a
// normal value: `significand` holds the implicit bit, `exponent` is unbiased.
[[nodiscard]] constexpr IntegerRounding round_normal(const bool negative, const u64 significand, const i32 exponent,
                                                     const i32 fraction_bits, const Rounding rule) noexcept {
    if (exponent >= 63) {
        // |x| >= 2^63: only -2^63 itself is in range.
        const bool lowest = negative && exponent == 63 && significand == (static_cast<u64>(1) << fraction_bits);
        if (lowest) return IntegerRounding::of(i64_min);
        return negative ? IntegerRounding::below_range() : IntegerRounding::above_range();
    }
    u64 whole = 0;
    Fraction fraction = Fraction::zero;
    if (exponent >= fraction_bits) {
        // An integer already: at most 2^62 * (2 - 2^-52) < 2^63.
        whole = significand << static_cast<u32>(exponent - fraction_bits);
    } else if (exponent < 0) {
        // 0 < |x| < 1.
        whole = 0;
        if (exponent < -1) {
            fraction = Fraction::below_half;
        } else {
            const bool exactly_half = significand == (static_cast<u64>(1) << fraction_bits);
            fraction = exactly_half ? Fraction::half : Fraction::above_half;
        }
    } else {
        const u32 shift = static_cast<u32>(fraction_bits - exponent); // 1 to fraction_bits
        whole = significand >> shift;
        const u64 rest = significand & ((static_cast<u64>(1) << shift) - 1u);
        const u64 half = static_cast<u64>(1) << (shift - 1u);
        if (rest == 0) {
            fraction = Fraction::zero;
        } else if (rest < half) {
            fraction = Fraction::below_half;
        } else if (rest == half) {
            fraction = Fraction::half;
        } else {
            fraction = Fraction::above_half;
        }
    }
    if (rounds_up(rule, negative, whole, fraction)) whole = whole + 1u;
    // whole <= 2^62 * (2 - 2^-52) + 1 < 2^63 here, so the negation is exact.
    const i64 magnitude = static_cast<i64>(whole);
    return IntegerRounding::of(negative ? -magnitude : magnitude);
}

} // namespace detail

[[nodiscard]] constexpr IntegerRounding round_to_i64(const double x, const Rounding rule) noexcept {
    const u64 bits = __builtin_bit_cast(u64, x);
    const bool negative = (bits >> 63) != 0;
    const u32 biased = static_cast<u32>((bits >> 52) & 0x7ffu);
    const u64 mantissa = bits & ((static_cast<u64>(1) << 52) - 1u);
    if (biased == 0x7ffu) {
        if (mantissa != 0) return IntegerRounding::not_a_number();
        return negative ? IntegerRounding::below_range() : IntegerRounding::above_range();
    }
    // Zero, and a subnormal read as zero.
    if (biased == 0) return IntegerRounding::of(0);
    const u64 significand = mantissa | (static_cast<u64>(1) << 52);
    const IntegerRounding result =
        detail::round_normal(negative, significand, static_cast<i32>(biased) - 1023, 52, rule);
    return result;
}

[[nodiscard]] constexpr IntegerRounding round_to_i64(const float x, const Rounding rule) noexcept {
    const u32 bits = __builtin_bit_cast(u32, x);
    const bool negative = (bits >> 31) != 0;
    const u32 biased = (bits >> 23) & 0xffu;
    const u32 mantissa = bits & ((static_cast<u32>(1) << 23) - 1u);
    if (biased == 0xffu) {
        if (mantissa != 0) return IntegerRounding::not_a_number();
        return negative ? IntegerRounding::below_range() : IntegerRounding::above_range();
    }
    if (biased == 0) return IntegerRounding::of(0);
    const u64 significand = static_cast<u64>(mantissa | (static_cast<u32>(1) << 23));
    const IntegerRounding result =
        detail::round_normal(negative, significand, static_cast<i32>(biased) - 127, 23, rule);
    return result;
}

// The same, for the 32-bit range (pixel coordinates; Windows' long).
[[nodiscard]] constexpr IntegerRounding round_to_i32(const double x, const Rounding rule) noexcept {
    const IntegerRounding wide = round_to_i64(x, rule);
    const i64* const rounded = wide.value();
    if (rounded == nullptr) return wide;
    if (*rounded > detail::i32_highest) return IntegerRounding::above_range();
    if (*rounded < detail::i32_lowest) return IntegerRounding::below_range();
    return wide;
}

[[nodiscard]] constexpr IntegerRounding round_to_i32(const float x, const Rounding rule) noexcept {
    const IntegerRounding wide = round_to_i64(x, rule);
    const i64* const rounded = wide.value();
    if (rounded == nullptr) return wide;
    if (*rounded > detail::i32_highest) return IntegerRounding::above_range();
    if (*rounded < detail::i32_lowest) return IntegerRounding::below_range();
    return wide;
}

// The saturated readings most callers want: the nearest integer, halves away
// from zero (lround's rule), clamped to the range, NaN as 0.
[[nodiscard]] constexpr i64 round_to_i64_saturated(const double x) noexcept {
    return round_to_i64(x, Rounding::nearest_ties_away).saturated(i64_min, i64_max);
}

[[nodiscard]] constexpr i32 round_to_i32_saturated(const double x) noexcept {
    const i64 clamped = round_to_i32(x, Rounding::nearest_ties_away).saturated(detail::i32_lowest, detail::i32_highest);
    return static_cast<i32>(clamped);
}

} // namespace stxlib
