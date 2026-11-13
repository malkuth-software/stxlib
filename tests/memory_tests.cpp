// Tests of stxlib::memory (round trips, alignment, refusals). The must-end
// cases of the ownership tags are in death_tests.cpp. Tested over the grid
// below, not proved (proofs/REPORT.md).

#include "stxlib/memory.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <cstring>

namespace {

using stxlib::usize;

void round_trips_keep_alignment() {
    bool all_allocated = true;
    bool all_aligned = true;
    for (usize alignment = 1; alignment <= 4096; alignment *= 2) {
        for (usize size = 0; size <= 300; size += 37) {
            void* const block = stxlib::memory::allocate(size, alignment);
            if (block == nullptr) {
                all_allocated = false;
                continue;
            }
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(block);
            if ((address & static_cast<std::uintptr_t>(alignment - 1)) != 0) all_aligned = false;
            std::memset(block, 0x5a, size);
            stxlib::memory::release(block);
        }
    }
    CHECK(all_allocated);
    CHECK(all_aligned);
}

void zero_sizes_are_unique() {
    void* const first = stxlib::memory::allocate(0, 8);
    void* const second = stxlib::memory::allocate(0, 8);
    CHECK(first != nullptr && second != nullptr && first != second);
    stxlib::memory::release(first);
    stxlib::memory::release(second);
}

void refusals() {
    CHECK(stxlib::memory::allocate(16, 0) == nullptr);
    CHECK(stxlib::memory::allocate(16, 3) == nullptr);
    CHECK(stxlib::memory::allocate(16, 24) == nullptr);
    CHECK(stxlib::memory::allocate(stxlib::usize_max, 16) == nullptr);
    // Refused before the host allocator is asked, in both modes (a sanitizer
    // would otherwise report the huge request itself).
    CHECK(stxlib::memory::allocate(stxlib::usize_max - 2, 4096) == nullptr);
    stxlib::memory::release(nullptr);
    CHECK(stxlib::memory::backend() == stxlib::memory::Backend::host);
}

} // namespace

int main() {
    round_trips_keep_alignment();
    zero_sizes_are_unique();
    refusals();
    const int status = stxlib_tests::finish("stxlib memory");
    return status;
}
