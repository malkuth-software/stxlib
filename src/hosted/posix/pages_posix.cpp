// Pages on POSIX hosts (include/stxlib/pages.hpp), over mmap, mprotect
// and munmap, shared by macOS, glibc, musl and FreeBSD, as GUI.Forms'
// sealed_pages_posix.cpp maps them (491156eac). A1 platform-rest.
//
// mmap with MAP_PRIVATE | MAP_ANON and no file returns zeroed pages
// (POSIX.1-2024 makes MAP_ANONYMOUS standard; macOS, glibc and musl
// define MAP_ANON as well; FreeBSD: sys/sys/mman.h:52, 71, 94 at the pin
// fb9033d2876d for PROT_READ, MAP_PRIVATE and MAP_ANON). The length is
// rounded up here to whole pages, so mprotect and munmap cover exactly the
// mapping.

#include "stxlib/capabilities.hpp"
#include "stxlib/pages.hpp"

#include "core/host_failure.hpp"

#include <cerrno>

#include <sys/mman.h>
#include <unistd.h>

namespace stxlib {

namespace {

ForeignCode errno_code(const int error) noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
    return result;
}

} // namespace

usize page_size() noexcept {
    const long reported = ::sysconf(_SC_PAGESIZE);
    const usize result = reported > 0 ? static_cast<usize>(reported) : 4096;
    return result;
}

OperationResult<PageBlock> PageBlock::map(const usize bytes) noexcept {
    const Operation operation = "stxlib.pages.map";
    if (!capability_supported(Capability::page_protection)) {
        return OperationResult<PageBlock>::failure(host_failure::record(ErrorKind::unsupported, operation, HostCode::none));
    }
    if (bytes == 0) return OperationResult<PageBlock>::success(PageBlock());
    const usize page = page_size();
    if (bytes > usize_max - (page - 1)) {
        return OperationResult<PageBlock>::failure(host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::too_long));
    }
    const usize whole = (bytes + page - 1) / page * page;
    void* const mapped = ::mmap(nullptr, whole, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) {
        const ErrorRecord record =
            host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none, errno_code(errno));
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
    if (::mprotect(base_, mapped_, PROT_READ) != 0) {
        return host_failure::failed(ErrorKind::foreign_failure, operation, HostCode::none, errno_code(errno));
    }
    sealed_ = true;
    return OperationStatus::success();
}

void PageBlock::unmap() noexcept {
    if (base_ == nullptr) return;
    // munmap fails only for an address or length that is not a mapping of
    // ours, which the block's invariant excludes.
    static_cast<void>(::munmap(base_, mapped_));
    base_ = nullptr;
    count_ = 0;
    mapped_ = 0;
    sealed_ = false;
}

} // namespace stxlib
