#pragma once

// The primitives each family's image modules supply: libraries
// (posix/library_posix.cpp, windows/images_windows.cpp), address lookup
// (the same files) and image regions (macos/image_regions_macos.cpp,
// elf/image_regions_elf.cpp, windows/images_windows.cpp); exactly one of
// each is linked. core/library.cpp and core/images.cpp validate every
// request before calling them: names are non-empty, NUL-terminated, of the
// lengths images.hpp and library.hpp allow, and paths are absolute host
// names in the family's form (file_backend.hpp, "Names").

#include "stxlib/images.hpp"
#include "stxlib/library.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib::image_backend {

// The family's image format, which decides how a region is named
// (images.hpp, ImageRegionQuery).
enum class ImageFormat : u8 {
    mach_o = 0,
    pe = 1,
    elf = 2,
};

#if defined(__APPLE__)
inline constexpr ImageFormat image_format = ImageFormat::mach_o;
#elif defined(_WIN32)
inline constexpr ImageFormat image_format = ImageFormat::pe;
#else
inline constexpr ImageFormat image_format = ImageFormat::elf;
#endif

// How a library is found (library.hpp, "Search").
enum class LoadMode : u8 {
    path = 0,        // exactly this file
    system = 1,      // a file name, searched as the host searches its own libraries
    loaded_only = 2, // a file name already loaded; nothing is loaded
};

// On success `handle` holds one reference to the image; never 0.
[[nodiscard]] Status open_library(const char* name, LoadMode mode, uptr& handle) noexcept;
// Gives the reference back; the handle is released even on failure.
[[nodiscard]] Status close_library(uptr handle) noexcept;
// The symbol's address, or not_found with HostCode::symbol_not_found.
[[nodiscard]] Status find_symbol(uptr handle, const char* name, uptr& address) noexcept;

// describe_address's work, rooms already checked.
[[nodiscard]] Status describe(uptr address, MutableView<char> image_name_room, MutableView<char> symbol_name_room,
                              AddressDescription& description) noexcept;

// Visits the query's region in every image (`containing` false) or in the
// image holding `address` only. The query's names are checked; the
// visitor is not null.
[[nodiscard]] Status visit_regions(const ImageRegionQuery& query, bool containing, uptr address,
                                   ImageRegionVisitor visitor, void* context) noexcept;

// Copies `count` host bytes into `room`, cut to its capacity; the copied
// view and whether it was cut. Shared by the families.
inline View<char> copy_into(const char* const bytes, const usize count, const MutableView<char> room,
                            bool& truncated) noexcept {
    const usize copied = count < room.count() ? count : room.count();
    for (usize index = 0; index < copied; ++index) room[index] = bytes[index];
    truncated = copied < count;
    const View<char> result(room.data(), copied);
    return result;
}

} // namespace stxlib::image_backend
