// Tests of stx_minmallocpp itself (src/state/stx_minmallocpp/allocator.hpp),
// linked directly into this program, so they run in every configuration,
// whatever backend stxlib_state has. Tested over the cases below, not
// proved (proofs/REPORT.md, stx_minmallocpp).
//
// What they exercise:
//   - every size class (one block of each class's size and of one byte
//     more than the class before it), the page kinds, alignments on both
//     sides of 64 KiB, pages of one block, and mappings of one block;
//   - blocks are distinct, do not overlap, and keep what was written;
//   - a page filled past its capacity (full pages abandoned, claimed again
//     by the release that follows);
//   - every refusal: null, an address that is not ours, an address in a
//     region's metadata, an address inside a block, the unused tail of a
//     page, and a second release, in the same thread and across threads;
//   - releases from another thread, a thread that exits holding blocks, a
//     thread that exits leaving a part-used page, and threads that hand
//     blocks round a ring;
//   - exhaustion and bad arguments as null; trim.

#include "state/stx_minmallocpp/allocator.hpp"
#include "state/stx_minmallocpp/layout.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

namespace {

using stxlib::usize;
using stxlib::stx_minmallocpp::allocate;
using stxlib::stx_minmallocpp::Release;
using stxlib::stx_minmallocpp::release;

[[nodiscard]] bool aligned_to(const void* const block, const usize alignment) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(block);
    const bool result = (address & static_cast<std::uintptr_t>(alignment - 1)) == 0;
    return result;
}

void round_trips() {
    const usize sizes[] = {0,     1,      15,     16,      17,      100,     128,     129,       160,
                           161,   1000,   4096,   8192,    8193,    60000,   65536,   65537,     300000,
                           524288, 524289, 600000, 5000000, 33554432, 33554433, 40000000};
    bool all_allocated = true;
    bool all_aligned = true;
    bool all_released = true;
    for (const usize size : sizes) {
        for (usize alignment = 1; alignment <= static_cast<usize>(1) << 21; alignment *= 4) {
            void* const block = allocate(size, alignment);
            if (block == nullptr) {
                all_allocated = false;
                continue;
            }
            if (!aligned_to(block, alignment) || !aligned_to(block, 16)) all_aligned = false;
            if (size != 0) {
                unsigned char* const bytes = static_cast<unsigned char*>(block);
                bytes[0] = 0x5a;
                bytes[size - 1] = 0xa5;
            }
            if (release(block) != Release::released) all_released = false;
        }
    }
    CHECK(all_allocated);
    CHECK(all_aligned);
    CHECK(all_released);
}

void every_class() {
    using stxlib::stx_minmallocpp::class_count;
    using stxlib::stx_minmallocpp::class_table;
    bool all_good = true;
    usize previous = 0;
    for (usize index = 0; index < class_count; ++index) {
        const usize block_bytes = class_table.shapes[index].block_bytes;
        // The class's largest and smallest request.
        unsigned char* const largest = static_cast<unsigned char*>(allocate(block_bytes, 16));
        unsigned char* const smallest = static_cast<unsigned char*>(allocate(previous + 1, 16));
        if (largest == nullptr || smallest == nullptr || largest == smallest) {
            all_good = false;
        } else {
            largest[0] = 1;
            largest[block_bytes - 1] = 2;
            smallest[previous] = 3;
            if (largest[0] != 1 || largest[block_bytes - 1] != 2 || smallest[previous] != 3) all_good = false;
        }
        if (largest != nullptr && release(largest) != Release::released) all_good = false;
        if (smallest != nullptr && release(smallest) != Release::released) all_good = false;
        previous = block_bytes;
    }
    CHECK(all_good);
}

