#pragma once

// STXlib's per-thread and per-process state, owned by one module: the
// stxlib_state shared library (CMakeLists.txt). Every copy of STXlib's code
// (each shared library or executable that links stxlib::boundary
// statically) reaches the one state through the two exported accessors
// below, never through a thread_local or static of its own. Since layout 2
// that includes the platform layer's process-wide state: the console and
// environment locks, the console's read-ahead bytes, the exit-task table,
// the event watch and the raw-terminal flag. Layout 3 adds write_file's
// temporary-name serial and the abandoned child processes. Layout 4 adds
// thread identities (the issuer and each thread's own) and the local time
// zone's load state. Layout 5 adds the allocator (src/state/allocator.cpp):
// stxlib::memory's backend, tags and ownership check live here, so a block
// allocated in one module may be released in another. Layout 6 drops the
// zone's load state (owner decision D12 = a: no host zone rules are read)
// and adds Windows' ICU module handle. Layout 7 adds the SIMD level
// selection (stxlib::algorithms), so one choice holds in every module.
// Layout 8 (the A2 swap-in) stores frames as ShadowFrameRecords (kind,
// numbers, sequence and copied texts, 120 bytes) in the stack and the
// shadow records, counts dropped frames, and marks a thread running the
// fail-fast reporter. Layout 9 (decision 2026-11-15, notes
// decisions/2026-11-15-hand-in-questions.md, "S3"; audit A, S3) sizes the
// shadow records by build type: a development build (STXLIB_SHADOW) keeps
// 4, each with its native trace; a release build keeps 1, frames only, with
// no trace array. Release and development modules do not mix: the build
// type is part of the layout key the accessors check (layout_key, below).
// Layout 10 (decision 2026-11-15, notes
// decisions/2026-11-15-thread-local-exit-safety.md, rule 2; notes
// planning/stages/E-thread-records.md) moves ThreadState out of the
// thread_local into a block stxlib_state allocates on first access and its
// thread-exit hook releases; the only thread_local is a trivially
// destructible ThreadFlags anchor (identity, fail-fast depth and flag, the
// hook phase), and consumers register their own per-thread records
// (include/stxlib/thread_records.hpp). Layout 10 also gave AllocatorCalls
// `heap_failure`, mimalloc's report of detected heap corruption (smi's
// mimalloc audit, D6). Layout 11 (the owner, 2026-11-15; notes
// decisions/2026-11-15-gui-forms-priority.md) removes it with the C
// mimalloc: stx_minmallocpp stops the process itself on a contradiction in
// its own state, and the host backend detects none. Layout 12 (the owner,
// 2026-11-15; notes decisions/2026-11-15-boundary-sideloading.md) gives the
// anchor the thread's stack bounds, recorded by STXlib on a path that is
// not a failure (note_thread_stack, below) so the frame walk never asks the
// host at failure time.
//
// Why (the GUI.Forms coordinator's constraint, 2026-11-13): on Windows each
// module that links the static C++ runtime and STXlib statically has its
// own copy of every thread_local, static and non-inline function body; on
// ELF and Mach-O the same holds once modules hide their symbols (GUI.Forms'
// export lists). A shadow stack, a sequence counter or an open-invocation
// list duplicated per module would split: a scope in one module would push
// where a gate in another never looks.
//
// The structures are plain data with layouts this header defines. Every
// module is built from the same STXlib commit (one pin, decision M3), and
// the accessors check `layout_key` (the layout version and the build type)
// so a module built from a different layout, or a development module with
// a release stxlib_state or the reverse, fails at its first access instead
// of reading misplaced fields.
//
// Lifetime (layout 10): the thread state is a block stxlib_state allocates
// through its allocator on the thread's first access and releases from its
// own thread-exit hook (src/hosted/<family>/thread_exit_<family>.cpp, part
// of stxlib_state), once, at a normal thread exit; process termination
// releases nothing (rule 1 as reworded). The only thread_local is the
// trivially destructible ThreadFlags anchor (rule 2; D11). With no block
// (the hook has run, or the first allocation failed) a thread runs in
// degraded mode: current_thread_state() is null, the shadow facilities use
// the anchor's failure reserve (ThreadFlags::reserve) and each other
// facility says what it does without the block. The process state is a static
// of stxlib_state with constant initialisation. The allocator's backend is
// stx_minmallocpp (the default) or malloc.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/error.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/require.hpp"
#include "stxlib/shadow.hpp"
#include "stxlib/trampoline.hpp"
#include "stxlib/types.hpp"

