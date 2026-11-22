#pragma once

// stx_minmallocpp: STXlib's own allocator. A conversion of mimalloc v3's
// design to house-style C++ (the owner, 2026-11-15), for stxlib_state's
// allocator calls (state.hpp, AllocatorCalls). layout.hpp, page.hpp,
// region.hpp and heap.hpp state the structure; this is the contract.
//
// Ownership. A block from allocate belongs to its caller until it is passed
// to release, once, from any thread. The allocator owns every byte of
// address space it reserves and returns it to the host only through its own
// calls; it never frees memory it did not reserve.
//
// Failure. Exhaustion (the host refuses address space or memory, or a fixed
// table is full) is a null result at the call and changes nothing else: no
// errno, no output, no waiting, no handler. A release of an address that is
// not the start of a live block is `refused`, decided without reading or
// writing the address or anything near it, in every build. Finding the
// allocator's own state contradicted (a block released twice from two
// threads, seen only when the two releases meet) stops the process.
//
// What it never does: throw, call back into any other code, call a
// new-handler, read the environment, write to a stream, read the clock,
// draw entropy, or make a host call that is a cancellation point (os.hpp).
// It never reads or writes the memory of a block, live or free.
//
// Threads. Any thread may call anything here at any time, including during
// its own exit and after its exit hook has run. A thread's first allocation
// gives it a heap; its normal exit gives that heap's pages up. Process
// termination releases nothing. Not async-signal-safe. Not usable in the
// child of a fork of a multithreaded process.
//
// Process end (Windows; the logic review's L-05, the security audit's
// A-stxlib-allocator-state-01-F2). ExitProcess stops every other thread
// where it stands and then runs the modules' DLL_PROCESS_DETACH code on
// the exiting thread; a stopped thread may hold one of the allocator's
// locks, which nothing will ever let go. Once os::process_ending() says so,
// the allocator takes no lock: allocate still serves a block its common
// path can take (it takes no lock) and returns null for anything else;
// release finishes on its common path and otherwise returns released at
// once, leaving the block where it is, and no longer refuses what it
// cannot check; trim and end_thread return having given back nothing. The
// memory goes with the process, as termination's rule says (rule 1:
// termination releases nothing). Only the exiting thread runs then, so the
// moment the answer turns true needs no ordering with other threads.
// The host backend is stricter (src/state/allocator.cpp): there every
// allocation fails and every release returns at once, whatever the size
// (memory.hpp, "Process end", compares them).
//
// Not yet built, run or proved (proofs/REPORT.md, stx_minmallocpp).

#include "stxlib/types.hpp"

// ic: stxlib::stx_minmallocpp: generated from src/state/stx_minmallocpp/allocator.ic.toml by tools/ic_declarations.py;
// ic: edit the declaration, not these lines. Thread: any, unless a call says otherwise. Identity: block address: the
// ic: start of a live block; a stale one: returns Release::refused, unchanged.
namespace stxlib::stx_minmallocpp {

enum class Release : u8 {
    released = 0,
    // The address is not the start of a live block of this allocator.
    refused = 1,
};

// `size` bytes aligned to `alignment`. A size of zero yields a unique
// block. Null when alignment is not a power of two or exceeds a region
// (1 GiB; 64 MiB with 32-bit addresses, layout.hpp), when size exceeds
// half the address space, or on exhaustion. The contents are
// indeterminate. Blocks of up to 512 KiB with an alignment of up to 64 KiB
// cost a fixed number of steps on the fast path.
// ic: Requires power_of_two(alignment) and alignment <= alignment_limit and size <= largest_request, else returns null,
// ic: unchanged. Fails exhausted (the host refuses address space or memory, or a fixed table is full: a null result,
// ic: nothing else changes), unchanged. Returns void*, owned by the caller.
[[nodiscard]] void* allocate(usize size, usize alignment) noexcept;

// Releases a block from allocate. Refuses null, an address outside the
// allocator's address space, an address inside its metadata, an address
// that is not a block's start, and a block that is already free.
// ic: Transfers the block from caller to allocator.
[[nodiscard]] Release release(void* address) noexcept;

// Merges releases from other threads into the calling thread's pages,
// frees its empty pages, and returns idle memory to the host. A
// maintenance call for a quiet moment; proportional to the pages the
// thread owns and to the number of regions.
void trim() noexcept;

// Gives up the calling thread's heap now, as its exit would: its pages are
// abandoned (or freed if empty) and its slot returned. The thread may go on
// allocating and releasing; it is then served as a thread without a heap.
// For a thread that wants its memory handed on before it exits, and for
// STXlib's own thread-exit hook to call once it replaces the allocator's.
void end_thread() noexcept;

} // namespace stxlib::stx_minmallocpp
