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

// Zero-size and tiny blocks at large alignments, and the last block of a
// filled page: the release ownership check asks mimalloc about the header
// only, so none of these is refused (smi's mimalloc audit, A1 and A2; a
// refusal would end the process in a tagged build).
void edge_blocks_release() {
    for (stxlib::usize shift = 15; shift <= 21; ++shift) {
        const stxlib::usize alignment = static_cast<stxlib::usize>(1) << shift;
        void* const empty = stxlib::memory::allocate(0, alignment);
        void* const one = stxlib::memory::allocate(1, alignment);
        CHECK(empty != nullptr && one != nullptr);
        stxlib::memory::release(empty);
        stxlib::memory::release(one);
    }
    constexpr stxlib::usize filled = 4100;
    static void* blocks[filled]{};
    bool all_allocated = true;
    for (stxlib::usize index = 0; index < filled; ++index) {
        blocks[index] = stxlib::memory::allocate(0, 16);
        if (blocks[index] == nullptr) all_allocated = false;
    }
    CHECK(all_allocated);
    for (stxlib::usize index = 0; index < filled; ++index) stxlib::memory::release(blocks[index]);
    // An alignment above PTRDIFF_MAX is refused, not wrapped (A6): usize's
    // top bit at every width (a fixed << 63 is past usize's width on wasm32).
    CHECK(stxlib::memory::allocate(16, static_cast<stxlib::usize>(1) << (sizeof(stxlib::usize) * 8 - 1)) == nullptr);
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
#if defined(STXLIB_EXPECT_MIMALLOC)
    CHECK(stxlib::memory::backend() == stxlib::memory::Backend::mimalloc);
#elif defined(STXLIB_EXPECT_STX_MINMALLOCPP)
    CHECK(stxlib::memory::backend() == stxlib::memory::Backend::stx_minmallocpp);
#else
    CHECK(stxlib::memory::backend() == stxlib::memory::Backend::host);
#endif
}

} // namespace

int main() {
    round_trips_keep_alignment();
    zero_sizes_are_unique();
    edge_blocks_release();
    refusals();
    const int status = stxlib_tests::finish("stxlib memory");
    return status;
}
