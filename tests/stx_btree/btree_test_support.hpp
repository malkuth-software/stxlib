#pragma once

// The two helpers the sequence tree tests use from stx_btree's
// tests/test_support.hpp (stx_auxlib b6e330a): the failure report and
// SplitMix64, unchanged but for SplitMix64's modular marker (its
// arithmetic is modulo 2^64; core/checked.hpp). The key makers stay behind
// with the maps STXlib does not carry.

#include "core/checked.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace stx_btree_tests {

class TestFailure final {
public:
    static void expect(bool condition, const char* label, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAILED [%s]: %s\n", label, what);
            std::abort();
        }
    }
};

// SplitMix64 (Steele, Lea, Flood), whose finalizer is a bijection of 64-bit
// words: distinct indexes give distinct keys.
class SplitMix64 final {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state(seed) {}

    [[nodiscard]] STXLIB_MODULAR static std::uint64_t mix(std::uint64_t value) noexcept {
        std::uint64_t z = value + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    [[nodiscard]] STXLIB_MODULAR std::uint64_t next() noexcept {
        const std::uint64_t value = mix(state);
        state += 0x9e3779b97f4a7c15ULL;
        return value;
    }

    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
        const std::uint64_t value = next();
        return value % bound;
    }

private:
    std::uint64_t state = 0;
};

} // namespace stx_btree_tests
