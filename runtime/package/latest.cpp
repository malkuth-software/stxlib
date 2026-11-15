// latest.cpp: see latest.h.

#include "latest.h"

namespace stxpackage {

LatestOutcome::LatestOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

LatestOutcome LatestOutcome::parsed(const Latest& value) noexcept {
    return LatestOutcome(Alternatives(value));
}

LatestOutcome LatestOutcome::refused(LatestRefusal reason) noexcept {
    return LatestOutcome(Alternatives(reason));
}

const Latest* LatestOutcome::as_parsed() const noexcept {
    return std::get_if<Latest>(&alternatives_);
}

const LatestRefusal* LatestOutcome::as_refused() const noexcept {
    return std::get_if<LatestRefusal>(&alternatives_);
}

const char* latest_refusal_name(LatestRefusal reason) noexcept {
    switch (reason) {
    case LatestRefusal::too_large: return "too_large";
    case LatestRefusal::header_malformed: return "header_malformed";
    case LatestRefusal::version_malformed: return "version_malformed";
    case LatestRefusal::url_malformed: return "url_malformed";
    case LatestRefusal::trailing_bytes: return "trailing_bytes";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

// The line starting at begin with its prefix: the value's [begin, end) and
// the next line's start, or false (no prefix, no LF).
[[nodiscard]] bool line_value(std::span<const char> text, std::size_t begin, const char* prefix,
                              std::size_t& value_begin, std::size_t& value_end) noexcept {
    std::size_t index = 0;
    while (prefix[index] != '\0') {
        if (begin + index >= text.size() || text[begin + index] != prefix[index]) {
            return false;
        }
        index += 1;
    }
    value_begin = begin + index;
    std::size_t end = value_begin;
    while (end < text.size() && text[end] != '\n') {
        end += 1;
    }
    if (end == text.size()) {
        return false;
    }
    value_end = end;
    return true;
}

}  // namespace

LatestOutcome parse_latest(std::span<const char> text) noexcept {
    if (text.size() > latest_size_max) {
        return LatestOutcome::refused(LatestRefusal::too_large);
    }
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    if (!line_value(text, 0, "stx-latest 1", value_begin, value_end) || value_end != value_begin) {
        return LatestOutcome::refused(LatestRefusal::header_malformed);
    }
    if (!line_value(text, value_end + 1u, "version ", value_begin, value_end)) {
        return LatestOutcome::refused(LatestRefusal::version_malformed);
    }
    const std::size_t digits = value_end - value_begin;
    if (digits == 0u || digits > 10u || text[value_begin] == '0') {
        return LatestOutcome::refused(LatestRefusal::version_malformed);
    }
    std::uint64_t version = 0;
    for (std::size_t index = value_begin; index < value_end; index += 1) {
        if (text[index] < '0' || text[index] > '9') {
            return LatestOutcome::refused(LatestRefusal::version_malformed);
        }
        version = version * 10u + static_cast<std::uint64_t>(text[index] - '0');
    }
    if (version > UINT32_MAX) {
        return LatestOutcome::refused(LatestRefusal::version_malformed);
    }
    if (!line_value(text, value_end + 1u, "url ", value_begin, value_end)) {
        return LatestOutcome::refused(LatestRefusal::url_malformed);
    }
    const stxnet::UrlOutcome url = stxnet::parse_https_url(text.subspan(value_begin, value_end - value_begin));
    const stxnet::HttpsUrl* const parsed = url.as_parsed();
    if (parsed == nullptr) {
        return LatestOutcome::refused(LatestRefusal::url_malformed);
    }
    if (value_end + 1u != text.size()) {
        return LatestOutcome::refused(LatestRefusal::trailing_bytes);
    }
    Latest latest{};
    latest.version = static_cast<std::uint32_t>(version);
    latest.url = *parsed;
    return LatestOutcome::parsed(latest);
}

}  // namespace stxpackage
