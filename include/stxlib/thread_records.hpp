#pragma once

// Per-thread records (notes planning/stages/E-thread-records.md; decision
// 2026-11-15, notes decisions/2026-11-15-thread-local-exit-safety.md). A
// consumer registers a record type once per process; each thread that asks
// for the record gets its own, created on first access and released by
// STXlib's thread-exit hook when the thread exits normally, or earlier by
// end_thread_records. STXlib keeps its own per-thread state the same way, so
// no thread_local with a destructor exists anywhere (rule 2).
//
// Lifetime and ownership: stxlib_state owns every record's memory (STXlib's
// allocator; it never unwinds). A record belongs to its thread: a pointer to
// it may be handed to another thread, but the record lives only until its
// own thread's normal exit or end_thread_records. Process termination
// (exit, a return from main, fail-fast) releases no record on any thread
// (rule 1 as reworded); the main thread's records are released only if it
// ends through pthread_exit. Records of live threads stay reachable from a
// process-wide registry, so a leak checker sees them as live.
//
// Phases, per thread: once the thread-exit hook starts, or the thread calls
// end_thread_records, thread_record fails with ErrorKind::disposed and never
// creates or re-creates a record; existing_thread_record returns a record
// still waiting its turn (whole) and null for one let go or being released.
//
// The callbacks are foreign code (AGENTS.md, "Early exits and
// termination"). Both run on the record's thread with STXlib's invariants
// whole and no STXlib lock held: initialize before the record is published
// (an early exit leaks it), release after it is let go (an early exit leaks
// it; nothing is released twice). They must not longjmp out. Release
// callbacks run newest type first; STXlib's own state is released after
// them, so a release callback may call STXlib.
//
// STXlib's own per-thread cost: one trivially destructible thread_local
// anchor of 2,072 bytes (64-bit hosts; a static_assert bounds it), in the
// default TLS model (global-dynamic in the shared stxlib_state, so it loads
// through dlopen as well as at start; static TLS when loaded at start), plus
// a block allocated on the thread's first touch of STXlib. The anchor holds
// the thread's identity, its record counters, the fail-fast state, its
// stack bounds (recorded before any failure, for stack traces) and a
// failure reserve (owner decision 2026-11-15): a thread whose block could
// not be allocated, or whose block the hook has released, keeps numbering
// its error records and keeps an 8-frame shadow stack and one shadow record
// of up to 8 frames without a native trace (a block keeps 64 frames and 4
// traced records in a development build, 1 in a release build, plus the
// recent-gate ring); trampolines refuse to enter on such a thread.
//
// Windows: the hook runs at DLL_THREAD_DETACH under the loader lock. A
// release callback the hook runs there keeps to what Microsoft's
// "Dynamic-Link Library Best Practices" allows under that lock: Kernel32
// functions, memory and files; never functions in User32.dll or Gdi32.dll,
// LoadLibrary, the registry, or waiting on another thread. A thread whose
// records need more (GDI objects, windows) calls end_thread_records before
// it returns, which runs the same release sequence outside the lock.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// How many record types a process may register (STXlib's own state is not
// one of them), and the largest record size and alignment accepted.
inline constexpr usize thread_record_type_capacity = 16;
inline constexpr usize thread_record_size_limit = 1024 * 1024;
inline constexpr usize thread_record_alignment_limit = 64;

// A record type. `initialize` (optional) is called once on the record's
// thread after the record is allocated and zero-filled, before it is
// published; without it, zero-filled is the initial state. `release`
// (optional) is called once on the record's thread after the record is let
// go, before its memory is released.
struct ThreadRecordType final {
    usize size{0};      // 1 .. thread_record_size_limit
    usize alignment{0}; // a power of two, 1 .. thread_record_alignment_limit
    void (*initialize)(void* record, void* context) noexcept {nullptr};
    void (*release)(void* record, void* context) noexcept {nullptr};
    void* context{nullptr};
};

