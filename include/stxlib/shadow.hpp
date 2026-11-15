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
// The capture (every build since the A2 swap-in; GUI.Forms' C ABI 1.2
// promises frames in release builds, and S5's reason, "release builds keep
// today's cost", holds because GUI.Forms records them today): the innermost
// unclosed frame an unwind leaves copies the logical stack as it was at the
// throw (its own frame included) into a shadow record, before popping. In
// a development build (STXLIB_SHADOW, CMakeLists.txt: on in Debug and
// sanitizer builds) it also takes a native trace of return addresses, and
// the gate adds the exception's type; release builds carry neither (S5 =
// 1). Outer frames of the same unwind see that capture pending and only
// pop. The gate that catches the exception attaches the shadow record to
// the error record it makes (ErrorRecord::shadow()). When no frame saw the
// unwind (possible only when a handler outside every gate parks an
// exception, park_shadow), the capture is taken at the handler and says so
// (CapturePoint::gate).
//
// Frames (the A2 swap-in, decision A2-1 option 1, with GUI.Forms' record
// form): a frame is a kind, two numbers, a sequence and two short texts, a
// subject and a name, copied at the push (never pointed to, so two runs'
// stacks compare). Kind 0, shadow_frame_kind_gate, is a gate's or an
// operation's own frame, whose name is the operation's name; every other
// kind is the consumer's (GUI.Forms' gf_shadow_frame_kind values). A text
// longer than its field is cut at the last whole UTF-8 sequence that fits,
// and the frame's flags say which text was cut.
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
// Every function here reads and writes only the calling thread's state; a
// ShadowReference is read only on the thread that made it.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/error.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

inline constexpr usize shadow_frame_capacity = 64;
inline constexpr usize shadow_trace_capacity = 32;
// Shadow records each thread keeps in a development build (STXLIB_SHADOW);
// a release build keeps 1, frames only (decision 2026-11-15, "S3"). An
// older record is overwritten.
inline constexpr usize shadow_record_capacity = 4;
// Error records from gates each thread keeps (development builds).
inline constexpr usize recent_failure_capacity = 8;

// The bytes a frame keeps of its subject and of its name.
inline constexpr usize shadow_subject_capacity = 48;
inline constexpr usize shadow_name_capacity = 32;

// Frame kinds are the consumer's, except 0: a gate's own frame (or
// in_shadow_frame's by operation), whose name is the operation's name and
// whose subject is empty.
inline constexpr u32 shadow_frame_kind_gate = 0;

// Bits of ShadowFrameRecord::flags: the text was longer than its field.
inline constexpr u32 shadow_subject_truncated = 1;
inline constexpr u32 shadow_name_truncated = 2;

// One logical frame, as the stack holds it and as it is read back (120
// bytes). The text arrays hold subject_count and name_count bytes of UTF-8,
// not terminated; the bytes after them are zero.
struct ShadowFrameRecord final {
    u32 kind{shadow_frame_kind_gate};
    u32 flags{0};
    u64 first{0};
    u64 second{0};
    u64 sequence{0};
    u32 subject_count{0};
    u32 name_count{0};
    char subject[shadow_subject_capacity]{};
    char name[shadow_name_capacity]{};

    // Borrowed from this record.
    [[nodiscard]] TextView subject_text() const& noexcept {
        const TextView result = detail::TextViewAccess::adopt_validated(subject, subject_count);
        return result;
    }
    TextView subject_text() const&& = delete;
    [[nodiscard]] TextView name_text() const& noexcept {
        const TextView result = detail::TextViewAccess::adopt_validated(name, name_count);
        return result;
    }
    TextView name_text() const&& = delete;
};

static_assert(sizeof(ShadowFrameRecord) == 120, "the frame record is GUI.Forms' 112-byte frame with u64 numbers");

