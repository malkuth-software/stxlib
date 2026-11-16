// Tests of the display surface's pure parts (surface.hpp; core/surface_keys,
// core/surface_input, core/surface_blit) on every host, and of
// open_surface where the family has no surface. Bounds, each exhaustive:
// - keys: all 65536 evdev codes; all 256 usages for the modifier bits;
//   all 256 usages x 16 modifier sets x Caps Lock x Num Lock for the text;
// - the tracker: every sequence of 1 to 6 events over a 12-event alphabet
//   (12^6 = 2,985,984 sequences of six, and all shorter ones as prefixes)
//   on a 5 by 3 surface, against a model of position, buttons and
//   modifiers;
// - map_absolute: every range with minimum in -3..3 and span 1..6, every
//   value from minimum - 2 to maximum + 2, every side of 1..7 pixels,
//   against floor((v - min) * (p - 1) / span) with clamping; and the
//   extreme range INT_MIN..INT_MAX at sides 1, 2 and 16384;
// - blit: rect_within over every x, y in 0..6 and width, height in 0..6 on
//   a 5 by 4 surface, plus the values whose sums overflow 32 bits; every
//   rectangle within it copied in both formats, every pixel inside checked
//   converted and every pixel outside untouched; rgb565_of over all 2^24
//   colours.
// The FreeBSD backend's system calls are checked by the appliance's VM
// scenario and the board's drawing test target, not here.

#include "core/surface_blit.hpp"
#include "core/surface_input.hpp"
#include "core/surface_keys.hpp"
#include "stxlib/capabilities.hpp"
#include "stxlib/surface.hpp"
#include "test_support.hpp"

#include <climits>
#include <cstring>

