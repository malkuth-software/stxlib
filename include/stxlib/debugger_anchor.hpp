#pragma once

// The debugger anchor: where a debugger finds every thread's logical stack
// (shadow.hpp) without symbols, in a stripped release build too. Asked for
// by GUI.Forms' Windows side, whose Visual Studio Concord filter shows the
// logical frames in C# stack traces; this header is the layout that reader
// mirrors. Nothing here is called: it describes memory.
//
// The descriptor. The module that holds STXlib's state (stxlib_state: the
// shared library, or the program that links it statically) publishes one
// DebuggerAnchor, constant-initialised, in a section named `.stxshdw`. The
// name is the same on PE, ELF and Mach-O (on Mach-O in the __DATA segment:
// "__DATA,.stxshdw"); on PE it is exactly the eight bytes a section name
// may hold. A reader finds the section from the module's base: on PE and
// Mach-O through the headers mapped at the base, on ELF through the
// section headers in the module's file.
//
// The registry. A fixed array of registry_capacity DebuggerThreadEntry, in
// the process state, one entry per live thread that has registered:
// - a thread writes its entry once, when its state block is made or when
//   it falls back to the failure reserve (its block's allocation or
//   thread-exit hook failed), and clears it in the thread-exit hook; a
//   thread on the reserve has no hook, so its entry stays until the
//   process ends;
// - claiming and clearing are lock-free (one compare-exchange on the
//   thread id, then plain atomic stores) and never allocate; pushing and
//   popping frames do not touch the registry;
// - a full registry stops registering: a thread that finds no free entry
//   has none, and nothing is allocated.
//
// Readers trust nothing. Every field may be stale or torn when the process
// is stopped at an arbitrary point: check the magic, the sizes and the
// layout key; bound the depth by frame_capacity; read every address with a
// read that may fail; match os_thread_id with the debugger's own list of
// live threads. An entry is published in this order: the thread id (by
// compare-exchange from 0), then store_address and frame_capacity; it is
// cleared in the reverse order, the thread id last.
//
// Reading one thread's stack: take the entry whose os_thread_id is the
// thread's (Windows: GetCurrentThreadId's value; Linux: gettid; macOS:
// pthread_threadid_np; FreeBSD: pthread_getthreadid_np); read the u32 at
// store_address + depth_offset; the frames stored are
// min(depth, frame_capacity), frame i at store_address + frames_offset +
// i * frame_stride, oldest first, each a ShadowFrameRecord (shadow.hpp; 120
// bytes). Deeper frames are counted in depth and not stored.

#include "stxlib/types.hpp"

namespace stxlib {

// The section's name, and its first eight bytes' value.
inline constexpr char debugger_anchor_section_name[] = ".stxshdw";
inline constexpr char debugger_anchor_magic[8] = {'S', 'T', 'X', 'S', 'H', 'D', 'W', '1'};

// The registry's capacity: 1,024 threads, 24 KiB.
inline constexpr u32 debugger_thread_capacity = 1024;

// The descriptor in `.stxshdw` (48 bytes; a reader accepts struct_size of
// 48 or more and reads only the fields it knows). Little-endian on every
// host STXlib supports.
struct DebuggerAnchor final {
    char magic[8];         //  0: debugger_anchor_magic, "STXSHDW1", not terminated
    u32 struct_size;       //  8: sizeof(DebuggerAnchor), at least 48
    u32 layout_key;        // 12: the state's layout key (layout version and build type)
    u64 registry_address;  // 16: the first DebuggerThreadEntry
    u32 registry_capacity; // 24: entries in the registry
    u32 entry_size;        // 28: bytes per entry, at least 24
    u32 depth_offset;      // 32: the u32 depth in a shadow store
    u32 frames_offset;     // 36: frames[0] in a shadow store
    u32 frame_stride;      // 40: bytes per frame (120)
    u32 reserved;          // 44: zero
};

static_assert(sizeof(DebuggerAnchor) == 48, "the descriptor's layout is fixed");

// One registry entry (24 bytes).
struct DebuggerThreadEntry final {
    u64 os_thread_id;   //  0: the host's id of the thread; 0 when the entry is free
    u64 store_address;  //  8: the thread's shadow store (its block's, or the failure reserve)
    u32 frame_capacity; // 16: frames the store holds: 64 (a block) or 8 (the reserve)
    u32 reserved;       // 20: zero
};

static_assert(sizeof(DebuggerThreadEntry) == 24, "the entry's layout is fixed");

} // namespace stxlib
