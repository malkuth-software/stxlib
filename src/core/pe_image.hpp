#pragma once

// Reading a loaded PE image (Windows) from its bytes: its size, a named
// section, and the export nearest below an address (images.hpp). Pure:
// every read goes through read_u16/read_u32, which check the offset
// against the view, so a damaged or hostile header can make a lookup fail
// but never read outside the view. In proof scope
// (tests/platform_rest_exhaustive.cpp; proofs/esbmc/platform_rest.cpp).
//
// Layout (Microsoft, "PE Format", as published; unconfirmed at a pin, the
// offsets are the format's and are the same in mingw-w64's winnt.h):
//   - DOS header: "MZ" at 0; e_lfanew (u32) at 0x3C, the NT headers'
//     offset.
//   - NT headers: "PE\0\0"; the file header (20 bytes): NumberOfSections
//     (u16) at +6, SizeOfOptionalHeader (u16) at +20; then the optional
//     header at +24: Magic (u16) 0x20B for PE32+ (the only form STXlib's
//     64-bit hosts load), SizeOfImage (u32) at +56, NumberOfRvaAndSizes
//     (u32) at +108, the data directories (8 bytes each: RVA, size) at
//     +112, the export table first.
//   - Section headers (40 bytes each) after the optional header: Name
//     (8 bytes, NUL-padded), VirtualSize (u32) at +8, VirtualAddress (u32)
//     at +12.
//   - Export directory: NumberOfFunctions (u32) at +20, NumberOfNames at
//     +24, AddressOfFunctions at +28, AddressOfNames at +32,
//     AddressOfNameOrdinals at +36 (all RVAs). A function RVA inside the
//     export directory's own range is a forwarder (a name, not code).
// In a loaded image an RVA is an offset from the image base, so the view
// is the image's mapping: [base, base + SizeOfImage).

#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::pe_image {

[[nodiscard]] inline bool read_u16(const View<Byte> image, const usize offset, u16& value) noexcept {
    if (offset > image.count() || image.count() - offset < 2) return false;
    const Byte* const at = image.data() + offset;
    value = static_cast<u16>(static_cast<u16>(at[0]) | static_cast<u16>(static_cast<u16>(at[1]) << 8));
    return true;
}

[[nodiscard]] inline bool read_u32(const View<Byte> image, const usize offset, u32& value) noexcept {
    if (offset > image.count() || image.count() - offset < 4) return false;
    const Byte* const at = image.data() + offset;
    value = static_cast<u32>(at[0]) | (static_cast<u32>(at[1]) << 8) | (static_cast<u32>(at[2]) << 16) |
            (static_cast<u32>(at[3]) << 24);
    return true;
}

// The facts of the headers every lookup needs.
struct Headers final {
    usize optional_offset{0};
    usize section_table_offset{0};
    u16 section_count{0};
    u32 size_of_image{0};
    u32 export_rva{0};
    u32 export_size{0};
};

inline constexpr u32 pe32_plus_magic = 0x20B;

// Reads the headers; false when they are not a PE32+ image's or do not lie
// inside the view. Only the first page is needed (the headers are there),
// so a caller may pass a view of the first page to learn SizeOfImage.
[[nodiscard]] inline bool read_headers(const View<Byte> image, Headers& headers) noexcept {
    headers = Headers{};
    u16 dos_magic = 0;
    if (!read_u16(image, 0, dos_magic) || dos_magic != 0x5A4D) return false;
    u32 nt_offset = 0;
    if (!read_u32(image, 0x3C, nt_offset)) return false;
    u32 signature = 0;
    if (!read_u32(image, nt_offset, signature) || signature != 0x00004550) return false;
    const usize file_header = static_cast<usize>(nt_offset) + 4;
    u16 section_count = 0;
    u16 optional_size = 0;
    if (!read_u16(image, file_header + 2, section_count)) return false;
    if (!read_u16(image, file_header + 16, optional_size)) return false;
    const usize optional = file_header + 20;
    u16 magic = 0;
    if (!read_u16(image, optional, magic) || magic != pe32_plus_magic) return false;
    u32 size_of_image = 0;
    if (!read_u32(image, optional + 56, size_of_image)) return false;
    u32 directory_count = 0;
    if (!read_u32(image, optional + 108, directory_count)) return false;
    headers.optional_offset = optional;
    headers.section_table_offset = optional + optional_size;
    headers.section_count = section_count;
    headers.size_of_image = size_of_image;
    // The export directory is entry 0, present when the count says so and
    // it lies inside the optional header.
    if (directory_count >= 1 && optional_size >= 120) {
        if (!read_u32(image, optional + 112, headers.export_rva)) return false;
        if (!read_u32(image, optional + 116, headers.export_size)) return false;
    }
    return true;
}

