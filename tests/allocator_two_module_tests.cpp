// Blocks of STXlib's allocator cross modules (include/stxlib/memory.hpp):
// this program and a shared library, each with its own copy of
// stxlib::boundary, allocate and release each other's blocks and owned
// objects through the one allocator in stxlib_state (layout 5). In
// tag-checking builds a block released by the wrong allocator, or one the
// allocator does not recognise, would end the process, so passing here
// means both copies reached the same allocator. Tested, not proved
// (proofs/REPORT.md).

#include "allocator_two_module.hpp"

#include "test_support.hpp"

#include <cstring>
#include <utility>

namespace {

using stxlib::usize;

bool filled(const void* const block, const usize size, const unsigned char fill) {
    const unsigned char* const bytes = static_cast<const unsigned char*>(block);
    bool result = true;
    for (usize index = 0; index < size; ++index) result = result && bytes[index] == fill;
    return result;
}

void same_allocator() {
    CHECK(stxlib_allocator_two_module::library_backend() == stxlib::memory::backend());
    CHECK(stxlib_allocator_two_module::library_tags_checked() == stxlib::memory::tags_checked());
}

void raw_blocks() {
    // Made in the library, released here; then the reverse. Sizes cross
    // mimalloc's small, medium and large classes.
    const usize sizes[] = {0, 1, 64, 1000, 70000, 3000000};
    for (const usize size : sizes) {
        void* const theirs = stxlib_allocator_two_module::library_allocate(size, 0xa5);
        CHECK(theirs != nullptr);
        if (theirs != nullptr) {
            CHECK(filled(theirs, size, 0xa5));
            stxlib::memory::release(theirs);
        }
        void* const ours = stxlib::memory::allocate(size, 64);
        CHECK(ours != nullptr);
        if (ours != nullptr) {
            std::memset(ours, 0x3c, size);
            stxlib_allocator_two_module::library_release(ours);
        }
    }
}

void owned_objects() {
    {
        // A Path made in the library, destroyed here.
        const stxlib::Result<stxlib::Path> path = stxlib_allocator_two_module::library_path("/tmp/two/modules");
        CHECK(path.succeeded());
    }
    {
        // A Path made here, destroyed in the library.
        stxlib::Result<stxlib::Path> path = stxlib::Path::parse("/tmp/one/module");
        CHECK(path.succeeded());
        stxlib_allocator_two_module::library_destroy_path(std::move(path));
    }
    {
        // A thread started in the library, joined and destroyed here.
        stxlib::u32 counter = 0;
        stxlib::Result<stxlib::Thread> started = stxlib_allocator_two_module::library_thread(&counter);
        CHECK(started.succeeded());
        if (started.succeeded()) {
            stxlib::Thread thread = std::move(started).take_value();
            CHECK(thread.join() == stxlib::JoinResult::joined);
        }
        CHECK(counter == 1);
    }
    {
        // A pool created in the library, used and destroyed here.
        stxlib::Result<stxlib::AtomicPool> created = stxlib_allocator_two_module::library_pool(2);
        CHECK(created.succeeded());
        if (created.succeeded()) {
            stxlib::AtomicPool pool = std::move(created).take_value();
            CHECK(pool.thread_count() == 2);
        }
    }
}

} // namespace

int main() {
    same_allocator();
    raw_blocks();
    owned_objects();
    return stxlib_tests::finish("allocator_two_module");
}
