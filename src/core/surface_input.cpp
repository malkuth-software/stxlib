#include "core/surface_input.hpp"

#include "core/surface_keys.hpp"

namespace stxlib::surface_input {

namespace {

constexpr i64 delta_limit = 2147483647;

[[nodiscard]] i64 add_clamped(const i64 sum, const i32 value) noexcept {
    const i64 total = sum + static_cast<i64>(value);
    if (total > delta_limit) return delta_limit;
    if (total < -delta_limit) return -delta_limit;
    return total;
}

[[nodiscard]] i32 clamp_to(const i64 value, const u32 pixels) noexcept {
    const i64 last = static_cast<i64>(pixels) - 1;
    if (value < 0) return 0;
    if (value > last) return static_cast<i32>(last);
    return static_cast<i32>(value);
}

[[nodiscard]] SurfaceEvent blank_event(const SurfaceEventKind kind, const u64 time_ms) noexcept {
    SurfaceEvent event{};
    event.kind = kind;
    event.time_ms = time_ms;
    return event;
}

// Bit index of a button code, or 8 for none.
[[nodiscard]] u32 button_index(const u16 code) noexcept {
    switch (code) {
    case btn_left:
    case btn_touch:
        return 0U;
    case btn_right:
        return 1U;
    case btn_middle:
        return 2U;
    default:
        return 8U;
    }
}

[[nodiscard]] SurfaceButton button_of_index(const u32 index) noexcept {
    switch (index) {
    case 0U:
        return SurfaceButton::primary;
    case 1U:
        return SurfaceButton::secondary;
    case 2U:
        return SurfaceButton::middle;
    default:
        return SurfaceButton::none;
    }
}

} // namespace

i32 map_absolute(const i32 value, const AxisRange range, const u32 pixels) noexcept {
    const i64 span = static_cast<i64>(range.maximum) - static_cast<i64>(range.minimum);
    const i64 offset = static_cast<i64>(value) - static_cast<i64>(range.minimum);
    if (offset <= 0) return 0;
    if (offset >= span) return static_cast<i32>(pixels - 1U);
    // 0 < offset < span < 2^32 and pixels - 1 < 2^32: the product is below
    // 2^64 in unsigned arithmetic, and the quotient below pixels - 1.
    const u64 scaled = (static_cast<u64>(offset) * static_cast<u64>(pixels - 1U)) / static_cast<u64>(span);
    return static_cast<i32>(scaled);
}

Tracker::Tracker(const u32 width, const u32 height) noexcept
    : width_(width == 0U ? 1U : width), height_(height == 0U ? 1U : height), x_(0), y_(0), buttons_(0U),
      held_modifiers_(0U), caps_lock_(false), num_lock_(true), pending_{} {
    x_ = static_cast<i32>(width_ / 2U);
    y_ = static_cast<i32>(height_ / 2U);
}

u8 Tracker::modifiers() const noexcept {
    u8 result = 0U;
    for (u32 bit = 0U; bit < 8U; ++bit) {
        if ((held_modifiers_ & static_cast<u8>(1U << bit)) != 0U) {
            result = static_cast<u8>(result | surface_keys::modifier_of_usage(0xE0U + bit));
        }
    }
    return result;
}

void Tracker::clear(const u32 device) noexcept {
    const Pending empty{};
    pending_[device] = empty;
}

usize Tracker::key(const RawEvent& event, const u64 time_ms, SurfaceEvent* const out) noexcept {
    if (event.value < 0 || event.value > 2) return 0U;
    const bool down = event.value != 0;
    const bool repeat = event.value == 2;
    const u32 usage = surface_keys::usage_of_code(event.code);
    if (usage >= 0xE0U && usage <= 0xE7U) {
        const u8 bit = static_cast<u8>(1U << (usage - 0xE0U));
        held_modifiers_ = down ? static_cast<u8>(held_modifiers_ | bit) : static_cast<u8>(held_modifiers_ & ~bit);
    }
    if (down && !repeat && usage == surface_keys::usage_caps_lock) caps_lock_ = !caps_lock_;
    if (down && !repeat && usage == surface_keys::usage_num_lock) num_lock_ = !num_lock_;
    SurfaceEvent pressed = blank_event(SurfaceEventKind::key, time_ms);
    pressed.down = down ? 1U : 0U;
    pressed.repeat = repeat ? 1U : 0U;
    pressed.physical_key = usage;
    pressed.scan_code = event.code;
    pressed.modifiers = modifiers();
    out[0] = pressed;
    if (!down) return 1U;
    const char typed = surface_keys::text_of_usage(usage, modifiers(), caps_lock_, num_lock_);
    if (typed == 0) return 1U;
    SurfaceEvent text = blank_event(SurfaceEventKind::text, time_ms);
    text.text_length = 1U;
    text.text[0] = static_cast<Byte>(typed);
    text.modifiers = modifiers();
    out[1] = text;
    return 2U;
}

usize Tracker::report(const u32 device, const AxisRange x_range, const AxisRange y_range, const u64 time_ms,
                      SurfaceEvent* const out) noexcept {
    const Pending frame = pending_[device];
    clear(device);
    usize count = 0U;
    i32 new_x = clamp_to(static_cast<i64>(x_) + frame.dx, width_);
    i32 new_y = clamp_to(static_cast<i64>(y_) + frame.dy, height_);
    if (frame.has_abs_x && x_range.maximum > x_range.minimum) new_x = map_absolute(frame.abs_x, x_range, width_);
    if (frame.has_abs_y && y_range.maximum > y_range.minimum) new_y = map_absolute(frame.abs_y, y_range, height_);
    const u8 held = modifiers();
    if (new_x != x_ || new_y != y_) {
        x_ = new_x;
        y_ = new_y;
        SurfaceEvent moved = blank_event(SurfaceEventKind::pointer, time_ms);
        moved.action = SurfacePointerAction::move;
        moved.x = x_;
        moved.y = y_;
        moved.modifiers = held;
        out[count] = moved;
        count += 1U;
    }
    for (u32 index = 0U; index < 3U; ++index) {
        const u8 bit = static_cast<u8>(1U << index);
        if ((frame.button_seen & bit) == 0U) continue;
        const bool pressed = (frame.button_state & bit) != 0U;
        const bool was = (buttons_ & bit) != 0U;
        if (pressed == was) continue;
        buttons_ = pressed ? static_cast<u8>(buttons_ | bit) : static_cast<u8>(buttons_ & ~bit);
        SurfaceEvent button = blank_event(SurfaceEventKind::pointer, time_ms);
        button.action = pressed ? SurfacePointerAction::down : SurfacePointerAction::up;
        button.button = button_of_index(index);
        button.x = x_;
        button.y = y_;
        button.modifiers = held;
        out[count] = button;
        count += 1U;
    }
    if (frame.wheel != 0 || frame.hwheel != 0) {
        SurfaceEvent wheel = blank_event(SurfaceEventKind::wheel, time_ms);
        // evdev: REL_WHEEL positive away from the user (up), REL_HWHEEL
        // positive right; GUI.Forms: positive up and left.
        wheel.wheel_y = static_cast<i32>(frame.wheel);
        wheel.wheel_x = static_cast<i32>(-frame.hwheel);
        wheel.x = x_;
        wheel.y = y_;
        wheel.modifiers = held;
        out[count] = wheel;
        count += 1U;
    }
    return count;
}

usize Tracker::feed(const u32 device, const RawEvent& event, const AxisRange x_range, const AxisRange y_range,
                    const u64 time_ms, SurfaceEvent* const out) noexcept {
    if (device >= devices_max) return 0U;
    Pending& frame = pending_[device];
    switch (event.type) {
    case ev_syn:
        if (event.code == syn_report) return report(device, x_range, y_range, time_ms, out);
        if (event.code == syn_dropped) clear(device);
        return 0U;
    case ev_key: {
        if (event.code < surface_keys::evdev_code_count) return key(event, time_ms, out);
        const u32 index = button_index(event.code);
        if (index < 3U && event.value >= 0 && event.value <= 1) {
            const u8 bit = static_cast<u8>(1U << index);
            frame.button_seen = static_cast<u8>(frame.button_seen | bit);
            frame.button_state = event.value == 1 ? static_cast<u8>(frame.button_state | bit)
                                                  : static_cast<u8>(frame.button_state & ~bit);
        }
        return 0U;
    }
    case ev_rel:
        switch (event.code) {
        case rel_x:
            frame.dx = add_clamped(frame.dx, event.value);
            break;
        case rel_y:
            frame.dy = add_clamped(frame.dy, event.value);
            break;
        case rel_wheel:
            frame.wheel = add_clamped(frame.wheel, event.value);
            break;
        case rel_hwheel:
            frame.hwheel = add_clamped(frame.hwheel, event.value);
            break;
        default:
            break;
        }
        return 0U;
    case ev_abs:
        if (event.code == abs_x) {
            frame.abs_x = event.value;
            frame.has_abs_x = true;
        } else if (event.code == abs_y) {
            frame.abs_y = event.value;
            frame.has_abs_y = true;
        }
        return 0U;
    default:
        return 0U;
    }
}

} // namespace stxlib::surface_input
