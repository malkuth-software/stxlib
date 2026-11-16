#pragma once

// The display surface's keys (surface.hpp; owner decision 2026-11-15, the
// appliance surface interface): evdev key codes, as FreeBSD's evdev(4)
// reports them (the Linux numbering, sys/dev/evdev/input-event-codes.h),
// turned into what GUI.Forms' single-surface host takes:
// - a USB HID usage on the Keyboard/Keypad page 0x07, the value GUI.Forms'
//   SurfaceInputRecord::physical_key holds (GUI.Forms claude/cp34-web-host
//   15013986b, src/host/surface/input/surface_input.hpp:37, "a USB HID
//   usage"; its web host's table, src/host/web/input/web_input_rules.cpp:15-17,
//   uses the bare usage, 0x04 for A);
// - the modifier bits of gui_forms::Modifier (input_events.hpp:50-56:
//   shift 1, control 2, alt 4, meta 8, at the same commit);
// - the committed text of a press on the US layout (the appliance's only
//   layout in 1.0), or none.
//
// Pure code: no system call, no allocation. The usage table is the inverse
// of FreeBSD's own evdev_usb_scancodes[] (sys/dev/evdev/evdev_utils.c at the
// pin), restricted to the keys a keyboard reports; tools/check_surface_keys.py
// compares the two on the M4, entry by entry, over all 256 usages, and
// tests/surface_tests.cpp checks the table's own properties over all 65536
// codes.

#include "stxlib/types.hpp"

namespace stxlib::surface_keys {

// The evdev codes the tables cover: every code below this. Higher codes
// (buttons, multimedia keys beyond the page) have no usage and no text.
inline constexpr u32 evdev_code_count = 256;

// The page 0x07 usage of `code`, or 0 when the key has none.
[[nodiscard]] u32 usage_of_code(u32 code) noexcept;

// The gui_forms::Modifier bit a held key contributes (usages 0xE0..0xE7),
// or 0.
inline constexpr u8 modifier_shift = 1U;
inline constexpr u8 modifier_control = 2U;
inline constexpr u8 modifier_alt = 4U;
inline constexpr u8 modifier_meta = 8U;
[[nodiscard]] u8 modifier_of_usage(u32 usage) noexcept;

// The ASCII text a press of `usage` commits on the US layout, or 0: letters
// (Caps Lock flips their case), digits and punctuation (with Shift), space,
// and the keypad's digits and point when Num Lock is on, its operators
// always. No text while Control, Alt or Meta is held, and none for keys
// that act rather than type (Enter, Tab, Backspace, Escape, Delete, the
// arrows): GUI.Forms takes those as keys.
[[nodiscard]] char text_of_usage(u32 usage, u8 modifiers, bool caps_lock, bool num_lock) noexcept;

// Usages of the lock keys, which the tracker toggles on each press.
inline constexpr u32 usage_caps_lock = 0x39U;
inline constexpr u32 usage_num_lock = 0x53U;

} // namespace stxlib::surface_keys
