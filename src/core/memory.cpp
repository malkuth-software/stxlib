// stxlib::memory (include/stxlib/memory.hpp) over the allocator that
// stxlib_state owns (src/state/allocator.cpp), so every module STXlib is
// linked into allocates from and releases to the same allocator. This file
// keeps what belongs to each module's copy: refusing a bad alignment before
// the call, the report when the allocator refuses a release, and the
// fail-fast when the allocator has detected heap corruption.

#include "stxlib/memory.hpp"

#include "stxlib/error.hpp"
#include "stxlib/fail_fast.hpp"

#include "core/report.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

namespace stxlib::memory {

namespace {

[[nodiscard]] bool power_of_two(const usize value) noexcept {
    const bool result = value != 0 && (value & (value - 1)) == 0;
    return result;
}

[[noreturn]] void reject_release() noexcept {
    host::hold_cancellation(); // the report writes (audit E, section 4.9)
    report::text("stxlib: requirement failed: stxlib::memory::release: the block is not from memory::allocate, "
                 "or was already released\n");
    host::end_process_abnormally();
}

// The allocator detected heap corruption (state.hpp, AllocatorCalls::
// heap_failure; allocator.cpp, D6). The heap cannot be trusted, so the
// process ends here, before the block of the call that detected it reaches
// its caller. fail_fast allocates nothing.
[[noreturn]] void heap_corrupted(const u32 code) noexcept {
#if defined(_WIN32)
    constexpr ForeignFamily family = ForeignFamily::crt_errno;
#else
    constexpr ForeignFamily family = ForeignFamily::posix_errno;
#endif
    const ErrorRecord record = ErrorRecord::make(
        ErrorKind::internal_failure, "stxlib.memory.heap",
        ErrorDetail{.foreign = ForeignCode::of(family, static_cast<i64>(code)),
                    .message = "the allocator detected heap corruption"});
    fail_fast(record);
}

void check_heap(const state::AllocatorCalls& calls) noexcept {
    const u32 code = (*calls.heap_failure).load(std::memory_order_relaxed);
    if (code != 0) heap_corrupted(code);
}

} // namespace

Backend backend() noexcept {
    const Backend result = static_cast<Backend>(state::current_allocator().backend);
    return result;
}

bool tags_checked() noexcept {
    const bool result = state::current_allocator().tags_checked;
    return result;
}

void* allocate(const usize size, const usize alignment) noexcept {
    if (!power_of_two(alignment)) return nullptr;
    const state::AllocatorCalls& calls = state::current_allocator();
    void* const result = calls.allocate(size, alignment);
    check_heap(calls);
    return result;
}

void release(void* const address) noexcept {
    if (address == nullptr) return;
    const state::AllocatorCalls& calls = state::current_allocator();
    const state::ReleaseOutcome outcome = calls.release(address);
    check_heap(calls);
    if (outcome == state::ReleaseOutcome::refused) reject_release();
}

} // namespace stxlib::memory
