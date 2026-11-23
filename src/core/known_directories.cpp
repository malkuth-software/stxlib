// The user's directories (include/stxlib/files.hpp, known_directory): each
// family's convention over the environment (environment.hpp, so every read
// holds its lock) and the family's directories module
// (hosted/directories_backend.hpp). A1 platform-rest.

#include "stxlib/capabilities.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/files.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "hosted/directories_backend.hpp"

namespace stxlib {

namespace {

constexpr Operation operation = "stxlib.files.known_directory";

OperationResult<Path> failure_of(const ErrorKind kind, const HostCode code) noexcept {
    OperationResult<Path> result = OperationResult<Path>::failure(host_failure::record(kind, operation, code));
    return result;
}

// A variable's value as an absolute Path. Absent, empty, or not an
// absolute path: not_found, so the caller falls back (the XDG
// specification ignores a relative value). out_of_memory passes through.
OperationResult<Path> absolute_variable(const TextView name) noexcept {
    OperationResult<Utf8String> value = read_variable(name, maximum_environment_bytes);
    if (value.failed()) {
        if ((*value.error()).kind() == ErrorKind::out_of_memory) return OperationResult<Path>::failure(*value.error());
        return failure_of(ErrorKind::not_found, HostCode::none);
    }
    const TextView text = (*value.value()).view();
    if (text.empty()) return failure_of(ErrorKind::not_found, HostCode::none);
    OperationResult<Path> parsed = Path::parse(text);
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return failure_of(ErrorKind::not_found, HostCode::none);
    }
    if (!(*parsed.value()).is_absolute()) return failure_of(ErrorKind::not_found, HostCode::none);
    return parsed;
}

// HOME, else the user database's home (POSIX hosts).
OperationResult<Path> home_directory() noexcept {
    OperationResult<Path> from_environment = absolute_variable("HOME");
    if (from_environment.succeeded()) return from_environment;
    if ((*from_environment.error()).kind() == ErrorKind::out_of_memory) return from_environment;
    Growable<char> text;
    const OperationStatus found = directories_backend::user_database_home(text);
    if (found.failed()) return OperationResult<Path>::failure(*found.error());
    OperationResult<Path> parsed = Path::from_posix_name(text.data(), text.count());
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return failure_of(ErrorKind::not_found, HostCode::no_home);
    }
    if (!(*parsed.value()).is_absolute()) return failure_of(ErrorKind::not_found, HostCode::no_home);
    return parsed;
}

// `base` joined with the relative `tail`.
OperationResult<Path> under(const Path& base, const TextView tail) noexcept {
    OperationResult<Path> relative = Path::parse(tail);
    if (relative.failed()) return relative;
    OperationResult<Path> result = base.join(*relative.value());
    return result;
}

OperationResult<Path> apple_directory(const KnownDirectory which) noexcept {
    OperationResult<Path> home = home_directory();
    if (home.failed() || which == KnownDirectory::home) return home;
    const TextView tail = which == KnownDirectory::user_cache ? TextView("Library/Caches")
                                                              : TextView("Library/Application Support");
    OperationResult<Path> result = under(*home.value(), tail);
    return result;
}

OperationResult<Path> xdg_directory(const KnownDirectory which) noexcept {
    if (which == KnownDirectory::home) {
        OperationResult<Path> home = home_directory();
        return home;
    }
    TextView variable = "XDG_CONFIG_HOME";
    TextView fallback = ".config";
    if (which == KnownDirectory::user_data) {
        variable = "XDG_DATA_HOME";
        fallback = ".local/share";
    } else if (which == KnownDirectory::user_cache) {
        variable = "XDG_CACHE_HOME";
        fallback = ".cache";
    }
    OperationResult<Path> named = absolute_variable(variable);
    if (named.succeeded()) return named;
    if ((*named.error()).kind() == ErrorKind::out_of_memory) return named;
    OperationResult<Path> home = home_directory();
    if (home.failed()) return home;
    OperationResult<Path> result = under(*home.value(), fallback);
    return result;
}

OperationResult<Path> windows_directory(const KnownDirectory which) noexcept {
    Growable<char> text;
    const OperationStatus found = directories_backend::known_folder(which, text);
    if (found.failed()) return OperationResult<Path>::failure(*found.error());
    // The backend refuses a UNC name (A-stxlib-core-01-F1); checked again,
    // since "//server/share" would parse as the local "/server/share".
    if (text.count() >= 2 && text[0] == '/' && text[1] == '/') {
        return failure_of(ErrorKind::unsupported, HostCode::none);
    }
    OperationResult<Path> parsed = Path::parse_wtf8(text.data(), text.count());
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return failure_of(ErrorKind::invalid_argument, HostCode::invalid_name);
    }
    return parsed;
}

} // namespace

OperationResult<Path> known_directory(const KnownDirectory which) noexcept {
    if (!capability_supported(Capability::known_directories)) {
        return failure_of(ErrorKind::unsupported, HostCode::none);
    }
    switch (which) {
    case KnownDirectory::home:
    case KnownDirectory::user_configuration:
    case KnownDirectory::user_data:
    case KnownDirectory::user_cache:
    case KnownDirectory::user_local_configuration:
        break;
    default:
        return failure_of(ErrorKind::unsupported, HostCode::invalid_value);
    }
    switch (directories_backend::convention) {
    case directories_backend::Convention::apple:
        return apple_directory(which);
    case directories_backend::Convention::xdg:
        return xdg_directory(which);
    case directories_backend::Convention::windows_folders:
        return windows_directory(which);
    }
    return failure_of(ErrorKind::unsupported, HostCode::none);
}

} // namespace stxlib
