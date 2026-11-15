// stxlib::memory (include/stxlib/memory.hpp) over the allocator that
// stxlib_state owns (src/state/allocator.cpp), so every module STXlib is
// linked into allocates from and releases to the same allocator. This file
// keeps what belongs to each module's copy: refusing a bad alignment before
// the call, and the report when the allocator refuses a release.

#include "stxlib/memory.hpp"

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
    void* const result = state::current_allocator().allocate(size, alignment);
    return result;
}

void release(void* const address) noexcept {
    if (address == nullptr) return;
    const state::ReleaseOutcome outcome = state::current_allocator().release(address);
    if (outcome == state::ReleaseOutcome::refused) reject_release();
}

} // namespace stxlib::memory
