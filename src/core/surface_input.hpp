#pragma once

// The display surface's input (surface.hpp): evdev(4) events, as FreeBSD
// reports them from /dev/input/event* (struct input_event's type, code and
// value, in the Linux numbering of sys/dev/evdev/input-event-codes.h),
// turned into SurfaceEvents.
//
// Pure code: no system call, no allocation, fixed state. The FreeBSD
// backend (src/hosted/freebsd/surface_freebsd.cpp) reads the devices and
// feeds each event here with its device's absolute-axis ranges
// (EVIOCGABS). Tested on every host (tests/surface_tests.cpp), the state
// properties over every sequence of up to six events of a fixed alphabet.
//
// The rules:
// - Keys (codes below 256) are reported at once: a key event (the usage of
//   surface_keys.hpp, the evdev code as scan_code, value 1 down, 2 repeat,
//   0 up), then, for a press or repeat that types, a text event. Held
//   modifier keys make the modifier bits; Caps Lock and Num Lock toggle on
//   each press (Num Lock starts on).
// - Pointer motion, buttons and wheels collect per device until that
//   device's SYN_REPORT, then come out in this order: one move (when the
//   position changed), one down or up per button whose state changed
//   (primary, secondary, middle), one wheel (when it turned). Relative
//   motion moves the pointer by device units = pixels; absolute axes (a
//   tablet, a touchscreen, QEMU's usb-tablet) map their range onto the
//   surface. The pointer stays within the surface; it starts at the centre.
// - SYN_DROPPED discards what the device collected since its last report
//   (evdev's rule: the events between are lost); keys already reported
//   stand.
// - BTN_LEFT, BTN_RIGHT, BTN_MIDDLE are the three buttons; BTN_TOUCH is the
//   primary button. Other buttons and event types are ignored.

#include "stxlib/surface.hpp"
#include "stxlib/types.hpp"

namespace stxlib::surface_input {

inline constexpr u32 devices_max = 16;
// The most events one fed event produces (a report: move, three buttons,
// wheel).
inline constexpr usize events_per_input_max = 5;

// evdev's numbering (input-event-codes.h).
inline constexpr u16 ev_syn = 0x00U;
inline constexpr u16 ev_key = 0x01U;
inline constexpr u16 ev_rel = 0x02U;
inline constexpr u16 ev_abs = 0x03U;
inline constexpr u16 syn_report = 0U;
inline constexpr u16 syn_dropped = 3U;
inline constexpr u16 rel_x = 0x00U;
inline constexpr u16 rel_y = 0x01U;
inline constexpr u16 rel_hwheel = 0x06U;
inline constexpr u16 rel_wheel = 0x08U;
inline constexpr u16 abs_x = 0x00U;
inline constexpr u16 abs_y = 0x01U;
inline constexpr u16 btn_left = 0x110U;
inline constexpr u16 btn_right = 0x111U;
inline constexpr u16 btn_middle = 0x112U;
inline constexpr u16 btn_touch = 0x14AU;

struct RawEvent final {
    u16 type;
    u16 code;
    i32 value;
};

// A device's absolute axis: used when maximum > minimum.
struct AxisRange final {
    i32 minimum;
    i32 maximum;
};

class Tracker final {
public:
    // A surface of width by height pixels, both at least 1.
    Tracker(u32 width, u32 height) noexcept;

    // Feeds one event of `device` (below devices_max; others are ignored)
    // and writes the events it completes to out (room for
    // events_per_input_max). Returns how many.
    usize feed(u32 device, const RawEvent& event, AxisRange x_range, AxisRange y_range, u64 time_ms,
               SurfaceEvent* out) noexcept;

    [[nodiscard]] i32 x() const noexcept { return x_; }
    [[nodiscard]] i32 y() const noexcept { return y_; }
    [[nodiscard]] u8 modifiers() const noexcept;
    // Bit 0 primary, 1 secondary, 2 middle.
    [[nodiscard]] u8 buttons() const noexcept { return buttons_; }

private:
    struct Pending final {
        i64 dx;
        i64 dy;
        i32 abs_x;
        i32 abs_y;
        bool has_abs_x;
        bool has_abs_y;
        i64 wheel;
        i64 hwheel;
        // Bit b set: button b was reported in this frame, with state in
        // button_state's bit b.
        u8 button_seen;
        u8 button_state;
    };

    usize key(const RawEvent& event, u64 time_ms, SurfaceEvent* out) noexcept;
    usize report(u32 device, AxisRange x_range, AxisRange y_range, u64 time_ms, SurfaceEvent* out) noexcept;
    void clear(u32 device) noexcept;

    u32 width_;
    u32 height_;
    i32 x_;
    i32 y_;
    u8 buttons_;
    // Bit (usage - 0xE0) set while that modifier key is held.
    u8 held_modifiers_;
    bool caps_lock_;
    bool num_lock_;
    Pending pending_[devices_max];
};

// Where absolute value `value` of an axis falls on a surface side of
// `pixels` (at least 1): minimum..maximum maps linearly onto 0..pixels-1,
// rounding down; values outside the range clamp. Requires maximum > minimum.
[[nodiscard]] i32 map_absolute(i32 value, AxisRange range, u32 pixels) noexcept;

} // namespace stxlib::surface_input
