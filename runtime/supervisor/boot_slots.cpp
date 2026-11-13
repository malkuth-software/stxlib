// boot_slots.cpp: see boot_slots.h.

#include "boot_slots.h"

namespace stxboot {

char slot_letter(Slot slot) noexcept {
    switch (slot) {
    case Slot::a: return 'A';
    case Slot::b: return 'B';
    default: return '?';
    }
}

std::optional<Slot> slot_from_letter(char letter) noexcept {
    if (letter == 'A') {
        return Slot::a;
    }
    if (letter == 'B') {
        return Slot::b;
    }
    return std::nullopt;
}

namespace {

// Whether text[begin, end) equals word.
[[nodiscard]] bool equals(std::span<const char> text, std::size_t begin, std::size_t end, const char* word) noexcept {
    std::size_t index = 0;
    for (; begin + index < end; index += 1) {
        if (word[index] == '\0' || word[index] != text[begin + index]) {
            return false;
        }
    }
    return word[index] == '\0';
}

}  // namespace

std::optional<SlotFile> parse_slot_file(std::span<const char> text) noexcept {
    if (text.size() > slot_file_size_max) {
        return std::nullopt;
    }
    std::optional<Slot> good;
    std::optional<Slot> try_slot;
    std::size_t begin = 0;
    // Each pass consumes one line (at least its newline or the rest).
    while (begin < text.size()) {
        std::size_t end = begin;
        while (end < text.size() && text[end] != '\n') {
            end += 1;
        }
        if (end > begin) {
            // "good=X" or "try=X": the line is exactly that.
            const std::size_t length = end - begin;
            std::optional<Slot>* target = nullptr;
            if (length == 6 && equals(text, begin, begin + 5, "good=")) {
                target = &good;
            } else if (length == 5 && equals(text, begin, begin + 4, "try=")) {
                target = &try_slot;
            } else {
                return std::nullopt;
            }
            if ((*target).has_value()) {
                return std::nullopt;
            }
            const std::optional<Slot> slot = slot_from_letter(text[end - 1]);
            if (!slot.has_value()) {
                return std::nullopt;
            }
            *target = slot;
        }
        begin = end + 1;
    }
    if (!good.has_value() || !try_slot.has_value()) {
        return std::nullopt;
    }
    SlotFile file{};
    file.good = *good;
    file.try_slot = *try_slot;
    return file;
}

std::array<char, slot_file_text_size> format_slot_file(const SlotFile& file) noexcept {
    std::array<char, slot_file_text_size> text{'g', 'o', 'o', 'd', '=', '?', '\n', 't', 'r', 'y', '=', '?', '\n', '\n'};
    text[5] = slot_letter(file.good);
    text[11] = slot_letter(file.try_slot);
    return text;
}

std::optional<std::uint32_t> parse_tries(std::span<const char> text) noexcept {
    // The loader reads "^tries=(%d)"; the supervisor is as lenient about
    // what follows, so both agree on every file.
    if (text.size() < 7 || !equals(text, 0, 6, "tries=")) {
        return std::nullopt;
    }
    const char digit = text[6];
    if (digit < '0' || digit > '9') {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(digit - '0');
}

std::array<char, tries_file_size> format_tries(std::uint32_t count) noexcept {
    std::array<char, tries_file_size> text{};
    const char* const prefix = "tries=";
    for (std::size_t index = 0; index < 6; index += 1) {
        text[index] = prefix[index];
    }
    const std::uint32_t digit = count > 9u ? 9u : count;
    text[6] = static_cast<char>('0' + static_cast<int>(digit));
    for (std::size_t index = 7; index + 1 < tries_file_size; index += 1) {
        text[index] = ' ';
    }
    text[tries_file_size - 1] = '\n';
    return text;
}

BootFacts boot_facts(const char* slot, const char* tries, const char* fallback) noexcept {
    BootFacts facts{};
    facts.slot = std::nullopt;
    facts.tries = 0;
    facts.fallback = false;
    if (slot != nullptr && slot[0] != '\0' && slot[1] == '\0') {
        facts.slot = slot_from_letter(slot[0]);
    }
    if (tries != nullptr && tries[0] >= '0' && tries[0] <= '9' && tries[1] == '\0') {
        facts.tries = static_cast<std::uint32_t>(tries[0] - '0');
    }
    if (fallback != nullptr && fallback[0] == '1' && fallback[1] == '\0') {
        facts.fallback = true;
    }
    return facts;
}

FailsafeReason failsafe_reason(ConfigState config, std::optional<std::uint64_t> restart_count) noexcept {
    if (restart_count.has_value() && *restart_count == failsafe_restart_limit) {
        return FailsafeReason::restart_limit;
    }
    switch (config) {
    case ConfigState::loaded: return FailsafeReason::none;
    case ConfigState::not_read: return FailsafeReason::none;
    case ConfigState::absent: return FailsafeReason::config_absent;
    case ConfigState::refused: return FailsafeReason::config_refused;
    case ConfigState::unreadable: return FailsafeReason::config_unreadable;
    default: return FailsafeReason::config_unreadable;
    }
}

const char* failsafe_reason_name(FailsafeReason reason) noexcept {
    switch (reason) {
    case FailsafeReason::none: return "none";
    case FailsafeReason::config_absent: return "config-absent";
    case FailsafeReason::config_refused: return "config-refused";
    case FailsafeReason::config_unreadable: return "config-unreadable";
    case FailsafeReason::restart_limit: return "restart-limit";
    default: return "unknown";
    }
}

bool boot_is_good(const GoodBootInputs& inputs) noexcept {
    if (!inputs.registered || !inputs.init_done || inputs.failsafe_taken) {
        return false;
    }
    if (!inputs.services_healthy || !inputs.network_up) {
        return false;
    }
    if (inputs.now_ms < inputs.init_done_ms) {
        return false;
    }
    return inputs.now_ms - inputs.init_done_ms >= good_boot_settle_ms;
}

bool services_healthy(std::span<const ServiceHealth> services) noexcept {
    for (const ServiceHealth& service : services) {
        switch (service.state) {
        case ServiceStateKind::running:
        case ServiceStateKind::waiting:
            break;
        case ServiceStateKind::stopped:
            if (!service.restart_never) {
                return false;
            }
            break;
        case ServiceStateKind::stopping:
            return false;
        default:
            return false;
        }
    }
    return true;
}

MarkGoodPlan mark_good_plan(const BootFacts& facts, std::optional<std::uint32_t> current_tries,
                            std::optional<SlotFile> current_slot_file) noexcept {
    MarkGoodPlan plan{};
    plan.write_tries_zero = false;
    plan.new_slot_file = std::nullopt;
    if (!facts.slot.has_value()) {
        return plan;
    }
    const Slot running = *facts.slot;
    plan.write_tries_zero = !current_tries.has_value() || *current_tries != 0u;
    // A slot file whose good slot is this one is left alone: its try may
    // name an update written during this boot, which must not be undone.
    const bool cancel_try = facts.fallback && current_slot_file.has_value() &&
                            (*current_slot_file).try_slot != running;
    if (!current_slot_file.has_value() || (*current_slot_file).good != running || cancel_try) {
        SlotFile file{};
        file.good = running;
        file.try_slot = running;
        plan.new_slot_file = file;
    }
    return plan;
}

}  // namespace stxboot
