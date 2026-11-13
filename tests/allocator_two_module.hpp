#pragma once

// The other module of tests/allocator_two_module_tests.cpp: a shared library that
// links its own copy of stxlib::boundary (tests/allocator_two_module_library.cpp).
// What it makes, the test program releases, and the reverse.

#include "stxlib/memory.hpp"
#include "stxlib/path.hpp"
#include "stxlib/thread_pool.hpp"
#include "stxlib/threads.hpp"

#if defined(_WIN32)
#if defined(STXLIB_ALLOCATOR_TWO_MODULE_BUILDING)
#define STXLIB_ALLOCATOR_TWO_MODULE_API __declspec(dllexport)
#else
#define STXLIB_ALLOCATOR_TWO_MODULE_API __declspec(dllimport)
#endif
#else
#define STXLIB_ALLOCATOR_TWO_MODULE_API __attribute__((visibility("default")))
#endif

namespace stxlib_allocator_two_module {

// A block of `size` bytes from memory::allocate in the library, filled with
// `fill`; null when allocation fails.
STXLIB_ALLOCATOR_TWO_MODULE_API void* library_allocate(stxlib::usize size, unsigned char fill) noexcept;
// memory::release in the library.
STXLIB_ALLOCATOR_TWO_MODULE_API void library_release(void* block) noexcept;
// The library's view of the allocator, to compare with the program's.
STXLIB_ALLOCATOR_TWO_MODULE_API stxlib::memory::Backend library_backend() noexcept;
STXLIB_ALLOCATOR_TWO_MODULE_API bool library_tags_checked() noexcept;
// A Path parsed in the library (its storage allocated there).
STXLIB_ALLOCATOR_TWO_MODULE_API stxlib::Result<stxlib::Path> library_path(stxlib::TextView text) noexcept;
// A thread started in the library that adds 1 to *counter and returns.
STXLIB_ALLOCATOR_TWO_MODULE_API stxlib::Result<stxlib::Thread> library_thread(stxlib::u32* counter) noexcept;
// A pool created in the library.
STXLIB_ALLOCATOR_TWO_MODULE_API stxlib::Result<stxlib::AtomicPool> library_pool(stxlib::u32 thread_count) noexcept;
// Destroys, in the library, a Path the program made.
STXLIB_ALLOCATOR_TWO_MODULE_API void library_destroy_path(stxlib::Result<stxlib::Path>&& path) noexcept;

} // namespace stxlib_allocator_two_module
