// Tests of the debugger anchor (include/stxlib/debugger_anchor.hpp): the
// registry is read back the way a debugger reads it, from the `.stxshdw`
// section of the module that holds STXlib's state, found from that module's
// base without symbols. PE and Mach-O: the section headers mapped at the
// base. ELF: the section headers in the module's file, placed by the
// module's load bias. Only the module's address is taken from STXlib
// (stxlib_state's exported process_state, to name the module); every value
// checked is read through the descriptor. Tested, not proved: each check is
// one schedule the host chose. Not built for Emscripten, whose modules have
// no sections a debugger reads.

#include "stxlib/debugger_anchor.hpp"
#include "stxlib/shadow.hpp"

#include "state/state.hpp"

#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#else
#include <dlfcn.h>
#include <elf.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace {

using stxlib::DebuggerAnchor;
using stxlib::DebuggerThreadEntry;
using stxlib::ShadowFrameRecord;
using stxlib::u32;
using stxlib::u64;

// An address inside the module that holds the state.
const void* state_module_address() {
    const void* const result = reinterpret_cast<const void*>(&stxlib::state::process_state);
    return result;
}

#if defined(_WIN32)

// The section from the PE headers mapped at the module's base.
const unsigned char* find_section(std::size_t& size) {
    HMODULE module = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (::GetModuleHandleExW(flags, static_cast<LPCWSTR>(state_module_address()), &module) == 0) return nullptr;
    const unsigned char* const base = reinterpret_cast<const unsigned char*>(module);
    u32 header_offset = 0;
    std::memcpy(&header_offset, base + 0x3c, sizeof(header_offset));
    const unsigned char* const pe = base + header_offset;
    if (std::memcmp(pe, "PE\0\0", 4) != 0) return nullptr;
    unsigned short section_count = 0;
    unsigned short optional_size = 0;
    std::memcpy(&section_count, pe + 6, sizeof(section_count));
    std::memcpy(&optional_size, pe + 20, sizeof(optional_size));
    const unsigned char* const sections = pe + 24 + optional_size;
    for (unsigned short index = 0; index < section_count; ++index) {
        const unsigned char* const section = sections + static_cast<std::size_t>(index) * 40;
        if (std::memcmp(section, ".stxshdw", 8) != 0) continue;
        u32 virtual_size = 0;
        u32 virtual_address = 0;
        std::memcpy(&virtual_size, section + 8, sizeof(virtual_size));
        std::memcpy(&virtual_address, section + 12, sizeof(virtual_address));
        size = virtual_size;
        return base + virtual_address;
    }
    return nullptr;
}

u64 own_thread_id() { return static_cast<u64>(::GetCurrentThreadId()); }

#elif defined(__APPLE__)

// The section from the Mach-O load commands mapped at the module's base.
const unsigned char* find_section(std::size_t& size) {
    Dl_info info{};
    if (::dladdr(state_module_address(), &info) == 0 || info.dli_fbase == nullptr) return nullptr;
    const mach_header_64* const header = static_cast<const mach_header_64*>(info.dli_fbase);
    unsigned long found_size = 0;
    const std::uint8_t* const data = ::getsectiondata(header, "__DATA", ".stxshdw", &found_size);
    size = static_cast<std::size_t>(found_size);
    return data;
}

// Not compared on macOS: the test would have to call pthread_threadid_np
// itself (house rule: no pthread outside the host layer).
u64 own_thread_id() { return 0; }

#else

