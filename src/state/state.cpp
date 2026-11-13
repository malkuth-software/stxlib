// The stxlib_state module (state.hpp): the one owner of STXlib's
// per-thread and per-process state. Built as a shared library whenever
// STXlib may be linked into more than one module (STXLIB_STATE_SHARED,
// CMakeLists.txt). It calls nothing, so it links on its own.

#include "state/state.hpp"

namespace stxlib::state {

namespace {

constinit thread_local ThreadState thread_state_instance{};
constinit ProcessState process_state_instance{};

} // namespace

ThreadState* thread_state(const u32 expected_layout) noexcept {
    if (expected_layout != layout_version) return nullptr;
    ThreadState* const result = &thread_state_instance;
    return result;
}

ProcessState* process_state(const u32 expected_layout) noexcept {
    if (expected_layout != layout_version) return nullptr;
    ProcessState* const result = &process_state_instance;
    return result;
}

} // namespace stxlib::state
