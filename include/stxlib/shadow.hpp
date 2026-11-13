#pragma once

// Exception shadowing in development builds (survey S1 = 1, owner
// 2026-11-13: a gate catch plus capture in the innermost shadow scope, no
// runtime hook; A1 design, notes planning/stages/A1-shadowing.md).
//
// The logical stack. in_shadow_frame(operation, first, second, target,
// arguments...) pushes one frame (an operation name and two numbers the
// operation defines, such as a table and a slot) on the calling thread's
// shadow stack, calls the named target, marks the frame closed, and returns
// the target's result; the frame is popped on every way out. Every gate
// (gate.hpp) opens a frame the same way for its operation. The stack holds
// shadow_frame_capacity frames; deeper frames are counted, not recorded.
//
// Detecting an unwind (the GUI.Forms coordinator's instruction,
// 2026-11-13): a frame left without having been marked closed is being left
// by an exception. No query of the C++ runtime is made: with a static
// runtime per module, std::uncaught_exceptions() counts only the calling
// module's throws, and GUI.Forms' Windows tests showed a frame opened in a
// library unseen by an exception thrown in the executable. The closed mark
// is a member of the frame's own scope object, written by the wrapper that
// opened it, so it is right whichever module threw.
//
// The capture. In a development build (STXLIB_SHADOW, CMakeLists.txt: on in
// Debug and sanitizer builds), the innermost unclosed frame an unwind
// leaves copies the logical stack as it was at the throw (its own frame
// included) and a native trace of return addresses into a shadow record,
// before popping. Outer frames of the same unwind see that capture pending
// and only pop. The gate that catches the exception attaches the shadow
// record to the error record it makes (ErrorRecord::shadow()). When no
// frame saw the unwind (possible only when a handler outside every gate
// parks an exception, park_shadow), the capture is taken at the handler and
// says so (CapturePoint::gate).
//
// Modules (the GUI.Forms coordinator's constraint, 2026-11-13): when STXlib
// is linked into several shared libraries, each has its own copy of
// STXlib's code and, with a static C++ runtime, its own runtime. All
// per-thread and per-process state (the shadow stack, the pending capture,
// the shadow records, the recent error records, the sequence counters, the
// open trampoline invocations, the fail-fast reporter) therefore lives in
// one module, the stxlib_state shared library, and every copy of STXlib's
// code reaches it through its exported accessors (src/state/state.hpp).
// A frame's scope object is inline and lives on its caller's stack, so it
// needs no shared state of its own.
//
// Release builds keep the logical stack (it is cheap and readable for
// diagnostics) but capture nothing: records carry no frames, no trace and
// no type (survey S5 = 1).

#include "stxlib/error.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

inline constexpr usize shadow_frame_capacity = 64;
inline constexpr usize shadow_trace_capacity = 32;
// Shadow records each thread keeps; an older one is overwritten.
inline constexpr usize shadow_record_capacity = 4;
// Error records from gates each thread keeps (development builds).
inline constexpr usize recent_failure_capacity = 8;

// One logical frame, as read back.
struct ShadowFrameRecord final {
    TextView operation{};
    u64 first{0};
    u64 second{0};
};

enum class CapturePoint : u8 {
    throw_site = 1, // the innermost scope the unwind left: the stack at the throw
    gate = 2,       // no scope saw the unwind: the stack at the catch
};

// What read_shadow found.
struct ShadowSummary final {
    CapturePoint point{CapturePoint::gate};
    usize frame_count{0};  // frames recorded, outermost first
    usize frames_lost{0};  // frames deeper than shadow_frame_capacity, not recorded
    usize trace_count{0};  // return addresses recorded, innermost first
};