// A registered type: a copyable handle, valid for the process's life.
class ThreadRecordSlot final {
public:
    constexpr ThreadRecordSlot() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept { return index_ != u32_max; }
    [[nodiscard]] constexpr u32 index() const noexcept { return index_; }
    [[nodiscard]] static constexpr ThreadRecordSlot from_index(const u32 index) noexcept {
        ThreadRecordSlot result;
        result.index_ = index;
        return result;
    }

private:
    u32 index_{u32_max};
};

// Registers a type. Failures: invalid_argument (size or alignment out of
// range), limit_exceeded (thread_record_type_capacity types exist),
// disposed (the process is ending: on Windows, once ExitProcess has stopped
// the other threads).
//
// Registration is permanent: there is no call to remove a type, and every
// later thread exit, and every end_thread_records, may call its release.
// The module holding the callbacks and the context must therefore never be
// unloaded once it has registered; STXlib does not pin it. Unloading it
// leaves a call into unmapped code for the next thread that ends with one
// of its records, and loading it again registers again, using another of
// the thread_record_type_capacity entries.
[[nodiscard]] OperationResult<ThreadRecordSlot> register_thread_record(const ThreadRecordType& type) noexcept;

// The calling thread's record of `slot`, created on first access. Failures:
// invalid_argument (an invalid slot, or a call from inside this slot's own
// initialize); out_of_memory (the record's allocation failed, and a later
// call tries again; or the thread's own block could not be allocated,
// which holds for the thread's life); unsupported (the host gave the
// thread no exit hook, for the thread's life); disposed (this thread's
// records are ending or have ended, or the process is ending).
[[nodiscard]] OperationResult<void*> thread_record(ThreadRecordSlot slot) noexcept;

// The calling thread's record of `slot` if it exists and has not been let
// go, else null. Never allocates and never calls initialize.
[[nodiscard]] void* existing_thread_record(ThreadRecordSlot slot) noexcept;

// Releases the calling thread's records now, newest type first, as the
// thread-exit hook would, and ends them: afterwards thread_record fails with
// disposed on this thread. STXlib itself stays usable on the thread until
// it exits. Calling it again does nothing.
void end_thread_records() noexcept;

// The typed form. `Initialize` and `Release` are the consumer's functions,
// called with the record (initialize on zero-filled memory: construct the
// Record there; release: destroy it). Both run in the consumer's code.
template <typename Record>
class ThreadRecord final {
public:
    template <void (*Initialize)(Record* record) noexcept, void (*Release)(Record* record) noexcept>
    [[nodiscard]] static OperationResult<ThreadRecord> register_type() noexcept {
        ThreadRecordType type{};
        type.size = sizeof(Record);
        type.alignment = alignof(Record);
        type.initialize = &initialize_record<Initialize>;
        type.release = &release_record<Release>;
        OperationResult<ThreadRecordSlot> registered = register_thread_record(type);
        if (registered.failed()) return OperationResult<ThreadRecord>::failure(*registered.error());
        ThreadRecord result;
        result.slot_ = static_cast<OperationResult<ThreadRecordSlot>&&>(registered).take_value();
        return OperationResult<ThreadRecord>::success(result);
    }

    [[nodiscard]] OperationResult<Record*> get() const noexcept {
        OperationResult<void*> found = thread_record(slot_);
        if (found.failed()) return OperationResult<Record*>::failure(*found.error());
        Record* const record = static_cast<Record*>(static_cast<OperationResult<void*>&&>(found).take_value());
        return OperationResult<Record*>::success(record);
    }

    [[nodiscard]] Record* existing() const noexcept {
        Record* const result = static_cast<Record*>(existing_thread_record(slot_));
        return result;
    }

    [[nodiscard]] ThreadRecordSlot slot() const noexcept { return slot_; }

private:
    template <void (*Initialize)(Record* record) noexcept>
    static void initialize_record(void* const record, void*) noexcept {
        Initialize(static_cast<Record*>(record));
    }
    template <void (*Release)(Record* record) noexcept>
    static void release_record(void* const record, void*) noexcept {
        Release(static_cast<Record*>(record));
    }

    ThreadRecordSlot slot_{};
};

} // namespace stxlib
