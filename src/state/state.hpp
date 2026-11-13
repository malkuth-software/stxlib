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
// the accessors check `layout_version` so a module built from a different
// layout fails at its first access instead of reading misplaced fields.
//
// Lifetime: the thread state is a thread_local of stxlib_state, trivially
// destructible and constant-initialised, so it needs no thread-exit
// handling (owner decision D11: no thread-local storage with a non-trivial
// destructor). The process state is a static of stxlib_state with constant
// initialisation. Neither ever allocates; the allocator is the one part of
// stxlib_state that calls anything (its backend: mimalloc or malloc).

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
inline constexpr u32 layout_version = 7;

// ProcessState::simd_selection before any request or selection.
inline constexpr i32 simd_unset = -1;

// One logical frame on the shadow stack. The name has static storage (an
// Operation's).
struct FrameEntry final {
    const char* name{nullptr};
    usize name_count{0};
    u64 first{0};
    u64 second{0};
};

// One shadow record: the logical stack and native trace for one exception.
struct ShadowRecordData final {
    u64 sequence{0}; // 0: empty
    CapturePoint point{CapturePoint::gate};
    usize frame_count{0};
    usize frames_lost{0};
    usize trace_count{0};
    FrameEntry frames[shadow_frame_capacity];
    uptr trace[shadow_trace_capacity]{};
};

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
    u32 layout{layout_version};

    // Error record sequence numbers (error.cpp).
    u64 last_record_sequence{0};

    // The shadow stack (shadow_frames.cpp). `depth` counts every pushed and
    // unpopped frame, also those beyond the capacity, which are not stored.
    // `pushes` counts every push ever, so a capture can tell whether normal
    // execution has resumed since it was taken.
    u32 depth{0};
    u64 pushes{0};
    FrameEntry frames[shadow_frame_capacity];

    // The pending capture: the shadow record of the exception unwinding now,
    // valid while `pushes` still equals `pending_pushes`.
    u64 pending_shadow{0};
    u64 pending_pushes{0};

    // Shadow records, a ring of shadow_record_capacity, newest at
    // `shadow_newest`; `last_shadow_sequence` issues their numbers.
    u64 last_shadow_sequence{0};
    usize shadow_count{0};
    usize shadow_newest{0};
    ShadowRecordData shadows[shadow_record_capacity];

    // Recent gate records (exception_shadow.cpp), a ring.
    usize recent_count{0};
    usize recent_newest{0};
    RecentSlot recent[recent_failure_capacity];

    // Open trampoline invocations (trampoline.cpp), a stack.
    usize open_depth{0};
    OpenInvocation open[trampoline_nesting_limit];

    // Entries into fail_fast (fail_fast.cpp); never reset.
    int fail_fast_depth{0};
    // The thread's identity (threads.cpp); 0 until issued. A Thread's host
    // thread takes the identity its owner issued before its entry runs.
    u64 thread_identity{0};
};

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
    u32 layout{layout_version};
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

// STXlib's one allocator (allocator.cpp; memory.hpp states the contract).
// `backend` is memory::Backend's value; `tags_checked` whether this build
// tags blocks and checks them on release.
struct AllocatorCalls final {
    void* (*allocate)(usize size, usize alignment) noexcept;
    ReleaseOutcome (*release)(void* address) noexcept;
    u8 backend;
    bool tags_checked;
};

// The calling thread's state, the process's and the allocator, from
// stxlib_state. `expected_layout` is the caller's layout_version; on a
// mismatch they return null, and the caller's copy of STXlib reports it.
STXLIB_STATE_API ThreadState* thread_state(u32 expected_layout) noexcept;
STXLIB_STATE_API ProcessState* process_state(u32 expected_layout) noexcept;
STXLIB_STATE_API const AllocatorCalls* allocator(u32 expected_layout) noexcept;

inline ThreadState& current_thread() noexcept {
    ThreadState* const thread = thread_state(layout_version);
    STXLIB_REQUIRE(thread != nullptr, "this module and stxlib_state were built with different state layouts");
    return *thread;
}

inline ProcessState& current_process() noexcept {
    ProcessState* const process = process_state(layout_version);
    STXLIB_REQUIRE(process != nullptr, "this module and stxlib_state were built with different state layouts");
    return *process;
}

inline const AllocatorCalls& current_allocator() noexcept {
    const AllocatorCalls* const calls = allocator(layout_version);
    STXLIB_REQUIRE(calls != nullptr, "this module and stxlib_state were built with different state layouts");
    return *calls;
}

} // namespace stxlib::state
