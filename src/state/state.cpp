// The stxlib_state module (state.hpp): the one owner of STXlib's
// per-thread and per-process state. Built as a shared library whenever
// STXlib may be linked into more than one module (STXLIB_STATE_SHARED,
// CMakeLists.txt).
//
// Per-thread state (layout 10; notes planning/stages/E-thread-records.md):
// the only thread_local is the Anchor below, trivially destructible
// (decision 2026-11-15, rule 2). On a thread's first touch, the block
// holding its ThreadState and its consumer records is allocated through
// the allocator (allocator.cpp), which never unwinds, linked into the
// registry (so it stays reachable from a global root: LeakSanitizer sees
// the main thread's block as live), and armed with the host's thread-exit
// hook (thread_exit_backend.hpp). The hook releases it once, at a normal
// thread exit; process termination releases nothing (rule 1 as reworded).
//
// Early exits (AGENTS.md): the only foreign calls here are a record type's
// initialize and release callbacks and the allocator backend (STXLIB_NO_UNWIND).
// Both callbacks are called with every invariant of this file whole: the
// record is unpublished (initialize) or already let go (release), and no
// lock is held.

#include "state/state.hpp"

#include "hosted/thread_exit_backend.hpp"
#include "stxlib/detail/early_exit.hpp"

#include <atomic>
#include <new>
#include <type_traits>

