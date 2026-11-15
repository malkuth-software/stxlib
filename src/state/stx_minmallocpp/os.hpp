#pragma once

// stx_minmallocpp's whole view of the host: address space reserved,
// committed, decommitted and released; a yield; a stop; and the thread-exit
// hook. Implemented once per host family under src/hosted/
// (posix/minmallocpp_os_posix.cpp, windows/minmallocpp_os_windows.cpp), the
// only places that name a host interface (AGENTS.md, "Hard boundaries").
//
// Early exit (AGENTS.md; the owner, 2026-11-15: "allocators must not
// unwind"). None of these calls can unwind, call back into STXlib, or act
// on a thread cancellation: on POSIX hosts they are mmap, mprotect, munmap
// and sched_yield, none of which is a cancellation point (POSIX.1-2024, XSH
// 2.9.5.2, read 2026-11-15: in neither list), and on Windows VirtualAlloc,
// VirtualFree and SwitchToThread. The allocator therefore makes each call
// with its tables whole and needs no cancellation guard.
//
// Nothing here reads the environment, writes to a stream, draws entropy or
// reads the clock.

#include "stxlib/types.hpp"

namespace stxlib::stx_minmallocpp::os {

// The host's page size in bytes: a power of two, or 0 when the host does
// not say. The allocator refuses to start unless it is a power of two no
// larger than a slice (65,536).
[[nodiscard]] usize page_bytes() noexcept;

// Reserves `bytes` of address space at a multiple of `alignment`, with no
// access and no memory behind it. `bytes` and `alignment` are multiples of
// the page size and `alignment` is a power of two. Null when the host
// refuses. At most a fixed number of host calls.
[[nodiscard]] void* reserve(usize bytes, usize alignment) noexcept;

// Makes [address, address + bytes), whole pages inside a reservation,
// readable and writable. Pages committed for the first time, or again after
// decommit, read as zero. False when the host refuses (its commit limit);
// the range is then as it was.
[[nodiscard]] bool commit(void* address, usize bytes) noexcept;

// Gives the memory behind [address, address + bytes) back to the host and
// removes access; the address space stays reserved. If the host refuses,
// the range stays committed, which costs memory and breaks nothing: no
// caller relies on the contents of a range it decommitted.
void decommit(void* address, usize bytes) noexcept;

// Releases a whole reservation made by reserve.
void release(void* address, usize bytes) noexcept;

// Lets another thread run; used only while waiting for a lock.
void yield() noexcept;

// Ends the process abnormally. Called when the allocator finds its own
// state contradicted (a block released twice across threads).
[[noreturn]] void stop() noexcept;

// The thread-exit hook. arm_thread_exit asks the host to call
// thread_ending(context), on this thread, when it exits normally; false
// when the host cannot (the thread then never gets a heap of its own).
// Process termination calls nothing (the owner's rule: termination frees
// nothing). thread_ending is the allocator's (allocator.cpp).
//
// To be replaced by one call from stxlib_state's own thread-exit hook once
// the thread records land (notes planning/stages/E-thread-records.md).
[[nodiscard]] bool arm_thread_exit(void* context) noexcept;

} // namespace stxlib::stx_minmallocpp::os

namespace stxlib::stx_minmallocpp {

// Called by the host layer at a thread's normal exit with the context that
// thread armed. Abandons the thread's pages and frees its heap slot.
void thread_ending(void* context) noexcept;

} // namespace stxlib::stx_minmallocpp
