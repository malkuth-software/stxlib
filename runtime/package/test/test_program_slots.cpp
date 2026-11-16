// test_program_slots.cpp: host tests of the per-program A/B state machine.
//
// Exhaustive run: from "none", every sequence of up to 10 events over the
// five events (installed; early_exit, refused and healthy of the process
// on the slot to run; rollback): 12,207,031 sequences. A model tracks what
// each slot holds (a version number, written by each install into
// install_slot) and which versions fell back. After every event:
// 1. the slot to run holds a version, and that version never fell back;
// 2. an install never writes the good slot;
// 3. a version that ran healthy and was kept stays installed until an
//    install overwrites its slot or a rollback replaces it;
// 4. a trial falls back on exactly its trial_attempts_max-th early exit,
//    or on a refusal, and never otherwise;
// 5. early_exit, refused and healthy of a process on the other slot
//    change nothing.
// And the state file: every state with refused versions 0, 1, 4294967295
// round-trips; every single-byte change (each position, all 256 values)
// and every single-byte deletion of every spelling is either refused or
// read as a state whose spelling is exactly the changed text.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include "../program_slots.h"
#include "../../supervisor/config/test/test_support.h"

namespace {

using stxpackage::ProgramState;
using stxpackage::Slot;
using stxpackage::SlotNote;
using stxpackage::SlotTransition;
using stxtest::check;

void test_examples() {
    const char* const test = "slots examples";
    const SlotTransition first = stxpackage::after_installed(ProgramState::none());
    check(first.note == SlotNote::first_install && first.next == ProgramState::good_only(Slot::a), test,
          "first install is good at once");
    check(stxpackage::install_slot(first.next) == Slot::b, test, "the next install goes to b");
    const SlotTransition trial = stxpackage::after_installed(first.next);
    check(trial.next == ProgramState::good_with_trial(Slot::a, 0) &&
              stxpackage::slot_to_run(trial.next) == std::optional<Slot>(Slot::b),
          test, "the trial runs from b");
    const SlotTransition kept = stxpackage::after_healthy(trial.next, Slot::b);
    check(kept.note == SlotNote::kept && kept.next == ProgramState::good_with_previous(Slot::b), test,
          "kept: b good, a previous");
    const SlotTransition back = stxpackage::after_rollback(kept.next);
    check(back.note == SlotNote::rolled_back && back.next == ProgramState::good_with_previous(Slot::a), test,
          "rollback swaps");
    check(stxpackage::after_rollback(ProgramState::good_only(Slot::a)).note == SlotNote::rollback_refused, test,
          "nothing to roll back to");
    SlotTransition step = stxpackage::after_installed(ProgramState::good_only(Slot::a));
    step = stxpackage::after_early_exit(step.next, Slot::b);
    step = stxpackage::after_early_exit(step.next, Slot::b);
    check(step.note == SlotNote::failure_counted && step.next == ProgramState::good_with_trial(Slot::a, 2), test,
          "two early exits counted");
    step = stxpackage::after_early_exit(step.next, Slot::b);
    check(step.note == SlotNote::fell_back && step.next == ProgramState::good_only(Slot::a), test,
          "the third falls back");
    const SlotTransition refused_good = stxpackage::after_refused(ProgramState::good_with_previous(Slot::b), Slot::b);
    check(refused_good.note == SlotNote::fell_back && refused_good.next == ProgramState::good_only(Slot::a), test,
          "a refused good version falls back to the previous one");
}

constexpr std::size_t event_kinds = 5;
constexpr std::size_t length_max = 10;

struct Model {
    // What each slot holds: 0 nothing, else the version installed there.
    std::uint32_t contents[2];
    // Bit v: version v fell back.
    std::uint64_t fallen;
    std::uint32_t next_version;
    // Early exits of the version now on trial.
    std::uint32_t trial_exits;
};

[[nodiscard]] std::size_t index_of(Slot slot) {
    return slot == Slot::a ? 0u : 1u;
}

[[nodiscard]] std::optional<Slot> trial_slot(const ProgramState& state) {
    const stxpackage::GoodWithTrial* const trial = state.as_good_with_trial();
    if (trial == nullptr) {
        return std::nullopt;
    }
    const std::optional<Slot> slot = stxpackage::other_slot((*trial).good);
    return slot;
}

std::size_t failures = 0;

void fail(const char* what, const std::size_t* events, std::size_t length) {
    failures += 1;
    if (failures <= 5u) {
        std::printf("  %s after:", what);
        for (std::size_t index = 0; index < length; index += 1) {
            std::printf(" %zu", events[index]);
        }
        std::printf("\n");
    }
}

// Applies event kind to state and model; checks the properties.
void apply(std::size_t kind, ProgramState& state, Model& model, const std::size_t* events, std::size_t length) {
    const std::optional<Slot> run = stxpackage::slot_to_run(state);
    const std::optional<Slot> trial_before = trial_slot(state);
    // Property 5, before the event: the other slot's process events change
    // nothing.
    if (run.has_value()) {
        const Slot other = stxpackage::other_slot(*run);
        if (!(stxpackage::after_early_exit(state, other).next == state) ||
            !(stxpackage::after_refused(state, other).next == state) ||
            !(stxpackage::after_healthy(state, other).next == state)) {
            fail("an event of the other slot changed the state", events, length);
        }
    }
    SlotTransition transition{state, SlotNote::unchanged};
    switch (kind) {
    case 0: {
        const Slot written = stxpackage::install_slot(state);
        if (!state.is_none() && run.has_value() && trial_before.has_value() == false && written == *run) {
            fail("install wrote the good slot", events, length);
        }
        if (!state.is_none() && trial_before.has_value() && written != *trial_before) {
            fail("install during a trial did not replace the trial", events, length);
        }
        model.contents[index_of(written)] = model.next_version;
        model.next_version += 1;
        model.trial_exits = 0;
        transition = stxpackage::after_installed(state);
        break;
    }
    case 1:
    case 2:
    case 3: {
        if (!run.has_value()) {
            return;
        }
        if (kind == 1) {
            transition = stxpackage::after_early_exit(state, *run);
        } else if (kind == 2) {
            transition = stxpackage::after_refused(state, *run);
        } else {
            transition = stxpackage::after_healthy(state, *run);
        }
        const bool on_trial = trial_before.has_value() && *trial_before == *run;
        if (kind == 1 && on_trial) {
            model.trial_exits += 1;
            const bool should_fall = model.trial_exits >= stxpackage::trial_attempts_max;
            if (should_fall != (transition.note == SlotNote::fell_back)) {
                fail("fell back other than on the last allowed early exit", events, length);
            }
        } else if (kind == 1 && transition.note != SlotNote::unchanged) {
            fail("an early exit of a kept version changed the state", events, length);
        }
        if (kind == 2 && on_trial && transition.note != SlotNote::fell_back) {
            fail("a refused trial did not fall back", events, length);
        }
        if (kind == 3 && on_trial && transition.note != SlotNote::kept) {
            fail("a healthy trial was not kept", events, length);
        }
        if (kind == 3 && !on_trial && transition.note != SlotNote::unchanged) {
            fail("healthy outside a trial changed the state", events, length);
        }
        if (transition.note == SlotNote::fell_back) {
            model.fallen |= std::uint64_t{1} << model.contents[index_of(*run)];
            model.contents[index_of(*run)] = 0;
        }
        break;
    }
    case 4: {
        transition = stxpackage::after_rollback(state);
        if (transition.note == SlotNote::rollback_refused && !(transition.next == state)) {
            fail("a refused rollback changed the state", events, length);
        }
        if (transition.note == SlotNote::trial_abandoned && trial_before.has_value()) {
            // The abandoned trial's version did not fail; its slot is
            // free for the next install.
            model.contents[index_of(*trial_before)] = 0;
        }
        break;
    }
    default:
        return;
    }
    if (transition.note == SlotNote::kept || transition.note == SlotNote::fell_back ||
        transition.note == SlotNote::trial_abandoned) {
        model.trial_exits = 0;
    }
    state = transition.next;
    // Property 1.
    const std::optional<Slot> now = stxpackage::slot_to_run(state);
    if (now.has_value()) {
        const std::uint32_t version = model.contents[index_of(*now)];
        if (version == 0u) {
            fail("the slot to run holds nothing", events, length);
        } else if ((model.fallen >> version) & 1u) {
            fail("a fallen version would run", events, length);
        }
    } else if (model.next_version > 1u) {
        fail("nothing to run after an install", events, length);
    }
    // Property 3: a previous slot holds a version that did not fall back.
    const stxpackage::GoodWithPrevious* const previous = state.as_good_with_previous();
    if (previous != nullptr) {
        const std::uint32_t kept = model.contents[index_of(stxpackage::other_slot((*previous).good))];
        if (kept == 0u || ((model.fallen >> kept) & 1u) != 0u) {
            fail("the previous slot holds nothing usable", events, length);
        }
    }
    // Property 4's count agrees with the state's.
    const stxpackage::GoodWithTrial* const trial = state.as_good_with_trial();
    if (trial != nullptr && (*trial).failures != model.trial_exits) {
        fail("the trial's failure count differs from the model's", events, length);
    }
}

void test_exhaustive() {
    std::size_t sequences = 0;
    std::size_t states_seen = 0;
    bool seen[11] = {};
    std::size_t events[length_max] = {};
    for (std::size_t length = 0; length <= length_max; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= event_kinds;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                events[position] = rest % event_kinds;
                rest /= event_kinds;
            }
            ProgramState state = ProgramState::none();
            Model model{{0, 0}, 0, 1, 0};
            for (std::size_t position = 0; position < length; position += 1) {
                apply(events[position], state, model, events, position + 1u);
            }
            // Which of the 11 states this sequence ends in.
            std::size_t which = 0;
            if (state.as_good_only() != nullptr) {
                which = 1u + index_of((*state.as_good_only()).good);
            } else if (state.as_good_with_trial() != nullptr) {
                which = 3u + 3u * index_of((*state.as_good_with_trial()).good) +
                        static_cast<std::size_t>((*state.as_good_with_trial()).failures);
            } else if (state.as_good_with_previous() != nullptr) {
                which = 9u + index_of((*state.as_good_with_previous()).good);
            }
            if (which < 11u && !seen[which]) {
                seen[which] = true;
                states_seen += 1;
            }
            sequences += 1;
        }
    }
    std::printf("exhaustive slots: %zu sequences, %zu of 11 states reached, %zu failures\n", sequences, states_seen,
                failures);
    check(failures == 0u, "exhaustive slots", "every property after every event");
    check(sequences == 12207031u && states_seen == 11u, "exhaustive slots", "the domain covered, all states reached");
}

