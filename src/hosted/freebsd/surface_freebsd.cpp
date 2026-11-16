// The display surface on FreeBSD (surface.hpp): the framebuffer fbd(4)
// (/dev/fb0), the first virtual terminal vt(4) (/dev/ttyv0) and the
// evdev(4) input devices (/dev/input/event0..15), FreeBSD's stock
// interfaces (choice 17; the surface interface sits over them, owner
// 2026-11-15). On the appliance the boot device ruleset shows these to the
// foreground target's group only (appliance/C2_DESIGN.md, "Devices").
//
// - The framebuffer: FBIOGTYPE gives width, height, depth and size
//   (sys/fbio.h); FBIO_GETLINEWIDTH the stride in bytes; mmap maps it
//   shared. The caller draws into a shadow surface (anonymous memory, BGRA8)
//   and present() copies damaged rectangles, converting
//   (core/surface_blit.hpp).
// - The terminal: KD_GRAPHICS stops vt drawing text over the surface
//   (KDSETMODE, sys/consio.h), and the keyboard goes raw (KDSKBMODE K_RAW,
//   sys/kbio.h), so vt neither types into the terminal nor switches
//   terminals: the foreground target alone holds the screen (owner answer
//   15). What vt still queues on the terminal is read and dropped. close()
//   puts back KD_TEXT and the saved keyboard mode.
// - Input: every event device that opens; its absolute X and Y ranges from
//   EVIOCGABS. core/surface_input.hpp turns the events into SurfaceEvents.
//   A device whose read fails other than with EAGAIN or EINTR (unplugged)
//   is closed and left out from then on.
//
// One surface per process (core/surface.cpp claims it), so the state is
// one static object: nothing is allocated but the shadow mapping.
//
// Early exits (decision 2026-11-15): every foreign call here is a kernel
// interface. open() records each resource in the state only after the call
// that acquired it succeeded, and on any failure releases what it holds in
// reverse order (undo_open), so an early exit at any call leaves either
// nothing acquired or the state naming exactly what is held; the kernel
// releases descriptors and mappings at process end and vt restores a
// terminal's modes when its last descriptor closes. present() and
// read_events() change only the shadow, the screen and the tracker, each
// whole after every call.

#include "core/host_failure.hpp"
#include "core/surface_blit.hpp"
#include "core/surface_input.hpp"
#include "hosted/surface_backend.hpp"
#include "stxlib/clocks.hpp"

#include <atomic>
#include <cerrno>
#include <climits>

#include <dev/evdev/input.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/consio.h>
#include <sys/fbio.h>
#include <sys/ioctl.h>
#include <sys/kbio.h>
#include <sys/mman.h>
#include <unistd.h>

namespace stxlib::detail {

struct SurfaceState final {
    int framebuffer{-1};
    int terminal{-1};
    int saved_keyboard_mode{K_XLATE};
    bool terminal_changed{false};
    int inputs[surface_input::devices_max]{};
    surface_input::AxisRange x_ranges[surface_input::devices_max]{};
    surface_input::AxisRange y_ranges[surface_input::devices_max]{};
    Byte* screen{nullptr};
    usize screen_bytes{0U};
    surface_blit::FramebufferView view{};
    Byte* shadow{nullptr};
    usize shadow_bytes{0U};
    u32 width{0U};
    u32 height{0U};
    std::atomic<bool> active{false};
    surface_input::Tracker tracker{1U, 1U};
    // Events a read produced beyond the caller's capacity, given out first
    // on the next read.
    SurfaceEvent backlog[64]{};
    usize backlog_count{0U};
    usize backlog_next{0U};
};

} // namespace stxlib::detail

