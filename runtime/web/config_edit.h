// config_edit.h: line edits on a configuration's text, for the web pages
// that change the user configuration (C2; design
// dev/notes/planning/stages/C2-web.md, section 3.3).
//
// The pages change one thing at a time (a target's settings, whether it
// starts at boot, the host name, the network, the SSH keys) and keep the
// rest of the text as the owner wrote it, comments included. The editor
// works on lines, as the format is written (config/boot_config.h):
//
//   - a header line: blanks, "[", a name, "]", blanks;
//   - a setting line: blanks, a name, blanks, "=", the value;
//   - an init line in [init]: blanks, words separated by blanks;
//   - anything else (comments, blank lines) is kept as it is.
//
// A section reaches from its header to the line before the next header (or
// the text's end). The editor does not parse; whatever it produces is
// parsed and checked whole before it is written (the supervisor parses
// again before it writes a turn), so an edit can never store a text the
// parser refuses.
//
// Pure code: no system call, no allocation.

#ifndef STXLIB_RUNTIME_WEB_CONFIG_EDIT_H
#define STXLIB_RUNTIME_WEB_CONFIG_EDIT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../supervisor/config/boot_config.h"

namespace stxweb {

constexpr std::size_t edit_text_bytes_max = stxconfig::config_size_max;
// A setting written by the editor: its name, " = " and its value fit one
// line of the format.
constexpr std::size_t edit_line_bytes_max = stxconfig::line_length_max;
// Settings one replace_section call writes at most.
constexpr std::size_t edit_section_settings_max = 16;

enum class EditResult : std::uint8_t {
    done,
    // The edited text would exceed edit_text_bytes_max.
    too_large,
    // A section or setting name that is not [a-z][a-z0-9-]*, at most
    // name_length_max bytes.
    name_not_allowed,
    // A value with a byte the format refuses (only printable ASCII, space
    // and tab), with blanks at either end (the parser would drop them), or
    // whose line would be longer than edit_line_bytes_max.
    value_not_allowed,
    // More settings than edit_section_settings_max.
    too_many_settings,
};

[[nodiscard]] const char* edit_result_name(EditResult result) noexcept;

// [a-z][a-z0-9-]*, 1..name_length_max bytes.
[[nodiscard]] bool config_name_valid(std::span<const char> name) noexcept;
// A value the editor writes: see value_not_allowed; may be empty.
[[nodiscard]] bool config_value_valid(std::span<const char> name, std::span<const char> value) noexcept;

struct EditSetting {
    std::span<const char> name;
    std::span<const char> value;
};

// Large (two text buffers): the caller owns one, with static storage.
class ConfigEditor {
public:
    ConfigEditor() noexcept;
    ConfigEditor(const ConfigEditor&) = delete;
    ConfigEditor& operator=(const ConfigEditor&) = delete;

    // Starts from text, adding a line end after its last line when it has
    // none; false (and an empty text) when that is longer than
    // edit_text_bytes_max.
    [[nodiscard]] bool load(std::span<const char> text) noexcept;
    [[nodiscard]] std::span<const char> text() const noexcept;

    // name = value in section: the setting's line replaced when it exists,
    // else added after the section's last setting or header; the section
    // added at the text's end when absent.
    [[nodiscard]] EditResult set_setting(std::span<const char> section, std::span<const char> name,
                                         std::span<const char> value) noexcept;
    // The setting's line removed, when there is one.
    [[nodiscard]] EditResult remove_setting(std::span<const char> section, std::span<const char> name) noexcept;
    // The section's header and every line up to the next header removed.
    [[nodiscard]] EditResult remove_section(std::span<const char> section) noexcept;
    // The section's lines after its header replaced by settings, one line
    // each, in order (comments inside it are not kept); the section added
    // at the end when absent. With no settings the section stays, empty.
    [[nodiscard]] EditResult replace_section(std::span<const char> section,
                                             std::span<const EditSetting> settings) noexcept;
    // In [init]: a line "start <service>" added (enabled) or every such line
    // removed (not enabled). [init] is added when absent and needed.
    [[nodiscard]] EditResult set_start(std::span<const char> service, bool enabled) noexcept;

    // Whether [init] holds a line "start <service>".
    [[nodiscard]] bool starts(std::span<const char> service) const noexcept;

private:
    struct SectionLines {
        // Offsets in text_: the header line's start, the first byte after
        // the header line, and the section's end (the next header's start
        // or the text's end).
        std::size_t header;
        std::size_t body;
        std::size_t end;
    };

    [[nodiscard]] std::optional<SectionLines> find_section(std::span<const char> section) const noexcept;
    // Replaces text_[begin, end) by insertion.
    [[nodiscard]] EditResult splice(std::size_t begin, std::size_t end, std::span<const char> insertion) noexcept;
    // The offset after the section's last line that is not blank or a
    // comment (its body when it has none).
    [[nodiscard]] std::size_t section_content_end(const SectionLines& lines) const noexcept;
    // Appends a header for section at the end, after a line end (and a blank
    // line when the text is not empty). Returns the new section's lines.
    [[nodiscard]] std::optional<SectionLines> append_section(std::span<const char> section) noexcept;

    char text_[edit_text_bytes_max];
    std::size_t size_;
    char scratch_[edit_text_bytes_max];
};

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_CONFIG_EDIT_H
