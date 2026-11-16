#pragma once

// The display surface (Capability::display_surface; owner decision
// 2026-11-15, "A surface interface for the appliance", in 1.0): one surface
// of pixels, its input as events, and a present call, for the appliance's
// foreground target. GUI.Forms' single-surface window manager drives it
// (GUI.Forms src/host/surface/manager/surface_host.hpp, the SurfaceHost
// contract), so GUI.Forms runs on the appliance's screen. Underneath are
// FreeBSD's stock interfaces (choice 17, kept): the framebuffer device
// fbd(4) (/dev/fb0), the virtual terminal vt(4) (/dev/ttyv0) and evdev(4)
// (/dev/input/event*), which the boot device ruleset opens to the
// foreground target's group only (appliance/C2_DESIGN.md, "Devices").
//
// How a GUI.Forms SurfaceHost maps onto it (cited at GUI.Forms
// claude/cp34-web-host 15013986b, src/host/surface/):
// - start() (manager/surface_host.hpp:85): open_surface(); the extent is
//   width() by height() device pixels at scale 1.
// - acquire_surface(w, h) (:92): pixels() when w and h are the screen's;
//   the screen's size never changes, so another size gets null pixels.
// - SurfaceView (:36-41): SurfacePixels has the same four fields, BGRA8,
//   premultiplied; the screen is opaque, so alpha is ignored.
// - present(damage) (:95): present(); SurfacePresent::presented and
//   inactive are SurfacePresentResult::presented and inactive (:46, :54); a
//   failure OperationStatus is ::failed (:56). Nothing is ever pending (:50): the
//   copy is done when present returns.
// - input() (:87): a thread of the host calls read_events and posts
//   one SurfaceInputRecord (input/surface_input.hpp:94-113) per event:
//   pointer, wheel, key, text and activation map to SurfaceInputKind
//   pointer, wheel, key, text and activation (:33-58), with the values
//   below chosen to be GUI.Forms' own (SurfacePointerAction :72-79,
//   PointerButton and Modifier in input_events.hpp:28-56, the HID usage of
//   physical_key :37).
// - draws_cursor() (:77): false; the manager draws the pointer.
// - set_title, pointer_capture, write_clipboard, place_text_input,
//   toggle_full_screen (:101-115): nothing to do on one full screen.
// - start_dialog (:121): unsupported (no host dialogs on the appliance).
// - want_frames (:98): the host's own timer; the framebuffer reports no
//   display frames.
//
// Threads: read_events on one thread, present on one thread (the
// two may differ: they share no state but an atomic activity flag); a
// Surface is not shared beyond that. One Surface per process: a second
// open fails with HostCode::busy while the first is open.
//
// Failures (ErrorRecord, capabilities.hpp "Failures"):
// - open_surface: unsupported where the host has no framebuffer surface
//   (every family but FreeBSD's) or the framebuffer's pixel format is not
//   one present() converts to (32-bit BGRX, 16-bit RGB565); not_found when
//   /dev/fb0 is absent; permission_denied when the process may not open it
//   (it is not the foreground target); limit_exceeded with HostCode::busy
//   for a second surface; out_of_memory when the shadow surface cannot be
//   mapped; io_failure with the host's code otherwise.
// - present: invalid_argument for a rectangle outside the surface (nothing
//   is copied); disposed when the surface is closed.
// - read_events: disposed when the surface is closed; invalid_argument for
//   no room or a wait below -1. An input device that fails (unplugged) is
//   left out from then on; with none left, read_events only waits.
//
// Nothing here depends on GUI.Forms' window capsule (now its public
// WindowCapsule control): the capsule is drawn and worked inside the
// manager, from the events above.
//
// What a failure leaves behind: open_surface leaves the screen and the
// terminal as they were; close (or the destructor) restores the terminal's
// text mode and keyboard mode before unmapping, and a process that ends
// without closing has its terminal restored by the next owner's open (the
// modes are set, not toggled) or by vt itself when the terminal closes.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

namespace detail {
struct SurfaceState;
}

