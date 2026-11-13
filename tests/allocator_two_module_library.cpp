// The library side of tests/allocator_two_module_tests.cpp (tests/allocator_two_module.hpp).

#include "allocator_two_module.hpp"

#include <cstring>
#include <utility>

namespace stxlib_allocator_two_module {

namespace {

void add_one(const stxlib::StopSignal&, void* const address) noexcept {
    stxlib::u32& counter = *static_cast<stxlib::u32*>(address);
    counter = counter + 1;
}

} // namespace

void* library_allocate(const stxlib::usize size, const unsigned char fill) noexcept {
    void* const block = stxlib::memory::allocate(size, 16);
    if (block != nullptr) std::memset(block, fill, size);
    return block;
}

void library_release(void* const block) noexcept { stxlib::memory::release(block); }

stxlib::memory::Backend library_backend() noexcept { return stxlib::memory::backend(); }

bool library_tags_checked() noexcept { return stxlib::memory::tags_checked(); }

stxlib::Result<stxlib::Path> library_path(const stxlib::TextView text) noexcept {
    stxlib::Result<stxlib::Path> result = stxlib::Path::parse(text);
    return result;
}

stxlib::Result<stxlib::Thread> library_thread(stxlib::u32* const counter) noexcept {
    stxlib::Result<stxlib::Thread> result = stxlib::Thread::start(add_one, counter);
    return result;
}

stxlib::Result<stxlib::AtomicPool> library_pool(const stxlib::u32 thread_count) noexcept {
    stxlib::Result<stxlib::AtomicPool> result = stxlib::AtomicPool::create(thread_count);
    return result;
}

void library_destroy_path(stxlib::Result<stxlib::Path>&& path) noexcept {
    const stxlib::Result<stxlib::Path> destroyed = std::move(path);
    static_cast<void>(destroyed);
}

} // namespace stxlib_allocator_two_module