// The section from the module's file: its section headers name it, and its
// address is the section's address plus the load bias, the base less the
// lowest loadable segment's address rounded down to that segment's
// alignment.
const unsigned char* find_section(std::size_t& size) {
    Dl_info info{};
    if (::dladdr(state_module_address(), &info) == 0 || info.dli_fname == nullptr) return nullptr;
    const char* path = info.dli_fname;
#if defined(__linux__)
    // The program's own name may be relative or bare.
    if (std::strchr(path, '/') == nullptr) path = "/proc/self/exe";
#endif
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr) return nullptr;
    const unsigned char* result = nullptr;
    Elf64_Ehdr header{};
    bool good = std::fread(&header, sizeof(header), 1, file) == 1 &&
                std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 && header.e_ident[EI_CLASS] == ELFCLASS64 &&
                header.e_shentsize == sizeof(Elf64_Shdr) && header.e_phentsize == sizeof(Elf64_Phdr);
    std::uintptr_t lowest = UINTPTR_MAX;
    for (unsigned index = 0; good && index < header.e_phnum; ++index) {
        Elf64_Phdr segment{};
        good = std::fseek(file, static_cast<long>(header.e_phoff + index * sizeof(segment)), SEEK_SET) == 0 &&
               std::fread(&segment, sizeof(segment), 1, file) == 1;
        if (!good || segment.p_type != PT_LOAD) continue;
        const std::uintptr_t align = segment.p_align == 0 ? 1 : static_cast<std::uintptr_t>(segment.p_align);
        const std::uintptr_t start = static_cast<std::uintptr_t>(segment.p_vaddr) & ~(align - 1);
        if (start < lowest) lowest = start;
    }
    Elf64_Shdr names{};
    good = good && lowest != UINTPTR_MAX && header.e_shstrndx < header.e_shnum &&
           std::fseek(file, static_cast<long>(header.e_shoff + header.e_shstrndx * sizeof(names)), SEEK_SET) == 0 &&
           std::fread(&names, sizeof(names), 1, file) == 1;
    for (unsigned index = 0; good && index < header.e_shnum; ++index) {
        Elf64_Shdr section{};
        good = std::fseek(file, static_cast<long>(header.e_shoff + index * sizeof(section)), SEEK_SET) == 0 &&
               std::fread(&section, sizeof(section), 1, file) == 1;
        if (!good) break;
        char name[16]{};
        if (std::fseek(file, static_cast<long>(names.sh_offset + section.sh_name), SEEK_SET) != 0) continue;
        if (std::fread(name, 1, sizeof(name) - 1, file) == 0) continue;
        if (std::strcmp(name, ".stxshdw") != 0) continue;
        const std::uintptr_t bias = reinterpret_cast<std::uintptr_t>(info.dli_fbase) - lowest;
        result = reinterpret_cast<const unsigned char*>(bias + static_cast<std::uintptr_t>(section.sh_addr));
        size = static_cast<std::size_t>(section.sh_size);
        break;
    }
    std::fclose(file);
    return result;
}

#if defined(__linux__)
u64 own_thread_id() {
    const long thread = ::syscall(SYS_gettid);
    const u64 result = thread > 0 ? static_cast<u64>(thread) : 0;
    return result;
}
#else
// FreeBSD: not compared (pthread_getthreadid_np is host-layer only).
u64 own_thread_id() { return 0; }
#endif

#endif

// The descriptor, copied out of the section and checked as a reader would.
bool read_anchor(DebuggerAnchor& anchor) {
    std::size_t size = 0;
    const unsigned char* const section = find_section(size);
    CHECK(section != nullptr);
    if (section == nullptr) return false;
    CHECK(size >= sizeof(DebuggerAnchor));
    if (size < sizeof(DebuggerAnchor)) return false;
    std::memcpy(&anchor, section, sizeof(anchor));
    CHECK(std::memcmp(anchor.magic, stxlib::debugger_anchor_magic, sizeof(anchor.magic)) == 0);
    CHECK(anchor.struct_size >= 48);
    CHECK(anchor.layout_key == stxlib::state::layout_key);
    CHECK(anchor.registry_address != 0);
    CHECK(anchor.registry_capacity == stxlib::debugger_thread_capacity);
    CHECK(anchor.entry_size >= 24);
    CHECK(anchor.frame_stride == sizeof(ShadowFrameRecord));
    CHECK(anchor.reserved == 0);
    // The registry the descriptor names is the process state's.
    const stxlib::state::ProcessState& process = stxlib::state::current_process();
    CHECK(anchor.registry_address == static_cast<u64>(reinterpret_cast<std::uintptr_t>(&process.debugger_threads[0])));
    return true;
}

// The entry whose store is `store`, read through the descriptor; false when
// none is.
bool find_entry(const DebuggerAnchor& anchor, const void* const store, DebuggerThreadEntry& found) {
    const u64 wanted = static_cast<u64>(reinterpret_cast<std::uintptr_t>(store));
    const unsigned char* const registry =
        reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(anchor.registry_address));
    for (u32 index = 0; index < anchor.registry_capacity; ++index) {
        DebuggerThreadEntry entry{};
        std::memcpy(&entry, registry + static_cast<std::size_t>(index) * anchor.entry_size, sizeof(entry));
        if (entry.os_thread_id != 0 && entry.store_address == wanted) {
            found = entry;
            return true;
        }
    }
    return false;
}

// Whether any entry names `thread`.
bool thread_listed(const DebuggerAnchor& anchor, const u64 thread) {
    const unsigned char* const registry =
        reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(anchor.registry_address));
    for (u32 index = 0; index < anchor.registry_capacity; ++index) {
        DebuggerThreadEntry entry{};
        std::memcpy(&entry, registry + static_cast<std::size_t>(index) * anchor.entry_size, sizeof(entry));
        if (entry.os_thread_id == thread) return true;
    }
    return false;
}

