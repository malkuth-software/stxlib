#pragma once

// The library of tests/allocator_exit_tests.cpp (tests/allocator_exit_library.cpp):
// threads that allocate and release through stxlib::memory until the process
// ends, and an object whose destructor allocates through it while the library
// is detached, after the host has stopped those threads.

#include "stxlib/types.hpp"

#if defined(_WIN32)
#if defined(STXLIB_ALLOCATOR_EXIT_BUILDING)
#define STXLIB_ALLOCATOR_EXIT_API __declspec(dllexport)
#else
#define STXLIB_ALLOCATOR_EXIT_API __declspec(dllimport)
#endif
#else
#define STXLIB_ALLOCATOR_EXIT_API __attribute__((visibility("default")))
#endif

namespace stxlib_allocator_exit {

// Starts `count` detached threads, each allocating and releasing blocks of
// mixed sizes, a 3,000,000-byte one (the allocator's structure lock) among
// them, until the process ends. Returns how many started.
STXLIB_ALLOCATOR_EXIT_API stxlib::u32 start_workers(stxlib::u32 count) noexcept;

// Names the file the detach-time destructor writes its "ok ..." line to once
// its own allocations have returned. `path` is copied (at most 1,023 bytes).
// `warm`: the program's main thread has allocated and released blocks of
// warm_sizes before returning from main, so the destructor, which runs on
// that thread, also checks that those sizes are served from the thread's own
// cache while a warm_large_size request is still refused.
STXLIB_ALLOCATOR_EXIT_API void arm_detach_check(const char* path, bool warm) noexcept;

// The small sizes the warm case caches on the main thread and asks for again
// at detach (alignment 16).
inline constexpr stxlib::usize warm_sizes[] = {16, 48, 256, 1000};
inline constexpr stxlib::usize warm_size_count = sizeof(warm_sizes) / sizeof(warm_sizes[0]);
// A request no thread cache serves (a singleton: the slow path).
inline constexpr stxlib::usize warm_large_size = 3000000;

} // namespace stxlib_allocator_exit
