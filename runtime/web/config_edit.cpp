// config_edit.cpp: line edits on a configuration's text (config_edit.h).

#include "config_edit.h"

namespace stxweb {

namespace {

[[nodiscard]] bool is_blank(char byte) noexcept {
    return byte == ' ' || byte == '\t';
}

[[nodiscard]] bool is_name_start(char byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}

[[nodiscard]] bool is_name_byte(char byte) noexcept {
    return is_name_start(byte) || (byte >= '0' && byte <= '9') || byte == '-';
}

// One line: [begin, content_end) without its line end; next is where the
// following line begins (the text's end for the last line).
struct Line {
    std::size_t begin;
    std::size_t content_end;
    std::size_t next;
};

[[nodiscard]] Line line_at(std::span<const char> text, std::size_t begin) noexcept {
    Line line{};
    line.begin = begin;
    std::size_t index = begin;
    while (index < text.size() && text[index] != '\n') {
        index += 1;
    }
    line.next = index < text.size() ? index + 1u : index;
    // A CR before the LF (or at the text's end) is part of the line end.
    if (index > begin && text[index - 1u] == '\r') {
        index -= 1u;
    }
    line.content_end = index;
    return line;
}

[[nodiscard]] std::size_t skip_blanks(std::span<const char> text, std::size_t index, std::size_t end) noexcept {
    while (index < end && is_blank(text[index])) {
        index += 1;
    }
    return index;
}

// The name at index (a run of name bytes starting with a letter); its end.
[[nodiscard]] std::size_t name_end(std::span<const char> text, std::size_t index, std::size_t end) noexcept {
    if (index >= end || !is_name_start(text[index])) {
        return index;
    }
    while (index < end && is_name_byte(text[index])) {
        index += 1;
    }
    return index;
}

[[nodiscard]] bool range_equals(std::span<const char> text, std::size_t begin, std::size_t end,
                                std::span<const char> expected) noexcept {
    if (end - begin != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); index += 1) {
        if (text[begin + index] != expected[index]) {
            return false;
        }
    }
    return true;
}

// The header's name range when the line is a header line.
[[nodiscard]] bool header_name(std::span<const char> text, const Line& line, std::size_t& begin,
                               std::size_t& end) noexcept {
    std::size_t index = skip_blanks(text, line.begin, line.content_end);
    if (index >= line.content_end || text[index] != '[') {
        return false;
    }
    begin = index + 1u;
    end = name_end(text, begin, line.content_end);
    if (end == begin || end >= line.content_end || text[end] != ']') {
        return false;
    }
    index = skip_blanks(text, end + 1u, line.content_end);
    return index == line.content_end;
}

// The setting's name range when the line is a setting line.
[[nodiscard]] bool setting_name(std::span<const char> text, const Line& line, std::size_t& begin,
                                std::size_t& end) noexcept {
    begin = skip_blanks(text, line.begin, line.content_end);
    end = name_end(text, begin, line.content_end);
    if (end == begin) {
        return false;
    }
    const std::size_t index = skip_blanks(text, end, line.content_end);
    return index < line.content_end && text[index] == '=';
}

// Whether the line is "start <service>" (bare words, blanks around).
[[nodiscard]] bool is_start_line(std::span<const char> text, const Line& line, std::span<const char> service) noexcept {
    std::size_t index = skip_blanks(text, line.begin, line.content_end);
    const std::size_t keyword_end = name_end(text, index, line.content_end);
    if (!range_equals(text, index, keyword_end, std::span<const char>("start", 5))) {
        return false;
    }
    index = skip_blanks(text, keyword_end, line.content_end);
    if (index == keyword_end) {
        return false;
    }
    const std::size_t service_end = name_end(text, index, line.content_end);
    if (!range_equals(text, index, service_end, service)) {
        return false;
    }
    return skip_blanks(text, service_end, line.content_end) == line.content_end;
}

[[nodiscard]] bool is_blank_or_comment(std::span<const char> text, const Line& line) noexcept {
    const std::size_t index = skip_blanks(text, line.begin, line.content_end);
    return index == line.content_end || text[index] == '#';
}

// Builds "name = value\n" into out; returns its length (fits: callers
// check config_value_valid first).
[[nodiscard]] std::size_t setting_line_text(std::span<const char> name, std::span<const char> value,
                                            std::span<char> out) noexcept {
    std::size_t size = 0;
    for (const char byte : name) {
        out[size] = byte;
        size += 1;
    }
    out[size] = ' ';
    out[size + 1u] = '=';
    size += 2;
    if (!value.empty()) {
        out[size] = ' ';
        size += 1;
    }
    for (const char byte : value) {
        out[size] = byte;
        size += 1;
    }
    out[size] = '\n';
    size += 1;
    return size;
}

}  // namespace

