// A benchmark of stxlib::memory (include/stxlib/memory.hpp), whatever
// backend stxlib_state was built with, beside the host's malloc as a
// reference. Not a test: it checks nothing and is not registered with
// ctest. Build it in two configurations (STXLIB_ALLOCATOR=mimalloc and
// stx_minmallocpp) and compare the columns.
//
// Each workload is run `repeats` times and the best time is reported, in
// nanoseconds per operation (an allocate or a release is one operation).
// The workloads, all through the public calls, so both backends pay the
// same call path:
//   pair      allocate one block and release it, over and over
//   fifo      allocate a batch, release it in allocation order
//   lifo      allocate a batch, release it in reverse order
//   churn     keep a table of live blocks of mixed sizes; replace a random
//             entry each step (steady state of a long-running program)
//   aligned   pair, with a 64-byte alignment
//   handoff   one thread allocates batches, another releases them
//
// The numbers depend on the machine and its load: compare columns of one
// run, or runs made back to back on an idle machine.

#include "stxlib/memory.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

namespace {

using stxlib::usize;

constexpr int repeats = 7;

// The two allocators under the same shape.
struct Calls final {
    void* (*allocate)(usize size, usize alignment);
    void (*release)(void* block);
};

void* stxlib_allocate(const usize size, const usize alignment) {
    void* const block = stxlib::memory::allocate(size, alignment);
    return block;
}

void stxlib_release(void* const block) { stxlib::memory::release(block); }

void* host_allocate(const usize size, const usize alignment) {
    if (alignment <= 16) {
        void* const block = std::malloc(size);
        return block;
    }
#if defined(_WIN32)
    // The Windows C runtime's aligned blocks need their own release call;
    // the reference then is an unaligned block of the same size.
    void* const unaligned = std::malloc(size);
    return unaligned;
#else
    void* aligned = nullptr;
    if (::posix_memalign(&aligned, alignment, size) != 0) return nullptr;
    return aligned;
#endif
}

void host_release(void* const block) { std::free(block); }

constexpr Calls stxlib_calls{.allocate = stxlib_allocate, .release = stxlib_release};
constexpr Calls host_calls{.allocate = host_allocate, .release = host_release};

[[nodiscard]] double seconds_now() {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const double result = std::chrono::duration<double>(now.time_since_epoch()).count();
    return result;
}

// Keeps the compiler from dropping a block nobody reads.
void touch(void* const block) {
    volatile unsigned char* const bytes = static_cast<volatile unsigned char*>(block);
    bytes[0] = 1;
}

[[nodiscard]] double pair(const Calls& calls, const usize size, const usize alignment, const usize rounds) {
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds; ++round) {
            void* const block = calls.allocate(size, alignment);
            touch(block);
            calls.release(block);
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    const double per_operation = best * 1e9 / static_cast<double>(rounds * 2);
    return per_operation;
}

[[nodiscard]] double batch(const Calls& calls, const usize size, const usize count, const usize rounds, const bool reverse) {
    std::vector<void*> blocks(count, nullptr);
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        const double start = seconds_now();
        for (usize round = 0; round < rounds; ++round) {
            for (usize index = 0; index < count; ++index) {
                blocks[index] = calls.allocate(size, 16);
                touch(blocks[index]);
            }
            if (reverse) {
                for (usize index = count; index > 0; --index) calls.release(blocks[index - 1]);
            } else {
                for (usize index = 0; index < count; ++index) calls.release(blocks[index]);
            }
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    const double per_operation = best * 1e9 / static_cast<double>(rounds * count * 2);
    return per_operation;
}

// xorshift64 (Marsaglia, "Xorshift RNGs", 2003): a fixed sequence, so both
// allocators see the same requests.
[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

[[nodiscard]] double churn(const Calls& calls, const usize slots, const usize steps) {
    std::vector<void*> table(slots, nullptr);
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        std::uint64_t random = 0x9e3779b97f4a7c15ULL;
        const double start = seconds_now();
        for (usize step = 0; step < steps; ++step) {
            const std::uint64_t draw = next_random(random);
            const usize slot = static_cast<usize>(draw % slots);
            const usize size = 16 + static_cast<usize>((draw >> 32) % 1008);
            if (table[slot] != nullptr) calls.release(table[slot]);
            table[slot] = calls.allocate(size, 16);
            touch(table[slot]);
        }
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
        for (usize slot = 0; slot < slots; ++slot) {
            if (table[slot] != nullptr) calls.release(table[slot]);
            table[slot] = nullptr;
        }
    }
    const double per_operation = best * 1e9 / static_cast<double>(steps * 2);
    return per_operation;
}

// One batch passed from the allocating thread to the releasing thread.
struct Handoff final {
    std::atomic<std::vector<void*>*> ready{nullptr};
    std::atomic<bool> done{false};
};

void release_handed(const Calls& calls, Handoff& handoff) {
    for (;;) {
        std::vector<void*>* const batch_blocks = handoff.ready.exchange(nullptr);
        if (batch_blocks == nullptr) {
            if (handoff.done.load()) return;
            std::this_thread::yield();
            continue;
        }
        for (void* const block : *batch_blocks) calls.release(block);
        delete batch_blocks;
    }
}

[[nodiscard]] double handoff(const Calls& calls, const usize size, const usize count, const usize batches) {
    double best = 1e30;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        Handoff shared{};
        std::thread releaser(release_handed, std::cref(calls), std::ref(shared));
        const double start = seconds_now();
        for (usize made = 0; made < batches; ++made) {
            std::vector<void*>* const blocks = new std::vector<void*>(count, nullptr);
            for (usize index = 0; index < count; ++index) {
                (*blocks)[index] = calls.allocate(size, 16);
                touch((*blocks)[index]);
            }
            std::vector<void*>* expected = nullptr;
            while (!shared.ready.compare_exchange_weak(expected, blocks)) {
                expected = nullptr;
                std::this_thread::yield();
            }
        }
        shared.done.store(true);
        releaser.join();
        const double elapsed = seconds_now() - start;
        if (elapsed < best) best = elapsed;
    }
    const double per_operation = best * 1e9 / static_cast<double>(batches * count * 2);
    return per_operation;
}

void row(const char* const name, const double ours, const double host) {
    std::printf("%-22s %9.2f %9.2f %8.2f\n", name, ours, host, ours / host);
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count == 2 && std::strcmp(arguments[1], "churn") == 0) {
        // Only the large churn, through stxlib::memory, several times over:
        // long enough for a sampling profiler to see where its time goes.
        for (int pass = 0; pass < 8; ++pass) {
            std::printf("churn 100000 slots %9.2f\n", churn(stxlib_calls, 100000, 2000000));
        }
        return 0;
    }
    const char* backend = "host";
    if (stxlib::memory::backend() == stxlib::memory::Backend::mimalloc) backend = "mimalloc";
    if (stxlib::memory::backend() == stxlib::memory::Backend::stx_minmallocpp) backend = "stx_minmallocpp";
    std::printf("stxlib::memory backend: %s, tags checked: %d\n", backend, stxlib::memory::tags_checked() ? 1 : 0);
    std::printf("%-22s %9s %9s %8s   (ns per operation, best of %d)\n", "workload", "stxlib", "malloc", "ratio", repeats);
    row("pair 16", pair(stxlib_calls, 16, 16, 2000000), pair(host_calls, 16, 16, 2000000));
    row("pair 64", pair(stxlib_calls, 64, 16, 2000000), pair(host_calls, 64, 16, 2000000));
    row("pair 256", pair(stxlib_calls, 256, 16, 2000000), pair(host_calls, 256, 16, 2000000));
    row("pair 1024", pair(stxlib_calls, 1024, 16, 2000000), pair(host_calls, 1024, 16, 2000000));
    row("pair 8192", pair(stxlib_calls, 8192, 16, 1000000), pair(host_calls, 8192, 16, 1000000));
    row("pair 65536", pair(stxlib_calls, 65536, 16, 500000), pair(host_calls, 65536, 16, 500000));
    row("pair 1 MiB", pair(stxlib_calls, 1048576, 16, 20000), pair(host_calls, 1048576, 16, 20000));
    row("aligned 100/64", pair(stxlib_calls, 100, 64, 2000000), pair(host_calls, 100, 64, 2000000));
    row("fifo 16 x 10000", batch(stxlib_calls, 16, 10000, 200, false), batch(host_calls, 16, 10000, 200, false));
    row("fifo 64 x 10000", batch(stxlib_calls, 64, 10000, 200, false), batch(host_calls, 64, 10000, 200, false));
    row("fifo 256 x 10000", batch(stxlib_calls, 256, 10000, 100, false), batch(host_calls, 256, 10000, 100, false));
    row("fifo 4096 x 2000", batch(stxlib_calls, 4096, 2000, 100, false), batch(host_calls, 4096, 2000, 100, false));
    row("lifo 64 x 10000", batch(stxlib_calls, 64, 10000, 200, true), batch(host_calls, 64, 10000, 200, true));
    row("lifo 256 x 10000", batch(stxlib_calls, 256, 10000, 100, true), batch(host_calls, 256, 10000, 100, true));
    row("churn 4096 slots", churn(stxlib_calls, 4096, 2000000), churn(host_calls, 4096, 2000000));
    row("churn 100000 slots", churn(stxlib_calls, 100000, 2000000), churn(host_calls, 100000, 2000000));
    row("handoff 64 x 1024", handoff(stxlib_calls, 64, 1024, 1000), handoff(host_calls, 64, 1024, 1000));
    return 0;
}
