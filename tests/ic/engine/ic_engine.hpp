#pragma once

// The block engine (the IC study's phase IC2; notes, gui_forms branch,
// planning/stages/IC-integrity-constraints/STUDY.md, section 5).
//
// tools/ic_declarations.py generates one block per declared call (legal),
// per clause and per refused state (misuse) into tests/ic/generated; this
// engine runs them. A testcase is a sequence: byte 0 picks the component,
// byte 1 the mode (bit 0: misuse blocks are eligible), then each step is
// [block][slot][length][length parameter bytes]. The engine keeps each slot's
// model (live, declared state, terms), offers a step only the blocks whose
// states and slot admit them (typestate lives here, so a block never runs
// where the declaration forbids it), and after each block compares what the
// object reports with what the declaration predicted:
//
// - legal mode: any difference, an undeclared failure or a stop is a defect;
// - misuse mode: the declared refusal must happen (the failure kind and code,
//   the null or value, or the process stopping with the declared message) and
//   the object must be unchanged.
//
// A block whose draw cannot meet its clauses bails (STITCH's bail: the step
// is skipped, nothing ran). Results of the engine are tests, by the owner's
// definition; the deductions are tools/ic_deduce.py's.
//
// Test code: std is allowed here (tests/test_support.hpp), STXlib's own
// rules apply otherwise.

#include "stxlib/error.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

#include <cstdio>
#include <new>

namespace stxlib_ic {

using stxlib::i64;
using stxlib::u32;
using stxlib::u64;
using stxlib::u8;
using stxlib::usize;

inline constexpr usize maximum_terms = 8;
inline constexpr usize maximum_slots = 4;
inline constexpr usize maximum_line = 1024;

// One slot's model: whether it holds an object, the declared state's index
// and the declared terms.
struct Model final {
    bool live{false};
    u32 state{0};
    u64 terms[maximum_terms]{};
};

enum class Outcome : u8 {
    completed,        // the call ran and did what the declaration says
    declared_failure, // the call failed as the declaration allows
    refused,          // a misuse was refused as declared
    shown,            // a stopping misuse, shown by its predicate (no child process here)
    bailed,           // the draw missed the clauses; nothing ran
    skipped,          // no block applies
    defect,           // the object and the declaration disagree
};

enum class DefectKind : u8 {
    none,
    misuse_accepted,    // a misuse ran without its declared refusal
    wrong_refusal,      // refused, but not as declared (kind, code or message)
    refusal_changed,    // refused, but the object changed
    expectation,        // a declared expectation after a legal call was false
    term_mismatch,      // a term the object reports differs from the prediction
    state_mismatch,     // the state query disagrees with the declared state
    invariant,          // a declared invariant does not hold
    undeclared_failure, // a legal call failed with a kind the declaration does not allow
};

enum class BlockMode : u8 { legal, misuse, outside };
enum class Needs : u8 { live, empty, any, none };
enum class Refusal : u8 { none, stops, failure, value };

[[nodiscard]] const char* defect_name(DefectKind kind) noexcept;
[[nodiscard]] const char* outcome_name(Outcome outcome) noexcept;

[[nodiscard]] constexpr u32 kind_bit(const stxlib::ErrorKind kind) noexcept {
    const u32 result = u32{1} << static_cast<u32>(kind);
    return result;
}

// ---- holding an object -------------------------------------------------------------------------

// A slot: storage for one object, which need not be movable (Mutex is not;
// stxlib::Optional needs a movable value).
template <typename Value>
class Holder final {
public:
    Holder() noexcept = default;
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    ~Holder() { reset(); }

    [[nodiscard]] bool present() const noexcept { return present_; }
    [[nodiscard]] Value* get() noexcept { return std::launder(reinterpret_cast<Value*>(storage_)); }
    [[nodiscard]] const Value* get() const noexcept {
        return std::launder(reinterpret_cast<const Value*>(storage_));
    }

    template <typename... Arguments>
    void emplace(Arguments&&... arguments) noexcept {
        reset();
        ::new (static_cast<void*>(storage_)) Value(static_cast<Arguments&&>(arguments)...);
        present_ = true;
    }

    void replace(Value&& value) noexcept { emplace(static_cast<Value&&>(value)); }