namespace {

using stxlib::Byte;
using stxlib::i32;
using stxlib::i64;
using stxlib::u8;
using stxlib::u16;
using stxlib::u32;
using stxlib::u64;
using stxlib::usize;

namespace keys = stxlib::surface_keys;
namespace input = stxlib::surface_input;
namespace blit = stxlib::surface_blit;

// ---- keys -------------------------------------------------------------------

void keys_table() {
    bool seen[256] = {};
    u32 mapped = 0;
    bool injective = true;
    bool in_page = true;
    for (u32 code = 0; code < 65536U; ++code) {
        const u32 usage = keys::usage_of_code(code);
        if (code >= keys::evdev_code_count) {
            CHECK(usage == 0U);
            if (usage != 0U) break;
            continue;
        }
        if (usage == 0U) continue;
        mapped += 1U;
        if (usage > 0xE7U) in_page = false;
        if (usage < 256U) {
            if (seen[usage]) injective = false;
            seen[usage] = true;
        }
    }
    CHECK(mapped == 132U);
    CHECK(injective);
    CHECK(in_page);
    // The letters from the keyboard's rows (evdev numbers the keys by
    // position: Q..P 16..25, A..L 30..38, Z..M 44..50); usage = 4 + letter.
    const char* const rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const u32 starts[3] = {16U, 30U, 44U};
    for (u32 row = 0; row < 3U; ++row) {
        for (u32 column = 0; rows[row][column] != '\0'; ++column) {
            const u32 letter = static_cast<u32>(rows[row][column] - 'a');
            CHECK(keys::usage_of_code(starts[row] + column) == 0x04U + letter);
        }
    }
    // The digit row 2..11 is "1234567890", usages 0x1E..0x27 in that order.
    for (u32 index = 0; index < 10U; ++index) CHECK(keys::usage_of_code(2U + index) == 0x1EU + index);
    // F1..F10 59..68, F11 87, F12 88, F13..F24 183..194.
    for (u32 index = 0; index < 10U; ++index) CHECK(keys::usage_of_code(59U + index) == 0x3AU + index);
    CHECK(keys::usage_of_code(87U) == 0x44U);
    CHECK(keys::usage_of_code(88U) == 0x45U);
    for (u32 index = 0; index < 12U; ++index) CHECK(keys::usage_of_code(183U + index) == 0x68U + index);
    // The modifiers: LEFTCTRL 29, LEFTSHIFT 42, LEFTALT 56, LEFTMETA 125,
    // RIGHTCTRL 97, RIGHTSHIFT 54, RIGHTALT 100, RIGHTMETA 126.
    CHECK(keys::usage_of_code(29U) == 0xE0U);
    CHECK(keys::usage_of_code(42U) == 0xE1U);
    CHECK(keys::usage_of_code(56U) == 0xE2U);
    CHECK(keys::usage_of_code(125U) == 0xE3U);
    CHECK(keys::usage_of_code(97U) == 0xE4U);
    CHECK(keys::usage_of_code(54U) == 0xE5U);
    CHECK(keys::usage_of_code(100U) == 0xE6U);
    CHECK(keys::usage_of_code(126U) == 0xE7U);
    CHECK(keys::usage_of_code(1U) == 0x29U);  // ESC
    CHECK(keys::usage_of_code(28U) == 0x28U); // ENTER
    CHECK(keys::usage_of_code(57U) == 0x2CU); // SPACE
}

void keys_modifiers() {
    for (u32 usage = 0; usage < 256U; ++usage) {
        u8 expected = 0U;
        if (usage == 0xE0U || usage == 0xE4U) expected = keys::modifier_control;
        if (usage == 0xE1U || usage == 0xE5U) expected = keys::modifier_shift;
        if (usage == 0xE2U || usage == 0xE6U) expected = keys::modifier_alt;
        if (usage == 0xE3U || usage == 0xE7U) expected = keys::modifier_meta;
        CHECK(keys::modifier_of_usage(usage) == expected);
    }
}

// The US layout written out by key, independently of the implementation's
// tables: for each usage, the unshifted and shifted characters.
char reference_text(const u32 usage, const u8 modifiers, const bool caps, const bool num) {
    if ((modifiers & 0x0EU) != 0U) return 0;
    const bool shift = (modifiers & 1U) != 0U;
    if (usage >= 0x04U && usage <= 0x1DU) {
        const char* const lower = "abcdefghijklmnopqrstuvwxyz";
        const char* const upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        return (shift != caps) ? upper[usage - 0x04U] : lower[usage - 0x04U];
    }
    struct Pair final {
        u32 usage;
        char plain;
        char shifted;
    };
    const Pair pairs[] = {
        {0x1EU, '1', '!'}, {0x1FU, '2', '@'}, {0x20U, '3', '#'}, {0x21U, '4', '$'}, {0x22U, '5', '%'},
        {0x23U, '6', '^'}, {0x24U, '7', '&'}, {0x25U, '8', '*'}, {0x26U, '9', '('}, {0x27U, '0', ')'},
        {0x2CU, ' ', ' '}, {0x2DU, '-', '_'}, {0x2EU, '=', '+'}, {0x2FU, '[', '{'}, {0x30U, ']', '}'},
        {0x31U, '\\', '|'}, {0x33U, ';', ':'}, {0x34U, '\'', '"'}, {0x35U, '`', '~'}, {0x36U, ',', '<'},
        {0x37U, '.', '>'}, {0x38U, '/', '?'}, {0x64U, '\\', '|'}, {0x54U, '/', '/'}, {0x55U, '*', '*'},
        {0x56U, '-', '-'}, {0x57U, '+', '+'}, {0x67U, '=', '='},
    };
    for (const Pair& pair : pairs) {
        if (pair.usage == usage) return shift ? pair.shifted : pair.plain;
    }
    if (num) {
        const char* const keypad = "1234567890.";
        if (usage >= 0x59U && usage <= 0x63U) return keypad[usage - 0x59U];
    }
    return 0;
}

void keys_text() {
    u32 mismatches = 0;
    for (u32 usage = 0; usage < 256U; ++usage) {
        for (u32 modifiers = 0; modifiers < 16U; ++modifiers) {
            for (u32 locks = 0; locks < 4U; ++locks) {
                const bool caps = (locks & 1U) != 0U;
                const bool num = (locks & 2U) != 0U;
                const char got = keys::text_of_usage(usage, static_cast<u8>(modifiers), caps, num);
                if (got != reference_text(usage, static_cast<u8>(modifiers), caps, num)) mismatches += 1U;
            }
        }
    }
    CHECK(mismatches == 0U);
}

// ---- tracker ----------------------------------------------------------------

constexpr u32 surface_width = 5U;
constexpr u32 surface_height = 3U;
constexpr input::AxisRange x_range{0, 100};
constexpr input::AxisRange y_range{0, 0}; // no absolute Y: relative only

const input::RawEvent alphabet[12] = {
    {input::ev_rel, input::rel_x, 1},      {input::ev_rel, input::rel_x, -7},
    {input::ev_rel, input::rel_y, 2},      {input::ev_abs, input::abs_x, 100},
    {input::ev_key, input::btn_left, 1},   {input::ev_key, input::btn_left, 0},
    {input::ev_key, input::btn_right, 1},  {input::ev_rel, input::rel_wheel, 1},
    {input::ev_syn, input::syn_report, 0}, {input::ev_syn, input::syn_dropped, 0},
    {input::ev_key, 42U, 1},               {input::ev_key, 30U, 1},
};

struct Model final {
    i64 x;
    i64 y;
    i64 dx;
    i64 dy;
    bool has_abs;
    i32 abs_x;
    i64 wheel;
    u8 buttons;
    u8 seen;
    u8 state;
    bool shift;
};

i64 clamp_model(const i64 value, const u32 pixels) {
    if (value < 0) return 0;
    if (value > static_cast<i64>(pixels) - 1) return static_cast<i64>(pixels) - 1;
    return value;
}

// Feeds one alphabet event to both; returns false on any disagreement.
bool step(input::Tracker& tracker, Model& model, const u32 letter) {
    stxlib::SurfaceEvent out[input::events_per_input_max] = {};
    const input::RawEvent& event = alphabet[letter];
    const usize made = tracker.feed(0U, event, x_range, y_range, 7U, out);
    if (made > input::events_per_input_max) return false;
    if (event.type == input::ev_rel && event.code == input::rel_x) model.dx += event.value;
    if (event.type == input::ev_rel && event.code == input::rel_y) model.dy += event.value;
    if (event.type == input::ev_rel && event.code == input::rel_wheel) model.wheel += event.value;
    if (event.type == input::ev_abs) {
        model.has_abs = true;
        model.abs_x = event.value;
    }
    if (event.type == input::ev_key && event.code >= 256U) {
        const u8 bit = event.code == input::btn_left ? 1U : 2U;
        model.seen = static_cast<u8>(model.seen | bit);
        model.state = event.value == 1 ? static_cast<u8>(model.state | bit) : static_cast<u8>(model.state & ~bit);
    }
    if (event.type == input::ev_syn && event.code == input::syn_dropped) {
        model.dx = 0;
        model.dy = 0;
        model.has_abs = false;
        model.wheel = 0;
        model.seen = 0U;
        model.state = 0U;
        return made == 0U;
    }
    if (event.type == input::ev_key && event.code < 256U) {
        if (event.code == 42U) model.shift = true;
        const u8 modifiers = model.shift ? 1U : 0U;
        if (made < 1U || out[0].kind != stxlib::SurfaceEventKind::key || out[0].modifiers != modifiers) return false;
        if (event.code == 30U) {
            if (made != 2U || out[1].kind != stxlib::SurfaceEventKind::text) return false;
            return out[1].text[0] == static_cast<Byte>(model.shift ? 'A' : 'a');
        }
        return made == 1U;
    }
    if (!(event.type == input::ev_syn && event.code == input::syn_report)) return made == 0U;
    // The report: the model's expected events, in order.
    i64 new_x = clamp_model(model.x + model.dx, surface_width);
    const i64 new_y = clamp_model(model.y + model.dy, surface_height);
    if (model.has_abs) new_x = input::map_absolute(model.abs_x, x_range, surface_width);
    usize at = 0U;
    if (new_x != model.x || new_y != model.y) {
        if (at >= made || out[at].kind != stxlib::SurfaceEventKind::pointer ||
            out[at].action != stxlib::SurfacePointerAction::move || out[at].x != new_x || out[at].y != new_y) {
            return false;
        }
        at += 1U;
    }
    model.x = new_x;
    model.y = new_y;
    for (u32 index = 0; index < 2U; ++index) {
        const u8 bit = static_cast<u8>(1U << index);
        if ((model.seen & bit) == 0U) continue;
        const bool pressed = (model.state & bit) != 0U;
        if (pressed == ((model.buttons & bit) != 0U)) continue;
        model.buttons = pressed ? static_cast<u8>(model.buttons | bit) : static_cast<u8>(model.buttons & ~bit);
        const stxlib::SurfaceButton button = index == 0U ? stxlib::SurfaceButton::primary : stxlib::SurfaceButton::secondary;
        if (at >= made || out[at].button != button ||
            out[at].action != (pressed ? stxlib::SurfacePointerAction::down : stxlib::SurfacePointerAction::up) ||
            out[at].x != model.x || out[at].y != model.y) {
            return false;
        }
        at += 1U;
    }
    if (model.wheel != 0) {
        if (at >= made || out[at].kind != stxlib::SurfaceEventKind::wheel || out[at].wheel_y != model.wheel) return false;
        at += 1U;
    }
    model.dx = 0;
    model.dy = 0;
    model.has_abs = false;
    model.wheel = 0;
    model.seen = 0U;
    model.state = 0U;
    return at == made && tracker.x() == model.x && tracker.y() == model.y && tracker.buttons() == model.buttons;
}

void tracker_sequences() {
    constexpr u32 letters = 12U;
    constexpr u32 length = 6U;
    u32 total = 1U;
    for (u32 index = 0; index < length; ++index) total *= letters;
    u32 failures = 0;
    for (u32 sequence = 0; sequence < total; ++sequence) {
        input::Tracker tracker(surface_width, surface_height);
        Model model{static_cast<i64>(surface_width / 2U), static_cast<i64>(surface_height / 2U), 0, 0, false, 0, 0,
                    0U, 0U, 0U, false};
        u32 rest = sequence;
        for (u32 position = 0; position < length; ++position) {
            const u32 letter = rest % letters;
            rest /= letters;
            if (!step(tracker, model, letter)) {
                failures += 1U;
                break;
            }
            if (tracker.x() < 0 || tracker.x() >= static_cast<i32>(surface_width) || tracker.y() < 0 ||
                tracker.y() >= static_cast<i32>(surface_height)) {
                failures += 1U;
                break;
            }
        }
    }
    CHECK(failures == 0U);
}

void tracker_other_devices() {
    input::Tracker tracker(4U, 4U);
    stxlib::SurfaceEvent out[input::events_per_input_max] = {};
    const input::RawEvent motion{input::ev_rel, input::rel_x, 1};
    const input::RawEvent report{input::ev_syn, input::syn_report, 0};
    CHECK(tracker.feed(input::devices_max, motion, x_range, y_range, 0U, out) == 0U);
    CHECK(tracker.feed(1U, motion, x_range, y_range, 0U, out) == 0U);
    // Device 1's motion is not device 0's report.
    CHECK(tracker.feed(0U, report, x_range, y_range, 0U, out) == 0U);
    CHECK(tracker.feed(1U, report, x_range, y_range, 0U, out) == 1U);
    CHECK(out[0].x == 3 && out[0].y == 2);
}

void absolute_mapping() {
    u32 failures = 0;
    for (i32 minimum = -3; minimum <= 3; ++minimum) {
        for (i32 span = 1; span <= 6; ++span) {
            const input::AxisRange range{minimum, minimum + span};
            for (u32 pixels = 1U; pixels <= 7U; ++pixels) {
                i32 last = 0;
                for (i32 value = minimum - 2; value <= minimum + span + 2; ++value) {
                    i64 offset = static_cast<i64>(value) - minimum;
                    if (offset < 0) offset = 0;
                    if (offset > span) offset = span;
                    const i64 expected = offset * (static_cast<i64>(pixels) - 1) / span;
                    const i32 got = input::map_absolute(value, range, pixels);
                    if (got != expected || got < last) failures += 1U;
                    last = got;
                }
            }
        }
    }
    CHECK(failures == 0U);
    const input::AxisRange widest{INT_MIN, INT_MAX};
    CHECK(input::map_absolute(INT_MIN, widest, 16384U) == 0);
    CHECK(input::map_absolute(INT_MAX, widest, 16384U) == 16383);
    CHECK(input::map_absolute(0, widest, 16384U) == 8191);
    CHECK(input::map_absolute(INT_MAX, widest, 1U) == 0);
    CHECK(input::map_absolute(INT_MAX - 1, widest, 2U) == 0);
    CHECK(input::map_absolute(INT_MAX, widest, 2U) == 1);
}

// ---- blit -------------------------------------------------------------------

void blit_within() {
    u32 failures = 0;
    for (u32 x = 0; x <= 6U; ++x) {
        for (u32 y = 0; y <= 6U; ++y) {
            for (u32 width = 0; width <= 6U; ++width) {
                for (u32 height = 0; height <= 6U; ++height) {
                    const stxlib::SurfaceRect rect{x, y, width, height};
                    const bool expected = width > 0U && height > 0U && x + width <= 5U && y + height <= 4U;
                    if (blit::rect_within(rect, 5U, 4U) != expected) failures += 1U;
                }
            }
        }
    }
    CHECK(failures == 0U);
    const stxlib::SurfaceRect overflow_width{4U, 0U, 0xFFFFFFFFU, 1U};
    const stxlib::SurfaceRect overflow_height{0U, 3U, 1U, 0xFFFFFFFEU};
    CHECK(!blit::rect_within(overflow_width, 5U, 4U));
    CHECK(!blit::rect_within(overflow_height, 5U, 4U));
}

void blit_copies() {
    constexpr u32 width = 5U;
    constexpr u32 height = 4U;
    Byte source[width * height * 4U];
    for (u32 index = 0; index < width * height * 4U; ++index) source[index] = static_cast<Byte>(index * 37U + 11U);
    const stxlib::SurfacePixels from{source, width * 4U, width, height};
    u32 failures = 0;
    for (u32 format = 0; format < 2U; ++format) {
        const usize pixel_bytes = format == 0U ? 4U : 2U;
        const usize row_bytes = width * pixel_bytes + 3U; // a stride wider than the row
        for (u32 x = 0; x < width; ++x) {
            for (u32 y = 0; y < height; ++y) {
                for (u32 w = 1; x + w <= width; ++w) {
                    for (u32 h = 1; y + h <= height; ++h) {
                        Byte target[(width * 4U + 3U) * height];
                        std::memset(target, 0xA5, sizeof(target));
                        const blit::FramebufferView to{target, row_bytes, width, height,
                                                       format == 0U ? blit::FramebufferFormat::bgrx8888
                                                                    : blit::FramebufferFormat::rgb565};
                        const stxlib::SurfaceRect rect{x, y, w, h};
                        blit::copy_rect(from, to, rect);
                        for (usize offset = 0; offset < row_bytes * height; ++offset) {
                            const usize row = offset / row_bytes;
                            const usize column_byte = offset % row_bytes;
                            const usize column = column_byte / pixel_bytes;
                            const bool inside = column_byte < width * pixel_bytes && row >= y && row < y + h &&
                                                column >= x && column < x + w;
                            Byte expected = 0xA5;
                            if (inside) {
                                const Byte* const pixel = source + row * width * 4U + column * 4U;
                                if (format == 0U) {
                                    expected = pixel[column_byte % 4U];
                                } else {
                                    const u32 value = (static_cast<u32>(pixel[2] & 0xF8U) << 8U) |
                                                      (static_cast<u32>(pixel[1] & 0xFCU) << 3U) |
                                                      (static_cast<u32>(pixel[0]) >> 3U);
                                    expected = static_cast<Byte>(column_byte % 2U == 0U ? value & 0xFFU : value >> 8U);
                                }
                            }
                            if (target[offset] != expected) failures += 1U;
                        }
                    }
                }
            }
        }
    }
    CHECK(failures == 0U);
}

void blit_rgb565_all() {
    u32 failures = 0;
    for (u32 colour = 0; colour < (1U << 24U); ++colour) {
        const Byte blue = static_cast<Byte>(colour & 0xFFU);
        const Byte green = static_cast<Byte>((colour >> 8U) & 0xFFU);
        const Byte red = static_cast<Byte>(colour >> 16U);
        const u32 expected = (static_cast<u32>(red & 0xF8U) << 8U) | (static_cast<u32>(green & 0xFCU) << 3U) |
                             (static_cast<u32>(blue) >> 3U);
        if (blit::rgb565_of(blue, green, red) != static_cast<u16>(expected)) failures += 1U;
    }
    CHECK(failures == 0U);
}

// ---- the public API where the family has no surface ---------------------------

void open_where_unsupported() {
#if !defined(__FreeBSD__)
    CHECK(!stxlib::capability_supported(stxlib::Capability::display_surface));
    stxlib::OperationResult<stxlib::Surface> opened = stxlib::open_surface();
    CHECK(opened.failed());
    if (opened.failed()) CHECK((*opened.error()).kind() == stxlib::ErrorKind::unsupported);
    // The claim was given back: a second try fails the same way, not busy.
    stxlib::OperationResult<stxlib::Surface> again = stxlib::open_surface();
    CHECK(again.failed());
    if (again.failed()) CHECK((*again.error()).kind() == stxlib::ErrorKind::unsupported);
#endif
    CHECK(stxlib::capability_name(stxlib::Capability::display_surface) == stxlib::TextView("display_surface"));
}

} // namespace

int main() {
    keys_table();
    keys_modifiers();
    keys_text();
    tracker_sequences();
    tracker_other_devices();
    absolute_mapping();
    blit_within();
    blit_copies();
    blit_rgb565_all();
    open_where_unsupported();
    return stxlib_tests::finish("surface_tests");
}
