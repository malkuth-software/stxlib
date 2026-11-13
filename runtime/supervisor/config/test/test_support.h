// test_support.h: the host tests' one checking function and their count.
// The tests build with the host's compiler under address and
// undefined-behaviour sanitizers (run_host_tests.sh).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_TEST_TEST_SUPPORT_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_TEST_TEST_SUPPORT_H

#include <cstddef>
#include <cstdio>
#include <span>

namespace stxtest {

struct Tally {
    std::size_t checks;
    std::size_t failures;
};

inline Tally tally{0, 0};

// Records one check; prints the failing ones.
inline void check(bool condition, const char* test, const char* what) noexcept {
    tally.checks += 1;
    if (!condition) {
        tally.failures += 1;
        std::printf("FAIL %s: %s\n", test, what);
    }
}

[[nodiscard]] inline std::size_t c_length(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length;
}

// A span over a string literal's characters, without its NUL.
[[nodiscard]] inline std::span<const char> text_of(const char* text) noexcept {
    const std::span<const char> view(text, c_length(text));
    return view;
}

[[nodiscard]] inline bool same_text(const char* left, const char* right) noexcept {
    std::size_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index += 1;
    }
    return left[index] == right[index];
}

// Prints the totals; the exit status is 0 only when every check passed.
[[nodiscard]] inline int finish(const char* suite) noexcept {
    std::printf("%s: %zu checks, %zu failed\n", suite, tally.checks, tally.failures);
    return tally.failures == 0 ? 0 : 1;
}

}  // namespace stxtest

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_TEST_TEST_SUPPORT_H
