// Pages on Windows (include/stxlib/pages.hpp), over VirtualAlloc,
// VirtualProtect and VirtualFree, as GUI.Forms' sealed pages map them on
// Windows (491156eac). A1 platform-rest. Host facts (Microsoft's
// documentation as published; unconfirmed at a pin):
//   - VirtualAlloc with MEM_COMMIT | MEM_RESERVE returns zeroed pages, and
//     rounds the size up to whole pages.
//   - VirtualProtect with PAGE_READONLY covers every page the range touches.
//   - VirtualFree with MEM_RELEASE takes size 0 and the allocation's base.
//   - GetSystemInfo's dwPageSize is the protection granularity.

#include "stxlib/capabilities.hpp"
#include "stxlib/pages.hpp"

#include "core/host_failure.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib {

namespace {

ForeignCode last_error() noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError()));
    return result;
}

} // namespace

usize page_size() noexcept {
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    const usize result = information.dwPageSize > 0 ? static_cast<usize>(information.dwPageSize) : 4096;
    return result;
}

OperationResult<PageBlock> PageBlock::map(const usize bytes) noexcept {
    const Operation operation = "stxlib.pages.map";
    if (!capability_supported(Capability::page_protection)) {
        return OperationResult<PageBlock>::failure(host_failure::record(ErrorKind::unsupported, operation,
                                                                        HostCode::none));
    }
    if (bytes == 0) return OperationResult<PageBlock>::success(PageBlock());
    const usize page = page_size();
    if (bytes > usize_max - (page - 1)) {
        return OperationResult<PageBlock>::failure(host_failure::record(ErrorKind::limit_exceeded, operation,
                                                                        HostCode::too_long));
    }
    const usize whole = (bytes + page - 1) / page * page;
    void* const mapped = VirtualAlloc(nullptr, static_cast<SIZE_T>(whole), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mapped == nullptr) {
        const ErrorRecord record = host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none, last_error());
        return OperationResult<PageBlock>::failure(record);
    }
    return OperationResult<PageBlock>::success(PageBlock(static_cast<Byte*>(mapped), bytes, whole));
}

PageBlock::~PageBlock() { unmap(); }

PageBlock::PageBlock(PageBlock&& other) noexcept
    : base_(other.base_), count_(other.count_), mapped_(other.mapped_), sealed_(other.sealed_) {
    other.base_ = nullptr;
    other.count_ = 0;
    other.mapped_ = 0;
    other.sealed_ = false;
}

PageBlock& PageBlock::operator=(PageBlock&& other) noexcept {
    if (this == &other) return *this;
    unmap();
    base_ = other.base_;
    count_ = other.count_;
    mapped_ = other.mapped_;
    sealed_ = other.sealed_;
    other.base_ = nullptr;
    other.count_ = 0;
    other.mapped_ = 0;
    other.sealed_ = false;
    return *this;
}

MutableView<Byte> PageBlock::writable() & noexcept {
    STXLIB_REQUIRE(!sealed_, "PageBlock::writable: the pages are sealed");
    const MutableView<Byte> result(base_, count_);
    return result;
}

OperationStatus PageBlock::seal() noexcept {
    const Operation operation = "stxlib.pages.seal";
    if (sealed_) return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::sealed);
    if (base_ == nullptr) {
        sealed_ = true;
        return OperationStatus::success();
    }
    DWORD previous = 0;
    if (VirtualProtect(base_, static_cast<SIZE_T>(mapped_), PAGE_READONLY, &previous) == 0) {
        return host_failure::failed(ErrorKind::foreign_failure, operation, HostCode::none, last_error());
    }
    sealed_ = true;
    return OperationStatus::success();
}

void PageBlock::unmap() noexcept {
    if (base_ == nullptr) return;
    // VirtualFree fails only for an address that is not an allocation's
    // base, which the block's invariant excludes.
    static_cast<void>(VirtualFree(base_, 0, MEM_RELEASE));
    base_ = nullptr;
    count_ = 0;
    mapped_ = 0;
    sealed_ = false;
}

} // namespace stxlib