    void reset() noexcept {
        if (present_) {
            (*get()).~Value();
            present_ = false;
        }
    }

private:
    alignas(Value) unsigned char storage_[sizeof(Value)]{};
    bool present_{false};
};

template <typename Slots>
[[nodiscard]] void* create_slots() noexcept {
    Slots* const slots = new (std::nothrow) Slots{};
    return slots;
}

template <typename Slots>
void destroy_slots(void* const storage) noexcept {
    delete static_cast<Slots*>(storage);
}

// The transcript's spellings (a transcript line is C++ a test can read; the
// regression tests replay the bytes, the transcript is their comment).
template <typename Value>
void adopt(Holder<Value>& holder, stxlib::OperationResult<Value>&& made) noexcept {
    if (made.succeeded()) holder.replace(static_cast<stxlib::OperationResult<Value>&&>(made).take_value());
}

[[nodiscard]] inline bool refused(const stxlib::OperationStatus& status) noexcept { return status.failed(); }

template <typename Value>
[[nodiscard]] bool refused(const stxlib::OperationResult<Value>& made) noexcept {
    return made.failed();
}

// ---- drawing values ----------------------------------------------------------------------------

// Reads a step's parameter bytes; past their end every read is zero, so a
// shorter testcase is a smaller one (the minimiser shortens toward zero).
class Draw final {
public:
    Draw() noexcept = default;
    Draw(const u8* const bytes, const usize count) noexcept : bytes_(bytes), count_(count) {}

    [[nodiscard]] u8 byte() noexcept;
    [[nodiscard]] u64 word() noexcept;
    [[nodiscard]] bool coin() noexcept { return (byte() & 1U) != 0; }

    // A value in [low, high], biased to the edges and to small offsets.
    [[nodiscard]] u64 pick(u64 low, u64 high) noexcept;
    [[nodiscard]] i64 pick_signed(i64 low, i64 high) noexcept;
    // A power of two in [low, high]; low when there is none (the clause then bails).
    [[nodiscard]] u64 power_of_two(u64 low, u64 high) noexcept;
    // Zero or a value with two bits set, at most high.
    [[nodiscard]] u64 not_power_of_two(u64 high) noexcept;
    // A value of [0, largest] outside [low, high]; false when there is none.
    [[nodiscard]] bool outside(u64 low, u64 high, u64 largest, u64& value) noexcept;
    [[nodiscard]] bool outside_signed(i64 low, i64 high, i64 smallest, i64 largest, i64& value) noexcept;
    [[nodiscard]] double real(double low, double high) noexcept;
    [[nodiscard]] double any_real() noexcept;
    [[nodiscard]] double not_finite() noexcept;

    [[nodiscard]] usize used() const noexcept { return position_; }

private:
    const u8* bytes_{nullptr};
    usize count_{0};
    usize position_{0};
};

// Narrow a bound: false when a strict bound leaves no value.
[[nodiscard]] bool tighten_low(u64& low, u64 value, bool strict) noexcept;
[[nodiscard]] bool tighten_high(u64& high, u64 value, bool strict) noexcept;
[[nodiscard]] bool tighten_low_signed(i64& low, i64 value, bool strict) noexcept;
[[nodiscard]] bool tighten_high_signed(i64& high, i64 value, bool strict) noexcept;

// ---- the transcript ----------------------------------------------------------------------------

class Transcript;

// One line of the transcript, built in pieces; a no-op without a transcript.
class Line final {
public:
    Line& text(const char* piece) noexcept;
    Line& object(usize slot) noexcept;
    Line& holder(usize slot) noexcept;
    Line& unsigned_value(u64 value) noexcept;
    Line& signed_value(i64 value) noexcept;
    Line& real_value(double value) noexcept;
    void end() noexcept;

    void start(Transcript* sink) noexcept {
        sink_ = sink;
        count_ = 0;
        buffer_[0] = '\0';
    }

private:
    Transcript* sink_{nullptr};
    usize count_{0};
    char buffer_[maximum_line]{};
};

class Transcript final {
public:
    Transcript() noexcept = default;
    Transcript(const Transcript&) = delete;
    Transcript& operator=(const Transcript&) = delete;
    ~Transcript();

    void add(const char* text, usize count) noexcept;
    void clear() noexcept;
    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] const char* line(usize index) const noexcept { return lines_[index]; }

private:
    char** lines_{nullptr};
    usize count_{0};
    usize capacity_{0};
};

// ---- a step ------------------------------------------------------------------------------------

struct Step final {
    Draw draw{};
    usize slot{0};
    Model before{};
    Model after{};
    u32 observed{0};          // terms to read from the object after the call
    bool unchanged{false};    // the object must equal `before`
    bool forks{false};        // a stopping misuse runs in a child process
    Refusal refusal{Refusal::none};
    DefectKind defect_kind{DefectKind::none};
    const char* message{nullptr};
    Line line_builder{};

