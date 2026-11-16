// test_http_head.cpp: host tests of the HTTP response head reader.
//
// Examples with their refusal reasons, then three exhaustive runs against
// references written another way:
// - find_head_end over every string of 0 to 12 bytes over CR, LF and 'x'
//   (797,161 strings), against a search for the four bytes;
// - the Content-Length value over every string of 0 to 7 bytes over
//   "0", "1", "9", " ", "\t", ",", "+", "a" (2,396,745 strings), inside a
//   200 response, against "optional blanks, 1-19 digits, optional blanks,
//   at most the limit";
// - one header line over every string of 1 to 6 bytes over "a", ":", " ",
//   "\t", "(", 0x7f, 0x80, 0x01 (299,592 strings), after a valid
//   Content-Length line, against "token, ':', value bytes".

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include "../http_head.h"
#include "../../supervisor/config/test/test_support.h"

namespace {

using stxnet::HeadOutcome;
using stxnet::HeadRefusal;
using stxtest::check;
using stxtest::text_of;

[[nodiscard]] HeadOutcome parse(const char* text, std::uint64_t body_max = 1000000u) {
    return stxnet::parse_response_head(text_of(text), body_max);
}

[[nodiscard]] bool refused_with(const char* text, HeadRefusal reason) {
    const HeadOutcome outcome = parse(text);
    return outcome.as_refused() != nullptr && (*outcome.as_refused()).reason == reason;
}

void test_examples() {
    const char* const test = "head examples";
    const HeadOutcome ok = parse("HTTP/1.1 200 OK\r\nContent-Length: 42\r\nServer: x\r\n\r\n");
    check(ok.as_body() != nullptr && (*ok.as_body()).content_length == 42u, test, "200 with a length");
    check(parse("HTTP/1.0 200\r\ncontent-length:0\r\n\r\n").as_body() != nullptr, test,
          "HTTP/1.0, no reason, lower-case name, no blank");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n").as_body() != nullptr, test,
          "a repeated equal length");
    const char* const redirect_text = "HTTP/1.1 302 Found\r\nLocation:  https://b/x  \r\nContent-Length: 0\r\n\r\n";
    const HeadOutcome redirect = parse(redirect_text);
    check(redirect.as_redirect() != nullptr && (*redirect.as_redirect()).status == 302u &&
              std::strncmp(&redirect_text[(*redirect.as_redirect()).location_offset], "https://b/x",
                           (*redirect.as_redirect()).location_length) == 0 &&
              (*redirect.as_redirect()).location_length == 11u,
          test, "302 with its location, trimmed");
    check(refused_with("HTTP/1.1 301 Moved\r\n\r\n", HeadRefusal::location_missing), test, "301 without location");
    check(refused_with("HTTP/1.1 301 M\r\nLocation: a\r\nLocation: b\r\n\r\n", HeadRefusal::location_repeated), test,
          "two locations");
    check(refused_with("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", HeadRefusal::status_not_accepted),
          test, "404");
    check(parse("HTTP/1.1 404 Not Found\r\n\r\n").as_refused() != nullptr &&
              (*parse("HTTP/1.1 404 Not Found\r\n\r\n").as_refused()).status == 404u,
          test, "the status is reported");
    check(refused_with("HTTP/1.1 200 OK\r\n\r\n", HeadRefusal::length_missing), test, "no length");
    check(refused_with("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
                       HeadRefusal::length_conflict),
          test, "two lengths");
    check(refused_with("HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\n", HeadRefusal::length_malformed), test,
          "a list");
    check(refused_with("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
                       HeadRefusal::transfer_encoding),
          test, "transfer-encoding");
    check(refused_with("HTTP/1.1 200 OK\r\nContent-Length: 1000001\r\n\r\n", HeadRefusal::too_large), test,
          "above the limit");
    check(parse("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n").as_body() != nullptr, test, "at the limit");
    check(refused_with("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n",
                       HeadRefusal::length_malformed),
          test, "20 digits");
    check(refused_with("HTTP/2 200\r\n\r\n", HeadRefusal::status_line_malformed), test, "HTTP/2");
    check(refused_with("HTTP/1.1 20 OK\r\n\r\n", HeadRefusal::status_line_malformed), test, "two digits");
    check(refused_with("HTTP/1.1 200OK\r\n\r\n", HeadRefusal::status_line_malformed), test, "no space");
    check(refused_with("HTTP/1.1 200 OK\n\r\n", HeadRefusal::status_line_malformed), test, "bare LF");
    check(refused_with("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n folded\r\n\r\n", HeadRefusal::header_malformed),
          test, "obs-fold");
    check(refused_with("HTTP/1.1 200 OK\r\nContent-Length : 1\r\n\r\n", HeadRefusal::header_malformed), test,
          "blank before colon");
    check(refused_with("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx", HeadRefusal::header_malformed), test,
          "bytes after the head");
    char many[4096] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n";
    std::size_t used = std::strlen(many);
    for (std::size_t line = 0; line < 100u; line += 1) {
        std::memcpy(&many[used], "X: y\r\n", 6);
        used += 6;
    }
    std::memcpy(&many[used], "\r\n", 3);
    check(refused_with(many, HeadRefusal::too_many_headers), test, "101 lines");
}

