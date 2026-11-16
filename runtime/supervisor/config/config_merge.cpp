// config_merge.cpp: the configuration a boot runs (config_merge.h).

#include "config_merge.h"

namespace stxconfig {

namespace {

struct LineSpan {
    std::size_t begin;  // first non-blank byte
    std::size_t end;    // before the line feed (or the text's end)
};

enum class LineKind : std::uint8_t {
    skip,     // blank or comment
    header,
    content,  // a setting or an init command
};

[[nodiscard]] bool is_blank(char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\r';
}

// Reads the line at *position and moves past it; nothing at the end.
[[nodiscard]] std::optional<LineSpan> next_line(std::span<const char> text, std::size_t& position) noexcept {
    if (position >= text.size()) {
        return std::nullopt;
    }
    std::size_t end = position;
    while (end < text.size() && text[end] != '\n') {
        end += 1;
    }
    std::size_t begin = position;
    while (begin < end && is_blank(text[begin])) {
        begin += 1;
    }
    std::size_t trimmed = end;
    while (trimmed > begin && is_blank(text[trimmed - 1u])) {
        trimmed -= 1;
    }
    position = end < text.size() ? end + 1u : end;
    const std::optional<LineSpan> line = LineSpan{begin, trimmed};
    return line;
}

[[nodiscard]] LineKind kind_of(std::span<const char> text, const LineSpan& line) noexcept {
    if (line.begin == line.end || text[line.begin] == '#') {
        return LineKind::skip;
    }
    return text[line.begin] == '[' ? LineKind::header : LineKind::content;
}

// A header's name: the bytes between '[' and ']'.
[[nodiscard]] std::span<const char> header_name(std::span<const char> text, const LineSpan& line) noexcept {
    std::size_t close = line.begin + 1u;
    while (close < line.end && text[close] != ']') {
        close += 1;
    }
    return text.subspan(line.begin + 1u, close - line.begin - 1u);
}

// A setting's name: up to the first blank or '='.
[[nodiscard]] std::span<const char> setting_name(std::span<const char> text, const LineSpan& line) noexcept {
    std::size_t end = line.begin;
    while (end < line.end && !is_blank(text[end]) && text[end] != '=') {
        end += 1;
    }
    return text.subspan(line.begin, end - line.begin);
}

[[nodiscard]] bool bytes_equal(std::span<const char> left, std::span<const char> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); index += 1) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::span<const char> c_text(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return std::span<const char>(text, length);
}

[[nodiscard]] bool starts_with(std::span<const char> bytes, const char* prefix) noexcept {
    const std::span<const char> head = c_text(prefix);
    return bytes.size() >= head.size() && bytes_equal(bytes.first(head.size()), head);
}

[[nodiscard]] bool has_section(std::span<const char> text, std::span<const char> name) noexcept {
    std::size_t position = 0;
    for (std::optional<LineSpan> line = next_line(text, position); line.has_value();
         line = next_line(text, position)) {
        if (kind_of(text, *line) == LineKind::header && bytes_equal(header_name(text, *line), name)) {
            return true;
        }
    }
    return false;
}

// Whether text's section sets a setting of that name.
[[nodiscard]] bool section_sets(std::span<const char> text, std::span<const char> section,
                                std::span<const char> name) noexcept {
    std::size_t position = 0;
    bool inside = false;
    for (std::optional<LineSpan> line = next_line(text, position); line.has_value();
         line = next_line(text, position)) {
        const LineKind kind = kind_of(text, *line);
        if (kind == LineKind::header) {
            inside = bytes_equal(header_name(text, *line), section);
        } else if (kind == LineKind::content && inside && bytes_equal(setting_name(text, *line), name)) {
            return true;
        }
    }
    return false;
}

class Writer {
public:
    explicit Writer(std::span<char> out) noexcept : out_(out), used_(0), fits_(true) {}

