// stx_minmallocpp's host layer on POSIX hosts (src/state/stx_minmallocpp/
// os.hpp): macOS, glibc, musl and FreeBSD. The whole host surface of the
// allocator is here: mmap, mprotect, munmap, sched_yield, sysconf, abort,
// and a pthread key for the thread-exit hook (pthread_key_create,
// pthread_setspecific, pthread_getspecific).
//
// Reserving. mmap with PROT_NONE, MAP_PRIVATE | MAP_ANON and no file maps
// address space with no access (MAP_ANON: pages_posix.cpp cites it for
// each host). An aligned reservation maps alignment bytes more and unmaps
// what lies before and after the aligned part: three host calls at most.
// Where the host has PROT_MAX (FreeBSD), the mapping states that it may
// later become readable and writable; without that, a host run with
// vm.imply_prot_max would refuse every commit (unconfirmed until run).
//
// Committing is mprotect to PROT_READ | PROT_WRITE. A private anonymous
// mapping reads as zero until written. Under strict memory accounting
// (Linux vm.overcommit_memory = 2; FreeBSD vm.overcommit with its swap
// reservation bits) making pages writable is what is charged, so a refusal
// arrives here, as false, and reaches the caller as a null allocation.
// Unconfirmed on each host until run.
//
// Decommitting maps fresh PROT_NONE pages over the range with MAP_FIXED.
// That replaces the pages in one call, so the memory and its charge go
// back to the host and a later commit reads zero. It is used on whole
// 1 MiB units only (region.cpp), which bounds the number of mappings.
//
// Cancellation. mmap, mprotect, munmap and sched_yield are in neither list
// of POSIX.1-2024, XSH 2.9.5.2 (read 2026-11-15). sysconf and the pthread
// key calls were not looked up there; unconfirmed. None of the calls here
// can unwind.
//
// The thread-exit hook is a pthread key whose destructor the host runs for
// every thread that returns from its start routine or calls pthread_exit,
// with the value the thread set. The key is made once, on the first heap.
// The key's value is the truth about the thread: null (nothing armed), the
// context it armed, or the ended mark, which the destructor leaves behind
// so that an allocation later in the thread's exit sees that the hook has
// run. The host clears a value before it calls the destructor and runs
// destructors again for values set meanwhile, a fixed number of rounds
// (PTHREAD_DESTRUCTOR_ITERATIONS); the mark is set again each round.

#include "state/stx_minmallocpp/os.hpp"

#include <atomic>
#include <cstdlib>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

namespace stxlib::stx_minmallocpp::os {

namespace {

constexpr u32 key_unmade = 0;
constexpr u32 key_being_made = 1;
constexpr u32 key_made = 2;
constexpr u32 key_refused = 3;

// What a reservation is mapped with: no access now, readable and writable
// later where the host wants that said up front.
#if defined(PROT_MAX)
constexpr int reserved_protection = PROT_NONE | PROT_MAX(PROT_READ | PROT_WRITE);
#else
constexpr int reserved_protection = PROT_NONE;
#endif

constinit std::atomic<u32> key_state{key_unmade};
pthread_key_t exit_key{};

// Its address is the key's value for a thread whose hook has run.
constinit char ended_mark{0};

// The key's destructor: gives up what the thread armed, once, and leaves
// the mark.
void exit_destructor(void* const context) {
    if (context != &ended_mark) thread_ending(context);
    static_cast<void>(::pthread_setspecific(exit_key, &ended_mark));
}

// The calling thread's value of the key; null before the key exists.
[[nodiscard]] void* key_value() noexcept {
    if (key_state.load(std::memory_order_acquire) != key_made) return nullptr;
    void* const result = ::pthread_getspecific(exit_key);
    return result;
}

// Makes the key once. The first caller makes it; a caller that arrives
// while it is being made yields until it is settled.
[[nodiscard]] bool ensure_exit_key() noexcept {
    for (;;) {
        u32 seen = key_state.load(std::memory_order_acquire);
        if (seen == key_made) return true;
        if (seen == key_refused) return false;
        if (seen == key_unmade && key_state.compare_exchange_strong(seen, key_being_made)) {
            const bool made = ::pthread_key_create(&exit_key, exit_destructor) == 0;
            key_state.store(made ? key_made : key_refused, std::memory_order_release);
            return made;
        }
        static_cast<void>(::sched_yield());
    }
}

} // namespace

usize page_bytes() noexcept {
    const long reported = ::sysconf(_SC_PAGESIZE);
    const usize result = reported > 0 ? static_cast<usize>(reported) : 0;
    return result;
}

void* reserve(const usize bytes, const usize alignment) noexcept {
    if (bytes > usize_max - alignment) return nullptr;
    const usize span = bytes + alignment;
    void* const mapped = ::mmap(nullptr, span, reserved_protection, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) return nullptr;
    const uptr start = reinterpret_cast<uptr>(mapped);
    const uptr aligned = (start + (alignment - 1)) & ~static_cast<uptr>(alignment - 1);
    const usize before = static_cast<usize>(aligned - start);
    const usize after = span - before - bytes;
    // munmap of part of a mapping we made cannot fail for a valid range.
    if (before != 0) static_cast<void>(::munmap(mapped, before));
    if (after != 0) static_cast<void>(::munmap(reinterpret_cast<void*>(aligned + bytes), after));
    void* const result = reinterpret_cast<void*>(aligned);
    return result;
}

bool commit(void* const address, const usize bytes) noexcept {
    const bool result = ::mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0;
    return result;
}

void decommit(void* const address, const usize bytes) noexcept {
    // On failure (the host's limit on mappings) the range stays as it was.
    static_cast<void>(::mmap(address, bytes, reserved_protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0));
}

void release(void* const address, const usize bytes) noexcept { static_cast<void>(::munmap(address, bytes)); }

void yield() noexcept { static_cast<void>(::sched_yield()); }

void stop() noexcept { std::abort(); }

bool arm_thread_exit(void* const context) noexcept {
    if (!ensure_exit_key()) return false;
    const bool result = ::pthread_setspecific(exit_key, context) == 0;
    return result;
}

void* armed_context() noexcept {
    void* const value = key_value();
    void* const result = value == &ended_mark ? nullptr : value;
    return result;
}

bool thread_has_ended() noexcept {
    const bool result = key_value() == &ended_mark;
    return result;
}

void mark_thread_ended() noexcept {
    if (key_state.load(std::memory_order_acquire) != key_made) return;
    static_cast<void>(::pthread_setspecific(exit_key, &ended_mark));
}

} // namespace stxlib::stx_minmallocpp::os