#include <atomic>

#if defined(_WIN32)
#if defined(STXLIB_STATE_BUILDING)
#define STXLIB_STATE_API __declspec(dllexport)
#elif defined(STXLIB_STATE_SHARED)
#define STXLIB_STATE_API __declspec(dllimport)
#else
#define STXLIB_STATE_API
#endif
#else
#define STXLIB_STATE_API __attribute__((visibility("default")))
#endif

namespace stxlib::state {

// Raised whenever a structure below changes shape.
inline constexpr u32 layout_version = 12;

// The build type's part of the layout: development builds (STXLIB_SHADOW)
// lay ThreadState out differently from release builds, so the key the
// accessors compare carries it in its top bit. stxlib_state is compiled with
// the same STXLIB_SHADOW setting as the boundary (CMakeLists.txt).
#if defined(STXLIB_SHADOW)
inline constexpr u32 layout_build_type = static_cast<u32>(1) << 31;
// Shadow records each thread keeps, and whether each carries a native trace.
inline constexpr usize shadow_records_kept = shadow_record_capacity;
#else
inline constexpr u32 layout_build_type = 0;
inline constexpr usize shadow_records_kept = 1;
#endif
inline constexpr u32 layout_key = layout_version | layout_build_type;

// ProcessState::simd_selection before any request or selection.
inline constexpr i32 simd_unset = -1;

// One shadow record: the logical stack and, when TraceCapacity is not 0,
// the native trace for one exception.
template <usize FrameCapacity, usize TraceCapacity>
struct ShadowRecordData final {
    u64 sequence{0}; // 0: empty
    CapturePoint point{CapturePoint::gate};
    usize frame_count{0};
    usize frames_lost{0};
    usize trace_count{0};
    ShadowFrameRecord frames[FrameCapacity];
    uptr trace[TraceCapacity]{};
};

template <usize FrameCapacity>
struct ShadowRecordData<FrameCapacity, 0> final {
    u64 sequence{0}; // 0: empty
    CapturePoint point{CapturePoint::gate};
    usize frame_count{0};
    usize frames_lost{0};
    usize trace_count{0}; // always 0: no trace is kept
    ShadowFrameRecord frames[FrameCapacity];
};

// A shadow stack with its pending capture and its ring of shadow records
// (shadow_frames.cpp, shadow_capture.hpp). `depth` counts every pushed and
// unpopped frame, also those beyond the capacity, which are not stored;
// `pushes` counts every push ever, so a capture can tell whether normal
// execution has resumed since it was taken; `dropped` counts pushes past
// the capacity. The pending capture is the shadow record of the exception
// unwinding now, valid while `pushes` still equals `pending_pushes`. The
// records are a ring, newest at `shadow_newest`; their numbers come from
// ThreadFlags::last_shadow_sequence, one sequence per thread whichever
// store holds the record.
template <usize FrameCapacity, usize RecordsKept, usize TraceCapacity>
struct ShadowStore final {
    static constexpr usize frame_capacity = FrameCapacity;
    static constexpr usize records_kept = RecordsKept;
    static constexpr usize trace_capacity = TraceCapacity;
    using Record = ShadowRecordData<FrameCapacity, TraceCapacity>;

