// target_ids.cpp: the supervisor issues target ids (target_ids.h).

#include "target_ids.h"

#include <cstring>

namespace stxconfig {

namespace {

[[nodiscard]] bool same_bytes(std::span<const char> left, const char* right, std::size_t right_length) noexcept {
    return left.size() == right_length && (right_length == 0u || std::memcmp(left.data(), right, right_length) == 0);
}

[[nodiscard]] std::span<const char> text_of(const BootConfig& config, TextRef ref) noexcept {
    return std::span<const char>(config.text(ref), ref.length);
}

[[nodiscard]] const char* state_word(LedgerState state) noexcept {
    switch (state) {
    case LedgerState::active: return "active";
    case LedgerState::deleted: return "deleted";
    case LedgerState::removed: return "removed";
    default: break;
    }
    // Fallback: never formed.
    return "removed";
}

// Appends bytes at out[length]; false when they do not fit.
[[nodiscard]] bool put(std::span<char> out, std::size_t& length, const char* bytes, std::size_t count) noexcept {
    if (count > out.size() - length) {
        return false;
    }
    for (std::size_t index = 0; index < count; index += 1) {
        out[length + index] = bytes[index];
    }
    length += count;
    return true;
}

[[nodiscard]] bool put_text(std::span<char> out, std::size_t& length, const char* text) noexcept {
    return put(out, length, text, std::strlen(text));
}

[[nodiscard]] bool put_decimal(std::span<char> out, std::size_t& length, std::uint32_t value) noexcept {
    char reversed[10] = {};
    std::size_t count = 0;
    std::uint32_t rest = value;
    // At most 10 digits in 32 bits.
    for (int digit = 0; digit < 10 && (rest != 0u || count == 0u); digit += 1) {
        reversed[count] = static_cast<char>('0' + rest % 10u);
        rest /= 10u;
        count += 1;
    }
    char digits[10] = {};
    for (std::size_t index = 0; index < count; index += 1) {
        digits[index] = reversed[count - 1u - index];
    }
    return put(out, length, digits, count);
}

// One word of line from begin: its end (a space, or the end).
[[nodiscard]] std::size_t word_end(std::span<const char> line, std::size_t begin) noexcept {
    std::size_t end = begin;
    while (end < line.size() && line[end] != ' ') {
        end += 1;
    }
    return end;
}

[[nodiscard]] bool is_name_byte(char byte, bool first) noexcept {
    const bool lower = byte >= 'a' && byte <= 'z';
    return first ? lower : (lower || (byte >= '0' && byte <= '9') || byte == '-');
}

[[nodiscard]] bool name_valid(std::span<const char> name) noexcept {
    if (name.empty() || name.size() > name_length_max) {
        return false;
    }
    for (std::size_t index = 0; index < name.size(); index += 1) {
        if (!is_name_byte(name[index], index == 0u)) {
            return false;
        }
    }
    return true;
}

// A folder as the ledger keeps it: below target_folder_parent, printable,
// no space, no ".." component (the parser checked it when it was issued;
// the ledger is read back, so it is checked again).
[[nodiscard]] bool folder_valid(std::span<const char> folder) noexcept {
    const std::size_t parent = std::strlen(target_folder_parent);
    if (folder.size() <= parent + 1u || folder.size() > line_length_max ||
        std::memcmp(folder.data(), target_folder_parent, parent) != 0 || folder[parent] != '/') {
        return false;
    }
    for (std::size_t index = 0; index < folder.size(); index += 1) {
        if (folder[index] <= ' ' || folder[index] > '~') {
            return false;
        }
        if (folder[index] == '.' && index + 1u < folder.size() && folder[index + 1u] == '.' &&
            folder[index - 1u] == '/' && (index + 2u == folder.size() || folder[index + 2u] == '/')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::uint32_t> id_of(std::span<const char> word) noexcept {
    if (word.empty() || word.size() > 3u || word[0] == '0') {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < word.size(); index += 1) {
        if (word[index] < '0' || word[index] > '9') {
            return std::nullopt;
        }
        value = value * 10u + static_cast<std::uint32_t>(word[index] - '0');
    }
    if (value == 0u || value > target_id_max) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> id = value;
    return id;
}

// The line of setting name in the section whose header is on section_line;
// the header's line when there is none.
[[nodiscard]] std::uint32_t setting_line(const BootConfig& config, std::uint32_t section_line,
                                         const char* name) noexcept {
    for (std::size_t section = 0; section < config.section_count(); section += 1) {
        if (config.section(section).line != section_line) {
            continue;
        }
        for (std::size_t index = 0; index < config.setting_count(); index += 1) {
            const Setting& setting = config.setting(index);
            if (setting.section_index == section && std::strcmp(config.text(setting.name), name) == 0) {
                return setting.line;
            }
        }
    }
    return section_line;
}

// The target named name in config, if there is one.
[[nodiscard]] const ServiceDefinition* target_named(const BootConfig& config, std::span<const char> name) noexcept {
    for (std::size_t index = 0; index < config.service_count(); index += 1) {
        const ServiceDefinition& service = config.service(index);
        if (service.target.has_value() && same_bytes(name, config.text(service.name), service.name.length)) {
            return &service;
        }
    }
    return nullptr;
}

[[nodiscard]] bool factory_uses_id(const BootConfig& factory, std::uint32_t id) noexcept {
    for (std::size_t index = 0; index < factory.service_count(); index += 1) {
        const ServiceDefinition& service = factory.service(index);
        if (service.target.has_value() && (*service.target).id == id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::span<const char> entry_name(const LedgerEntry& entry) noexcept {
    return std::span<const char>(entry.name, entry.name_length);
}

}  // namespace

TargetIdLedger::TargetIdLedger() noexcept : entries_{}, count_(0) {}

void TargetIdLedger::clear() noexcept {
    count_ = 0;
}

std::size_t TargetIdLedger::count() const noexcept {
    return count_;
}

const LedgerEntry& TargetIdLedger::entry(std::size_t index) const noexcept {
    return entries_[index < count_ ? index : 0u];
}

std::optional<std::size_t> TargetIdLedger::find_id(std::uint32_t id) const noexcept {
    for (std::size_t index = 0; index < count_; index += 1) {
        if (entries_[index].id == id) {
            const std::optional<std::size_t> found = index;
            return found;
        }
    }
    return std::nullopt;
}

bool TargetIdLedger::add(std::uint32_t id, LedgerState state, std::span<const char> name,
                         std::span<const char> folder) noexcept {
    if (count_ >= target_id_entries_max || id == 0u || id > target_id_max || find_id(id).has_value() ||
        name.size() > name_length_max || folder.size() > line_length_max) {
        return false;
    }
    // Kept in id order: shift the larger ones up by one.
    std::size_t place = count_;
    while (place > 0u && entries_[place - 1u].id > id) {
        entries_[place] = entries_[place - 1u];
        place -= 1u;
    }
    LedgerEntry& made = entries_[place];
    made = LedgerEntry{};
    made.id = id;
    made.state = state;
    made.name_length = static_cast<std::uint8_t>(name.size());
    for (std::size_t index = 0; index < name.size(); index += 1) {
        made.name[index] = name[index];
    }
    made.folder_length = static_cast<std::uint16_t>(folder.size());
    for (std::size_t index = 0; index < folder.size(); index += 1) {
        made.folder[index] = folder[index];
    }
    count_ += 1;
    return true;
}

void TargetIdLedger::set_state(std::size_t index, LedgerState state) noexcept {
    if (index >= count_) {
        return;
    }
    entries_[index].state = state;
    if (state == LedgerState::removed) {
        entries_[index].name_length = 0;
        entries_[index].folder_length = 0;
    }
}

bool TargetIdLedger::parse(std::span<const char> text) noexcept {
    clear();
    const std::size_t header = std::strlen(target_id_ledger_header);
    if (text.size() < header || std::memcmp(text.data(), target_id_ledger_header, header) != 0) {
        return false;
    }
    std::size_t begin = header;
    std::uint32_t previous = 0;
    // Each pass consumes one line; lines are at most target_id_entries_max.
    while (begin < text.size()) {
        std::size_t end = begin;
        while (end < text.size() && text[end] != '\n') {
            end += 1;
        }
        if (end == text.size() || count_ >= target_id_entries_max) {
            clear();
            return false;  // an unterminated last line, or too many
        }
        const std::span<const char> line = text.subspan(begin, end - begin);
        begin = end + 1u;
        const std::size_t id_end = word_end(line, 0);
        const std::optional<std::uint32_t> id = id_of(line.subspan(0, id_end));
        if (!id.has_value() || *id <= previous || id_end + 1u >= line.size()) {
            clear();
            return false;
        }
        previous = *id;
        const std::size_t state_begin = id_end + 1u;
        const std::size_t state_end = word_end(line, state_begin);
        const std::span<const char> state = line.subspan(state_begin, state_end - state_begin);
        if (same_bytes(state, "removed", 7u) && state_end == line.size()) {
            if (!add(*id, LedgerState::removed, std::span<const char>(), std::span<const char>())) {
                clear();
                return false;
            }
            continue;
        }
        const bool active = same_bytes(state, "active", 6u);
        if ((!active && !same_bytes(state, "deleted", 7u)) || state_end + 1u >= line.size()) {
            clear();
            return false;
        }
        const std::size_t name_begin = state_end + 1u;
        const std::size_t name_end = word_end(line, name_begin);
        const std::span<const char> name = line.subspan(name_begin, name_end - name_begin);
        if (!name_valid(name) || name_end + 1u >= line.size()) {
            clear();
            return false;
        }
        const std::span<const char> folder = line.subspan(name_end + 1u);
        if (!folder_valid(folder) ||
            !add(*id, active ? LedgerState::active : LedgerState::deleted, name, folder)) {
            clear();
            return false;
        }
    }
    return true;
}

std::optional<std::size_t> TargetIdLedger::format(std::span<char> out) const noexcept {
    std::size_t length = 0;
    bool fits = put_text(out, length, target_id_ledger_header);
    for (std::size_t index = 0; fits && index < count_; index += 1) {
        const LedgerEntry& item = entries_[index];
        fits = put_decimal(out, length, item.id) && put_text(out, length, " ") &&
               put_text(out, length, state_word(item.state));
        if (fits && item.state != LedgerState::removed) {
            fits = put_text(out, length, " ") && put(out, length, item.name, item.name_length) &&
                   put_text(out, length, " ") && put(out, length, item.folder, item.folder_length);
        }
        fits = fits && put_text(out, length, "\n");
    }
    if (!fits) {
        return std::nullopt;
    }
    const std::optional<std::size_t> made = length;
    return made;
}

IdIssueOutcome issue_target_ids(const BootConfig& user, const BootConfig& factory, std::span<const char> text,
                                TargetIdLedger& ledger, std::span<char> out) noexcept {
    IdIssueOutcome outcome{IdIssueKind::refused, 0, 0, 0};
    // 1. A given id must be the one the ledger gives that name, active.
    for (std::size_t index = 0; index < user.service_count(); index += 1) {
        const ServiceDefinition& service = user.service(index);
        if (!service.target.has_value() || (*service.target).id == 0u) {
            continue;
        }
        const std::optional<std::size_t> found = ledger.find_id((*service.target).id);
        const bool issued = found.has_value() && ledger.entry(*found).state == LedgerState::active &&
                            same_bytes(entry_name(ledger.entry(*found)), user.text(service.name), service.name.length);
        if (!issued) {
            outcome.line = setting_line(user, service.line, "id");
            outcome.column = 1;
            return outcome;
        }
    }
    // 2. Active entries the configuration no longer holds are deleted (a
    //    factory target's entry stays: the user configuration never holds
    //    one).
    for (std::size_t index = 0; index < ledger.count(); index += 1) {
        const LedgerEntry& item = ledger.entry(index);
        if (item.state != LedgerState::active) {
            continue;
        }
        const ServiceDefinition* const factory_target = target_named(factory, entry_name(item));
        if (factory_target != nullptr && (*(*factory_target).target).id == item.id) {
            continue;
        }
        if (target_named(user, entry_name(item)) == nullptr) {
            ledger.set_state(index, LedgerState::deleted);
        }
    }
    // 3. New ids, in file order: the lowest never used.
    std::uint32_t issued_ids[services_max] = {};
    std::uint32_t issued_lines[services_max] = {};
    std::size_t issued_count = 0;
    for (std::size_t index = 0; index < user.service_count() && issued_count < services_max; index += 1) {
        const ServiceDefinition& service = user.service(index);
        if (!service.target.has_value() || (*service.target).id != 0u) {
            continue;
        }
        // A target the ledger holds as active keeps its id (a page that
        // left it out); any other gets the lowest id never used.
        std::optional<std::uint32_t> kept;
        for (std::size_t entry = 0; entry < ledger.count() && !kept.has_value(); entry += 1) {
            const LedgerEntry& item = ledger.entry(entry);
            if (item.state == LedgerState::active &&
                same_bytes(entry_name(item), user.text(service.name), service.name.length)) {
                kept = item.id;
            }
        }
        std::uint32_t id = 1;
        if (kept.has_value()) {
            id = *kept;
        } else {
            while (id <= target_id_max && (ledger.find_id(id).has_value() || factory_uses_id(factory, id))) {
                id += 1;
            }
            if (id > target_id_max || !ledger.add(id, LedgerState::active, text_of(user, service.name),
                                                  text_of(user, (*service.target).folder))) {
                outcome.kind = IdIssueKind::ledger_full;
                return outcome;
            }
        }
        issued_ids[issued_count] = id;
        issued_lines[issued_count] = service.line;
        issued_count += 1;
    }
    // 4. The text with "id = <n>" after each such header line.
    std::size_t length = 0;
    std::uint32_t line_number = 1;
    std::size_t begin = 0;
    bool fits = true;
    while (fits && begin < text.size()) {
        std::size_t end = begin;
        while (end < text.size() && text[end] != '\n') {
            end += 1;
        }
        fits = put(out, length, &text[begin], end - begin) && put_text(out, length, "\n");
        for (std::size_t issued = 0; fits && issued < issued_count; issued += 1) {
            if (issued_lines[issued] == line_number) {
                fits = put_text(out, length, "id = ") && put_decimal(out, length, issued_ids[issued]) &&
                       put_text(out, length, "\n");
            }
        }
        begin = end + 1u;
        line_number += 1;
    }
    // A text without a final LF gets one (the copy above adds it).
    if (!fits || length > config_size_max) {
        outcome.kind = IdIssueKind::too_large;
        return outcome;
    }
    outcome.kind = IdIssueKind::issued;
    outcome.length = length;
    return outcome;
}

void plan_boot_ids(const BootConfig& running, TargetIdLedger& ledger, BootIdPlan& plan) noexcept {
    plan.removal_count = 0;
    plan.conflicts = 0;
    plan.changed = false;
    for (std::size_t index = 0; index < running.service_count(); index += 1) {
        const ServiceDefinition& service = running.service(index);
        if (!service.target.has_value() || (*service.target).id == 0u) {
            continue;
        }
        const std::span<const char> name = text_of(running, service.name);
        const std::optional<std::size_t> found = ledger.find_id((*service.target).id);
        if (!found.has_value()) {
            plan.changed =
                ledger.add((*service.target).id, LedgerState::active, name, text_of(running, (*service.target).folder)) ||
                plan.changed;
            continue;
        }
        const LedgerEntry& item = ledger.entry(*found);
        if (item.state == LedgerState::removed || !same_bytes(entry_name(item), name.data(), name.size())) {
            plan.conflicts += 1;
            continue;
        }
        if (item.state == LedgerState::deleted) {
            // An older turn holds it again, with its own id.
            ledger.set_state(*found, LedgerState::active);
            plan.changed = true;
        }
    }
    for (std::size_t index = 0; index < ledger.count(); index += 1) {
        if (ledger.entry(index).state == LedgerState::deleted && plan.removal_count < target_id_entries_max) {
            plan.removals[plan.removal_count] = index;
            plan.removal_count += 1;
        }
    }
}

bool every_target_has_id(const BootConfig& config) noexcept {
    for (std::size_t index = 0; index < config.service_count(); index += 1) {
        const ServiceDefinition& service = config.service(index);
        if (service.target.has_value() && (*service.target).id == 0u) {
            return false;
        }
    }
    return true;
}

}  // namespace stxconfig
