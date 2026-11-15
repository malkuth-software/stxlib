// test_manifest.cpp: host tests of the manifest and the container head.
//
// Manifest: examples with their refusals and lines; exhaustive over every
// list of 1 to 4 paths drawn from 7 paths chosen to meet the order and
// conflict rules ("a", "a-b", "a.b", "a/b", "a/b/c", "a0", "b": 2,800
// lists), against a reference (strictly ascending by strcmp, no path
// followed by '/' starting a later one); and canonical spelling: every
// single-byte change (each position, all 256 values) and deletion of a
// valid manifest is refused or re-spells to exactly the changed text.
// Container: examples; every truncation of a valid package's head is
// refused as truncated; the length line for every manifest length 1..65537.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include "../container.h"
#include "../manifest.h"
#include "../../supervisor/config/test/test_support.h"

namespace {

using stxpackage::Manifest;
using stxpackage::ManifestError;
using stxpackage::ManifestRefused;
using stxtest::check;
using stxtest::text_of;

Manifest manifest;

const char* const zeros = "0000000000000000000000000000000000000000000000000000000000000000";
const char* const hash_ab = "ab00000000000000000000000000000000000000000000000000000000000001";

[[nodiscard]] std::optional<ManifestRefused> parse(const char* text) {
    return stxpackage::parse_manifest(text_of(text), manifest);
}

[[nodiscard]] bool refused_with(const char* text, ManifestError error, std::uint32_t line) {
    const std::optional<ManifestRefused> refused = parse(text);
    if (!refused.has_value()) {
        return false;
    }
    if ((*refused).error != error || (*refused).line != line) {
        std::printf("  got %s at line %u\n", stxpackage::manifest_error_name((*refused).error), (*refused).line);
    }
    return (*refused).error == error && (*refused).line == line;
}

char built[stxpackage::manifest_size_max + 64];

const char* manifest_with(const char* name, const char* version, const char* lines) {
    std::snprintf(built, sizeof(built), "stx-manifest 1\nname %s\nversion %s\n%s", name, version, lines);
    return built;
}

void test_manifest_examples() {
    const char* const test = "manifest examples";
    char lines[512] = {};
    std::snprintf(lines, sizeof(lines), "file 0555 1234 %s bin/radiod\nfile 0444 0 %s share/empty\n", hash_ab, zeros);
    const char* const text = manifest_with("radio", "17", lines);
    check(!parse(text).has_value(), test, "a valid manifest");
    check(manifest.version == 17u && manifest.file_count == 2u && manifest.total_size == 1234u &&
              stxpackage::manifest_name_is(text_of(text), manifest, "radio") &&
              !stxpackage::manifest_name_is(text_of(text), manifest, "radi"),
          test, "version, files, total, name");
    check(manifest.files[0].mode == 0555u && manifest.files[0].sha256[0] == 0xabu &&
              manifest.files[0].sha256[31] == 0x01u && manifest.files[1].size == 0u,
          test, "mode, hash bytes, size 0");
    const std::optional<std::size_t> found = stxpackage::find_manifest_file(text_of(text), manifest, "share/empty", 11);
    check(found.has_value() && *found == 1u, test, "find");
    check(!stxpackage::find_manifest_file(text_of(text), manifest, "bin/radio", 9).has_value(), test,
          "find a prefix: absent");
    std::snprintf(lines, sizeof(lines), "file 0555 1 %s a\n", zeros);
    check(refused_with(manifest_with("Radio", "1", lines), ManifestError::name_malformed, 2), test, "name");
    check(refused_with(manifest_with("radio", "0", lines), ManifestError::version_malformed, 3), test, "version 0");
    check(refused_with(manifest_with("radio", "01", lines), ManifestError::version_malformed, 3), test,
          "leading zero");
    check(refused_with(manifest_with("radio", "4294967296", lines), ManifestError::version_malformed, 3), test,
          "version above 2^32 - 1");
    check(!parse(manifest_with("radio", "4294967295", lines)).has_value(), test, "version 2^32 - 1");
    check(refused_with(manifest_with("radio", "1", ""), ManifestError::no_files, 4), test, "no files");
    std::snprintf(lines, sizeof(lines), "file 0755 1 %s a\n", zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::mode_malformed, 4), test, "mode 0755");
    std::snprintf(lines, sizeof(lines), "file 0444 01 %s a\n", zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::size_malformed, 4), test, "size 01");
    std::snprintf(lines, sizeof(lines), "file 0444 1073741825 %s a\n", zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::file_too_large, 4), test, "2^30 + 1");
    std::snprintf(lines, sizeof(lines), "file 0444 1073741824 %s a\nfile 0444 1073741824 %s b\nfile 0444 1 %s c\n",
                  zeros, zeros, zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::package_too_large, 6), test,
          "above 2^31 in all");
    std::snprintf(lines, sizeof(lines), "file 0444 1 %s a\n", hash_ab);
    lines[16] = 'A';
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::hash_malformed, 4), test,
          "upper-case hex");
    std::snprintf(lines, sizeof(lines), "file 0444 1 %s /a\n", zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::path_malformed, 4), test, "absolute");
    std::snprintf(lines, sizeof(lines), "file 0444 1 %s a/../b\n", zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::path_malformed, 4), test, "..");
    std::snprintf(lines, sizeof(lines), "file 0444 1 %s a b\n", zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::path_malformed, 4), test, "space in path");
    std::snprintf(lines, sizeof(lines), "file 0444  1 %s a\n", zeros);
    check(refused_with(manifest_with("radio", "1", lines), ManifestError::size_malformed, 4), test, "two spaces");
    check(refused_with("stx-manifest 2\nname a\nversion 1\n", ManifestError::header_malformed, 1), test,
          "format 2");
    check(refused_with("stx-manifest 1\r\nname a\nversion 1\n", ManifestError::line_malformed, 1), test, "CR");
    check(refused_with("stx-manifest 1\nname a\nversion 1", ManifestError::line_malformed, 0), test,
          "no final LF");
    check(refused_with("stx-manifest 1\n", ManifestError::name_malformed, 2), test, "header only");
    check(refused_with("", ManifestError::line_malformed, 0), test, "empty");
}

