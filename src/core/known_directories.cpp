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

Result<Path> failure_of(const ErrorKind kind, const HostCode code) noexcept {
    Result<Path> result = Result<Path>::failure(host_failure::record(kind, operation, code));
    return result;
}

// A variable's value as an absolute Path. Absent, empty, or not an
// absolute path: not_found, so the caller falls back (the XDG
// specification ignores a relative value). out_of_memory passes through.
Result<Path> absolute_variable(const TextView name) noexcept {
    Result<Utf8String> value = read_variable(name, maximum_environment_bytes);
    if (value.failed()) {
        if ((*value.error()).kind() == ErrorKind::out_of_memory) return Result<Path>::failure(*value.error());
        return failure_of(ErrorKind::not_found, HostCode::none);
    }
    const TextView text = (*value.value()).view();
    if (text.empty()) return failure_of(ErrorKind::not_found, HostCode::none);
    Result<Path> parsed = Path::parse(text);
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return failure_of(ErrorKind::not_found, HostCode::none);
    }
    if (!(*parsed.value()).is_absolute()) return failure_of(ErrorKind::not_found, HostCode::none);
    return parsed;
}

// HOME, else the user database's home (POSIX hosts).
Result<Path> home_directory() noexcept {
    Result<Path> from_environment = absolute_variable("HOME");
    if (from_environment.succeeded()) return from_environment;
    if ((*from_environment.error()).kind() == ErrorKind::out_of_memory) return from_environment;
    Growable<char> text;
    const Status found = directories_backend::user_database_home(text);
    if (found.failed()) return Result<Path>::failure(*found.error());
    Result<Path> parsed = Path::from_posix_name(text.data(), text.count());
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return failure_of(ErrorKind::not_found, HostCode::no_home);
    }
    if (!(*parsed.value()).is_absolute()) return failure_of(ErrorKind::not_found, HostCode::no_home);
    return parsed;
}

// `base` joined with the relative `tail`.
Result<Path> under(const Path& base, const TextView tail) noexcept {
    Result<Path> relative = Path::parse(tail);
    if (relative.failed()) return relative;
    Result<Path> result = base.join(*relative.value());
    return result;
}

Result<Path> apple_directory(const KnownDirectory which) noexcept {
    Result<Path> home = home_directory();
    if (home.failed() || which == KnownDirectory::home) return home;
    const TextView tail = which == KnownDirectory::user_cache ? TextView("Library/Caches")
                                                              : TextView("Library/Application Support");
    Result<Path> result = under(*home.value(), tail);
    return result;
}

Result<Path> xdg_directory(const KnownDirectory which) noexcept {
    if (which == KnownDirectory::home) {
        Result<Path> home = home_directory();
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
    Result<Path> named = absolute_variable(variable);
    if (named.succeeded()) return named;
    if ((*named.error()).kind() == ErrorKind::out_of_memory) return named;
    Result<Path> home = home_directory();
    if (home.failed()) return home;
    Result<Path> result = under(*home.value(), fallback);
    return result;
}

Result<Path> windows_directory(const KnownDirectory which) noexcept {
    Growable<char> text;
    const Status found = directories_backend::known_folder(which, text);
    if (found.failed()) return Result<Path>::failure(*found.error());
    Result<Path> parsed = Path::parse_wtf8(text.data(), text.count());
    if (parsed.failed()) {
        if ((*parsed.error()).kind() == ErrorKind::out_of_memory) return parsed;
        return failure_of(ErrorKind::invalid_argument, HostCode::invalid_name);
    }
    return parsed;
}

} // namespace

Result<Path> known_directory(const KnownDirectory which) noexcept {
    if (!capability_supported(Capability::known_directories)) {
        return failure_of(ErrorKind::unsupported, HostCode::none);
    }
    switch (which) {
    case KnownDirectory::home:
    case KnownDirectory::user_configuration:
    case KnownDirectory::user_data:
    case KnownDirectory::user_cache:
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
