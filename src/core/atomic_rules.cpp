// Every atomic type STXlib uses is lock-free on every host it builds for
// (audit B, section 3.12 and R4). A std::atomic or std::atomic_ref of a type
// that is not lock-free compiles anyway and falls back to a lock in the
// toolchain's atomic library (libatomic, or compiler-rt's), which is a
// separate module, not async-signal-safe, and not address-free across
// processes. Stating it here turns that fallback into a build failure.
//
// tools/check_std_rules.py (--profile stxlib) reports any std::atomic<T> or
// std::atomic_ref<T> in src/ whose T has no assert in the checked tree, so a
// new atomic type is added here (or beside its use) in the same change.
// Object pointers share one assert (they are one size on every host).
//
// Lock-free does not mean inline. On Linux and FreeBSD aarch64 the
// compilers turn each read-modify-write of these types into a call to an
// outline-atomics helper (__aarch64_cas4_acq_rel and its kin), still
// lock-free: the helper runs one LSE instruction or a load-exclusive/
// store-exclusive loop. STXlib defines those helpers itself
// (src/hosted/elf/outline_atomics_aarch64.cpp), so these atomics reach
// neither the toolchain's runtime nor the C library's getauxval; the
// asserts below are unchanged by that.

#include "stxlib/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace stxlib {

namespace {

static_assert(std::atomic<bool>::is_always_lock_free, "atomic<bool> must be lock-free");
static_assert(std::atomic<u8>::is_always_lock_free, "atomic<u8> must be lock-free");
static_assert(std::atomic<u16>::is_always_lock_free, "atomic<u16> must be lock-free");
static_assert(std::atomic<u32>::is_always_lock_free, "atomic<u32> must be lock-free");
static_assert(std::atomic<u64>::is_always_lock_free, "atomic<u64> must be lock-free");
static_assert(std::atomic<i32>::is_always_lock_free, "atomic<i32> must be lock-free");
static_assert(std::atomic<int>::is_always_lock_free, "atomic<int> must be lock-free");
static_assert(std::atomic<std::size_t>::is_always_lock_free, "atomic<size_t> must be lock-free");
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free, "atomic<uintptr_t> must be lock-free");
static_assert(std::atomic<void*>::is_always_lock_free, "atomic of a pointer must be lock-free");

static_assert(std::atomic_ref<std::uint8_t>::is_always_lock_free, "atomic_ref<uint8_t> must be lock-free");
static_assert(std::atomic_ref<std::uint16_t>::is_always_lock_free, "atomic_ref<uint16_t> must be lock-free");
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free, "atomic_ref<uint32_t> must be lock-free");
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free, "atomic_ref<uint64_t> must be lock-free");
static_assert(std::atomic_ref<std::uintptr_t>::is_always_lock_free, "atomic_ref<uintptr_t> must be lock-free");

// The check folds int into i32 and every pointer into void*: true on every
// host STXlib builds for, and stated rather than assumed.
static_assert(sizeof(int) == sizeof(i32), "int is 32 bits on STXlib's hosts");
static_assert(sizeof(void*) == sizeof(std::uintptr_t), "a pointer fits uintptr_t exactly on STXlib's hosts");

} // namespace

} // namespace stxlib