void test_exhaustive_head_end() {
    const char alphabet[] = {'\r', '\n', 'x'};
    char bytes[16];
    std::size_t values = 0;
    for (std::size_t length = 0; length <= 12u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= 3u;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                bytes[position] = alphabet[rest % 3u];
                rest /= 3u;
            }
            std::size_t expected = 0;
            for (std::size_t at = 0; at + 4u <= length; at += 1) {
                if (std::memcmp(&bytes[at], "\r\n\r\n", 4) == 0) {
                    expected = at + 4u;
                    break;
                }
            }
            const std::optional<std::size_t> got = stxnet::find_head_end(std::span<const char>(bytes, length));
            const bool same = expected == 0u ? !got.has_value() : (got.has_value() && *got == expected);
            if (!same) {
                check(false, "exhaustive head end", "as the reference");
            }
            values += 1;
        }
    }
    std::printf("exhaustive head end: %zu values\n", values);
    check(values == 797161u, "exhaustive head end", "the stated domain was covered");
}

void test_exhaustive_lengths() {
    const char alphabet[] = {'0', '1', '9', ' ', '\t', ',', '+', 'a'};
    constexpr std::size_t symbols = sizeof(alphabet);
    constexpr std::uint64_t limit = 1000000u;
    const char prefix[] = "HTTP/1.1 200 OK\r\nContent-Length:";
    const std::size_t prefix_length = sizeof(prefix) - 1u;
    char head[64];
    std::memcpy(head, prefix, prefix_length);
    std::size_t values = 0;
    for (std::size_t length = 0; length <= 7u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= symbols;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            char* const value = &head[prefix_length];
            for (std::size_t position = 0; position < length; position += 1) {
                value[position] = alphabet[rest % symbols];
                rest /= symbols;
            }
            std::memcpy(&value[length], "\r\n\r\n", 5);
            // Reference: trim blanks, then digits only, 1..19 of them.
            std::size_t begin = 0;
            std::size_t end = length;
            while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) {
                begin += 1;
            }
            while (end > begin && (value[end - 1u] == ' ' || value[end - 1u] == '\t')) {
                end -= 1;
            }
            bool digits = end > begin && end - begin <= 19u;
            std::uint64_t number = 0;
            for (std::size_t at = begin; digits && at < end; at += 1) {
                digits = value[at] >= '0' && value[at] <= '9';
                number = number * 10u + static_cast<std::uint64_t>(value[at] - '0');
            }
            const HeadOutcome outcome = stxnet::parse_response_head(text_of(head), limit);
            bool same = false;
            if (!digits) {
                same = outcome.as_refused() != nullptr &&
                       (*outcome.as_refused()).reason == HeadRefusal::length_malformed;
            } else if (number > limit) {
                same = outcome.as_refused() != nullptr && (*outcome.as_refused()).reason == HeadRefusal::too_large;
            } else {
                same = outcome.as_body() != nullptr && (*outcome.as_body()).content_length == number;
            }
            if (!same) {
                check(false, "exhaustive lengths", "as the reference");
            }
            values += 1;
        }
    }
    std::printf("exhaustive lengths: %zu values\n", values);
    check(values == 2396745u, "exhaustive lengths", "the stated domain was covered");
}

void test_exhaustive_header_lines() {
    const char alphabet[] = {'a', ':', ' ', '\t', '(', '\x7f', '\x80', '\x01'};
    constexpr std::size_t symbols = sizeof(alphabet);
    const char prefix[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n";
    const std::size_t prefix_length = sizeof(prefix) - 1u;
    char head[64];
    std::memcpy(head, prefix, prefix_length);
    std::size_t values = 0;
    for (std::size_t length = 1; length <= 6u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= symbols;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            char* const line = &head[prefix_length];
            for (std::size_t position = 0; position < length; position += 1) {
                line[position] = alphabet[rest % symbols];
                rest /= symbols;
            }
            std::memcpy(&line[length], "\r\n\r\n", 4);
            const std::size_t head_length = prefix_length + length + 4u;
            // Reference: 1+ 'a' (the only token byte here), ':', then bytes
            // that are tab, or 0x20..0xff but 0x7f.
            std::size_t colon = 0;
            while (colon < length && line[colon] == 'a') {
                colon += 1;
            }
            bool valid = colon > 0u && colon < length && line[colon] == ':';
            for (std::size_t at = colon + 1u; valid && at < length; at += 1) {
                const unsigned char code = static_cast<unsigned char>(line[at]);
                valid = code == 0x09u || (code >= 0x20u && code != 0x7fu);
            }
            const HeadOutcome outcome = stxnet::parse_response_head(std::span<const char>(head, head_length), 100u);
            const bool same = valid ? outcome.as_body() != nullptr
                                    : (outcome.as_refused() != nullptr &&
                                       (*outcome.as_refused()).reason == HeadRefusal::header_malformed);
            if (!same) {
                check(false, "exhaustive header lines", "as the reference");
            }
            values += 1;
        }
    }
    std::printf("exhaustive header lines: %zu values\n", values);
    check(values == 299592u, "exhaustive header lines", "the stated domain was covered");
}

}  // namespace

int main() {
    test_examples();
    test_exhaustive_head_end();
    test_exhaustive_lengths();
    test_exhaustive_header_lines();
    return stxtest::finish("test_http_head");
}
