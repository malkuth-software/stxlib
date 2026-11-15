// container.cpp: see container.h.

#include "container.h"

namespace stxpackage {

ContainerOutcome::ContainerOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

ContainerOutcome ContainerOutcome::head(const ContainerHead& value) noexcept {
    return ContainerOutcome(Alternatives(value));
}

ContainerOutcome ContainerOutcome::refused(ContainerRefusal reason) noexcept {
    return ContainerOutcome(Alternatives(reason));
}

const ContainerHead* ContainerOutcome::as_head() const noexcept {
    return std::get_if<ContainerHead>(&alternatives_);
}

const ContainerRefusal* ContainerOutcome::as_refused() const noexcept {
    return std::get_if<ContainerRefusal>(&alternatives_);
}

const char* container_refusal_name(ContainerRefusal reason) noexcept {
    switch (reason) {
    case ContainerRefusal::truncated: return "truncated";
    case ContainerRefusal::magic_malformed: return "magic_malformed";
    case ContainerRefusal::manifest_line_malformed: return "manifest_line_malformed";
    case ContainerRefusal::signature_line_malformed: return "signature_line_malformed";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

// Whether bytes at offset begin with literal; truncated is set when the
// bytes end first while still matching.
[[nodiscard]] bool starts_with(std::span<const char> bytes, std::size_t offset, const char* literal,
                               bool& truncated) noexcept {
    std::size_t index = 0;
    while (literal[index] != '\0') {
        if (offset + index >= bytes.size()) {
            truncated = true;
            return false;
        }
        if (bytes[offset + index] != literal[index]) {
            return false;
        }
        index += 1;
    }
    return true;
}

}  // namespace

ContainerOutcome parse_container_head(std::span<const char> bytes) noexcept {
    bool truncated = false;
    if (!starts_with(bytes, 0, "STXPKG 1\n", truncated)) {
        return ContainerOutcome::refused(truncated ? ContainerRefusal::truncated : ContainerRefusal::magic_malformed);
    }
    std::size_t offset = 9;
    if (!starts_with(bytes, offset, "manifest ", truncated)) {
        return ContainerOutcome::refused(truncated ? ContainerRefusal::truncated
                                                   : ContainerRefusal::manifest_line_malformed);
    }
    offset += 9;
    const std::size_t digits_begin = offset;
    std::size_t length = 0;
    while (offset < bytes.size() && bytes[offset] >= '0' && bytes[offset] <= '9' && offset - digits_begin < 6u) {
        length = length * 10u + static_cast<std::size_t>(bytes[offset] - '0');
        offset += 1;
    }
    if (offset == bytes.size()) {
        return ContainerOutcome::refused(ContainerRefusal::truncated);
    }
    const std::size_t digits = offset - digits_begin;
    if (digits == 0u || bytes[digits_begin] == '0' || bytes[offset] != '\n' || length > manifest_size_max) {
        return ContainerOutcome::refused(ContainerRefusal::manifest_line_malformed);
    }
    offset += 1;
    ContainerHead head{};
    head.manifest_offset = offset;
    head.manifest_length = length;
    if (bytes.size() - offset < length) {
        return ContainerOutcome::refused(ContainerRefusal::truncated);
    }
    offset += length;
    if (!starts_with(bytes, offset, "signature 64\n", truncated)) {
        return ContainerOutcome::refused(truncated ? ContainerRefusal::truncated
                                                   : ContainerRefusal::signature_line_malformed);
    }
    offset += 13;
    if (bytes.size() - offset < signature_size) {
        return ContainerOutcome::refused(ContainerRefusal::truncated);
    }
    head.signature_offset = offset;
    head.data_offset = offset + signature_size;
    return ContainerOutcome::head(head);
}

std::size_t write_container_prefix(std::size_t manifest_length, std::span<char> out) noexcept {
    if (manifest_length == 0u || manifest_length > manifest_size_max || out.size() < 24u) {
        return 0;
    }
    const char magic[] = "STXPKG 1\nmanifest ";
    std::size_t used = 0;
    for (std::size_t index = 0; magic[index] != '\0'; index += 1) {
        out[used] = magic[index];
        used += 1;
    }
    char digits[6] = {};
    std::size_t count = 0;
    std::size_t rest = manifest_length;
    while (rest > 0u) {
        digits[count] = static_cast<char>('0' + static_cast<char>(rest % 10u));
        rest /= 10u;
        count += 1;
    }
    while (count > 0u) {
        count -= 1;
        out[used] = digits[count];
        used += 1;
    }
    out[used] = '\n';
    used += 1;
    return used;
}

}  // namespace stxpackage
