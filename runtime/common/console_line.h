// console_line.h: what the appliance's first programs (stxinit,
// stxsupervisor) share for C1: a fixed-size text line, its writing to a
// descriptor, and the boot-time readings their markers carry.
//
// Hosted on FreeBSD's libc for C1; it moves to STXlib's own runtime in C2.
// Header-only; every function is noexcept and allocates nothing.

#ifndef STXLIB_RUNTIME_COMMON_CONSOLE_LINE_H
#define STXLIB_RUNTIME_COMMON_CONSOLE_LINE_H

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <time.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "console_line.h reads the Arm generic timer; other architectures need their own counter read"
#endif

namespace stxruntime {

// ---------------------------------------------------------------------------
// Text: a fixed buffer filled left to right. Every append checks capacity
// first; an append that does not fit writes nothing and marks the line
// truncated, so a truncated line is never mistaken for a complete one.

constexpr std::size_t line_capacity_bytes = 160;

struct LineText {
    char bytes[line_capacity_bytes];
    std::size_t length;
    bool truncated;
};

[[nodiscard]] inline LineText empty_line() noexcept {
    LineText line{};
    line.length = 0;
    line.truncated = false;
    return line;
}

inline void append_text(LineText& line, const char* text) noexcept {
    std::size_t text_length = 0;
    while (text[text_length] != '\0') {
        text_length += 1;
    }
    if (line.truncated || text_length > line_capacity_bytes - line.length) {
        line.truncated = true;
        return;
    }
    for (std::size_t index = 0; index < text_length; index += 1) {
        line.bytes[line.length + index] = text[index];
    }
    line.length += text_length;
}

// The largest std::uint64_t, 18446744073709551615, has 20 decimal digits.
constexpr std::size_t uint64_decimal_digits_max = 20;

inline void append_decimal(LineText& line, std::uint64_t value) noexcept {
    char digits[uint64_decimal_digits_max + 1];
    std::size_t count = 0;
    std::uint64_t remaining = value;
    do {
        digits[uint64_decimal_digits_max - 1 - count] =
            static_cast<char>('0' + static_cast<int>(remaining % 10u));
        remaining /= 10u;
        count += 1;
    } while (remaining != 0u);
    digits[uint64_decimal_digits_max] = '\0';
    append_text(line, &digits[uint64_decimal_digits_max - count]);
}

// A NUL-terminated copy of the line's text, for an argument vector. The
// destination holds line_capacity_bytes + 1 characters. A truncated line
// gives an empty string, never a partial argument.
inline void copy_line_to_c_string(const LineText& line, char (&destination)[line_capacity_bytes + 1]) noexcept {
    std::size_t length = line.truncated ? 0 : line.length;
    for (std::size_t index = 0; index < length; index += 1) {
        destination[index] = line.bytes[index];
    }
    destination[length] = '\0';
}

// Writes the whole line to a descriptor, or as much as it accepts. A short
// write is continued; an interrupted write is retried a bounded number of
// times; any other failure abandons the line (there is nowhere else to
// report it). A negative descriptor writes nothing.
inline void write_line(int descriptor, const LineText& line) noexcept {
    if (descriptor < 0) {
        return;
    }
    constexpr int interrupted_retries_max = 16;
    int interrupted_retries = 0;
    std::size_t written = 0;
    while (written < line.length) {
        const ssize_t result = write(descriptor, &line.bytes[written], line.length - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR && interrupted_retries < interrupted_retries_max) {
            interrupted_retries += 1;
            continue;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Readings for the markers.

struct GenericTimer {
    std::uint64_t count;
    std::uint64_t frequency_hertz;
};

// FreeBSD lets user mode read the virtual counter and its frequency
// (CNTKCTL_EL1.EL0VCTEN, set by sys/arm/arm/generic_timer.c), as its vDSO
// time functions do. The isb orders the counter read after the instructions
// before it.
[[nodiscard]] inline GenericTimer read_generic_timer() noexcept {
    GenericTimer timer{};
    std::uint64_t count = 0;
    std::uint64_t frequency = 0;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(count) : : "memory");
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
    timer.count = count;
    timer.frequency_hertz = frequency;
    return timer;
}

// A clock reading in nanoseconds, or nothing when the clock cannot be read.
using ClockNanoseconds = std::optional<std::uint64_t>;

constexpr std::uint64_t nanoseconds_per_second = 1000000000u;
constexpr std::uint64_t nanoseconds_per_millisecond = 1000000u;

// clock_gettime for a clock whose readings are never negative (CLOCK_UPTIME,
// CLOCK_MONOTONIC). tv_sec * 1e9 fits in 64 bits below 584 years.
[[nodiscard]] inline ClockNanoseconds read_clock(clockid_t clock) noexcept {
    struct timespec now{};
    if (clock_gettime(clock, &now) != 0) {
        return std::nullopt;
    }
    if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= static_cast<long>(nanoseconds_per_second)) {
        return std::nullopt;
    }
    const std::uint64_t seconds = static_cast<std::uint64_t>(now.tv_sec);
    const std::uint64_t nanoseconds = seconds * nanoseconds_per_second + static_cast<std::uint64_t>(now.tv_nsec);
    const ClockNanoseconds reading = nanoseconds;
    return reading;
}

// Appends " uptime_ns=<n> cntvct=<n> cntfrq=<n>" (uptime_ns=failed when the
// clock could not be read).
inline void append_boot_readings(LineText& line, const ClockNanoseconds& uptime, const GenericTimer& timer) noexcept {
    append_text(line, " uptime_ns=");
    if (uptime.has_value()) {
        append_decimal(line, *uptime);
    } else {
        append_text(line, "failed");
    }
    append_text(line, " cntvct=");
    append_decimal(line, timer.count);
    append_text(line, " cntfrq=");
    append_decimal(line, timer.frequency_hertz);
}

}  // namespace stxruntime

#endif  // STXLIB_RUNTIME_COMMON_CONSOLE_LINE_H