    void bytes(std::span<const char> part) noexcept {
        if (!fits_ || part.size() > out_.size() - used_) {
            fits_ = false;
            return;
        }
        for (std::size_t index = 0; index < part.size(); index += 1) {
            out_[used_ + index] = part[index];
        }
        used_ += part.size();
    }
    void line(std::span<const char> text, const LineSpan& span) noexcept {
        bytes(text.subspan(span.begin, span.end - span.begin));
        bytes(c_text("\n"));
    }
    void header(std::span<const char> name) noexcept {
        bytes(c_text("["));
        bytes(name);
        bytes(c_text("]\n"));
    }
    [[nodiscard]] std::optional<std::size_t> result() const noexcept {
        if (!fits_) {
            return std::nullopt;
        }
        const std::optional<std::size_t> size = used_;
        return size;
    }

private:
    std::span<char> out_;
    std::size_t used_;
    bool fits_;
};

// Every content line of text's section, in order.
void write_body(Writer& writer, std::span<const char> text, std::span<const char> section) noexcept {
    std::size_t position = 0;
    bool inside = false;
    for (std::optional<LineSpan> line = next_line(text, position); line.has_value();
         line = next_line(text, position)) {
        const LineKind kind = kind_of(text, *line);
        if (kind == LineKind::header) {
            inside = bytes_equal(header_name(text, *line), section);
        } else if (kind == LineKind::content && inside) {
            writer.line(text, *line);
        }
    }
}

// Whether the user's [init] starts the target of that name.
[[nodiscard]] bool user_starts(const BootConfig& user, std::span<const char> name) noexcept {
    for (std::size_t index = 0; index < user.init_command_count(); index += 1) {
        const StartCommand* const start = user.init_command(index).action.as_start();
        if (start != nullptr && bytes_equal(c_text(user.text((*start).service)), name)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_replaced_section(std::span<const char> name) noexcept {
    return bytes_equal(name, c_text(user_section_network)) || bytes_equal(name, c_text(user_section_access));
}

void write_start_setting(Writer& writer, const BootConfig& user, std::span<const char> section) noexcept {
    const std::span<const char> prefix = c_text(starts_with(section, service_section_prefix) ? service_section_prefix
                                                                                              : target_section_prefix);
    const std::span<const char> name = section.subspan(prefix.size());
    writer.bytes(c_text(user_starts(user, name) ? "start = boot\n" : "start = manual\n"));
}

}  // namespace

std::optional<std::size_t> merge_config_text(std::span<const char> factory_text, std::span<const char> user_text,
                                             const BootConfig& user, std::span<char> out) noexcept {
    Writer writer(out);
    const std::span<const char> system = c_text(user_section_system);
    const bool user_has_system = has_section(user_text, system);
    // The factory's lines.
    std::size_t position = 0;
    std::span<const char> current;
    for (std::optional<LineSpan> line = next_line(factory_text, position); line.has_value();
         line = next_line(factory_text, position)) {
        const LineKind kind = kind_of(factory_text, *line);
        if (kind == LineKind::skip) {
            continue;
        }
        if (kind == LineKind::header) {
            current = header_name(factory_text, *line);
            writer.line(factory_text, *line);
            if (bytes_equal(current, system) && user_has_system) {
                write_body(writer, user_text, system);
            } else if (is_replaced_section(current) && has_section(user_text, current)) {
                write_body(writer, user_text, current);
            }
            continue;
        }
        if (bytes_equal(current, system) && user_has_system &&
            section_sets(user_text, system, setting_name(factory_text, *line))) {
            continue;
        }
        if (is_replaced_section(current) && has_section(user_text, current)) {
            continue;
        }
        writer.line(factory_text, *line);
    }
    // The user's machine sections the factory does not have.
    const char* const machine_sections[] = {user_section_system, user_section_network, user_section_access};
    for (const char* const section : machine_sections) {
        const std::span<const char> name = c_text(section);
        if (!has_section(factory_text, name) && has_section(user_text, name)) {
            writer.header(name);
            write_body(writer, user_text, name);
        }
    }
    // The user's targets, each with its start setting.
    position = 0;
    std::optional<std::span<const char>> target;
    for (std::optional<LineSpan> line = next_line(user_text, position); line.has_value();
         line = next_line(user_text, position)) {
        const LineKind kind = kind_of(user_text, *line);
        if (kind == LineKind::header) {
            if (target.has_value()) {
                write_start_setting(writer, user, *target);
            }
            const std::span<const char> name = header_name(user_text, *line);
            target = std::nullopt;
            if (starts_with(name, service_section_prefix) || starts_with(name, target_section_prefix)) {
                target = name;
                writer.line(user_text, *line);
            }
            continue;
        }
        if (kind == LineKind::content && target.has_value()) {
            writer.line(user_text, *line);
        }
    }
    if (target.has_value()) {
        write_start_setting(writer, user, *target);
    }
    return writer.result();
}

const char* turn_refusal_name(TurnRefusal refusal) noexcept {
    switch (refusal) {
    case TurnRefusal::none: return "none";
    case TurnRefusal::parse: return "parse";
    case TurnRefusal::rule: return "rule";
    case TurnRefusal::too_large: return "too-large";
    case TurnRefusal::merged_parse: return "merged-parse";
    case TurnRefusal::id_missing: return "id-missing";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

std::optional<BootChoice> choose_boot_config(std::span<const char> factory_text, const BootConfig& factory,
                                             const std::optional<stxstore::TurnView> (&turns)[2],
                                             const stxstore::TurnChoice& choice, BootConfig& user_work,
                                             BootConfig& effective, std::span<char> out) noexcept {
    BootChoice result{};
    result.file_index = std::nullopt;
    result.sequence = 0;
    result.passed_over_count = 0;
    const std::optional<std::size_t> order[2] = {choice.newest, choice.older};
    const std::span<char> merged = out.size() > config_size_max ? out.first(config_size_max) : out;
    for (const std::optional<std::size_t>& candidate : order) {
        if (!candidate.has_value() || *candidate > 1u || !turns[*candidate].has_value()) {
            continue;
        }
        const stxstore::TurnView& turn = *turns[*candidate];
        TurnTried tried{};
        tried.file_index = *candidate;
        tried.sequence = turn.sequence;
        tried.refusal = TurnRefusal::none;
        const ParseOutcome user_parsed = parse_boot_config(turn.body, user_work);
        const ParseFailed* const user_failure = user_parsed.as_failure();
        if (user_failure != nullptr) {
            tried.refusal = TurnRefusal::parse;
            tried.code = static_cast<std::uint8_t>((*user_failure).error);
            tried.line = (*user_failure).line;
            tried.column = (*user_failure).column;
        } else {
            const std::optional<UserRuleBroken> broken = check_user_config(user_work, factory);
            if (broken.has_value()) {
                tried.refusal = TurnRefusal::rule;
                tried.code = static_cast<std::uint8_t>((*broken).rule);
                tried.line = (*broken).line;
                tried.column = (*broken).column;
            }
        }
        // Every target the boot runs has its id (the supervisor issued it).
        for (std::size_t index = 0; tried.refusal == TurnRefusal::none && index < user_work.service_count();
             index += 1) {
            const ServiceDefinition& service = user_work.service(index);
            if (service.target.has_value() && (*service.target).id == 0u) {
                tried.refusal = TurnRefusal::id_missing;
                tried.line = service.line;
                tried.column = 1;
            }
        }
        std::optional<std::size_t> length = std::nullopt;
        if (tried.refusal == TurnRefusal::none) {
            length = merge_config_text(factory_text, turn.body, user_work, merged);
            if (!length.has_value()) {
                tried.refusal = TurnRefusal::too_large;
            }
        }
        if (tried.refusal == TurnRefusal::none) {
            const ParseOutcome merged_parsed =
                parse_boot_config(std::span<const char>(merged.data(), *length), effective);
            const ParseFailed* const merged_failure = merged_parsed.as_failure();
            if (merged_failure == nullptr) {
                result.file_index = *candidate;
                result.sequence = turn.sequence;
                result.text_length = *length;
                const std::optional<BootChoice> chosen = result;
                return chosen;
            }
            tried.refusal = TurnRefusal::merged_parse;
            tried.code = static_cast<std::uint8_t>((*merged_failure).error);
            tried.line = (*merged_failure).line;
            tried.column = (*merged_failure).column;
        }
        result.passed_over[result.passed_over_count] = tried;
        result.passed_over_count += 1;
    }
    // The factory configuration alone.
    if (factory_text.size() > out.size()) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < factory_text.size(); index += 1) {
        out[index] = factory_text[index];
    }
    if (parse_boot_config(std::span<const char>(out.data(), factory_text.size()), effective).as_failure() != nullptr) {
        return std::nullopt;
    }
    result.text_length = factory_text.size();
    const std::optional<BootChoice> fallback = result;
    return fallback;
}

}  // namespace stxconfig