    u32 depth{0};
    u64 pushes{0};
    u64 dropped{0};
    ShadowFrameRecord frames[FrameCapacity];
    u64 pending_shadow{0};
    u64 pending_pushes{0};
    usize shadow_count{0};
    usize shadow_newest{0};
    Record shadows[RecordsKept];
};

#if defined(STXLIB_SHADOW)
inline constexpr usize full_trace_capacity = shadow_trace_capacity;
#else
inline constexpr usize full_trace_capacity = 0;
#endif
// The block's store: shadow_frame_capacity frames, shadow_records_kept
// records (4 with traces in a development build, 1 without in a release
// build).
using FullShadow = ShadowStore<shadow_frame_capacity, shadow_records_kept, full_trace_capacity>;
// The anchor's failure reserve (decision 2026-11-15, owner option 2 of
// E-thread-records.md section 7): an 8-frame stack and one record without
// a trace, used when the thread has no block.
inline constexpr usize reserve_frame_capacity = 8;
using ReserveShadow = ShadowStore<reserve_frame_capacity, 1, 0>;

// A kept error record. A union, so the array needs no ErrorRecord default
// (a record always holds a failure); assigning a record to the inactive
// member begins its lifetime (ErrorRecord's copy assignment is trivial,
// [class.union]).
union RecentSlot {
    struct Empty final {
    } empty;
    ErrorRecord record;

    constexpr RecentSlot() noexcept : empty() {}
};

// One open trampoline invocation of the thread.
struct OpenInvocation final {
    const void* table{nullptr};
    u64 handle{0};
};

struct ThreadState final {
    u32 layout{layout_key};

    // The shadow stack, its pending capture and its records.
    FullShadow shadow{};

    // Recent gate records (exception_shadow.cpp), a ring.
    usize recent_count{0};
    usize recent_newest{0};
    RecentSlot recent[recent_failure_capacity];

