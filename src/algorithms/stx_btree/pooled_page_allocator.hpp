#pragma once

// A page allocator that pools pages by size class (bbtree's `Allocator`
// trait anticipates one, D8). Each class carves its pages in address order
// from chunks that grow from 64 KiB to 2 MiB, and keeps freed pages on a free
// list for reuse. The system allocator's aligned path places 1 KiB pages
// aligned to 1 KiB with gaps between them (glibc: 1.55 GB of address space
// for 0.76 GB of pages at 10M entries); packed pages took the unsync map's
// u64 inserts at 1M from 1.33x to 0.94x bbtree's time on Linux x86-64
// (stx_auxlib's btree/docs/MEASUREMENTS.md).
//
// Ownership: every copy of a PooledPageAllocator shares one PagePool; the
// pool and its chunks live until the last copy goes, so a map owns its pool,
// and maps built from copies of one allocator share it. Pages return to the
// pool, never to the system, until the pool ends. Copying (also on move:
// there is no move constructor) keeps both copies usable.
//
// Threads: take and give_back hold the pool's mutex. The shared map calls
// them only when its own free lists are empty (D54), about once per leaf
// split, so the lock is off the operation path.
//
// Pages come zeroed, as MemoryPageAllocator's do; the latched map relies on
// it (D54).
//
// STXlib adaptation (from stx_auxlib b6e330a, btree/include/stx_btree/
// pooled_page_allocator.hpp): chunks come from memory::allocate and their
// records are a list in memory::allocate's blocks (stx_auxlib: aligned
// operator new and a std::vector); the shared pool is counted by the
// allocators that share it (stx_auxlib: std::shared_ptr), so a pool is made
// by an allocator and shared by copying it, and the constructor from a
// std::shared_ptr is gone; an exhausted system, or a seventeenth size class,
// gives a null page where stx_auxlib threw std::bad_alloc (and a default
// allocator whose pool could not be made gives only null pages); the lock is
// STXlib's Mutex. The carving, the chunk sizes, the free lists and Usage are
// stx_auxlib's.

#include "algorithms/stx_btree/page_allocator.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/threads.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace stxlib::stx_btree {

class PagePool final {
public:
    static constexpr std::size_t first_chunk_bytes = static_cast<std::size_t>(64) << 10;
    static constexpr std::size_t largest_chunk_bytes = static_cast<std::size_t>(2) << 20;

    // What the pool holds, for tests and measurement.
    struct Usage final {
        std::size_t chunk_bytes = 0;  // reserved from the system
        std::size_t pages_out = 0;    // handed out and not given back
        std::size_t pages_free = 0;   // on the free lists
    };

    PagePool() noexcept = default;
    PagePool(const PagePool&) = delete;
    PagePool& operator=(const PagePool&) = delete;
    ~PagePool() {
        Chunk* chunk = chunks;
        while (chunk != nullptr) {
            Chunk* const next = (*chunk).next;
            memory::release((*chunk).start);
            memory::release(chunk);
            chunk = next;
        }
    }

    // A page of `bytes` aligned to `alignment` (a power of two), not zeroed.
    // Null when the system has no chunk to give or the pool has no room for
    // another size class.
    [[nodiscard]] void* take(std::size_t bytes, std::size_t alignment) noexcept {
        const ScopedLock guard(mutex);
        SizeClass* const size_class = class_for(bytes, alignment);
        if (size_class == nullptr) {
            return nullptr;
        }
        if ((*size_class).free_top != nullptr) {
            std::byte* const page = (*size_class).free_top;
            (*size_class).free_top = read_link(page);
            (*size_class).free_count -= 1;
            (*size_class).out_count += 1;
            return page;
        }
        const std::size_t step = round_up(bytes, alignment);
        if ((*size_class).end - (*size_class).next < static_cast<std::ptrdiff_t>(step)) {
            if (!add_chunk(*size_class, step)) {
                return nullptr;
            }
        }
        std::byte* const page = (*size_class).next;
        (*size_class).next += step;
        (*size_class).out_count += 1;
        return page;
    }

    // Takes back a page from take(bytes, alignment) of this pool.
    void give_back(void* page, std::size_t bytes, std::size_t alignment) noexcept {
        const ScopedLock guard(mutex);
        SizeClass* size_class = find_class(bytes, alignment);
        std::byte* const freed = static_cast<std::byte*>(page);
        write_link(freed, (*size_class).free_top);
        (*size_class).free_top = freed;
        (*size_class).free_count += 1;
        (*size_class).out_count -= 1;
    }

    [[nodiscard]] Usage usage() noexcept {
        const ScopedLock guard(mutex);
        Usage total;
        const Chunk* chunk = chunks;
        while (chunk != nullptr) {
            total.chunk_bytes += (*chunk).bytes;
            chunk = (*chunk).next;
        }
        std::size_t index = 0;
        for (index = 0; index < class_count; index += 1) {
            total.pages_out += classes[index].out_count;
            total.pages_free += classes[index].free_count;
        }
        return total;
    }

private:
    friend class PooledPageAllocator;

    // A map uses three classes (leaf, internal, root pages); room for a few
    // maps with different layouts sharing one pool.
    static constexpr std::size_t class_capacity = 16;

    struct SizeClass final {
        std::size_t bytes = 0;
        std::size_t alignment = 0;
        std::byte* next = nullptr;      // the next uncarved page of the current chunk
        std::byte* end = nullptr;       // the current chunk's end
        std::byte* free_top = nullptr;  // freed pages, linked through their first bytes
        std::size_t next_chunk_bytes = first_chunk_bytes;
        std::size_t free_count = 0;
        std::size_t out_count = 0;
    };

