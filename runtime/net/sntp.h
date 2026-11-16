// sntp.h: one SNTP query at boot (C2; owner, roadmap item 25: HTTPS with
// one SNTP query at boot, so certificates can be checked against a real
// date). RFC 4330 section 5, client side, unicast.
//
// Pure code: no system call, no operating-system header, no allocation.
// stxsntp.cpp sends the request and sets the clock; this builds the request
// and judges the reply. Tested on the host (test/test_sntp.cpp), the time
// conversions exhaustively over all 2^32 seconds values and all 2^32
// fractions.
//
// The request is 48 bytes: LI 0, VN 4, mode 3 (first byte 0x23), all else
// zero but the transmit timestamp, which carries 64 random bits from the
// caller (RFC 4330 section 6 allows any value there; a random one makes an
// off-path forgery guess 64 bits). A reply is accepted only when:
// - it is at least 48 bytes;
// - mode 4 (server), VN 3 or 4, LI not 3 (clock not synchronized);
// - stratum 1..15 (0 is a kiss-o'-death, refused);
// - its originate timestamp equals the request's transmit timestamp;
// - its transmit timestamp is not zero.
//
// NTP seconds wrap every 2^32 s (era 0 ends in 2036). The era is chosen so
// that the time falls in [time_pivot_unix, time_pivot_unix + 2^32): an
// appliance built after the pivot never needs an earlier date.

#ifndef STXLIB_RUNTIME_NET_SNTP_H
#define STXLIB_RUNTIME_NET_SNTP_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace stxnet {

constexpr std::size_t sntp_packet_size = 48;
constexpr std::uint16_t sntp_port = 123;
// Seconds from 1900-01-01 (NTP's epoch) to 1970-01-01 (Unix's).
constexpr std::int64_t ntp_unix_offset_s = 2208988800;
// 2026-01-01T00:00:00Z: no accepted time is earlier.
constexpr std::int64_t time_pivot_unix = 1767225600;

// The request for nonce; request must be sntp_packet_size bytes.
void build_sntp_request(std::uint64_t nonce, std::span<std::uint8_t, sntp_packet_size> request) noexcept;

struct SntpTime {
    // Unix seconds and nanoseconds of the server's transmit timestamp.
    std::int64_t seconds;
    std::uint32_t nanoseconds;
};

enum class SntpRefusal : std::uint8_t {
    too_short,
    not_server_mode,
    version_unsupported,
    not_synchronized,
    kiss_of_death,
    stratum_invalid,
    nonce_mismatch,
    transmit_zero,
};

class SntpOutcome {
public:
    [[nodiscard]] static SntpOutcome accepted(const SntpTime& time) noexcept;
    [[nodiscard]] static SntpOutcome refused(SntpRefusal reason) noexcept;

    [[nodiscard]] const SntpTime* as_accepted() const noexcept;
    [[nodiscard]] const SntpRefusal* as_refused() const noexcept;

private:
    using Alternatives = std::variant<SntpRefusal, SntpTime>;
    explicit SntpOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

[[nodiscard]] const char* sntp_refusal_name(SntpRefusal reason) noexcept;

[[nodiscard]] SntpOutcome read_sntp_reply(std::span<const std::uint8_t> reply, std::uint64_t nonce) noexcept;

// The Unix seconds for an NTP seconds value, in the pivot's window.
[[nodiscard]] std::int64_t unix_seconds_of(std::uint32_t ntp_seconds) noexcept;
// floor(fraction * 10^9 / 2^32): the nanoseconds of an NTP fraction.
[[nodiscard]] std::uint32_t nanoseconds_of(std::uint32_t fraction) noexcept;

// The time to set: the server's transmit time plus half the round trip
// (round_trip_ns, measured on the monotonic clock from send to receive),
// normalized (nanoseconds below 10^9). round_trip_ns is at most
// sntp_round_trip_max_ns; stxsntp discards slower replies.
constexpr std::uint64_t sntp_round_trip_max_ns = 10000000000u;
[[nodiscard]] SntpTime time_to_set(const SntpTime& transmit, std::uint64_t round_trip_ns) noexcept;

}  // namespace stxnet

#endif  // STXLIB_RUNTIME_NET_SNTP_H
