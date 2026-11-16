// Tests of stxlib::random (include/stxlib/random.hpp). Ported from
// stx_auxlib's tests/random_tests.cpp at b6e330a with the same expected
// values, which come from tests/reference/random_reference.py, an
// independent implementation of the published algorithms; its ChaCha core
// reproduces the RFC 8439 section 2.3.2 ChaCha20 block, checked here too.
// Known answers are exact; the uniformity check is sampled.

#include "stxlib/random.hpp"

#include "test_support.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace {

namespace random = stxlib::random;
using stxlib::u32;
using stxlib::u64;

template <stxlib::usize Count>
bool same(const u64 (&left)[Count], const u64 (&right)[Count]) {
    for (stxlib::usize index = 0; index < Count; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

void chacha20_known_answer() {
    u32 input[16] = {0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U};
    for (u32 word = 0; word < 8; ++word) {
        const u32 base = word * 4;
        input[4 + word] = base | (base + 1) << 8 | (base + 2) << 16 | (base + 3) << 24;
    }
    input[12] = 1;
    input[13] = 0x09000000U;
    input[14] = 0x4a000000U;
    input[15] = 0;
    const u32 expected[16] = {0xe4e7f110U, 0x15593bd1U, 0x1fdd0f50U, 0xc47120a3U, 0xc7f4d1c7U, 0x0368c033U,
                              0x9aaa2204U, 0x4e6cd4c3U, 0x466482d2U, 0x09aa9f07U, 0x05d7c214U, 0xa2028bd9U,
                              0xd19c12b5U, 0xb94e16deU, 0xe883d0cbU, 0x4e3c50a2U};
    u32 output[16]{};
    random::detail::chacha_block(input, 10, output);
    bool equal = true;
    for (stxlib::usize index = 0; index < 16; ++index) equal = equal && output[index] == expected[index];
    CHECK(equal);
}

void seed_expansion() {
    const random::Seed seed = random::seed_from_number(42);
    const u64 expected[4] = {0xbdd732262feb6e95ULL, 0x28efe333b266f103ULL, 0x47526757130f9f52ULL,
                             0x581ce1ff0e4ae394ULL};
    CHECK(same(seed.words, expected));
}

void chacha12_stream() {
    const random::Seed seed = random::seed_from_number(42);
    random::ChaCha12Stream generator(seed);
    const u64 first[8] = {0x280b7b79f392fa12ULL, 0x4dadef83bc931d07ULL, 0xc195c99ba5375e5fULL,
                          0x7e657f1b6bdc3bfdULL, 0xfe40a244bc14b82fULL, 0x3dd75b637ba65c81ULL,
                          0x91c8dff96cfcd24aULL, 0xcb61b56a793c1223ULL};
    for (const u64 value : first) CHECK(generator.next() == value);
    for (int index = 8; index < 999; ++index) static_cast<void>(generator.next());
    CHECK(generator.next() == 0xd04aeb60bd0b72f6ULL);

    random::ChaCha12Stream other_stream(seed, 7);
    const u64 seventh[4] = {0xe48b814dc75d8cf8ULL, 0x55c9dc5976130508ULL, 0xf25fe228aa0e1a5bULL,
                            0x1b7d08372c59e3a8ULL};
    for (const u64 value : seventh) CHECK(other_stream.next() == value);

    // fill() is next() as little-endian bytes; a partial output is dropped.
    random::ChaCha12Stream bytes_generator(seed);
    stxlib::Byte bytes[12]{};
    bytes_generator.fill(stxlib::MutableView<stxlib::Byte>(bytes));
    for (stxlib::usize index = 0; index < 8; ++index) {
        CHECK(bytes[index] == static_cast<stxlib::Byte>(first[0] >> (8 * index)));
    }
    for (stxlib::usize index = 0; index < 4; ++index) {
        CHECK(bytes[8 + index] == static_cast<stxlib::Byte>(first[1] >> (8 * index)));
    }
    CHECK(bytes_generator.next() == first[2]);
}

void xoshiro() {
    const random::Seed seed = random::seed_from_number(42);
    random::Xoshiro256PlusPlus generator(seed);
    const u64 first[8] = {0xd0764d4f4476689fULL, 0x519e4174576f3791ULL, 0xfbe07cfb0c24ed8cULL,
                          0xb37d9f600cd835b8ULL, 0xcb231c3874846a73ULL, 0x968d9f004e50de7dULL,
                          0x201718ff221a3556ULL, 0x9ae94e070ed8cb46ULL};
    for (const u64 value : first) CHECK(generator.next() == value);
    random::Xoshiro256PlusPlus jumped(seed);
    jumped.jump();
    CHECK(jumped.next() == 0xc0b6f4be293b1ae5ULL);
    CHECK(jumped.next() == 0x5db3dd9683e7bb33ULL);
    random::Xoshiro256PlusPlus far(seed);
    far.long_jump();
    CHECK(far.next() == 0x02019a87bfc0bb07ULL);
    CHECK(far.next() == 0x25bee49209717963ULL);
    // An all-zero seed, which xoshiro cannot leave, is replaced.
    random::Xoshiro256PlusPlus zero(random::Seed{});
    random::Xoshiro256PlusPlus replacement(random::seed_from_number(0));
    const u64 zero_first = zero.next();
    CHECK(zero_first == replacement.next());
    CHECK(zero.next() != 0);
}

void product() {
    const random::detail::Product largest = random::detail::multiply(stxlib::u64_max, stxlib::u64_max);
    CHECK(largest.high == 0xfffffffffffffffeULL && largest.low == 1);
    const random::detail::Product mixed = random::detail::multiply(0x123456789abcdef0ULL, 0x0fedcba987654321ULL);
    CHECK(mixed.high == 0x0121fa00ad77d742ULL && mixed.low == 0x2236d88fe5618cf0ULL);
}

void distributions() {
    const random::Seed seed = random::seed_from_number(42);
    random::ChaCha12Stream dice_generator(seed);
    const u64 dice[10] = {0, 1, 4, 2, 5, 1, 3, 4, 1, 4};
    for (const u64 face : dice) CHECK(random::uniform_below(dice_generator, 6) == face);

    random::ChaCha12Stream shuffle_generator(seed);
    int deck[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    random::shuffle(shuffle_generator, stxlib::MutableView<int>(deck));
    const int shuffled[10] = {4, 0, 7, 8, 9, 5, 3, 6, 2, 1};
    bool deck_matches = true;
    for (stxlib::usize index = 0; index < 10; ++index) deck_matches = deck_matches && deck[index] == shuffled[index];
    CHECK(deck_matches);

    // Uniformity (sampled): 600,000 rolls of a die, each face within 1% of
    // 100,000.
    random::Xoshiro256PlusPlus counter_generator(seed);
    int counts[6]{};
    for (int roll = 0; roll < 600000; ++roll) {
        ++counts[static_cast<stxlib::usize>(random::uniform_below(counter_generator, 6))];
    }
    for (const int count : counts) CHECK(count > 99000 && count < 101000);

    CHECK(random::uniform_below(counter_generator, 1) == 0);
    CHECK(random::uniform_below(counter_generator, 0) == 0);
    const u64 huge_bound = (static_cast<u64>(1) << 63) + 1;
    bool below = true;
    for (int draw = 0; draw < 1000; ++draw) below = below && random::uniform_below(counter_generator, huge_bound) < huge_bound;
    CHECK(below);

    bool in_range = true;
    for (int draw = 0; draw < 10000; ++draw) {
        const stxlib::i64 value = random::uniform_between(counter_generator, -3, 3);
        in_range = in_range && value >= -3 && value <= 3;
    }
    CHECK(in_range);
    static_cast<void>(random::uniform_between(counter_generator, stxlib::i64_min, stxlib::i64_max));
    CHECK(random::uniform_between(counter_generator, 5, 5) == 5);

    bool units = true;
    for (int draw = 0; draw < 10000; ++draw) {
        const double unit = random::uniform_unit(counter_generator);
        units = units && unit >= 0.0 && unit < 1.0 && unit * 0x1.0p53 == std::floor(unit * 0x1.0p53);
        const float unit_float = random::uniform_unit_float(counter_generator);
        units = units && unit_float >= 0.0f && unit_float < 1.0f;
        const double real = random::uniform_real(counter_generator, -2.5, 7.25);
        units = units && real >= -2.5 && real < 7.25;
        const double wide = random::uniform_real(counter_generator, -std::numeric_limits<double>::max(),
                                                 std::numeric_limits<double>::max());
        units = units && std::isfinite(wide);
    }
    CHECK(units);
    CHECK(random::uniform_real(counter_generator, 1.0, 1.0) == 1.0);
    CHECK(!random::chance(counter_generator, 0.0));
    CHECK(random::chance(counter_generator, 1.0));

    // STXlib batch 3c9: no subnormals in or out of scale_into.
    const double units_to_try[] = {0.0, 0x1p-53, 0.25, 0.5, 0.75, 1.0 - 0x1p-53};
    bool flushed_ok = true;
    for (const double unit : units_to_try) {
        const double bottom = random::detail::scale_into(0.0, 0x1p-1022, unit);
        flushed_ok = flushed_ok && std::bit_cast<u64>(bottom) == 0;
        const double straddle = random::detail::scale_into(-0x1p-1020, 0x1p-1020, unit);
        flushed_ok = flushed_ok && std::fpclassify(straddle) != FP_SUBNORMAL && straddle >= -0x1p-1020 &&
                     straddle < 0x1p-1020;
        const double below_zero = random::detail::scale_into(-1.0, -0x1p-1074, unit);
        flushed_ok = flushed_ok && std::fpclassify(below_zero) != FP_SUBNORMAL && below_zero >= -1.0 &&
                     below_zero < 0.0;
    }
    CHECK(flushed_ok);
    CHECK(std::bit_cast<u64>(random::detail::scale_into(0x1p-1074, 1.0, 0.0)) == 0);
    CHECK(std::bit_cast<u64>(random::detail::scale_into(-0x1p-1074, 0x1p-1074, 0.5)) == 0x8000000000000000ULL);
}

void entropy() {
    const stxlib::OperationResult<random::Seed> first = random::entropy_seed();
    const stxlib::OperationResult<random::Seed> second = random::entropy_seed();
    CHECK(first.succeeded() && second.succeeded());
    if (first.succeeded() && second.succeeded()) CHECK(!same((*first.value()).words, (*second.value()).words));
}

} // namespace

int main() {
    chacha20_known_answer();
    seed_expansion();
    chacha12_stream();
    xoshiro();
    product();
    distributions();
    entropy();
    return stxlib_tests::finish("stxlib random");
}