// Allocates `count` blocks of `size`, checks they are distinct and do not
// overlap, fills each with its own byte, checks the fill, and releases
// them in the order `step` gives (a stride coprime to count).
void fill_and_drain(const usize size, const usize count, const usize step) {
    std::vector<unsigned char*> blocks(count, nullptr);
    bool all_allocated = true;
    for (usize index = 0; index < count; ++index) {
        blocks[index] = static_cast<unsigned char*>(allocate(size, 16));
        if (blocks[index] == nullptr) {
            all_allocated = false;
            continue;
        }
        std::memset(blocks[index], static_cast<int>(index & 0xff), size);
    }
    CHECK(all_allocated);
    if (!all_allocated) return;
    // As integers: ordering and subtracting pointers into different blocks
    // is not defined.
    std::vector<std::uintptr_t> sorted(count, 0);
    for (usize index = 0; index < count; ++index) {
        sorted[index] = reinterpret_cast<std::uintptr_t>(blocks[index]);
    }
    std::sort(sorted.begin(), sorted.end());
    bool apart = true;
    for (usize index = 1; index < count; ++index) {
        if (sorted[index] - sorted[index - 1] < size) apart = false;
    }
    CHECK(apart);
    bool kept = true;
    for (usize index = 0; index < count; ++index) {
        const unsigned char expected = static_cast<unsigned char>(index & 0xff);
        if (blocks[index][0] != expected || blocks[index][size - 1] != expected) kept = false;
    }
    CHECK(kept);
    bool all_released = true;
    usize position = 0;
    for (usize released = 0; released < count; ++released) {
        if (release(blocks[position]) != Release::released) all_released = false;
        position = (position + step) % count;
    }
    CHECK(all_released);
}

void pages_fill_and_drain() {
    // More blocks than one page holds, for a class of each page kind.
    fill_and_drain(16, 10000, 7);
    fill_and_drain(48, 5000, 3);
    fill_and_drain(1024, 300, 7);
    fill_and_drain(20000, 100, 3);
    fill_and_drain(200000, 40, 3);
    // More pages than a heap claims back of one class (64 small, 8
    // medium): the releases into the rest leave them abandoned.
    fill_and_drain(1024, 6000, 7);
    fill_and_drain(65536, 100, 7);
    // And again, so released pages and blocks are used a second time.
    fill_and_drain(16, 10000, 3);
    fill_and_drain(200000, 40, 7);
}

void refusals() {
    CHECK(release(nullptr) == Release::refused);
    int on_the_stack = 0;
    CHECK(release(&on_the_stack) == Release::refused);
    static int in_the_image = 0;
    CHECK(release(&in_the_image) == Release::refused);

    unsigned char* const block = static_cast<unsigned char*>(allocate(100, 16));
    CHECK(block != nullptr);
    if (block == nullptr) return;
    // Inside the block, and not a block start (blocks of this class are 112
    // bytes apart).
    CHECK(release(block + 8) == Release::refused);
    CHECK(release(block + 16) == Release::refused);
    // The region's metadata: its first slices.
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(block);
    const std::uintptr_t region = address & ~static_cast<std::uintptr_t>(stxlib::stx_minmallocpp::region_bytes - 1);
    CHECK(release(reinterpret_cast<void*>(region)) == Release::refused);
    CHECK(release(reinterpret_cast<void*>(region + 5 * stxlib::stx_minmallocpp::slice_bytes)) == Release::refused);
    // The unused tail of the page: 65,536 = 585 * 112 + 16.
    const std::uintptr_t page = address & ~static_cast<std::uintptr_t>(stxlib::stx_minmallocpp::slice_bytes - 1);
    CHECK(release(reinterpret_cast<void*>(page + 585 * 112)) == Release::refused);
    // Once, and not twice.
    CHECK(release(block) == Release::released);
    CHECK(release(block) == Release::refused);

    // A page of one block, and a mapping of one block.
    unsigned char* const alone = static_cast<unsigned char*>(allocate(600000, 16));
    unsigned char* const mapped = static_cast<unsigned char*>(allocate(40000000, 16));
    CHECK(alone != nullptr && mapped != nullptr);
    if (alone == nullptr || mapped == nullptr) return;
    CHECK(release(alone + 16) == Release::refused);
    CHECK(release(alone + stxlib::stx_minmallocpp::slice_bytes) == Release::refused);
    CHECK(release(alone) == Release::released);
    CHECK(release(alone) == Release::refused);
    CHECK(release(mapped + 4096) == Release::refused);
    CHECK(release(mapped) == Release::released);
    CHECK(release(mapped) == Release::refused);
}

