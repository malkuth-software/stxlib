// Desktop integration (include/stxlib/desktop.hpp): the target checks every
// family shares, over the family's module (hosted/desktop_backend.hpp).
// Ported from stx_auxlib's src/hosted/desktop_<host>.cpp at b6e330a, where
// each host repeated the checks.

#include "stxlib/desktop.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "hosted/desktop_backend.hpp"

namespace stxlib {

namespace {

bool starts_with_folded(const TextView text, const TextView prefix) noexcept {
    if (text.count() < prefix.count()) return false;
    for (usize index = 0; index < prefix.count(); ++index) {
        char character = text.data()[index];
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        if (character != prefix.data()[index]) return false;
    }
    return true;
}

bool has_control_character(const TextView text) noexcept {
    for (usize index = 0; index < text.count(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text.data()[index]);
        if (byte < 0x20 || byte == 0x7f) return true;
    }
    return false;
}

bool acceptable_path(const Path& path) noexcept {
    const bool result = path.is_absolute() && (desktop_backend::drive_roots || path.drive() == 0);
    return result;
}

Status refused(const Operation operation) noexcept {
    const Status result = host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    return result;
}

bool is_link(const TextView target) noexcept {
    const bool result = starts_with_folded(target, "http://") || starts_with_folded(target, "https://") ||
                        starts_with_folded(target, "mailto:");
    return result;
}

} // namespace

Status open_link_with_desktop(const TextView link) noexcept {
    const Operation operation = "stxlib.desktop.open_link";
    if (has_control_character(link) || !is_link(link)) return refused(operation);
    const Status opened = desktop_backend::open_target(link, true);
    return opened;
}

Status open_with_desktop(const TextView target) noexcept {
    const Operation operation = "stxlib.desktop.open";
    if (has_control_character(target)) return refused(operation);
    if (is_link(target)) {
        const Status opened = desktop_backend::open_target(target, true);
        return opened;
    }
    Result<Path> parsed = Path::parse(target);
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return Status::failure(*parsed.error());
        return refused(operation);
    }
    const Path& path = *parsed.value();
    if (!acceptable_path(path)) return refused(operation);
    // Parsed from a TextView, so its text is Unicode.
    const Status result = desktop_backend::open_target(path.text().unicode(), false);
    return result;
}

Status reveal_in_file_manager(const Path& path) noexcept {
    const Operation operation = "stxlib.desktop.reveal";
    if (!acceptable_path(path)) return refused(operation);
    // The file manager is started as a child process, which takes a Path's
    // text losslessly (children.hpp), host names that are not valid Unicode
    // included.
    const Status result = desktop_backend::reveal(path);
    return result;
}

Result<Path> current_executable() noexcept {
    const Operation operation = "stxlib.desktop.current_executable";
    Growable<char> text;
    const Status found = desktop_backend::executable_name(text);
    if (found.failed()) return Result<Path>::failure(*found.error());
    // The host's name, losslessly (D13 a; desktop_backend.hpp).
    Result<Path> parsed = desktop_backend::byte_names ? Path::from_posix_name(text.data(), text.count())
                                                      : Path::parse_wtf8(text.data(), text.count());
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return Result<Path>::failure(host_failure::record(ErrorKind::io_failure, operation, HostCode::invalid_name));
    }
    return parsed;
}

} // namespace stxlib
