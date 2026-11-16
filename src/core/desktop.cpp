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

// Characters a link may not hold (A-stxlib-hosted-01-F10). Windows hands a
// link to the scheme's registered command by putting it in place of "%1"
// ("Registering an Application to a URI Scheme", Microsoft Learn), and that
// command line is split again by the handler: a '"' or a space can end one
// argument and start another in a handler that does not guard against it.
// The rest are what cmd.exe and the C runtime's splitting treat specially
// ('<', '>', '^', '|', '\\') and the backtick. None may appear unencoded in a
// URI (RFC 3986 section 2: they are neither reserved nor unreserved), so a
// well-formed link carries them percent-encoded and loses nothing. The rule
// holds on every host, so a link accepted on one is accepted on all.
bool has_link_separator(const TextView text) noexcept {
    for (usize index = 0; index < text.count(); ++index) {
        const char character = text.data()[index];
        if (character == '"' || character == ' ' || character == '<' || character == '>' || character == '\\' ||
            character == '^' || character == '`' || character == '|') {
            return true;
        }
    }
    return false;
}

OperationStatus refused(const Operation operation) noexcept {
    const OperationStatus result = host_failure::failed(ErrorKind::invalid_argument, operation,
                                                        HostCode::invalid_value);
    return result;
}

bool is_link(const TextView target) noexcept {
    const bool result = starts_with_folded(target, "http://") || starts_with_folded(target, "https://") ||
                        starts_with_folded(target, "mailto:");
    return result;
}

} // namespace

OperationStatus open_link_with_desktop(const TextView link) noexcept {
    const Operation operation = "stxlib.desktop.open_link";
    if (has_control_character(link) || !is_link(link) || has_link_separator(link)) return refused(operation);
    const OperationStatus opened = desktop_backend::open_target(link, true);
    return opened;
}

OperationStatus open_with_desktop(const TextView target) noexcept {
    const Operation operation = "stxlib.desktop.open";
    if (has_control_character(target)) return refused(operation);
    if (is_link(target)) {
        if (has_link_separator(target)) return refused(operation);
        const OperationStatus opened = desktop_backend::open_target(target, true);
        return opened;
    }
    OperationResult<Path> parsed = Path::parse(target);
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return OperationStatus::failure(*parsed.error());
        return refused(operation);
    }
    const Path& path = *parsed.value();
    if (!acceptable_path(path)) return refused(operation);
    // Parsed from a TextView, so its text is Unicode.
    const OperationStatus result = desktop_backend::open_target(path.text().unicode(), false);
    return result;
}

OperationStatus reveal_in_file_manager(const Path& path) noexcept {
    const Operation operation = "stxlib.desktop.reveal";
    if (!acceptable_path(path)) return refused(operation);
    // The file manager is started as a child process, which takes a Path's
    // text losslessly (children.hpp), host names that are not valid Unicode
    // included.
    const OperationStatus result = desktop_backend::reveal(path);
    return result;
}

OperationResult<Path> current_executable() noexcept {
    const Operation operation = "stxlib.desktop.current_executable";
    Growable<char> text;
    const OperationStatus found = desktop_backend::executable_name(text);
    if (found.failed()) return OperationResult<Path>::failure(*found.error());
    // A host with drive roots never gives a UNC name here: its backend
    // refuses one (A-stxlib-core-01-F1). Checked again, since "//server/x"
    // would parse as the local "/server/x".
    if (desktop_backend::drive_roots && text.count() >= 2 && text[0] == '/' && text[1] == '/') {
        return OperationResult<Path>::failure(host_failure::record(ErrorKind::unsupported, operation, HostCode::none));
    }
    // The host's name, losslessly (D13 a; desktop_backend.hpp).
    OperationResult<Path> parsed = desktop_backend::byte_names ? Path::from_posix_name(text.data(), text.count())
                                                      : Path::parse_wtf8(text.data(), text.count());
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return OperationResult<Path>::failure(host_failure::record(ErrorKind::io_failure, operation,
                                                                   HostCode::invalid_name));
    }
    return parsed;
}

} // namespace stxlib
