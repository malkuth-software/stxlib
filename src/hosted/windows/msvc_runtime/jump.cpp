// longjmp, with _setjmp and _setjmpex (setjmp_x64.S).
//
// On Windows longjmp unwinds: "In Microsoft C++ code on Windows, longjmp
// uses the same stack-unwinding semantics as exception-handling code"
// (Microsoft Learn, "longjmp"), so the frames it leaves run their __finally
// blocks and, under the frame handler, their C++ cleanups. When _setjmp
// recorded a frame (clang always passes one, setjmp_x64.S), longjmp starts
// a target unwind to that frame and the address after the _setjmp call with
// the status STATUS_LONGJUMP (Learn, "NTSTATUS Values") and the buffer as
// the record's one parameter; RtlUnwindEx calls each frame's handler with
// the unwind flags, then restores the target context.
//
// The restore: with STATUS_LONGJUMP, the system's context restore takes
// the nonvolatile registers, RSP, RIP and the floating-point control state
// from the jump buffer named in ExceptionInformation[0], in this layout,
// and returns the unwind's ReturnValue in RAX. That is general knowledge,
// not documented on Learn; it is why JumpBuffer has the field order below,
// and the conformance suite's jump case confirms it against Microsoft's
// longjmp. Without a recorded frame (a null second argument), longjmp
// reloads the buffer directly and unwinds nothing.
//
// Compiled without exceptions.

#include "system_interim.h"

#include "stxlib/types.hpp"

namespace stxlib::msvc_runtime {

namespace {

// The buffer _setjmp fills (setjmp_x64.S writes these offsets).
struct alignas(16) JumpBuffer final {
    u64 frame;
    u64 rbx;
    u64 rsp;
    u64 rbp;
    u64 rsi;
    u64 rdi;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 rip;
    u32 mxcsr;
    u16 x87_control;
    u16 spare;
    u8 xmm[10][16];
};

static_assert(sizeof(JumpBuffer) == 256, "jmp_buf holds 256 bytes");
static_assert(__builtin_offsetof(JumpBuffer, rip) == 80, "setjmp_x64.S writes RIP at 80");
static_assert(__builtin_offsetof(JumpBuffer, mxcsr) == 88, "setjmp_x64.S writes MXCSR at 88");
static_assert(__builtin_offsetof(JumpBuffer, xmm) == 96, "setjmp_x64.S writes XMM6 at 96");

} // namespace

} // namespace stxlib::msvc_runtime

extern "C" {

[[noreturn]] void stxlib_msvc_longjmp_restore(const void* buffer, int value);

// std-rules: allow(setjmp) this file implements longjmp for the platform; STXlib's own code never calls it
[[noreturn]] void __cdecl longjmp(void* const buffer, const int value) {
    const stxlib::msvc_runtime::JumpBuffer& jump = *static_cast<const stxlib::msvc_runtime::JumpBuffer*>(buffer);
    // longjmp(buffer, 0) makes setjmp return 1 (C17 7.13.2.1p4).
    const int result = value == 0 ? 1 : value;
    if (jump.frame == 0) {
        stxlib_msvc_longjmp_restore(buffer, result);
    }
    EXCEPTION_RECORD record{};
    record.ExceptionCode = STATUS_LONGJUMP;
    record.ExceptionFlags = 0;
    record.NumberParameters = 1;
    record.ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(buffer);
    CONTEXT context{};
    void* const target_frame = reinterpret_cast<void*>(jump.frame);
    void* const target_ip = reinterpret_cast<void*>(jump.rip);
    void* const returned = reinterpret_cast<void*>(static_cast<stxlib::uptr>(static_cast<stxlib::u32>(result)));
    RtlUnwindEx(target_frame, target_ip, &record, returned, &context, nullptr);
    // RtlUnwindEx returns only if the target frame is not on this stack.
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

} // extern "C"
