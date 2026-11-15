// Row 19: memory::allocate and memory::release with allocate_tagged,
// release_tagged, tag_for and reject_release, allocation tags on (B1.md
// section 3.1).
// P0: no undefined behaviour.
// P1 (ownership), over any sequence of up to three calls:
//   - a block from allocate is released to the backend once;
//   - a second release, or a release of an address allocate did not return,
//     reaches fail() (through reject_release) before any backend call;
//   - a non-power-of-two alignment, and a size whose size + offset would
//     wrap, return null;
//   - a returned address is aligned to max(alignment, 16) and its `size`
//     bytes are writable.
// Excluded, as the source states (src/hosted/memory.cpp:20-22): a second
// release of an address that has since been allocated again; the harness
// assumes it away and the assumption is recorded.
// Environment (models below, recorded): the backend returns a fresh block
// aligned to the requested alignment, or null, and refuses sizes above
// 4096; host entropy is available (random::fill_entropy returns true with
// arbitrary bytes; the steady-clock fallback is outside this row); fail()
// ends the path where a rejection is expected and is a violation elsewhere.
// Bound: 3 calls; size and alignment symbolic; unwind 9 (the 8-byte
//        entropy fill).
#include "verify.h"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>

#include "memory_backend.hpp"
#include "stx_auxlib/memory.hpp"
#include "stx_auxlib/process.hpp"
#include "stx_auxlib/random.hpp"
//@ pin source=stx_auxlib path=src/hosted/memory_backend.hpp sha256=fc52778b99851ecc47891599438f20ba3bd1ee573caba22cc3c705bb3bd232b2
//@ pin source=stx_auxlib path=include/stx_auxlib/memory.hpp sha256=fdf7477c7cd2c717348b8416b54c48a271a4f1a3c3cf1d27e715ce292ec42dd3
//@ pin source=stx_auxlib path=include/stx_auxlib/process.hpp sha256=167df96da0b91ae17c5c0f1a04069b4691a1459c753d46c107cb853d634c96d6
//@ pin source=stx_auxlib path=include/stx_auxlib/random.hpp sha256=07cfb2c525d851ffb85e7770b31148342dd9ba629c4aaa044e67de3a019450bd

//@ model-begin reason="harness state shared by the environment models below"
namespace harness {
constexpr std::size_t slots = 4;
void* blocks[slots] = {nullptr, nullptr, nullptr, nullptr};
bool live[slots] = {false, false, false, false};
std::size_t block_count = 0;
bool rejection_expected = false;
} // namespace harness
//@ model-end

//@ model-begin reason="the allocator backend (memory_mimalloc.cpp or memory_host.cpp): a fresh aligned block or null"
namespace stx_auxlib::memory::backend_detail {
const Backend linked_backend = Backend::host;

void* allocate_aligned_block(const std::size_t size, const std::size_t alignment) noexcept {
    if (size > 4096 || harness::block_count == harness::slots || nondet_bool()) return nullptr;
    void* const block = std::malloc(size);
    if (block == nullptr) return nullptr;
    STX_ASSUME(reinterpret_cast<std::uintptr_t>(block) % alignment == 0);
    harness::blocks[harness::block_count] = block;
    harness::live[harness::block_count] = true;
    harness::block_count += 1;
    return block;
}

void release_aligned_block(void* const address) noexcept {
    STX_ASSERT(!harness::rejection_expected, "P1: a rejected release reached the backend");
    bool found = false;
    for (std::size_t index = 0; index < harness::block_count; ++index) {
        if (harness::live[index] && harness::blocks[index] == address) {
            harness::live[index] = false;
            found = true;
        }
    }
    STX_ASSERT(found, "P1: the backend receives only its own live blocks");
    std::free(address);
}

bool owns_block(const void* const address) noexcept {
    for (std::size_t index = 0; index < harness::block_count; ++index) {
        if (harness::live[index] && harness::blocks[index] == address) return true;
    }
    return false;
}
} // namespace stx_auxlib::memory::backend_detail
//@ model-end

