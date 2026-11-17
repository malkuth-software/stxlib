// std::exception_ptr's runtime: the __ExceptionPtr* functions libc++ calls
// on the Microsoft ABI (libcxx/src/support/runtime/exception_pointer_msvc.ipp,
// llvmorg-22.1.8: Create, Destroy, Copy, Assign, Compare, ToBool, Swap,
// CurrentException, Rethrow, CopyException). Microsoft's STL exports them
// from msvcp140; supplying them here keeps libc++ unpatched (the queue
// entry's question on exception_ptr records the comparison).
//
// Representation. libc++ gives exception_ptr two pointers (__ptr1_,
// __ptr2_, libcxx/include/__exception/exception_ptr.h); this runtime uses
// the first for a reference-counted ExceptionBlock and keeps the second
// null. A block owns a copy of the exception object, made with the thrown
// type's copy constructor (catchable type entry 0, the exact type), and
// destroys it with the ThrowInfo's destructor when the last reference
// goes. A structured exception captured under -EHa keeps only its record.
//
// Storage comes from the process heap (HeapAlloc on GetProcessHeap,
// Microsoft Learn), released only here. When a block cannot be allocated,
// the exception_ptr refers to one permanent block holding std::bad_alloc,
// as [propagation]p8 allows ("a reference to an instance of bad_alloc").
//
// Rethrow copies the object again, into the rethrowing frame, because a
// thrown object's life ends with its last handler (frame_handler.cpp,
// run_catch), while the block's object lives as long as its references.

#include "runtime_state.hpp"

#include "stxlib/detail/early_exit.hpp"

#include <atomic>
#include <new>

