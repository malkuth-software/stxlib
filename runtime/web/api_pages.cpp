// api_pages.cpp: the web interface's 1.0 pages (api.h): the user
// configuration, targets, programs, uploads, the system update, network
// and access.

#include <cassert>

#include "../supervisor/config/user_config.h"
#include "api.h"
#include "api_internal.h"

namespace stxweb {

namespace {

using api_detail::http_status_of;
using api_detail::write_service_record;
using api_detail::write_slot;

[[nodiscard]] bool is_digit(char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

[[nodiscard]] bool is_alpha(char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

// A decimal number without leading zeros at text[at..], at most maximum;
// advances at. Nothing when malformed.
[[nodiscard]] std::optional<std::uint32_t> read_small_decimal(std::span<const char> text, std::size_t& at,
                                                             std::uint32_t maximum) noexcept {
    const std::size_t begin = at;
    std::uint32_t value = 0;
    while (at < text.size() && is_digit(text[at]) && at - begin < 3u) {
        value = value * 10u + static_cast<std::uint32_t>(text[at] - '0');
        at += 1;
    }
    const std::size_t length = at - begin;
    if (length == 0u || (length > 1u && text[begin] == '0') || value > maximum ||
        (at < text.size() && is_digit(text[at]))) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> found = value;
    return found;
}

// Four dotted numbers from text[0..]; advances at to their end.
[[nodiscard]] bool read_ipv4(std::span<const char> text, std::size_t& at) noexcept {
    for (std::size_t part = 0; part < 4u; part += 1) {
        if (part > 0u) {
            if (at >= text.size() || text[at] != '.') {
                return false;
            }
            at += 1;
        }
        if (!read_small_decimal(text, at, 255u).has_value()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool equals_text(std::span<const char> bytes, const char* text) noexcept {
    return equals_exact(bytes, text);
}

[[nodiscard]] std::span<const char> text_of(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return std::span<const char>(text, length);
}

// The key types an [access] key may have.
const char* const ssh_key_types[] = {
    "ssh-ed25519",
    "ecdsa-sha2-nistp256",
    "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521",
    "sk-ssh-ed25519@openssh.com",
    "sk-ecdsa-sha2-nistp256@openssh.com",
};

// The [access] setting names, in order.
const char* const ssh_key_names[ssh_keys_max] = {
    "ssh-key-1", "ssh-key-2", "ssh-key-3", "ssh-key-4", "ssh-key-5", "ssh-key-6", "ssh-key-7", "ssh-key-8",
};

const char service_prefix[] = "service-";

// "service-" + name into out (NUL-terminated); its span.
[[nodiscard]] std::span<const char> target_section(const stxcontrol::ServiceName& name,
                                                   char (&out)[sizeof(service_prefix) + stxcontrol::service_name_bytes_max]) noexcept {
    std::size_t size = 0;
    for (std::size_t index = 0; index + 1u < sizeof(service_prefix); index += 1) {
        out[size] = service_prefix[index];
        size += 1;
    }
    for (std::size_t index = 0; index < name.length; index += 1) {
        out[size] = name.bytes[index];
        size += 1;
    }
    out[size] = '\0';
    return std::span<const char>(out, size);
}

// The JSON value index of an object's member when it is a string; else
// nothing.
[[nodiscard]] std::optional<std::size_t> string_member(const JsonDocument& document, std::size_t object,
                                                       const char* key) noexcept {
    const std::optional<std::size_t> value = document.member(object, key);
    if (!value.has_value() || document.token(*value).kind != JsonKind::string) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Value rules.

bool hostname_valid(std::span<const char> text) noexcept {
    if (text.empty() || text.size() > 253u) {
        return false;
    }
    std::size_t label_length = 0;
    for (std::size_t index = 0; index < text.size(); index += 1) {
        const char byte = text[index];
        if (byte == '.') {
            if (label_length == 0u || text[index - 1u] == '-') {
                return false;
            }
            label_length = 0;
            continue;
        }
        if (!is_alpha(byte) && !is_digit(byte) && byte != '-') {
            return false;
        }
        if (byte == '-' && label_length == 0u) {
            return false;
        }
        label_length += 1;
        if (label_length > 63u) {
            return false;
        }
    }
    return label_length > 0u && text[text.size() - 1u] != '-';
}

bool ipv4_text_valid(std::span<const char> text) noexcept {
    std::size_t at = 0;
    return read_ipv4(text, at) && at == text.size();
}

bool ipv4_prefix_text_valid(std::span<const char> text) noexcept {
    std::size_t at = 0;
    if (!read_ipv4(text, at) || at >= text.size() || text[at] != '/') {
        return false;
    }
    at += 1;
    return read_small_decimal(text, at, 32u).has_value() && at == text.size();
}

bool ssh_key_valid(std::span<const char> text) noexcept {
    std::size_t type_length = 0;
    while (type_length < text.size() && text[type_length] != ' ') {
        type_length += 1;
    }
    bool type_known = false;
    for (const char* const type : ssh_key_types) {
        if (equals_text(text.subspan(0, type_length), type)) {
            type_known = true;
        }
    }
    if (!type_known || type_length >= text.size()) {
        return false;
    }
    std::size_t at = type_length + 1u;
    const std::size_t data_begin = at;
    bool padding = false;
    while (at < text.size() && text[at] != ' ') {
        const char byte = text[at];
        if (byte == '=') {
            padding = true;
        } else if (padding || !(is_alpha(byte) || is_digit(byte) || byte == '+' || byte == '/')) {
            return false;
        }
        at += 1;
    }
    const std::size_t data_length = at - data_begin;
    if (data_length == 0u || data_length % 4u != 0u) {
        return false;
    }
    if (at == text.size()) {
        return true;
    }
    // A comment: visible ASCII and spaces, not ending in a blank (the
    // configuration's value rule).
    at += 1;
    if (at == text.size() || text[text.size() - 1u] == ' ') {
        return false;
    }
    for (; at < text.size(); at += 1) {
        const char byte = text[at];
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Uploads.

std::optional<UploadKind> upload_kind_of(std::span<const char> path) noexcept {
    if (equals_text(path, "/api/upload/package")) {
        const std::optional<UploadKind> kind = UploadKind::package;
        return kind;
    }
    if (equals_text(path, "/api/upload/system")) {
        const std::optional<UploadKind> kind = UploadKind::system;
        return kind;
    }
    return std::nullopt;
}

std::uint64_t upload_bytes_max(UploadKind kind) noexcept {
    switch (kind) {
    case UploadKind::package: return upload_package_bytes_max;
    case UploadKind::system: return upload_system_bytes_max;
    default: break;
    }
    // Fallback: an unknown kind takes nothing.
    return 0;
}

const char* upload_path_of(UploadKind kind) noexcept {
    switch (kind) {
    case UploadKind::package: return stxcontrol::staged_package_path;
    case UploadKind::system: return stxcontrol::staged_system_path;
    default: break;
    }
    // Fallback: an unknown kind is staged as a package (never reached:
    // upload_kind_of makes only known kinds).
    return stxcontrol::staged_package_path;
}

UploadDecision::UploadDecision(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

UploadDecision UploadDecision::accepted(UploadKind kind) noexcept {
    UploadAccepted state{};
    state.kind = kind;
    const UploadDecision decision(Alternatives(std::in_place_type<UploadAccepted>, state));
    return decision;
}

UploadDecision UploadDecision::refused(const ApiResponse& response) noexcept {
    const UploadDecision decision(Alternatives(std::in_place_type<ApiResponse>, response));
    return decision;
}

const UploadAccepted* UploadDecision::as_accepted() const noexcept {
    return std::get_if<UploadAccepted>(&alternatives_);
}

const ApiResponse* UploadDecision::as_refused() const noexcept {
    return std::get_if<ApiResponse>(&alternatives_);
}

UploadDecision Api::begin_upload(const ApiRequest& request, std::uint64_t content_length) noexcept {
    ApiResponse response = api_detail::blank_response(std::span<const char>(body_, 0));
    body_size_ = 0;
    const std::optional<UploadKind> kind = upload_kind_of(request.path);
    if (!kind.has_value()) {
        respond_error(response, 404, "not_found");
        return UploadDecision::refused(response);
    }
    if (request.method != Method::post) {
        respond_error(response, 405, "method_not_allowed");
        response.allow = "POST";
        return UploadDecision::refused(response);
    }
    if (!request.content_type.has_value() || !equals_lower(*request.content_type, "application/octet-stream")) {
        respond_error(response, 415, "unsupported_media_type");
        response.event.kind = ApiEventKind::refused;
        response.event.detail = "content-type";
        return UploadDecision::refused(response);
    }
    if (!origin_acceptable(request)) {
        respond_error(response, 403, "origin_refused");
        response.event.kind = ApiEventKind::refused;
        response.event.detail = "origin";
        return UploadDecision::refused(response);
    }
    const std::optional<std::size_t> session = session_of(request);
    if (!session.has_value()) {
        respond_error(response, 401, "not_logged_in");
        return UploadDecision::refused(response);
    }
    if (!request.csrf_token.has_value() || !sessions_.csrf_matches(*session, *request.csrf_token)) {
        respond_error(response, 403, "csrf_failed");
        response.event.kind = ApiEventKind::refused;
        response.event.detail = "csrf";
        return UploadDecision::refused(response);
    }
    if (content_length == 0u || content_length > upload_bytes_max(*kind)) {
        respond_error(response, 413, content_length == 0u ? "upload_empty" : "upload_too_large");
        return UploadDecision::refused(response);
    }
    return UploadDecision::accepted(*kind);
}

ApiResponse Api::finish_upload(UploadKind kind, const UploadSummary& summary) noexcept {
    ApiResponse response = api_detail::blank_response(std::span<const char>(body_, 0));
    body_size_ = 0;
    response.event.kind = ApiEventKind::operated;
    response.event.detail = kind == UploadKind::package ? "upload-package" : "upload-system";
    response.event.status = summary.stored ? "ok" : "failed";
    if (!summary.stored) {
        respond_error(response, 500, summary.error_number == 0 ? "upload_incomplete" : "upload_store_failed");
        return response;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("staged");
    writer.string(kind == UploadKind::package ? "package" : "system");
    writer.key("bytes");
    writer.unsigned_integer(summary.bytes);
    writer.key("sha256");
    writer.string(summary.sha256);
    writer.end_object();
    body_size_ = writer.ok() ? writer.size() : 0u;
    respond_json(response, writer.ok() ? 200 : 500);
    return response;
}

// ---------------------------------------------------------------------------
// The user configuration.

bool Api::supervisor_ok(const std::optional<stxcontrol::ResponseView>& answer, stxcontrol::Command command,
                        ApiResponse& response) noexcept {
    if (!answer.has_value() || (*answer).command != command) {
        respond_error(response, 503, "supervisor_unavailable");
        return false;
    }
    if ((*answer).status != stxcontrol::Status::ok) {
        respond_error(response, http_status_of((*answer).status), stxcontrol::status_name((*answer).status));
        return false;
    }
    return true;
}

bool Api::fetch_user_config() noexcept {
    fetch_failure_ = std::nullopt;
    const stxcontrol::Request request = stxcontrol::request_without_name(stxcontrol::Command::config_read, 0);
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    if (!answer.has_value() || (*answer).command != stxcontrol::Command::config_read) {
        return false;
    }
    if ((*answer).status != stxcontrol::Status::ok) {
        fetch_failure_ = (*answer).status;
        return false;
    }
    const std::optional<stxcontrol::ConfigTextView> view = stxcontrol::decode_config_text((*answer).payload);
    if (!view.has_value()) {
        return false;
    }
    // char may alias the bytes.
    const std::span<const char> text(reinterpret_cast<const char*>((*view).text.data()), (*view).text.size());
    if (!editor_.load(text)) {
        return false;
    }
    user_source_ = (*view).source;
    user_sequence_ = (*view).sequence;
    user_written_since_start_ = (*view).written_since_boot;
    return true;
}

bool Api::load_user_config(ApiResponse& response) noexcept {
    if (fetch_user_config()) {
        return true;
    }
    if (fetch_failure_.has_value()) {
        respond_error(response, http_status_of(*fetch_failure_), stxcontrol::status_name(*fetch_failure_));
    } else {
        respond_error(response, 503, "supervisor_unavailable");
    }
    return false;
}

bool Api::parse_user_config(ApiResponse& response) noexcept {
    const stxconfig::ParseOutcome parsed = stxconfig::parse_boot_config(editor_.text(), check_config_);
    if (!parsed.is_success()) {
        respond_error(response, 500, "settings_unparsable");
        return false;
    }
    return true;
}

bool Api::edit_done(EditResult result, ApiResponse& response) noexcept {
    if (result == EditResult::done) {
        return true;
    }
    respond_error(response, result == EditResult::too_large ? 413 : 400, edit_result_name(result));
    return false;
}

void Api::write_user_config(std::span<const char> text, ApiResponse& response) noexcept {
    response.event.kind = ApiEventKind::operated;
    response.event.detail = "config-write";
    // The protocol carries one byte at least; an empty configuration is
    // written as one line end, which parses the same.
    const char line_end[] = "\n";
    const std::span<const char> sent = text.empty() ? std::span<const char>(line_end, 1) : text;
    // Parsed here first, so that a refused text is answered in detail
    // without a round trip and never meets the protocol's byte rule.
    const stxconfig::ParseOutcome parsed = stxconfig::parse_boot_config(sent, check_config_);
    const stxconfig::ParseFailed* const failure = parsed.as_failure();
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    if (failure != nullptr) {
        writer.begin_object();
        writer.key("error");
        writer.string("config_refused");
        writer.key("kind");
        writer.string("parse");
        writer.key("reason");
        writer.string(stxconfig::parse_error_name((*failure).error));
        writer.key("line");
        writer.unsigned_integer((*failure).line);
        writer.key("column");
        writer.unsigned_integer((*failure).column);
        writer.end_object();
        body_size_ = writer.ok() ? writer.size() : 0u;
        respond_json(response, 422);
        response.event.status = "config_refused";
        return;
    }
    if (sent.size() > sizeof(request_payload_)) {
        respond_error(response, 413, "config_too_large");
        return;
    }
    for (std::size_t index = 0; index < sent.size(); index += 1) {
        request_payload_[index] = static_cast<std::uint8_t>(sent[index]);
    }
    const stxcontrol::Request request = stxcontrol::request_with_payload(
        stxcontrol::request_without_name(stxcontrol::Command::config_write, 0),
        std::span<const std::uint8_t>(request_payload_, sent.size()));
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    response.event.status = answer.has_value() ? stxcontrol::status_name((*answer).status) : "unavailable";
    if (answer.has_value() && (*answer).command == stxcontrol::Command::config_write &&
        (*answer).status == stxcontrol::Status::config_refused) {
        const std::optional<stxcontrol::ConfigRefusal> refusal = stxcontrol::decode_config_refusal((*answer).payload);
        if (!refusal.has_value()) {
            respond_error(response, 503, "supervisor_unavailable");
            return;
        }
        const char* reason = "unknown";
        if ((*refusal).kind == stxcontrol::RefusalKind::user_rule) {
            const std::optional<stxconfig::UserRule> rule = stxconfig::user_rule_from_code((*refusal).code);
            reason = rule.has_value() ? stxconfig::user_rule_name(*rule) : "unknown";
        } else {
            reason = stxconfig::parse_error_name(static_cast<stxconfig::ParseError>((*refusal).code));
        }
        writer.begin_object();
        writer.key("error");
        writer.string("config_refused");
        writer.key("kind");
        writer.string((*refusal).kind == stxcontrol::RefusalKind::user_rule ? "rule" : "parse");
        writer.key("reason");
        writer.string(reason);
        writer.key("line");
        writer.unsigned_integer((*refusal).line);
        writer.key("column");
        writer.unsigned_integer((*refusal).column);
        writer.end_object();
        body_size_ = writer.ok() ? writer.size() : 0u;
        respond_json(response, 422);
        return;
    }
    if (!supervisor_ok(answer, stxcontrol::Command::config_write, response)) {
        return;
    }
    const std::optional<stxcontrol::ConfigWritten> written = stxcontrol::decode_config_written((*answer).payload);
    if (!written.has_value()) {
        respond_error(response, 503, "supervisor_unavailable");
        return;
    }
    writer.begin_object();
    writer.key("sequence");
    writer.unsigned_integer((*written).sequence);
    writer.key("pending");
    writer.boolean(true);
    writer.end_object();
    body_size_ = writer.ok() ? writer.size() : 0u;
    respond_json(response, 200);
}

void Api::write_section_settings(JsonWriter& writer, const char* section) const noexcept {
    writer.begin_object();
    for (std::size_t index = 0; index < check_config_.section_count(); index += 1) {
        if (!equals_text(text_of(check_config_.text(check_config_.section(index).name)), section)) {
            continue;
        }
        for (std::size_t setting = 0; setting < check_config_.setting_count(); setting += 1) {
            const stxconfig::Setting& item = check_config_.setting(setting);
            if (item.section_index != index) {
                continue;
            }
            writer.key(check_config_.text(item.name));
            writer.string(check_config_.text(item.value));
        }
    }
    writer.end_object();
}

void Api::handle_settings_read(ApiResponse& response) noexcept {
    if (!load_user_config(response)) {
        return;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("source");
    writer.string(stxcontrol::config_source_name(user_source_));
    writer.key("sequence");
    writer.unsigned_integer(user_sequence_);
    writer.key("pending");
    writer.boolean(user_written_since_start_);
    writer.key("text");
    writer.string(editor_.text());
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

void Api::handle_settings_write(ApiResponse& response) noexcept {
    const std::optional<std::size_t> text_member = string_member(document_, 0, "text");
    const std::optional<std::size_t> base_member = document_.member(0, "base_sequence");
    if (!text_member.has_value() || !base_member.has_value() ||
        document_.token(*base_member).kind != JsonKind::number || document_.token(*base_member).integer < 0) {
        respond_error(response, 400, "text_or_base_sequence_missing");
        return;
    }
    const std::optional<std::size_t> size =
        document_.decode_string(*text_member, std::span<char>(config_text_, stxconfig::config_size_max));
    if (!size.has_value()) {
        respond_error(response, 413, "config_too_large");
        return;
    }
    if (!load_user_config(response)) {
        return;
    }
    const std::int64_t base = document_.token(*base_member).integer;
    if (static_cast<std::int64_t>(user_sequence_) != base) {
        JsonWriter writer(std::span<char>(body_, sizeof(body_)));
        writer.begin_object();
        writer.key("error");
        writer.string("conflict");
        writer.key("sequence");
        writer.unsigned_integer(user_sequence_);
        writer.end_object();
        body_size_ = writer.ok() ? writer.size() : 0u;
        respond_json(response, 409);
        return;
    }
    write_user_config(std::span<const char>(config_text_, *size), response);
}

void Api::handle_settings_check(ApiResponse& response) noexcept {
    const std::optional<std::size_t> member = string_member(document_, 0, "text");
    if (!member.has_value()) {
        respond_error(response, 400, "text_missing");
        return;
    }
    // One byte more than the format allows: a longer text is refused by
    // the parser as file_too_large, as the supervisor would refuse it.
    const std::optional<std::size_t> size =
        document_.decode_string(*member, std::span<char>(config_text_, stxconfig::config_size_max + 1u));
    const std::size_t text_size = size.has_value() ? *size : stxconfig::config_size_max + 1u;
    const stxconfig::ParseOutcome outcome =
        stxconfig::parse_boot_config(std::span<const char>(config_text_, text_size), check_config_);
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    const stxconfig::ParseFailed* const failure = outcome.as_failure();
    if (failure != nullptr) {
        writer.key("ok");
        writer.boolean(false);
        writer.key("kind");
        writer.string("parse");
        writer.key("reason");
        writer.string(stxconfig::parse_error_name((*failure).error));
        writer.key("line");
        writer.unsigned_integer((*failure).line);
        writer.key("column");
        writer.unsigned_integer((*failure).column);
    } else {
        // The parsed configuration copied its texts, so the buffer can take
        // the factory configuration now. Without one, no name is taken.
        const ConfigOutcome factory = backend_.read_config(std::span<char>(config_text_, stxconfig::config_size_max));
        const ConfigRead* const read = factory.as_read();
        bool factory_parsed = false;
        if (read != nullptr && (*read).size <= stxconfig::config_size_max) {
            factory_parsed = stxconfig::parse_boot_config(std::span<const char>(config_text_, (*read).size),
                                                          factory_config_)
                                 .is_success();
        }
        if (!factory_parsed) {
            factory_config_.clear();
        }
        const std::optional<stxconfig::UserRuleBroken> broken =
            stxconfig::check_user_config(check_config_, factory_config_);
        writer.key("ok");
        writer.boolean(!broken.has_value());
        if (broken.has_value()) {
            writer.key("kind");
            writer.string("rule");
            writer.key("reason");
            writer.string(stxconfig::user_rule_name((*broken).rule));
            writer.key("line");
            writer.unsigned_integer((*broken).line);
            writer.key("column");
            writer.unsigned_integer((*broken).column);
        } else {
            writer.key("sections");
            writer.unsigned_integer(check_config_.section_count());
            writer.key("settings");
            writer.unsigned_integer(check_config_.setting_count());
            writer.key("targets");
            writer.unsigned_integer(check_config_.service_count());
        }
        writer.key("factory_checked");
        writer.boolean(factory_parsed);
    }
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

// ---------------------------------------------------------------------------
// Targets and programs.

void Api::write_program_record(JsonWriter& writer, const stxcontrol::ProgramRecord& record) const noexcept {
    writer.begin_object();
    writer.key("active");
    write_slot(writer, record.active);
    writer.key("last_good");
    write_slot(writer, record.last_good);
    writer.key("on_trial");
    writer.boolean(record.on_trial);
    writer.key("update_available");
    writer.boolean(record.update_available);
    writer.key("failed_starts");
    writer.unsigned_integer(record.failed_starts);
    writer.key("version_a");
    if (record.version_a.length == 0u) {
        writer.null();
    } else {
        writer.string(std::span<const char>(record.version_a.bytes, record.version_a.length));
    }
    writer.key("version_b");
    if (record.version_b.length == 0u) {
        writer.null();
    } else {
        writer.string(std::span<const char>(record.version_b.bytes, record.version_b.length));
    }
    writer.end_object();
}

void Api::handle_targets(ApiResponse& response) noexcept {
    if (!load_user_config(response) || !parse_user_config(response)) {
        return;
    }
    // The services, copied out before the next request reuses the
    // backend's storage.
    stxcontrol::ServiceRecord services[stxcontrol::services_listed_max]{};
    std::size_t service_count = 0;
    bool services_known = false;
    {
        const stxcontrol::Request request =
            stxcontrol::request_without_name(stxcontrol::Command::list_services, 0);
        const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
        if (answer.has_value() && (*answer).command == stxcontrol::Command::list_services &&
            (*answer).status == stxcontrol::Status::ok) {
            const std::optional<std::uint8_t> count = stxcontrol::decode_service_list_count((*answer).payload);
            if (count.has_value()) {
                services_known = true;
                for (std::size_t index = 0; index < *count; index += 1) {
                    const std::optional<stxcontrol::ServiceRecord> record = stxcontrol::decode_service_record(
                        stxcontrol::service_list_record((*answer).payload, index));
                    if (!record.has_value()) {
                        services_known = false;
                        break;
                    }
                    services[service_count] = *record;
                    service_count += 1;
                }
            }
        }
    }
    if (!services_known) {
        service_count = 0;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("sequence");
    writer.unsigned_integer(user_sequence_);
    writer.key("pending");
    writer.boolean(user_written_since_start_);
    writer.key("targets");
    writer.begin_array();
    // Whether the host side answers program_state; asked until it says it
    // does not.
    bool programs_supported = true;
    for (std::size_t index = 0; index < check_config_.service_count(); index += 1) {
        const stxconfig::ServiceDefinition& target = check_config_.service(index);
        const char* const name = check_config_.text(target.name);
        const std::span<const char> name_bytes = text_of(name);
        char section[sizeof(service_prefix) + stxcontrol::service_name_bytes_max];
        const std::optional<stxcontrol::ServiceName> wire = stxcontrol::service_name_of(name_bytes);
        if (!wire.has_value()) {
            continue;
        }
        (void)target_section(*wire, section);
        writer.begin_object();
        writer.key("name");
        writer.string(name);
        writer.key("settings");
        write_section_settings(writer, section);
        writer.key("enabled");
        writer.boolean(editor_.starts(name_bytes));
        writer.key("service");
        bool found = false;
        for (std::size_t service = 0; service < service_count; service += 1) {
            if (stxcontrol::service_names_equal(services[service].name, *wire)) {
                write_service_record(writer, services[service]);
                found = true;
                break;
            }
        }
        if (!found) {
            writer.null();
        }
        writer.key("program");
        bool program_written = false;
        if (programs_supported) {
            const stxcontrol::Request request =
                stxcontrol::request_for_service(stxcontrol::Command::program_state, 0, *wire);
            const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
            if (answer.has_value() && (*answer).status == stxcontrol::Status::unsupported) {
                programs_supported = false;
            } else if (answer.has_value() && (*answer).command == stxcontrol::Command::program_state &&
                       (*answer).status == stxcontrol::Status::ok) {
                const std::optional<stxcontrol::ProgramRecord> record =
                    stxcontrol::decode_program_record((*answer).payload);
                if (record.has_value() && stxcontrol::service_names_equal((*record).name, *wire)) {
                    write_program_record(writer, *record);
                    program_written = true;
                }
            }
        }
        if (!program_written) {
            writer.null();
        }
        writer.end_object();
    }
    writer.end_array();
    writer.key("programs_supported");
    writer.boolean(programs_supported);
    writer.key("services_known");
    writer.boolean(services_known);
    // The factory's services: running beside the targets, not editable.
    writer.key("system_services");
    writer.begin_array();
    for (std::size_t service = 0; service < service_count; service += 1) {
        char name[stxcontrol::service_name_bytes_max + 1];
        stxcontrol::service_name_c_string(services[service].name, name);
        if (check_config_.find_service(name).has_value()) {
            continue;
        }
        write_service_record(writer, services[service]);
    }
    writer.end_array();
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

void Api::handle_target_write(const stxcontrol::ServiceName& name, ApiResponse& response) noexcept {
    const std::optional<std::size_t> settings = document_.member(0, "settings");
    const std::optional<std::size_t> enabled = document_.member(0, "enabled");
    if (!settings.has_value() || document_.token(*settings).kind != JsonKind::object || !enabled.has_value() ||
        (document_.token(*enabled).kind != JsonKind::boolean_true &&
         document_.token(*enabled).kind != JsonKind::boolean_false)) {
        respond_error(response, 400, "settings_or_enabled_missing");
        return;
    }
    const std::size_t count = document_.token(*settings).count;
    if (count > edit_section_settings_max) {
        respond_error(response, 400, edit_result_name(EditResult::too_many_settings));
        return;
    }
    // Each member's key and value, decoded into fixed buffers.
    char names[edit_section_settings_max][stxconfig::name_length_max + 1];
    char values[edit_section_settings_max][edit_line_bytes_max + 1];
    EditSetting pairs[edit_section_settings_max];
    std::size_t token = *settings + 1u;
    for (std::size_t index = 0; index < count; index += 1) {
        const std::size_t key = token;
        const std::size_t value = token + 1u;
        if (value >= document_.token_count() || document_.token(value).kind != JsonKind::string) {
            respond_error(response, 400, "setting_not_string");
            return;
        }
        const std::optional<std::size_t> key_size =
            document_.decode_string(key, std::span<char>(names[index], stxconfig::name_length_max));
        const std::optional<std::size_t> value_size =
            document_.decode_string(value, std::span<char>(values[index], edit_line_bytes_max));
        if (!key_size.has_value()) {
            respond_error(response, 400, edit_result_name(EditResult::name_not_allowed));
            return;
        }
        if (!value_size.has_value()) {
            respond_error(response, 400, edit_result_name(EditResult::value_not_allowed));
            return;
        }
        pairs[index].name = std::span<const char>(names[index], *key_size);
        pairs[index].value = std::span<const char>(values[index], *value_size);
        token = document_.token(value).end;
    }
    if (!load_user_config(response)) {
        return;
    }
    char section_text[sizeof(service_prefix) + stxcontrol::service_name_bytes_max];
    const std::span<const char> section = target_section(name, section_text);
    if (!edit_done(editor_.replace_section(section, std::span<const EditSetting>(pairs, count)), response) ||
        !edit_done(editor_.set_start(std::span<const char>(name.bytes, name.length),
                                     document_.token(*enabled).kind == JsonKind::boolean_true),
                   response)) {
        return;
    }
    write_user_config(editor_.text(), response);
}

void Api::handle_target_delete(const stxcontrol::ServiceName& name, ApiResponse& response) noexcept {
    if (!load_user_config(response)) {
        return;
    }
    char section_text[sizeof(service_prefix) + stxcontrol::service_name_bytes_max];
    const std::span<const char> section = target_section(name, section_text);
    if (!edit_done(editor_.remove_section(section), response) ||
        !edit_done(editor_.set_start(std::span<const char>(name.bytes, name.length), false), response)) {
        return;
    }
    write_user_config(editor_.text(), response);
}

void Api::handle_program(stxcontrol::Command command, const stxcontrol::ServiceName& name,
                         ApiResponse& response) noexcept {
    stxcontrol::Request request = stxcontrol::request_for_service(command, 0, name);
    if (command == stxcontrol::Command::program_install) {
        const std::optional<std::size_t> source = string_member(document_, 0, "source");
        stxcontrol::InstallSource install{};
        char url[stxcontrol::install_url_bytes_max];
        install.kind = stxcontrol::InstallKind::staged;
        install.url = std::span<const char>();
        if (source.has_value() && document_.string_equals(*source, text_of("url"))) {
            const std::optional<std::size_t> member = string_member(document_, 0, "url");
            const std::optional<std::size_t> size =
                member.has_value() ? document_.decode_string(*member, std::span<char>(url, sizeof(url)))
                                   : std::nullopt;
            if (!size.has_value()) {
                respond_error(response, 400, "url_missing");
                return;
            }
            install.kind = stxcontrol::InstallKind::url;
            install.url = std::span<const char>(url, *size);
        } else if (!source.has_value() || !document_.string_equals(*source, text_of("upload"))) {
            respond_error(response, 400, "source_missing");
            return;
        }
        const std::optional<std::size_t> size = stxcontrol::encode_install_source(
            install, std::span<std::uint8_t>(request_payload_, sizeof(request_payload_)));
        if (!size.has_value()) {
            respond_error(response, 400, "url_not_allowed");
            return;
        }
        request = stxcontrol::request_with_payload(request, std::span<const std::uint8_t>(request_payload_, *size));
    }
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    if (stxcontrol::command_operates(command)) {
        response.event.kind = ApiEventKind::operated;
        response.event.detail = stxcontrol::command_name(command);
        stxcontrol::service_name_c_string(name, response.event.service);
        response.event.status = answer.has_value() ? stxcontrol::status_name((*answer).status) : "unavailable";
    }
    if (!answer.has_value() || (*answer).command != command) {
        respond_error(response, 503, "supervisor_unavailable");
        return;
    }
    const stxcontrol::Status status = (*answer).status;
    std::optional<stxcontrol::ProgramRecord> record;
    if (!(*answer).payload.empty()) {
        record = stxcontrol::decode_program_record((*answer).payload);
        if (!record.has_value() || !stxcontrol::service_names_equal((*record).name, name)) {
            respond_error(response, 503, "supervisor_unavailable");
            return;
        }
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    if (status == stxcontrol::Status::ok) {
        writer.key("result");
        writer.string(command == stxcontrol::Command::program_state ? "ok" : "started");
    } else {
        writer.key("error");
        writer.string(stxcontrol::status_name(status));
    }
    writer.key("program");
    if (record.has_value()) {
        write_program_record(writer, *record);
    } else {
        writer.null();
    }
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, http_status_of(status));
}

void Api::handle_target_log(const stxcontrol::ServiceName& name, ApiResponse& response) noexcept {
    // Read access in the supervisor's terms; the page needs only a session
    // (a GET, so no CSRF token: it changes nothing).
    const stxcontrol::Request request =
        stxcontrol::request_for_service(stxcontrol::Command::target_log_tail, 0, name);
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    if (!supervisor_ok(answer, stxcontrol::Command::target_log_tail, response)) {
        return;
    }
    const std::optional<stxcontrol::BootLogTailView> tail = stxcontrol::decode_boot_log_tail((*answer).payload);
    if (!tail.has_value()) {
        respond_error(response, 503, "supervisor_unavailable");
        return;
    }
    char name_text[stxcontrol::service_name_bytes_max + 1];
    stxcontrol::service_name_c_string(name, name_text);
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("name");
    writer.string(name_text);
    writer.key("present");
    writer.boolean((*tail).present);
    writer.key("front_cut");
    writer.boolean((*tail).front_cut);
    writer.key("text");
    // The writer replaces bytes that are not UTF-8: a target's output is
    // whatever it wrote.
    writer.string(std::span<const char>(reinterpret_cast<const char*>((*tail).text.data()), (*tail).text.size()));
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

void Api::handle_system_update(ApiResponse& response) noexcept {
    const stxcontrol::Request request = stxcontrol::request_without_name(stxcontrol::Command::system_update, 0);
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    response.event.kind = ApiEventKind::operated;
    response.event.detail = "system-update";
    response.event.status = answer.has_value() ? stxcontrol::status_name((*answer).status) : "unavailable";
    if (!supervisor_ok(answer, stxcontrol::Command::system_update, response)) {
        return;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("result");
    writer.string("started");
    writer.end_object();
    body_size_ = writer.ok() ? writer.size() : 0u;
    respond_json(response, 200);
}

void Api::handle_operation(ApiResponse& response) noexcept {
    const stxcontrol::Request request = stxcontrol::request_without_name(stxcontrol::Command::operation_state, 0);
    const std::optional<stxcontrol::ResponseView> answer = backend_.ask_supervisor(request);
    if (!supervisor_ok(answer, stxcontrol::Command::operation_state, response)) {
        return;
    }
    const std::optional<stxcontrol::OperationRecord> record = stxcontrol::decode_operation_record((*answer).payload);
    if (!record.has_value()) {
        respond_error(response, 503, "supervisor_unavailable");
        return;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("kind");
    writer.string(stxcontrol::operation_kind_name((*record).kind));
    writer.key("state");
    writer.string(stxcontrol::operation_state_name((*record).state));
    writer.key("result");
    writer.string(stxcontrol::status_name((*record).result));
    writer.key("elapsed_ms");
    writer.unsigned_integer((*record).elapsed_ms);
    writer.key("target");
    if ((*record).name.length == 0u) {
        writer.null();
    } else {
        writer.string(std::span<const char>((*record).name.bytes, (*record).name.length));
    }
    writer.end_object();
    body_size_ = writer.ok() ? writer.size() : 0u;
    respond_json(response, writer.ok() ? 200 : 500);
}

// ---------------------------------------------------------------------------
// Network and access.

void Api::handle_network(ApiResponse& response) noexcept {
    const std::optional<std::size_t> count =
        backend_.list_addresses(std::span<NetworkAddress>(addresses_, network_addresses_max));
    if (!count.has_value() || *count > network_addresses_max) {
        respond_error(response, 503, "network_unavailable");
        return;
    }
    // The settings, when the supervisor answers; the addresses either way.
    bool settings_known = false;
    if (fetch_user_config()) {
        settings_known = stxconfig::parse_boot_config(editor_.text(), check_config_).is_success();
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("addresses");
    writer.begin_array();
    for (std::size_t index = 0; index < *count; index += 1) {
        const NetworkAddress& address = addresses_[index];
        char text[address_text_bytes_max];
        (void)format_address(address.family, std::span<const std::uint8_t, 16>(address.bytes), text);
        // The backend NUL-terminates the name; a longer one is cut.
        char name[interface_name_bytes_max + 1];
        for (std::size_t position = 0; position <= interface_name_bytes_max; position += 1) {
            name[position] = position < interface_name_bytes_max ? address.interface_name[position] : '\0';
        }
        writer.begin_object();
        writer.key("interface");
        writer.string(name);
        writer.key("family");
        writer.string(address.family == AddressFamily::ipv4 ? "inet" : "inet6");
        writer.key("address");
        writer.string(text);
        writer.key("prefix");
        writer.unsigned_integer(address.prefix_length);
        writer.end_object();
    }
    writer.end_array();
    writer.key("hostname");
    const std::optional<stxconfig::TextRef> hostname =
        settings_known ? check_config_.find_setting(stxconfig::user_section_system, "hostname") : std::nullopt;
    if (hostname.has_value()) {
        writer.string(check_config_.text(*hostname));
    } else {
        writer.null();
    }
    writer.key("settings");
    if (settings_known) {
        write_section_settings(writer, stxconfig::user_section_network);
    } else {
        writer.null();
    }
    writer.key("sequence");
    writer.unsigned_integer(settings_known ? user_sequence_ : 0u);
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

void Api::handle_network_write(ApiResponse& response) noexcept {
    // Every member is optional but mode; each is checked for its form.
    char hostname[254];
    char mode[8];
    char address[20];
    char gateway[16];
    char dns[16];
    struct Field {
        const char* key;
        char* buffer;
        std::size_t capacity;
        std::optional<std::span<const char>> value;
    };
    Field fields[] = {
        {"hostname", hostname, sizeof(hostname), std::nullopt}, {"mode", mode, sizeof(mode), std::nullopt},
        {"address", address, sizeof(address), std::nullopt},    {"gateway", gateway, sizeof(gateway), std::nullopt},
        {"dns", dns, sizeof(dns), std::nullopt},
    };
    for (Field& field : fields) {
        const std::optional<std::size_t> member = document_.member(0, field.key);
        if (!member.has_value() || document_.token(*member).kind == JsonKind::null) {
            continue;
        }
        if (document_.token(*member).kind != JsonKind::string) {
            respond_error(response, 400, "field_not_string");
            return;
        }
        const std::optional<std::size_t> size =
            document_.decode_string(*member, std::span<char>(field.buffer, field.capacity));
        if (!size.has_value()) {
            respond_error(response, 400, "field_too_long");
            return;
        }
        // An empty string is the same as absent.
        if (*size > 0u) {
            field.value = std::span<const char>(field.buffer, *size);
        }
    }
    const std::optional<std::span<const char>>& hostname_value = fields[0].value;
    const std::optional<std::span<const char>>& mode_value = fields[1].value;
    const std::optional<std::span<const char>>& address_value = fields[2].value;
    const std::optional<std::span<const char>>& gateway_value = fields[3].value;
    const std::optional<std::span<const char>>& dns_value = fields[4].value;
    if (hostname_value.has_value() && !hostname_valid(*hostname_value)) {
        respond_error(response, 400, "hostname_not_allowed");
        return;
    }
    const bool dhcp = mode_value.has_value() && equals_text(*mode_value, "dhcp");
    const bool fixed = mode_value.has_value() && equals_text(*mode_value, "static");
    if (!dhcp && !fixed) {
        respond_error(response, 400, "mode_not_allowed");
        return;
    }
    if (fixed && (!address_value.has_value() || !ipv4_prefix_text_valid(*address_value))) {
        respond_error(response, 400, "address_not_allowed");
        return;
    }
    if (fixed && gateway_value.has_value() && !ipv4_text_valid(*gateway_value)) {
        respond_error(response, 400, "gateway_not_allowed");
        return;
    }
    if (dns_value.has_value() && !ipv4_text_valid(*dns_value)) {
        respond_error(response, 400, "dns_not_allowed");
        return;
    }
    EditSetting pairs[4];
    std::size_t count = 0;
    pairs[count].name = text_of("mode");
    pairs[count].value = *mode_value;
    count += 1;
    if (fixed) {
        pairs[count].name = text_of("address");
        pairs[count].value = *address_value;
        count += 1;
        if (gateway_value.has_value()) {
            pairs[count].name = text_of("gateway");
            pairs[count].value = *gateway_value;
            count += 1;
        }
    }
    if (dns_value.has_value()) {
        pairs[count].name = text_of("dns");
        pairs[count].value = *dns_value;
        count += 1;
    }
    if (!load_user_config(response)) {
        return;
    }
    const std::span<const char> system = text_of(stxconfig::user_section_system);
    const std::span<const char> hostname_name = text_of("hostname");
    const EditResult hostname_edit = hostname_value.has_value()
                                         ? editor_.set_setting(system, hostname_name, *hostname_value)
                                         : editor_.remove_setting(system, hostname_name);
    if (!edit_done(hostname_edit, response) ||
        !edit_done(editor_.replace_section(text_of(stxconfig::user_section_network),
                                           std::span<const EditSetting>(pairs, count)),
                   response)) {
        return;
    }
    write_user_config(editor_.text(), response);
}

void Api::handle_access(ApiResponse& response) noexcept {
    if (!load_user_config(response) || !parse_user_config(response)) {
        return;
    }
    JsonWriter writer(std::span<char>(body_, sizeof(body_)));
    writer.begin_object();
    writer.key("keys");
    writer.begin_array();
    for (const char* const key_name : ssh_key_names) {
        const std::optional<stxconfig::TextRef> value =
            check_config_.find_setting(stxconfig::user_section_access, key_name);
        if (value.has_value()) {
            writer.string(check_config_.text(*value));
        }
    }
    writer.end_array();
    writer.key("password_set");
    writer.boolean(backend_.password_is_set());
    writer.key("sequence");
    writer.unsigned_integer(user_sequence_);
    writer.end_object();
    if (!writer.ok()) {
        respond_error(response, 500, "internal");
        return;
    }
    body_size_ = writer.size();
    respond_json(response, 200);
}

void Api::handle_access_write(ApiResponse& response) noexcept {
    const std::optional<std::size_t> keys = document_.member(0, "keys");
    if (!keys.has_value() || document_.token(*keys).kind != JsonKind::array) {
        respond_error(response, 400, "keys_missing");
        return;
    }
    const std::size_t count = document_.token(*keys).count;
    if (count > ssh_keys_max) {
        respond_error(response, 400, "too_many_keys");
        return;
    }
    char values[ssh_keys_max][edit_line_bytes_max + 1];
    EditSetting pairs[ssh_keys_max];
    std::size_t token = *keys + 1u;
    for (std::size_t index = 0; index < count; index += 1) {
        if (document_.token(token).kind != JsonKind::string) {
            respond_error(response, 400, "key_not_string");
            return;
        }
        const std::optional<std::size_t> size =
            document_.decode_string(token, std::span<char>(values[index], edit_line_bytes_max));
        const std::span<const char> name = text_of(ssh_key_names[index]);
        if (!size.has_value() || !config_value_valid(name, std::span<const char>(values[index], *size))) {
            respond_error(response, 400, "key_too_long");
            return;
        }
        if (!ssh_key_valid(std::span<const char>(values[index], *size))) {
            respond_error(response, 400, "key_not_allowed");
            return;
        }
        pairs[index].name = name;
        pairs[index].value = std::span<const char>(values[index], *size);
        token = document_.token(token).end;
    }
    if (!load_user_config(response)) {
        return;
    }
    if (!edit_done(editor_.replace_section(text_of(stxconfig::user_section_access),
                                           std::span<const EditSetting>(pairs, count)),
                   response)) {
        return;
    }
    write_user_config(editor_.text(), response);
}

}  // namespace stxweb
