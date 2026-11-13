#pragma once

// The program's clocks and its time values (ported from stx_auxlib's
// include/stx_auxlib/clocks.hpp at b6e330a, restated without std::chrono;
// the two steady clocks are owner decision D12 = a,
// decisions/2026-11-13-d11-d12-threads-clocks.md).
//
// IntervalTime::now() (Capability::interval_clock) and ElapsedTime::now()
// (Capability::elapsed_clock) never go backwards and are unaffected by
// changes to the wall clock. Their epochs are unspecified (typically boot),
// so their values are meaningful only relative to one another within one
// run, and the two are distinct types so that they cannot be mixed. They
// differ in suspension (sleep, hibernation):
//   - IntervalTime stops while the machine is suspended, so a span of it is
//     time the program could have run: use it for timeouts, waits, frame
//     pacing and measurement. Every timed wait in STXlib uses it. Linux
//     CLOCK_MONOTONIC, macOS CLOCK_UPTIME_RAW, Windows
//     QueryUnbiasedInterruptTimePrecise.
//   - ElapsedTime continues while the machine is suspended, so a span of it
//     is real time passed: use it for "how long ago" and expiry that must
//     count a night's sleep. Linux CLOCK_BOOTTIME, macOS CLOCK_MONOTONIC,
//     Windows QueryInterruptTimePrecise.
//   - FreeBSD has no clock that counts suspension: CLOCK_BOOTTIME is
//     defined as CLOCK_MONOTONIC (sys/sys/_clock_id.h:85 at the pin
//     fb9033d2876d), and CLOCK_MONOTONIC and CLOCK_UPTIME both read
//     nanouptime (sys/kern/kern_time.c:360-365). There IntervalTime reads
//     CLOCK_UPTIME, Capability::elapsed_clock is false, and
//     ElapsedTime::now() reads the same uptime: it still never goes
//     backwards, but a span of it does not include time suspended. Whether
//     uptime itself stops in suspension depends on the time counter
//     hardware (unconfirmed).
// IntervalTime replaces std::chrono::steady_clock (stx_auxlib's
// MonotonicTime, whose behaviour in suspension was the host's).
//
// WallTime::now() is Coordinated Universal Time as nanoseconds since
// 1970-01-01T00:00:00Z, ignoring leap seconds (Unix time). It can jump when
// the system time is set, so measure spans with IntervalTime or
// ElapsedTime. It replaces std::chrono::system_clock.
//
// All four types count signed 64-bit nanoseconds, about 292 years either
// side of the epoch. Arithmetic saturates at the ends of that range instead
// of wrapping, so a deadline far in the future stays in the future. The
// host's clocks may tick more coarsely (Windows' interrupt time counts
// 100 ns units); now() reports what the host provides, converted without
// rounding.

#include "stxlib/types.hpp"

namespace stxlib {

namespace detail {

[[nodiscard]] constexpr i64 saturating_add(const i64 left, const i64 right) noexcept {
    i64 result = 0;
    if (__builtin_add_overflow(left, right, &result)) return right < 0 ? i64_min : i64_max;
    return result;
}

[[nodiscard]] constexpr i64 saturating_subtract(const i64 left, const i64 right) noexcept {
    i64 result = 0;
    if (__builtin_sub_overflow(left, right, &result)) return right < 0 ? i64_max : i64_min;
    return result;
}

[[nodiscard]] constexpr i64 saturating_scale(const i64 count, const i64 factor) noexcept {
    i64 result = 0;
    if (__builtin_mul_overflow(count, factor, &result)) return count < 0 ? i64_min : i64_max;
    return result;
}

} // namespace detail

// A signed span of time in nanoseconds.
class Duration final {
public:
    constexpr Duration() noexcept = default;

    [[nodiscard]] static constexpr Duration nanoseconds(const i64 count) noexcept { return Duration(count); }
    [[nodiscard]] static constexpr Duration microseconds(const i64 count) noexcept {
        return Duration(detail::saturating_scale(count, 1000));
    }
    [[nodiscard]] static constexpr Duration milliseconds(const i64 count) noexcept {
        return Duration(detail::saturating_scale(count, 1000000));
    }
    [[nodiscard]] static constexpr Duration seconds(const i64 count) noexcept {
        return Duration(detail::saturating_scale(count, 1000000000));
    }
    // The longest representable duration.
    [[nodiscard]] static constexpr Duration longest() noexcept { return Duration(i64_max); }

    [[nodiscard]] constexpr i64 count_nanoseconds() const noexcept { return nanoseconds_; }
    // Whole milliseconds, rounded toward zero.
    [[nodiscard]] constexpr i64 count_milliseconds() const noexcept { return nanoseconds_ / 1000000; }
    [[nodiscard]] constexpr bool negative() const noexcept { return nanoseconds_ < 0; }

    [[nodiscard]] constexpr Duration plus(const Duration other) const noexcept {
        return Duration(detail::saturating_add(nanoseconds_, other.nanoseconds_));
    }
    [[nodiscard]] constexpr Duration minus(const Duration other) const noexcept {
        return Duration(detail::saturating_subtract(nanoseconds_, other.nanoseconds_));
    }