// A section's place in the image.
struct Section final {
    u32 rva{0};
    u32 size{0};
};

// The first section whose name is `name` (1 to 8 bytes), or whose name is
// `name` followed by '$' (a COFF group the linker did not merge); false
// when there is none, or when it does not lie inside the view.
[[nodiscard]] inline bool find_section(const View<Byte> image, const Headers& headers, const View<char> name,
                                       Section& section) noexcept {
    section = Section{};
    const usize length = name.count();
    if (length == 0 || length > 8) return false;
    for (u16 index = 0; index < headers.section_count; ++index) {
        const usize entry = headers.section_table_offset + static_cast<usize>(index) * 40;
        if (entry > image.count() || image.count() - entry < 40) return false;
        const Byte* const header = image.data() + entry;
        bool matches = true;
        for (usize at = 0; at < length; ++at) {
            if (header[at] != static_cast<Byte>(name.data()[at])) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (length < 8 && header[length] != 0 && header[length] != '$') continue;
        u32 virtual_size = 0;
        u32 rva = 0;
        if (!read_u32(image, entry + 8, virtual_size) || !read_u32(image, entry + 12, rva)) return false;
        if (rva > image.count() || image.count() - rva < virtual_size) return false;
        section.rva = rva;
        section.size = virtual_size;
        return true;
    }
    return false;
}

// The export nearest at or below an RVA, and its name's place.
struct Export final {
    u32 rva{0};
    u32 name_rva{0};
    usize name_count{0};
};

// The longest export name read; longer names are refused.
inline constexpr usize maximum_export_name = 4096;

// The named, non-forwarded export with the greatest RVA at or below
// `target`; false when there is none or the export data does not lie
// inside the view. Work: one pass over NumberOfNames, which the view
// bounds (each name takes four bytes of the view), then one name scan of
// at most maximum_export_name bytes.
[[nodiscard]] inline bool nearest_export(const View<Byte> image, const Headers& headers, const u32 target,
                                         Export& found) noexcept {
    found = Export{};
    if (headers.export_rva == 0 || headers.export_size == 0) return false;
    const usize directory = headers.export_rva;
    u32 function_count = 0;
    u32 name_count = 0;
    u32 functions = 0;
    u32 names = 0;
    u32 ordinals = 0;
    if (!read_u32(image, directory + 20, function_count) || !read_u32(image, directory + 24, name_count) ||
        !read_u32(image, directory + 28, functions) || !read_u32(image, directory + 32, names) ||
        !read_u32(image, directory + 36, ordinals)) {
        return false;
    }
    if (name_count > image.count() / 4) return false;
    const u64 forward_begin = headers.export_rva;
    const u64 forward_end = forward_begin + headers.export_size;
    bool any = false;
    for (u32 index = 0; index < name_count; ++index) {
        u16 ordinal = 0;
        if (!read_u16(image, static_cast<usize>(ordinals) + static_cast<usize>(index) * 2, ordinal)) return false;
        if (ordinal >= function_count) continue;
        u32 rva = 0;
        if (!read_u32(image, static_cast<usize>(functions) + static_cast<usize>(ordinal) * 4, rva)) return false;
        if (rva >= forward_begin && rva < forward_end) continue;
        if (rva > target) continue;
        if (any && rva <= found.rva) continue;
        u32 name_rva = 0;
        if (!read_u32(image, static_cast<usize>(names) + static_cast<usize>(index) * 4, name_rva)) return false;
        found.rva = rva;
        found.name_rva = name_rva;
        any = true;
    }
    if (!any) return false;
    // The name: bytes up to its NUL, inside the view and the bound.
    const usize start = found.name_rva;
    if (start >= image.count()) return false;
    const usize available = image.count() - start;
    const usize limit = available < maximum_export_name ? available : maximum_export_name;
    usize length = 0;
    while (length < limit && image.data()[start + length] != 0) length = length + 1;
    if (length == limit) return false;
    found.name_count = length;
    return true;
}

} // namespace stxlib::pe_image
