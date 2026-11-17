// Exceptions on several threads at once: each thread's uncaught count and
// active catches are its own.

#include "case_support.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

constexpr int thread_count = 4;
constexpr int rounds = 2000;

struct Work {
    int seed;
    int total;
    int wrong_count;
};

struct Checker {
    Work& work;
    ~Checker() {
        if (std::uncaught_exceptions() != 1) (*&work).wrong_count = (*&work).wrong_count + 1;
    }
};

void throw_value(Work& work, const int value) {
    Checker checker{work};
    static_cast<void>(checker);
    throw value;
}

DWORD WINAPI run(void* const argument) {
    Work& work = *static_cast<Work*>(argument);
    for (int round = 0; round < rounds; ++round) {
        try {
            throw_value(work, work.seed + round);
        } catch (const int value) {
            try {
                throw value + 1;
            } catch (const int inner) {
                if (std::uncaught_exceptions() != 0) work.wrong_count = work.wrong_count + 1;
                work.total = work.total + (inner - value);
            }
        }
    }
    return 0;
}

} // namespace

int main() {
    Work work[thread_count] = {};
    HANDLE threads[thread_count] = {};
    for (int index = 0; index < thread_count; ++index) {
        work[index].seed = index * 1000;
        threads[index] = CreateThread(nullptr, 0, run, &work[index], 0, nullptr);
    }
    WaitForMultipleObjects(thread_count, threads, TRUE, INFINITE);
    for (int index = 0; index < thread_count; ++index) {
        CloseHandle(threads[index]);
        SAYF("thread %d total %d wrong %d", index, work[index].total, work[index].wrong_count);
    }
    return 0;
}
