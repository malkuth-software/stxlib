#pragma once

// Whole pages from the host, which can be sealed read-only
// (Capability::page_protection). Replaces GUI.Forms' sealed font pages
// (src/host/posix/fonts/sealed_pages_posix.cpp at 491156eac: mmap,
// mprotect, munmap; A0 inventory, section 3). A1 platform-rest.
//
// A PageBlock is memory STXlib owns outside its allocator: it is mapped
// from the host (mmap with MAP_PRIVATE | MAP_ANON on POSIX hosts,
// VirtualAlloc on Windows), zeroed by the host, and unmapped only by its
// own destructor (owner, 2026-11-02: memory we allocate is released only by
// us). Sealing makes every page read-only (mprotect PROT_READ,
// VirtualProtect PAGE_READONLY): a later write faults, which is the point:
// data that must not change after it is built cannot be changed by a
// stray write.
//
// Concurrency: a PageBlock has one owner. Sealing is not synchronization:
// publish a sealed block to other threads through a lock or a release
// store as for any other data.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// The host's page size in bytes (sysconf(_SC_PAGESIZE); GetSystemInfo's
// dwPageSize). Read once per call; never 0 (4096 if the host cannot say).
[[nodiscard]] usize page_size() noexcept;

class PageBlock final {
public:
    // Maps `bytes` (rounded up to whole pages) of zeroed, writable memory.
    // Zero bytes yields the empty block. Failures: limit_exceeded when the
    // rounding overflows; out_of_memory when the host refuses (its code as
    // the foreign code); unsupported where Capability::page_protection is
    // false.
    [[nodiscard]] static OperationResult<PageBlock> map(usize bytes) noexcept;

    PageBlock() noexcept = default;
    // Unmaps the pages; borrows of them end.
    ~PageBlock();
    PageBlock(const PageBlock&) = delete;
    PageBlock& operator=(const PageBlock&) = delete;
    PageBlock(PageBlock&& other) noexcept;
    // Unmaps this block's pages first.
    PageBlock& operator=(PageBlock&& other) noexcept;

    // The bytes asked for, and the whole pages mapped (at least as many).
    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] usize mapped_bytes() const noexcept { return mapped_; }
    [[nodiscard]] bool sealed() const noexcept { return sealed_; }

    // The first count() bytes, read-only. Borrowed from this block: valid
    // until it is moved from or destroyed.
    [[nodiscard]] View<Byte> bytes() const& noexcept {
        const View<Byte> result(base_, count_);
        return result;
    }
    View<Byte> bytes() const&& = delete;
    // The same bytes, writable. Requirement: not sealed() (a write after
    // sealing faults; the requirement turns the attempt into a report).
    [[nodiscard]] MutableView<Byte> writable() & noexcept;
    MutableView<Byte> writable() && = delete;

    // Makes every page read-only, for good. Failures: invalid_argument with
    // HostCode::sealed when already sealed; foreign_failure with the host's
    // code (the block is then unchanged and still writable).
    [[nodiscard]] OperationStatus seal() noexcept;

private:
    PageBlock(Byte* const pages, const usize requested, const usize whole) noexcept
        : base_(pages), count_(requested), mapped_(whole) {}

    void unmap() noexcept;

    Byte* base_{nullptr};
    usize count_{0};
    usize mapped_{0};
    bool sealed_{false};
};

} // namespace stxlib