// The surface's pixels: `height` rows of `width` BGRA8 pixels, `row_bytes`
// apart (at least width * 4), owned by the Surface. They keep what the
// caller wrote until the Surface closes.
struct SurfacePixels final {
    Byte* pixels;
    usize row_bytes;
    u32 width;
    u32 height;
};

// A rectangle of device pixels, x and y from the top left.
struct SurfaceRect final {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
};

enum class SurfacePresent : u8 {
    // The rectangles are on the screen.
    presented = 0,
    // Another surface holds the screen: nothing was shown; compose again
    // after the next activation event. The appliance 1.0 never reports it
    // (its console keyboard is raw, so no terminal switch happens), but a
    // caller handles it as GUI.Forms' manager does.
    inactive = 1,
};

enum class SurfaceEventKind : u8 {
    pointer = 0,
    wheel = 1,
    key = 2,
    text = 3,
    activation = 4,
};

// GUI.Forms' SurfacePointerAction values (move 0, down 1, up 2).
enum class SurfacePointerAction : u8 {
    move = 0,
    down = 1,
    up = 2,
};

// GUI.Forms' PointerButton order.
enum class SurfaceButton : u8 {
    none = 0,
    primary = 1,
    secondary = 2,
    middle = 3,
};

// Modifier bits, as gui_forms::Modifier.
inline constexpr u8 surface_modifier_shift = 1U;
inline constexpr u8 surface_modifier_control = 2U;
inline constexpr u8 surface_modifier_alt = 4U;
inline constexpr u8 surface_modifier_meta = 8U;

// One input event. Fields a kind does not use are zero.
// - pointer: action, button (down and up), x and y (device pixels, within
//   the surface), modifiers.
// - wheel: wheel_x and wheel_y in lines, positive up and left (GUI.Forms'
//   sign), x and y where the pointer is, modifiers.
// - key: down, repeat, physical_key (USB HID usage, page 0x07; 0 for a key
//   without one), scan_code (the evdev code), modifiers after the event.
// - text: text_length (1) bytes of UTF-8 in text: the US layout's character
//   for the key pressed just before it.
// - activation: down 1 when the surface holds the screen again (compose
//   everything), 0 when it lost it.
// time_ms is the interval clock (clocks.hpp) in milliseconds when the
// event was read.
struct SurfaceEvent final {
    SurfaceEventKind kind;
    SurfacePointerAction action;
    SurfaceButton button;
    u8 modifiers;
    u8 down;
    u8 repeat;
    u8 text_length;
    Byte text[4];
    u32 physical_key;
    u32 scan_code;
    i32 x;
    i32 y;
    i32 wheel_x;
    i32 wheel_y;
    u64 time_ms;
};

// The open surface. Move-only; closing (or destroying) it gives the
// screen back to the terminal.
class Surface final {
public:
    Surface(Surface&& other) noexcept;
    Surface& operator=(Surface&& other) noexcept;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface();

    [[nodiscard]] u32 width() const noexcept;
    [[nodiscard]] u32 height() const noexcept;
    // Null pixels after close.
    [[nodiscard]] SurfacePixels pixels() const noexcept;
    // Shows `count` rectangles of the pixels (each within the surface;
    // count 0 shows nothing).
    [[nodiscard]] OperationResult<SurfacePresent> present(const SurfaceRect* rects, usize count) noexcept;
    // Reads input into `events` (at most `capacity`), waiting up to
    // `wait_ms` milliseconds for the first (0: no wait; -1: no limit).
    // Returns how many were written; 0 when the wait passed.
    [[nodiscard]] OperationResult<usize> read_events(SurfaceEvent* events, usize capacity, i64 wait_ms) noexcept;
    // Gives the screen back; idempotent.
    void close() noexcept;

private:
    friend OperationResult<Surface> open_surface() noexcept;
    explicit Surface(detail::SurfaceState* state) noexcept;
    detail::SurfaceState* state_;
};

[[nodiscard]] OperationResult<Surface> open_surface() noexcept;

} // namespace stxlib
