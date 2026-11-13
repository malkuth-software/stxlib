#pragma once

// The program's two clocks and its time values (ported from stx_auxlib's
// include/stx_auxlib/clocks.hpp at b6e330a, restated without std::chrono).
//
// MonotonicTime::now() never goes backwards and is unaffected by changes to
// the wall clock; its epoch is unspecified (typically boot), so its values
// are meaningful only relative to one another within one run. It replaces
// std::chrono::steady_clock. Whether it continues during suspend is the
// host's (survey D12, open: it may become two clocks, interval and elapsed).
//
// WallTime::now() is Coordinated Universal Time as nanoseconds since
// 1970-01-01T00:00:00Z, ignoring leap seconds (Unix time). It can jump when
// the system time is set, so measure intervals with MonotonicTime. It
// replaces std::chrono::system_clock.
//
// All three types count signed 64-bit nanoseconds, about 292 years either
// side of the epoch. Arithmetic saturates at the ends of that range instead
// of wrapping, so a deadline far in the future stays in the future. The
// host's clocks may tick more coarsely; now() reports what the host
// provides, converted without rounding.

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

// A reading of the monotonic clock.
class MonotonicTime final {
public:
    constexpr MonotonicTime() noexcept = default;

    [[nodiscard]] static MonotonicTime now() noexcept;
    [[nodiscard]] static constexpr MonotonicTime from_nanoseconds(const i64 count) noexcept {
        return MonotonicTime(count);
    }

    [[nodiscard]] constexpr i64 count_nanoseconds() const noexcept { return nanoseconds_; }
    // This time minus `earlier`.
    [[nodiscard]] constexpr Duration since(const MonotonicTime earlier) const noexcept {
        return Duration::nanoseconds(detail::saturating_subtract(nanoseconds_, earlier.nanoseconds_));
    }
    [[nodiscard]] constexpr MonotonicTime after(const Duration interval) const noexcept {
        return MonotonicTime(detail::saturating_add(nanoseconds_, interval.count_nanoseconds()));
    }

    [[nodiscard]] constexpr bool operator==(const MonotonicTime other) const noexcept {
        return nanoseconds_ == other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator!=(const MonotonicTime other) const noexcept {
        return nanoseconds_ != other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<(const MonotonicTime other) const noexcept {
        return nanoseconds_ < other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<=(const MonotonicTime other) const noexcept {
        return nanoseconds_ <= other.nanoseconds_;
    }

private:
    constexpr explicit MonotonicTime(const i64 count) noexcept : nanoseconds_(count) {}

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