// The order and conflict rules, exhaustive over lists of the 7 paths.
void test_exhaustive_path_lists() {
    const char* const paths[] = {"a", "a-b", "a.b", "a/b", "a/b/c", "a0", "b"};
    constexpr std::size_t path_count = 7;
    std::size_t lists = 0;
    std::size_t accepted = 0;
    std::size_t chosen[4] = {};
    for (std::size_t length = 1; length <= 4u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= path_count;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                chosen[position] = rest % path_count;
                rest /= path_count;
            }
            char lines[1024] = {};
            std::size_t used = 0;
            for (std::size_t position = 0; position < length; position += 1) {
                const int written = std::snprintf(&lines[used], sizeof(lines) - used, "file 0444 1 %s %s\n", zeros,
                                                  paths[chosen[position]]);
                used += static_cast<std::size_t>(written);
            }
            bool expected = true;
            for (std::size_t left = 0; left < length; left += 1) {
                for (std::size_t right = left + 1u; right < length; right += 1) {
                    const char* const earlier = paths[chosen[left]];
                    const char* const later = paths[chosen[right]];
                    const std::size_t earlier_length = std::strlen(earlier);
                    if (right == left + 1u && std::strcmp(earlier, later) >= 0) {
                        expected = false;
                    }
                    if (std::strncmp(earlier, later, earlier_length) == 0 && later[earlier_length] == '/') {
                        expected = false;
                    }
                }
            }
            const bool got = !parse(manifest_with("p", "1", lines)).has_value();
            if (got != expected) {
                check(false, "exhaustive path lists", "as the reference");
                std::printf("  list:");
                for (std::size_t position = 0; position < length; position += 1) {
                    std::printf(" %s", paths[chosen[position]]);
                }
                std::printf("\n");
            }
            accepted += expected ? 1u : 0u;
            lists += 1;
        }
    }
    std::printf("exhaustive path lists: %zu lists, %zu accepted\n", lists, accepted);
    check(lists == 2800u, "exhaustive path lists", "the stated domain was covered");
}

// The canonical spelling of a parsed manifest (the test's own formatter).
std::size_t spell(std::span<const char> text, const Manifest& parsed, char* out, std::size_t capacity) {
    int used = std::snprintf(out, capacity, "stx-manifest 1\nname %.*s\nversion %u\n",
                             static_cast<int>(parsed.name_length), &text[parsed.name_offset], parsed.version);
    for (std::uint32_t index = 0; index < parsed.file_count; index += 1) {
        const stxpackage::ManifestFile& file = parsed.files[index];
        used += std::snprintf(&out[used], capacity - static_cast<std::size_t>(used), "file %s %llu ",
                              file.mode == 0555u ? "0555" : "0444", static_cast<unsigned long long>(file.size));
        for (const std::uint8_t byte : file.sha256) {
            used += std::snprintf(&out[used], capacity - static_cast<std::size_t>(used), "%02x", byte);
        }
        used += std::snprintf(&out[used], capacity - static_cast<std::size_t>(used), " %.*s\n",
                              static_cast<int>(file.path_length), &text[file.path_offset]);
    }
    return static_cast<std::size_t>(used);
}

