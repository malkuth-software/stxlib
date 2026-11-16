#include "core/surface_blit.hpp"

#include <cstring>

namespace stxlib::surface_blit {

bool rect_within(const SurfaceRect& rect, const u32 width, const u32 height) noexcept {
    if (rect.width == 0U || rect.height == 0U) return false;
    if (rect.x >= width || rect.y >= height) return false;
    const bool fits = rect.width <= width - rect.x && rect.height <= height - rect.y;
    return fits;
}

u16 rgb565_of(const Byte blue, const Byte green, const Byte red) noexcept {
    const u32 value = ((static_cast<u32>(red) >> 3U) << 11U) | ((static_cast<u32>(green) >> 2U) << 5U) |
                      (static_cast<u32>(blue) >> 3U);
    return static_cast<u16>(value);
}

void copy_rect(const SurfacePixels& from, const FramebufferView& to, const SurfaceRect& rect) noexcept {
    for (u32 row = 0U; row < rect.height; ++row) {
        const usize y = static_cast<usize>(rect.y) + row;
        const Byte* const source = from.pixels + y * from.row_bytes + static_cast<usize>(rect.x) * 4U;
        switch (to.format) {
        case FramebufferFormat::bgrx8888: {
            Byte* const target = to.pixels + y * to.row_bytes + static_cast<usize>(rect.x) * 4U;
            std::memcpy(target, source, static_cast<usize>(rect.width) * 4U);
            break;
        }
        case FramebufferFormat::rgb565: {
            Byte* const target = to.pixels + y * to.row_bytes + static_cast<usize>(rect.x) * 2U;
            for (u32 column = 0U; column < rect.width; ++column) {
                const Byte* const pixel = source + static_cast<usize>(column) * 4U;
                const u16 value = rgb565_of(pixel[0], pixel[1], pixel[2]);
                // Little-endian, as both reference machines store it.
                target[static_cast<usize>(column) * 2U] = static_cast<Byte>(value & 0xFFU);
                target[static_cast<usize>(column) * 2U + 1U] = static_cast<Byte>(value >> 8U);
            }
            break;
        }
        default:
            // A value outside the enumeration: nothing is copied.
            return;
        }
    }
}

} // namespace stxlib::surface_blit
