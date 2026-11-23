// The cache module on macOS (hosted/cache_backend.hpp). Host facts (Apple's
// sysctl names as published; unconfirmed at a pin): hw.perflevel0.* names the
// performance cores' caches on machines with two kinds of core, hw.* the
// machine's otherwise; hw.cachelinesize is the line. macOS reports no
// associativity, so `ways` keeps its default (8).

#include "hosted/cache_backend.hpp"

#include <sys/sysctl.h>
#include <sys/types.h>

namespace stxlib::cache_backend {

namespace {

// The named value when the host reports a positive one, else 0.
[[nodiscard]] usize reported(const char* const name) noexcept {
    long long value = 0;
    size_t length = sizeof value;
    if (::sysctlbyname(name, &value, &length, nullptr, 0) != 0 || value <= 0) return 0;
    const usize result = static_cast<usize>(value);
    return result;
}

// The first of two names the host reports, else `fallback`.
[[nodiscard]] usize either(const char* const preferred, const char* const general, const usize fallback) noexcept {
    const usize first = reported(preferred);
    if (first != 0) return first;
    const usize second = reported(general);
    if (second != 0) return second;
    return fallback;
}

} // namespace

void read_data_caches(DataCaches& caches) noexcept {
    caches.level1 = either("hw.perflevel0.l1dcachesize", "hw.l1dcachesize", caches.level1);
    caches.level2 = either("hw.perflevel0.l2cachesize", "hw.l2cachesize", caches.level2);
    const usize line = reported("hw.cachelinesize");
    if (line != 0) caches.line = line;
}

} // namespace stxlib::cache_backend
