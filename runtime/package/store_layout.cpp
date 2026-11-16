// store_layout.cpp: see store_layout.h.

#include "store_layout.h"

namespace stxpackage {

namespace {

[[nodiscard]] bool append(StorePath& path, const char* text) noexcept {
    for (std::size_t index = 0; text[index] != '\0'; index += 1) {
        if (path.length >= store_path_max) {
            return false;
        }
        path.text[path.length] = text[index];
        path.length += 1;
    }
    return true;
}

}  // namespace

std::optional<StorePath> store_path(const char* const* parts, std::size_t count) noexcept {
    StorePath path{};
    path.length = 0;
    std::size_t first = 0;
    if (count > 0u && parts[0] == nullptr) {
        first = 1;
    } else if (!append(path, programs_root)) {
        return std::nullopt;
    }
    for (std::size_t index = first; index < count; index += 1) {
        if ((path.length > 0u && !append(path, "/")) || !append(path, parts[index])) {
            return std::nullopt;
        }
    }
    path.text[path.length] = '\0';
    const std::optional<StorePath> result = path;
    return result;
}

}  // namespace stxpackage
