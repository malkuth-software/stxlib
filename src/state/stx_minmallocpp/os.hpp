#pragma once

// stx_minmallocpp's whole view of the host: address space reserved,
// committed, decommitted and released; a yield; a stop; and the thread-exit
// hook. Implemented once per host family under src/hosted/
// (posix/minmallocpp_os_posix.cpp, windows/minmallocpp_os_windows.cpp), the
// only places that name a host interface (AGENTS.md, "Hard boundaries").
//
// Early exit (AGENTS.md; the owner, 2026-11-15: "allocators must not
// unwind"). The host functions behind this interface are, on POSIX hosts,
// mmap, mprotect, munmap, sched_yield and sysconf, the pthread key calls
// pthread_key_create, pthread_setspecific and pthread_getspecific, and
// abort; on Windows, VirtualAlloc, VirtualFree, GetSystemInfo,
// SwitchToThread and abort. None can unwind. mmap, mprotect, munmap and
// sched_yield are in neither list of cancellation points (POSIX.1-2024,
// XSH 2.9.5.2, read 2026-11-15); sysconf and the three pthread key calls
// were not looked up there (unconfirmed). abort is called only by stop,
// which ends the process; it raises SIGABRT, so a handler the program
// installed runs first. The allocator makes each call with its tables
// whole and needs no cancellation guard.
// tools/check_stx_minmallocpp_surface.py checks this list against the
// compiled objects.
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
// thread_ending, on this thread, when it exits normally; false when the
// host cannot (the thread then never gets a heap of its own). On POSIX
// hosts thread_ending receives `context`; on Windows the host's hook runs
// for every thread and passes null. Process termination calls nothing (the
// owner's rule: termination frees nothing). thread_ending is the
// allocator's (allocator.cpp).
//
// To be replaced by one call from stxlib_state's own thread-exit hook once
// the thread records land (notes planning/stages/E-thread-records.md).
[[nodiscard]] bool arm_thread_exit(void* context) noexcept;

// What the calling thread armed and has not yet given up, or null. The
// allocator's thread_local pointer is a cache of this: on a host that
// frees a thread's thread_local storage before its exit hook runs
// (suspected of macOS), the pointer reads as fresh again while the thread
// still has a heap, and this says so.
[[nodiscard]] void* armed_context() noexcept;

// True once the calling thread's exit hook has run, for as long as the
// host keeps saying so. Such a thread is given no new heap.
[[nodiscard]] bool thread_has_ended() noexcept;

} // namespace stxlib::stx_minmallocpp::os

namespace stxlib::stx_minmallocpp {

// Called by the host layer at a thread's normal exit, with the context the
// thread armed or null (os::arm_thread_exit). Abandons the thread's pages
// and frees its heap slot.
void thread_ending(void* context) noexcept;

} // namespace stxlib::stx_minmallocpp
