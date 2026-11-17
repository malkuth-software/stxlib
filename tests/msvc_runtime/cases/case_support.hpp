#pragma once

// Shared by the conformance cases (tests/msvc_runtime): each case prints
// lines to stdout and exits with a code; the runner compares both, case by
// case, between the program linked with Microsoft's vcruntime (the oracle)
// and the same source linked with STXlib's runtime.

#include <exception>
#include <stdio.h>
#include <stdlib.h>

// Flushed at once: a case may end in terminate, whose handler leaves with
// _exit, which flushes nothing.
inline void say(const char* const text) {
    fputs(text, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

#define SAYF(...)                                                                                                  \
    do {                                                                                                           \
        printf(__VA_ARGS__);                                                                                       \
        fputc('\n', stdout);                                                                                       \
        fflush(stdout);                                                                                            \
    } while (0)

// std::terminate's handler for the cases that end in it: a fixed line and
// exit code, the same under either runtime.
[[noreturn]] inline void case_terminate_handler() {
    say("terminate handler");
    _exit(3);
}

// Prints its name when destroyed, so the order of cleanups is visible.
struct Tracker final {
    const char* name;
    explicit Tracker(const char* const tracker_name) : name(tracker_name) {}
    ~Tracker() {
        SAYF("~%s uncaught=%d", name, std::uncaught_exceptions());
    }
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;
};