namespace stxlib::msvc_runtime {

namespace {

// The object starts at this alignment within the block; an exception type
// aligned more strictly is refused by a rethrow (fail fast), never misaligned.
constexpr usize object_alignment = 64;
// A thrown type larger than this is not copied onto the stack by a rethrow.
constexpr usize max_rethrow_size = 64 * 1024;

static_assert(std::atomic<i64>::is_always_lock_free, "atomic<i64> must be lock-free");

struct ExceptionBlock final {
    std::atomic<i64> references;
    bool permanent;
    HandledException exception;
    usize object_size;
    Byte* object;                        // inside this block, or null for a structured exception
};

struct Pointer final {
    ExceptionBlock* block;
    void* unused;
};

static_assert(sizeof(Pointer) == 2 * sizeof(void*), "libc++'s exception_ptr holds two pointers");

constinit ExceptionBlock bad_alloc_block{};
constinit std::atomic<bool> bad_alloc_ready{false};

ExceptionBlock* permanent_bad_alloc() noexcept {
    if (bad_alloc_ready.load(std::memory_order_acquire)) return &bad_alloc_block;
    const ThrowInfo* info = nullptr;
    void* const object = bad_alloc_object(&info);
    void* image_base = nullptr;
    RtlPcToFileHeader(const_cast<ThrowInfo*>(info), &image_base);
    // Every thread that gets here writes the same values.
    bad_alloc_block.references.store(1, std::memory_order_relaxed);
    bad_alloc_block.permanent = true;
    bad_alloc_block.exception.code = cxx_exception_code;
    bad_alloc_block.exception.flags = EXCEPTION_NONCONTINUABLE;
    bad_alloc_block.exception.parameter_count = cxx_exception_parameters;
    bad_alloc_block.exception.parameters[0] = static_cast<ULONG_PTR>(cxx_exception_magic);
    bad_alloc_block.exception.parameters[1] = reinterpret_cast<ULONG_PTR>(object);
    bad_alloc_block.exception.parameters[2] = reinterpret_cast<ULONG_PTR>(info);
    bad_alloc_block.exception.parameters[3] = reinterpret_cast<ULONG_PTR>(image_base);
    bad_alloc_block.object_size = 0;
    bad_alloc_block.object = static_cast<Byte*>(object);
    bad_alloc_ready.store(true, std::memory_order_release);
    return &bad_alloc_block;
}

// A block with room for `object_size` bytes at object_alignment, or null.
ExceptionBlock* allocate_block(const usize object_size) noexcept {
    const usize header = (sizeof(ExceptionBlock) + object_alignment - 1) / object_alignment * object_alignment;
    if (object_size > static_cast<usize>(0x7FFFFFFF)) return nullptr;
    const usize total = header + object_size + object_alignment;
    void* const memory = HeapAlloc(GetProcessHeap(), 0, total);
    if (memory == nullptr) return nullptr;
    ExceptionBlock* const block = static_cast<ExceptionBlock*>(memory);
    new (block) ExceptionBlock{};
    const uptr start = reinterpret_cast<uptr>(memory) + header;
    const uptr aligned = (start + object_alignment - 1) / object_alignment * object_alignment;
    (*block).references.store(1, std::memory_order_relaxed);
    (*block).permanent = false;
    (*block).object_size = object_size;
    (*block).object = object_size == 0 ? nullptr : reinterpret_cast<Byte*>(aligned);
    return block;
}

void release_block(ExceptionBlock* const block) noexcept {
    if (block == nullptr || (*block).permanent) return;
    const i64 left = (*block).references.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left != 0) return;
    if ((*block).object != nullptr) destroy_exception_object((*block).exception);
    (*block).~ExceptionBlock();
    HeapFree(GetProcessHeap(), 0, block);
}

void retain_block(ExceptionBlock* const block) noexcept {
    if (block == nullptr || (*block).permanent) return;
    (*block).references.fetch_add(1, std::memory_order_relaxed);
}

// A block holding a copy of the C++ exception `object` of the type `info`
// describes, from the image at `image_base`; the bad_alloc block when the
// copy cannot be stored.
ExceptionBlock* capture_cxx(void* const object, const ThrowInfo& info, const uptr image_base) noexcept {
    const CatchableType* const type = exact_catchable_type(info, image_base);
    if (type == nullptr || (*type).size < 0) fail_fast();
    const usize size = static_cast<usize>((*type).size);
    ExceptionBlock* const block = allocate_block(size);
    if (block == nullptr) return permanent_bad_alloc();
    copy_exception_object(*type, image_base, (*block).object, object);
    (*block).exception.code = cxx_exception_code;
    (*block).exception.flags = EXCEPTION_NONCONTINUABLE;
    (*block).exception.parameter_count = cxx_exception_parameters;
    (*block).exception.parameters[0] = static_cast<ULONG_PTR>(cxx_exception_magic);
    (*block).exception.parameters[1] = reinterpret_cast<ULONG_PTR>((*block).object);
    (*block).exception.parameters[2] = reinterpret_cast<ULONG_PTR>(&info);
    (*block).exception.parameters[3] = static_cast<ULONG_PTR>(image_base);
    return block;
}

Pointer& as_pointer(void* const storage) noexcept {
    Pointer& result = *static_cast<Pointer*>(storage);
    return result;
}

const Pointer& as_pointer(const void* const storage) noexcept {
    const Pointer& result = *static_cast<const Pointer*>(storage);
    return result;
}

} // namespace

} // namespace stxlib::msvc_runtime

// C++ linkage, global namespace: libc++ declares them so on this ABI
// (src/support/runtime/exception_pointer_msvc.ipp, _LIBCPP_CRT_FUNC), and
// imports them by their decorated names, ?__ExceptionPtrCreate@@YAXPEAX@Z
// and the rest (llvm-nm over libc++.lib, 2026-11-16). The parameters carry
// no top-level const: the Microsoft decoration encodes it (Q for "* const"
// against P), so a const parameter here names a different symbol.

void __cdecl __ExceptionPtrCreate(void* storage) {
    using namespace stxlib::msvc_runtime;
    Pointer& pointer = as_pointer(storage);
    pointer.block = nullptr;
    pointer.unused = nullptr;
}

void __cdecl __ExceptionPtrDestroy(void* storage) {
    using namespace stxlib::msvc_runtime;
    Pointer& pointer = as_pointer(storage);
    ExceptionBlock* const block = pointer.block;
    pointer.block = nullptr;
    release_block(block);
}