    // Open trampoline invocations (trampoline.cpp), a stack.
    usize open_depth{0};
    OpenInvocation open[trampoline_nesting_limit];

};

// The phase of a thread's records (ThreadFlags::phase; E-thread-records.md,
// section 2).
enum class ThreadPhase : u8 {
    none = 0,   // no block yet
    live = 1,   // the block exists; records may be created
    ending = 2, // end_thread_records or the hook started: no record is created
    ended = 3,  // the hook released the block: degraded mode
};

// The calling thread's anchor: stxlib_state's only thread_local (rule 2),
// trivially destructible and constant-initialised, so it is usable before
// any allocation, during and after the thread-exit hook, and on the
// fail-fast path. On macOS dyld frees its storage before the hook and makes
// it again, zero, when the hook reads it: the hook rebuilds it from the
// armed block (state.cpp, end_thread).
//
// Size (layout 12, by field arithmetic on a 64-bit host; the static_assert
// in state.cpp bounds it): the three counters 24 bytes, the depth, flag and
// phase bytes 8, the stack bounds 16, the reserve 2,016 (its stack 24 + 8 x
// 120 frames + 32 of pending capture and ring counters, its record 40 + 8 x
// 120): 2,064 bytes; with the block pointer the anchor is 2,072 bytes of
// TLS per thread.
//
// What a thread on the reserve records, compared with a block's
// ThreadState: error-record sequence numbers and shadow-record numbers are
// the same (they live here in both cases); the shadow stack keeps 8 frames,
// not 64 (deeper frames are counted, not stored); one shadow record of at
// most 8 frames and no native trace is kept, not 4 traced records of 64
// frames (development) or 1 of 64 (release); the recent-gate ring (8
// records, development) is not kept; trampolines refuse to enter (their
// open-invocation stack is not reserved).
struct ThreadFlags final {
    // The thread's identity (threads.cpp); 0 until issued. A Thread's host
    // thread takes the identity its owner issued before its entry runs.
    u64 thread_identity{0};
    // Error record sequence numbers (error.cpp), and shadow record numbers
    // (shadow_capture.hpp), per thread.
    u64 last_record_sequence{0};
    u64 last_shadow_sequence{0};
    // Entries into fail_fast (fail_fast.cpp); never reset.
    i32 fail_fast_depth{0};
    // True while this thread runs the installed fail-fast reporter; in a
    // development build a shadow frame opened then fails a requirement
    // (fail_fast.hpp lists what a reporter may call).
    bool in_reporter{false};
    ThreadPhase phase{ThreadPhase::none};
    // True once the block's allocation failed: degraded for the thread's
    // life (a later block would pair a shadow leave with an unrecorded
    // enter; E-thread-records.md, section 7).
    bool block_failed{false};
    // The thread's stack, [stack_low, stack_high), as STXlib found it on a
    // path that is not a failure (note_thread_stack;
    // hosted/posix/stack_bounds_posix.cpp); stack_high 0: not recorded. The frame walk (src/hosted/posix/trace_posix.cpp) reads
    // these and never asks the host itself. Recorded at stxlib_state's load
    // for the loading thread, at a thread's block, and at a Thread's start;
    // never while the thread fails fast or runs the reporter. On macOS the
    // hook's rebuild of the anchor (above) starts them again at 0.
    uptr stack_low{0};
    uptr stack_high{0};
    // The failure reserve: the shadow stack and record of a thread with no
    // block (its allocation failed, or the hook has released it).
    ReserveShadow reserve{};
};

// The per-thread budget, per build type (house style §1, "account for peak
// storage"; audit A, S3). Since layout 10 ThreadState is a block allocated
// on the thread's first touch of STXlib (threads that never touch it carry
// only the 2,072-byte anchor, ThreadFlags with its failure reserve). By field
// arithmetic on a 64-bit host:
//                         development                  release
//   frames            64 x 120          =  7,680       7,680
//   shadows            4 x (40 + 64 x 120 + 32 x 8)    1 x (40 + 64 x 120)
//                                       = 31,904       7,720
//   recent             8 x ErrorRecord (about 340)
//                                       =  2,720       2,720
//   open              32 x 16           =    512         512
//   counters and flags                    about 100   about 100
//   total                                about 42,900 about 18,700
// Each budget leaves about 5 KiB of margin for padding and for growth of
// ErrorRecord; a layout that needs more raises the budget here, with its
// reason, in the same change.
#if defined(STXLIB_SHADOW)
inline constexpr usize thread_state_budget = 48 * 1024;
#else
inline constexpr usize thread_state_budget = 24 * 1024;
#endif
static_assert(sizeof(ThreadState) <= thread_state_budget,
              "ThreadState exceeds its stated per-thread budget; see the arithmetic above");

// A process-wide lock (core/spin_lock.hpp takes and releases it). Plain
// data, so it lives here; zero is unlocked.
struct SpinLock final {
    std::atomic<u32> held{0};
};

// One registered exit task (process.cpp).
struct ExitTaskEntry final {
    void (*function)(void* context){nullptr};
    void* context{nullptr};
};

inline constexpr usize exit_task_capacity = 32;
// Bytes kept for each signal's earlier handling while an event watch is
// active (events_posix.cpp checks sizeof(struct sigaction) against it).
inline constexpr usize saved_signal_action_bytes = 256;
inline constexpr usize watched_signal_count = 5;
inline constexpr usize abandoned_child_capacity = 256;

enum class ReporterPhase : int {
    none = 0,
    installing = 1,
    installed = 2,
};

struct ProcessState final {
    u32 layout{layout_key};
    // Publishes `reporter`: written while installing, read after an acquire
    // load of installed (fail_fast.cpp).
    std::atomic<int> reporter_phase{static_cast<int>(ReporterPhase::none)};
    FailFastReporter reporter{};

    // The console (console.cpp). Output calls hold output_lock so no other
    // call's text comes between their pieces. Input calls hold input_lock;
    // `pending` holds bytes read past the last returned line, in a block
    // from STXlib's allocator (memory.hpp), which every module shares
    // (layout 5), so a line begun in one module's call continues in
    // another's. Never released: it lives as long as the process.
    SpinLock console_output_lock{};
    SpinLock console_input_lock{};
    char* console_pending{nullptr};
    usize console_pending_count{0};
    usize console_pending_capacity{0};
    bool console_input_ended{false};
    // Windows: a high surrogate read at the end of one console read.
    u16 console_pending_high_surrogate{0};

