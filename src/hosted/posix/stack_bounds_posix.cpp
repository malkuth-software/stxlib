// The calling thread's stack on POSIX hosts (hosted/stack_bounds_backend.hpp),
// compiled into stxlib_state. Asked once per thread on a path that is not a
// failure (state.cpp, note_stack_here) and kept in the thread's anchor; the
// frame walk reads the anchor, never this (trace_posix.cpp). STXlib
// sideloads alongside the host C library and owns the answer: one rule per
// kind of thread, so the walk behaves the same whichever C library is
// loaded (notes decisions/2026-11-15-boundary-sideloading.md).
//
// A thread the C library created (every thread but a Linux process's
// initial one): its stack is a block fixed when the thread was created,
// and the C library's own query reports it.
// - glibc 2.41 nptl/pthread_getattr_np.c: the thread's stack block, under
//   the thread's descriptor lock, reallocating a cpuset for the affinity
//   copy on every call. Allowed here; this is why the query never runs at
//   failure time.
// - musl 1.2.5 src/thread/pthread_getattr_np.c: copies the thread's stack
//   and stack_size; no allocation, no lock (git.musl-libc.org, tag v1.2.5).
// - FreeBSD: pthread_attr_get_np (include/pthread_np.h:42 at fb9033d2876d,
//   defined at lib/libthr/thread/thr_attr.c:125) fills an attribute object
//   the caller initialised with pthread_attr_init; it mallocs a cpuset
//   (:137-142) and takes the thread through _thr_find_thread (:146). The
//   main thread's attribute is set once, at libthr's start
//   (lib/libthr/thread/thr_init.c, init_main_thread: stackaddr_attr =
//   _usrstack - _thr_stack_initial, with _thr_stack_initial from
//   AT_USRSTACKLIM or RLIMIT_STACK), so it does not change afterwards.
// - macOS: pthread_get_stackaddr_np and pthread_get_stacksize_np read the
//   thread's own fields, the main thread's size through one getrlimit
//   (apple-oss-distributions/libpthread, src/pthread.c on its main branch as
//   read 2026-11-15, not a pinned tag).
//
// A Linux process's initial thread: its stack is the kernel's growing
// mapping, and the C libraries disagree. glibc 2.41 (nptl/
// pthread_getattr_np.c, the initial-thread branch) reads /proc/self/maps,
// finds the mapping holding __libc_stack_end, and reports a stack reaching
// down RLIMIT_STACK's soft limit from that mapping's end, cut at the end of
// the mapping below it ("last_to"). musl 1.2.5 probes downwards from the
// auxiliary vector's page with mremap while the call fails with ENOMEM, so
// it reports only the part mapped so far: recorded early, that bound would
// be too high once the stack grew. STXlib therefore applies glibc's rule
// itself, on every Linux C library: it reads /proc/self/maps (open, read,
// close into a fixed buffer, no allocation, cancellation held off), takes
// the mapping holding its own frame, and keeps [max(end - soft limit, end
// of the mapping below), end); with no soft limit (RLIM_INFINITY), the end
// of the mapping below. The kernel grows the mapping only while it stays
// within the soft limit, measured from its end (Linux mm/mmap.c,
// acct_stack_growth; not read at a pin), so every frame the thread can
// have lies inside. If /proc is not mounted, the C library's own answer is
// kept (glibc then fails; musl gives the mapped part).
//
// What the walk relies on (trace_posix.cpp): its first record is its own
// frame, and each next record lies strictly above the last and below the
// recorded end, so it reads only between a frame of the running stack and
// that stack's end, which is mapped. The lower bound only rejects a first
// frame that is not on the recorded stack (an alternate signal stack, a
// fiber), where the walk finds nothing and the unwinder is used. On the
// initial thread the lower bound reaches below the mapped part; nothing
// else is mapped there unless a program places a mapping at a fixed or
// hinted address inside the stack's reach (Linux keeps its top-down mmap
// base below the stack's soft-limit reach, mm/util.c mmap_base; not read
// at a pin), and only a frame chain running on such a mapping could be
// admitted. A later setrlimit is not seen: the bounds are the ones
// recorded.

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "hosted/stack_bounds_backend.hpp"

#include <pthread.h>

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#if defined(__linux__)
#include <cerrno>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace stxlib::stack_bounds_backend {