namespace detail {

// Defined in src/core/shadow_frames.cpp (each module's copy), over the
// state in stxlib_state. shadow_frame_enter returns the depth the frame was
// pushed at; shadow_frame_leave pops it and, when `closed` is false and no
// capture of this unwind is pending, captures first.
[[nodiscard]] u32 shadow_frame_enter(TextView operation, u64 first, u64 second) noexcept;
void shadow_frame_leave(u32 depth, bool closed) noexcept;

// One frame for its lifetime, used only by the wrappers below and the gates:
// they construct it, call their target, call close() when the target has
// returned, and return. A destructor that finds it unclosed is running in
// an unwind. Constructor and destructor are inline and call out of line
// (the GUI.Forms coordinator's fix for per-module destructors, bf3d0b6f1).
// Requirement: frames are left in reverse order of opening on their thread
// (automatic objects only).
class ShadowFrameScope final {
public:
    ShadowFrameScope(const Operation operation, const u64 first, const u64 second) noexcept
        : depth_(shadow_frame_enter(operation.name(), first, second)) {}
    ~ShadowFrameScope() { shadow_frame_leave(depth_, closed_); }

    ShadowFrameScope(const ShadowFrameScope&) = delete;
    ShadowFrameScope& operator=(const ShadowFrameScope&) = delete;
    ShadowFrameScope(ShadowFrameScope&&) = delete;
    ShadowFrameScope& operator=(ShadowFrameScope&&) = delete;

    // The target returned normally.
    void close() noexcept { closed_ = true; }

private:
    u32 depth_;
    bool closed_{false};
};

// The type target(arguments...) returns.
template <typename Type>
Type&& declare_value() noexcept;
template <typename Target, typename... Arguments>
struct FrameTargetReturn final {
    using Type = decltype(declare_value<Target&>()(declare_value<Arguments&>()...));
};
template <typename Type>
struct ReturnsVoid final {
    static constexpr bool value = false;
};
template <>
struct ReturnsVoid<void> final {
    static constexpr bool value = true;
};

} // namespace detail

// Calls target(arguments...) inside one shadow frame and returns its result
// (any type, or nothing). An exception passes through unchanged; if this
// is the innermost frame it leaves, the frame captures first.
template <typename Target, typename... Arguments>
typename detail::FrameTargetReturn<Target, Arguments...>::Type in_shadow_frame(const Operation operation,
                                                                               const u64 first, const u64 second,
                                                                               Target& target,
                                                                               Arguments&... arguments) {
    using Return = typename detail::FrameTargetReturn<Target, Arguments...>::Type;
    detail::ShadowFrameScope frame(operation, first, second);
    if constexpr (detail::ReturnsVoid<Return>::value) {
        target(arguments...);
        frame.close();
    } else {
        // Not const: a const local is copied, not moved, on return.
        Return result = target(arguments...);
        frame.close();
        return result;
    }
}

// True in a development build of STXlib.
[[nodiscard]] bool shadowing_enabled() noexcept;

// The calling thread's current shadow depth (frames pushed and not popped,
// including any beyond the capacity).
[[nodiscard]] usize shadow_depth() noexcept;

// Copies the shadow record `reference` names: up to frames.count() frames,
// outermost first, and up to trace.count() return addresses, innermost
// first. not_found when it is empty or has been overwritten (each thread
// keeps shadow_record_capacity). The frames' operation names have static
// storage.
[[nodiscard]] Result<ShadowSummary> read_shadow(ShadowReference reference, MutableView<ShadowFrameRecord> frames,
                                                MutableView<uptr> trace) noexcept;

// For code that carries an exception as a value and rethrows it later
// (GUI.Forms' class tables park an exception_ptr; A1 design, section 3.3).
// park_shadow, called inside the handler that parks the exception, returns
// the exception's shadow record (capturing at the handler, as a gate does,
// when no scope saw the unwind) and clears the pending capture.
// restore_shadow, called just before the rethrow, makes it pending again,
// so the scopes the rethrow unwinds keep it and the outer gate attaches it.
// Both do nothing in a release build (park_shadow returns no reference).
[[nodiscard]] ShadowReference park_shadow() noexcept;
void restore_shadow(ShadowReference reference) noexcept;

// The number of gate records the calling thread keeps now (at most
// recent_failure_capacity; 0 when shadowing is off).
[[nodiscard]] usize recent_failure_count() noexcept;

// A copy of the calling thread's gate record `age` steps back (0 is the
// newest), or not_found when the thread keeps fewer than age + 1.
[[nodiscard]] Result<ErrorRecord> recent_failure(usize age) noexcept;

// Forgets the calling thread's kept records.
void clear_recent_failures() noexcept;

} // namespace stxlib