const char* edit_result_name(EditResult result) noexcept {
    switch (result) {
    case EditResult::done: return "done";
    case EditResult::too_large: return "config_too_large";
    case EditResult::name_not_allowed: return "name_not_allowed";
    case EditResult::value_not_allowed: return "value_not_allowed";
    case EditResult::too_many_settings: return "too_many_settings";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

bool config_name_valid(std::span<const char> name) noexcept {
    if (name.empty() || name.size() > stxconfig::name_length_max || !is_name_start(name[0])) {
        return false;
    }
    for (const char byte : name) {
        if (!is_name_byte(byte)) {
            return false;
        }
    }
    return true;
}

bool config_value_valid(std::span<const char> name, std::span<const char> value) noexcept {
    // "name = value": the name, " =", and " value" when there is one.
    const std::size_t line_length = name.size() + 2u + (value.empty() ? 0u : 1u + value.size());
    if (line_length > edit_line_bytes_max) {
        return false;
    }
    if (!value.empty() && (is_blank(value[0]) || is_blank(value[value.size() - 1u]))) {
        return false;
    }
    for (const char byte : value) {
        const bool printable = byte >= 0x20 && byte <= 0x7e;
        if (!printable && byte != '\t') {
            return false;
        }
    }
    return true;
}

ConfigEditor::ConfigEditor() noexcept : text_{}, size_(0), scratch_{} {}

bool ConfigEditor::load(std::span<const char> text) noexcept {
    // A text whose last line has no line end gets one, so every line the
    // editor finds ends in LF; the parser reads both alike.
    const bool needs_end = !text.empty() && text[text.size() - 1u] != '\n';
    if (text.size() + (needs_end ? 1u : 0u) > edit_text_bytes_max) {
        size_ = 0;
        return false;
    }
    for (std::size_t index = 0; index < text.size(); index += 1) {
        text_[index] = text[index];
    }
    size_ = text.size();
    if (needs_end) {
        text_[size_] = '\n';
        size_ += 1;
    }
    return true;
}

std::span<const char> ConfigEditor::text() const noexcept {
    return std::span<const char>(text_, size_);
}

std::optional<ConfigEditor::SectionLines> ConfigEditor::find_section(std::span<const char> section) const noexcept {
    const std::span<const char> all = text();
    std::optional<SectionLines> found;
    std::size_t offset = 0;
    // Each line advances offset by at least one byte.
    while (offset < all.size()) {
        const Line line = line_at(all, offset);
        std::size_t begin = 0;
        std::size_t end = 0;
        if (header_name(all, line, begin, end)) {
            if (found.has_value()) {
                (*found).end = line.begin;
                return found;
            }
            if (range_equals(all, begin, end, section)) {
                SectionLines lines{};
                lines.header = line.begin;
                lines.body = line.next;
                lines.end = all.size();
                found = lines;
            }
        }
        offset = line.next;
    }
    return found;
}

std::size_t ConfigEditor::section_content_end(const SectionLines& lines) const noexcept {
    const std::span<const char> all = text();
    std::size_t content_end = lines.body;
    std::size_t offset = lines.body;
    while (offset < lines.end) {
        const Line line = line_at(all, offset);
        if (!is_blank_or_comment(all, line)) {
            content_end = line.next;
        }
        offset = line.next;
    }
    return content_end;
}

EditResult ConfigEditor::splice(std::size_t begin, std::size_t end, std::span<const char> insertion) noexcept {
    const std::size_t kept_after = size_ - end;
    const std::size_t new_size = begin + insertion.size() + kept_after;
    if (new_size > edit_text_bytes_max) {
        return EditResult::too_large;
    }
    // Through the scratch buffer, so overlap does not matter.
    for (std::size_t index = 0; index < kept_after; index += 1) {
        scratch_[index] = text_[end + index];
    }
    for (std::size_t index = 0; index < insertion.size(); index += 1) {
        text_[begin + index] = insertion[index];
    }
    for (std::size_t index = 0; index < kept_after; index += 1) {
        text_[begin + insertion.size() + index] = scratch_[index];
    }
    size_ = new_size;
    return EditResult::done;
}

std::optional<ConfigEditor::SectionLines> ConfigEditor::append_section(std::span<const char> section) noexcept {
    // "\n" (when the last line has no end) + "\n" (a blank line, when the
    // text is not empty) + "[" name "]\n".
    char header[stxconfig::name_length_max + 5];
    std::size_t size = 0;
    if (size_ > 0u && text_[size_ - 1u] != '\n') {
        header[size] = '\n';
        size += 1;
    }
    if (size_ > 0u) {
        header[size] = '\n';
        size += 1;
    }
    const std::size_t header_start = size_ + size;
    header[size] = '[';
    size += 1;
    for (const char byte : section) {
        header[size] = byte;
        size += 1;
    }
    header[size] = ']';
    header[size + 1u] = '\n';
    size += 2;
    if (splice(size_, size_, std::span<const char>(header, size)) != EditResult::done) {
        return std::nullopt;
    }
    SectionLines lines{};
    lines.header = header_start;
    lines.body = size_;
    lines.end = size_;
    const std::optional<SectionLines> found = lines;
    return found;
}

EditResult ConfigEditor::set_setting(std::span<const char> section, std::span<const char> name,
                                     std::span<const char> value) noexcept {
    if (!config_name_valid(section) || !config_name_valid(name)) {
        return EditResult::name_not_allowed;
    }
    if (!config_value_valid(name, value)) {
        return EditResult::value_not_allowed;
    }
    char line_text[edit_line_bytes_max + 1];
    const std::size_t line_size = setting_line_text(name, value, line_text);
    const std::span<const char> insertion(line_text, line_size);
    std::optional<SectionLines> lines = find_section(section);
    if (!lines.has_value()) {
        const std::size_t saved_size = size_;
        lines = append_section(section);
        if (!lines.has_value()) {
            return EditResult::too_large;
        }
        const EditResult result = splice((*lines).body, (*lines).body, insertion);
        if (result != EditResult::done) {
            size_ = saved_size;
        }
        return result;
    }
    const std::span<const char> all = text();
    std::size_t offset = (*lines).body;
    while (offset < (*lines).end) {
        const Line line = line_at(all, offset);
        std::size_t begin = 0;
        std::size_t end = 0;
        if (setting_name(all, line, begin, end) && range_equals(all, begin, end, name)) {
            // Keep the line's own end: a line without one (the text's last)
            // gets none; the insertion's LF is dropped then.
            const bool had_end = line.next > line.content_end;
            const std::span<const char> replacement = had_end ? insertion : insertion.subspan(0, line_size - 1u);
            return splice(line.begin, had_end ? line.next : line.content_end, replacement);
        }
        offset = line.next;
    }
    const std::size_t at = section_content_end(*lines);
    // The section's last line may lack a line end (the text's end).
    if (at > 0u && at == size_ && text_[at - 1u] != '\n') {
        char with_end[edit_line_bytes_max + 2];
        with_end[0] = '\n';
        for (std::size_t index = 0; index < line_size; index += 1) {
            with_end[index + 1u] = line_text[index];
        }
        return splice(at, at, std::span<const char>(with_end, line_size + 1u));
    }
    return splice(at, at, insertion);
}

EditResult ConfigEditor::remove_setting(std::span<const char> section, std::span<const char> name) noexcept {
    if (!config_name_valid(section) || !config_name_valid(name)) {
        return EditResult::name_not_allowed;
    }
    const std::optional<SectionLines> lines = find_section(section);
    if (!lines.has_value()) {
        return EditResult::done;
    }
    const std::span<const char> all = text();
    std::size_t offset = (*lines).body;
    while (offset < (*lines).end) {
        const Line line = line_at(all, offset);
        std::size_t begin = 0;
        std::size_t end = 0;
        if (setting_name(all, line, begin, end) && range_equals(all, begin, end, name)) {
            return splice(line.begin, line.next, std::span<const char>());
        }
        offset = line.next;
    }
    return EditResult::done;
}

EditResult ConfigEditor::remove_section(std::span<const char> section) noexcept {
    if (!config_name_valid(section)) {
        return EditResult::name_not_allowed;
    }
    const std::optional<SectionLines> lines = find_section(section);
    if (!lines.has_value()) {
        return EditResult::done;
    }
    return splice((*lines).header, (*lines).end, std::span<const char>());
}

EditResult ConfigEditor::replace_section(std::span<const char> section, std::span<const EditSetting> settings) noexcept {
    if (!config_name_valid(section)) {
        return EditResult::name_not_allowed;
    }
    if (settings.size() > edit_section_settings_max) {
        return EditResult::too_many_settings;
    }
    for (const EditSetting& setting : settings) {
        if (!config_name_valid(setting.name)) {
            return EditResult::name_not_allowed;
        }
        if (!config_value_valid(setting.name, setting.value)) {
            return EditResult::value_not_allowed;
        }
    }
    // The new body, built in a local buffer: at most 16 lines of 256 bytes.
    char body[edit_section_settings_max * (edit_line_bytes_max + 1u) + 1u];
    std::size_t body_size = 0;
    for (const EditSetting& setting : settings) {
        body_size += setting_line_text(setting.name, setting.value, std::span<char>(body + body_size, sizeof(body) - body_size));
    }
    const std::size_t saved_size = size_;
    std::optional<SectionLines> lines = find_section(section);
    if (!lines.has_value()) {
        lines = append_section(section);
        if (!lines.has_value()) {
            return EditResult::too_large;
        }
    }
    // A following section keeps a blank line before its header.
    const bool another_follows = (*lines).end < size_;
    if (another_follows && body_size < sizeof(body)) {
        body[body_size] = '\n';
        body_size += 1;
    }
    const EditResult result = splice((*lines).body, (*lines).end, std::span<const char>(body, body_size));
    if (result != EditResult::done) {
        size_ = saved_size < size_ ? saved_size : size_;
    }
    return result;
}

bool ConfigEditor::starts(std::span<const char> service) const noexcept {
    const std::optional<SectionLines> lines = find_section(std::span<const char>("init", 4));
    if (!lines.has_value()) {
        return false;
    }
    const std::span<const char> all = text();
    std::size_t offset = (*lines).body;
    while (offset < (*lines).end) {
        const Line line = line_at(all, offset);
        if (is_start_line(all, line, service)) {
            return true;
        }
        offset = line.next;
    }
    return false;
}

EditResult ConfigEditor::set_start(std::span<const char> service, bool enabled) noexcept {
    if (!config_name_valid(service)) {
        return EditResult::name_not_allowed;
    }
    const std::span<const char> init("init", 4);
    std::optional<SectionLines> lines = find_section(init);
    if (!enabled) {
        // Remove every such line; each removal shortens the section, and
        // there are at most as many as the section has lines.
        while (lines.has_value()) {
            const std::span<const char> all = text();
            std::optional<Line> match;
            std::size_t offset = (*lines).body;
            while (offset < (*lines).end) {
                const Line line = line_at(all, offset);
                if (is_start_line(all, line, service)) {
                    match = line;
                    break;
                }
                offset = line.next;
            }
            if (!match.has_value()) {
                return EditResult::done;
            }
            const EditResult result = splice((*match).begin, (*match).next, std::span<const char>());
            if (result != EditResult::done) {
                return result;
            }
            lines = find_section(init);
        }
        return EditResult::done;
    }
    if (starts(service)) {
        return EditResult::done;
    }
    const std::size_t saved_size = size_;
    if (!lines.has_value()) {
        lines = append_section(init);
        if (!lines.has_value()) {
            return EditResult::too_large;
        }
    }
    char line_text[stxconfig::name_length_max + 8];
    std::size_t size = 0;
    const std::size_t at = section_content_end(*lines);
    if (at > 0u && at == size_ && text_[at - 1u] != '\n') {
        line_text[size] = '\n';
        size += 1;
    }
    const char keyword[] = "start ";
    for (std::size_t index = 0; index + 1u < sizeof(keyword); index += 1) {
        line_text[size] = keyword[index];
        size += 1;
    }
    for (const char byte : service) {
        line_text[size] = byte;
        size += 1;
    }
    line_text[size] = '\n';
    size += 1;
    const EditResult result = splice(at, at, std::span<const char>(line_text, size));
    if (result != EditResult::done) {
        size_ = saved_size < size_ ? saved_size : size_;
    }
    return result;
}

}  // namespace stxweb
