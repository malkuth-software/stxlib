// stxlib::memory (include/stxlib/memory.hpp) over the allocator that
// stxlib_state owns (src/state/allocator.cpp, or stx_minmallocpp's
// state_calls.cpp), so every module STXlib is linked into allocates from and
// releases to the same allocator. This file keeps what belongs to each
// module's copy: the module's binding to the allocator's calls and the
// report when the allocator refuses a release. A bad alignment is refused
// by the allocator itself (state.hpp, AllocatorCalls; the efficiency
// review's F8), not tested again here.

#include "stxlib/memory.hpp"

#include "core/report.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

#include <atomic>
#include <type_traits>

namespace stxlib::memory {

namespace {

// This module's binding to the allocator's calls (the efficiency review's
// F9): a write-once memo (AGENTS, "Hard boundaries") of the address
// state::allocator gives, the one AllocatorCalls object of stxlib_state,
// constant-initialised and never changed (state.hpp). The first call in
// the module asks stxlib_state through current_allocator, so a layout
// mismatch still ends the process at the module's first allocator call;
// every later call reads the memo, one relaxed load in place of a call into
// stxlib_state. Relaxed is enough: the object behind the address is
// constant data of stxlib_state's image, initialised before any code of
// the process runs, so no store has to be made visible with it. Racing
// first calls compute the same address; the first to store it wins and the
// others keep what they computed, which is equal.
//
// Unload and reload: the memo is part of this module's image (stxlib_boundary
// is linked statically into each module). Unloading the module discards
// it; a reload maps it again, constant-initialised to null, and the first
// call checks the layout again. The address it holds stays valid while the
// module is loaded: the module depends on stxlib_state, so the dynamic
// loader keeps stxlib_state loaded at least as long (beyond that, a shared
// stxlib_state is built -z nodelete on Linux, and pins itself on Windows
// once it arms a thread block: thread_exit_windows.cpp); in a static build
// both are the same image.
constinit std::atomic<const state::AllocatorCalls*> bound_calls{nullptr};

static_assert(std::is_trivially_destructible_v<std::atomic<const state::AllocatorCalls*>>,
              "rule 2: the memo has no destructor to run at exit");

[[gnu::noinline]] const state::AllocatorCalls& bind_calls() noexcept {
    const state::AllocatorCalls& calls = state::current_allocator();
    const state::AllocatorCalls* expected = nullptr;
    static_cast<void>(bound_calls.compare_exchange_strong(expected, &calls, std::memory_order_relaxed));
    return calls;
}

[[nodiscard]] const state::AllocatorCalls& module_calls() noexcept {
    const state::AllocatorCalls* const known = bound_calls.load(std::memory_order_relaxed);
    if (known != nullptr) return *known;
    const state::AllocatorCalls& bound = bind_calls();
    return bound;
}

[[noreturn]] void reject_release() noexcept {
    host::hold_cancellation(); // the report writes (audit E, section 4.9)
    report::text("stxlib: requirement failed: stxlib::memory::release: the block is not from memory::allocate, "
                 "or was already released\n");
    host::end_process_abnormally();
}

} // namespace

Backend backend() noexcept {
    const Backend result = static_cast<Backend>(module_calls().backend);
    return result;
}

bool tags_checked() noexcept {
    const bool result = module_calls().tags_checked;
    return result;
}

void* allocate(const usize size, const usize alignment) noexcept {
    void* const result = module_calls().allocate(size, alignment);
    return result;
}

void release(void* const address) noexcept {
    if (address == nullptr) return;
    const state::ReleaseOutcome outcome = module_calls().release(address);
    if (outcome == state::ReleaseOutcome::refused) reject_release();
}

} // namespace stxlib::memory
