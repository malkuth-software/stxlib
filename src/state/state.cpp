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
// initialize and release callbacks, the allocator backend (STXLIB_NO_UNWIND)
// and the stack-bounds query (stack_bounds_backend.hpp: no callback, its
// cancellation points held off), made before anything is changed.
// Both callbacks are called with every invariant of this file whole: the
// record is unpublished (initialize) or already let go (release), and no
// lock is held.
//
// The debugger anchor (stxlib/debugger_anchor.hpp): this module's
// `.stxshdw` descriptor names the registry in the process state. A thread
// claims an entry when its block is made (the block's store, 64 frames) or
// when it falls back to the failure reserve (the anchor's reserve, 8
// frames), and its hook clears the entry before the block is released.
// Claiming is a bounded scan (at most debugger_thread_capacity entries)
// with one compare-exchange per free entry tried; clearing is three atomic
// stores. Neither locks, allocates or calls anything that can exit early
// (thread_exit_backend::os_thread_id calls none).

#include "state/state.hpp"

#include "hosted/stack_bounds_backend.hpp"
#include "hosted/thread_exit_backend.hpp"
#include "stxlib/detail/early_exit.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
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
    // The thread's debugger registry entry, or no_debugger_slot.
    u32 debugger_slot{u32_max};
};

constexpr u32 no_debugger_slot = u32_max;

struct Anchor final {
    ThreadFlags flags{};
    ThreadBlock* block{nullptr};
};

static_assert(std::is_trivially_destructible_v<Anchor>, "rule 2: every thread_local is trivially destructible");
static_assert(sizeof(Anchor) <= 2112, "the anchor (2,072 bytes by state.hpp's arithmetic) is the only TLS stxlib_state adds per thread");

constinit thread_local Anchor anchor{};
constinit ProcessState process_state_instance{};

// ---- the debugger anchor (stxlib/debugger_anchor.hpp) -----------------------------

static_assert(std::endian::native == std::endian::little, "the debugger anchor's layout is little-endian");
static_assert(offsetof(DebuggerThreadSlot, os_thread_id) == offsetof(DebuggerThreadEntry, os_thread_id) &&
                  offsetof(DebuggerThreadSlot, store_address) == offsetof(DebuggerThreadEntry, store_address) &&
                  offsetof(DebuggerThreadSlot, frame_capacity) == offsetof(DebuggerThreadEntry, frame_capacity) &&
                  offsetof(DebuggerThreadSlot, reserved) == offsetof(DebuggerThreadEntry, reserved),
              "the registry entry's fields are where the public layout puts them");
static_assert(sizeof(std::atomic<u64>) == 8 && std::atomic<u64>::is_always_lock_free &&
                  sizeof(std::atomic<u32>) == 4 && std::atomic<u32>::is_always_lock_free,
              "the registry's atomics are plain words a debugger can read");
// Both stores a thread can register share their prefix, so one pair of
// offsets serves both.
static_assert(offsetof(FullShadow, depth) == offsetof(ReserveShadow, depth) &&
                  offsetof(FullShadow, frames) == offsetof(ReserveShadow, frames),
              "the block's store and the reserve agree on depth and frames");
static_assert(sizeof(ShadowFrameRecord) == 120, "the frame stride is GUI.Forms' 120 bytes");

} // namespace

// Outside the unnamed namespace: the descriptor below has external
// linkage, so its type must too.

// The registry's address as the descriptor holds it: a pointer, so that it
// is a constant initialiser (an address cast to an integer is not), widened
// to 8 bytes where pointers are 4.
struct RegistryAddress final {
    const DebuggerThreadSlot* pointer;
#if UINTPTR_MAX == 0xffffffffu
    u32 high{0};
#endif
};

static_assert(sizeof(RegistryAddress) == 8, "the registry address takes 8 bytes");

// DebuggerAnchor's layout, with the registry address as a pointer.
struct AnchorDescriptor final {
    char magic[8];
    u32 struct_size;
    u32 layout_key;
    RegistryAddress registry_address;
    u32 registry_capacity;
    u32 entry_size;
    u32 depth_offset;
    u32 frames_offset;
    u32 frame_stride;
    u32 reserved;
};

static_assert(sizeof(AnchorDescriptor) == sizeof(DebuggerAnchor) &&
                  offsetof(AnchorDescriptor, struct_size) == offsetof(DebuggerAnchor, struct_size) &&
                  offsetof(AnchorDescriptor, layout_key) == offsetof(DebuggerAnchor, layout_key) &&
                  offsetof(AnchorDescriptor, registry_address) == offsetof(DebuggerAnchor, registry_address) &&
                  offsetof(AnchorDescriptor, registry_capacity) == offsetof(DebuggerAnchor, registry_capacity) &&
                  offsetof(AnchorDescriptor, entry_size) == offsetof(DebuggerAnchor, entry_size) &&
                  offsetof(AnchorDescriptor, depth_offset) == offsetof(DebuggerAnchor, depth_offset) &&
                  offsetof(AnchorDescriptor, frames_offset) == offsetof(DebuggerAnchor, frames_offset) &&
                  offsetof(AnchorDescriptor, frame_stride) == offsetof(DebuggerAnchor, frame_stride) &&
                  offsetof(AnchorDescriptor, reserved) == offsetof(DebuggerAnchor, reserved),
              "the descriptor is DebuggerAnchor's layout");

} // namespace stxlib::state