void __cdecl __ExceptionPtrCopy(void* storage, const void* other) {
    using namespace stxlib::msvc_runtime;
    Pointer& pointer = as_pointer(storage);
    ExceptionBlock* const block = as_pointer(other).block;
    retain_block(block);
    pointer.block = block;
    pointer.unused = nullptr;
}

void __cdecl __ExceptionPtrAssign(void* storage, const void* other) {
    using namespace stxlib::msvc_runtime;
    Pointer& pointer = as_pointer(storage);
    ExceptionBlock* const incoming = as_pointer(other).block;
    retain_block(incoming);
    ExceptionBlock* const outgoing = pointer.block;
    pointer.block = incoming;
    release_block(outgoing);
}

bool __cdecl __ExceptionPtrCompare(const void* left, const void* right) {
    using namespace stxlib::msvc_runtime;
    const bool result = as_pointer(left).block == as_pointer(right).block;
    return result;
}

bool __cdecl __ExceptionPtrToBool(const void* storage) {
    using namespace stxlib::msvc_runtime;
    const bool result = as_pointer(storage).block != nullptr;
    return result;
}

void __cdecl __ExceptionPtrSwap(void* left, void* right) {
    using namespace stxlib::msvc_runtime;
    Pointer& a = as_pointer(left);
    Pointer& b = as_pointer(right);
    ExceptionBlock* const held = a.block;
    a.block = b.block;
    b.block = held;
}

void __cdecl __ExceptionPtrCurrentException(void* storage) {
    using namespace stxlib::msvc_runtime;
    Pointer& pointer = as_pointer(storage);
    pointer.block = nullptr;
    pointer.unused = nullptr;
    const CatchRecord* const record = innermost_catch();
    if (record == nullptr) return;
    const HandledException& exception = (*record).exception;
    if (!is_cxx_exception(exception)) {
        ExceptionBlock* const block = allocate_block(0);
        if (block == nullptr) {
            pointer.block = permanent_bad_alloc();
            return;
        }
        (*block).exception = exception;
        pointer.block = block;
        return;
    }
    const ThrowInfo* const info = exception_throw_info(exception);
    if (info == nullptr) return;
    pointer.block = capture_cxx(exception_object(exception), *info, exception_image_base(exception));
}

void __cdecl __ExceptionPtrCopyException(void* storage, const void* object, const void* throw_info) {
    using namespace stxlib::msvc_runtime;
    Pointer& pointer = as_pointer(storage);
    ExceptionBlock* const outgoing = pointer.block;
    pointer.block = nullptr;
    release_block(outgoing);
    if (object == nullptr || throw_info == nullptr) return;
    void* image_base = nullptr;
    RtlPcToFileHeader(const_cast<void*>(throw_info), &image_base);
    if (image_base == nullptr) fail_fast();
    const ThrowInfo& info = *static_cast<const ThrowInfo*>(throw_info);
    pointer.block = capture_cxx(const_cast<void*>(object), info, reinterpret_cast<stxlib::uptr>(image_base));
}

[[noreturn]] void __cdecl __ExceptionPtrRethrow(const void* storage) {
    using namespace stxlib::msvc_runtime;
    const ExceptionBlock* const block = as_pointer(storage).block;
    // rethrow_exception of a null exception_ptr is undefined ([propagation]p10).
    if (block == nullptr) terminate_now();
    HandledException exception = (*block).exception;
    if (!is_cxx_exception(exception)) {
        raise_handled(exception);
    }
    const ThrowInfo* const info = exception_throw_info(exception);
    const stxlib::uptr image_base = exception_image_base(exception);
    const CatchableType* const type = exact_catchable_type(*info, image_base);
    if (type == nullptr || (*type).size < 0) fail_fast();
    const stxlib::usize size = static_cast<stxlib::usize>((*type).size);
    if (size > max_rethrow_size) fail_fast();
    // The thrown copy lives in this frame, below every handler, until the
    // last handler ends its life.
    void* const copy = __builtin_alloca_with_align(size == 0 ? 1 : size, object_alignment * 8);
    copy_exception_object(*type, image_base, copy, (*block).object);
    exception.parameters[1] = reinterpret_cast<ULONG_PTR>(copy);
    add_uncaught(1);
    raise_handled(exception);
}
