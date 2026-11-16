#pragma once

// Randomness with named, frozen algorithms, replacing rand, srand,
// std::random_device, the standard engines and distributions, and
// std::shuffle (stx_auxlib owner decisions of 2026-10-31). Ported from
// stx_auxlib's include/stx_auxlib/random.hpp at b6e330a; the interface is
// restated in STXlib's vocabulary (fixed arrays, views, OperationResult), and the
// algorithms and their outputs are unchanged.
//
// Reproducibility: a Seed and a generator name fix the whole sequence on
// every host and toolchain, forever. ChaCha12Stream is ChaCha with 12
// rounds over a 256-bit key; Xoshiro256PlusPlus is Blackman and Vigna's
// xoshiro256++ 1.0; seed_from_number expands a 64-bit number with
// SplitMix64. The distributions below are specified here, not by a
// standard library (whose distributions differ between libc++, libstdc++
// and MSVC for the same engine and seed). Changing any of them would break
// stored seeds; a new algorithm gets a new name instead.
//
// Choosing a generator: ChaCha12Stream is the default. Its output cannot be
// predicted from earlier output, so it is safe for anything (shuffles a
// server trusts, temporary names, tokens). Xoshiro256PlusPlus is faster and
// statistically excellent but predictable from its output: use it by name
// for hot loops where speed matters and security never will (particles,
// noise).
//
// Entropy fails closed: when the host has no entropy source, a request for
// an unpredictable seed returns a failure, never a weak seed.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::random {

// 256 bits that determine a generator's whole sequence.
struct Seed final {
    u64 words[4]{};
};

// An unpredictable seed from the host's entropy source (stxlib::fill_entropy,
// entropy.hpp); its failures are fill_entropy's.
[[nodiscard]] OperationResult<Seed> entropy_seed() noexcept;

// A seed fixed by one number (a deal number, a level seed, a test case):
// four successive SplitMix64 outputs from state `number`.
[[nodiscard]] Seed seed_from_number(u64 number) noexcept;

// ChaCha with 12 rounds. Key: the seed's words, each little-endian into
// two key words. A stream is 2^64 blocks (2^70 bytes) long and repeats
// after that, as ChaCha's 64-bit block counter does. Block layout as in
// Bernstein's original ChaCha: words 12 and 13 a 64-bit block counter
// starting at 0, words 14 and 15 the 64-bit `stream`, so one seed gives
// 2^64 independent streams. Each next() takes the next two output words of
// the block, the first as the low half.
class ChaCha12Stream final {
public:
    static constexpr bool is_stxlib_generator = true;

    explicit ChaCha12Stream(const Seed& seed, u64 stream = 0) noexcept;

    // Inline: one bounds test and two loads; refill runs once per 32
    // outputs.
    [[nodiscard]] u64 next() noexcept {
        if (position_ == 64) refill();
        const u64 low = blocks_[position_];
        const u64 high = blocks_[position_ + 1];
        position_ += 2;
        const u64 result = low | (high << 32);
        return result;
    }

    // Fills `output` with the stream's next bytes (little-endian words).
    // Whole 64-bit outputs are consumed; a trailing partial output's unused
    // bytes are discarded.
    void fill(MutableView<Byte> output) noexcept;

private:
    void refill() noexcept;

    u32 input_[16]{};
    // Four consecutive blocks in order, computed together in 128-bit lane
    // groups.
    u32 blocks_[64]{};
    // The next unused word of blocks_; 64 when they are used up.
    usize position_{64};
};

// xoshiro256++ 1.0. The state is the seed's words; an all-zero seed, which
// the algorithm cannot leave, is replaced by seed_from_number(0).
class Xoshiro256PlusPlus final {
public:
    static constexpr bool is_stxlib_generator = true;

    explicit Xoshiro256PlusPlus(const Seed& seed) noexcept;

    [[nodiscard]] u64 next() noexcept;
    // Advances by 2^128 outputs: up to 2^128 non-overlapping subsequences
    // for parallel work.
    void jump() noexcept;
    // Advances by 2^192 outputs.
    void long_jump() noexcept;

private:
    void apply_jump(const u64 (&table)[4]) noexcept;

    u64 state_[4]{};
};

// Explicit opt-in: a type is a generator only if it declares
// `is_stxlib_generator`, never because it happens to have next().
template <typename Generator>
concept RandomGenerator = Generator::is_stxlib_generator;