    // One chunk from the system, in a list of the pool's chunks.
    struct Chunk final {
        std::byte* start;
        std::size_t bytes;
        Chunk* next;
    };

    [[nodiscard]] static std::size_t round_up(std::size_t bytes, std::size_t alignment) noexcept {
        return (bytes + alignment - 1) / alignment * alignment;
    }

    [[nodiscard]] static std::byte* read_link(std::byte* page) noexcept {
        std::byte* next = nullptr;
        std::memcpy(&next, page, sizeof(next));
        return next;
    }

    static void write_link(std::byte* page, std::byte* next) noexcept {
        std::memcpy(page, &next, sizeof(next));
    }

    [[nodiscard]] SizeClass* find_class(std::size_t bytes, std::size_t alignment) noexcept {
        std::size_t index = 0;
        for (index = 0; index < class_count; index += 1) {
            if (classes[index].bytes == bytes && classes[index].alignment == alignment) {
                return &classes[index];
            }
        }
        return nullptr;
    }

    // The class for the size and alignment, made on first use; null when
    // every class is taken (stx_auxlib threw std::bad_alloc).
    [[nodiscard]] SizeClass* class_for(std::size_t bytes, std::size_t alignment) noexcept {
        SizeClass* found = find_class(bytes, alignment);
        if (found != nullptr) {
            return found;
        }
        if (class_count == class_capacity) {
            return nullptr;
        }
        classes[class_count] = SizeClass{.bytes = bytes,
                                         .alignment = alignment,
                                         .next = nullptr,
                                         .end = nullptr,
                                         .free_top = nullptr,
                                         .next_chunk_bytes = first_chunk_bytes,
                                         .free_count = 0,
                                         .out_count = 0};
        class_count += 1;
        return &classes[class_count - 1];
    }

    // A fresh chunk for the class: the next size in its doubling, at least
    // one page; the rest of the old chunk, less than a page, is left unused.
    // False, with nothing changed, when the system has none.
    [[nodiscard]] bool add_chunk(SizeClass& size_class, std::size_t step) noexcept {
        std::size_t bytes = size_class.next_chunk_bytes;
        if (bytes < step) {
            bytes = step;
        }
        bytes = round_up(bytes, step);
        void* const record = memory::allocate(sizeof(Chunk), alignof(Chunk));
        if (record == nullptr) {
            return false;
        }
        std::byte* const start = static_cast<std::byte*>(memory::allocate(bytes, size_class.alignment));
        if (start == nullptr) {
            memory::release(record);
            return false;
        }
        chunks = ::new (record) Chunk{.start = start, .bytes = bytes, .next = chunks};
        size_class.next = start;
        size_class.end = start + bytes;
        if (size_class.next_chunk_bytes < largest_chunk_bytes) {
            size_class.next_chunk_bytes *= 2;
        }
        return true;
    }

    // STXlib adaptation: the pool lock is STXlib's Mutex.
    Mutex mutex;
    SizeClass classes[class_capacity];
    std::size_t class_count = 0;
    Chunk* chunks = nullptr;
    // The allocators sharing this pool (PooledPageAllocator's copies).
    std::atomic<std::size_t> sharers{0};
};

class PooledPageAllocator final {
public:
    // An allocator with a pool of its own; when the pool cannot be made, the
    // allocator gives only null pages.
    PooledPageAllocator() noexcept : pool(make_pool()) {}

    // Shares `other`'s pool.
    PooledPageAllocator(const PooledPageAllocator& other) noexcept : pool(other.pool) {
        share(pool);
    }
    PooledPageAllocator& operator=(const PooledPageAllocator& other) noexcept {
        if (pool != other.pool) {
            share(other.pool);
            leave(pool);
            pool = other.pool;
        }
        return *this;
    }
    ~PooledPageAllocator() {
        leave(pool);
    }

    // Null when the pool has no page (or there is no pool).
    [[nodiscard]] void* allocate_page(PageKind, std::size_t bytes, std::size_t alignment) const noexcept {
        if (pool == nullptr) {
            return nullptr;
        }
        void* page = (*pool).take(bytes, alignment);
        if (page == nullptr) {
            return nullptr;
        }
        std::memset(page, 0, bytes);
        return page;
    }

    void deallocate_page(PageKind, void* page, std::size_t bytes, std::size_t alignment) const noexcept {
        (*pool).give_back(page, bytes, alignment);
    }

    // Requirement: the allocator has a pool (has_pool()).
    [[nodiscard]] PagePool& page_pool() const noexcept {
        return *pool;
    }
    [[nodiscard]] bool has_pool() const noexcept {
        return pool != nullptr;
    }

    [[nodiscard]] bool operator==(const PooledPageAllocator& other) const noexcept {
        return pool == other.pool;
    }

private:
    [[nodiscard]] static PagePool* make_pool() noexcept {
        void* const storage = memory::allocate(sizeof(PagePool), alignof(PagePool));
        if (storage == nullptr) {
            return nullptr;
        }
        PagePool* const made = ::new (storage) PagePool();
        (*made).sharers.store(1, std::memory_order_relaxed);
        return made;
    }
    static void share(PagePool* shared) noexcept {
        if (shared != nullptr) {
            (*shared).sharers.fetch_add(1, std::memory_order_relaxed);
        }
    }
    static void leave(PagePool* shared) noexcept {
        if (shared == nullptr) {
            return;
        }
        if ((*shared).sharers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            (*shared).~PagePool();
            memory::release(shared);
        }
    }

    PagePool* pool;
};

} // namespace stxlib::stx_btree