namespace stxlib::state {

namespace {

struct ThreadBlock final {
    ThreadState state{};
    void* records[thread_record_type_capacity]{};
    // Slots whose initialize is running on this thread (bit per slot).
    u32 initializing{0};
    // The registry's links (registry_lock).
    ThreadBlock* previous{nullptr};
    ThreadBlock* next{nullptr};
};

struct Anchor final {
    ThreadFlags flags{};
    ThreadBlock* block{nullptr};
};

static_assert(std::is_trivially_destructible_v<Anchor>, "rule 2: every thread_local is trivially destructible");
static_assert(sizeof(Anchor) <= 2112, "the anchor (2,056 bytes by state.hpp's arithmetic) is the only TLS stxlib_state adds per thread");

constinit thread_local Anchor anchor{};
constinit ProcessState process_state_instance{};

// The record types: entries [0, type_count) are immutable once published by
// the release store of type_count; registration is serialised by type_lock.
constinit ThreadRecordTypeEntry types[thread_record_type_capacity]{};
constinit std::atomic<u32> type_count{0};
constinit std::atomic<bool> type_lock{false};

// Every live block, so each stays reachable from a global root until the
// hook releases it. The lock is held only around list edits, which call
// nothing (under Windows' loader lock it never waits on a thread that waits
// on the loader lock).
constinit ThreadBlock* registry_head{nullptr};
constinit std::atomic<bool> registry_lock{false};

class STXLIB_EARLY_EXIT_LOCK FlagLock final {
public:
    explicit FlagLock(std::atomic<bool>& flag) noexcept : flag_(flag) {
        while (flag_.exchange(true, std::memory_order_acquire)) {
            while (flag_.load(std::memory_order_relaxed)) {
            }
        }
    }
    ~FlagLock() { flag_.store(false, std::memory_order_release); }
    FlagLock(const FlagLock&) = delete;
    FlagLock& operator=(const FlagLock&) = delete;

private:
    std::atomic<bool>& flag_;
};

void link_block(ThreadBlock& block) noexcept {
    const FlagLock lock(registry_lock);
    block.previous = nullptr;
    block.next = registry_head;
    if (registry_head != nullptr) (*registry_head).previous = &block;
    registry_head = &block;
}

void unlink_block(ThreadBlock& block) noexcept {
    const FlagLock lock(registry_lock);
    if (block.previous != nullptr) {
        (*block.previous).next = block.next;
    } else {
        registry_head = block.next;
    }
    if (block.next != nullptr) (*block.next).previous = block.previous;
    block.previous = nullptr;
    block.next = nullptr;
}

STXLIB_NO_UNWIND void* allocate_bytes(const usize size, const usize alignment) noexcept {
    const AllocatorCalls* const calls = allocator(layout_key);
    void* const result = (*calls).allocate(size, alignment);
    return result;
}

STXLIB_NO_UNWIND void release_bytes(void* const address) noexcept {
    const AllocatorCalls* const calls = allocator(layout_key);
    // A block this module allocated is never refused; were it refused, it
    // is left in place rather than released twice.
    static_cast<void>((*calls).release(address));
}

// The calling thread's block, allocated on first use; null in degraded mode
// (the hook has released it, or its allocation failed once). A thread whose
// records ended before it touched STXlib still gets a block, in phase
// ending.
STXLIB_NO_UNWIND ThreadBlock* block_for_thread(const bool create) noexcept {
    if (anchor.block != nullptr) return anchor.block;
    if (!create || anchor.flags.phase == ThreadPhase::ended || anchor.flags.block_failed) return nullptr;
    void* const memory = allocate_bytes(sizeof(ThreadBlock), alignof(ThreadBlock));
    if (memory == nullptr) {
        anchor.flags.block_failed = true;
        return nullptr;
    }
    ThreadBlock* const block = ::new (memory) ThreadBlock{};
    if (!thread_exit_backend::arm(block)) {
        // No hook: a block nothing would release is never made.
        release_bytes(memory);
        anchor.flags.block_failed = true;
        return nullptr;
    }
    link_block(*block);
    anchor.block = block;
    if (anchor.flags.phase == ThreadPhase::none) anchor.flags.phase = ThreadPhase::live;
    return block;
}

// Releases the consumer records, newest type first: each is let go (its
// slot cleared) before its release callback runs and its memory is freed,
// so an early exit from the callback leaks that record and frees nothing
// twice. The phase is ending first, so a callback cannot create a record.
void release_records(ThreadBlock& block) noexcept {
    const u32 count = type_count.load(std::memory_order_acquire);
    for (u32 index = count; index > 0; --index) {
        const u32 slot = index - 1;
        void* const record = block.records[slot];
        if (record == nullptr) continue;
        block.records[slot] = nullptr;
        const ThreadRecordTypeEntry& type = types[slot];
        if (type.release != nullptr) {
            STXLIB_EARLY_EXIT_WHOLE("the record is let go and no lock is held; an early exit leaks it");
            type.release(record, type.context);
        }
        release_bytes(record);
    }
}

} // namespace

ThreadState* thread_state(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return nullptr;
    ThreadBlock* const block = block_for_thread(true);
    if (block == nullptr) return nullptr;
    ThreadState* const result = &(*block).state;
    return result;
}

ThreadFlags* thread_flags(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return nullptr;
    ThreadFlags* const result = &anchor.flags;
    return result;
}

ProcessState* process_state(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return nullptr;
    ProcessState* const result = &process_state_instance;
    return result;
}

RecordStatus thread_record_register(const u32 expected_layout, const ThreadRecordTypeEntry* const type,
                                    u32* const slot) noexcept {
    if (expected_layout != layout_key) return RecordStatus::layout_mismatch;
    if (type == nullptr || slot == nullptr) return RecordStatus::invalid_argument;
    const ThreadRecordTypeEntry entry = *type;
    const bool alignment_valid = entry.alignment != 0 && (entry.alignment & (entry.alignment - 1)) == 0 &&
                                 entry.alignment <= thread_record_alignment_limit;
    if (entry.size == 0 || entry.size > thread_record_size_limit || !alignment_valid) {
        return RecordStatus::invalid_argument;
    }
    const FlagLock lock(type_lock);
    const u32 count = type_count.load(std::memory_order_relaxed);
    if (count == thread_record_type_capacity) return RecordStatus::limit_exceeded;
    types[count] = entry;
    type_count.store(count + 1, std::memory_order_release);
    *slot = count;
    return RecordStatus::ok;
}

RecordStatus thread_record_get(const u32 expected_layout, const u32 slot, void** const record) noexcept {
    if (expected_layout != layout_key) return RecordStatus::layout_mismatch;
    if (record == nullptr || slot >= type_count.load(std::memory_order_acquire)) return RecordStatus::invalid_argument;
    if (anchor.flags.phase == ThreadPhase::ending || anchor.flags.phase == ThreadPhase::ended) {
        return RecordStatus::disposed;
    }
    ThreadBlock* const block = block_for_thread(true);
    if (block == nullptr) return RecordStatus::out_of_memory;
    if ((*block).records[slot] != nullptr) {
        *record = (*block).records[slot];
        return RecordStatus::ok;
    }
    const u32 bit = static_cast<u32>(1) << slot;
    if (((*block).initializing & bit) != 0) return RecordStatus::invalid_argument;
    const ThreadRecordTypeEntry& type = types[slot];
    void* const memory = allocate_bytes(type.size, type.alignment);
    if (memory == nullptr) return RecordStatus::out_of_memory;
    unsigned char* const bytes = static_cast<unsigned char*>(memory);
    for (usize index = 0; index < type.size; ++index) bytes[index] = 0;
    if (type.initialize != nullptr) {
        (*block).initializing = (*block).initializing | bit;
        STXLIB_EARLY_EXIT_WHOLE("the record is unpublished and no lock is held; an early exit leaks it");
        type.initialize(memory, type.context);
        (*block).initializing = (*block).initializing & ~bit;
        if (anchor.flags.phase != ThreadPhase::live) {
            // initialize ended this thread's records: this one is never
            // published, so it is released here, once.
            if (type.release != nullptr) {
                STXLIB_EARLY_EXIT_WHOLE("the record was never published and no lock is held");
                type.release(memory, type.context);
            }
            release_bytes(memory);
            return RecordStatus::disposed;
        }
    }
    (*block).records[slot] = memory;
    *record = memory;
    return RecordStatus::ok;
}

void* thread_record_existing(const u32 expected_layout, const u32 slot) noexcept {
    if (expected_layout != layout_key) return nullptr;
    if (slot >= type_count.load(std::memory_order_acquire)) return nullptr;
    ThreadBlock* const block = block_for_thread(false);
    if (block == nullptr) return nullptr;
    void* const result = (*block).records[slot];
    return result;
}

void thread_records_end(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return;
    if (anchor.flags.phase == ThreadPhase::ending || anchor.flags.phase == ThreadPhase::ended) return;
    anchor.flags.phase = ThreadPhase::ending;
    // Without a block there is nothing to release.
    if (anchor.block != nullptr) release_records(*anchor.block);
}

namespace detail {

// The host's thread-exit hook (thread_exit_backend.hpp), on the exiting
// thread: end the consumer records, then let go of the block (anchor and
// registry) and release it, ThreadState last.
void end_thread(void* const armed) noexcept {
    ThreadBlock* const block = anchor.block;
    if (block == nullptr || block != armed) return;
    if (anchor.flags.phase == ThreadPhase::live) {
        anchor.flags.phase = ThreadPhase::ending;
        release_records(*block);
    }
    anchor.block = nullptr;
    anchor.flags.phase = ThreadPhase::ended;
    unlink_block(*block);
    (*block).~ThreadBlock();
    release_bytes(block);
}

} // namespace detail

} // namespace stxlib::state