void bad_requests() {
    CHECK(allocate(16, 0) == nullptr);
    CHECK(allocate(16, 3) == nullptr);
    CHECK(allocate(16, static_cast<usize>(1) << 31) == nullptr);
    CHECK(allocate(stxlib::usize_max, 16) == nullptr);
    CHECK(allocate(stxlib::usize_max - 2, 4096) == nullptr);
    // More than any host maps: exhaustion is a null result. With 32-bit
    // addresses (wasm32) the largest request considered is 2 GiB less one
    // byte, and a mapping of nearly 2 GiB cannot be had beside the program
    // under Emscripten's default maximum memory of 2 GiB.
    if constexpr (stxlib::stx_minmallocpp::wide_addresses) {
        CHECK(allocate(stxlib::usize_max >> 4, 16) == nullptr);
    } else {
        CHECK(allocate(stxlib::stx_minmallocpp::largest_request - stxlib::stx_minmallocpp::slice_bytes, 16) == nullptr);
    }
    void* const still_works = allocate(64, 64);
    CHECK(still_works != nullptr);
    CHECK(release(still_works) == Release::released);
}

void large_alignments() {
    const usize alignments[] = {static_cast<usize>(1) << 17, static_cast<usize>(1) << 20, static_cast<usize>(1) << 24,
                                static_cast<usize>(1) << 26, static_cast<usize>(1) << 30};
    bool all_good = true;
    bool beyond_refused = true;
    for (const usize alignment : alignments) {
        // Above a region (64 MiB with 32-bit addresses) an alignment is
        // refused.
        if (alignment > stxlib::stx_minmallocpp::alignment_limit) {
            if (allocate(1, alignment) != nullptr) beyond_refused = false;
            continue;
        }
        void* const small = allocate(1, alignment);
        void* const large = allocate(3000000, alignment);
        if (small == nullptr || large == nullptr || !aligned_to(small, alignment) || !aligned_to(large, alignment)) {
            all_good = false;
        }
        if (small != nullptr && release(small) != Release::released) all_good = false;
        if (large != nullptr && release(large) != Release::released) all_good = false;
    }
    CHECK(all_good);
    CHECK(beyond_refused);
}

// ---- threads ----------------------------------------------------------------------

struct Batch final {
    std::vector<void*> blocks{};
    bool allocated{true};
};

void allocate_batch(Batch& batch, const usize size, const usize count) {
    batch.blocks.assign(count, nullptr);
    for (usize index = 0; index < count; ++index) {
        batch.blocks[index] = allocate(size, 16);
        if (batch.blocks[index] == nullptr) {
            batch.allocated = false;
            continue;
        }
        std::memset(batch.blocks[index], 0x3c, size);
    }
}

struct ReleaseTally final {
    usize released{0};
    usize refused{0};
};

void release_batch(const Batch& batch, ReleaseTally& tally) {
    for (void* const block : batch.blocks) {
        if (block == nullptr) continue;
        if (release(block) == Release::released) {
            tally.released = tally.released + 1;
        } else {
            tally.refused = tally.refused + 1;
        }
    }
}

void released_by_another_thread() {
    // This thread allocates; another, which has no heap of its own,
    // releases. The pages that had filled were abandoned: the releases
    // leave them so, list each when its first block comes back, and free
    // each when its last does. The page still current here gets remote
    // bits, merged when this thread releases or allocates again.
    Batch batch{};
    allocate_batch(batch, 64, 20000);
    CHECK(batch.allocated);
    ReleaseTally tally{};
    std::thread other(release_batch, std::cref(batch), std::ref(tally));
    other.join();
    CHECK(tally.released == 20000 && tally.refused == 0);
    // A second release, from this thread, of blocks another thread released.
    CHECK(release(batch.blocks[0]) == Release::refused);
    CHECK(release(batch.blocks[19999]) == Release::refused);
    Batch again{};
    allocate_batch(again, 64, 20000);
    CHECK(again.allocated);
    ReleaseTally own{};
    release_batch(again, own);
    CHECK(own.released == 20000 && own.refused == 0);
}

void thread_exits_holding_blocks() {
    // A thread allocates and exits; its pages are abandoned with every
    // block live. This thread releases them all, which claims the pages.
    Batch batch{};
    std::thread worker(allocate_batch, std::ref(batch), static_cast<usize>(256), static_cast<usize>(3000));
    worker.join();
    CHECK(batch.allocated);
    bool kept = true;
    for (void* const block : batch.blocks) {
        const unsigned char* const bytes = static_cast<const unsigned char*>(block);
        if (bytes == nullptr || bytes[0] != 0x3c || bytes[255] != 0x3c) kept = false;
    }
    CHECK(kept);
    ReleaseTally tally{};
    release_batch(batch, tally);
    CHECK(tally.released == 3000 && tally.refused == 0);
}

