// The startup shim's main probe: static initialization, the security
// cookie, a large frame (__chkstk), thread_local initialization and
// destruction on other threads, a thread-safe static under contention, an
// exception through vcruntime, a DLL's initialization, an indirect call
// under Control Flow Guard, and the order of exit functions.
//
// Prints one line per check and "startup probe: all checks passed" at the
// end of main; a failing check prints FAILED. The exit functions check
// their own order and end the process with 1 if it is wrong.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" int probe_library_value(void);
extern "C" int probe_library_thread_constructions(void);
extern "C" uintptr_t __security_cookie;

namespace {

int failures = 0;

void check(const bool condition, const char* const name) {
    if (condition) {
        std::printf("ok %s\n", name);
    } else {
        std::printf("FAILED %s\n", name);
        failures = failures + 1;
    }
}

// Static initialization: constructed before main, in declaration order.
int construction_order[2] = {0, 0};
int construction_count = 0;

struct Ordered {
    explicit Ordered(const int identity) {
        construction_order[construction_count] = identity;
        construction_count = construction_count + 1;
    }
};

Ordered first_static(1);
Ordered second_static(2);

// A frame larger than many pages, touched at both ends.
__attribute__((noinline)) int touch_large_frame() {
    volatile unsigned char frame[256 * 1024];
    frame[0] = 1;
    frame[sizeof(frame) - 1] = 2;
    const int sum = frame[0] + frame[sizeof(frame) - 1];
    return sum;
}

// A thread_local with a constructor and a destructor, counted.
std::atomic<int> local_constructions{0};
std::atomic<int> local_destructions{0};

struct Counted {
    int value;
    Counted() : value(5) { local_constructions.fetch_add(1); }
    ~Counted() { local_destructions.fetch_add(1); }
};

thread_local Counted counted;

DWORD WINAPI use_thread_local(LPVOID parameter) {
    static_cast<void>(parameter);
    const int value = counted.value;
    return static_cast<DWORD>(value);
}

// A function-local static whose constructor is slow, entered by many
// threads at once.
std::atomic<int> slow_constructions{0};

struct Slow {
    int value;
    Slow() : value(9) {
        slow_constructions.fetch_add(1);
        Sleep(50);
    }
};

int read_slow_static() {
    static Slow slow;
    return slow.value;
}

DWORD WINAPI use_slow_static(LPVOID parameter) {
    static_cast<void>(parameter);
    const int value = read_slow_static();
    return static_cast<DWORD>(value);
}

bool run_threads(LPTHREAD_START_ROUTINE routine, const int count, const DWORD expected) {
    HANDLE threads[8] = {};
    bool all_expected = true;
    for (int index = 0; index < count; ++index) {
        threads[index] = CreateThread(nullptr, 0, routine, nullptr, 0, nullptr);
        if (threads[index] == nullptr) {
            return false;
        }
    }
    for (int index = 0; index < count; ++index) {
        WaitForSingleObject(threads[index], INFINITE);
        DWORD code = 0;
        GetExitCodeThread(threads[index], &code);
        if (code != expected) {
            all_expected = false;
        }
        CloseHandle(threads[index]);
    }
    return all_expected;
}

int throw_and_catch() {
    int result = 0;
    try {
        throw std::runtime_error("probe");
    } catch (const std::exception& caught) {
        if (caught.what()[0] == 'p') {
            result = 1;
        }
    }
    return result;
}

int add_one(const int value) {
    return value + 1;
}

// Exit functions: registered in main, run in reverse; the last to run
// (registered first) checks that the others ran, in order.
int exit_record[3] = {0, 0, 0};
int exit_count = 0;

void record_exit(const int identity) {
    if (exit_count < 3) {
        exit_record[exit_count] = identity;
    }
    exit_count = exit_count + 1;
}

void exit_one() {
    record_exit(1);
}

void exit_two() {
    record_exit(2);
}

void exit_three() {
    record_exit(3);
}

void check_exit_order() {
    const bool ordered = exit_count == 3 && exit_record[0] == 3 && exit_record[1] == 2 && exit_record[2] == 1;
    std::printf("exit order: %d %d %d (%s)\n", exit_record[0], exit_record[1], exit_record[2], ordered ? "ok" : "wrong");
    std::fflush(stdout);
    if (!ordered) {
        _exit(1);
    }
}

}  // namespace

int main() {
    check(construction_count == 2 && construction_order[0] == 1 && construction_order[1] == 2, "static initialization order");
    check(__security_cookie != 0x00002B992DDFA232ULL && __security_cookie != 0, "security cookie chosen");
    check(touch_large_frame() == 3, "large frame");

    check(run_threads(&use_thread_local, 4, 5), "thread_local values");
    check(local_constructions.load() == 4 && local_destructions.load() == 4,
          "thread_local construction and destruction per thread");

    check(run_threads(&use_slow_static, 8, 9), "thread-safe static value");
    check(slow_constructions.load() == 1, "thread-safe static constructed once");

    check(throw_and_catch() == 1, "exception through vcruntime");
    check(probe_library_value() == 42, "DLL static and thread_local initialization");
    check(probe_library_thread_constructions() >= 1, "DLL thread_local constructed");

    int (*volatile indirect)(int) = &add_one;
    check(indirect(41) == 42, "indirect call under Control Flow Guard");

    check(std::atexit(&check_exit_order) == 0, "atexit");
    std::atexit(&exit_one);
    std::atexit(&exit_two);
    std::atexit(&exit_three);

    if (failures == 0) {
        std::printf("startup probe: all checks passed\n");
    }
    std::fflush(stdout);
    return failures;
}