void test_state_file() {
    const ProgramState states[] = {
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
    const std::uint32_t versions[] = {0u, 1u, 4294967295u};
    std::size_t mutations = 0;
    std::size_t mutation_failures = 0;
    for (const ProgramState& state : states) {
        for (const std::uint32_t version : versions) {
            const stxpackage::StoredState stored{state, version};
            char text[stxpackage::state_file_size_max] = {};
            const std::size_t length = stxpackage::format_program_state(
                stored, std::span<char, stxpackage::state_file_size_max>(text));
            const std::optional<stxpackage::StoredState> read =
                stxpackage::parse_program_state(std::span<const char>(text, length));
            check(read.has_value() && (*read).state == state && (*read).refused_version == version, "state file",
                  "round trip");
            char changed[stxpackage::state_file_size_max] = {};
            for (std::size_t position = 0; position < length; position += 1) {
                for (std::uint32_t byte = 0; byte < 256u; byte += 1) {
                    std::memcpy(changed, text, length);
                    changed[position] = static_cast<char>(byte);
                    const std::optional<stxpackage::StoredState> mutated =
                        stxpackage::parse_program_state(std::span<const char>(changed, length));
                    if (mutated.has_value()) {
                        char spelled[stxpackage::state_file_size_max] = {};
                        const std::size_t spelled_length = stxpackage::format_program_state(
                            *mutated, std::span<char, stxpackage::state_file_size_max>(spelled));
                        if (spelled_length != length || std::memcmp(spelled, changed, length) != 0) {
                            mutation_failures += 1;
                        }
                    }
                    mutations += 1;
                }
                // Deletion of the byte at position.
                std::memcpy(changed, text, position);
                std::memcpy(&changed[position], &text[position + 1u], length - position - 1u);
                const std::optional<stxpackage::StoredState> shorter =
                    stxpackage::parse_program_state(std::span<const char>(changed, length - 1u));
                if (shorter.has_value()) {
                    char spelled[stxpackage::state_file_size_max] = {};
                    const std::size_t spelled_length = stxpackage::format_program_state(
                        *shorter, std::span<char, stxpackage::state_file_size_max>(spelled));
                    if (spelled_length != length - 1u || std::memcmp(spelled, changed, length - 1u) != 0) {
                        mutation_failures += 1;
                    }
                }
                mutations += 1;
            }
        }
    }
    std::printf("state file: %zu mutations, %zu not canonical\n", mutations, mutation_failures);
    check(mutation_failures == 0u, "state file", "every accepted mutation is canonical");
}

}  // namespace

int main() {
    test_examples();
    test_exhaustive();
    test_state_file();
    return stxtest::finish("test_program_slots");
}
