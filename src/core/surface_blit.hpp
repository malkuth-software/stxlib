#pragma once

// The display surface's present (surface.hpp): copying damaged rectangles
// from the caller's BGRA8 pixels to the framebuffer's own format.
//
// Pure code: no system call, no allocation. Tested on every host
// (tests/surface_tests.cpp): every rectangle of a 5 by 4 surface, in both
// formats, against a per-pixel reference, with every pixel outside the
// rectangle checked untouched.
//
// The formats are what fbd(4) offers on the reference machines: 32-bit
// pixels with blue in the lowest byte (UEFI GOP's
// PixelBlueGreenRedReserved8BitPerColor on x86, the Pi firmware's
// framebuffer as U-Boot hands it on), and 16-bit RGB565 (a small panel). fbd
// reports only the depth (FBIOGTYPE), not the channel order, so the order
// of a 32-bit framebuffer is an assumption each board's checklist confirms
// with the drawing test target (appliance/boards/*.md).

#include "stxlib/surface.hpp"
#include "stxlib/types.hpp"

namespace stxlib::surface_blit {

enum class FramebufferFormat : u8 {
    bgrx8888 = 0,
    rgb565 = 1,
};

struct FramebufferView final {
    Byte* pixels;
    usize row_bytes;
    u32 width;
    u32 height;
    FramebufferFormat format;
};

// Whether `rect` is non-empty and lies within width by height (no
// overflow in the sums).
[[nodiscard]] bool rect_within(const SurfaceRect& rect, u32 width, u32 height) noexcept;

// Copies `rect` of `from` to the same place in `to`, converting. Requires
// rect_within for both, and from's row_bytes at least width * 4, to's at
// least width times its pixel size.
void copy_rect(const SurfacePixels& from, const FramebufferView& to, const SurfaceRect& rect) noexcept;

// One BGRA8 pixel as RGB565: the top 5, 6 and 5 bits of red, green, blue.
[[nodiscard]] u16 rgb565_of(Byte blue, Byte green, Byte red) noexcept;

} // namespace stxlib::surface_blit
