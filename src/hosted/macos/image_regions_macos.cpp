// Image regions on macOS (hosted/image_backend.hpp, visit_regions): the
// named section of each dyld image, through _dyld_image_count,
// _dyld_get_image_header and getsectiondata, as GUI.Forms' localizable.cpp
// reads its tables (491156eac). getsectiondata returns the section's
// address with the image's slide applied and its size (getsectiondata(3);
// unconfirmed at a pin). Only 64-bit images (MH_MAGIC_64) are read: every
// image in a process STXlib supports is one. A1 platform-rest.
//
// The dyld image list can change while it is walked (another thread's
// dlopen or dlclose); an image added or removed during the walk may be
// missed, as images.hpp states.

#include "hosted/image_backend.hpp"

#include "core/host_failure.hpp"

#include <cstdint>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

namespace stxlib::image_backend {

namespace {

// A query name as a NUL-terminated field (images.cpp has checked it holds
// 1 to 16 bytes).
struct NameField final {
    char bytes[17]{};
};

NameField field_of(const TextView name) noexcept {
    NameField field{};
    for (usize index = 0; index < name.count() && index < 16; ++index) field.bytes[index] = name.data()[index];
    return field;
}

// Visits the section in one image; false when the visitor asks to stop.
bool visit_image(const mach_header_64* const header, const NameField& segment, const NameField& section,
                 const ImageRegionVisitor visitor, void* const context) noexcept {
    unsigned long size = 0;
    const std::uint8_t* const data = ::getsectiondata(header, segment.bytes, section.bytes, &size);
    if (data == nullptr || size == 0) return true;
    ImageRegion region{};
    region.bytes = View<Byte>(reinterpret_cast<const Byte*>(data), static_cast<usize>(size));
    region.form = ImageRegionForm::named_section;
    region.alignment = 4;
    region.image_base = reinterpret_cast<uptr>(header);
    const bool result = visitor(region, context);
    return result;
}

} // namespace

OperationStatus visit_regions(const ImageRegionQuery& query, const bool containing, const uptr address,
                     const ImageRegionVisitor visitor, void* const context) noexcept {
    const NameField segment = field_of(query.mach_o_segment);
    const NameField section = field_of(query.mach_o_section);
    if (containing) {
        Dl_info information{};
        if (::dladdr(reinterpret_cast<const void*>(address), &information) == 0 || information.dli_fbase == nullptr) {
            return host_failure::failed(ErrorKind::not_found, "stxlib.images.visit_regions_containing",
                                        HostCode::no_image);
        }
        const mach_header_64* const header = static_cast<const mach_header_64*>(information.dli_fbase);
        if ((*header).magic != MH_MAGIC_64) {
            return host_failure::failed(ErrorKind::not_found, "stxlib.images.visit_regions_containing",
                                        HostCode::no_image);
        }
        static_cast<void>(visit_image(header, segment, section, visitor, context));
        return OperationStatus::success();
    }
    const std::uint32_t count = ::_dyld_image_count();
    for (std::uint32_t index = 0; index < count; ++index) {
        const mach_header* const header = ::_dyld_get_image_header(index);
        if (header == nullptr || (*header).magic != MH_MAGIC_64) continue;
        const mach_header_64* const header64 = reinterpret_cast<const mach_header_64*>(header);
        if (!visit_image(header64, segment, section, visitor, context)) break;
    }
    return OperationStatus::success();
}

} // namespace stxlib::image_backend