    // The environment (environment.cpp). `environment_generation` counts
    // successful changes, read under the same lock by code that caches
    // what the host derived from the environment.
    SpinLock environment_lock{};
    u64 environment_generation{0};

    // Exit tasks (process.cpp), a stack of exit_task_count entries.
    SpinLock exit_lock{};
    ExitTaskEntry exit_tasks[exit_task_capacity]{};
    usize exit_task_count{0};
    bool exit_handler_registered{false};

    // Process events (events_<family>.cpp). The signal or console handler
    // sets bits in pending_events only.
    std::atomic<u8> pending_events{0};
    std::atomic<u8> watched_events{0};
    std::atomic<bool> event_watch_active{false};
    alignas(16) unsigned char saved_signal_actions[watched_signal_count][saved_signal_action_bytes]{};

    // The raw terminal (terminal_<family>.cpp): true while one is held.
    std::atomic<bool> raw_terminal_active{false};

    // Files (files.cpp): the serial in write_file's temporary names, so two
    // modules never draw the same one.
    std::atomic<u64> temporary_serial{0};

    // Child processes (children_posix.cpp): children destroyed before being
    // waited for, reaped without blocking by later starts and waits. A fixed
    // table (stx_auxlib's list grew on the heap); when it is full and a
    // reaping pass frees nothing, the child is left to the process's end and
    // counted in abandoned_children_lost.
    SpinLock children_lock{};
    i64 abandoned_children[abandoned_child_capacity]{};
    usize abandoned_child_count{0};
    u64 abandoned_children_lost{0};
    // Threads (threads.cpp): the last thread identity issued; identities are
    // never reused, and the issuer refuses at 2^64 - 1.
    std::atomic<u64> last_thread_identity{0};
    // Windows' ICU (local_time_windows.cpp), loaded on first use and kept
    // for the process's life: its module handle, published once.
    std::atomic<void*> zone_mapping_module{nullptr};
    // The SIMD level (algorithms simd.cpp): simd_unset, a request (16 plus
    // the level) or the selected level (0 to 2); a selection is final.
    std::atomic<i32> simd_selection{simd_unset};
};

// What memory::release did with an address (allocator.cpp).
enum class ReleaseOutcome : u8 {
    released = 0,
    // Not a live block of STXlib's allocator (tag-checking builds only); the
    // caller reports it and ends the process.
    refused = 1,
};

// STXlib's one allocator (allocator.cpp, or stx_minmallocpp's
// state_calls.cpp; memory.hpp states the contract). `allocate` returns null
// for an alignment that is not a power of two: every backend tests it
// itself (allocator.cpp's allocate_block, with and without tags;
// stx_minmallocpp's allocate, on its common path and in allocate_general),
// so memory.cpp does not test it again (the efficiency review's F8).
// `backend` is memory::Backend's value; `tags_checked` whether this build
// tags blocks and checks them on release. One constant-initialised const
// object per stxlib_state, never changed, so a module may keep its address
// (memory.cpp).
struct AllocatorCalls final {
    void* (*allocate)(usize size, usize alignment) noexcept;
    ReleaseOutcome (*release)(void* address) noexcept;
    u8 backend;
    bool tags_checked;
};

// A per-thread record type (thread_records.hpp; E-thread-records.md). The
// callbacks are foreign code: called on the record's thread with
// stxlib_state's invariants whole (form 1).
inline constexpr usize thread_record_type_capacity = 16;
inline constexpr usize thread_record_size_limit = 1024 * 1024;
inline constexpr usize thread_record_alignment_limit = 64;

struct ThreadRecordTypeEntry final {
    usize size{0};
    usize alignment{0};
    void (*initialize)(void* record, void* context) noexcept {nullptr};
    void (*release)(void* record, void* context) noexcept {nullptr};
    void* context{nullptr};
};

// What a record call did (thread_record_register, thread_record_get).
enum class RecordStatus : u8 {
    ok = 0,
    layout_mismatch = 1,
    invalid_argument = 2, // a type out of range, an unknown slot, or a call from inside the slot's initialize
    limit_exceeded = 3,   // the type table is full
    out_of_memory = 4,    // the record's allocation failed; a later call tries again
    disposed = 5,         // this thread's records are ending or ended
    no_hook = 6,          // the host gave no thread-exit hook (POSIX: no key), so nothing could be released
};

// The calling thread's state (null in degraded mode or on a layout
// mismatch), its anchor (null only on a layout mismatch), the process's
// state and the allocator, from stxlib_state. `expected_layout` is the
// caller's layout_key; on a mismatch (another layout version, or the other
// build type) they return null and the current_ accessors below refuse.
// thread_state allocates the block on the thread's first call; it never
// unwinds (AGENTS.md, "Allocators must not unwind").
STXLIB_NO_UNWIND STXLIB_STATE_API ThreadState* thread_state(u32 expected_layout) noexcept;
STXLIB_STATE_API ThreadFlags* thread_flags(u32 expected_layout) noexcept;
STXLIB_STATE_API ProcessState* process_state(u32 expected_layout) noexcept;
STXLIB_STATE_API const AllocatorCalls* allocator(u32 expected_layout) noexcept;

// Per-thread records (E-thread-records.md, section 6).
STXLIB_STATE_API RecordStatus thread_record_register(u32 expected_layout, const ThreadRecordTypeEntry* type,
                                                     u32* slot) noexcept;
STXLIB_STATE_API RecordStatus thread_record_get(u32 expected_layout, u32 slot, void** record) noexcept;
STXLIB_STATE_API void* thread_record_existing(u32 expected_layout, u32 slot) noexcept;
STXLIB_STATE_API void thread_records_end(u32 expected_layout) noexcept;

// Records the calling thread's stack bounds in its anchor
// (ThreadFlags::stack_low and stack_high) unless they are recorded already
// or the thread is failing fast or running the reporter
// (fail_fast_depth, in_reporter). The host's query
// (hosted/stack_bounds_backend.hpp) may allocate, lock and make system
// calls, so only a path that is not a failure calls this: stxlib_state
// itself at its load and at a thread's block, and core/threads.cpp at a
// Thread's start. Never unwinds; no callback.
STXLIB_STATE_API void note_thread_stack(u32 expected_layout) noexcept;

// Ends the process after a layout mismatch, writing `message` to the host's
// error stream and aborting. It uses no thread state, so it cannot recurse
// into an accessor (src/hosted/<family>/thread_exit_<family>.cpp).
[[noreturn]] STXLIB_STATE_API void refuse_layout(const char* message) noexcept;

inline constexpr char layout_refusal[] =
    "stxlib: this module and stxlib_state were built with different state layouts or build types\n";

// The calling thread's state, or null in degraded mode (E-thread-records.md,
// section 2): every caller says what its facility does without it.
inline ThreadState* current_thread_state() noexcept {
    ThreadFlags* const flags = thread_flags(layout_key);
    if (flags == nullptr) refuse_layout(layout_refusal);
    ThreadState* const thread = thread_state(layout_key);
    return thread;
}

inline ThreadFlags& current_flags() noexcept {
    ThreadFlags* const flags = thread_flags(layout_key);
    if (flags == nullptr) refuse_layout(layout_refusal);
    return *flags;
}

inline ProcessState& current_process() noexcept {
    ProcessState* const process = process_state(layout_key);
    if (process == nullptr) refuse_layout(layout_refusal);
    return *process;
}

inline void note_current_stack() noexcept {
    note_thread_stack(layout_key);
}

inline const AllocatorCalls& current_allocator() noexcept {
    const AllocatorCalls* const calls = allocator(layout_key);
    if (calls == nullptr) refuse_layout(layout_refusal);
    return *calls;
}

} // namespace stxlib::state