// The calling thread's store, as STXlib holds it (only to find its entry).
const void* own_store() {
    stxlib::state::ThreadState* const thread = stxlib::state::current_thread_state();
    if (thread == nullptr) return nullptr;
    const void* const result = &(*thread).shadow;
    return result;
}

// Reads the stack in `entry` through the descriptor's offsets and checks
// its depth and its innermost frame's subject.
void check_stack(const DebuggerAnchor& anchor, const DebuggerThreadEntry& entry, const u32 depth,
                 const char* const innermost_subject) {
    const unsigned char* const store =
        reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(entry.store_address));
    u32 read_depth = 0;
    std::memcpy(&read_depth, store + anchor.depth_offset, sizeof(read_depth));
    CHECK(read_depth == depth);
    CHECK(entry.frame_capacity == stxlib::shadow_frame_capacity);
    if (read_depth == 0 || read_depth > entry.frame_capacity) return;
    ShadowFrameRecord frame{};
    const std::size_t at = anchor.frames_offset + static_cast<std::size_t>(read_depth - 1) * anchor.frame_stride;
    std::memcpy(&frame, store + at, sizeof(frame));
    const std::size_t length = std::strlen(innermost_subject);
    CHECK(frame.kind == 2);
    CHECK(frame.subject_count == length && std::memcmp(frame.subject, innermost_subject, length) == 0);
}

void test_own_thread(const DebuggerAnchor& anchor) {
    const stxlib::ShadowFrameText outer{.kind = 2, .subject = "form.main", .name = "outer", .first = 1};
    const stxlib::ShadowFrameMark outer_mark = stxlib::enter_shadow_frame(outer);
    const void* const store = own_store();
    CHECK(store != nullptr);
    DebuggerThreadEntry entry{};
    const bool listed = store != nullptr && find_entry(anchor, store, entry);
    CHECK(listed);
    if (listed) {
        const u64 expected = own_thread_id();
        CHECK(expected == 0 || entry.os_thread_id == expected);
        check_stack(anchor, entry, 1, "form.main");
        const stxlib::ShadowFrameText inner{.kind = 2, .subject = "button.ok", .name = "click", .first = 2};
        const stxlib::ShadowFrameMark inner_mark = stxlib::enter_shadow_frame(inner);
        check_stack(anchor, entry, 2, "button.ok");
        stxlib::leave_shadow_frame(inner_mark, true);
        check_stack(anchor, entry, 1, "form.main");
    }
    stxlib::leave_shadow_frame(outer_mark, true);
}

// What the worker thread and the test share.
struct Shared final {
    const DebuggerAnchor* anchor{nullptr};
    std::atomic<int> step{0};
    std::atomic<bool> listed{false};
    std::atomic<u64> thread_id{0};
    DebuggerThreadEntry seen{};
};

// Holds two frames, finds its own entry, and waits for the test to read it.
void worker_body(Shared* const shared) {
    const stxlib::ShadowFrameText first{.kind = 2, .subject = "grid.view", .name = "paint"};
    const stxlib::ShadowFrameMark first_mark = stxlib::enter_shadow_frame(first);
    const stxlib::ShadowFrameText second{.kind = 2, .subject = "grid.cell", .name = "draw"};
    const stxlib::ShadowFrameMark second_mark = stxlib::enter_shadow_frame(second);
    const void* const store = own_store();
    DebuggerThreadEntry entry{};
    if (store != nullptr && find_entry(*(*shared).anchor, store, entry)) {
        (*shared).seen = entry;
        (*shared).thread_id.store(entry.os_thread_id);
        (*shared).listed.store(true);
    }
    (*shared).step.store(1);
    while ((*shared).step.load() != 2) std::this_thread::yield();
    stxlib::leave_shadow_frame(second_mark, true);
    stxlib::leave_shadow_frame(first_mark, true);
}

// Another thread registers, is read while it holds two frames, and its
// entry is gone once its exit hook has run (join returns after it).
void test_other_thread(const DebuggerAnchor& anchor) {
    Shared shared{};
    shared.anchor = &anchor;
    std::thread worker(worker_body, &shared);
    while (shared.step.load() != 1) std::this_thread::yield();
    CHECK(shared.listed.load());
    // Read from this thread while the worker stands still, as a debugger
    // reads a stopped thread.
    if (shared.listed.load()) check_stack(anchor, shared.seen, 2, "grid.cell");
    shared.step.store(2);
    worker.join();
    CHECK(shared.thread_id.load() != 0);
    CHECK(!thread_listed(anchor, shared.thread_id.load()));
}

} // namespace

int main() {
    DebuggerAnchor anchor{};
    if (read_anchor(anchor)) {
        test_own_thread(anchor);
        test_other_thread(anchor);
    }
    return stxlib_tests::finish("debugger_anchor_tests");
}