namespace stxlib::surface_backend {

namespace {

using detail::SurfaceState;

constexpr u32 side_max = 16384U;
constexpr usize reads_per_device_max = 16U;
constexpr usize events_per_read = 64U;

SurfaceState surface_state;

[[nodiscard]] ErrorRecord errno_record(const Operation operation, const int code) noexcept {
    ErrorKind kind = ErrorKind::io_failure;
    if (code == ENOENT || code == ENXIO) kind = ErrorKind::not_found;
    if (code == EACCES || code == EPERM) kind = ErrorKind::permission_denied;
    if (code == ENOMEM) kind = ErrorKind::out_of_memory;
    const ErrorRecord record =
        host_failure::record(kind, operation, HostCode::none, ForeignCode::of(ForeignFamily::posix_errno, code));
    return record;
}

void close_inputs(SurfaceState& state) noexcept {
    for (u32 index = 0U; index < surface_input::devices_max; ++index) {
        if (state.inputs[index] >= 0) (void)::close(state.inputs[index]);
        state.inputs[index] = -1;
    }
}

// Releases everything the state holds, in reverse order of acquisition.
void undo_open(SurfaceState& state) noexcept {
    state.active.store(false, std::memory_order_release);
    close_inputs(state);
    if (state.terminal >= 0) {
        if (state.terminal_changed) {
            (void)ioctl(state.terminal, KDSKBMODE, state.saved_keyboard_mode);
            (void)ioctl(state.terminal, KDSETMODE, KD_TEXT);
            state.terminal_changed = false;
        }
        (void)::close(state.terminal);
        state.terminal = -1;
    }
    if (state.shadow != nullptr) {
        (void)munmap(state.shadow, state.shadow_bytes);
        state.shadow = nullptr;
        state.shadow_bytes = 0U;
    }
    if (state.screen != nullptr) {
        (void)munmap(state.screen, state.screen_bytes);
        state.screen = nullptr;
        state.screen_bytes = 0U;
    }
    if (state.framebuffer >= 0) {
        (void)::close(state.framebuffer);
        state.framebuffer = -1;
    }
    state.backlog_count = 0U;
    state.backlog_next = 0U;
}

// "/dev/input/event<index>" into path (index below 100).
void event_path(const u32 index, char (&path)[24]) noexcept {
    const char prefix[] = "/dev/input/event";
    usize at = 0U;
    for (; prefix[at] != '\0'; ++at) path[at] = prefix[at];
    if (index >= 10U) {
        path[at] = static_cast<char>('0' + static_cast<char>(index / 10U));
        at += 1U;
    }
    path[at] = static_cast<char>('0' + static_cast<char>(index % 10U));
    path[at + 1U] = '\0';
}

[[nodiscard]] surface_input::AxisRange axis_range(const int device, const unsigned int axis) noexcept {
    struct input_absinfo info{};
    surface_input::AxisRange range{0, 0};
    if (ioctl(device, EVIOCGABS(axis), &info) == 0) {
        range.minimum = info.minimum;
        range.maximum = info.maximum;
    }
    return range;
}

void open_inputs(SurfaceState& state) noexcept {
    for (u32 index = 0U; index < surface_input::devices_max; ++index) {
        char path[24] = {};
        event_path(index, path);
        const int device = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (device < 0) continue;
        state.inputs[index] = device;
        state.x_ranges[index] = axis_range(device, ABS_X);
        state.y_ranges[index] = axis_range(device, ABS_Y);
    }
}

// Puts one produced event where it goes: the caller's array while it has
// room, then the backlog; a full backlog drops it.
void deliver(SurfaceState& state, const SurfaceEvent& event, SurfaceEvent* const events, const usize capacity,
             usize& written) noexcept {
    if (written < capacity) {
        events[written] = event;
        written += 1U;
        return;
    }
    const usize backlog_size = sizeof(state.backlog) / sizeof(state.backlog[0]);
    if (state.backlog_count < backlog_size) {
        state.backlog[state.backlog_count] = event;
        state.backlog_count += 1U;
    }
}

void read_device(SurfaceState& state, const u32 index, const u64 time_ms, SurfaceEvent* const events,
                 const usize capacity, usize& written) noexcept {
    for (usize round = 0U; round < reads_per_device_max; ++round) {
        struct input_event raw[events_per_read];
        const ssize_t got = ::read(state.inputs[index], raw, sizeof(raw));
        if (got < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                (void)::close(state.inputs[index]);
                state.inputs[index] = -1;
            }
            return;
        }
        const usize count = static_cast<usize>(got) / sizeof(raw[0]);
        for (usize item = 0U; item < count; ++item) {
            const surface_input::RawEvent event{raw[item].type, raw[item].code, raw[item].value};
            SurfaceEvent produced[surface_input::events_per_input_max];
            const usize made = state.tracker.feed(index, event, state.x_ranges[index], state.y_ranges[index],
                                                  time_ms, produced);
            for (usize output = 0U; output < made; ++output) deliver(state, produced[output], events, capacity, written);
        }
        if (count < events_per_read) return;
    }
}

void drain_terminal(const SurfaceState& state) noexcept {
    for (usize round = 0U; round < reads_per_device_max; ++round) {
        char discard[256];
        const ssize_t got = ::read(state.terminal, discard, sizeof(discard));
        if (got <= 0 || static_cast<usize>(got) < sizeof(discard)) return;
    }
}

} // namespace

