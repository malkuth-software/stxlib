// test_sntp.cpp: host tests of the SNTP request and reply.
//
// Exhaustive:
// - unix_seconds_of over all 2^32 NTP seconds values: the result lies in
//   [pivot, pivot + 2^32) and is congruent to the input plus the epoch
//   offset modulo 2^32 (so it is the one Unix time in the window with that
//   NTP spelling);
// - nanoseconds_of over all 2^32 fractions: n * 2^32 <= f * 10^9 <
//   (n + 1) * 2^32 (the floor), hence n < 10^9;
// - read_sntp_reply over all 65,536 pairs of first byte and stratum,
//   against the acceptance rule written out separately.
// Examples cover the nonce, the transmit timestamp and time_to_set.

#include <cstdint>
#include <cstdio>
#include <span>

#include "../sntp.h"
#include "../../supervisor/config/test/test_support.h"

namespace {

using stxnet::SntpOutcome;
using stxnet::SntpRefusal;
using stxtest::check;

constexpr std::uint64_t nonce = 0x0123456789abcdefu;

void put_64(std::uint8_t* bytes, std::uint64_t value) {
    for (std::size_t index = 0; index < 8u; index += 1) {
        bytes[index] = static_cast<std::uint8_t>(value >> (56u - 8u * index));
    }
}

// A reply: first byte, stratum, originate = nonce, transmit as given.
void make_reply(std::uint8_t (&reply)[48], std::uint8_t first, std::uint8_t stratum, std::uint64_t transmit) {
    for (std::uint8_t& byte : reply) {
        byte = 0;
    }
    reply[0] = first;
    reply[1] = stratum;
    put_64(&reply[24], nonce);
    put_64(&reply[40], transmit);
}

void test_request() {
    const char* const test = "sntp request";
    std::uint8_t request[48];
    for (std::uint8_t& byte : request) {
        byte = 0xffu;
    }
    stxnet::build_sntp_request(nonce, std::span<std::uint8_t, 48>(request));
    bool rest_zero = true;
    for (std::size_t index = 1; index < 40u; index += 1) {
        rest_zero = rest_zero && request[index] == 0u;
    }
    check(request[0] == 0x23u && rest_zero, test, "0x23, then zeros");
    check(request[40] == 0x01u && request[41] == 0x23u && request[46] == 0xcdu && request[47] == 0xefu, test,
          "the nonce as the transmit timestamp, big-endian");
}

void test_examples() {
    const char* const test = "sntp reply";
    std::uint8_t reply[48];
    // 2026-11-15T00:00:00Z = Unix 1794700800; NTP = + 2208988800 (mod 2^32).
    const std::uint64_t ntp_seconds = (1794700800u + 2208988800u) & 0xffffffffu;
    make_reply(reply, 0x24u, 2u, (ntp_seconds << 32u) | 0x80000000u);
    const SntpOutcome good = stxnet::read_sntp_reply(std::span<const std::uint8_t>(reply), nonce);
    check(good.as_accepted() != nullptr && (*good.as_accepted()).seconds == 1794700800 &&
              (*good.as_accepted()).nanoseconds == 500000000u,
          test, "a 2026 time read in the window, half a second");
    const SntpOutcome wrong = stxnet::read_sntp_reply(std::span<const std::uint8_t>(reply), nonce + 1u);
    check(wrong.as_refused() != nullptr && *wrong.as_refused() == SntpRefusal::nonce_mismatch, test,
          "another nonce");
    const SntpOutcome short_reply = stxnet::read_sntp_reply(std::span<const std::uint8_t>(reply, 47), nonce);
    check(short_reply.as_refused() != nullptr && *short_reply.as_refused() == SntpRefusal::too_short, test,
          "47 bytes");
    make_reply(reply, 0x24u, 2u, 0);
    const SntpOutcome zero = stxnet::read_sntp_reply(std::span<const std::uint8_t>(reply), nonce);
    check(zero.as_refused() != nullptr && *zero.as_refused() == SntpRefusal::transmit_zero, test, "transmit zero");
    // 2036-02-07T06:28:16Z: NTP era 1 begins; seconds value 0.
    check(stxnet::unix_seconds_of(0) == 2085978496, test, "NTP seconds 0 is 2036 in the window");
    check(stxnet::unix_seconds_of(static_cast<std::uint32_t>((1767225600u + 2208988800u) & 0xffffffffu)) ==
              1767225600,
          test, "the pivot itself");
    const stxnet::SntpTime transmit{1794700800, 900000000u};
    const stxnet::SntpTime set = stxnet::time_to_set(transmit, 400000000u);
    check(set.seconds == 1794700801 && set.nanoseconds == 100000000u, test, "half the round trip, carried");
    const stxnet::SntpTime capped = stxnet::time_to_set(transmit, UINT64_MAX);
    check(capped.seconds == 1794700805 && capped.nanoseconds == 900000000u, test, "the round trip capped at 10 s");
}

void test_exhaustive_seconds() {
    constexpr std::int64_t window = std::int64_t{1} << 32;
    std::uint64_t failures = 0;
    std::uint64_t values = 0;
    std::uint32_t ntp = 0;
    do {
        const std::int64_t unix_time = stxnet::unix_seconds_of(ntp);
        const bool inside = unix_time >= stxnet::time_pivot_unix && unix_time < stxnet::time_pivot_unix + window;
        const bool congruent =
            (static_cast<std::uint64_t>(unix_time + stxnet::ntp_unix_offset_s) & 0xffffffffu) == ntp;
        failures += (inside && congruent) ? 0u : 1u;
        values += 1;
        ntp += 1;
    } while (ntp != 0u);
    std::printf("exhaustive seconds: %llu values, %llu failures\n", static_cast<unsigned long long>(values),
                static_cast<unsigned long long>(failures));
    check(failures == 0u && values == (std::uint64_t{1} << 32), "exhaustive seconds",
          "every value in the window, congruent");
}

void test_exhaustive_fractions() {
    std::uint64_t failures = 0;
    std::uint64_t values = 0;
    std::uint32_t fraction = 0;
    do {
        const std::uint64_t n = stxnet::nanoseconds_of(fraction);
        const std::uint64_t scaled = static_cast<std::uint64_t>(fraction) * 1000000000u;
        const bool floor = (n << 32u) <= scaled && scaled < ((n + 1u) << 32u) && n < 1000000000u;
        failures += floor ? 0u : 1u;
        values += 1;
        fraction += 1;
    } while (fraction != 0u);
    std::printf("exhaustive fractions: %llu values, %llu failures\n", static_cast<unsigned long long>(values),
                static_cast<unsigned long long>(failures));
    check(failures == 0u && values == (std::uint64_t{1} << 32), "exhaustive fractions", "every value floored");
}

void test_exhaustive_header() {
    std::uint8_t reply[48];
    std::size_t values = 0;
    std::size_t accepted = 0;
    for (std::uint32_t first = 0; first < 256u; first += 1) {
        for (std::uint32_t stratum = 0; stratum < 256u; stratum += 1) {
            make_reply(reply, static_cast<std::uint8_t>(first), static_cast<std::uint8_t>(stratum),
                       std::uint64_t{1} << 40u);
            const std::uint32_t leap = first / 64u;
            const std::uint32_t version = (first / 8u) % 8u;
            const std::uint32_t mode = first % 8u;
            const bool expected =
                mode == 4u && (version == 3u || version == 4u) && leap != 3u && stratum >= 1u && stratum <= 15u;
            const SntpOutcome outcome = stxnet::read_sntp_reply(std::span<const std::uint8_t>(reply), nonce);
            if ((outcome.as_accepted() != nullptr) != expected) {
                check(false, "exhaustive header", "as the rule");
            }
            accepted += expected ? 1u : 0u;
            values += 1;
        }
    }
    std::printf("exhaustive header: %zu values, %zu accepted\n", values, accepted);
    // 2 versions x 3 leap values x 15 strata.
    check(values == 65536u && accepted == 90u, "exhaustive header", "the stated domain, 90 accepted");
}

}  // namespace

int main() {
    test_request();
    test_examples();
    test_exhaustive_header();
    test_exhaustive_seconds();
    test_exhaustive_fractions();
    return stxtest::finish("test_sntp");
}
