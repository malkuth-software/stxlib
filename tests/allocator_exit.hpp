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

// Names the file the detach-time destructor writes "ok" to once its own
// allocations have succeeded. `path` is copied (at most 1,023 bytes).
STXLIB_ALLOCATOR_EXIT_API void arm_detach_check(const char* path) noexcept;

} // namespace stxlib_allocator_exit