OperationResult<detail::SurfaceState*> open() noexcept {
    SurfaceState& state = surface_state;
    for (u32 index = 0U; index < surface_input::devices_max; ++index) state.inputs[index] = -1;
    const int framebuffer = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (framebuffer < 0) return OperationResult<SurfaceState*>::failure(errno_record("stxlib.surface.open", errno));
    state.framebuffer = framebuffer;
    struct fbtype type{};
    if (ioctl(framebuffer, FBIOGTYPE, &type) != 0) {
        const int code = errno;
        undo_open(state);
        return OperationResult<SurfaceState*>::failure(errno_record("stxlib.surface.open", code));
    }
    surface_blit::FramebufferFormat format = surface_blit::FramebufferFormat::bgrx8888;
    usize pixel_bytes = 4U;
    if (type.fb_depth == 16) {
        format = surface_blit::FramebufferFormat::rgb565;
        pixel_bytes = 2U;
    } else if (type.fb_depth != 32) {
        undo_open(state);
        return OperationResult<SurfaceState*>::failure(
            host_failure::record(ErrorKind::unsupported, "stxlib.surface.open", HostCode::invalid_value));
    }
    unsigned int stride = 0U;
    if (ioctl(framebuffer, FBIO_GETLINEWIDTH, &stride) != 0) {
        const int code = errno;
        undo_open(state);
        return OperationResult<SurfaceState*>::failure(errno_record("stxlib.surface.open", code));
    }
    if (type.fb_width <= 0 || type.fb_height <= 0 || static_cast<u32>(type.fb_width) > side_max ||
        static_cast<u32>(type.fb_height) > side_max || static_cast<usize>(stride) < static_cast<usize>(type.fb_width) * pixel_bytes) {
        undo_open(state);
        return OperationResult<SurfaceState*>::failure(
            host_failure::record(ErrorKind::unsupported, "stxlib.surface.open", HostCode::invalid_value));
    }
    const u32 width = static_cast<u32>(type.fb_width);
    const u32 height = static_cast<u32>(type.fb_height);
    const usize screen_bytes = static_cast<usize>(stride) * height;
    void* const screen = mmap(nullptr, screen_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, framebuffer, 0);
    if (screen == MAP_FAILED) {
        const int code = errno;
        undo_open(state);
        return OperationResult<SurfaceState*>::failure(errno_record("stxlib.surface.open", code));
    }
    state.screen = static_cast<Byte*>(screen);
    state.screen_bytes = screen_bytes;
    const usize shadow_bytes = static_cast<usize>(width) * 4U * height;
    void* const shadow = mmap(nullptr, shadow_bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (shadow == MAP_FAILED) {
        undo_open(state);
        return OperationResult<SurfaceState*>::failure(
            host_failure::record(ErrorKind::out_of_memory, "stxlib.surface.open", HostCode::none));
    }
    state.shadow = static_cast<Byte*>(shadow);
    state.shadow_bytes = shadow_bytes;
    const int terminal = ::open("/dev/ttyv0", O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (terminal >= 0) {
        state.terminal = terminal;
        int keyboard_mode = K_XLATE;
        if (ioctl(terminal, KDGKBMODE, &keyboard_mode) != 0 || ioctl(terminal, KDSETMODE, KD_GRAPHICS) != 0) {
            const int code = errno;
            undo_open(state);
            return OperationResult<SurfaceState*>::failure(errno_record("stxlib.surface.open", code));
        }
        state.saved_keyboard_mode = keyboard_mode;
        state.terminal_changed = true;
        if (ioctl(terminal, KDSKBMODE, K_RAW) != 0) {
            const int code = errno;
            undo_open(state);
            return OperationResult<SurfaceState*>::failure(errno_record("stxlib.surface.open", code));
        }
    }
    open_inputs(state);
    const surface_blit::FramebufferView view{state.screen, static_cast<usize>(stride), width, height, format};
    state.view = view;
    state.width = width;
    state.height = height;
    state.tracker = surface_input::Tracker(width, height);
    state.backlog_count = 0U;
    state.backlog_next = 0U;
    state.active.store(true, std::memory_order_release);
    return OperationResult<SurfaceState*>::success(&state);
}

void close(detail::SurfaceState* const state) noexcept {
    if (state != nullptr) undo_open(*state);
}

u32 width(const detail::SurfaceState* const state) noexcept { return (*state).width; }

u32 height(const detail::SurfaceState* const state) noexcept { return (*state).height; }

SurfacePixels pixels(const detail::SurfaceState* const state) noexcept {
    const SurfacePixels result{(*state).shadow, static_cast<usize>((*state).width) * 4U, (*state).width,
                               (*state).height};
    return result;
}

OperationResult<SurfacePresent> present(detail::SurfaceState* const state, const SurfaceRect* const rects,
                                        const usize count) noexcept {
    if (!(*state).active.load(std::memory_order_acquire)) {
        return OperationResult<SurfacePresent>::success(SurfacePresent::inactive);
    }
    const SurfacePixels from = pixels(state);
    for (usize index = 0U; index < count; ++index) surface_blit::copy_rect(from, (*state).view, rects[index]);
    return OperationResult<SurfacePresent>::success(SurfacePresent::presented);
}

OperationResult<usize> read_events(detail::SurfaceState* const state, SurfaceEvent* const events, const usize capacity,
                                   const i64 wait_ms) noexcept {
    SurfaceState& surface = *state;
    usize written = 0U;
    while (surface.backlog_next < surface.backlog_count && written < capacity) {
        events[written] = surface.backlog[surface.backlog_next];
        surface.backlog_next += 1U;
        written += 1U;
    }
    if (surface.backlog_next == surface.backlog_count) {
        surface.backlog_count = 0U;
        surface.backlog_next = 0U;
    }
    if (written != 0U) return OperationResult<usize>::success(written);
    struct pollfd watched[surface_input::devices_max + 1U]{};
    u32 owners[surface_input::devices_max + 1U]{};
    nfds_t watched_count = 0U;
    for (u32 index = 0U; index < surface_input::devices_max; ++index) {
        if (surface.inputs[index] < 0) continue;
        watched[watched_count].fd = surface.inputs[index];
        watched[watched_count].events = POLLIN;
        owners[watched_count] = index;
        watched_count += 1U;
    }
    if (surface.terminal >= 0) {
        watched[watched_count].fd = surface.terminal;
        watched[watched_count].events = POLLIN;
        owners[watched_count] = surface_input::devices_max;
        watched_count += 1U;
    }
    const int timeout = wait_ms < 0 ? -1 : (wait_ms > INT_MAX ? INT_MAX : static_cast<int>(wait_ms));
    const int ready = poll(watched, watched_count, timeout);
    if (ready <= 0) return OperationResult<usize>::success(0U);
    const u64 time_ms = static_cast<u64>(IntervalTime::now().count_nanoseconds() / 1000000);
    for (nfds_t item = 0U; item < watched_count; ++item) {
        if ((watched[item].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
        if (owners[item] == surface_input::devices_max) {
            drain_terminal(surface);
        } else {
            read_device(surface, owners[item], time_ms, events, capacity, written);
        }
    }
    return OperationResult<usize>::success(written);
}

} // namespace stxlib::surface_backend