void test_canonical() {
    char lines[512] = {};
    std::snprintf(lines, sizeof(lines), "file 0555 1234 %s bin/x\nfile 0444 0 %s s/e\n", hash_ab, zeros);
    char original[1024] = {};
    std::snprintf(original, sizeof(original), "%s", manifest_with("ab", "17", lines));
    const std::size_t length = std::strlen(original);
    char changed[1024] = {};
    char spelled[2048] = {};
    std::size_t mutations = 0;
    std::size_t failures = 0;
    for (std::size_t position = 0; position < length; position += 1) {
        for (std::uint32_t byte = 0; byte <= 256u; byte += 1) {
            std::size_t changed_length = length;
            std::memcpy(changed, original, length);
            if (byte == 256u) {
                std::memmove(&changed[position], &changed[position + 1u], length - position - 1u);
                changed_length -= 1;
            } else {
                changed[position] = static_cast<char>(byte);
            }
            const std::span<const char> text(changed, changed_length);
            if (!stxpackage::parse_manifest(text, manifest).has_value()) {
                const std::size_t spelled_length = spell(text, manifest, spelled, sizeof(spelled));
                if (spelled_length != changed_length || std::memcmp(spelled, changed, changed_length) != 0) {
                    failures += 1;
                }
            }
            mutations += 1;
        }
    }
    std::printf("canonical manifest: %zu mutations, %zu not canonical\n", mutations, failures);
    check(failures == 0u, "canonical manifest", "every accepted mutation is canonical");
}

void test_container() {
    const char* const test = "container";
    char lines[256] = {};
    std::snprintf(lines, sizeof(lines), "file 0444 3 %s a\n", zeros);
    const char* const text = manifest_with("p", "1", lines);
    const std::size_t manifest_length = std::strlen(text);
    char package[1024] = {};
    std::size_t used = stxpackage::write_container_prefix(manifest_length, std::span<char>(package, sizeof(package)));
    check(used == 9u + 9u + 3u + 1u, test, "prefix length");
    std::memcpy(&package[used], text, manifest_length);
    used += manifest_length;
    std::memcpy(&package[used], "signature 64\n", 13);
    used += 13;
    std::memset(&package[used], 's', 64);
    used += 64;
    const std::size_t head_length = used;
    std::memcpy(&package[used], "xyz", 3);
    used += 3;
    const stxpackage::ContainerOutcome whole =
        stxpackage::parse_container_head(std::span<const char>(package, used));
    const stxpackage::ContainerHead* const head = whole.as_head();
    check(head != nullptr && (*head).manifest_offset == 22u && (*head).manifest_length == manifest_length &&
              (*head).signature_offset == 22u + manifest_length + 13u && (*head).data_offset == head_length,
          test, "offsets");
    std::size_t truncations = 0;
    for (std::size_t length = 0; length < head_length; length += 1) {
        const stxpackage::ContainerOutcome cut = stxpackage::parse_container_head(std::span<const char>(package, length));
        if (cut.as_refused() == nullptr || *cut.as_refused() != stxpackage::ContainerRefusal::truncated) {
            check(false, test, "every truncation of the head is refused as truncated");
            std::printf("  length %zu\n", length);
        }
        truncations += 1;
    }
    check(truncations == head_length, test, "every truncation tried");
    check(stxpackage::parse_container_head(std::span<const char>(package, head_length)).as_head() != nullptr, test,
          "the head alone");
    package[0] = 'X';
    check(stxpackage::parse_container_head(std::span<const char>(package, used)).as_refused() != nullptr &&
              *stxpackage::parse_container_head(std::span<const char>(package, used)).as_refused() ==
                  stxpackage::ContainerRefusal::magic_malformed,
          test, "magic");
    package[0] = 'S';
    const char* const bad_lengths[] = {"STXPKG 1\nmanifest 0\n", "STXPKG 1\nmanifest 01\n", "STXPKG 1\nmanifest 65537\n",
                                       "STXPKG 1\nmanifest 1x\n", "STXPKG 1\nmanifest \n"};
    for (const char* const bad : bad_lengths) {
        const stxpackage::ContainerOutcome outcome = stxpackage::parse_container_head(text_of(bad));
        check(outcome.as_refused() != nullptr &&
                  *outcome.as_refused() == stxpackage::ContainerRefusal::manifest_line_malformed,
              test, bad);
    }
    // Every manifest length: the prefix the builder writes reads back.
    std::size_t lengths = 0;
    for (std::size_t length = 0; length <= stxpackage::manifest_size_max + 1u; length += 1) {
        char prefix[32] = {};
        const std::size_t written = stxpackage::write_container_prefix(length, std::span<char>(prefix, sizeof(prefix)));
        const bool valid = length >= 1u && length <= stxpackage::manifest_size_max;
        if (valid != (written > 0u)) {
            check(false, test, "the builder writes exactly the valid lengths");
            continue;
        }
        if (valid) {
            const stxpackage::ContainerOutcome outcome =
                stxpackage::parse_container_head(std::span<const char>(prefix, written));
            if (outcome.as_refused() == nullptr ||
                *outcome.as_refused() != stxpackage::ContainerRefusal::truncated) {
                check(false, test, "a written prefix reads as a head cut short after it");
            }
            char number[16] = {};
            std::snprintf(number, sizeof(number), "%zu\n", length);
            if (std::strncmp(&prefix[18], number, std::strlen(number)) != 0) {
                check(false, test, "the length written in decimal");
            }
        }
        lengths += 1;
    }
    check(lengths == stxpackage::manifest_size_max + 2u, test, "every length tried");
}

}  // namespace

int main() {
    test_manifest_examples();
    test_exhaustive_path_lists();
    test_canonical();
    test_container();
    return stxtest::finish("test_manifest");
}
