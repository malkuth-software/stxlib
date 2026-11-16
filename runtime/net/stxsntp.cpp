// stxsntp.cpp: sets the clock once at boot from one SNTP server (C2; owner,
// roadmap item 25). A service of the factory configuration:
//
//   [service-clock]
//   command = /sbin/stxsntp pool.ntp.org
//   restart = never
//
// It resolves the server, sends one request (sntp.h) and waits
// reply_timeout_ms for the reply; on any failure it tries again, waiting
// 2 s, then twice as long each time up to 64 s, for at most
// attempts_window_s in all (the network may still be coming up). The
// first accepted reply sets CLOCK_REALTIME to the server's transmit time
// plus half the round trip, and the program prints
//
//   STXSNTP set unix=<s> step_ms=<signed> round_trip_ms=<n>
//
// and exits 0; after the window it prints "STXSNTP failed <reason>" and
// exits 1. Securelevel 1 permits the step (kern_time.c limits steps only
// above 1). Nothing listens: the socket is connected to the server's
// address and port and closed after the attempt.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sntp.h"

namespace {

constexpr int reply_timeout_ms = 2000;
constexpr std::uint32_t attempts_window_s = 600;
constexpr std::uint32_t retry_first_s = 2;
constexpr std::uint32_t retry_max_s = 64;

[[nodiscard]] std::uint64_t monotonic_ns() noexcept {
    struct timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000u + static_cast<std::uint64_t>(now.tv_nsec);
}

// One attempt: the reason it failed, or nullptr when the clock was set.
[[nodiscard]] const char* attempt(const char* server) noexcept {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    struct addrinfo* addresses = nullptr;
    if (getaddrinfo(server, "123", &hints, &addresses) != 0 || addresses == nullptr) {
        return "resolve";
    }
    const int socket_descriptor =
        socket((*addresses).ai_family, (*addresses).ai_socktype | SOCK_CLOEXEC, (*addresses).ai_protocol);
    if (socket_descriptor < 0) {
        freeaddrinfo(addresses);
        return "socket";
    }
    const bool connected = connect(socket_descriptor, (*addresses).ai_addr, (*addresses).ai_addrlen) == 0;
    freeaddrinfo(addresses);
    if (!connected) {
        (void)close(socket_descriptor);
        return "connect";
    }
    std::uint64_t nonce = 0;
    arc4random_buf(&nonce, sizeof(nonce));
    std::uint8_t request[stxnet::sntp_packet_size] = {};
    stxnet::build_sntp_request(nonce, std::span<std::uint8_t, stxnet::sntp_packet_size>(request));
    const std::uint64_t sent_ns = monotonic_ns();
    if (send(socket_descriptor, request, sizeof(request), 0) != static_cast<ssize_t>(sizeof(request))) {
        (void)close(socket_descriptor);
        return "send";
    }
    struct pollfd wait_for{};
    wait_for.fd = socket_descriptor;
    wait_for.events = POLLIN;
    if (poll(&wait_for, 1, reply_timeout_ms) != 1) {
        (void)close(socket_descriptor);
        return "timeout";
    }
    std::uint8_t reply[128] = {};
    const ssize_t got = recv(socket_descriptor, reply, sizeof(reply), 0);
    const std::uint64_t received_ns = monotonic_ns();
    (void)close(socket_descriptor);
    if (got < 0) {
        return "receive";
    }
    const stxnet::SntpOutcome outcome =
        stxnet::read_sntp_reply(std::span<const std::uint8_t>(reply, static_cast<std::size_t>(got)), nonce);
    const stxnet::SntpTime* const time = outcome.as_accepted();
    if (time == nullptr) {
        return stxnet::sntp_refusal_name(*outcome.as_refused());
    }
    const std::uint64_t round_trip_ns = received_ns >= sent_ns ? received_ns - sent_ns : 0u;
    if (round_trip_ns > stxnet::sntp_round_trip_max_ns) {
        return "round-trip-too-long";
    }
    const stxnet::SntpTime set = stxnet::time_to_set(*time, round_trip_ns);
    struct timespec before{};
    (void)clock_gettime(CLOCK_REALTIME, &before);
    struct timespec value{};
    value.tv_sec = static_cast<time_t>(set.seconds);
    value.tv_nsec = static_cast<long>(set.nanoseconds);
    if (clock_settime(CLOCK_REALTIME, &value) != 0) {
        return "settime";
    }
    const long long step_ms = (static_cast<long long>(set.seconds) - static_cast<long long>(before.tv_sec)) * 1000LL +
                              (static_cast<long long>(set.nanoseconds) - static_cast<long long>(before.tv_nsec)) /
                                  1000000LL;
    std::printf("STXSNTP set unix=%lld step_ms=%lld round_trip_ms=%llu\n", static_cast<long long>(set.seconds),
                step_ms, static_cast<unsigned long long>(round_trip_ns / 1000000u));
    (void)std::fflush(stdout);
    return nullptr;
}

}  // namespace

int main(int argument_count, char** arguments) {
    if (argument_count != 2) {
        std::printf("STXSNTP failed usage: stxsntp <server>\n");
        return 2;
    }
    const std::uint64_t started_ns = monotonic_ns();
    std::uint32_t wait_s = retry_first_s;
    const char* reason = "none";
    // Each pass is one attempt; the window bounds them (at most
    // attempts_window_s / retry_first_s passes).
    while (true) {
        reason = attempt(arguments[1]);
        if (reason == nullptr) {
            return 0;
        }
        const std::uint64_t elapsed_s = (monotonic_ns() - started_ns) / 1000000000u;
        if (elapsed_s + wait_s > attempts_window_s) {
            break;
        }
        (void)sleep(wait_s);
        wait_s = wait_s * 2u < retry_max_s ? wait_s * 2u : retry_max_s;
    }
    std::printf("STXSNTP failed %s\n", reason);
    return 1;
}
