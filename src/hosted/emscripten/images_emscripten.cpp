// Libraries, address lookup and image regions on Emscripten
// (hosted/image_backend.hpp; owner decision 2026-11-15): none. A program
// is one wasm module; loading another needs a build made for dynamic
// linking (MAIN_MODULE and SIDE_MODULE, src/settings.js), which STXlib's
// family does not ask for, and wasm code cannot map an address back to a
// module or symbol. Capability::dynamic_libraries, address_lookup and
// image_regions are false (core/capabilities.cpp), and core checks them
// before calling here; these definitions answer unsupported in case a
// caller reaches them anyway.

#include "hosted/image_backend.hpp"

#include "core/host_failure.hpp"

namespace stxlib::image_backend {

namespace {

Status unsupported(const Operation operation) noexcept {
    const Status result = host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
    return result;
}

} // namespace

Status open_library(const char* const name, const LoadMode mode, uptr& handle) noexcept {
    static_cast<void>(name);
    static_cast<void>(mode);
    handle = 0;
    const Status result = unsupported("stxlib.library.open");
    return result;
}

Status close_library(const uptr handle) noexcept {
    static_cast<void>(handle);
    const Status result = unsupported("stxlib.library.close");
    return result;
}

Status find_symbol(const uptr handle, const char* const name, uptr& address) noexcept {
    static_cast<void>(handle);
    static_cast<void>(name);
    address = 0;
    const Status result = unsupported("stxlib.library.find_symbol");
    return result;
}

Status describe(const uptr address, const MutableView<char> image_name_room, const MutableView<char> symbol_name_room,
                AddressDescription& description) noexcept {
    static_cast<void>(address);
    static_cast<void>(image_name_room);
    static_cast<void>(symbol_name_room);
    static_cast<void>(description);
    const Status result = unsupported("stxlib.images.describe_address");
    return result;
}

Status visit_regions(const ImageRegionQuery& query, const bool containing, const uptr address,
                     const ImageRegionVisitor visitor, void* const context) noexcept {
    static_cast<void>(query);
    static_cast<void>(containing);
    static_cast<void>(address);
    static_cast<void>(visitor);
    static_cast<void>(context);
    const Status result = unsupported("stxlib.images.visit_regions");
    return result;
}

} // namespace stxlib::image_backend
