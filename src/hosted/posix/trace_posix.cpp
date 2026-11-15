// Native traces on POSIX hosts (host.hpp, capture_return_addresses and
// capture_report_addresses), shared by macOS, FreeBSD and Linux whichever C
// library is loaded alongside STXlib (glibc, musl). Both calls take the same
// path: STXlib's own walk of the frame-pointer chain, bounded by the
// thread's stack as STXlib recorded it, with the unwinder as the fallback
// (notes decisions/2026-11-15-boundary-sideloading.md: STXlib owns what it
// can, so the walk is the same on every host).
//
// The bounds. stxlib_state records each thread's stack in its anchor
// (state.hpp, ThreadFlags::stack_low and stack_high) on a path that is not
// a failure: at its own load for the loading thread, when the thread's
// block is made, and when a Thread starts (state.cpp, note_stack_here;
// hosted/posix/stack_bounds_posix.cpp asks the host and says what each
// host does). The walk reads the anchor, which is constant-initialised
// thread_local storage already touched by fail_fast before any reporter
// runs, and asks the host nothing: no allocation, no lock, no system call,
// on glibc, musl and FreeBSD alike. A thread with no recorded bounds (one
// that never touched STXlib's thread state, and whose first touch is a
// failure; or a macOS thread after its thread-exit hook rebuilt the anchor)
// walks only on macOS, whose own query reads the thread's fields without a
// lock or an allocation (pthread_get_stackaddr_np, pthread_get_stacksize_np;
// apple-oss-distributions/libpthread, src/pthread.c on its main branch as
// read 2026-11-15, not a pinned tag; the main thread's size takes one
// getrlimit); elsewhere it takes the unwinder.
//
// Why a walk at all: the batch 1 fold (2026-11-13, sbi, Homebrew clang
// 22.1.8 on arm64 macOS) found _Unwind_Backtrace returning no frames when
// called from a shadow frame's destructor during phase-2 unwinding in
// Debug, UBSan and integer-check builds (cause unconfirmed), and libgcc's
// unwinder alone gave fewer than two frames on Alpine (CI 43733503918,
// linux-musl Plain, 2026-11-15). The walk does not involve the unwinder.
//
// The unwinder (_Unwind_Backtrace) is the fallback where the walk cannot
// run: no recorded bounds (above, not macOS), a first frame not on the
// recorded stack (an alternate signal stack, a fiber), or a chain broken
// at the first caller (code built without frame pointers, such as an
// optimised Linux build; CI 43700804309, glibc Release without exceptions,
// captured one address). What it does: libgcc 14.2.0
// (libgcc/unwind-dw2-fde-dip.c) looks up frames registered with
// __register_frame_info first, then _dl_find_object where glibc has it
// (2.35 and later), else dl_iterate_phdr, with a static header cache and no
// allocation in that file. The registered path (libgcc/unwind-dw2-fde.c,
// ATOMIC_FDE_FAST_PATH) is a lock-free b-tree lookup, but the first lookup
// in an object not yet initialised takes object_mutex and calls
// init_object, which allocates; without the fast path every lookup takes
// object_mutex. Registered frames come from code that calls
// __register_frame_info (JIT compilers); a process with none never takes
// that path. glibc's _dl_find_object and dl_iterate_phdr, and LLVM
// libunwind (FreeBSD's unwinder, and macOS's), were not read at a pin.
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
// Safety: every load is from inside the recorded stack, 8-byte aligned, at
// or above the walk's own frame, and each next record must lie strictly
// above the current one and below the stack's end, so a broken chain ends
// the walk; it never loops, and it reads only between a frame of the
// running stack and that stack's end (stack_bounds_posix.cpp says why that
// range is mapped, also on a Linux initial thread whose recorded lower
// bound is below the part mapped so far).

#include "hosted/host.hpp"
#include "state/state.hpp"

#include <cstring>

#include <unwind.h>

#if defined(__APPLE__)
#include <pthread.h>
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

// The calling thread's stack as STXlib recorded it; on macOS, when nothing
// is recorded, the thread's own fields (report-safe there, above).
bool current_stack(StackBounds& bounds) noexcept {
    const state::ThreadFlags& flags = state::current_flags();
    if (flags.stack_high != 0) {
        bounds.low = flags.stack_low;
        bounds.high = flags.stack_high;
        return true;
    }
#if defined(__APPLE__)
    const pthread_t self = ::pthread_self();
    const uptr top = reinterpret_cast<uptr>(::pthread_get_stackaddr_np(self));
    const uptr size = static_cast<uptr>(::pthread_get_stacksize_np(self));
    if (top == 0 || size == 0 || size > top) return false;
    bounds.low = top - size;
    bounds.high = top;
    return true;
#else
    return false;
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

// The same path: the walk asks the host nothing (above), so a report takes
// it too.
usize capture_report_addresses(uptr* const addresses, const usize capacity) noexcept {
    const usize result = capture_return_addresses(addresses, capacity);
    return result;
}

} // namespace stxlib::host
