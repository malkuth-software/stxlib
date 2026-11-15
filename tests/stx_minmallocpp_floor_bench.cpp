// Where stx_minmallocpp's time per call goes, layer by layer. Not a test:
// it checks nothing and is not registered with ctest. The allocator's
// sources are linked directly (no stxlib_state, no stxlib::memory), built
// as stxlib_state builds them and without the entry and exit checks.
//
// One workload, an allocate and release pair of one size, timed through
// each layer of the fast path, so the difference between two rows is the
// cost of what the lower row leaves out:
//
//   public        allocate(size, alignment) and release(address): what
//                 stxlib_state calls. Reads the thread's heap pointer in
//                 each call.
//   heap passed   allocate_with and release_with: the caller already has
//                 its heap. Public minus this is the thread-local read.
//   class passed  allocate_class_with and release_with: the caller also
//                 knows the size class (a size fixed at compile time).
//                 Heap passed minus this is the argument checks and the
//                 class arithmetic.
//   take only     take_block on the class entry, and release_with: no
//                 call into the slow path's wrapper at all.
//   pointer read  reading the thread's heap pointer and nothing else, in a
//                 function of its own (so it includes one call and return).
//   host malloc   the host's malloc and free, for scale.
//
// Nanoseconds per call (an allocate or a release), best of `repeats`.

#include "state/stx_minmallocpp/heap.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

using stxlib::usize;
using namespace stxlib::stx_minmallocpp;

constexpr int repeats = 9;
constexpr usize rounds = 4000000;

[[nodiscard]] double seconds_now() {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const double result = std::chrono::duration<double>(now.time_since_epoch()).count();
    return result;
}

void touch(void* const block) {
    volatile unsigned char* const bytes = static_cast<volatile unsigned char*>(block);
    bytes[0] = 1;
}

[[nodiscard]] double per_call(const double best_seconds) {
    const double result = best_seconds * 1e9 / static_cast<double>(rounds * 2);
    return result;
}

[[nodiscard]] double time_public(const usize size) {
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds; ++round) {
            void* const block = allocate(size, 16);
            touch(block);
            static_cast<void>(release(block));
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    return per_call(best);
}

[[nodiscard]] double time_heap_passed(const usize size) {
    Heap& heap = *current_heap;
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds; ++round) {
            void* const block = allocate_with(heap, size, 16);
            touch(block);
            static_cast<void>(release_with(heap, block));
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    return per_call(best);
}

[[nodiscard]] double time_class_passed(const usize size) {
    Heap& heap = *current_heap;
    const usize class_index = class_of(size);
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds; ++round) {
            void* const block = allocate_class_with(heap, class_index);
            touch(block);
            static_cast<void>(release_with(heap, block));
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    return per_call(best);
}

[[nodiscard]] double time_take_only(const usize size) {
    Heap& heap = *current_heap;
    ClassEntry& entry = heap.entries[class_of(size)];
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds; ++round) {
            // The pair releases what it took, so the current word is never empty.
            void* const block = take_block(entry);
            touch(block);
            static_cast<void>(release_with(heap, block));
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    return per_call(best);
}

// In a function of its own, as each public call reads it: the compiler
// would otherwise read it once for the whole loop.
[[gnu::noinline]] Heap* read_pointer() {
    Heap* const heap = current_heap;
    return heap;
}

[[nodiscard]] double time_pointer_read() {
    double best = 1e30;
    volatile stxlib::uptr sink = 0;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds * 2; ++round) {
            sink = reinterpret_cast<stxlib::uptr>(read_pointer());
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    static_cast<void>(sink);
    return per_call(best);
}

[[nodiscard]] double time_host(const usize size) {
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds; ++round) {
            void* const block = std::malloc(size);
            touch(block);
            std::free(block);
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    return per_call(best);
}

} // namespace

int main() {
    // Give this thread its heap and a current page of each size used.
    const usize sizes[] = {16, 64, 1024};
    for (const usize size : sizes) {
        void* const first = allocate(size, 16);
        static_cast<void>(release(first));
    }
    std::printf("%-8s %8s %12s %13s %10s %13s %12s   (ns per call, best of %d)\n", "size", "public", "heap passed",
                "class passed", "take only", "pointer read", "host malloc", repeats);
    for (const usize size : sizes) {
        std::printf("%-8zu %8.2f %12.2f %13.2f %10.2f %13.2f %12.2f\n", size, time_public(size), time_heap_passed(size),
                    time_class_passed(size), time_take_only(size), time_pointer_read(), time_host(size));
    }
    return 0;
}