    [[nodiscard]] constexpr bool operator==(const Duration other) const noexcept {
        return nanoseconds_ == other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator!=(const Duration other) const noexcept {
        return nanoseconds_ != other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<(const Duration other) const noexcept {
        return nanoseconds_ < other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<=(const Duration other) const noexcept {
        return nanoseconds_ <= other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator>(const Duration other) const noexcept {
        return nanoseconds_ > other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator>=(const Duration other) const noexcept {
        return nanoseconds_ >= other.nanoseconds_;
    }

private:
    constexpr explicit Duration(const i64 count) noexcept : nanoseconds_(count) {}

    i64 nanoseconds_{0};
};

// A reading of the interval clock (stops during suspension).
class IntervalTime final {
public:
    constexpr IntervalTime() noexcept = default;

    [[nodiscard]] static IntervalTime now() noexcept;
    [[nodiscard]] static constexpr IntervalTime from_nanoseconds(const i64 count) noexcept {
        return IntervalTime(count);
    }

    [[nodiscard]] constexpr i64 count_nanoseconds() const noexcept { return nanoseconds_; }
    // This time minus `earlier`.
    [[nodiscard]] constexpr Duration since(const IntervalTime earlier) const noexcept {
        return Duration::nanoseconds(detail::saturating_subtract(nanoseconds_, earlier.nanoseconds_));
    }
    [[nodiscard]] constexpr IntervalTime after(const Duration interval) const noexcept {
        return IntervalTime(detail::saturating_add(nanoseconds_, interval.count_nanoseconds()));
    }

    [[nodiscard]] constexpr bool operator==(const IntervalTime other) const noexcept {
        return nanoseconds_ == other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator!=(const IntervalTime other) const noexcept {
        return nanoseconds_ != other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<(const IntervalTime other) const noexcept {
        return nanoseconds_ < other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<=(const IntervalTime other) const noexcept {
        return nanoseconds_ <= other.nanoseconds_;
    }

private:
    constexpr explicit IntervalTime(const i64 count) noexcept : nanoseconds_(count) {}

    i64 nanoseconds_{0};
};

// A reading of the elapsed clock (continues during suspension).
class ElapsedTime final {
public:
    constexpr ElapsedTime() noexcept = default;

    [[nodiscard]] static ElapsedTime now() noexcept;
    [[nodiscard]] static constexpr ElapsedTime from_nanoseconds(const i64 count) noexcept {
        return ElapsedTime(count);
    }

    [[nodiscard]] constexpr i64 count_nanoseconds() const noexcept { return nanoseconds_; }
    // This time minus `earlier`.
    [[nodiscard]] constexpr Duration since(const ElapsedTime earlier) const noexcept {
        return Duration::nanoseconds(detail::saturating_subtract(nanoseconds_, earlier.nanoseconds_));
    }
    [[nodiscard]] constexpr ElapsedTime after(const Duration interval) const noexcept {
        return ElapsedTime(detail::saturating_add(nanoseconds_, interval.count_nanoseconds()));
    }

    [[nodiscard]] constexpr bool operator==(const ElapsedTime other) const noexcept {
        return nanoseconds_ == other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator!=(const ElapsedTime other) const noexcept {
        return nanoseconds_ != other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<(const ElapsedTime other) const noexcept {
        return nanoseconds_ < other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<=(const ElapsedTime other) const noexcept {
        return nanoseconds_ <= other.nanoseconds_;
    }

private:
    constexpr explicit ElapsedTime(const i64 count) noexcept : nanoseconds_(count) {}

    i64 nanoseconds_{0};
};

// A reading of the wall clock: Unix time in nanoseconds.
class WallTime final {
public:
    constexpr WallTime() noexcept = default;

    [[nodiscard]] static WallTime now() noexcept;
    [[nodiscard]] static constexpr WallTime from_unix_nanoseconds(const i64 count) noexcept { return WallTime(count); }

    [[nodiscard]] constexpr i64 unix_nanoseconds() const noexcept { return nanoseconds_; }
    // Whole seconds since the epoch, rounded toward negative infinity.
    [[nodiscard]] constexpr i64 unix_seconds() const noexcept {
        const i64 quotient = nanoseconds_ / 1000000000;
        return (nanoseconds_ % 1000000000 < 0) ? quotient - 1 : quotient;
    }
    [[nodiscard]] constexpr Duration since(const WallTime earlier) const noexcept {
        return Duration::nanoseconds(detail::saturating_subtract(nanoseconds_, earlier.nanoseconds_));
    }

    [[nodiscard]] constexpr bool operator==(const WallTime other) const noexcept {
        return nanoseconds_ == other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator!=(const WallTime other) const noexcept {
        return nanoseconds_ != other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<(const WallTime other) const noexcept {
        return nanoseconds_ < other.nanoseconds_;
    }

private:
    constexpr explicit WallTime(const i64 count) noexcept : nanoseconds_(count) {}

    i64 nanoseconds_{0};
};

} // namespace stxlib
