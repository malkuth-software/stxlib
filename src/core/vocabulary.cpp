// The owned vocabulary's allocation path and its test switch
// (include/stxlib/detail/vocabulary_allocation.hpp).

#include "stxlib/detail/vocabulary_allocation.hpp"

#include "stxlib/memory.hpp"

namespace stxlib::detail {

namespace {

// One thread's switch. Plain data with constant initialisation, so a thread
// reads it without a guard or a constructor call.
struct RefusalSwitch final {
    bool armed{false};
    u64 allowed{0};
    u64 granted{0};
    u64 released{0};
};

thread_local RefusalSwitch refusal_switch{};

} // namespace

void* vocabulary_allocate(const usize size, const usize alignment) noexcept {
    RefusalSwitch& state = refusal_switch;
    if (state.armed) {
        if (state.allowed == 0) return nullptr;
        state.allowed = state.allowed - 1;
    }
    void* const result = memory::allocate(size, alignment);
    if (result != nullptr) state.granted = state.granted + 1;
    return result;
}

void vocabulary_release(void* const address) noexcept {
    if (address == nullptr) return;
    RefusalSwitch& state = refusal_switch;
    state.released = state.released + 1;
    memory::release(address);
}

void arm_vocabulary_refusals(const u64 allowed) noexcept {
    RefusalSwitch& state = refusal_switch;
    state.armed = true;
    state.allowed = allowed;
}

void disarm_vocabulary_refusals() noexcept {
    RefusalSwitch& state = refusal_switch;
    state.armed = false;
    state.allowed = 0;
}

u64 vocabulary_allocations_granted() noexcept { return refusal_switch.granted; }

u64 vocabulary_blocks_released() noexcept { return refusal_switch.released; }

} // namespace stxlib::detail
