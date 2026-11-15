// Web storage (include/stxlib/web.hpp): validation, then the Emscripten
// family's storage module (hosted/web_backend.hpp). Every other family
// answers unsupported here and links no web module. Holds no state.

#include "stxlib/capabilities.hpp"
#include "stxlib/web.hpp"

#include "core/host_failure.hpp"

#if defined(__EMSCRIPTEN__)
#include "hosted/web_backend.hpp"
#endif

namespace stxlib {

namespace {

Status unsupported(const Operation operation) noexcept {
    const Status result = host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
    return result;
}

bool known_kind(const StorageKind kind) noexcept {
    const bool result = kind == StorageKind::memory || kind == StorageKind::indexed_db ||
                        kind == StorageKind::origin_private;
    return result;
}

} // namespace

Durability web_storage_durability(const StorageKind kind) noexcept {
    switch (kind) {
    case StorageKind::memory:
        return Durability::none;
    case StorageKind::indexed_db:
        return Durability::ordered;
    case StorageKind::origin_private:
        return Durability::durable;
    }
    return Durability::none;
}

Status mount_web_storage(const Path& path, const StorageKind kind) noexcept {
    const Operation operation = "stxlib.web.mount";
    if (!capability_supported(Capability::web_storage)) return unsupported(operation);
    if (!known_kind(kind)) return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    if (!path.is_absolute()) return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
#if defined(__EMSCRIPTEN__)
    Result<Buffer<char>> name = path.posix_name();
    if (name.failed()) {
        const ErrorKind failure = (*name.error()).kind();
        if (failure == ErrorKind::out_of_memory) return Status::failure(*name.error());
        return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    }
    const Status result = web_backend::mount((*name.value()).view().data(), kind);
    return result;
#else
    return unsupported(operation);
#endif
}

Status restore_web_storage() noexcept {
    const Operation operation = "stxlib.web.restore";
    if (!capability_supported(Capability::web_storage)) return unsupported(operation);
#if defined(__EMSCRIPTEN__)
    const Status result = web_backend::restore();
    return result;
#else
    return unsupported(operation);
#endif
}

Status persist_web_storage() noexcept {
    const Operation operation = "stxlib.web.persist";
    if (!capability_supported(Capability::web_storage)) return unsupported(operation);
#if defined(__EMSCRIPTEN__)
    const Status result = web_backend::persist();
    return result;
#else
    return unsupported(operation);
#endif
}

Status persist_web_storage_on_page_hide() noexcept {
    const Operation operation = "stxlib.web.persist_on_page_hide";
    if (!capability_supported(Capability::web_storage)) return unsupported(operation);
#if defined(__EMSCRIPTEN__)
    const Status result = web_backend::persist_on_page_hide();
    return result;
#else
    return unsupported(operation);
#endif
}

} // namespace stxlib