// The descriptor itself, in `.stxshdw`. Writable data, never written: the
// registry address needs a relocation in a shared library, which ELF
// linkers refuse in a read-only section of position-independent code.
// Kept by every linker: `used` (and `retain` on ELF, against
// --gc-sections; Mach-O's no_dead_strip comes with `used`), and on MSVC the
// /INCLUDE below, against /OPT:REF. extern "C", so that it also has one
// plain name for a debugger that has symbols.
#if defined(_MSC_VER)
#pragma section(".stxshdw", read, write)
#define STXLIB_DEBUGGER_ANCHOR_SECTION __declspec(allocate(".stxshdw"))
#if defined(_WIN64)
#pragma comment(linker, "/INCLUDE:stxlib_debugger_anchor")
#else
#pragma comment(linker, "/INCLUDE:_stxlib_debugger_anchor")
#endif
#elif defined(__APPLE__)
#define STXLIB_DEBUGGER_ANCHOR_SECTION __attribute__((used, section("__DATA,.stxshdw")))
#elif defined(__ELF__) && defined(__has_attribute)
#if __has_attribute(retain)
#define STXLIB_DEBUGGER_ANCHOR_SECTION __attribute__((used, retain, section(".stxshdw")))
#else
#define STXLIB_DEBUGGER_ANCHOR_SECTION __attribute__((used, section(".stxshdw")))
#endif
#else
#define STXLIB_DEBUGGER_ANCHOR_SECTION __attribute__((used, section(".stxshdw")))
#endif

extern "C" {
extern stxlib::state::AnchorDescriptor stxlib_debugger_anchor;
STXLIB_DEBUGGER_ANCHOR_SECTION constinit stxlib::state::AnchorDescriptor stxlib_debugger_anchor{
    .magic = {'S', 'T', 'X', 'S', 'H', 'D', 'W', '1'},
    .struct_size = sizeof(stxlib::DebuggerAnchor),
    .layout_key = stxlib::state::layout_key,
    .registry_address = {.pointer = &stxlib::state::process_state_instance.debugger_threads[0]},
    .registry_capacity = stxlib::debugger_thread_capacity,
    .entry_size = sizeof(stxlib::DebuggerThreadEntry),
    .depth_offset = static_cast<stxlib::u32>(offsetof(stxlib::state::FullShadow, depth)),
    .frames_offset = static_cast<stxlib::u32>(offsetof(stxlib::state::FullShadow, frames)),
    .frame_stride = sizeof(stxlib::ShadowFrameRecord),
    .reserved = 0,
};
}