//@ model-begin reason="host entropy (src/hosted/entropy_posix.cpp): available, arbitrary bytes"
namespace stx_auxlib::random {
bool fill_entropy(const std::span<std::byte> output) noexcept {
    for (std::size_t index = 0; index < output.size(); ++index) output[index] = static_cast<std::byte>(nondet_uchar());
    return true;
}
} // namespace stx_auxlib::random
//@ model-end

//@ model-begin reason="stx_auxlib::fail (src/hosted/process.cpp) reports and aborts; here it ends the path where a rejection is expected"
namespace stx_auxlib {
[[noreturn]] void fail(const std::string_view message, const std::source_location location) noexcept {
    (void)message;
    (void)location;
    STX_ASSERT(harness::rejection_expected, "P1: fail() only where a release must be rejected");
    STX_ASSUME(false);
    std::abort();
}
} // namespace stx_auxlib
//@ model-end

//@ lift source=stx_auxlib path=src/hosted/memory.cpp lines=52-171 sha256=0cd2a246c011853a66c05e849db698ad7cba8f3111344b1eb93463f8325a635b
//@ mutant id=m1 source=stx_auxlib path=src/hosted/memory.cpp line=118 from="if (tag != tag_for(address)) reject_release();" to=""

namespace memory = stx_auxlib::memory;

namespace {

bool handed_and_owned(void* const* handed, const bool* owned, const std::size_t count, const void* address) {
    for (std::size_t index = 0; index < count; ++index) {
        if (owned[index] && handed[index] == address) return true;
    }
    return false;
}

} // namespace

int main() {
    void* handed[3] = {nullptr, nullptr, nullptr};
    bool owned[3] = {false, false, false};
    std::size_t count = 0;
    alignas(16) unsigned char foreign[64] = {};
    for (std::size_t step = 0; step < 3; ++step) {
        const unsigned char action = nondet_uchar();
        if (action == 0) {
            const std::size_t size = nondet_size();
            const std::size_t alignment = nondet_size();
            void* const block = memory::allocate(size, alignment);
            const std::size_t offset = alignment < 16 ? 16 : alignment;
            if (!std::has_single_bit(alignment)) STX_ASSERT(block == nullptr, "P1: a non-power-of-two alignment gives null");
            if (size > std::numeric_limits<std::size_t>::max() - offset) STX_ASSERT(block == nullptr, "P1: a wrapping size gives null");
            if (block != nullptr) {
                STX_ASSERT(reinterpret_cast<std::uintptr_t>(block) % offset == 0, "P1: aligned to max(alignment, 16)");
                if (size > 0) {
                    static_cast<unsigned char*>(block)[0] = 1;
                    static_cast<unsigned char*>(block)[size - 1] = 1;
                }
            }
            handed[count] = block;
            owned[count] = block != nullptr;
            count += 1;
        } else if (action == 1 && count > 0) {
            const std::size_t index = nondet_size();
            STX_ASSUME(index < count);
            if (owned[index]) {
                memory::release(handed[index]);
                owned[index] = false;
            } else if (handed[index] == nullptr) {
                memory::release(nullptr);
            } else {
                // The source's stated exclusion: the address is not live again.
                STX_ASSUME(!handed_and_owned(handed, owned, count, handed[index]));
                harness::rejection_expected = true;
                memory::release(handed[index]);
                STX_ASSERT(false, "P1: a second release returned instead of failing");
            }
        } else if (action == 2) {
            harness::rejection_expected = true;
            memory::release(foreign + 32);
            STX_ASSERT(false, "P1: a foreign release returned instead of failing");
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (owned[index]) memory::release(handed[index]);
    }
    for (std::size_t index = 0; index < harness::block_count; ++index)
        STX_ASSERT(!harness::live[index], "P1: every block is released once");
    STX_VACUITY_PROBE();
    return 0;
}