namespace {

#if defined(__linux__)

// The bytes kept of one line of /proc/self/maps. A line begins with its
// range ("from-to ", at most 34 bytes on a 64-bit host); a longer line (a
// long path name) is read for its range and the rest skipped.
constexpr usize maps_room = 512;

// Parses the "from-to " range that begins a line of /proc/self/maps
// (proc(5): lower-case hexadecimal). False when the line does not begin
// with one, or the range is empty.
bool parse_range(const char* const line, const usize count, uptr& from, uptr& to) noexcept {
    uptr values[2]{0, 0};
    usize index = 0;
    for (usize field = 0; field < 2; ++field) {
        usize digits = 0;
        uptr value = 0;
        while (index < count) {
            const char character = line[index];
            u32 digit = 16;
            if (character >= '0' && character <= '9') digit = static_cast<u32>(character - '0');
            if (character >= 'a' && character <= 'f') digit = static_cast<u32>(character - 'a') + 10;
            if (digit == 16) break;
            if (digits == 2 * sizeof(uptr)) return false;
            value = (value << 4) | static_cast<uptr>(digit);
            digits = digits + 1;
            index = index + 1;
        }
        if (digits == 0 || index == count) return false;
        const char separator = field == 0 ? '-' : ' ';
        if (line[index] != separator) return false;
        index = index + 1;
        values[field] = value;
    }
    if (values[0] >= values[1]) return false;
    from = values[0];
    to = values[1];
    return true;
}

// The mapping holding `inside`, and the end of the highest mapping below it
// (0 when none), from /proc/self/maps, which lists mappings in ascending
// order. The caller holds cancellation off.
bool find_mapping(const uptr inside, uptr& from, uptr& to, uptr& below) noexcept {
    const int file = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (file < 0) return false;
    char buffer[maps_room];
    usize held = 0;
    bool ended = false;
    // True while the rest of a line already parsed is being passed over.
    bool skipping = false;
    bool found = false;
    uptr last_end = 0;
    while (!found) {
        if (held < maps_room && !ended) {
            const ssize_t got = ::read(file, buffer + held, maps_room - held);
            if (got < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (got == 0) ended = true;
            held = held + static_cast<usize>(got);
        }
        usize line_end = held;
        for (usize index = 0; index < held; ++index) {
            if (buffer[index] == '\n') {
                line_end = index;
                break;
            }
        }
        const bool whole = line_end < held;
        if (!whole && !ended && held < maps_room) continue;
        if (held == 0) break;
        if (!skipping) {
            uptr line_from = 0;
            uptr line_to = 0;
            if (parse_range(buffer, line_end, line_from, line_to)) {
                if (line_from <= inside && inside < line_to) {
                    from = line_from;
                    to = line_to;
                    below = last_end;
                    found = true;
                } else if (line_to <= inside) {
                    last_end = line_to;
                }
            }
        }
        if (whole) {
            const usize kept = held - (line_end + 1);
            for (usize index = 0; index < kept; ++index) buffer[index] = buffer[line_end + 1 + index];
            held = kept;
            skipping = false;
        } else {
            // A full buffer with no line end (its range is parsed; the rest
            // of the line is passed over), or a last line with no newline.
            held = 0;
            skipping = !ended;
        }
    }
    static_cast<void>(::close(file));
    return found;
}

bool initial_thread_stack(uptr& low, uptr& high) noexcept {
    const uptr inside = reinterpret_cast<uptr>(__builtin_frame_address(0));
    int cancellation = PTHREAD_CANCEL_ENABLE;
    const bool held_off = ::pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancellation) == 0;
    uptr from = 0;
    uptr to = 0;
    uptr below = 0;
    const bool found = find_mapping(inside, from, to, below);
    struct rlimit limit{};
    const bool limited = ::getrlimit(RLIMIT_STACK, &limit) == 0;
    if (held_off) static_cast<void>(::pthread_setcancelstate(cancellation, &cancellation));
    if (!found || !limited) return false;
    uptr reach = below;
    if (limit.rlim_cur != RLIM_INFINITY && static_cast<u64>(limit.rlim_cur) < static_cast<u64>(to - below)) {
        reach = to - static_cast<uptr>(limit.rlim_cur);
    }
    if (reach > from) reach = from;
    low = reach;
    high = to;
    return true;
}

bool is_initial_thread() noexcept {
    const long thread = ::syscall(SYS_gettid);
    const bool result = thread == static_cast<long>(::getpid());
    return result;
}

#endif

#if !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
// The C library's own answer for the calling thread.
bool library_stack(uptr& low, uptr& high) noexcept {
    pthread_attr_t attributes{};
#if defined(__FreeBSD__)
    if (::pthread_attr_init(&attributes) != 0) return false;
    if (::pthread_attr_get_np(::pthread_self(), &attributes) != 0) {
        static_cast<void>(::pthread_attr_destroy(&attributes));
        return false;
    }
#else
    if (::pthread_getattr_np(::pthread_self(), &attributes) != 0) return false;
#endif
    void* address = nullptr;
    size_t size = 0;
    const int got = ::pthread_attr_getstack(&attributes, &address, &size);
    static_cast<void>(::pthread_attr_destroy(&attributes));
    if (got != 0 || address == nullptr || size == 0) return false;
    const uptr start = reinterpret_cast<uptr>(address);
    if (size > ~start) return false;
    low = start;
    high = start + size;
    return true;
}
#endif

} // namespace

bool query(uptr& low, uptr& high) noexcept {
#if defined(__EMSCRIPTEN__)
    // Emscripten's traces are none (trace_emscripten.cpp): nothing walks.
    static_cast<void>(low);
    static_cast<void>(high);
    return false;
#elif defined(__APPLE__)
    const pthread_t self = ::pthread_self();
    const uptr top = reinterpret_cast<uptr>(::pthread_get_stackaddr_np(self));
    const uptr size = static_cast<uptr>(::pthread_get_stacksize_np(self));
    if (top == 0 || size == 0 || size > top) return false;
    low = top - size;
    high = top;
    return true;
#else
#if defined(__linux__)
    if (is_initial_thread() && initial_thread_stack(low, high)) return true;
#endif
    const bool result = library_stack(low, high);
    return result;
#endif
}

} // namespace stxlib::stack_bounds_backend
