// The loaded images (include/stxlib/images.hpp): request checks over the
// family's image modules (hosted/image_backend.hpp). A1 platform-rest.

#include "stxlib/capabilities.hpp"
#include "stxlib/images.hpp"

#include "core/host_failure.hpp"
#include "hosted/image_backend.hpp"

namespace stxlib {

namespace {

// The longest names each format's fields hold (images.hpp).
constexpr usize mach_o_name_bytes = 16;
constexpr usize pe_name_bytes = 8;

bool name_fits(const TextView name, const usize limit) noexcept {
    const bool result = !name.empty() && name.count() <= limit;
    return result;
}

// The query names the region in this family's format.
bool acceptable_query(const ImageRegionQuery& query) noexcept {
    switch (image_backend::image_format) {
    case image_backend::ImageFormat::mach_o:
        return name_fits(query.mach_o_segment, mach_o_name_bytes) &&
               name_fits(query.mach_o_section, mach_o_name_bytes);
    case image_backend::ImageFormat::pe:
        return name_fits(query.pe_section, pe_name_bytes);
    case image_backend::ImageFormat::elf:
        // Note segments are found by type, not name (images.hpp).
        return true;
    }
    return false;
}

OperationStatus visit(const ImageRegionQuery& query, const bool containing, const uptr address,
                      const ImageRegionVisitor visitor, void* const context, const Operation operation) noexcept {
    if (!capability_supported(Capability::image_regions)) {
        return host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
    }
    if (visitor == nullptr || !acceptable_query(query)) {
        return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    }
    const OperationStatus result = image_backend::visit_regions(query, containing, address, visitor, context);
    return result;
}

} // namespace

OperationResult<AddressDescription> describe_address(const uptr address, const MutableView<char> image_name_room,
                                                     const MutableView<char> symbol_name_room) noexcept {
    if (!capability_supported(Capability::address_lookup)) {
        const ErrorRecord record =
            host_failure::record(ErrorKind::unsupported, "stxlib.images.describe_address", HostCode::none);
        return OperationResult<AddressDescription>::failure(record);
    }
    AddressDescription description{};
    const OperationStatus described = image_backend::describe(address, image_name_room, symbol_name_room, description);
    if (described.failed()) return OperationResult<AddressDescription>::failure(*described.error());
    return OperationResult<AddressDescription>::success(description);
}

OperationStatus visit_image_regions(const ImageRegionQuery& query, const ImageRegionVisitor visitor,
                                    void* const context) noexcept {
    const OperationStatus result = visit(query, false, 0, visitor, context, "stxlib.images.visit_regions");
    return result;
}

OperationStatus visit_image_regions_containing(const uptr address, const ImageRegionQuery& query,
                                               const ImageRegionVisitor visitor, void* const context) noexcept {
    const OperationStatus result = visit(query, true, address, visitor, context,
                                         "stxlib.images.visit_regions_containing");
    return result;
}

} // namespace stxlib
