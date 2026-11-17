// The global operator new and delete, for Stage 1 only.
//
// A libc++ built in vcruntime mode (LIBCXX_CXX_ABI=vcruntime) leaves the
// global allocation functions to the startup library (libcxx/src/new.cpp
// defines them only without _LIBCPP_ABI_VCRUNTIME; the study's section
// 4.1), so this shim supplies them while Stage 1 links such a libc++. A
// libc++ built with _LIBCPP_NO_VCRUNTIME (Stage 2) defines them itself;
// such an image then links without the vcruntime-mode archive, so that it
// has one definition.
//
// Behaviour, from [new.delete]: the throwing forms retry while a new
// handler is installed (the UCRT's _callnewh calls it and returns nonzero
// when there was one) and throw std::bad_alloc when none is; the nothrow
// forms return null instead; a request for zero bytes gets a unique
// allocation of one byte. Aligned forms use the UCRT's _aligned_malloc and
// _aligned_free, which its documentation requires to be paired; alignments
// up to the default new alignment never reach them. The deleting forms
// accept null.
//
// This file is the shim's only one compiled with exceptions: throwing
// std::bad_alloc is operator new's contract. The exception runtime that
// carries the throw is vcruntime's in Stage 1.

#include "hosted/windows/msvc_runtime/system_interim.h"

#include <new>
#include <stdlib.h>

namespace {

void* allocate_or_null(const size_t size) noexcept {
    size_t request = size;
    if (request == 0) {
        request = 1;
    }
    for (;;) {
        void* const block = malloc(request);
        if (block != nullptr) {
            return block;
        }
        const int handled = _callnewh(request);
        if (handled == 0) {
            return nullptr;
        }
    }
}

void* allocate_aligned_or_null(const size_t size, const std::align_val_t alignment) noexcept {
    size_t request = size;
    if (request == 0) {
        request = 1;
    }
    const size_t alignment_bytes = static_cast<size_t>(alignment);
    for (;;) {
        void* const block = _aligned_malloc(request, alignment_bytes);
        if (block != nullptr) {
            return block;
        }
        const int handled = _callnewh(request);
        if (handled == 0) {
            return nullptr;
        }
    }
}

void* allocate_or_throw(const size_t size) {
    void* const block = allocate_or_null(size);
    if (block == nullptr) {
        throw std::bad_alloc();
    }
    return block;
}

void* allocate_aligned_or_throw(const size_t size, const std::align_val_t alignment) {
    void* const block = allocate_aligned_or_null(size, alignment);
    if (block == nullptr) {
        throw std::bad_alloc();
    }
    return block;
}

}  // namespace

void* operator new(size_t size) {
    void* const block = allocate_or_throw(size);
    return block;
}

void* operator new[](size_t size) {
    void* const block = allocate_or_throw(size);
    return block;
}

void* operator new(size_t size, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    void* const block = allocate_or_null(size);
    return block;
}

void* operator new[](size_t size, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    void* const block = allocate_or_null(size);
    return block;
}

void* operator new(size_t size, std::align_val_t alignment) {
    void* const block = allocate_aligned_or_throw(size, alignment);
    return block;
}

void* operator new[](size_t size, std::align_val_t alignment) {
    void* const block = allocate_aligned_or_throw(size, alignment);
    return block;
}

void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    void* const block = allocate_aligned_or_null(size, alignment);
    return block;
}

void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    void* const block = allocate_aligned_or_null(size, alignment);
    return block;
}

void operator delete(void* block) noexcept {
    free(block);
}

void operator delete[](void* block) noexcept {
    free(block);
}

void operator delete(void* block, size_t size) noexcept {
    static_cast<void>(size);
    free(block);
}

void operator delete[](void* block, size_t size) noexcept {
    static_cast<void>(size);
    free(block);
}

void operator delete(void* block, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    free(block);
}

void operator delete[](void* block, const std::nothrow_t& tag) noexcept {
    static_cast<void>(tag);
    free(block);
}

void operator delete(void* block, std::align_val_t alignment) noexcept {
    static_cast<void>(alignment);
    _aligned_free(block);
}

void operator delete[](void* block, std::align_val_t alignment) noexcept {
    static_cast<void>(alignment);
    _aligned_free(block);
}

void operator delete(void* block, size_t size, std::align_val_t alignment) noexcept {
    static_cast<void>(size);
    static_cast<void>(alignment);
    _aligned_free(block);
}

void operator delete[](void* block, size_t size, std::align_val_t alignment) noexcept {
    static_cast<void>(size);
    static_cast<void>(alignment);
    _aligned_free(block);
}

void operator delete(void* block, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    static_cast<void>(alignment);
    static_cast<void>(tag);
    _aligned_free(block);
}

void operator delete[](void* block, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    static_cast<void>(alignment);
    static_cast<void>(tag);
    _aligned_free(block);
}
