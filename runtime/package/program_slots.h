// program_slots.h: which of a program's two slots runs, and how an install
// becomes the running version (C2: A/B per program, appliance/C2_DESIGN.md,
// "Programs").
//
// Pure code: no system call, no operating-system header, no allocation.
// Tested on the host (test/test_program_slots.cpp), exhaustively: every
// state against every sequence of events up to a bound, against a model of
// the two slots' contents.
//
// Every program in the store (/data/programs/<program>/) has slots a and b.
// The supervisor is the only writer of a program's state; stxpkg writes
// slots, never the state, and only the slot install_slot names. States:
//
//   none                       nothing installed
//   good=g, other=empty        g runs; the other slot holds nothing usable
//   good=g, other=trial(n)     a new version is on trial in the other slot
//                              and runs; n early exits so far (0..2)
//   good=g, other=previous     g runs; the other slot keeps the version g
//                              replaced, for a rollback
//
// 1 + 2 x (1 + 3 + 1) = 11 states. Events (ran: the slot the process that
// the event concerns was started on):
//
//   installed           an install into install_slot finished: none ->
//                       good=a, other=empty (nothing to fall back to, so
//                       the first version is kept at once); otherwise ->
//                       other=trial(0) (any previous version is gone: the
//                       install overwrote its slot)
//   early_exit(ran)     the process exited before healthy_after: on the
//                       trial slot, trial(n) -> trial(n + 1), and at
//                       trial_attempts_max early exits -> other=empty (fell
//                       back); otherwise nothing changes
//   refused(ran)        the launcher refused the package in ran (exit 125):
//                       on the trial slot -> other=empty (fell back); on
//                       the good slot with a previous version -> good=the
//                       previous slot, other=empty (fell back); otherwise
//                       nothing changes (nothing verified is left to run)
//   healthy(ran)        the process ran healthy_after: on the trial slot ->
//                       good=that slot, other=previous (kept); otherwise
//                       nothing changes
//   rollback            by request: previous -> the two swap (good=the
//                       other slot, other=previous: a second rollback rolls
//                       forward); trial -> other=empty (abandoned); empty
//                       or none -> refused, nothing changes
//
// What the exhaustive test checks after every event: the slot to run holds
// an installed version that has not failed; an install never writes the
// good slot; a version that fell back is never run again; at most
// trial_attempts_max starts of a trial end early before it falls back.

#ifndef STXLIB_RUNTIME_PACKAGE_PROGRAM_SLOTS_H
#define STXLIB_RUNTIME_PACKAGE_PROGRAM_SLOTS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace stxpackage {

constexpr std::uint8_t trial_attempts_max = 3;

enum class Slot : std::uint8_t {
    a,
    b,
};

[[nodiscard]] Slot other_slot(Slot slot) noexcept;
[[nodiscard]] char slot_letter(Slot slot) noexcept;
[[nodiscard]] std::optional<Slot> slot_of_letter(char letter) noexcept;

struct NoProgram {
};
struct GoodOnly {
    Slot good;
};
struct GoodWithTrial {
    Slot good;
    // Early exits of the trial so far: 0..trial_attempts_max - 1.
    std::uint8_t failures;
};
struct GoodWithPrevious {
    Slot good;
};

class ProgramState {
public:
    [[nodiscard]] static ProgramState none() noexcept;
    [[nodiscard]] static ProgramState good_only(Slot good) noexcept;
    // failures must be below trial_attempts_max.
    [[nodiscard]] static ProgramState good_with_trial(Slot good, std::uint8_t failures) noexcept;
    [[nodiscard]] static ProgramState good_with_previous(Slot good) noexcept;

    [[nodiscard]] bool is_none() const noexcept;
    [[nodiscard]] const GoodOnly* as_good_only() const noexcept;
    [[nodiscard]] const GoodWithTrial* as_good_with_trial() const noexcept;
    [[nodiscard]] const GoodWithPrevious* as_good_with_previous() const noexcept;

    [[nodiscard]] bool operator==(const ProgramState& other) const noexcept;

private:
    using Alternatives = std::variant<NoProgram, GoodOnly, GoodWithTrial, GoodWithPrevious>;
    explicit ProgramState(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// What a transition did, for the console and the targets layer.
enum class SlotNote : std::uint8_t {
    unchanged,
    first_install,     // none -> good=a
    trial_started,     // an install is on trial
    failure_counted,   // a trial's early exit, below the limit
    fell_back,         // the trial (or a refused good version) was dropped
    kept,              // the trial became good
    rolled_back,       // previous and good swapped
    trial_abandoned,   // rollback during a trial
    rollback_refused,  // nothing to roll back to
};

[[nodiscard]] const char* slot_note_name(SlotNote note) noexcept;

struct SlotTransition {
    ProgramState next;
    SlotNote note;
};

// The slot an install writes: a when nothing is installed, else the slot
// that is not good.
[[nodiscard]] Slot install_slot(const ProgramState& state) noexcept;
// The slot to start, or nothing (none).
[[nodiscard]] std::optional<Slot> slot_to_run(const ProgramState& state) noexcept;

[[nodiscard]] SlotTransition after_installed(const ProgramState& state) noexcept;
[[nodiscard]] SlotTransition after_early_exit(const ProgramState& state, Slot ran) noexcept;
[[nodiscard]] SlotTransition after_refused(const ProgramState& state, Slot ran) noexcept;
[[nodiscard]] SlotTransition after_healthy(const ProgramState& state, Slot ran) noexcept;
[[nodiscard]] SlotTransition after_rollback(const ProgramState& state) noexcept;

// ---------------------------------------------------------------------------
// The state file, /data/programs/<program>/state: one canonical spelling
// per state,
//
//   stx-program-state 1
//   good <none|a|b>
//   other <empty|trial 0|trial 1|trial 2|previous>     (absent for none)
//   refused <version>                                   0 for none
//
// refused is the last version that fell back; the update service does not
// install that version again (the supervisor records it; program_slots
// itself does not interpret it).

constexpr std::size_t state_file_size_max = 96;

struct StoredState {
    ProgramState state;
    std::uint32_t refused_version;
};

// The file's bytes for stored, into out (state_file_size_max bytes); the
// length written.
[[nodiscard]] std::size_t format_program_state(const StoredState& stored,
                                               std::span<char, state_file_size_max> out) noexcept;
// The state the bytes spell, or nothing when they are not exactly one
// canonical spelling.
[[nodiscard]] std::optional<StoredState> parse_program_state(std::span<const char> text) noexcept;

}  // namespace stxpackage

#endif  // STXLIB_RUNTIME_PACKAGE_PROGRAM_SLOTS_H