    Line& line() noexcept { return line_builder; }
    void observe_term(const u32 index) noexcept { observed = observed | (u32{1} << index); }

    [[nodiscard]] Outcome bail(const char* why) noexcept;
    [[nodiscard]] Outcome defect(DefectKind kind, const char* what) noexcept;
    // A misuse refused with a failure: kind (0: any) and, when check_code, code.
    [[nodiscard]] Outcome refused(const stxlib::ErrorRecord& error, u32 kind, u32 code, bool check_code) noexcept;
    [[nodiscard]] Outcome refused_by_value() noexcept;
    // A legal call failed: declared when its kind is in `kinds`; the object is
    // then unchanged (as declared) or in `state` with its terms as it reports.
    [[nodiscard]] Outcome failed(const stxlib::ErrorRecord& error, u32 kinds, bool unchanged_after, u32 state) noexcept;
    [[nodiscard]] Outcome failed_by_value(bool declared) noexcept;
    // A misuse that stops the process with `expected` (STXLIB_REQUIRE).
    [[nodiscard]] Outcome requirement(void (*call)(void*), void* context, const char* expected) noexcept;
};

// ---- a component -------------------------------------------------------------------------------

struct BlockInfo final {
    const char* name;
    u32 call;
    BlockMode mode;
    u32 states; // bit per admitting state
    Needs needs;
    Refusal refusal;
};

struct Component final {
    const char* name;
    const char* declaration;
    const char* object;
    usize slot_count;
    u32 term_count;
    const char* const* term_names;
    const char* const* term_reads;
    u32 state_count;
    const char* const* state_names;
    u32 initial;
    u32 call_count;
    const char* const* call_names;
    u32 block_count;
    const BlockInfo* blocks;
    void* (*create)();
    void (*destroy)(void*, const Model*);
    Outcome (*run)(void*, usize, Step&);
    bool (*read)(void*, usize, Model&, u32, bool&, bool&);
    const char* (*invariant_text)();
};

// ---- integrity counts --------------------------------------------------------------------------

inline constexpr usize maximum_blocks = 256;
inline constexpr usize maximum_calls = 64;
inline constexpr usize maximum_states = 8;

// What a run did, per component (the study's integrity_counts): every step's
// outcome, the misuse checks by the refusal they checked, the blocks run, the
// states and edges (call, from, to) reached.
struct IntegrityCounts final {
    u64 cases{0};
    u64 steps{0};
    u64 completed{0};
    u64 declared_failures{0};
    u64 refused_failure{0};
    u64 refused_value{0};
    u64 refused_stops{0};
    u64 shown_stops{0};
    u64 bailed{0};
    u64 skipped{0};
    u64 defects{0};
    u64 legal_defects{0};
    u64 block_runs[maximum_blocks]{};
    u32 states_reached{0};
    u8 edges[maximum_calls][maximum_states]{}; // bit per target state

    [[nodiscard]] u32 state_count() const noexcept;
    [[nodiscard]] u32 edge_count() const noexcept;
    [[nodiscard]] u32 blocks_run(u32 block_count) const noexcept;
};

// What a case found.
struct Finding final {
    bool found{false};
    DefectKind kind{DefectKind::none};
    usize step{0};
    u32 block{0};
    bool legal{false};
    char what[256]{};
};

struct Options final {
    bool forks{false};          // run stopping misuses in a child process (POSIX only)
    usize maximum_steps{64};
    Transcript* transcript{nullptr};
};

// Runs one testcase against `components` (byte 0 picks one). Returns whether
// the case found a defect; counts go to `counts[component]`.
bool run_case(const Component* const* components, usize component_count, const u8* bytes, usize count,
              const Options& options, IntegrityCounts* counts, Finding& finding) noexcept;

// The component a testcase names.
[[nodiscard]] usize component_of(const u8* bytes, usize count, usize component_count) noexcept;

// The index of the component named `name`, or `count` when none is.
[[nodiscard]] usize index_of(const Component* const* components, usize count, const char* name) noexcept;

// Delta debugging over steps, then over parameter bytes: the shortest
// testcase that still finds a defect of the same kind in the same block.
// Writes the result over `bytes` and returns its length.
usize minimise(const Component* const* components, usize component_count, u8* bytes, usize count,
               const Options& options) noexcept;

// Writes the testcase as an ordinary C++ test (tests/ic/regressions): the
// transcript as its comment, the bytes replayed through the engine, checked
// to find nothing. Returns whether the file was written.
bool write_regression(const char* path, const Component& component, const u8* bytes, usize count,
                      const Transcript& transcript, const Finding& finding) noexcept;

} // namespace stxlib_ic
