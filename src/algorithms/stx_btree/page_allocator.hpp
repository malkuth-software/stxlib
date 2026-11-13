#pragma once

// Every allocation a tree makes is a node page of a kind and a layout known
// at compile time (bbtree D8): leaves, internal nodes and the root page.
// Pages come zeroed, aligned to their size class.
//
// STXlib adaptation (from stx_auxlib b6e330a, btree/include/stx_btree/
// page_allocator.hpp): the default allocator takes its pages from
// memory::allocate (allocator-owns-everything) in place of the global
// aligned operator new, and reports exhaustion by returning null instead of
// throwing std::bad_alloc; the sequence tree turns a null page into a
// refused edit that leaves the tree unchanged (sequence_tree.hpp,
// "Failure").

#include "stxlib/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace stxlib::stx_btree {

enum class PageKind : std::uint8_t { leaf, internal, root };

// The default allocator: memory::allocate, zero-filled. Null when memory is
// exhausted (or the size is not representable).
class MemoryPageAllocator final {
public:
    [[nodiscard]] void* allocate_page(PageKind, std::size_t bytes, std::size_t alignment) const noexcept {
        void* page = memory::allocate(bytes, alignment);
        if (page == nullptr) {
            return nullptr;
        }
        std::memset(page, 0, bytes);
        return page;
    }

    void deallocate_page(PageKind, void* page, std::size_t, std::size_t) const noexcept { memory::release(page); }

    [[nodiscard]] bool operator==(const MemoryPageAllocator&) const noexcept { return true; }
};

} // namespace stxlib::stx_btree
