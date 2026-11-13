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
    case ParseError::service_control_unknown: return "service_control_unknown";
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

// The service section being read; it becomes a ServiceDefinition when the
// next header or the end of the text closes it.
struct ServiceDraft {
    TextRef name;
    std::uint32_t line;
    std::uint32_t name_column;
    std::optional<ServiceCommandWords> command;
    std::optional<TextRef> directory;
    std::optional<RestartPolicy> restart;
    std::optional<ControlAccess> control;
};

// The parse's state between lines.
struct ParseState {
    // Index of the current section, or nothing before the first header.
    std::optional<std::uint32_t> section_index;
    bool in_init_section;
    // Present while the current section is a service section.
    std::optional<ServiceDraft> service;
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

// Closes the current service section, if any: a service needs a command.
[[nodiscard]] LineResult finish_service(ConfigBuilder& builder, ParseState& state) noexcept {
    if (!state.service.has_value()) {
        return std::nullopt;
    }
    const ServiceDraft draft = *state.service;
    state.service.reset();
    if (!draft.command.has_value()) {
        return refuse_at(draft.line, draft.name_column, ParseError::service_command_missing);
    }
    ServiceDefinition service{};
    service.name = draft.name;
    service.argument_count = (*draft.command).count;
    for (std::uint32_t index = 0; index < (*draft.command).count; index += 1) {
        service.arguments[index] = (*draft.command).arguments[index];
    }
    service.directory = draft.directory;
    service.restart = draft.restart.has_value() ? *draft.restart : RestartPolicy::on_failure;
    service.control = draft.control.has_value() ? *draft.control : ControlAccess::none;
    service.line = draft.line;
    // Room was checked when the section began.
    builder.add_service(service);
    return std::nullopt;
}

// Whether the stored section name starts with the service prefix.
[[nodiscard]] bool has_service_prefix(const char* name) noexcept {
    std::size_t index = 0;
    while (service_section_prefix[index] != '\0') {
        if (name[index] != service_section_prefix[index]) {
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
    if (has_service_prefix(builder.text(*name))) {
        const std::size_t service_begin = name_begin + c_string_length(service_section_prefix);
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
        service = draft;
    }
    Section section{};
    section.name = *name;
    section.line = line.number;
    const std::uint32_t index = builder.add_section(section);
    state.section_index = index;
    state.in_init_section = c_strings_equal(builder.text(*name), "init");
    state.service = service;
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

[[nodiscard]] LineResult read_service_command(ConfigBuilder& builder, ServiceDraft& draft, const LineView& line,
                                              std::size_t value_begin, std::size_t value_end) noexcept {
    // The words of the value only: a view of the line that ends where the
    // value ends, so columns stay the line's.
    LineView value_line = line;
    value_line.length = value_end;
    CommandWords words{};
    const LineResult split = split_words(builder, value_line, value_begin, words);
    if (split.has_value()) {
        return split;
    }
    if (words.count == 0u) {
        return refuse(line, value_begin, ParseError::program_not_absolute);
    }
    if (words.count > run_arguments_max) {
        return refuse(line, words.words[run_arguments_max].index, ParseError::too_many_run_arguments);
    }
    if (words.words[0].text.length == 0u || builder.text(words.words[0].text)[0] != '/') {
        return refuse(line, words.words[0].index, ParseError::program_not_absolute);
    }
    ServiceCommandWords command{};
    command.count = static_cast<std::uint32_t>(words.count);
    for (std::size_t index = 0; index < words.count; index += 1) {
        command.arguments[index] = words.words[index].text;
    }
    draft.command = command;
    return std::nullopt;
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
    if (bytes_are(line, begin, end_of_name, "directory")) {
        if (value_begin >= value_end || line.bytes[value_begin] != '/') {
            return refuse(line, value_begin, ParseError::directory_not_absolute);
        }
        // The value parse_setting has just stored.
        const BootConfig& config = builder.config();
        draft.directory = config.setting(config.setting_count() - 1u).value;
        return std::nullopt;
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

// After the last line: every start names a defined service.
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
    if (setting.has_value() || !state.service.has_value()) {
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
        closing = check_starts(config);
    }
    if (closing.has_value()) {
        config.clear();
        return ParseOutcome::failed((*closing).line, (*closing).column, (*closing).error);
    }
    return ParseOutcome::succeeded();
}

}  // namespace stxconfig
