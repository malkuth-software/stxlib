// test_latest.cpp: host tests of the latest document.
//
// Examples with their refusals, then exhaustive over the version line:
// every string of 0 to 8 bytes over "0", "1", "4", "9", " ", "a" in
// "version <value>" (2,015,539 strings), against "1 to 10 digits, no
// leading zero, at most 4294967295".

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../latest.h"
#include "../../supervisor/config/test/test_support.h"

namespace {

using stxpackage::LatestOutcome;
using stxpackage::LatestRefusal;
using stxtest::check;
using stxtest::same_text;
using stxtest::text_of;

[[nodiscard]] bool refused_with(const char* text, LatestRefusal reason) {
    const LatestOutcome outcome = stxpackage::parse_latest(text_of(text));
    return outcome.as_refused() != nullptr && *outcome.as_refused() == reason;
}

void test_examples() {
    const char* const test = "latest examples";
    const LatestOutcome good = stxpackage::parse_latest(text_of("stx-latest 1\nversion 42\nurl https://u.example/p/42.stxpkg\n"));
    check(good.as_parsed() != nullptr && (*good.as_parsed()).version == 42u &&
              same_text((*good.as_parsed()).url.host, "u.example") &&
              same_text((*good.as_parsed()).url.path, "/p/42.stxpkg"),
          test, "version and url");
    check(refused_with("stx-latest 2\nversion 1\nurl https://a/\n", LatestRefusal::header_malformed), test,
          "format 2");
    check(refused_with("stx-latest 1 \nversion 1\nurl https://a/\n", LatestRefusal::header_malformed), test,
          "header with a blank");
    check(refused_with("stx-latest 1\nversion 1\nurl http://a/\n", LatestRefusal::url_malformed), test, "http");
    check(refused_with("stx-latest 1\nversion 1\nurl https://a/", LatestRefusal::url_malformed), test,
          "no final LF");
    check(refused_with("stx-latest 1\nversion 1\nurl https://a/\n\n", LatestRefusal::trailing_bytes), test,
          "a blank line after");
    check(refused_with("stx-latest 1\nurl https://a/\nversion 1\n", LatestRefusal::version_malformed), test,
          "lines out of order");
    char large[1100] = {};
    std::memcpy(large, "stx-latest 1\nversion 1\nurl https://a/", 37);
    std::memset(&large[37], 'x', 1000);
    large[1037] = '\n';
    check(refused_with(large, LatestRefusal::too_large), test, "above 1024 bytes");
}

void test_exhaustive_versions() {
    const char alphabet[] = {'0', '1', '4', '9', ' ', 'a'};
    constexpr std::size_t symbols = sizeof(alphabet);
    char text[64] = "stx-latest 1\nversion ";
    const std::size_t prefix_length = std::strlen(text);
    std::size_t values = 0;
    for (std::size_t length = 0; length <= 8u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= symbols;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                text[prefix_length + position] = alphabet[rest % symbols];
                rest /= symbols;
            }
            std::memcpy(&text[prefix_length + length], "\nurl https://a/\n", 17);
            bool expected = length >= 1u && length <= 10u && text[prefix_length] != '0';
            std::uint64_t number = 0;
            for (std::size_t position = 0; expected && position < length; position += 1) {
                const char byte = text[prefix_length + position];
                expected = byte >= '0' && byte <= '9';
                number = number * 10u + static_cast<std::uint64_t>(byte - '0');
            }
            expected = expected && number <= 4294967295u;
            const LatestOutcome outcome = stxpackage::parse_latest(text_of(text));
            const bool same = expected ? (outcome.as_parsed() != nullptr && (*outcome.as_parsed()).version == number)
                                       : (outcome.as_refused() != nullptr &&
                                          *outcome.as_refused() == LatestRefusal::version_malformed);
            if (!same) {
                check(false, "exhaustive versions", "as the reference");
            }
            values += 1;
        }
    }
    std::printf("exhaustive versions: %zu values\n", values);
    check(values == 2015539u, "exhaustive versions", "the stated domain was covered");
}

}  // namespace

int main() {
    test_examples();
    test_exhaustive_versions();
    return stxtest::finish("test_latest");
}
