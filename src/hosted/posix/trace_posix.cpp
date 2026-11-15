// Native traces on POSIX hosts (host.hpp, capture_return_addresses), shared
// by macOS, glibc, musl and FreeBSD. FreeBSD has no pthread_getattr_np; its
// pthread_attr_get_np (include/pthread_np.h:42 at fb9033d2876d, defined at
// lib/libthr/thread/thr_attr.c:125) fills an attribute object the caller
// has initialised with pthread_attr_init (A1 platform-rest).
//
// Primary mechanism: an owned walk of the frame-pointer chain, bounded by
// the calling thread's stack. The batch 1 fold (2026-11-13, sbi, Homebrew
// clang 22.1.8 on arm64 macOS) found _Unwind_Backtrace returning no frames
// when called from a shadow frame's destructor during phase-2 unwinding in
// Debug, UBSan and integer-check builds, while ASan and TSan builds
// returned frames; the cause is unconfirmed. The walk does not involve the
// unwinder at all, so it is the same inside a cleanup as anywhere else.
// _Unwind_Backtrace remains the fallback when the walk finds at most one
// record (no stack bounds, a first frame without a frame record, or a chain
// broken at the first caller; CI 43700804309, glibc Release without
// exceptions, captured one address).
//
// The frame record. AArch64: "The lowest addressed double-word shall point
// to the previous frame record", and "the highest addressed double-word
// shall contain the value passed in LR on entry to the current function"
// (ARM-software/abi-aa, aapcs64.rst at tag 2024Q3, "The Frame Pointer").
// x86-64 with a frame pointer: the prologue `push rbp; mov rbp, rsp` leaves
// the caller's rbp at [rbp] and the return address at [rbp + 8], the same
// layout (the standard prologue; the psABI makes the frame pointer
// optional; not checked at a pinned revision).
//
// Where frame pointers are kept: on Apple arm64 the frame chain is required
// to be valid at all times (Apple, "Writing ARM64 code for Apple
// platforms"; unconfirmed at a pinned revision: the page did not render
// for reading). Clang and GCC keep them at -O0, and sanitizer builds
// usually add -fno-omit-frame-pointer; CMakeLists.txt adds it to STXlib's
// targets whenever shadowing is on. Optimised Linux code built without
// that flag uses rbp or x29 as a general register, and the walk then stops
// early at the first frame whose record fails the checks below.
//
// Safety: every load is from inside the calling thread's stack, between
// its bounds, 8-byte aligned, and each next record must lie strictly above
// the current one, so a broken chain ends the walk; it never reads outside
// the stack and never loops.
//
// Report safety (capture_report_addresses, for capture_stack_trace and a
// fail-fast reporter; sb1, 2026-11-15, sources read as stated):
// - The bounds query allocates on glibc and FreeBSD, so a report never
//   makes it there. glibc 2.41 nptl/pthread_getattr_np.c takes the thread's
//   descriptor lock (lll_lock on the thread's lock), reallocs a cpuset for the
//   affinity copy on every call, and on the initial thread fopens
//   /proc/self/maps and reads it with getline. FreeBSD fb9033d2876d
//   lib/libthr/thread/thr_attr.c:137-142 mallocs the cpuset when the
//   attribute has none (as after pthread_attr_init), and :146 takes the
//   thread through _thr_find_thread, released at :152 or :165.
// - macOS: pthread_get_stackaddr_np and pthread_get_stacksize_np return the
//   calling thread's (or the main thread's) own fields without
//   _pthread_list_lock and without allocating; the main thread's size path
//   makes one getrlimit system call (apple-oss-distributions/libpthread,
//   src/pthread.c on its main branch as read 2026-11-15, not a pinned tag).
//   The walk is the report's mechanism there.
// - The unwinder, which a report uses on the other POSIX hosts: libgcc
//   14.2.0 (libgcc/unwind-dw2-fde-dip.c) looks up frames registered with
//   __register_frame_info first, then _dl_find_object where glibc has it
//   (2.35 and later), else dl_iterate_phdr, with a static header cache and
//   no allocation in that file. The registered path
//   (libgcc/unwind-dw2-fde.c, ATOMIC_FDE_FAST_PATH) is a lock-free b-tree
//   lookup, but the first lookup in an object not yet initialised takes
//   object_mutex and calls init_object, which allocates; without the fast
//   path every lookup takes object_mutex. Registered frames come from code
//   that calls __register_frame_info (JIT compilers); a process with none
//   never takes that path. glibc's _dl_find_object and dl_iterate_phdr,
//   and LLVM libunwind (FreeBSD's unwinder, and macOS's fallback when the
//   walk finds at most one record), were not read at a pin.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "hosted/host.hpp"

#include <cstring>

#include <pthread.h>
#include <unwind.h>

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#if defined(__APPLE__) && defined(__arm64e__)
#include <ptrauth.h>
#endif