void allocate_and_release_half(Batch& batch, const usize size, const usize count) {
    allocate_batch(batch, size, count);
    for (usize index = 0; index < count; index += 2) {
        if (batch.blocks[index] != nullptr && release(batch.blocks[index]) == Release::released) {
            batch.blocks[index] = nullptr;
        }
    }
}

void thread_exits_leaving_part_used_pages() {
    // The pages the worker leaves have free blocks: they go on the class's
    // abandoned list, and this thread's allocations of that class adopt
    // them.
    Batch left{};
    std::thread worker(allocate_and_release_half, std::ref(left), static_cast<usize>(512), static_cast<usize>(2000));
    worker.join();
    CHECK(left.allocated);
    Batch mine{};
    allocate_batch(mine, 512, 2000);
    CHECK(mine.allocated);
    // Nothing handed to this thread is a block the worker still holds.
    std::vector<std::uintptr_t> held{};
    for (void* const block : left.blocks) {
        if (block != nullptr) held.push_back(reinterpret_cast<std::uintptr_t>(block));
    }
    std::sort(held.begin(), held.end());
    bool disjoint = true;
    for (void* const block : mine.blocks) {
        if (std::binary_search(held.begin(), held.end(), reinterpret_cast<std::uintptr_t>(block))) disjoint = false;
    }
    CHECK(disjoint);
    ReleaseTally tally{};
    release_batch(mine, tally);
    release_batch(left, tally);
    CHECK(tally.released == 3000 && tally.refused == 0);
}

// One slot between two neighbours of the ring.
struct Slot final {
    std::atomic<void*> block{nullptr};
};

struct RingTally final {
    std::atomic<usize> failures{0};
};

// Allocates blocks and puts them in `out`; takes blocks from `in` and
// releases them. A block is released by the neighbour that took it, or by
// the thread that made it when the neighbour did not.
void ring_member(Slot& in, Slot& out, RingTally& tally, const usize rounds, const unsigned char mark) {
    for (usize round = 0; round < rounds; ++round) {
        const usize size = 16 + (round % 40) * 24;
        unsigned char* const made = static_cast<unsigned char*>(allocate(size, 16));
        if (made == nullptr) {
            static_cast<void>(tally.failures.fetch_add(1));
            continue;
        }
        made[0] = mark;
        made[size - 1] = mark;
        // What the neighbour did not take comes back and is released here;
        // nobody waits, so a member that finishes early blocks no one.
        void* const displaced = out.block.exchange(made);
        if (displaced != nullptr && release(displaced) != Release::released) static_cast<void>(tally.failures.fetch_add(1));
        void* const taken = in.block.exchange(nullptr);
        if (taken != nullptr && release(taken) != Release::released) static_cast<void>(tally.failures.fetch_add(1));
    }
}

void blocks_round_a_ring() {
    constexpr usize members = 4;
    constexpr usize rounds = 50000;
    Slot slots[members]{};
    RingTally tally{};
    std::vector<std::thread> threads{};
    for (usize member = 0; member < members; ++member) {
        threads.emplace_back(ring_member, std::ref(slots[member]), std::ref(slots[(member + 1) % members]), std::ref(tally),
                             rounds, static_cast<unsigned char>(member + 1));
    }
    for (std::thread& thread : threads) thread.join();
    // What is still in the slots was allocated and never taken.
    bool drained = true;
    for (Slot& slot : slots) {
        void* const left = slot.block.exchange(nullptr);
        if (left != nullptr && release(left) != Release::released) drained = false;
    }
    CHECK(drained);
    CHECK(tally.failures.load() == 0);
}

void trim_then_use() {
    stxlib::stx_minmallocpp::trim();
    fill_and_drain(96, 4000, 7);
    stxlib::stx_minmallocpp::trim();
    void* const block = allocate(32, 16);
    CHECK(block != nullptr);
    CHECK(release(block) == Release::released);
}

} // namespace

int main() {
    round_trips();
    every_class();
    pages_fill_and_drain();
    refusals();
    bad_requests();
    large_alignments();
    released_by_another_thread();
    thread_exits_holding_blocks();
    thread_exits_leaving_part_used_pages();
    blocks_round_a_ring();
    trim_then_use();
    const int status = stxlib_tests::finish("stxlib stx_minmallocpp");
    return status;
}