// The frame a gate opens, which is the whole frame (so a consumer's frame
// that wraps a gated call does not appear twice). Written as
// GateFrame{"table.slot", table, slot} for a gate's own frame, or with
// designated initializers for a consumer kind. The operation names the
// error record either way, and is the frame's name when kind is
// shadow_frame_kind_gate (subject and name are then ignored). The texts are
// borrowed only for the push.
struct GateFrame final {
    Operation operation;
    u64 first{0};
    u64 second{0};
    u32 kind{shadow_frame_kind_gate};
    TextView subject{};
    TextView name{};
    u64 sequence{0};
};

// A consumer's frame with texts, for in_shadow_frame (GUI.Forms' input
// dispatch, layout and paint frames). Requirement: kind is not
// shadow_frame_kind_gate. The texts are borrowed only for the push.
struct ShadowFrameText final {
    u32 kind{0};
    TextView subject{};
    TextView name{};
    u64 first{0};
    u64 second{0};
    u64 sequence{0};
};

// The calling thread's stack beside its frames.
struct ShadowStackState final {
    usize depth{0};    // frames open now, recorded or not
    usize recorded{0}; // min(depth, shadow_frame_capacity)
    u64 pushes{0};     // every frame opened on the thread
    u64 dropped{0};    // every frame opened past shadow_frame_capacity
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

// One push: what the frame records. A gate frame's name is its
// operation's.
struct FramePush final {
    u32 kind{shadow_frame_kind_gate};
    TextView subject{};
    TextView name{};
    u64 first{0};
    u64 second{0};
    u64 sequence{0};
};

[[nodiscard]] inline FramePush push_of(const GateFrame& frame) noexcept {
    FramePush result{.kind = frame.kind,
                     .subject = frame.subject,
                     .name = frame.name,
                     .first = frame.first,
                     .second = frame.second,
                     .sequence = frame.sequence};
    if (frame.kind == shadow_frame_kind_gate) {
        result.subject = TextView();
        result.name = frame.operation.name();
    }
    return result;
}

[[nodiscard]] inline FramePush push_of(const ShadowFrameText& frame) noexcept {
    STXLIB_REQUIRE(frame.kind != shadow_frame_kind_gate, "a ShadowFrameText needs a consumer kind (not 0)");
    const FramePush result{.kind = frame.kind,
                           .subject = frame.subject,
                           .name = frame.name,
                           .first = frame.first,
                           .second = frame.second,
                           .sequence = frame.sequence};
    return result;
}

// Defined in src/core/shadow_frames.cpp (each module's copy), over the
// state in stxlib_state. shadow_frame_enter copies the frame (texts cut to
// their fields) and returns the depth it was pushed at; shadow_frame_leave
// pops it and, when `closed` is false and no capture of this unwind is
// pending, captures first.
[[nodiscard]] u32 shadow_frame_enter(const FramePush& push) noexcept;
void shadow_frame_leave(u32 depth, bool closed) noexcept;

// One frame for its lifetime, used only by the wrappers below and the gates:
// they construct it, call their target, call close() when the target has
// returned, and return. A destructor that finds it unclosed is running in
// an unwind. Constructor and destructor are inline and call out of line
// (the GUI.Forms coordinator's fix for per-module destructors, bf3d0b6f1).
// Requirement: frames are left in reverse order of opening on their thread
// (automatic objects only).
class STXLIB_EARLY_EXIT_GUARD STXLIB_EARLY_EXIT_OPEN ShadowFrameScope final {
public:
    explicit ShadowFrameScope(const FramePush& push) noexcept : depth_(shadow_frame_enter(push)) {}
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

namespace detail {

template <typename Target, typename... Arguments>
typename FrameTargetReturn<Target, Arguments...>::Type call_in_frame(const FramePush& push, Target& target,
                                                                     Arguments&... arguments) {
    using Return = typename FrameTargetReturn<Target, Arguments...>::Type;
    ShadowFrameScope frame(push);
    if constexpr (ReturnsVoid<Return>::value) {
        target(arguments...);
        frame.close();
    } else {
        // Not const: a const local is copied, not moved, on return.
        Return result = target(arguments...);
        frame.close();
        return result;
    }
}

} // namespace detail

// A frame opened by hand, for a consumer whose own frame types cannot name
// STXlib's (GUI.Forms' public ShadowFrameCall and ShadowFrameScope keep
// their declarations before 1.0, S3 = 1; their bodies call these).
struct ShadowFrameMark final {
    u32 depth{0};
};

// Pushes `frame` (texts copied and cut, as every frame's) and returns the
// mark the matching leave takes. Requirement: frame.kind is not
// shadow_frame_kind_gate.
[[nodiscard]] ShadowFrameMark enter_shadow_frame(const ShadowFrameText& frame) noexcept;

// Pops the frame `mark` names. closed: the frame's work returned normally;
// false: an exception is leaving it, which takes or keeps the throw-site
// capture exactly as a gate's frame does. Requirement: leaves are strictly
// nested (the innermost open frame of the calling thread).
void leave_shadow_frame(ShadowFrameMark mark, bool closed) noexcept;

// Calls target(arguments...) inside one shadow frame and returns its result
// (any type, or nothing). An exception passes through unchanged; if this
// is the innermost frame it leaves, the frame captures first. The
// operation form opens a gate-kind frame named by the operation.
template <typename Target, typename... Arguments>
typename detail::FrameTargetReturn<Target, Arguments...>::Type in_shadow_frame(const Operation operation,
                                                                               const u64 first, const u64 second,
                                                                               Target& target,
                                                                               Arguments&... arguments) {
    const GateFrame frame{operation, first, second};
    using Return = typename detail::FrameTargetReturn<Target, Arguments...>::Type;
    if constexpr (detail::ReturnsVoid<Return>::value) {
        detail::call_in_frame(detail::push_of(frame), target, arguments...);
    } else {
        Return result = detail::call_in_frame(detail::push_of(frame), target, arguments...);
        return result;
    }
}

// The same with a consumer's frame and its texts.
template <typename Target, typename... Arguments>
typename detail::FrameTargetReturn<Target, Arguments...>::Type in_shadow_frame(const ShadowFrameText& frame,
                                                                               Target& target,
                                                                               Arguments&... arguments) {
    using Return = typename detail::FrameTargetReturn<Target, Arguments...>::Type;
    if constexpr (detail::ReturnsVoid<Return>::value) {
        detail::call_in_frame(detail::push_of(frame), target, arguments...);
    } else {
        Return result = detail::call_in_frame(detail::push_of(frame), target, arguments...);
        return result;
    }
}

// True in a development build of STXlib: shadow records carry a native
// trace and gate records the exception's type. Frames are captured in
// every build.
[[nodiscard]] bool shadowing_enabled() noexcept;

// The calling thread's current shadow depth (frames pushed and not popped,
// including any beyond the capacity).
[[nodiscard]] usize shadow_depth() noexcept;

// Copies the calling thread's open frames, min(recorded, frames.count())
// of them, outermost first, and returns the stack's state. Never fails.
[[nodiscard]] ShadowStackState read_shadow_stack(MutableView<ShadowFrameRecord> frames) noexcept;

// Copies the shadow record `reference` names: up to frames.count() frames,
// outermost first, and up to trace.count() return addresses, innermost
// first (none in a release build). not_found when it is empty or has been
// overwritten (each thread keeps shadow_record_capacity in a development
// build, 1 in a release build).
[[nodiscard]] Result<ShadowSummary> read_shadow(ShadowReference reference, MutableView<ShadowFrameRecord> frames,
                                                MutableView<uptr> trace) noexcept;

// For code that carries an exception as a value and rethrows it later
// (GUI.Forms' class tables park an exception_ptr; A1 design, section 3.3).
// park_shadow, called inside the handler that parks the exception, returns
// the exception's shadow record (capturing at the handler, as a gate does,
// when no scope saw the unwind) and clears the pending capture.
// restore_shadow, called just before the rethrow, makes it pending again,
// so the scopes the rethrow unwinds keep it and the outer gate attaches it.
// Both work in every build (frames are captured in every build).
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
