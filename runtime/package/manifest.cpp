// manifest.cpp: see manifest.h.

#include "manifest.h"

#include "../supervisor/config/boot_config.h"

namespace stxpackage {

const char* manifest_error_name(ManifestError error) noexcept {
    switch (error) {
    case ManifestError::too_large: return "too_large";
    case ManifestError::line_malformed: return "line_malformed";
    case ManifestError::header_malformed: return "header_malformed";
    case ManifestError::name_malformed: return "name_malformed";
    case ManifestError::version_malformed: return "version_malformed";
    case ManifestError::file_line_malformed: return "file_line_malformed";
    case ManifestError::mode_malformed: return "mode_malformed";
    case ManifestError::size_malformed: return "size_malformed";
    case ManifestError::file_too_large: return "file_too_large";
    case ManifestError::package_too_large: return "package_too_large";
    case ManifestError::hash_malformed: return "hash_malformed";
    case ManifestError::path_malformed: return "path_malformed";
    case ManifestError::not_sorted: return "not_sorted";
    case ManifestError::path_conflict: return "path_conflict";
    case ManifestError::too_many_files: return "too_many_files";
    case ManifestError::no_files: return "no_files";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

bool sha256_of_hex(const char* hex, std::size_t length, std::uint8_t (&digest)[sha256_size]) noexcept {
    if (length != 2u * sha256_size) {
        return false;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        const char byte = hex[index];
        std::uint32_t nibble = 0;
        if (byte >= '0' && byte <= '9') {
            nibble = static_cast<std::uint32_t>(byte - '0');
        } else if (byte >= 'a' && byte <= 'f') {
            nibble = static_cast<std::uint32_t>(byte - 'a' + 10);
        } else {
            return false;
        }
        if (index % 2u == 0u) {
            digest[index / 2u] = static_cast<std::uint8_t>(nibble << 4u);
        } else {
            digest[index / 2u] = static_cast<std::uint8_t>(digest[index / 2u] | nibble);
        }
    }
    return true;
}

namespace {

struct Span {
    std::size_t begin;
    std::size_t end;
};

[[nodiscard]] bool span_is(std::span<const char> text, const Span& span, const char* literal) noexcept {
    std::size_t index = 0;
    while (literal[index] != '\0') {
        if (span.begin + index >= span.end || text[span.begin + index] != literal[index]) {
            return false;
        }
        index += 1;
    }
    return span.begin + index == span.end;
}

// A decimal without a leading zero ("0" itself allowed when zero_allowed),
// at most maximum, or nothing.
[[nodiscard]] std::optional<std::uint64_t> decimal_of(std::span<const char> text, const Span& span,
                                                      std::uint64_t maximum, bool zero_allowed) noexcept {
    const std::size_t length = span.end - span.begin;
    if (length == 0u || length > 19u) {
        return std::nullopt;
    }
    if (text[span.begin] == '0' && (length > 1u || !zero_allowed)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t index = span.begin; index < span.end; index += 1) {
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
        value = value * 10u + static_cast<std::uint64_t>(text[index] - '0');
    }
    if (value > maximum) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> result = value;
    return result;
}

[[nodiscard]] bool name_valid(std::span<const char> text, const Span& span) noexcept {
    const std::size_t length = span.end - span.begin;
    if (length == 0u || length > name_length_max || text[span.begin] < 'a' || text[span.begin] > 'z') {
        return false;
    }
    for (std::size_t index = span.begin; index < span.end; index += 1) {
        const char byte = text[index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-')) {
            return false;
        }
    }
    return true;
}

// Splits the line [begin, end) at single spaces into at most count fields;
// the last field takes the rest of the line. False when there are fewer.
[[nodiscard]] bool split_fields(std::span<const char> text, std::size_t begin, std::size_t end, Span* fields,
                                std::size_t count) noexcept {
    std::size_t field_begin = begin;
    for (std::size_t field = 0; field + 1u < count; field += 1) {
        std::size_t index = field_begin;
        while (index < end && text[index] != ' ') {
            index += 1;
        }
        if (index == end) {
            return false;
        }
        fields[field] = Span{field_begin, index};
        field_begin = index + 1u;
    }
    fields[count - 1u] = Span{field_begin, end};
    return true;
}

// Byte order of two paths in the text: negative, zero or positive.
[[nodiscard]] int compare_paths(std::span<const char> text, const ManifestFile& left,
                                const ManifestFile& right) noexcept {
    const std::size_t shorter = left.path_length < right.path_length ? left.path_length : right.path_length;
    for (std::size_t index = 0; index < shorter; index += 1) {
        const unsigned char l = static_cast<unsigned char>(text[left.path_offset + index]);
        const unsigned char r = static_cast<unsigned char>(text[right.path_offset + index]);
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    if (left.path_length == right.path_length) {
        return 0;
    }
    return left.path_length < right.path_length ? -1 : 1;
}

// Whether directory's path followed by '/' begins path's.
[[nodiscard]] bool is_directory_of(std::span<const char> text, const ManifestFile& directory,
                                   const ManifestFile& path) noexcept {
    if (path.path_length <= directory.path_length) {
        return false;
    }
    for (std::size_t index = 0; index < directory.path_length; index += 1) {
        if (text[directory.path_offset + index] != text[path.path_offset + index]) {
            return false;
        }
    }
    return text[path.path_offset + directory.path_length] == '/';
}

[[nodiscard]] std::optional<ManifestRefused> refuse(ManifestError error, std::uint32_t line) noexcept {
    const std::optional<ManifestRefused> refused = ManifestRefused{error, line};
    return refused;
}

// One file line's fields into file, or the refusal.
[[nodiscard]] std::optional<ManifestError> read_file_line(std::span<const char> text, std::size_t begin,
                                                          std::size_t end, ManifestFile& file) noexcept {
    Span fields[5] = {};
    if (!split_fields(text, begin, end, fields, 5) || !span_is(text, fields[0], "file")) {
        return ManifestError::file_line_malformed;
    }
    if (span_is(text, fields[1], "0444")) {
        file.mode = mode_data;
    } else if (span_is(text, fields[1], "0555")) {
        file.mode = mode_executable;
    } else {
        return ManifestError::mode_malformed;
    }
    const std::optional<std::uint64_t> size = decimal_of(text, fields[2], UINT64_MAX, true);
    if (!size.has_value()) {
        return ManifestError::size_malformed;
    }
    if (*size > file_size_max) {
        return ManifestError::file_too_large;
    }
    file.size = *size;
    if (!sha256_of_hex(&text[fields[3].begin], fields[3].end - fields[3].begin, file.sha256)) {
        return ManifestError::hash_malformed;
    }
    const std::size_t path_length = fields[4].end - fields[4].begin;
    if (!stxconfig::package_path_valid(&text[fields[4].begin], path_length)) {
        return ManifestError::path_malformed;
    }
    file.path_offset = static_cast<std::uint32_t>(fields[4].begin);
    file.path_length = static_cast<std::uint32_t>(path_length);
    return std::nullopt;
}

}  // namespace

std::optional<ManifestRefused> parse_manifest(std::span<const char> text, Manifest& manifest) noexcept {
    if (text.size() > manifest_size_max) {
        return refuse(ManifestError::too_large, 0);
    }
    if (text.empty() || text[text.size() - 1u] != '\n') {
        return refuse(ManifestError::line_malformed, 0);
    }
    manifest.file_count = 0;
    manifest.total_size = 0;
    std::uint32_t line_number = 0;
    std::size_t begin = 0;
    // Each pass consumes one line and its LF; the text ends in LF.
    while (begin < text.size()) {
        std::size_t end = begin;
        while (text[end] != '\n') {
            const unsigned char byte = static_cast<unsigned char>(text[end]);
            if (byte < 0x20u || byte > 0x7eu) {
                return refuse(ManifestError::line_malformed, line_number + 1u);
            }
            end += 1;
        }
        line_number += 1;
        const Span line{begin, end};
        if (line_number == 1u) {
            if (!span_is(text, line, "stx-manifest 1")) {
                return refuse(ManifestError::header_malformed, line_number);
            }
        } else if (line_number == 2u) {
            Span fields[2] = {};
            if (!split_fields(text, begin, end, fields, 2) || !span_is(text, fields[0], "name") ||
                !name_valid(text, fields[1])) {
                return refuse(ManifestError::name_malformed, line_number);
            }
            manifest.name_offset = static_cast<std::uint32_t>(fields[1].begin);
            manifest.name_length = static_cast<std::uint32_t>(fields[1].end - fields[1].begin);
        } else if (line_number == 3u) {
            Span fields[2] = {};
            if (!split_fields(text, begin, end, fields, 2) || !span_is(text, fields[0], "version")) {
                return refuse(ManifestError::version_malformed, line_number);
            }
            const std::optional<std::uint64_t> version = decimal_of(text, fields[1], UINT32_MAX, false);
            if (!version.has_value()) {
                return refuse(ManifestError::version_malformed, line_number);
            }
            manifest.version = static_cast<std::uint32_t>(*version);
        } else {
            if (manifest.file_count == manifest_files_max) {
                return refuse(ManifestError::too_many_files, line_number);
            }
            ManifestFile& file = manifest.files[manifest.file_count];
            const std::optional<ManifestError> error = read_file_line(text, begin, end, file);
            if (error.has_value()) {
                return refuse(*error, line_number);
            }
            if (manifest.file_count > 0u && compare_paths(text, manifest.files[manifest.file_count - 1u], file) >= 0) {
                return refuse(ManifestError::not_sorted, line_number);
            }
            // Both at most 2^31 before the sum: no overflow.
            manifest.total_size += file.size;
            if (manifest.total_size > package_size_max) {
                return refuse(ManifestError::package_too_large, line_number);
            }
            manifest.file_count += 1;
        }
        begin = end + 1u;
    }
    if (line_number < 3u) {
        return refuse(line_number == 0u ? ManifestError::header_malformed
                                        : (line_number == 1u ? ManifestError::name_malformed
                                                             : ManifestError::version_malformed),
                      line_number + 1u);
    }
    if (manifest.file_count == 0u) {
        return refuse(ManifestError::no_files, line_number + 1u);
    }
    // Sorted, so the paths a directory would prefix follow it directly:
    // every path with p as a prefix lies between p and the first path that
    // does not have it.
    for (std::uint32_t index = 0; index < manifest.file_count; index += 1) {
        for (std::uint32_t later = index + 1u; later < manifest.file_count; later += 1) {
            const ManifestFile& directory = manifest.files[index];
            const ManifestFile& path = manifest.files[later];
            bool has_prefix = path.path_length >= directory.path_length;
            for (std::size_t byte = 0; has_prefix && byte < directory.path_length; byte += 1) {
                has_prefix = text[directory.path_offset + byte] == text[path.path_offset + byte];
            }
            if (!has_prefix) {
                break;
            }
            if (is_directory_of(text, directory, path)) {
                return refuse(ManifestError::path_conflict, 3u + later + 1u);
            }
        }
    }
    return std::nullopt;
}

bool manifest_name_is(std::span<const char> text, const Manifest& manifest, const char* name) noexcept {
    std::size_t index = 0;
    while (name[index] != '\0') {
        if (index >= manifest.name_length || text[manifest.name_offset + index] != name[index]) {
            return false;
        }
        index += 1;
    }
    return index == manifest.name_length;
}

std::optional<std::size_t> find_manifest_file(std::span<const char> text, const Manifest& manifest, const char* path,
                                              std::size_t length) noexcept {
    std::size_t low = 0;
    std::size_t high = manifest.file_count;
    // Halves [low, high) each pass: at most 11 passes for 1024 files.
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2u;
        const ManifestFile& file = manifest.files[middle];
        int order = 0;
        const std::size_t shorter = file.path_length < length ? file.path_length : length;
        for (std::size_t index = 0; index < shorter && order == 0; index += 1) {
            const unsigned char l = static_cast<unsigned char>(text[file.path_offset + index]);
            const unsigned char r = static_cast<unsigned char>(path[index]);
            if (l != r) {
                order = l < r ? -1 : 1;
            }
        }
        if (order == 0 && file.path_length != length) {
            order = file.path_length < length ? -1 : 1;
        }
        if (order == 0) {
            const std::optional<std::size_t> found = middle;
            return found;
        }
        if (order < 0) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return std::nullopt;
}

}  // namespace stxpackage
