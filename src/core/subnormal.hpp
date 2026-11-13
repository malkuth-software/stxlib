#pragma once

// STXlib's subnormal rule (owner decisions 2026-11-13, notes
// decisions/2026-11-13-locals-float-mutability.md, "Subnormals" and D1):
// numeric code neither accepts nor produces subnormal values. A subnormal
// value is replaced by zero of its sign. Works on the bits; the
// floating-point environment (flush-to-zero, denormals-are-zero) is never
// set or read. Zero, normal, infinite and NaN values pass unchanged, NaN
// payloads included. Private to STXlib's sources (batch 3c9; the math's
// flush.cpp of batch 3c8 uses it too).

#include <bit>
#include <cstdint>

namespace stxlib::detail {

// Zero of the value's sign when the value is subnormal (biased exponent 0,
// significand not 0), else the value itself. A zero maps to itself.
[[nodiscard]] inline double flush_subnormal(const double value) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const std::uint64_t exponent_bits = 0x7ff0000000000000ULL;
    const std::uint64_t sign_bit = 0x8000000000000000ULL;
    if ((bits & exponent_bits) == 0) {
        const double signed_zero = std::bit_cast<double>(bits & sign_bit);
        return signed_zero;
    }
    return value;
}

[[nodiscard]] inline float flush_subnormal(const float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t exponent_bits = 0x7f800000U;
    const std::uint32_t sign_bit = 0x80000000U;
    if ((bits & exponent_bits) == 0) {
        const float signed_zero = std::bit_cast<float>(bits & sign_bit);
        return signed_zero;
    }
    return value;
}

} // namespace stxlib::detail