namespace stxlib::state {

namespace {

static_assert(sizeof(debugger_anchor_magic) == sizeof(stxlib_debugger_anchor.magic), "the magic is eight bytes");

// Claims a free registry entry for the calling thread's store; the entry,
// or no_debugger_slot when the host gives no thread id or the registry is
// full (the thread then has none; nothing is allocated). The scan starts at
// the id's own place, so threads spread over the table.
u32 claim_debugger_slot(const void* const store, const u32 frame_capacity) noexcept {
    const u64 thread = thread_exit_backend::os_thread_id();
    if (thread == 0) return no_debugger_slot;
    DebuggerThreadSlot* const slots = process_state_instance.debugger_threads;
    const u32 start = static_cast<u32>(thread % debugger_thread_capacity);
    for (u32 step = 0; step < debugger_thread_capacity; ++step) {
        const u32 index = (start + step) % debugger_thread_capacity;
        DebuggerThreadSlot& slot = slots[index];
        u64 expected = 0;
        if (slot.os_thread_id.load(std::memory_order_relaxed) != 0) continue;
        if (!slot.os_thread_id.compare_exchange_strong(expected, thread, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
            continue;
        }
        const u64 address = static_cast<u64>(reinterpret_cast<std::uintptr_t>(store));
        slot.store_address.store(address, std::memory_order_release);
        slot.frame_capacity.store(frame_capacity, std::memory_order_release);
        return index;
    }
    return no_debugger_slot;
}

// Clears an entry claim_debugger_slot gave, the thread id last.
void clear_debugger_slot(const u32 index) noexcept {
    if (index >= debugger_thread_capacity) return;
    DebuggerThreadSlot& slot = process_state_instance.debugger_threads[index];
    slot.store_address.store(0, std::memory_order_relaxed);
    slot.frame_capacity.store(0, std::memory_order_relaxed);
    slot.os_thread_id.store(0, std::memory_order_release);
}

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

// Records the calling thread's stack bounds (state.hpp, note_thread_stack):
// once, and only while the thread is neither failing fast nor running the
// reporter, since the host's query may allocate and lock.
void note_stack_here() noexcept {
    ThreadFlags& flags = anchor.flags;
    if (flags.stack_high != 0) return;
    if (flags.fail_fast_depth != 0 || flags.in_reporter) return;
    uptr low = 0;
    uptr high = 0;
    if (!stack_bounds_backend::query(low, high)) return;
    flags.stack_low = low;
    flags.stack_high = high;
}

// The thread that loads stxlib_state (a program's initial thread when it is
// linked in, the loading thread when a module is opened later) has its
// bounds recorded then, before any failure can come: such a thread may
// never make a block (a program that only captures a stack). A dynamic
// initialiser of this module; it touches only the anchor, which is
// constant-initialised, so its order against other initialisers does not
// matter. Not on Windows or Emscripten, whose query answers nothing.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
bool note_loading_thread() noexcept {
    note_stack_here();
    return true;
}

[[maybe_unused]] const bool loading_thread_noted = note_loading_thread();
#endif

// The calling thread's block, allocated on first use; null in degraded mode
// (the hook has released it, or its allocation failed once). A thread whose
// records ended before it touched STXlib still gets a block, in phase
// ending.
STXLIB_NO_UNWIND ThreadBlock* block_for_thread(const bool create) noexcept {
    if (anchor.block != nullptr) return anchor.block;
    if (!create || anchor.flags.phase == ThreadPhase::ended || anchor.flags.block_failed) return nullptr;
    if (thread_exit_backend::ended()) {
        // The hook has released this thread's block, and the anchor's
        // storage was made again since (macOS; see end_thread): the thread
        // stays ended.
        anchor.flags.phase = ThreadPhase::ended;
        return nullptr;
    }
    // Before the allocation, so a thread whose block fails still has its
    // bounds for a fail-fast report.
    note_stack_here();
    void* const memory = allocate_bytes(sizeof(ThreadBlock), alignof(ThreadBlock));
    if (memory == nullptr) {
        anchor.flags.block_failed = true;
        // Falls back to the reserve, for the thread's life: registered
        // once, and never cleared, since no hook is armed.
        static_cast<void>(claim_debugger_slot(&anchor.flags.reserve, reserve_frame_capacity));
        return nullptr;
    }
    ThreadBlock* const block = ::new (memory) ThreadBlock{};
    if (!thread_exit_backend::arm(block)) {
        // No hook: a block nothing would release is never made.
        release_bytes(memory);
        anchor.flags.block_failed = true;
        static_cast<void>(claim_debugger_slot(&anchor.flags.reserve, reserve_frame_capacity));
        return nullptr;
    }
    link_block(*block);
    (*block).debugger_slot = claim_debugger_slot(&(*block).state.shadow, FullShadow::frame_capacity);
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
    if (block == nullptr) {
        if (anchor.flags.phase == ThreadPhase::ended) return RecordStatus::disposed;
        return RecordStatus::out_of_memory;
    }
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

void note_thread_stack(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return;
    note_stack_here();
}

namespace detail {

// The host's thread-exit hook (thread_exit_backend.hpp), on the exiting
// thread, with the block it was armed with: end the consumer records, then
// let go of the block (anchor and registry) and release it, ThreadState
// last.
//
// The armed block, not the anchor, says what to release, because on macOS
// the anchor's storage can be gone by now. dyld keeps a module's
// thread_local storage under a pthread key of its own whose destructor is
// free (dyld main, libdyld/ThreadLocalVariables.cpp:
// initializeThunksFromDisk, dyld_thread_key_create(&key, &::free)), created
// when the image is set up and so at a lower index than this module's key,
// which is created on the first block; libpthread runs the key destructors
// in ascending index order (libpthread main, src/pthread_tsd.c:
// _pthread_tsd_cleanup_new; pthread_key_create takes the lowest free
// index). So dyld frees the anchor first, and reading it here makes dyld
// allocate it again, as a fresh thread's (instantiateVariable: malloc, the
// initial content, dyld_thread_setspecific), which dyld's key frees on the
// next destructor round. The anchor is set up again from the armed block:
// the block (its ThreadState, records and registry links) is whole; the
// flags, its ReserveShadow among them, start again as a fresh thread's.
// When the anchor is not the original, end_thread_records may have run
// already: its released slots are null, so releasing them again is a no-op.
// Afterwards thread_exit_backend::ended keeps the thread ended through the
// remaining destructor rounds, whatever the anchor's storage holds then.
void end_thread(void* const armed) noexcept {
    if (armed == nullptr) return;
    ThreadBlock* const block = static_cast<ThreadBlock*>(armed);
    const bool kept = anchor.block == block;
    if (!kept) anchor.block = block;
    if (!kept || anchor.flags.phase == ThreadPhase::live) {
        anchor.flags.phase = ThreadPhase::ending;
        release_records(*block);
    }
    anchor.block = nullptr;
    anchor.flags.phase = ThreadPhase::ended;
    // Before the block's memory goes, so a debugger stops reading it first.
    clear_debugger_slot((*block).debugger_slot);
    (*block).debugger_slot = no_debugger_slot;
    unlink_block(*block);
    (*block).~ThreadBlock();
    release_bytes(block);
}

} // namespace detail

} // namespace stxlib::state