namespace stxlib::host {

namespace {

// ---- the frame-pointer walk ------------------------------------------------

struct StackBounds final {
    uptr low{0};
    uptr high{0};
};

bool current_stack(StackBounds& bounds) noexcept {
#if defined(__APPLE__)
    const pthread_t self = ::pthread_self();
    const uptr top = reinterpret_cast<uptr>(::pthread_get_stackaddr_np(self));
    const uptr size = static_cast<uptr>(::pthread_get_stacksize_np(self));
    if (top == 0 || size == 0 || size > top) return false;
    bounds.low = top - size;
    bounds.high = top;
    return true;
#else
    pthread_attr_t attributes{};
#if defined(__FreeBSD__)
    if (::pthread_attr_init(&attributes) != 0) return false;
    if (::pthread_attr_get_np(::pthread_self(), &attributes) != 0) {
        static_cast<void>(::pthread_attr_destroy(&attributes));
        return false;
    }
#else
    if (::pthread_getattr_np(::pthread_self(), &attributes) != 0) return false;
#endif
    void* address = nullptr;
    size_t size = 0;
    const int got = ::pthread_attr_getstack(&attributes, &address, &size);
    static_cast<void>(::pthread_attr_destroy(&attributes));
    if (got != 0 || address == nullptr || size == 0) return false;
    bounds.low = reinterpret_cast<uptr>(address);
    if (size > ~bounds.low) return false;
    bounds.high = bounds.low + size;
    return true;
#endif
}

uptr strip(const uptr address) noexcept {
#if defined(__APPLE__) && defined(__arm64e__)
    const uptr result = reinterpret_cast<uptr>(
        ptrauth_strip(reinterpret_cast<void*>(address), ptrauth_key_return_address));
    return result;
#else
    return address;
#endif
}

uptr load(const uptr address) noexcept {
    uptr value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

// Walks from this function's own frame record: its saved return address is
// in its caller, capture_return_addresses, which is where the unwinder
// fallback's first recorded address also falls.
[[gnu::noinline]] usize walk_frames(uptr* const addresses, const usize capacity) noexcept {
    StackBounds bounds{};
    if (!current_stack(bounds)) return 0;
    uptr record = reinterpret_cast<uptr>(__builtin_frame_address(0));
    usize count = 0;
    while (count < capacity) {
        if (record < bounds.low || record % 8 != 0) break;
        if (record > bounds.high || bounds.high - record < 2 * sizeof(uptr)) break;
        const uptr previous = load(record);
        const uptr returned = strip(load(record + sizeof(uptr)));
        if (returned == 0) break;
        addresses[count] = returned;
        count = count + 1;
        if (previous <= record) break;
        record = previous;
    }
    return count;
}

// ---- the unwinder fallback -------------------------------------------------

struct TraceSink final {
    uptr* addresses{nullptr};
    usize capacity{0};
    usize count{0};
    usize skip{0};
};

_Unwind_Reason_Code record_frame(struct _Unwind_Context* const context, void* const argument) {
    TraceSink& sink = *static_cast<TraceSink*>(argument);
    if (sink.skip > 0) {
        sink.skip = sink.skip - 1;
        return _URC_NO_REASON;
    }
    if (sink.count == sink.capacity) return _URC_END_OF_STACK;
    const uptr address = static_cast<uptr>(_Unwind_GetIP(context));
    if (address == 0) return _URC_END_OF_STACK;
    sink.addresses[sink.count] = address;
    sink.count = sink.count + 1;
    return _URC_NO_REASON;
}

usize unwind_frames(uptr* const addresses, const usize capacity) noexcept {
    // Skips this function's own frame.
    TraceSink sink{addresses, capacity, 0, 1};
    static_cast<void>(_Unwind_Backtrace(record_frame, &sink));
    const usize result = sink.count;
    return result;
}

} // namespace

usize capture_return_addresses(uptr* const addresses, const usize capacity) noexcept {
    if (addresses == nullptr || capacity == 0) return 0;
    // A walk of one record means the chain broke at the first caller (code
    // built without frame pointers, such as an optimised Linux build); the
    // unwinder then reads the unwind tables instead, and the walk is kept
    // only when the unwinder finds less.
    const usize walked = walk_frames(addresses, capacity);
    if (walked >= 2 || walked == capacity) return walked;
    const usize unwound = unwind_frames(addresses, capacity);
    if (unwound >= walked) return unwound;
    const usize result = walk_frames(addresses, capacity);
    return result;
}

usize capture_report_addresses(uptr* const addresses, const usize capacity) noexcept {
    if (addresses == nullptr || capacity == 0) return 0;
#if defined(__APPLE__)
    // The walk's bounds are pthread_get_stackaddr_np and
    // pthread_get_stacksize_np on the calling thread, which read the thread's
    // own record without a lock or an allocation (above), so the walk is
    // report-safe here, and it is the mechanism that works inside a cleanup.
    const usize result = capture_return_addresses(addresses, capacity);
    return result;
#else
    // pthread_getattr_np (glibc) and pthread_attr_get_np (FreeBSD) allocate
    // and lock (above), so a report takes the unwinder only.
    const usize result = unwind_frames(addresses, capacity);
    return result;
#endif
}

} // namespace stxlib::host
