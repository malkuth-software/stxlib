#pragma once

// A minimal check harness for STXlib's tests (no third-party framework:
// every download is the owner's approval first). Each test program calls
// its named test functions from main and returns finish().

#include <cstdio>

namespace stxlib_tests {

struct Tally final {
    int passed{0};
    int failed{0};
};

inline Tally& tally() {
    static Tally state{};
    return state;
}

inline void check(const bool condition, const char* const expression, const char* const file, const int line) {
    Tally& state = tally();
    if (condition) {
        state.passed = state.passed + 1;
        return;
    }
    state.failed = state.failed + 1;
    std::fprintf(stderr, "FAILED: %s (%s:%d)\n", expression, file, line);
}

inline int finish(const char* const suite) {
    const Tally& state = tally();
    std::printf("%s: %d checks passed, %d failed\n", suite, state.passed, state.failed);
    const int status = state.failed == 0 ? 0 : 1;
    return status;
}

} // namespace stxlib_tests

#define CHECK(condition) ::stxlib_tests::check((condition), #condition, __FILE__, __LINE__)
