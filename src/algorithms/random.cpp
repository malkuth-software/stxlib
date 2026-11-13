// stxlib::random (include/stxlib/random.hpp): the generators and seed
// expansion, pure computation. Ported from stx_auxlib's src/random.cpp at
// b6e330a: the same computation over STXlib's types (C arrays in place of
// std::array; host entropy through stxlib::fill_entropy). tests/reference/
// random_reference.py is the independent reference the tests check against.

#include "stxlib/random.hpp"

#include "core/checked.hpp"
#include "core/subnormal.hpp"
#include "stxlib/entropy.hpp"

#include <bit>
#include <cmath>
#include <cstring>

namespace stxlib::random {

namespace {

constexpr u32 chacha_constants[4] = {0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U};
constexpr int chacha12_double_rounds = 6;

// ChaCha, SplitMix64 and xoshiro are defined modulo 2^32 or 2^64; their
// wraparound is the algorithm.
STXLIB_MODULAR void quarter_round(u32 (&x)[16], const usize a, const usize b, const usize c,
                                  const usize d) noexcept {
    x[a] += x[b];
    x[d] = std::rotl(x[d] ^ x[a], 16);
    x[c] += x[d];
    x[b] = std::rotl(x[b] ^ x[c], 12);
    x[a] += x[b];
    x[d] = std::rotl(x[d] ^ x[a], 8);
    x[c] += x[d];
    x[b] = std::rotl(x[b] ^ x[c], 7);
}

STXLIB_MODULAR u64 splitmix_step(u64& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    u64 mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
    const u64 result = mixed ^ (mixed >> 31);
    return result;
}

// xoshiro256's jump polynomials (Blackman and Vigna's reference code).
constexpr u64 jump_table[4] = {0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL, 0xa9582618e03fc9aaULL,
                               0x39abdc4529b1661cULL};
constexpr u64 long_jump_table[4] = {0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL, 0x77710069854ee241ULL,
                                    0x39109bb02acbe635ULL};

} // namespace

STXLIB_MODULAR void detail::chacha_block(const u32 (&input)[16], const int double_rounds,
                                         u32 (&output)[16]) noexcept {
    u32 x[16]{};
    for (usize index = 0; index < 16; ++index) x[index] = input[index];
    for (int round = 0; round < double_rounds; ++round) {
        quarter_round(x, 0, 4, 8, 12);
        quarter_round(x, 1, 5, 9, 13);
        quarter_round(x, 2, 6, 10, 14);
        quarter_round(x, 3, 7, 11, 15);
        quarter_round(x, 0, 5, 10, 15);
        quarter_round(x, 1, 6, 11, 12);
        quarter_round(x, 2, 7, 8, 13);
        quarter_round(x, 3, 4, 9, 14);
    }
    for (usize index = 0; index < 16; ++index) output[index] = x[index] + input[index];
}

namespace {

// One ChaCha word across four blocks: lane i belongs to block i. GCC and
// Clang get their generic 16-byte vector type, which they lower to one
// 128-bit register (NEON, SSE2) on every target; it is a compiler feature,
// not a platform intrinsic. Other compilers use the array form, which
// computes the same values.
#if defined(__GNUC__) || defined(__clang__)
using Lanes = u32 __attribute__((vector_size(16)));

STXLIB_MODULAR void quarter_round_lanes(Lanes (&x)[16], const usize a, const usize b, const usize c,
                                        const usize d) noexcept {
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = (x[d] << 16) | (x[d] >> 16);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = (x[b] << 12) | (x[b] >> 20);
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = (x[d] << 8) | (x[d] >> 24);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = (x[b] << 7) | (x[b] >> 25);
}

Lanes broadcast(const u32 value) noexcept {
    const Lanes result = {value, value, value, value};
    return result;
}

void set_lane(Lanes& lanes, const usize lane, const u32 value) noexcept { lanes[lane] = value; }

STXLIB_MODULAR void add_lanes(Lanes& target, const Lanes& source) noexcept { target += source; }
#else
struct Lanes final {
    u32 lane[4];
};

STXLIB_MODULAR void add_lanes(Lanes& target, const Lanes& source) noexcept {
    for (usize index = 0; index < 4; ++index) target.lane[index] += source.lane[index];
}

STXLIB_MODULAR void xor_rotate_lanes(Lanes& target, const Lanes& source, const int count) noexcept {
    for (usize index = 0; index < 4; ++index) {
        target.lane[index] = std::rotl(target.lane[index] ^ source.lane[index], count);
    }
}

void quarter_round_lanes(Lanes (&x)[16], const usize a, const usize b, const usize c, const usize d) noexcept {
    add_lanes(x[a], x[b]);
    xor_rotate_lanes(x[d], x[a], 16);
    add_lanes(x[c], x[d]);
    xor_rotate_lanes(x[b], x[c], 12);
    add_lanes(x[a], x[b]);
    xor_rotate_lanes(x[d], x[a], 8);
    add_lanes(x[c], x[d]);
    xor_rotate_lanes(x[b], x[c], 7);
}

Lanes broadcast(const u32 value) noexcept {
    const Lanes result = {{value, value, value, value}};
    return result;
}

void set_lane(Lanes& lanes, const usize lane, const u32 value) noexcept { lanes.lane[lane] = value; }
#endif
static_assert(sizeof(Lanes) == 16, "four 32-bit lanes");

// Four blocks of the same key and stream at counters c to c + 3, block i
// in output[16 i .. 16 i + 15]; each equals detail::chacha_block at its
// counter.
STXLIB_MODULAR void chacha_four_blocks(const u32 (&input)[16], const int double_rounds,
                                       u32 (&output)[64]) noexcept {
    // The counters' two words for the four blocks.
    const u64 counter = input[12] | (static_cast<u64>(input[13]) << 32);
    Lanes counter_low = broadcast(0);
    Lanes counter_high = broadcast(0);
    for (usize lane = 0; lane < 4; ++lane) {
        const u64 lane_counter = counter + lane;
        set_lane(counter_low, lane, static_cast<u32>(lane_counter));
        set_lane(counter_high, lane, static_cast<u32>(lane_counter >> 32));
    }
    Lanes x[16]{};
    for (usize word = 0; word < 16; ++word) x[word] = broadcast(input[word]);
    x[12] = counter_low;
    x[13] = counter_high;
    // The starting state is rebuilt for the final addition rather than kept:
    // sixteen more live vectors would spill the working state.
    for (int round = 0; round < double_rounds; ++round) {
        quarter_round_lanes(x, 0, 4, 8, 12);
        quarter_round_lanes(x, 1, 5, 9, 13);
        quarter_round_lanes(x, 2, 6, 10, 14);
        quarter_round_lanes(x, 3, 7, 11, 15);
        quarter_round_lanes(x, 0, 5, 10, 15);
        quarter_round_lanes(x, 1, 6, 11, 12);
        quarter_round_lanes(x, 2, 7, 8, 13);
        quarter_round_lanes(x, 3, 4, 9, 14);
    }
    // Word-major vectors to block-major output.
    u32 word_major[64]{};
    for (usize word = 0; word < 16; ++word) {
        Lanes start = broadcast(input[word]);
        if (word == 12) start = counter_low;
        if (word == 13) start = counter_high;
        add_lanes(x[word], start);
        std::memcpy(word_major + word * 4, &x[word], sizeof(Lanes));
    }
    for (usize block = 0; block < 4; ++block) {
        for (usize word = 0; word < 16; ++word) output[block * 16 + word] = word_major[word * 4 + block];
    }
}

} // namespace

detail::Product detail::multiply(const u64 left, const u64 right) noexcept {
    const u64 left_low = left & 0xffffffffULL;
    const u64 left_high = left >> 32;
    const u64 right_low = right & 0xffffffffULL;
    const u64 right_high = right >> 32;
    const u64 low_low = left_low * right_low;
    const u64 high_low = left_high * right_low;
    const u64 low_high = left_low * right_high;
    const u64 high_high = left_high * right_high;
    // The middle partial sums, carried into the high half.
    const u64 middle = (low_low >> 32) + (high_low & 0xffffffffULL) + low_high;
    Product result{};
    result.high = high_high + (high_low >> 32) + (middle >> 32);
    result.low = ((middle & 0xffffffffULL) << 32) | (low_low & 0xffffffffULL);
    return result;
}

u64 detail::distance(const i64 low, const i64 high) noexcept {
    // Same signs: the signed difference cannot overflow. Opposite signs:
    // high + |low|, with |low| = -(low + 1) + 1 so i64_min has one.
    if (low >= 0 || high < 0) return static_cast<u64>(high - low);
    const u64 below_zero = static_cast<u64>(-(low + 1)) + 1;
    const u64 result = static_cast<u64>(high) + below_zero;
    return result;
}

i64 detail::advance(const i64 low, const u64 offset) noexcept {
    if (low >= 0) return low + static_cast<i64>(offset);
    // Steps that stay below zero, then the rest from zero upwards.
    const u64 to_minus_one = static_cast<u64>(-(low + 1));
    if (offset <= to_minus_one) return low + static_cast<i64>(offset);
    const i64 result = static_cast<i64>(offset - to_minus_one - 1);
    return result;
}

// STXlib batch 3c9 (owner decision D1, 2026-11-13): no subnormals. The
// bounds are read with subnormals as zero of their sign, and a result that
// would be subnormal is zero of its sign, or `low` when that zero is not
// below `high`.
double detail::scale_into(const double given_low, const double given_high, const double unit) noexcept {
    const double low = ::stxlib::detail::flush_subnormal(given_low);
    const double high = ::stxlib::detail::flush_subnormal(given_high);
    if (!(low < high) || !std::isfinite(low) || !std::isfinite(high)) return low;
    const double width = high - low;
    double result = 0.0;
    if (std::isfinite(width)) {
        result = low + width * unit;
    } else {
        // The width overflows only for bounds of opposite sign near the
        // largest double; interpolate without forming it.
        result = low * (1.0 - unit) + high * unit;
    }
    if (result >= high) result = std::nextafter(high, low);
    if (result < low) result = low;
    result = ::stxlib::detail::flush_subnormal(result);
    if (!(result < high)) result = low;
    return result;
}

Result<Seed> entropy_seed() noexcept {
    Byte bytes[32]{};
    const Status filled = fill_entropy(MutableView<Byte>(bytes));
    if (filled.failed()) return Result<Seed>::failure(*filled.error());
    Seed seed{};
    for (usize word = 0; word < 4; ++word) {
        u64 value = 0;
        for (usize byte = 0; byte < 8; ++byte) {
            const u64 part = bytes[word * 8 + byte];
            value |= part << (8 * byte);
        }
        seed.words[word] = value;
    }
    return Result<Seed>::success(seed);
}

Seed seed_from_number(const u64 number) noexcept {
    u64 state = number;
    Seed seed{};
    for (u64& word : seed.words) word = splitmix_step(state);
    return seed;
}

ChaCha12Stream::ChaCha12Stream(const Seed& seed, const u64 stream) noexcept {
    for (usize index = 0; index < 4; ++index) input_[index] = chacha_constants[index];
    for (usize word = 0; word < 4; ++word) {
        input_[4 + 2 * word] = static_cast<u32>(seed.words[word]);
        input_[5 + 2 * word] = static_cast<u32>(seed.words[word] >> 32);
    }
    input_[12] = 0;
    input_[13] = 0;
    input_[14] = static_cast<u32>(stream);
    input_[15] = static_cast<u32>(stream >> 32);
}

// Computes the four blocks for counters c to c + 3, then advances the
// 64-bit counter in words 12 and 13 by four.
STXLIB_MODULAR void ChaCha12Stream::refill() noexcept {
    chacha_four_blocks(input_, chacha12_double_rounds, blocks_);
    u64 counter = input_[12] | (static_cast<u64>(input_[13]) << 32);
    counter += 4;
    input_[12] = static_cast<u32>(counter);
    input_[13] = static_cast<u32>(counter >> 32);
    position_ = 0;
}

void ChaCha12Stream::fill(const MutableView<Byte> output) noexcept {
    usize filled = 0;
    while (filled < output.count()) {
        const u64 value = next();
        const usize remaining = output.count() - filled;
        const usize count = remaining < 8 ? remaining : 8;
        for (usize byte = 0; byte < count; ++byte) output[filled + byte] = static_cast<Byte>(value >> (8 * byte));
        filled += count;
    }
}

Xoshiro256PlusPlus::Xoshiro256PlusPlus(const Seed& seed) noexcept {
    for (usize index = 0; index < 4; ++index) state_[index] = seed.words[index];
    const bool all_zero = state_[0] == 0 && state_[1] == 0 && state_[2] == 0 && state_[3] == 0;
    if (all_zero) {
        const Seed replacement = seed_from_number(0);
        for (usize index = 0; index < 4; ++index) state_[index] = replacement.words[index];
    }
}

STXLIB_MODULAR u64 Xoshiro256PlusPlus::next() noexcept {
    const u64 result = std::rotl(state_[0] + state_[3], 23) + state_[0];
    const u64 shifted = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= shifted;
    state_[3] = std::rotl(state_[3], 45);
    return result;
}

// Applies a jump polynomial: the state becomes the XOR of the states at the
// polynomial's set bits.
void Xoshiro256PlusPlus::apply_jump(const u64 (&table)[4]) noexcept {
    u64 accumulated[4]{};
    for (const u64 word : table) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            if ((word & (static_cast<u64>(1) << bit)) != 0) {
                for (usize index = 0; index < 4; ++index) accumulated[index] ^= state_[index];
            }
            static_cast<void>(next());
        }
    }
    for (usize index = 0; index < 4; ++index) state_[index] = accumulated[index];
}

void Xoshiro256PlusPlus::jump() noexcept { apply_jump(jump_table); }

void Xoshiro256PlusPlus::long_jump() noexcept { apply_jump(long_jump_table); }

} // namespace stxlib::random
