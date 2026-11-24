// The replaceable global allocation and deallocation functions
// ([new.delete]), per module: libc++ built with _LIBCPP_NO_VCRUNTIME leaves
// them to the platform's runtime (its new.cpp is compiled only with
// LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS), as vcruntime supplies them to a
// program linked with it. They live in the always-static module library so
// a program's own replacement, an object file, takes precedence over these
// archive members ([replacement.functions]).
//
// Storage comes from the UCRT heap: malloc and free, and _aligned_malloc and
// _aligned_free for alignments above the default (Microsoft Learn,
// "_aligned_malloc"). A failed allocation calls the new-handler while one is
// installed and retries ([new.delete.single]p3); without one, the throwing
// forms throw std::bad_alloc and the nothrow forms return null.
//
// This file is compiled with exceptions: the throwing forms throw, and the
// nothrow forms catch std::bad_alloc from a new-handler, as the standard
// requires them to report it as a null result.

#include "system_interim.h"

// Each function is its own archive member, so a program that replaces some of
// them (a test injecting allocation failures replaces operator new and delete
// but not the nothrow forms) does not also pull in this file's definitions of
// those it replaced: CMakeLists.txt compiles the file once per function with
// STXLIB_NEW_DELETE_PART set to its index. Without it, every function is
// defined.
#ifdef STXLIB_NEW_DELETE_PART
#define STXLIB_NEW_DELETE_IN_PART(index) (STXLIB_NEW_DELETE_PART == (index))
#else
#define STXLIB_NEW_DELETE_IN_PART(index) 1
#endif

#include <new>
#include <stddef.h>
#include <stdlib.h>

namespace {

// The UCRT's malloc already returns storage aligned for every fundamental
// type (16 bytes on x64); larger alignments need _aligned_malloc, whose
// blocks only _aligned_free may release.
[[maybe_unused]] constexpr size_t default_alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

[[maybe_unused]] void* allocate_or_null(const size_t size) noexcept {
    const size_t request = size == 0 ? 1 : size;
    return malloc(request);
}

[[maybe_unused]] void* allocate_aligned_or_null(const size_t size, const size_t alignment) noexcept {
    const size_t request = size == 0 ? 1 : size;
    return _aligned_malloc(request, alignment);
}

[[maybe_unused]] void* allocate(const size_t size) {
    void* block = allocate_or_null(size);
    while (block == nullptr) {
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
        block = allocate_or_null(size);
    }
    return block;
}

[[maybe_unused]] void* allocate_aligned(const size_t size, const size_t alignment) {
    void* block = allocate_aligned_or_null(size, alignment);
    while (block == nullptr) {
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
        block = allocate_aligned_or_null(size, alignment);
    }
    return block;
}

[[maybe_unused]] void* allocate_nothrow(const size_t size) noexcept {
    void* block = allocate_or_null(size);
    while (block == nullptr) {
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) return nullptr;
        try {
            handler();
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
        block = allocate_or_null(size);
    }
    return block;
}

[[maybe_unused]] void* allocate_aligned_nothrow(const size_t size, const size_t alignment) noexcept {
    void* block = allocate_aligned_or_null(size, alignment);
    while (block == nullptr) {
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) return nullptr;
        try {
            handler();
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
        block = allocate_aligned_or_null(size, alignment);
    }
    return block;
}

[[maybe_unused]] size_t alignment_value(const std::align_val_t alignment) noexcept {
    const size_t value = static_cast<size_t>(alignment);
    return value < default_alignment ? default_alignment : value;
}

} // namespace

#if STXLIB_NEW_DELETE_IN_PART(0)
void* operator new(const size_t size) {
    return allocate(size);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(1)
void* operator new[](const size_t size) {
    return allocate(size);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(2)
void* operator new(const size_t size, const std::nothrow_t&) noexcept {
    return allocate_nothrow(size);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(3)
void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
    return allocate_nothrow(size);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(4)
void* operator new(const size_t size, const std::align_val_t alignment) {
    return allocate_aligned(size, alignment_value(alignment));
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(5)
void* operator new[](const size_t size, const std::align_val_t alignment) {
    return allocate_aligned(size, alignment_value(alignment));
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(6)
void* operator new(const size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocate_aligned_nothrow(size, alignment_value(alignment));
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(7)
void* operator new[](const size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocate_aligned_nothrow(size, alignment_value(alignment));
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(8)
void operator delete(void* const block) noexcept {
    free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(9)
void operator delete[](void* const block) noexcept {
    free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(10)
void operator delete(void* const block, const size_t size) noexcept {
    static_cast<void>(size);
    free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(11)
void operator delete[](void* const block, const size_t size) noexcept {
    static_cast<void>(size);
    free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(12)
void operator delete(void* const block, const std::nothrow_t&) noexcept {
    free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(13)
void operator delete[](void* const block, const std::nothrow_t&) noexcept {
    free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(14)
void operator delete(void* const block, const std::align_val_t alignment) noexcept {
    static_cast<void>(alignment);
    _aligned_free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(15)
void operator delete[](void* const block, const std::align_val_t alignment) noexcept {
    static_cast<void>(alignment);
    _aligned_free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(16)
void operator delete(void* const block, const size_t size, const std::align_val_t alignment) noexcept {
    static_cast<void>(size);
    static_cast<void>(alignment);
    _aligned_free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(17)
void operator delete[](void* const block, const size_t size, const std::align_val_t alignment) noexcept {
    static_cast<void>(size);
    static_cast<void>(alignment);
    _aligned_free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(18)
void operator delete(void* const block, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    static_cast<void>(alignment);
    _aligned_free(block);
}
#endif

#if STXLIB_NEW_DELETE_IN_PART(19)
void operator delete[](void* const block, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    static_cast<void>(alignment);
    _aligned_free(block);
}
#endif
