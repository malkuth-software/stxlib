// boot_config.cpp: the boot configuration's parser (format: boot_config.h).
//
// One pass over the text, line by line, with no recursion and no
// backtracking: each line is classified by its first non-blank byte and
// parsed by one function. Every store into the configuration is preceded by
// its limit check; the first refusal stops the parse and empties the
// configuration.

#include "boot_config.h"

#include <cassert>
#include <new>
#include <type_traits>

namespace stxconfig {

static_assert(std::is_trivially_destructible_v<InitCommand>,
              "BootConfig::clear ends command lifetimes without running destructors");
static_assert(std::is_trivially_copyable_v<InitCommand>, "commands are stored by copy into raw storage");
static_assert(config_size_max <= UINT32_MAX && text_store_bytes <= UINT32_MAX,
              "TextRef offsets and line numbers are 32-bit");

// ---------------------------------------------------------------------------
// CommandAction and ParseOutcome: factories and queries.

CommandAction::CommandAction(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

CommandAction CommandAction::mount(const MountCommand& command) noexcept {
    const CommandAction action(Alternatives(std::in_place_type<MountCommand>, command));
    return action;
}

CommandAction CommandAction::wait_for(const WaitForCommand& command) noexcept {
    const CommandAction action(Alternatives(std::in_place_type<WaitForCommand>, command));
    return action;
}

CommandAction CommandAction::set(const SetCommand& command) noexcept {
    const CommandAction action(Alternatives(std::in_place_type<SetCommand>, command));
    return action;
}

CommandAction CommandAction::start(const StartCommand& command) noexcept {
    const CommandAction action(Alternatives(std::in_place_type<StartCommand>, command));
    return action;
}

CommandAction CommandAction::run(const RunCommand& command) noexcept {
    const CommandAction action(Alternatives(std::in_place_type<RunCommand>, command));
    return action;
}

CommandAction CommandAction::sysctl(const SysctlCommand& command) noexcept {
    const CommandAction action(Alternatives(std::in_place_type<SysctlCommand>, command));
    return action;
}

const MountCommand* CommandAction::as_mount() const noexcept {
    return std::get_if<MountCommand>(&alternatives_);
}

const WaitForCommand* CommandAction::as_wait_for() const noexcept {
    return std::get_if<WaitForCommand>(&alternatives_);
}

const SetCommand* CommandAction::as_set() const noexcept {
    return std::get_if<SetCommand>(&alternatives_);
}

const StartCommand* CommandAction::as_start() const noexcept {
    return std::get_if<StartCommand>(&alternatives_);
}

const RunCommand* CommandAction::as_run() const noexcept {
    return std::get_if<RunCommand>(&alternatives_);
}

const SysctlCommand* CommandAction::as_sysctl() const noexcept {
    return std::get_if<SysctlCommand>(&alternatives_);
}

const char* CommandAction::keyword() const noexcept {
    if (std::holds_alternative<MountCommand>(alternatives_)) {
        return "mount";
    }
    if (std::holds_alternative<WaitForCommand>(alternatives_)) {
        return "wait-for";
    }
    if (std::holds_alternative<SetCommand>(alternatives_)) {
        return "set";
    }
    if (std::holds_alternative<StartCommand>(alternatives_)) {
        return "start";
    }
    if (std::holds_alternative<RunCommand>(alternatives_)) {
        return "run";
    }
    if (std::holds_alternative<SysctlCommand>(alternatives_)) {
        return "sysctl";
    }
    // Fallback: a variant left valueless (impossible here: every
    // alternative is trivially copyable, so no assignment can throw).
    return "unknown";
}

ParseOutcome::ParseOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

ParseOutcome ParseOutcome::succeeded() noexcept {
    const ParseOutcome outcome(Alternatives(std::in_place_type<ParseSucceeded>));
    return outcome;
}

ParseOutcome ParseOutcome::failed(std::uint32_t line, std::uint32_t column, ParseError error) noexcept {
    ParseFailed failure{};
    failure.line = line;
    failure.column = column;
    failure.error = error;
    const ParseOutcome outcome(Alternatives(std::in_place_type<ParseFailed>, failure));
    return outcome;
}

bool ParseOutcome::is_success() const noexcept {
    return std::holds_alternative<ParseSucceeded>(alternatives_);
}

const ParseFailed* ParseOutcome::as_failure() const noexcept {
    return std::get_if<ParseFailed>(&alternatives_);
}

const char* parse_error_name(ParseError error) noexcept {
    switch (error) {
    case ParseError::file_too_large: return "file_too_large";
    case ParseError::line_too_long: return "line_too_long";
    case ParseError::byte_not_allowed: return "byte_not_allowed";
    case ParseError::header_malformed: return "header_malformed";
    case ParseError::name_malformed: return "name_malformed";
    case ParseError::section_repeated: return "section_repeated";
    case ParseError::too_many_sections: return "too_many_sections";
    case ParseError::setting_outside_section: return "setting_outside_section";
    case ParseError::setting_malformed: return "setting_malformed";
    case ParseError::setting_repeated: return "setting_repeated";
    case ParseError::too_many_settings: return "too_many_settings";
    case ParseError::quote_unterminated: return "quote_unterminated";
    case ParseError::escape_malformed: return "escape_malformed";
    case ParseError::word_after_quote: return "word_after_quote";
    case ParseError::quote_in_word: return "quote_in_word";
    case ParseError::too_many_words: return "too_many_words";
    case ParseError::command_unknown: return "command_unknown";
    case ParseError::command_arguments_wrong: return "command_arguments_wrong";
    case ParseError::file_system_unknown: return "file_system_unknown";
    case ParseError::access_unknown: return "access_unknown";
    case ParseError::presence_kind_unknown: return "presence_kind_unknown";
    case ParseError::number_malformed: return "number_malformed";
    case ParseError::wait_too_long: return "wait_too_long";
    case ParseError::variable_name_malformed: return "variable_name_malformed";
    case ParseError::program_not_absolute: return "program_not_absolute";
    case ParseError::too_many_run_arguments: return "too_many_run_arguments";
    case ParseError::guard_nested: return "guard_nested";
    case ParseError::too_many_commands: return "too_many_commands";
    case ParseError::text_store_full: return "text_store_full";
    case ParseError::service_setting_unknown: return "service_setting_unknown";
    case ParseError::service_command_missing: return "service_command_missing";
    case ParseError::service_restart_unknown: return "service_restart_unknown";
    case ParseError::directory_not_absolute: return "directory_not_absolute";
    case ParseError::too_many_services: return "too_many_services";
    case ParseError::start_unknown_service: return "start_unknown_service";
    case ParseError::folder_malformed: return "folder_malformed";
    case ParseError::folder_outside_data: return "folder_outside_data";
    case ParseError::device_pattern_malformed: return "device_pattern_malformed";
    case ParseError::too_many_devices: return "too_many_devices";
    case ParseError::sysctl_malformed: return "sysctl_malformed";
    case ParseError::target_setting_missing: return "target_setting_missing";
    case ParseError::target_role_unknown: return "target_role_unknown";
    case ParseError::target_id_malformed: return "target_id_malformed";
    case ParseError::target_id_repeated: return "target_id_repeated";
    case ParseError::second_foreground: return "second_foreground";
    case ParseError::devices_not_foreground: return "devices_not_foreground";
    case ParseError::port_malformed: return "port_malformed";
    case ParseError::too_many_ports: return "too_many_ports";
    case ParseError::port_repeated: return "port_repeated";
    case ParseError::program_path_malformed: return "program_path_malformed";
    case ParseError::start_policy_unknown: return "start_policy_unknown";
    case ParseError::update_url_malformed: return "update_url_malformed";
    case ParseError::update_interval_malformed: return "update_interval_malformed";
    case ParseError::healthy_after_malformed: return "healthy_after_malformed";
    case ParseError::name_repeated: return "name_repeated";
    case ParseError::network_setting_unknown: return "network_setting_unknown";
    case ParseError::network_value_malformed: return "network_value_malformed";
    case ParseError::folder_repeated: return "folder_repeated";
    case ParseError::service_control_unknown: return "service_control_unknown";
    case ParseError::target_setting_in_service: return "target_setting_in_service";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown_error";
}

// ---------------------------------------------------------------------------
// BootConfig.

BootConfig::BootConfig() noexcept
    : text_store_{}, text_store_used_(0), sections_{}, section_count_(0), settings_{}, setting_count_(0),
      init_command_storage_{}, init_command_count_(0), has_init_section_(false), services_{}, service_count_(0) {}

void BootConfig::clear() noexcept {
    // InitCommand is trivially destructible (asserted above): resetting the
    // count ends the stored commands' lifetimes.
    text_store_used_ = 0;
    section_count_ = 0;
    setting_count_ = 0;
    init_command_count_ = 0;
    has_init_section_ = false;
    service_count_ = 0;
}

std::size_t BootConfig::section_count() const noexcept {
    return section_count_;
}

std::size_t BootConfig::setting_count() const noexcept {
    return setting_count_;
}

std::size_t BootConfig::init_command_count() const noexcept {
    return init_command_count_;
}

bool BootConfig::has_init_section() const noexcept {
    return has_init_section_;
}

const Section& BootConfig::section(std::size_t index) const noexcept {
    assert(index < section_count_);
    return sections_[index];
}

const Setting& BootConfig::setting(std::size_t index) const noexcept {
    assert(index < setting_count_);
    return settings_[index];
}

const InitCommand& BootConfig::init_command(std::size_t index) const noexcept {
    assert(index < init_command_count_);
    const unsigned char* const slot = &init_command_storage_[index * sizeof(InitCommand)];
    return *std::launder(reinterpret_cast<const InitCommand*>(slot));
}

std::size_t BootConfig::service_count() const noexcept {
    return service_count_;
}

const ServiceDefinition& BootConfig::service(std::size_t index) const noexcept {
    assert(index < service_count_);
    return services_[index];
}

const char* BootConfig::text(const TextRef& reference) const noexcept {
    // The text and its NUL lie inside the used part of the store.
    assert(static_cast<std::size_t>(reference.offset) + reference.length < text_store_used_);
    return &text_store_[reference.offset];
}

std::size_t BootConfig::text_store_used() const noexcept {
    return text_store_used_;
}

namespace {

[[nodiscard]] bool c_strings_equal(const char* left, const char* right) noexcept {
    std::size_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index += 1;
    }
    return left[index] == right[index];
}

}  // namespace

std::optional<TextRef> BootConfig::find_setting(const char* section_name, const char* setting_name) const noexcept {
    for (std::size_t section_index = 0; section_index < section_count_; section_index += 1) {
        if (!c_strings_equal(text(sections_[section_index].name), section_name)) {
            continue;
        }
        for (std::size_t setting_index = 0; setting_index < setting_count_; setting_index += 1) {
            const Setting& candidate = settings_[setting_index];
            if (candidate.section_index == section_index && c_strings_equal(text(candidate.name), setting_name)) {
                const std::optional<TextRef> found = candidate.value;
                return found;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::size_t> BootConfig::find_service(const char* name) const noexcept {
    for (std::size_t index = 0; index < service_count_; index += 1) {
        if (c_strings_equal(text(services_[index].name), name)) {
            const std::optional<std::size_t> found = index;
            return found;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ConfigBuilder: the parser's only way to write into a BootConfig. Each
// operation checks its limit and reports whether it stored anything.

class ConfigBuilder {
public:
    explicit ConfigBuilder(BootConfig& config) noexcept : config_(config), open_text_offset_(0) {}

    // A stored text is built in three steps: begin, append bytes, finish.
    // Only one text is open at a time.
    void begin_text() noexcept {
        open_text_offset_ = config_.text_store_used_;
    }

    [[nodiscard]] bool append_byte(char byte) noexcept {
        // One byte stays free for the terminating NUL.
        if (config_.text_store_used_ + 1u >= text_store_bytes) {
            return false;
        }
        config_.text_store_[config_.text_store_used_] = byte;
        config_.text_store_used_ += 1;
        return true;
    }

    [[nodiscard]] TextRef finish_text() noexcept {
        // append_byte left room for this NUL.
        config_.text_store_[config_.text_store_used_] = '\0';
        TextRef reference{};
        reference.offset = static_cast<std::uint32_t>(open_text_offset_);
        reference.length = static_cast<std::uint32_t>(config_.text_store_used_ - open_text_offset_);
        config_.text_store_used_ += 1;
        return reference;
    }

    [[nodiscard]] const char* text(const TextRef& reference) const noexcept {
        return config_.text(reference);
    }

    [[nodiscard]] bool section_exists(const char* name) const noexcept {
        for (std::size_t index = 0; index < config_.section_count_; index += 1) {
            if (c_strings_equal(config_.text(config_.sections_[index].name), name)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool sections_full() const noexcept {
        return config_.section_count_ >= sections_max;
    }

    // Precondition: !sections_full(). Returns the new section's index.
    [[nodiscard]] std::uint32_t add_section(const Section& section) noexcept {
        assert(!sections_full());
        config_.sections_[config_.section_count_] = section;
        config_.section_count_ += 1;
        return static_cast<std::uint32_t>(config_.section_count_ - 1u);
    }

    void mark_init_section() noexcept {
        config_.has_init_section_ = true;
    }

    [[nodiscard]] bool setting_exists(std::uint32_t section_index, const char* name) const noexcept {
        for (std::size_t index = 0; index < config_.setting_count_; index += 1) {
            const Setting& candidate = config_.settings_[index];
            if (candidate.section_index == section_index && c_strings_equal(config_.text(candidate.name), name)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool settings_full() const noexcept {
        return config_.setting_count_ >= settings_max;
    }

    void add_setting(const Setting& setting) noexcept {
        assert(!settings_full());
        config_.settings_[config_.setting_count_] = setting;
        config_.setting_count_ += 1;
    }

    [[nodiscard]] bool commands_full() const noexcept {
        return config_.init_command_count_ >= init_commands_max;
    }

    [[nodiscard]] bool services_full() const noexcept {
        return config_.service_count_ >= services_max;
    }

    void add_service(const ServiceDefinition& service) noexcept {
        assert(!services_full());
        config_.services_[config_.service_count_] = service;
        config_.service_count_ += 1;
    }

    [[nodiscard]] const BootConfig& config() const noexcept {
        return config_;
    }

    void add_command(const InitCommand& command) noexcept {
        assert(!commands_full());
        unsigned char* const slot = &config_.init_command_storage_[config_.init_command_count_ * sizeof(InitCommand)];
        new (slot) InitCommand(command);
        config_.init_command_count_ += 1;
    }

private:
    BootConfig& config_;
    std::size_t open_text_offset_;
};

namespace {

// ---------------------------------------------------------------------------
// Bytes and lines.

// A line of the text: its bytes without the line end (LF, or CR LF) and its
// 1-based number. bytes borrows from the text being parsed.
struct LineView {
    const char* bytes;
    std::size_t length;
    std::uint32_t number;
};

[[nodiscard]] std::size_t c_string_length(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length;
}

[[nodiscard]] bool is_blank(char byte) noexcept {
    return byte == ' ' || byte == '\t';
}

[[nodiscard]] bool is_allowed_byte(char byte) noexcept {
    const unsigned char value = static_cast<unsigned char>(byte);
    return byte == '\t' || (value >= 0x20u && value <= 0x7eu);
}

[[nodiscard]] bool is_lower_letter(char byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}

[[nodiscard]] bool is_digit(char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

[[nodiscard]] bool is_name_byte(char byte) noexcept {
    return is_lower_letter(byte) || is_digit(byte) || byte == '-';
}

[[nodiscard]] bool is_variable_start(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || is_lower_letter(byte) || byte == '_';
}

[[nodiscard]] bool is_variable_byte(char byte) noexcept {
    return is_variable_start(byte) || is_digit(byte);
}

[[nodiscard]] std::uint32_t column_of(std::size_t index) noexcept {
    // Lines are at most line_length_max bytes, so the column fits.
    return static_cast<std::uint32_t>(index + 1u);
}

// A service command's words, as read from its setting.
struct ServiceCommandWords {
    std::uint32_t count;
    TextRef arguments[run_arguments_max];
};

// A service's device patterns, as read from its setting.
struct ServiceDeviceWords {
    std::uint32_t count;
    TextRef patterns[devices_max];
};

// Where a setting was, for a refusal found when the section closes.
struct SettingPlace {
    std::uint32_t line;
    std::uint32_t column;
};

// A target's declared ports, as read from its setting.
struct TargetPortWords {
    std::uint32_t count;
    PortDeclaration ports[ports_max];
};

// The target settings read so far (each absent until its line).
struct TargetDraft {
    std::optional<TargetRole> role;
    std::optional<std::uint32_t> id;
    std::optional<TextRef> program;
    std::optional<TextRef> folder;
    std::optional<TargetPortWords> ports;
    std::optional<ServiceDeviceWords> devices;
    std::optional<SettingPlace> devices_place;
    std::optional<StartPolicy> start;
    std::optional<TextRef> update_url;
    std::optional<std::uint32_t> update_interval_s;
    std::optional<SettingPlace> update_interval_place;
    std::optional<std::uint32_t> healthy_after_s;
};

// The service or target section being read; it becomes a ServiceDefinition
// when the next header or the end of the text closes it.
struct ServiceDraft {
    TextRef name;
    std::uint32_t line;
    std::uint32_t name_column;
    std::optional<ServiceCommandWords> command;
    // Where the command's first word is: checked when the section closes,
    // as a package path (target) or an absolute path (service), because a
    // [service-<name>] section becomes a target at its first target
    // setting, which may follow the command.
    std::optional<SettingPlace> program_place;
    std::optional<TextRef> directory;
    std::optional<RestartPolicy> restart;
    // Present for a target section.
    std::optional<TargetDraft> target;
    std::optional<ControlAccess> control;
};

// The parse's state between lines.
struct ParseState {
    // Index of the current section, or nothing before the first header.
    std::optional<std::uint32_t> section_index;
    bool in_init_section;
    // Present while the current section is a service or target section.
    std::optional<ServiceDraft> service;
    // Whether the current section is [network].
    bool in_network_section;
};

// A line's outcome: nothing (the line was accepted) or the refusal.
using LineResult = std::optional<ParseFailed>;

[[nodiscard]] LineResult refuse_at(std::uint32_t line_number, std::uint32_t column, ParseError error) noexcept {
    ParseFailed failure{};
    failure.line = line_number;
    failure.column = column;
    failure.error = error;
    const LineResult result = failure;
    return result;
}

[[nodiscard]] LineResult refuse(const LineView& line, std::size_t index, ParseError error) noexcept {
    return refuse_at(line.number, column_of(index), error);
}

// A target's settings from its draft, or the refusal: role, folder and
// command are required (id is optional: 0 until the supervisor issues it,
// target_ids.h); devices belong to the foreground target only;
// update-interval needs update-url.
[[nodiscard]] LineResult finish_target(const ServiceDraft& draft, const TargetDraft& target_draft,
                                       ServiceDefinition& service) noexcept {
    if (!target_draft.role.has_value() || !target_draft.folder.has_value()) {
        return refuse_at(draft.line, draft.name_column, ParseError::target_setting_missing);
    }
    if (*target_draft.role != TargetRole::foreground && target_draft.devices.has_value()) {
        const SettingPlace place =
            target_draft.devices_place.has_value() ? *target_draft.devices_place : SettingPlace{draft.line, 1};
        return refuse_at(place.line, place.column, ParseError::devices_not_foreground);
    }
    if (target_draft.update_interval_s.has_value() && !target_draft.update_url.has_value()) {
        const SettingPlace place = target_draft.update_interval_place.has_value() ? *target_draft.update_interval_place
                                                                                 : SettingPlace{draft.line, 1};
        return refuse_at(place.line, place.column, ParseError::update_interval_malformed);
    }
    TargetSettings target{};
    target.role = *target_draft.role;
    target.id = target_draft.id.has_value() ? *target_draft.id : 0u;
    target.program = target_draft.program.has_value() ? *target_draft.program : draft.name;
    target.folder = *target_draft.folder;
    target.port_count = 0;
    if (target_draft.ports.has_value()) {
        target.port_count = (*target_draft.ports).count;
        for (std::uint32_t index = 0; index < (*target_draft.ports).count; index += 1) {
            target.ports[index] = (*target_draft.ports).ports[index];
        }
    }
    target.device_count = 0;
    if (target_draft.devices.has_value()) {
        target.device_count = (*target_draft.devices).count;
        for (std::uint32_t index = 0; index < (*target_draft.devices).count; index += 1) {
            target.devices[index] = (*target_draft.devices).patterns[index];
        }
    }
    target.start = target_draft.start.has_value() ? *target_draft.start : StartPolicy::boot;
    target.update_url = target_draft.update_url;
    target.update_interval_s =
        target_draft.update_interval_s.has_value() ? *target_draft.update_interval_s : update_interval_default_s;
    target.healthy_after_s =
        target_draft.healthy_after_s.has_value() ? *target_draft.healthy_after_s : healthy_after_default_s;
    service.target = target;
    return std::nullopt;
}

// Closes the current service or target section, if any: each needs a
// command, a target its required settings.
// Whether the command's first word suits the section's final kind: a
// package path for a target, an absolute path for a service.
[[nodiscard]] bool program_word_valid(const ConfigBuilder& builder, const ServiceDraft& draft) noexcept {
    const TextRef program = (*draft.command).arguments[0];
    const char* const text = builder.text(program);
    if (draft.target.has_value()) {
        return package_path_valid(text, program.length);
    }
    return program.length != 0u && text[0] == '/';
}

[[nodiscard]] LineResult finish_service(ConfigBuilder& builder, ParseState& state) noexcept {
    if (!state.service.has_value()) {
        return std::nullopt;
    }
    const ServiceDraft draft = *state.service;
    state.service.reset();
    if (!draft.command.has_value()) {
        return refuse_at(draft.line, draft.name_column, ParseError::service_command_missing);
    }
    if (draft.program_place.has_value() && !program_word_valid(builder, draft)) {
        const SettingPlace place = *draft.program_place;
        return refuse_at(place.line, place.column,
                         draft.target.has_value() ? ParseError::program_path_malformed
                                                  : ParseError::program_not_absolute);
    }
    ServiceDefinition service{};
    service.name = draft.name;
    service.argument_count = (*draft.command).count;
    for (std::uint32_t index = 0; index < (*draft.command).count; index += 1) {
        service.arguments[index] = (*draft.command).arguments[index];
    }
    service.directory = draft.directory;
    service.target = std::nullopt;
    if (draft.target.has_value()) {
        const LineResult target = finish_target(draft, *draft.target, service);
        if (target.has_value()) {
            return target;
        }
    }
    service.restart = draft.restart.has_value() ? *draft.restart : RestartPolicy::on_failure;
    service.control = draft.control.has_value() ? *draft.control : ControlAccess::none;
    service.line = draft.line;
    // Room was checked when the section began.
    builder.add_service(service);
    return std::nullopt;
}

// Whether the stored section name starts with prefix.
[[nodiscard]] bool has_prefix(const char* name, const char* prefix) noexcept {
    std::size_t index = 0;
    while (prefix[index] != '\0') {
        if (name[index] != prefix[index]) {
            return false;
        }
        index += 1;
    }
    return true;
}

// Stores bytes [begin, end) of the line as one text.
[[nodiscard]] std::optional<TextRef> store_bytes(ConfigBuilder& builder, const LineView& line, std::size_t begin,
                                                 std::size_t end) noexcept {
    builder.begin_text();
    for (std::size_t index = begin; index < end; index += 1) {
        if (!builder.append_byte(line.bytes[index])) {
            return std::nullopt;
        }
    }
    const std::optional<TextRef> stored = builder.finish_text();
    return stored;
}

// The end of the name starting at begin (the first byte that is not a name
// byte), whether or not the name is valid.
[[nodiscard]] std::size_t name_end(const LineView& line, std::size_t begin) noexcept {
    std::size_t index = begin;
    while (index < line.length && is_name_byte(line.bytes[index])) {
        index += 1;
    }
    return index;
}

[[nodiscard]] bool name_is_valid(const LineView& line, std::size_t begin, std::size_t end) noexcept {
    const std::size_t length = end - begin;
    return length >= 1u && length <= name_length_max && is_lower_letter(line.bytes[begin]);
}

[[nodiscard]] std::size_t skip_blanks(const LineView& line, std::size_t begin) noexcept {
    std::size_t index = begin;
    while (index < line.length && is_blank(line.bytes[index])) {
        index += 1;
    }
    return index;
}

// ---------------------------------------------------------------------------
// Headers and settings.

// "[name]" followed only by blanks. begin is the '['.
[[nodiscard]] LineResult parse_header(ConfigBuilder& builder, ParseState& state, const LineView& line,
                                      std::size_t begin) noexcept {
    const std::size_t name_begin = begin + 1u;
    const std::size_t end_of_name = name_end(line, name_begin);
    if (!name_is_valid(line, name_begin, end_of_name)) {
        return refuse(line, name_begin, ParseError::name_malformed);
    }
    if (end_of_name >= line.length || line.bytes[end_of_name] != ']') {
        return refuse(line, end_of_name, ParseError::header_malformed);
    }
    const std::size_t after = skip_blanks(line, end_of_name + 1u);
    if (after != line.length) {
        return refuse(line, after, ParseError::header_malformed);
    }
    if (builder.sections_full()) {
        return refuse(line, begin, ParseError::too_many_sections);
    }
    const std::optional<TextRef> name = store_bytes(builder, line, name_begin, end_of_name);
    if (!name.has_value()) {
        return refuse(line, name_begin, ParseError::text_store_full);
    }
    // The name is stored before the repeat check so it can be compared as a
    // C string; a refused file is emptied, so the stored copy does not stay.
    if (builder.section_exists(builder.text(*name))) {
        return refuse(line, name_begin, ParseError::section_repeated);
    }
    const LineResult finished = finish_service(builder, state);
    if (finished.has_value()) {
        return finished;
    }
    std::optional<ServiceDraft> service;
    const bool is_service = has_prefix(builder.text(*name), service_section_prefix);
    const bool is_target = has_prefix(builder.text(*name), target_section_prefix);
    if (is_service || is_target) {
        const char* const prefix = is_service ? service_section_prefix : target_section_prefix;
        const std::size_t service_begin = name_begin + c_string_length(prefix);
        // The name after the prefix is itself a name: at least one byte, a
        // lower-case letter first (the rest are name bytes already).
        if (service_begin >= end_of_name || !is_lower_letter(line.bytes[service_begin])) {
            return refuse(line, name_begin, ParseError::name_malformed);
        }
        if (builder.services_full()) {
            return refuse(line, begin, ParseError::too_many_services);
        }
        const std::optional<TextRef> service_name = store_bytes(builder, line, service_begin, end_of_name);
        if (!service_name.has_value()) {
            return refuse(line, service_begin, ParseError::text_store_full);
        }
        ServiceDraft draft{};
        draft.name = *service_name;
        draft.line = line.number;
        draft.name_column = column_of(name_begin);
        if (is_target) {
            draft.target = TargetDraft{};
        }
        service = draft;
    }
    Section section{};
    section.name = *name;
    section.line = line.number;
    const std::uint32_t index = builder.add_section(section);
    state.section_index = index;
    state.in_init_section = c_strings_equal(builder.text(*name), "init");
    state.service = service;
    state.in_network_section = c_strings_equal(builder.text(*name), "network");
    if (state.in_init_section) {
        builder.mark_init_section();
    }
    return std::nullopt;
}

// "name = value". begin is the name's first byte.
[[nodiscard]] LineResult parse_setting(ConfigBuilder& builder, const ParseState& state, const LineView& line,
                                       std::size_t begin) noexcept {
    if (!state.section_index.has_value()) {
        return refuse(line, begin, ParseError::setting_outside_section);
    }
    const std::uint32_t section_index = *state.section_index;
    const std::size_t end_of_name = name_end(line, begin);
    if (!name_is_valid(line, begin, end_of_name)) {
        return refuse(line, begin, ParseError::name_malformed);
    }
    const std::size_t equals = skip_blanks(line, end_of_name);
    if (equals >= line.length || line.bytes[equals] != '=') {
        return refuse(line, equals, ParseError::setting_malformed);
    }
    const std::size_t value_begin = skip_blanks(line, equals + 1u);
    std::size_t value_end = line.length;
    while (value_end > value_begin && is_blank(line.bytes[value_end - 1u])) {
        value_end -= 1;
    }
    if (builder.settings_full()) {
        return refuse(line, begin, ParseError::too_many_settings);
    }
    const std::optional<TextRef> name = store_bytes(builder, line, begin, end_of_name);
    if (!name.has_value()) {
        return refuse(line, begin, ParseError::text_store_full);
    }
    if (builder.setting_exists(section_index, builder.text(*name))) {
        return refuse(line, begin, ParseError::setting_repeated);
    }
    const std::optional<TextRef> value = store_bytes(builder, line, value_begin, value_end);
    if (!value.has_value()) {
        return refuse(line, value_begin, ParseError::text_store_full);
    }
    Setting setting{};
    setting.section_index = section_index;
    setting.name = *name;
    setting.value = *value;
    setting.line = line.number;
    builder.add_setting(setting);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Init commands: words.

struct Word {
    TextRef text;
    // Index in the line of the word's first byte (its quote, if quoted).
    std::size_t index;
};

struct CommandWords {
    Word words[command_words_max];
    std::size_t count;
};

// A quoted word starting at the '"' at begin. On success, *next is the index
// after the closing quote.
[[nodiscard]] LineResult read_quoted_word(ConfigBuilder& builder, const LineView& line, std::size_t begin,
                                          Word& word, std::size_t& next) noexcept {
    builder.begin_text();
    std::size_t index = begin + 1u;
    for (;;) {
        if (index >= line.length) {
            return refuse(line, begin, ParseError::quote_unterminated);
        }
        const char byte = line.bytes[index];
        if (byte == '"') {
            break;
        }
        char stored = byte;
        if (byte == '\\') {
            if (index + 1u >= line.length) {
                return refuse(line, index, ParseError::escape_malformed);
            }
            const char escaped = line.bytes[index + 1u];
            if (escaped != '"' && escaped != '\\') {
                return refuse(line, index, ParseError::escape_malformed);
            }
            stored = escaped;
            index += 1;
        }
        if (!builder.append_byte(stored)) {
            return refuse(line, index, ParseError::text_store_full);
        }
        index += 1;
    }
    // index is at the closing quote.
    const std::size_t after = index + 1u;
    if (after < line.length && !is_blank(line.bytes[after])) {
        return refuse(line, after, ParseError::word_after_quote);
    }
    word.text = builder.finish_text();
    word.index = begin;
    next = after;
    return std::nullopt;
}

[[nodiscard]] LineResult read_bare_word(ConfigBuilder& builder, const LineView& line, std::size_t begin, Word& word,
                                        std::size_t& next) noexcept {
    std::size_t index = begin;
    while (index < line.length && !is_blank(line.bytes[index])) {
        if (line.bytes[index] == '"') {
            return refuse(line, index, ParseError::quote_in_word);
        }
        index += 1;
    }
    const std::optional<TextRef> stored = store_bytes(builder, line, begin, index);
    if (!stored.has_value()) {
        return refuse(line, begin, ParseError::text_store_full);
    }
    word.text = *stored;
    word.index = begin;
    next = index;
    return std::nullopt;
}

[[nodiscard]] LineResult split_words(ConfigBuilder& builder, const LineView& line, std::size_t begin,
                                     CommandWords& words) noexcept {
    words.count = 0;
    std::size_t index = skip_blanks(line, begin);
    while (index < line.length) {
        if (words.count >= command_words_max) {
            return refuse(line, index, ParseError::too_many_words);
        }
        Word word{};
        std::size_t next = index;
        const LineResult read = line.bytes[index] == '"' ? read_quoted_word(builder, line, index, word, next)
                                                         : read_bare_word(builder, line, index, word, next);
        if (read.has_value()) {
            return read;
        }
        words.words[words.count] = word;
        words.count += 1;
        // Each read consumes at least one byte (a quote or a non-blank), so
        // the loop ends within line.length iterations.
        index = skip_blanks(line, next);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Init commands: meaning.

// The words of one command (after any guard): words.words[first .. count).
struct CommandSpan {
    const CommandWords& words;
    std::size_t first;
};

[[nodiscard]] std::size_t span_length(const CommandSpan& span) noexcept {
    return span.words.count - span.first;
}

[[nodiscard]] const Word& span_word(const CommandSpan& span, std::size_t position) noexcept {
    assert(span.first + position < span.words.count);
    return span.words.words[span.first + position];
}

[[nodiscard]] bool word_is(const ConfigBuilder& builder, const Word& word, const char* literal) noexcept {
    return c_strings_equal(builder.text(word.text), literal);
}

[[nodiscard]] std::optional<PresenceKind> presence_kind_of(const ConfigBuilder& builder, const Word& word) noexcept {
    if (word_is(builder, word, "device")) {
        return PresenceKind::device;
    }
    if (word_is(builder, word, "file")) {
        return PresenceKind::file;
    }
    return std::nullopt;
}

// A decimal number of 1 to 10 digits, nothing else.
[[nodiscard]] std::optional<std::uint64_t> decimal_of(const ConfigBuilder& builder, const Word& word) noexcept {
    constexpr std::uint32_t digits_max = 10;
    if (word.text.length == 0u || word.text.length > digits_max) {
        return std::nullopt;
    }
    const char* const digits = builder.text(word.text);
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < word.text.length; index += 1) {
        if (!is_digit(digits[index])) {
            return std::nullopt;
        }
        // At most 10 digits: value stays below 10^10, far inside 64 bits.
        value = value * 10u + static_cast<std::uint64_t>(digits[index] - '0');
    }
    const std::optional<std::uint64_t> parsed = value;
    return parsed;
}

[[nodiscard]] bool text_is_name(const ConfigBuilder& builder, const Word& word) noexcept {
    if (word.text.length == 0u || word.text.length > name_length_max) {
        return false;
    }
    const char* const name = builder.text(word.text);
    if (!is_lower_letter(name[0])) {
        return false;
    }
    for (std::uint32_t index = 1; index < word.text.length; index += 1) {
        if (!is_name_byte(name[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool text_is_variable_name(const ConfigBuilder& builder, const Word& word) noexcept {
    if (word.text.length == 0u || word.text.length > name_length_max) {
        return false;
    }
    const char* const name = builder.text(word.text);
    if (!is_variable_start(name[0])) {
        return false;
    }
    for (std::uint32_t index = 1; index < word.text.length; index += 1) {
        if (!is_variable_byte(name[index])) {
            return false;
        }
    }
    return true;
}

// The result of reading one command's meaning: the action, or a refusal.
class ActionResult {
public:
    [[nodiscard]] static ActionResult action(const CommandAction& action) noexcept {
        const ActionResult result(Alternatives(std::in_place_type<CommandAction>, action));
        return result;
    }
    [[nodiscard]] static ActionResult refused(const ParseFailed& failure) noexcept {
        const ActionResult result(Alternatives(std::in_place_type<ParseFailed>, failure));
        return result;
    }
    [[nodiscard]] const CommandAction* as_action() const noexcept {
        return std::get_if<CommandAction>(&alternatives_);
    }
    [[nodiscard]] const ParseFailed* as_refusal() const noexcept {
        return std::get_if<ParseFailed>(&alternatives_);
    }

private:
    using Alternatives = std::variant<CommandAction, ParseFailed>;
    explicit ActionResult(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}
    Alternatives alternatives_;
};

[[nodiscard]] ActionResult refuse_action(const LineView& line, std::size_t index, ParseError error) noexcept {
    const LineResult refusal = refuse(line, index, error);
    return ActionResult::refused(*refusal);
}

[[nodiscard]] ActionResult read_mount(const ConfigBuilder& builder, const LineView& line,
                                      const CommandSpan& span) noexcept {
    if (span_length(span) != 5u) {
        return refuse_action(line, span_word(span, 0).index, ParseError::command_arguments_wrong);
    }
    MountCommand command{};
    const Word& file_system = span_word(span, 1);
    if (word_is(builder, file_system, "ufs")) {
        command.file_system = FileSystemKind::ufs;
    } else if (word_is(builder, file_system, "msdosfs")) {
        command.file_system = FileSystemKind::msdosfs;
    } else {
        return refuse_action(line, file_system.index, ParseError::file_system_unknown);
    }
    command.device = span_word(span, 2).text;
    command.directory = span_word(span, 3).text;
    const Word& access = span_word(span, 4);
    if (word_is(builder, access, "ro")) {
        command.access = MountAccess::read_only;
    } else if (word_is(builder, access, "rw")) {
        command.access = MountAccess::read_write;
    } else {
        return refuse_action(line, access.index, ParseError::access_unknown);
    }
    return ActionResult::action(CommandAction::mount(command));
}

[[nodiscard]] ActionResult read_wait_for(const ConfigBuilder& builder, const LineView& line,
                                         const CommandSpan& span) noexcept {
    if (span_length(span) != 4u) {
        return refuse_action(line, span_word(span, 0).index, ParseError::command_arguments_wrong);
    }
    const std::optional<PresenceKind> kind = presence_kind_of(builder, span_word(span, 1));
    if (!kind.has_value()) {
        return refuse_action(line, span_word(span, 1).index, ParseError::presence_kind_unknown);
    }
    const Word& milliseconds = span_word(span, 3);
    const std::optional<std::uint64_t> timeout = decimal_of(builder, milliseconds);
    if (!timeout.has_value()) {
        return refuse_action(line, milliseconds.index, ParseError::number_malformed);
    }
    if (*timeout > wait_ms_max) {
        return refuse_action(line, milliseconds.index, ParseError::wait_too_long);
    }
    WaitForCommand command{};
    command.kind = *kind;
    command.path = span_word(span, 2).text;
    command.timeout_ms = static_cast<std::uint32_t>(*timeout);
    return ActionResult::action(CommandAction::wait_for(command));
}

[[nodiscard]] ActionResult read_set(const ConfigBuilder& builder, const LineView& line,
                                    const CommandSpan& span) noexcept {
    if (span_length(span) != 3u) {
        return refuse_action(line, span_word(span, 0).index, ParseError::command_arguments_wrong);
    }
    const Word& name = span_word(span, 1);
    if (!text_is_variable_name(builder, name)) {
        return refuse_action(line, name.index, ParseError::variable_name_malformed);
    }
    SetCommand command{};
    command.name = name.text;
    command.value = span_word(span, 2).text;
    return ActionResult::action(CommandAction::set(command));
}

[[nodiscard]] ActionResult read_start(const ConfigBuilder& builder, const LineView& line,
                                      const CommandSpan& span) noexcept {
    if (span_length(span) != 2u) {
        return refuse_action(line, span_word(span, 0).index, ParseError::command_arguments_wrong);
    }
    const Word& service = span_word(span, 1);
    if (!text_is_name(builder, service)) {
        return refuse_action(line, service.index, ParseError::name_malformed);
    }
    StartCommand command{};
    command.service = service.text;
    return ActionResult::action(CommandAction::start(command));
}

[[nodiscard]] ActionResult read_run(const ConfigBuilder& builder, const LineView& line,
                                    const CommandSpan& span) noexcept {
    const std::size_t length = span_length(span);
    if (length < 2u) {
        return refuse_action(line, span_word(span, 0).index, ParseError::command_arguments_wrong);
    }
    const std::size_t argument_count = length - 1u;
    if (argument_count > run_arguments_max) {
        return refuse_action(line, span_word(span, 1u + run_arguments_max).index,
                             ParseError::too_many_run_arguments);
    }
    const Word& program = span_word(span, 1);
    if (program.text.length == 0u || builder.text(program.text)[0] != '/') {
        return refuse_action(line, program.index, ParseError::program_not_absolute);
    }
    RunCommand command{};
    command.argument_count = static_cast<std::uint32_t>(argument_count);
    for (std::size_t position = 0; position < argument_count; position += 1) {
        command.arguments[position] = span_word(span, 1u + position).text;
    }
    return ActionResult::action(CommandAction::run(command));
}

[[nodiscard]] bool is_sysctl_name_byte(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || is_lower_letter(byte) || is_digit(byte) || byte == '_' || byte == '-' ||
           byte == '%' || byte == '.';
}

// sysctl name=value. The name: bytes of [A-Za-z0-9_%.-], at least one,
// neither starting nor ending with '.', no empty component (".."). The
// value: anything after the first '=' (possibly empty), read by the
// variable's type when the command runs.
[[nodiscard]] ActionResult read_sysctl(const ConfigBuilder& builder, const LineView& line,
                                       const CommandSpan& span) noexcept {
    if (span_length(span) != 2u) {
        return refuse_action(line, span_word(span, 0).index, ParseError::command_arguments_wrong);
    }
    const Word& setting = span_word(span, 1);
    const char* const text = builder.text(setting.text);
    std::uint32_t equals = 0;
    while (equals < setting.text.length && text[equals] != '=') {
        equals += 1;
    }
    if (equals == 0u || equals >= setting.text.length) {
        return refuse_action(line, setting.index, ParseError::sysctl_malformed);
    }
    if (text[0] == '.' || text[equals - 1u] == '.') {
        return refuse_action(line, setting.index, ParseError::sysctl_malformed);
    }
    for (std::uint32_t index = 0; index < equals; index += 1) {
        if (!is_sysctl_name_byte(text[index]) || (text[index] == '.' && index + 1u < equals && text[index + 1u] == '.')) {
            return refuse_action(line, setting.index, ParseError::sysctl_malformed);
        }
    }
    SysctlCommand command{};
    command.setting = setting.text;
    command.name_length = equals;
    return ActionResult::action(CommandAction::sysctl(command));
}

[[nodiscard]] ActionResult read_action(const ConfigBuilder& builder, const LineView& line,
                                       const CommandSpan& span) noexcept {
    const Word& keyword = span_word(span, 0);
    if (word_is(builder, keyword, "mount")) {
        return read_mount(builder, line, span);
    }
    if (word_is(builder, keyword, "wait-for")) {
        return read_wait_for(builder, line, span);
    }
    if (word_is(builder, keyword, "set")) {
        return read_set(builder, line, span);
    }
    if (word_is(builder, keyword, "start")) {
        return read_start(builder, line, span);
    }
    if (word_is(builder, keyword, "run")) {
        return read_run(builder, line, span);
    }
    if (word_is(builder, keyword, "sysctl")) {
        return read_sysctl(builder, line, span);
    }
    if (word_is(builder, keyword, "if-present")) {
        // Reached only for a guarded command (parse_command takes an
        // unguarded if-present itself): guards do not nest.
        return refuse_action(line, keyword.index, ParseError::guard_nested);
    }
    return refuse_action(line, keyword.index, ParseError::command_unknown);
}

// One line of the init section. begin is its first non-blank byte.
[[nodiscard]] LineResult parse_command(ConfigBuilder& builder, const LineView& line, std::size_t begin) noexcept {
    CommandWords words{};
    const LineResult split = split_words(builder, line, begin, words);
    if (split.has_value()) {
        return split;
    }
    // begin is a non-blank byte, so there is at least one word.
    assert(words.count >= 1u);
    if (builder.commands_full()) {
        return refuse(line, begin, ParseError::too_many_commands);
    }
    std::optional<PresenceGuard> guard;
    std::size_t first = 0;
    if (word_is(builder, words.words[0], "if-present")) {
        if (words.count < 4u) {
            return refuse(line, words.words[0].index, ParseError::command_arguments_wrong);
        }
        const std::optional<PresenceKind> kind = presence_kind_of(builder, words.words[1]);
        if (!kind.has_value()) {
            return refuse(line, words.words[1].index, ParseError::presence_kind_unknown);
        }
        PresenceGuard presence{};
        presence.kind = *kind;
        presence.path = words.words[2].text;
        guard = presence;
        first = 3;
    }
    const CommandSpan span{words, first};
    const ActionResult result = read_action(builder, line, span);
    const ParseFailed* const refusal = result.as_refusal();
    if (refusal != nullptr) {
        const LineResult refused = *refusal;
        return refused;
    }
    const CommandAction* const action = result.as_action();
    if (action == nullptr) {
        // Fallback: an ActionResult is always one of its two states.
        return refuse(line, begin, ParseError::command_unknown);
    }
    const InitCommand command{guard, *action, line.number, column_of(begin)};
    builder.add_command(command);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Service settings. The line has already been accepted and stored as an
// ordinary setting (parse_setting); this reads its meaning for the service.

// Whether bytes [begin, end) of the line are exactly literal.
[[nodiscard]] bool bytes_are(const LineView& line, std::size_t begin, std::size_t end, const char* literal) noexcept {
    const std::size_t length = c_string_length(literal);
    if (end - begin != length) {
        return false;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        if (line.bytes[begin + index] != literal[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_folder_byte(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || is_lower_letter(byte) || is_digit(byte) || byte == '.' || byte == '_' ||
           byte == '-' || byte == '/';
}

// Whether bytes [begin, end) are a path's component "." or "..".
[[nodiscard]] bool is_dot_component(const char* bytes, std::size_t begin, std::size_t end) noexcept {
    const std::size_t length = end - begin;
    if (length == 1u) {
        return bytes[begin] == '.';
    }
    if (length == 2u) {
        return bytes[begin] == '.' && bytes[begin + 1u] == '.';
    }
    return false;
}

// A folder: an absolute path of folder bytes with no empty, "." or ".."
// component and no trailing '/', below target_folder_parent. Returns the
// refusal, or nothing.
[[nodiscard]] std::optional<ParseError> folder_error(const char* bytes, std::size_t length) noexcept {
    if (length < 2u || bytes[0] != '/' || bytes[length - 1u] == '/') {
        return ParseError::folder_malformed;
    }
    std::size_t component_begin = 1;
    for (std::size_t index = 1; index <= length; index += 1) {
        if (index < length && !is_folder_byte(bytes[index])) {
            return ParseError::folder_malformed;
        }
        if (index == length || bytes[index] == '/') {
            if (index == component_begin || is_dot_component(bytes, component_begin, index)) {
                return ParseError::folder_malformed;
            }
            component_begin = index + 1u;
        }
    }
    // Below the parent: "<parent>/" and at least one more component.
    const std::size_t parent_length = c_string_length(target_folder_parent);
    if (length <= parent_length + 1u || bytes[parent_length] != '/') {
        return ParseError::folder_outside_data;
    }
    for (std::size_t index = 0; index < parent_length; index += 1) {
        if (bytes[index] != target_folder_parent[index]) {
            return ParseError::folder_outside_data;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool is_pattern_byte(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || is_lower_letter(byte) || is_digit(byte) || byte == '.' || byte == '_' ||
           byte == '/' || byte == '*' || byte == '?' || byte == '+' || byte == '-';
}

// A device pattern: 1..device_pattern_length_max pattern bytes, not
// starting with '/', with no ".." component.
[[nodiscard]] bool pattern_is_valid(const char* bytes, std::size_t length) noexcept {
    if (length == 0u || length > device_pattern_length_max || bytes[0] == '/') {
        return false;
    }
    std::size_t component_begin = 0;
    for (std::size_t index = 0; index <= length; index += 1) {
        if (index < length && !is_pattern_byte(bytes[index])) {
            return false;
        }
        if (index == length || bytes[index] == '/') {
            if (index - component_begin == 2u && bytes[component_begin] == '.' && bytes[component_begin + 1u] == '.') {
                return false;
            }
            component_begin = index + 1u;
        }
    }
    return true;
}

// The words of a setting's value: a view of the line that ends where the
// value ends, so columns stay the line's.
[[nodiscard]] LineResult split_value_words(ConfigBuilder& builder, const LineView& line, std::size_t value_begin,
                                           std::size_t value_end, CommandWords& words) noexcept {
    LineView value_line = line;
    value_line.length = value_end;
    return split_words(builder, value_line, value_begin, words);
}

[[nodiscard]] LineResult read_target_devices(ConfigBuilder& builder, TargetDraft& target, const LineView& line,
                                             std::size_t value_begin, std::size_t value_end) noexcept {
    CommandWords words{};
    const LineResult split = split_value_words(builder, line, value_begin, value_end, words);
    if (split.has_value()) {
        return split;
    }
    if (words.count == 0u) {
        return refuse(line, value_begin, ParseError::device_pattern_malformed);
    }
    if (words.count > devices_max) {
        return refuse(line, words.words[devices_max].index, ParseError::too_many_devices);
    }
    ServiceDeviceWords devices{};
    devices.count = static_cast<std::uint32_t>(words.count);
    for (std::size_t index = 0; index < words.count; index += 1) {
        const Word& word = words.words[index];
        if (!pattern_is_valid(builder.text(word.text), word.text.length)) {
            return refuse(line, word.index, ParseError::device_pattern_malformed);
        }
        devices.patterns[index] = word.text;
    }
    target.devices = devices;
    target.devices_place = SettingPlace{line.number, column_of(value_begin)};
    return std::nullopt;
}

// A decimal number of bytes [begin, end): 1 to 10 digits, no sign, no
// leading zero, at most maximum; or nothing.
[[nodiscard]] std::optional<std::uint32_t> bounded_decimal(const char* bytes, std::size_t length,
                                                           std::uint32_t maximum) noexcept {
    if (length == 0u || length > 10u || (bytes[0] == '0' && length > 1u)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < length; index += 1) {
        if (!is_digit(bytes[index])) {
            return std::nullopt;
        }
        // At most 10 digits: below 10^10, far inside 64 bits.
        value = value * 10u + static_cast<std::uint64_t>(bytes[index] - '0');
    }
    if (value > maximum) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> result = static_cast<std::uint32_t>(value);
    return result;
}

// "<tcp|udp>/<1-65535>", or nothing.
[[nodiscard]] std::optional<PortDeclaration> port_of(const char* bytes, std::size_t length) noexcept {
    PortDeclaration port{};
    std::size_t number_begin = 0;
    if (length > 4u && bytes[0] == 't' && bytes[1] == 'c' && bytes[2] == 'p' && bytes[3] == '/') {
        port.protocol = PortProtocol::tcp;
        number_begin = 4;
    } else if (length > 4u && bytes[0] == 'u' && bytes[1] == 'd' && bytes[2] == 'p' && bytes[3] == '/') {
        port.protocol = PortProtocol::udp;
        number_begin = 4;
    } else {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> number = bounded_decimal(&bytes[number_begin], length - number_begin, 65535u);
    if (!number.has_value() || *number == 0u) {
        return std::nullopt;
    }
    port.port = static_cast<std::uint16_t>(*number);
    const std::optional<PortDeclaration> result = port;
    return result;
}

[[nodiscard]] LineResult read_target_ports(ConfigBuilder& builder, TargetDraft& target, const LineView& line,
                                           std::size_t value_begin, std::size_t value_end) noexcept {
    CommandWords words{};
    const LineResult split = split_value_words(builder, line, value_begin, value_end, words);
    if (split.has_value()) {
        return split;
    }
    if (words.count == 0u) {
        return refuse(line, value_begin, ParseError::port_malformed);
    }
    if (words.count > ports_max) {
        return refuse(line, words.words[ports_max].index, ParseError::too_many_ports);
    }
    TargetPortWords ports{};
    ports.count = static_cast<std::uint32_t>(words.count);
    for (std::size_t index = 0; index < words.count; index += 1) {
        const Word& word = words.words[index];
        const std::optional<PortDeclaration> port = port_of(builder.text(word.text), word.text.length);
        if (!port.has_value()) {
            return refuse(line, word.index, ParseError::port_malformed);
        }
        for (std::size_t earlier = 0; earlier < index; earlier += 1) {
            if (ports.ports[earlier].protocol == (*port).protocol && ports.ports[earlier].port == (*port).port) {
                return refuse(line, word.index, ParseError::port_repeated);
            }
        }
        ports.ports[index] = *port;
    }
    target.ports = ports;
    return std::nullopt;
}

// The command's words. A service's program is an absolute path; a
// target's is a file of its package, a relative path.
[[nodiscard]] LineResult read_service_command(ConfigBuilder& builder, ServiceDraft& draft, const LineView& line,
                                              std::size_t value_begin, std::size_t value_end) noexcept {
    CommandWords words{};
    const LineResult split = split_value_words(builder, line, value_begin, value_end, words);
    if (split.has_value()) {
        return split;
    }
    const bool is_target = draft.target.has_value();
    const ParseError program_error = is_target ? ParseError::program_path_malformed : ParseError::program_not_absolute;
    if (words.count == 0u) {
        return refuse(line, value_begin, program_error);
    }
    if (words.count > run_arguments_max) {
        return refuse(line, words.words[run_arguments_max].index, ParseError::too_many_run_arguments);
    }
    // The first word's form is checked when the section closes
    // (program_word_valid), once the section's kind is known.
    draft.program_place = SettingPlace{line.number, column_of(words.words[0].index)};
    ServiceCommandWords command{};
    command.count = static_cast<std::uint32_t>(words.count);
    for (std::size_t index = 0; index < words.count; index += 1) {
        command.arguments[index] = words.words[index].text;
    }
    draft.command = command;
    return std::nullopt;
}

// The value parse_setting has just stored.
[[nodiscard]] TextRef last_value(const ConfigBuilder& builder) noexcept {
    const BootConfig& config = builder.config();
    return config.setting(config.setting_count() - 1u).value;
}

// An https URL a line can hold: "https://", then a host of at least one
// byte, and only bytes that may appear in a URL unescaped or escaped
// (printable, no blank, no '"', '<', '>', '\\', '^', '`', '{', '|', '}').
// stxpkg parses it fully before use (package/url.h); this only keeps text
// that cannot be one out of the configuration.
[[nodiscard]] bool update_url_plausible(const char* bytes, std::size_t length) noexcept {
    const char prefix[] = "https://";
    const std::size_t prefix_length = sizeof(prefix) - 1u;
    if (length <= prefix_length) {
        return false;
    }
    for (std::size_t index = 0; index < prefix_length; index += 1) {
        if (bytes[index] != prefix[index]) {
            return false;
        }
    }
    if (bytes[prefix_length] == '/') {
        return false;
    }
    for (std::size_t index = prefix_length; index < length; index += 1) {
        const char byte = bytes[index];
        const bool excluded = byte <= ' ' || byte > '~' || byte == '"' || byte == '<' || byte == '>' ||
                              byte == '\\' || byte == '^' || byte == '`' || byte == '{' || byte == '|' || byte == '}';
        if (excluded) {
            return false;
        }
    }
    return true;
}

// Settings of a service section (parse_setting accepted the line).
[[nodiscard]] LineResult parse_service_only_setting(ConfigBuilder& builder, ServiceDraft& draft,
                                                    const LineView& line, std::size_t begin, std::size_t end_of_name,
                                                    std::size_t value_begin, std::size_t value_end) noexcept {
    if (bytes_are(line, begin, end_of_name, "directory")) {
        if (value_begin >= value_end || line.bytes[value_begin] != '/') {
            return refuse(line, value_begin, ParseError::directory_not_absolute);
        }
        draft.directory = last_value(builder);
        return std::nullopt;
    }
    // Services only: a target never gets a control channel.
    if (bytes_are(line, begin, end_of_name, "control")) {
        if (bytes_are(line, value_begin, value_end, "none")) {
            draft.control = ControlAccess::none;
        } else if (bytes_are(line, value_begin, value_end, "read")) {
            draft.control = ControlAccess::read;
        } else if (bytes_are(line, value_begin, value_end, "operate")) {
            draft.control = ControlAccess::operate;
        } else {
            return refuse(line, value_begin, ParseError::service_control_unknown);
        }
        return std::nullopt;
    }
    return refuse(line, begin, ParseError::service_setting_unknown);
}

// Settings of a target section (parse_setting accepted the line).
[[nodiscard]] LineResult parse_target_setting(ConfigBuilder& builder, TargetDraft& target, const LineView& line,
                                              std::size_t begin, std::size_t end_of_name, std::size_t value_begin,
                                              std::size_t value_end) noexcept {
    const char* const value = &line.bytes[value_begin];
    const std::size_t value_length = value_end - value_begin;
    if (bytes_are(line, begin, end_of_name, "role")) {
        if (bytes_are(line, value_begin, value_end, "foreground")) {
            target.role = TargetRole::foreground;
        } else if (bytes_are(line, value_begin, value_end, "server")) {
            target.role = TargetRole::server;
        } else {
            return refuse(line, value_begin, ParseError::target_role_unknown);
        }
        return std::nullopt;
    }
    if (bytes_are(line, begin, end_of_name, "id")) {
        const std::optional<std::uint32_t> id = bounded_decimal(value, value_length, target_id_max);
        if (!id.has_value() || *id == 0u) {
            return refuse(line, value_begin, ParseError::target_id_malformed);
        }
        target.id = *id;
        return std::nullopt;
    }
    if (bytes_are(line, begin, end_of_name, "program")) {
        const std::size_t end = name_end(line, value_begin);
        if (end != value_end || !name_is_valid(line, value_begin, value_end)) {
            return refuse(line, value_begin, ParseError::name_malformed);
        }
        target.program = last_value(builder);
        return std::nullopt;
    }
    if (bytes_are(line, begin, end_of_name, "folder")) {
        const std::optional<ParseError> error = folder_error(value, value_length);
        if (error.has_value()) {
            return refuse(line, value_begin, *error);
        }
        target.folder = last_value(builder);
        return std::nullopt;
    }
    if (bytes_are(line, begin, end_of_name, "ports")) {
        return read_target_ports(builder, target, line, value_begin, value_end);
    }
    if (bytes_are(line, begin, end_of_name, "devices")) {
        return read_target_devices(builder, target, line, value_begin, value_end);
    }
    if (bytes_are(line, begin, end_of_name, "start")) {
        if (bytes_are(line, value_begin, value_end, "boot")) {
            target.start = StartPolicy::boot;
        } else if (bytes_are(line, value_begin, value_end, "manual")) {
            target.start = StartPolicy::manual;
        } else {
            return refuse(line, value_begin, ParseError::start_policy_unknown);
        }
        return std::nullopt;
    }
    if (bytes_are(line, begin, end_of_name, "update-url")) {
        if (!update_url_plausible(value, value_length)) {
            return refuse(line, value_begin, ParseError::update_url_malformed);
        }
        target.update_url = last_value(builder);
        return std::nullopt;
    }
    if (bytes_are(line, begin, end_of_name, "update-interval")) {
        const std::optional<std::uint32_t> seconds = bounded_decimal(value, value_length, update_interval_max_s);
        if (!seconds.has_value() || *seconds < update_interval_min_s) {
            return refuse(line, value_begin, ParseError::update_interval_malformed);
        }
        target.update_interval_s = *seconds;
        target.update_interval_place = SettingPlace{line.number, column_of(begin)};
        return std::nullopt;
    }
    if (bytes_are(line, begin, end_of_name, "healthy-after")) {
        const std::optional<std::uint32_t> seconds = bounded_decimal(value, value_length, healthy_after_max_s);
        if (!seconds.has_value() || *seconds == 0u) {
            return refuse(line, value_begin, ParseError::healthy_after_malformed);
        }
        target.healthy_after_s = *seconds;
        return std::nullopt;
    }
    return refuse(line, begin, ParseError::service_setting_unknown);
}

// The settings only a target has (boot_config.h, the target section).
constexpr const char* const target_setting_names[] = {
    "role", "id", "program", "folder", "ports", "devices", "start", "update-url", "update-interval", "healthy-after",
};

[[nodiscard]] bool is_target_setting_name(const LineView& line, std::size_t begin, std::size_t end_of_name) noexcept {
    for (const char* const name : target_setting_names) {
        if (bytes_are(line, begin, end_of_name, name)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] LineResult parse_service_setting(ConfigBuilder& builder, ServiceDraft& draft, const LineView& line,
                                               std::size_t begin) noexcept {
    // parse_setting accepted the line, so this layout holds: name, blanks,
    // '=', blanks, value, trailing blanks.
    const std::size_t end_of_name = name_end(line, begin);
    const std::size_t equals = skip_blanks(line, end_of_name);
    const std::size_t value_begin = skip_blanks(line, equals + 1u);
    std::size_t value_end = line.length;
    while (value_end > value_begin && is_blank(line.bytes[value_end - 1u])) {
        value_end -= 1;
    }
    if (bytes_are(line, begin, end_of_name, "command")) {
        return read_service_command(builder, draft, line, value_begin, value_end);
    }
    if (bytes_are(line, begin, end_of_name, "restart")) {
        if (bytes_are(line, value_begin, value_end, "never")) {
            draft.restart = RestartPolicy::never;
        } else if (bytes_are(line, value_begin, value_end, "on-failure")) {
            draft.restart = RestartPolicy::on_failure;
        } else if (bytes_are(line, value_begin, value_end, "always")) {
            draft.restart = RestartPolicy::always;
        } else {
            return refuse(line, value_begin, ParseError::service_restart_unknown);
        }
        return std::nullopt;
    }
    // A [service-<name>] section that names a target setting is a target
    // (C2: the web configuration writes targets so, sc2's C2-web.md section
    // 3.3 and section 7 item 4; appliance/C2_DESIGN.md, "Targets"). Its
    // first such line makes the draft a target's, provided the section has
    // set no service-only setting so far.
    if (!draft.target.has_value() && is_target_setting_name(line, begin, end_of_name)) {
        if (draft.directory.has_value() || draft.control.has_value()) {
            return refuse(line, begin, ParseError::target_setting_in_service);
        }
        draft.target = TargetDraft{};
    }
    if (draft.target.has_value()) {
        return parse_target_setting(builder, *draft.target, line, begin, end_of_name, value_begin, value_end);
    }
    return parse_service_only_setting(builder, draft, line, begin, end_of_name, value_begin, value_end);
}

// An IPv4 address with a prefix length: "<dotted quad>/<0-32>".
[[nodiscard]] bool address_with_prefix_valid(const char* bytes, std::size_t length) noexcept {
    std::size_t slash = 0;
    while (slash < length && bytes[slash] != '/') {
        slash += 1;
    }
    if (slash == length) {
        return false;
    }
    if (!ipv4_of(bytes, slash).has_value()) {
        return false;
    }
    return bounded_decimal(&bytes[slash + 1u], length - slash - 1u, 32u).has_value();
}

// An interface name as ifconfig(8) prints one: 1..15 bytes (IFNAMSIZ 16
// with its NUL) of [a-z0-9], a letter first.
[[nodiscard]] bool interface_name_valid(const char* bytes, std::size_t length) noexcept {
    if (length == 0u || length > 15u || !is_lower_letter(bytes[0])) {
        return false;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        if (!is_lower_letter(bytes[index]) && !is_digit(bytes[index])) {
            return false;
        }
    }
    return true;
}

// [network]: only the settings the format lists, each value well formed.
// Whether the static settings agree with the mode is stxnet's to report
// (it has the whole section); here each value is checked alone.
[[nodiscard]] LineResult parse_network_setting(ConfigBuilder& builder, const LineView& line,
                                               std::size_t begin) noexcept {
    const std::size_t end_of_name = name_end(line, begin);
    const std::size_t equals = skip_blanks(line, end_of_name);
    const std::size_t value_begin = skip_blanks(line, equals + 1u);
    std::size_t value_end = line.length;
    while (value_end > value_begin && is_blank(line.bytes[value_end - 1u])) {
        value_end -= 1;
    }
    const char* const value = &line.bytes[value_begin];
    const std::size_t value_length = value_end - value_begin;
    bool valid = false;
    if (bytes_are(line, begin, end_of_name, "mode")) {
        valid = bytes_are(line, value_begin, value_end, "dhcp") || bytes_are(line, value_begin, value_end, "static");
    } else if (bytes_are(line, begin, end_of_name, "interface")) {
        valid = interface_name_valid(value, value_length);
    } else if (bytes_are(line, begin, end_of_name, "address")) {
        valid = address_with_prefix_valid(value, value_length);
    } else if (bytes_are(line, begin, end_of_name, "gateway")) {
        valid = ipv4_of(value, value_length).has_value();
    } else if (bytes_are(line, begin, end_of_name, "dns")) {
        CommandWords words{};
        const LineResult split = split_value_words(builder, line, value_begin, value_end, words);
        if (split.has_value()) {
            return split;
        }
        valid = words.count >= 1u && words.count <= name_servers_max;
        for (std::size_t index = 0; valid && index < words.count; index += 1) {
            valid = ipv4_of(builder.text(words.words[index].text), words.words[index].text.length).has_value();
        }
    } else {
        return refuse(line, begin, ParseError::network_setting_unknown);
    }
    if (!valid) {
        return refuse(line, value_begin, ParseError::network_value_malformed);
    }
    return std::nullopt;
}

// Whether one folder is the other or contains it (both valid folders, so
// a component boundary is a '/').
[[nodiscard]] bool folders_overlap(const char* left, const char* right) noexcept {
    std::size_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index += 1;
    }
    if (left[index] == '\0') {
        return right[index] == '\0' || right[index] == '/';
    }
    return right[index] == '\0' && left[index] == '/';
}

// After the last line: names unique across services and targets; target
// ids unique; at most one foreground target; no listener declared twice;
// no folder shared or nested. Each refusal is placed at the later
// definition's header.
[[nodiscard]] LineResult check_definitions(const BootConfig& config) noexcept {
    std::size_t foreground_count = 0;
    for (std::size_t index = 0; index < config.service_count(); index += 1) {
        const ServiceDefinition& later = config.service(index);
        for (std::size_t earlier_index = 0; earlier_index < index; earlier_index += 1) {
            const ServiceDefinition& earlier = config.service(earlier_index);
            if (c_strings_equal(config.text(earlier.name), config.text(later.name))) {
                return refuse_at(later.line, 1, ParseError::name_repeated);
            }
        }
        if (!later.target.has_value()) {
            continue;
        }
        const TargetSettings& target = *later.target;
        if (target.role == TargetRole::foreground) {
            foreground_count += 1;
            if (foreground_count > 1u) {
                return refuse_at(later.line, 1, ParseError::second_foreground);
            }
        }
        for (std::size_t earlier_index = 0; earlier_index < index; earlier_index += 1) {
            const ServiceDefinition& earlier = config.service(earlier_index);
            if (!earlier.target.has_value()) {
                continue;
            }
            const TargetSettings& other = *earlier.target;
            if (other.id != 0u && other.id == target.id) {
                return refuse_at(later.line, 1, ParseError::target_id_repeated);
            }
            if (folders_overlap(config.text(other.folder), config.text(target.folder))) {
                return refuse_at(later.line, 1, ParseError::folder_repeated);
            }
            for (std::uint32_t mine = 0; mine < target.port_count; mine += 1) {
                for (std::uint32_t theirs = 0; theirs < other.port_count; theirs += 1) {
                    if (target.ports[mine].protocol == other.ports[theirs].protocol &&
                        target.ports[mine].port == other.ports[theirs].port) {
                        return refuse_at(later.line, 1, ParseError::port_repeated);
                    }
                }
            }
        }
    }
    return std::nullopt;
}

// After the last line: every start names a defined service or target.
[[nodiscard]] LineResult check_starts(const BootConfig& config) noexcept {
    for (std::size_t index = 0; index < config.init_command_count(); index += 1) {
        const InitCommand& command = config.init_command(index);
        const StartCommand* const start = command.action.as_start();
        if (start != nullptr && !config.find_service(config.text((*start).service)).has_value()) {
            return refuse_at(command.line, command.column, ParseError::start_unknown_service);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// One line, classified by its first non-blank byte.

[[nodiscard]] LineResult parse_line(ConfigBuilder& builder, ParseState& state, const LineView& line) noexcept {
    if (line.length > line_length_max) {
        return refuse(line, line_length_max, ParseError::line_too_long);
    }
    for (std::size_t index = 0; index < line.length; index += 1) {
        if (!is_allowed_byte(line.bytes[index])) {
            return refuse(line, index, ParseError::byte_not_allowed);
        }
    }
    const std::size_t first = skip_blanks(line, 0);
    if (first == line.length || line.bytes[first] == '#') {
        return std::nullopt;
    }
    if (line.bytes[first] == '[') {
        return parse_header(builder, state, line, first);
    }
    if (state.in_init_section) {
        return parse_command(builder, line, first);
    }
    const LineResult setting = parse_setting(builder, state, line, first);
    if (setting.has_value()) {
        return setting;
    }
    if (state.in_network_section) {
        return parse_network_setting(builder, line, first);
    }
    if (!state.service.has_value()) {
        return setting;
    }
    return parse_service_setting(builder, *state.service, line, first);
}

}  // namespace

ParseOutcome parse_boot_config(std::span<const char> text, BootConfig& config) noexcept {
    config.clear();
    if (text.size() > config_size_max) {
        return ParseOutcome::failed(1, 1, ParseError::file_too_large);
    }
    ConfigBuilder builder(config);
    ParseState state{};
    state.in_init_section = false;
    state.in_network_section = false;
    const char* const bytes = text.data();
    const std::size_t size = text.size();
    std::size_t line_begin = 0;
    std::uint32_t line_number = 1;
    // Each iteration consumes one line, at least its LF, except the last
    // line, which ends the loop; so the loop runs at most size + 1 times.
    while (line_begin <= size) {
        std::size_t line_end = line_begin;
        while (line_end < size && bytes[line_end] != '\n') {
            line_end += 1;
        }
        const bool at_text_end = line_end == size;
        if (at_text_end && line_begin == size) {
            // Text ending in LF (or empty): no further line.
            break;
        }
        std::size_t content_end = line_end;
        if (content_end > line_begin && bytes[content_end - 1u] == '\r') {
            content_end -= 1;
        }
        LineView line{};
        line.bytes = &bytes[line_begin];
        line.length = content_end - line_begin;
        line.number = line_number;
        const LineResult result = parse_line(builder, state, line);
        if (result.has_value()) {
            config.clear();
            return ParseOutcome::failed((*result).line, (*result).column, (*result).error);
        }
        if (at_text_end) {
            break;
        }
        line_begin = line_end + 1u;
        line_number += 1;
    }
    LineResult closing = finish_service(builder, state);
    if (!closing.has_value()) {
        closing = check_definitions(config);
    }
    if (!closing.has_value()) {
        closing = check_starts(config);
    }
    if (closing.has_value()) {
        config.clear();
        return ParseOutcome::failed((*closing).line, (*closing).column, (*closing).error);
    }
    return ParseOutcome::succeeded();
}

std::uint32_t target_identity(const TargetSettings& target) noexcept {
    // id is at most target_id_max, so the sum stays far inside 32 bits.
    return target_identity_base + target.id;
}

bool package_path_valid(const char* bytes, std::size_t length) noexcept {
    if (length == 0u || length > package_path_length_max || bytes[0] == '/' || bytes[length - 1u] == '/') {
        return false;
    }
    std::size_t component_begin = 0;
    for (std::size_t index = 0; index <= length; index += 1) {
        if (index < length && !is_folder_byte(bytes[index])) {
            return false;
        }
        if (index == length || bytes[index] == '/') {
            if (index == component_begin || is_dot_component(bytes, component_begin, index)) {
                return false;
            }
            component_begin = index + 1u;
        }
    }
    return true;
}

std::optional<std::uint32_t> ipv4_of(const char* bytes, std::size_t length) noexcept {
    std::uint32_t address = 0;
    std::size_t index = 0;
    for (std::uint32_t part = 0; part < 4u; part += 1) {
        if (part > 0u) {
            if (index >= length || bytes[index] != '.') {
                return std::nullopt;
            }
            index += 1;
        }
        const std::size_t begin = index;
        while (index < length && is_digit(bytes[index]) && index - begin < 3u) {
            index += 1;
        }
        const std::optional<std::uint32_t> value = bounded_decimal(&bytes[begin], index - begin, 255u);
        if (!value.has_value()) {
            return std::nullopt;
        }
        address = (address << 8u) | *value;
    }
    if (index != length) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> result = address;
    return result;
}

}  // namespace stxconfig