namespace detail {

// One ChaCha block: `double_rounds` column-and-diagonal round pairs over
// `input`, plus `input` (ChaCha12 uses 6, ChaCha20 10). Exposed for the
// known-answer tests. `output` may not be `input`.
void chacha_block(const u32 (&input)[16], int double_rounds, u32 (&output)[16]) noexcept;

// The high and low halves of a 64 x 64 bit product, without a 128-bit type.
struct Product final {
    u64 high{0};
    u64 low{0};
};
[[nodiscard]] Product multiply(u64 left, u64 right) noexcept;

// high - low as an unsigned count, for low <= high, without wrapping.
[[nodiscard]] u64 distance(i64 low, i64 high) noexcept;
// low + offset, for offset <= distance(low, high), without wrapping.
[[nodiscard]] i64 advance(i64 low, u64 offset) noexcept;

[[nodiscard]] double scale_into(double low, double high, double unit) noexcept;

} // namespace detail

// A uniformly distributed integer in [0, bound), by Lemire's
// nearly-divisionless method (2019), drawing one output, or more when it
// rejects. `bound` must be positive; a zero bound yields 0 without drawing.
template <RandomGenerator Generator>
[[nodiscard]] u64 uniform_below(Generator& generator, const u64 bound) noexcept {
    if (bound == 0) return 0;
    detail::Product product = detail::multiply(generator.next(), bound);
    if (product.low < bound) {
        // 2^64 mod bound, as (2^64 - bound) mod bound: u64_max - bound + 1
        // cannot wrap for bound >= 1.
        const u64 threshold = (u64_max - bound + 1) % bound;
        while (product.low < threshold) product = detail::multiply(generator.next(), bound);
    }
    return product.high;
}

// A uniformly distributed integer in [low, high], inclusive; `low` must not
// exceed `high` (when it does, the result is `low`).
template <RandomGenerator Generator>
[[nodiscard]] i64 uniform_between(Generator& generator, const i64 low, const i64 high) noexcept {
    if (high <= low) return low;
    const u64 span = detail::distance(low, high);
    u64 offset = 0;
    if (span == u64_max) {
        offset = generator.next();
    } else {
        offset = uniform_below(generator, span + 1);
    }
    const i64 result = detail::advance(low, offset);
    return result;
}

// A double in [0, 1): the output's top 53 bits times 2^-53, so every
// result is a multiple of 2^-53.
template <RandomGenerator Generator>
[[nodiscard]] double uniform_unit(Generator& generator) noexcept {
    const u64 bits = generator.next() >> 11;
    const double result = static_cast<double>(bits) * 0x1.0p-53;
    return result;
}

// A float in [0, 1): the output's top 24 bits times 2^-24.
template <RandomGenerator Generator>
[[nodiscard]] float uniform_unit_float(Generator& generator) noexcept {
    const u64 bits = generator.next() >> 40;
    const float result = static_cast<float>(bits) * 0x1.0p-24f;
    return result;
}

// A double in [low, high): low + (high - low) * uniform_unit, with a result
// that rounds up to `high` moved to the largest double below it. Requires
// finite low < high (otherwise the result is `low`). No subnormals (batch
// 3c9): subnormal bounds are read as zero of their sign (so the test
// low < high is made on the read bounds, and the result is the read `low`
// when it fails), and a result that would be subnormal is zero of its sign,
// or `low` when that zero is not below `high`.
template <RandomGenerator Generator>
[[nodiscard]] double uniform_real(Generator& generator, const double low, const double high) noexcept {
    const double unit = uniform_unit(generator);
    const double result = detail::scale_into(low, high, unit);
    return result;
}

// True with probability `probability`, compared against uniform_unit;
// probabilities at or below 0 never, at or above 1 always.
template <RandomGenerator Generator>
[[nodiscard]] bool chance(Generator& generator, const double probability) noexcept {
    const bool result = uniform_unit(generator) < probability;
    return result;
}

// Fisher-Yates, from the last element down: for i from count - 1 to 1, swap
// element i with element uniform_below(i + 1). Element must be nothrow
// move constructible and move assignable.
template <RandomGenerator Generator, typename Element>
void shuffle(Generator& generator, const MutableView<Element> elements) noexcept {
    // Compiler built-in traits, since a public header names no std:: type
    // (audit A, I2).
    static_assert(__is_nothrow_constructible(Element, Element&&),
                  "shuffle: Element must be nothrow move constructible");
    static_assert(__is_nothrow_assignable(Element&, Element&&), "shuffle: Element must be nothrow move assignable");
    for (usize index = elements.count(); index > 1; --index) {
        const usize last = index - 1;
        const usize other = static_cast<usize>(uniform_below(generator, index));
        if (other == last) continue;
        Element held(static_cast<Element&&>(elements[last]));
        elements[last] = static_cast<Element&&>(elements[other]);
        elements[other] = static_cast<Element&&>(held);
    }
}

} // namespace stxlib::random
