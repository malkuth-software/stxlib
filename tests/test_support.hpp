#pragma once

// A minimal check harness for STXlib's tests (no third-party framework:
// every download is the owner's approval first). Each test program calls
// its named test functions from main and returns finish(). Checks may run
// on several threads at once (the thread tests check from their workers), so
// the counters are atomic; finish() runs after the workers are joined.

#include <atomic>
#include <cstdio>

namespace stxlib_tests {

struct Tally final {
    std::atomic<int> passed{0};
    std::atomic<int> failed{0};
};

inline Tally& tally() {
    static Tally state{};
    return state;
}

inline void check(const bool condition, const char* const expression, const char* const file, const int line) {
    Tally& state = tally();
    if (condition) {
        state.passed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state.failed.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "FAILED: %s (%s:%d)\n", expression, file, line);
}

inline int finish(const char* const suite) {
    const Tally& state = tally();
    const int passed = state.passed.load(std::memory_order_relaxed);
    const int failed = state.failed.load(std::memory_order_relaxed);
    std::printf("%s: %d checks passed, %d failed\n", suite, passed, failed);
    const int status = failed == 0 ? 0 : 1;
    return status;
}

} // namespace stxlib_tests

#define CHECK(condition) ::stxlib_tests::check((condition), #condition, __FILE__, __LINE__)
