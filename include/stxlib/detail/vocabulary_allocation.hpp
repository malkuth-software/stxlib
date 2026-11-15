#pragma once

// The one allocation path of the owned vocabulary (vector.hpp, text.hpp's
// Utf8String, unique.hpp, shared.hpp): STXlib's allocator (memory.hpp),
// through one out-of-line function so that tests can make it refuse.
//
// Why a seam: every operation of the vocabulary promises what a refused
// allocation leaves behind (house style, section 2). A promise about every
// allocation is checked by refusing each one in turn, which needs a switch
// the allocator itself does not offer. The switch is per thread, off unless
// a test arms it, and costs one thread-local read per allocation; it never
// changes what a successful allocation returns.

#include "stxlib/types.hpp"

namespace stxlib::detail {

// memory::allocate(size, alignment), unless this thread has armed refusals
// and the countdown has reached zero, in which case null (the same answer
// as an exhausted allocator). Never throws.
[[nodiscard]] void* vocabulary_allocate(usize size, usize alignment) noexcept;

// memory::release(address); null is a no-op.
void vocabulary_release(void* address) noexcept;

// Tests only. After `allowed` more vocabulary allocation calls on this
// thread, every further one is refused until disarm_vocabulary_refusals.
// arm(0) refuses the next one.
void arm_vocabulary_refusals(u64 allowed) noexcept;
void disarm_vocabulary_refusals() noexcept;

// Tests only: vocabulary allocations this thread has been granted, and
// blocks it has released, since it started. One grant takes at least one
// allocator call, so a 64-bit count does not wrap in any real run.
[[nodiscard]] u64 vocabulary_allocations_granted() noexcept;
[[nodiscard]] u64 vocabulary_blocks_released() noexcept;

} // namespace stxlib::detail
