// program_slots.cpp: see program_slots.h.

#include "program_slots.h"

namespace stxpackage {

Slot other_slot(Slot slot) noexcept {
    return slot == Slot::a ? Slot::b : Slot::a;
}

char slot_letter(Slot slot) noexcept {
    return slot == Slot::a ? 'a' : 'b';
}

std::optional<Slot> slot_of_letter(char letter) noexcept {
    if (letter == 'a') {
        const std::optional<Slot> slot = Slot::a;
        return slot;
    }
    if (letter == 'b') {
        const std::optional<Slot> slot = Slot::b;
        return slot;
    }
    return std::nullopt;
}

ProgramState::ProgramState(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

ProgramState ProgramState::none() noexcept {
    return ProgramState(Alternatives(NoProgram{}));
}

ProgramState ProgramState::good_only(Slot good) noexcept {
    return ProgramState(Alternatives(GoodOnly{good}));
}

ProgramState ProgramState::good_with_trial(Slot good, std::uint8_t failures) noexcept {
    const std::uint8_t bounded = failures < trial_attempts_max ? failures : static_cast<std::uint8_t>(trial_attempts_max - 1u);
    return ProgramState(Alternatives(GoodWithTrial{good, bounded}));
}

ProgramState ProgramState::good_with_previous(Slot good) noexcept {
    return ProgramState(Alternatives(GoodWithPrevious{good}));
}

bool ProgramState::is_none() const noexcept {
    return std::holds_alternative<NoProgram>(alternatives_);
}

const GoodOnly* ProgramState::as_good_only() const noexcept {
    return std::get_if<GoodOnly>(&alternatives_);
}

const GoodWithTrial* ProgramState::as_good_with_trial() const noexcept {
    return std::get_if<GoodWithTrial>(&alternatives_);
}

const GoodWithPrevious* ProgramState::as_good_with_previous() const noexcept {
    return std::get_if<GoodWithPrevious>(&alternatives_);
}

bool ProgramState::operator==(const ProgramState& other) const noexcept {
    if (alternatives_.index() != other.alternatives_.index()) {
        return false;
    }
    const GoodOnly* const only = as_good_only();
    if (only != nullptr) {
        return (*only).good == (*other.as_good_only()).good;
    }
    const GoodWithTrial* const trial = as_good_with_trial();
    if (trial != nullptr) {
        return (*trial).good == (*other.as_good_with_trial()).good &&
               (*trial).failures == (*other.as_good_with_trial()).failures;
    }
    const GoodWithPrevious* const previous = as_good_with_previous();
    if (previous != nullptr) {
        return (*previous).good == (*other.as_good_with_previous()).good;
    }
    return true;
}

const char* slot_note_name(SlotNote note) noexcept {
    switch (note) {
    case SlotNote::unchanged: return "unchanged";
    case SlotNote::first_install: return "first_install";
    case SlotNote::trial_started: return "trial_started";
    case SlotNote::failure_counted: return "failure_counted";
    case SlotNote::fell_back: return "fell_back";
    case SlotNote::kept: return "kept";
    case SlotNote::rolled_back: return "rolled_back";
    case SlotNote::trial_abandoned: return "trial_abandoned";
    case SlotNote::rollback_refused: return "rollback_refused";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

// The good slot of an installed state; a for none (callers check none
// first).
[[nodiscard]] Slot good_of(const ProgramState& state) noexcept {
    const GoodOnly* const only = state.as_good_only();
    if (only != nullptr) {
        return (*only).good;
    }
    const GoodWithTrial* const trial = state.as_good_with_trial();
    if (trial != nullptr) {
        return (*trial).good;
    }
    const GoodWithPrevious* const previous = state.as_good_with_previous();
    if (previous != nullptr) {
        return (*previous).good;
    }
    return Slot::a;
}

[[nodiscard]] SlotTransition unchanged(const ProgramState& state) noexcept {
    return SlotTransition{state, SlotNote::unchanged};
}

}  // namespace

Slot install_slot(const ProgramState& state) noexcept {
    if (state.is_none()) {
        return Slot::a;
    }
    return other_slot(good_of(state));
}

std::optional<Slot> slot_to_run(const ProgramState& state) noexcept {
    if (state.is_none()) {
        return std::nullopt;
    }
    const GoodWithTrial* const trial = state.as_good_with_trial();
    const Slot slot = trial != nullptr ? other_slot((*trial).good) : good_of(state);
    const std::optional<Slot> result = slot;
    return result;
}

SlotTransition after_installed(const ProgramState& state) noexcept {
    if (state.is_none()) {
        return SlotTransition{ProgramState::good_only(Slot::a), SlotNote::first_install};
    }
    return SlotTransition{ProgramState::good_with_trial(good_of(state), 0), SlotNote::trial_started};
}

SlotTransition after_early_exit(const ProgramState& state, Slot ran) noexcept {
    const GoodWithTrial* const trial = state.as_good_with_trial();
    if (trial == nullptr || ran != other_slot((*trial).good)) {
        return unchanged(state);
    }
    const std::uint32_t failures = static_cast<std::uint32_t>((*trial).failures) + 1u;
    if (failures >= trial_attempts_max) {
        return SlotTransition{ProgramState::good_only((*trial).good), SlotNote::fell_back};
    }
    return SlotTransition{ProgramState::good_with_trial((*trial).good, static_cast<std::uint8_t>(failures)),
                          SlotNote::failure_counted};
}

SlotTransition after_refused(const ProgramState& state, Slot ran) noexcept {
    const GoodWithTrial* const trial = state.as_good_with_trial();
    if (trial != nullptr) {
        if (ran != other_slot((*trial).good)) {
            return unchanged(state);
        }
        return SlotTransition{ProgramState::good_only((*trial).good), SlotNote::fell_back};
    }
    const GoodWithPrevious* const previous = state.as_good_with_previous();
    if (previous != nullptr && ran == (*previous).good) {
        return SlotTransition{ProgramState::good_only(other_slot((*previous).good)), SlotNote::fell_back};
    }
    return unchanged(state);
}

SlotTransition after_healthy(const ProgramState& state, Slot ran) noexcept {
    const GoodWithTrial* const trial = state.as_good_with_trial();
    if (trial == nullptr || ran != other_slot((*trial).good)) {
        return unchanged(state);
    }
    return SlotTransition{ProgramState::good_with_previous(ran), SlotNote::kept};
}

SlotTransition after_rollback(const ProgramState& state) noexcept {
    const GoodWithPrevious* const previous = state.as_good_with_previous();
    if (previous != nullptr) {
        return SlotTransition{ProgramState::good_with_previous(other_slot((*previous).good)), SlotNote::rolled_back};
    }
    const GoodWithTrial* const trial = state.as_good_with_trial();
    if (trial != nullptr) {
        return SlotTransition{ProgramState::good_only((*trial).good), SlotNote::trial_abandoned};
    }
    return SlotTransition{state, SlotNote::rollback_refused};
}

// ---------------------------------------------------------------------------
// The state file.

namespace {

void append(std::span<char, state_file_size_max> out, std::size_t& used, const char* text) noexcept {
    for (std::size_t index = 0; text[index] != '\0' && used < state_file_size_max; index += 1) {
        out[used] = text[index];
        used += 1;
    }
}

void append_decimal(std::span<char, state_file_size_max> out, std::size_t& used, std::uint32_t value) noexcept {
    char digits[10] = {};
    std::size_t count = 0;
    std::uint32_t rest = value;
    do {
        digits[count] = static_cast<char>('0' + static_cast<char>(rest % 10u));
        rest /= 10u;
        count += 1;
    } while (rest > 0u && count < sizeof(digits));
    while (count > 0u && used < state_file_size_max) {
        count -= 1;
        out[used] = digits[count];
        used += 1;
    }
}

}  // namespace

std::size_t format_program_state(const StoredState& stored, std::span<char, state_file_size_max> out) noexcept {
    std::size_t used = 0;
    append(out, used, "stx-program-state 1\ngood ");
    if (stored.state.is_none()) {
        append(out, used, "none\n");
    } else {
        const char letter[3] = {slot_letter(good_of(stored.state)), '\n', '\0'};
        append(out, used, letter);
        const GoodWithTrial* const trial = stored.state.as_good_with_trial();
        if (stored.state.as_good_only() != nullptr) {
            append(out, used, "other empty\n");
        } else if (trial != nullptr) {
            const char count[3] = {static_cast<char>('0' + static_cast<char>((*trial).failures)), '\n', '\0'};
            append(out, used, "other trial ");
            append(out, used, count);
        } else {
            append(out, used, "other previous\n");
        }
    }
    append(out, used, "refused ");
    append_decimal(out, used, stored.refused_version);
    append(out, used, "\n");
    return used;
}

std::optional<StoredState> parse_program_state(std::span<const char> text) noexcept {
    if (text.size() > state_file_size_max) {
        return std::nullopt;
    }
    // Canonical spelling: parse loosely by trying every state, then accept
    // only when formatting it gives exactly the text. 11 states; the
    // version is read from the last line.
    const char refused_prefix[] = "refused ";
    const std::size_t prefix_length = sizeof(refused_prefix) - 1u;
    if (text.empty() || text[text.size() - 1u] != '\n') {
        return std::nullopt;
    }
    std::size_t line_begin = text.size() - 1u;
    while (line_begin > 0u && text[line_begin - 1u] != '\n') {
        line_begin -= 1;
    }
    if (text.size() - 1u - line_begin <= prefix_length) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < prefix_length; index += 1) {
        if (text[line_begin + index] != refused_prefix[index]) {
            return std::nullopt;
        }
    }
    const std::size_t digits_begin = line_begin + prefix_length;
    const std::size_t digits_end = text.size() - 1u;
    if (digits_end - digits_begin > 10u) {
        return std::nullopt;
    }
    std::uint64_t version = 0;
    for (std::size_t index = digits_begin; index < digits_end; index += 1) {
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
        version = version * 10u + static_cast<std::uint64_t>(text[index] - '0');
    }
    if (version > UINT32_MAX) {
        return std::nullopt;
    }
    const ProgramState candidates[] = {
        ProgramState::none(),
        ProgramState::good_only(Slot::a),
        ProgramState::good_only(Slot::b),
        ProgramState::good_with_trial(Slot::a, 0),
        ProgramState::good_with_trial(Slot::a, 1),
        ProgramState::good_with_trial(Slot::a, 2),
        ProgramState::good_with_trial(Slot::b, 0),
        ProgramState::good_with_trial(Slot::b, 1),
        ProgramState::good_with_trial(Slot::b, 2),
        ProgramState::good_with_previous(Slot::a),
        ProgramState::good_with_previous(Slot::b),
    };
    for (const ProgramState& candidate : candidates) {
        const StoredState stored{candidate, static_cast<std::uint32_t>(version)};
        char spelled[state_file_size_max] = {};
        const std::size_t length = format_program_state(stored, std::span<char, state_file_size_max>(spelled));
        if (length != text.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t index = 0; index < length && same; index += 1) {
            same = spelled[index] == text[index];
        }
        if (same) {
            const std::optional<StoredState> result = stored;
            return result;
        }
    }
    return std::nullopt;
}

}  // namespace stxpackage
