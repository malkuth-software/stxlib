// The library side of tests/allocator_unload_tests.cpp: a module holding its
// own copy of STXlib, loaded and unloaded by a program that links none.

#include "stxlib/memory.hpp"

#include <cstring>

extern "C" __attribute__((visibility("default"))) int stxlib_unload_round() noexcept {
    // Grows the registry's table (sanitizer builds track every block), then
    // leaves no block tracked.
    void* const block = stxlib::memory::allocate(64, 16);
    if (block == nullptr) return 1;
    std::memset(block, 0x5a, 64);
    stxlib::memory::release(block);
    return 0;
}
