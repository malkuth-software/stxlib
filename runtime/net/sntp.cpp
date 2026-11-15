// sntp.cpp: see sntp.h.

#include "sntp.h"

namespace stxnet {

SntpOutcome::SntpOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

SntpOutcome SntpOutcome::accepted(const SntpTime& time) noexcept {
    return SntpOutcome(Alternatives(time));
}

SntpOutcome SntpOutcome::refused(SntpRefusal reason) noexcept {
    return SntpOutcome(Alternatives(reason));
}

const SntpTime* SntpOutcome::as_accepted() const noexcept {
    return std::get_if<SntpTime>(&alternatives_);
}

const SntpRefusal* SntpOutcome::as_refused() const noexcept {
    return std::get_if<SntpRefusal>(&alternatives_);
}

const char* sntp_refusal_name(SntpRefusal reason) noexcept {
    switch (reason) {
    case SntpRefusal::too_short: return "too_short";
    case SntpRefusal::not_server_mode: return "not_server_mode";
    case SntpRefusal::version_unsupported: return "version_unsupported";
    case SntpRefusal::not_synchronized: return "not_synchronized";
    case SntpRefusal::kiss_of_death: return "kiss_of_death";
    case SntpRefusal::stratum_invalid: return "stratum_invalid";
    case SntpRefusal::nonce_mismatch: return "nonce_mismatch";
    case SntpRefusal::transmit_zero: return "transmit_zero";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

// Packet offsets (RFC 4330 section 4).
constexpr std::size_t stratum_offset = 1;
constexpr std::size_t originate_offset = 24;
constexpr std::size_t transmit_offset = 40;

[[nodiscard]] std::uint64_t big_endian_64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8u; index += 1) {
        value = (value << 8u) | static_cast<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

}  // namespace

void build_sntp_request(std::uint64_t nonce, std::span<std::uint8_t, sntp_packet_size> request) noexcept {
    for (std::size_t index = 0; index < sntp_packet_size; index += 1) {
        request[index] = 0;
    }
    // LI 0 (bits 7-6), VN 4 (bits 5-3), mode 3 (bits 2-0).
    request[0] = 0x23u;
    for (std::size_t index = 0; index < 8u; index += 1) {
        request[transmit_offset + index] = static_cast<std::uint8_t>(nonce >> (56u - 8u * index));
    }
}

std::int64_t unix_seconds_of(std::uint32_t ntp_seconds) noexcept {
    // The pivot as an NTP seconds value, modulo 2^32; the distance from it
    // modulo 2^32 is in [0, 2^32).
    const std::uint32_t pivot_ntp = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(time_pivot_unix + ntp_unix_offset_s) & 0xffffffffu);
    const std::uint32_t distance = ntp_seconds - pivot_ntp;
    return time_pivot_unix + static_cast<std::int64_t>(distance);
}

std::uint32_t nanoseconds_of(std::uint32_t fraction) noexcept {
    // fraction * 10^9 < 2^32 * 2^30 = 2^62: no overflow.
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(fraction) * 1000000000u) >> 32u);
}

SntpOutcome read_sntp_reply(std::span<const std::uint8_t> reply, std::uint64_t nonce) noexcept {
    if (reply.size() < sntp_packet_size) {
        return SntpOutcome::refused(SntpRefusal::too_short);
    }
    const std::uint8_t first = reply[0];
    const std::uint32_t leap = static_cast<std::uint32_t>(first >> 6u) & 0x3u;
    const std::uint32_t version = static_cast<std::uint32_t>(first >> 3u) & 0x7u;
    const std::uint32_t mode = static_cast<std::uint32_t>(first) & 0x7u;
    if (mode != 4u) {
        return SntpOutcome::refused(SntpRefusal::not_server_mode);
    }
    if (version != 3u && version != 4u) {
        return SntpOutcome::refused(SntpRefusal::version_unsupported);
    }
    if (leap == 3u) {
        return SntpOutcome::refused(SntpRefusal::not_synchronized);
    }
    const std::uint8_t stratum = reply[stratum_offset];
    if (stratum == 0u) {
        return SntpOutcome::refused(SntpRefusal::kiss_of_death);
    }
    if (stratum > 15u) {
        return SntpOutcome::refused(SntpRefusal::stratum_invalid);
    }
    if (big_endian_64(reply, originate_offset) != nonce) {
        return SntpOutcome::refused(SntpRefusal::nonce_mismatch);
    }
    const std::uint64_t transmit = big_endian_64(reply, transmit_offset);
    if (transmit == 0u) {
        return SntpOutcome::refused(SntpRefusal::transmit_zero);
    }
    SntpTime time{};
    time.seconds = unix_seconds_of(static_cast<std::uint32_t>(transmit >> 32u));
    time.nanoseconds = nanoseconds_of(static_cast<std::uint32_t>(transmit & 0xffffffffu));
    return SntpOutcome::accepted(time);
}

SntpTime time_to_set(const SntpTime& transmit, std::uint64_t round_trip_ns) noexcept {
    const std::uint64_t bounded = round_trip_ns < sntp_round_trip_max_ns ? round_trip_ns : sntp_round_trip_max_ns;
    // Below 10^9 + 5 * 10^9: inside 64 bits.
    const std::uint64_t nanoseconds = static_cast<std::uint64_t>(transmit.nanoseconds) + bounded / 2u;
    SntpTime result{};
    result.seconds = transmit.seconds + static_cast<std::int64_t>(nanoseconds / 1000000000u);
    result.nanoseconds = static_cast<std::uint32_t>(nanoseconds % 1000000000u);
    return result;
}

}  // namespace stxnet
