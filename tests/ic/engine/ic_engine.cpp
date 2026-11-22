// The block engine (ic_engine.hpp): drawing, the transcript, the oracle after
// each step, the minimiser and the regression writer.

#include "ic/engine/ic_engine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define STXLIB_IC_FORKS 1
#endif

namespace stxlib_ic {

const char* defect_name(const DefectKind kind) noexcept {
    switch (kind) {
    case DefectKind::none:
        return "none";
    case DefectKind::misuse_accepted:
        return "misuse accepted";
    case DefectKind::wrong_refusal:
        return "wrong refusal";
    case DefectKind::refusal_changed:
        return "refusal changed the object";
    case DefectKind::expectation:
        return "expectation false";
    case DefectKind::term_mismatch:
        return "term mismatch";
    case DefectKind::state_mismatch:
        return "state mismatch";
    case DefectKind::invariant:
        return "invariant broken";
    case DefectKind::undeclared_failure:
        return "undeclared failure";
    }
    return "unknown";
}

const char* outcome_name(const Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::completed:
        return "completed";
    case Outcome::declared_failure:
        return "declared failure";
    case Outcome::refused:
        return "refused";
    case Outcome::shown:
        return "shown";
    case Outcome::bailed:
        return "bailed";
    case Outcome::skipped:
        return "skipped";
    case Outcome::defect:
        return "defect";
    }
    return "unknown";
}

// ---- drawing -----------------------------------------------------------------------------------

u8 Draw::byte() noexcept {
    u8 result = 0;
    if (position_ < count_) result = bytes_[position_];
    position_ = position_ + 1;
    return result;
}

u64 Draw::word() noexcept {
    u64 result = 0;
    for (u32 index = 0; index < 8; ++index) result = result | (static_cast<u64>(byte()) << (8U * index));
    return result;
}

u64 Draw::pick(const u64 low, const u64 high) noexcept {
    if (low >= high) return low;
    const u64 span = high - low;
    const u8 selector = static_cast<u8>(byte() % 8U);
    if (selector == 0) return low;
    if (selector == 1) return high;
    if (selector == 2) return low + 1;
    if (selector == 3) return high - 1;
    if (selector == 4 || selector == 5) {
        const u64 offset = byte();
        const u64 result = offset <= span ? low + offset : low + offset % (span + 1);
        return result;
    }
    const u64 random = word();
    if (span == ~u64{0}) return random;
    const u64 result = low + random % (span + 1);
    return result;
}

i64 Draw::pick_signed(const i64 low, const i64 high) noexcept {
    if (low >= high) return low;
    // The offset from low, in unsigned arithmetic (the span may exceed i64's range).
    const u64 span = static_cast<u64>(high) - static_cast<u64>(low);
    const u64 offset = pick(0, span);
    const i64 result = static_cast<i64>(static_cast<u64>(low) + offset);
    return result;
}

u64 Draw::power_of_two(const u64 low, const u64 high) noexcept {
    u32 exponents[64]{};
    u32 count = 0;
    for (u32 exponent = 0; exponent < 64; ++exponent) {
        const u64 value = u64{1} << exponent;
        if (value >= low && value <= high) {
            exponents[count] = exponent;
            count = count + 1;
        }
    }
    if (count == 0) return low;
    const u64 result = u64{1} << exponents[byte() % count];
    return result;
}

u64 Draw::not_power_of_two(const u64 high) noexcept {
    if (high < 3 || byte() % 4U == 0) return 0;
    u64 value = pick(3, high);
    if ((value & (value - 1)) == 0) value = value - 1; // 2^k - 1 with k >= 2: two bits or more
    return value;
}

bool Draw::outside(const u64 low, const u64 high, const u64 largest, u64& value) noexcept {
    const bool below = low > 0 && low <= largest;
    const bool above = high < largest;
    if (low > high) {
        // Nothing is admitted: any value is outside.
        value = pick(0, largest);
        return true;
    }
    if (!below && !above) return false;
    if (below && (!above || coin())) {
        value = coin() ? low - 1 : pick(0, low - 1);
        return true;
    }
    value = coin() ? high + 1 : pick(high + 1, largest);
    return true;
}

bool Draw::outside_signed(const i64 low, const i64 high, const i64 smallest, const i64 largest, i64& value) noexcept {
    const bool below = low > smallest;
    const bool above = high < largest;
    if (low > high) {
        value = pick_signed(smallest, largest);
        return true;
    }
    if (!below && !above) return false;
    if (below && (!above || coin())) {
        value = coin() ? low - 1 : pick_signed(smallest, low - 1);
        return true;
    }
    value = coin() ? high + 1 : pick_signed(high + 1, largest);
    return true;
}

double Draw::real(const double low, const double high) noexcept {
    const u8 selector = static_cast<u8>(byte() % 4U);
    if (selector == 0) return low;
    if (selector == 1) return high;
    const double unit = static_cast<double>(word() >> 11U) * (1.0 / 9007199254740992.0);
    const double result = low + (high - low) * unit;
    return result;
}

double Draw::any_real() noexcept {
    const u8 selector = static_cast<u8>(byte() % 8U);
    if (selector == 0) return 0.0;
    if (selector == 1) return -0.0;
    if (selector == 2) return 1.0;
    if (selector == 3) return 1.0e300;
    if (selector == 4) return 4.9406564584124654e-324;
    const u64 bits = word();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!__builtin_isfinite(value)) value = 0.5;
    return value;
}

double Draw::not_finite() noexcept {
    const u8 selector = static_cast<u8>(byte() % 3U);
    if (selector == 0) return __builtin_inf();
    if (selector == 1) return -__builtin_inf();
    return __builtin_nan("");
}

bool tighten_low(u64& low, u64 value, const bool strict) noexcept {
    if (strict) {
        if (value == ~u64{0}) return false;
        value = value + 1;
    }
    if (value > low) low = value;
    return true;
}

bool tighten_high(u64& high, u64 value, const bool strict) noexcept {
    if (strict) {
        if (value == 0) return false;
        value = value - 1;
    }
    if (value < high) high = value;
    return true;
}

bool tighten_low_signed(i64& low, i64 value, const bool strict) noexcept {
    if (strict) {
        if (value == static_cast<i64>(~u64{0} >> 1U)) return false;
        value = value + 1;
    }
    if (value > low) low = value;
    return true;
}

bool tighten_high_signed(i64& high, i64 value, const bool strict) noexcept {
    if (strict) {
        if (value == -static_cast<i64>(~u64{0} >> 1U) - 1) return false;
        value = value - 1;
    }
    if (value < high) high = value;
    return true;
}

// ---- the transcript ----------------------------------------------------------------------------

Line& Line::text(const char* const piece) noexcept {
    if (sink_ == nullptr) return *this;
    usize index = 0;
    while (piece[index] != '\0' && count_ + 1 < maximum_line) {
        buffer_[count_] = piece[index];
        count_ = count_ + 1;
        index = index + 1;
    }
    buffer_[count_] = '\0';
    return *this;
}

Line& Line::object(const usize slot) noexcept {
    char piece[48]{};
    std::snprintf(piece, sizeof(piece), "(*holder%zu.get())", slot);
    return text(piece);
}

Line& Line::holder(const usize slot) noexcept {
    char piece[32]{};
    std::snprintf(piece, sizeof(piece), "holder%zu", slot);
    return text(piece);
}

Line& Line::unsigned_value(const u64 value) noexcept {
    char piece[32]{};
    std::snprintf(piece, sizeof(piece), value <= 2147483647ULL ? "%llu" : "%lluULL",
                  static_cast<unsigned long long>(value));
    return text(piece);
}

Line& Line::signed_value(const i64 value) noexcept {
    char piece[40]{};
    if (value == -static_cast<i64>(~u64{0} >> 1U) - 1) return text("(-9223372036854775807LL - 1)");
    std::snprintf(piece, sizeof(piece), "%lld", static_cast<long long>(value));
    return text(piece);
}

Line& Line::real_value(const double value) noexcept {
    char piece[40]{};
    std::snprintf(piece, sizeof(piece), "%.17g", value);
    return text(piece);
}

void Line::end() noexcept {
    if (sink_ == nullptr) return;
    (*sink_).add(buffer_, count_);
    count_ = 0;
    buffer_[0] = '\0';
}

Transcript::~Transcript() {
    clear();
    std::free(static_cast<void*>(lines_));
}

void Transcript::add(const char* const text, const usize count) noexcept {
    if (count_ == capacity_) {
        const usize grown = capacity_ == 0 ? 32 : capacity_ * 2;
        char** const moved = static_cast<char**>(std::realloc(static_cast<void*>(lines_), grown * sizeof(char*)));
        if (moved == nullptr) return;
        lines_ = moved;
        capacity_ = grown;
    }
    char* const copy = static_cast<char*>(std::malloc(count + 1));
    if (copy == nullptr) return;
    std::memcpy(copy, text, count);
    copy[count] = '\0';
    lines_[count_] = copy;
    count_ = count_ + 1;
}

void Transcript::clear() noexcept {
    for (usize index = 0; index < count_; ++index) std::free(static_cast<void*>(lines_[index]));
    count_ = 0;
}

// ---- a step ------------------------------------------------------------------------------------

Outcome Step::bail(const char* const why) noexcept {
    message = why;
    return Outcome::bailed;
}

Outcome Step::defect(const DefectKind kind, const char* const what) noexcept {
    defect_kind = kind;
    message = what;
    return Outcome::defect;
}

Outcome Step::refused(const stxlib::ErrorRecord& error, const u32 kind, const u32 code,
                      const bool check_code) noexcept {
    refusal = Refusal::failure;
    unchanged = true;
    if (kind != 0 && (kind_bit(error.kind()) & kind) == 0) {
        return defect(DefectKind::wrong_refusal, "refused with another error kind");
    }
    if (check_code && error.code() != code) return defect(DefectKind::wrong_refusal, "refused with another code");
    return Outcome::refused;
}

Outcome Step::refused_by_value() noexcept {
    refusal = Refusal::value;
    unchanged = true;
    return Outcome::refused;
}

Outcome Step::failed(const stxlib::ErrorRecord& error, const u32 kinds, const bool unchanged_after,
                     const u32 state) noexcept {
    if ((kinds & kind_bit(error.kind())) == 0) {
        return defect(DefectKind::undeclared_failure, "failed with a kind the declaration does not allow");
    }
    unchanged = unchanged_after;
    if (!unchanged) {
        after.state = state;
        observed = ~u32{0};
    }
    return Outcome::declared_failure;
}

Outcome Step::failed_by_value(const bool declared) noexcept {
    if (!declared) return defect(DefectKind::undeclared_failure, "failed, which the declaration does not allow");
    unchanged = true;
    return Outcome::declared_failure;
}

Outcome Step::requirement(void (*const call)(void*), void* const context, const char* const expected) noexcept {
    refusal = Refusal::stops;
    unchanged = true;
#if defined(STXLIB_IC_FORKS)
    if (!forks) return Outcome::shown;
    int ends[2]{};
    if (::pipe(ends) != 0) return Outcome::shown;
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return Outcome::shown;
    }
    if (child == 0) {
        ::dup2(ends[1], 2);
        ::close(ends[0]);
        (*call)(context);
        ::_exit(0);
    }
    ::close(ends[1]);
    char text[4096]{};
    usize got = 0;
    for (;;) {
        if (got + 1 >= sizeof(text)) {
            char spill[256]{};
            if (::read(ends[0], spill, sizeof(spill)) <= 0) break;
            continue;
        }
        const ssize_t read_count = ::read(ends[0], text + got, sizeof(text) - 1 - got);
        if (read_count <= 0) break;
        got = got + static_cast<usize>(read_count);
    }
    ::close(ends[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    text[got] = '\0';
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return defect(DefectKind::misuse_accepted, "the call returned instead of stopping");
    }
    if (std::strstr(text, expected) == nullptr) {
        return defect(DefectKind::wrong_refusal, "stopped without the declared message");
    }
    return Outcome::refused;
#else
    static_cast<void>(call);
    static_cast<void>(context);
    static_cast<void>(expected);
    return Outcome::shown;
#endif
}

// ---- counts ------------------------------------------------------------------------------------

u32 IntegrityCounts::state_count() const noexcept {
    const u32 result = static_cast<u32>(__builtin_popcount(states_reached));
    return result;
}

u32 IntegrityCounts::edge_count() const noexcept {
    u32 result = 0;
    for (usize call = 0; call < maximum_calls; ++call) {
        for (usize from = 0; from < maximum_states; ++from) {
            result = result + static_cast<u32>(__builtin_popcount(edges[call][from]));
        }
    }
    return result;
}

u32 IntegrityCounts::blocks_run(const u32 block_count) const noexcept {
    u32 result = 0;
    for (u32 block = 0; block < block_count && block < maximum_blocks; ++block) {
        if (block_runs[block] != 0) result = result + 1;
    }
    return result;
}

// ---- running a case ----------------------------------------------------------------------------

usize component_of(const u8* const bytes, const usize count, const usize component_count) noexcept {
    if (count == 0 || component_count == 0) return 0;
    const usize result = bytes[0] % component_count;
    return result;
}

namespace {

bool admits(const BlockInfo& info, const Model& model, const bool misuse) noexcept {
    if (!misuse && info.mode != BlockMode::legal) return false;
    if (info.needs == Needs::live && !model.live) return false;
    if (info.needs == Needs::empty && model.live) return false;
    const bool uses_state = info.needs == Needs::live || (info.needs == Needs::any && model.live);
    if (uses_state && model.state < 32 && (info.states & (u32{1} << model.state)) == 0) return false;
    return true;
}

void describe(Finding& finding, const Component& component, const Step& step, const u32 block,
              const char* const detail) noexcept {
    std::snprintf(finding.what, sizeof(finding.what), "%s, %s: %s: %s", component.name,
                  component.blocks[block].name, defect_name(step.defect_kind),
                  detail != nullptr ? detail : (step.message != nullptr ? step.message : ""));
}

// Compares the object in the step's slot with `expected`: terms (those in
// `observed` are taken from the object instead), state and invariants.
// Returns the defect, or none.
DefectKind compare(const Component& component, void* const storage, Step& step, Model& expected, const u32 observed,
                   char* const detail, const usize detail_size) noexcept {
    if (!expected.live) return DefectKind::none;
    Model reported{};
    bool agrees = true;
    bool whole = true;
    if (!(*component.read)(storage, step.slot, reported, expected.state, agrees, whole)) {
        std::snprintf(detail, detail_size, "the slot is empty, the declaration predicts an object");
        return DefectKind::state_mismatch;
    }
    for (u32 term = 0; term < component.term_count && term < maximum_terms; ++term) {
        if ((observed & (u32{1} << term)) != 0) {
            expected.terms[term] = reported.terms[term];
            continue;
        }
        if (expected.terms[term] != reported.terms[term]) {
            std::snprintf(detail, detail_size, "%s (read as %s): predicted %llu, reported %llu",
                          component.term_names[term], component.term_reads[term],
                          static_cast<unsigned long long>(expected.terms[term]),
                          static_cast<unsigned long long>(reported.terms[term]));
            return DefectKind::term_mismatch;
        }
    }
    if (!agrees) {
        std::snprintf(detail, detail_size, "the object does not report the declared state %s",
                      expected.state < component.state_count ? component.state_names[expected.state] : "?");
        return DefectKind::state_mismatch;
    }
    if (!whole) {
        std::snprintf(detail, detail_size, "%s", (*component.invariant_text)());
        return DefectKind::invariant;
    }
    return DefectKind::none;
}

} // namespace

bool run_case(const Component* const* const components, const usize component_count, const u8* const bytes,
              const usize count, const Options& options, IntegrityCounts* const counts, Finding& finding) noexcept {
    finding = Finding{};
    if (component_count == 0 || count < 2) return false;
    const usize which = component_of(bytes, count, component_count);
    const Component& component = *components[which];
    IntegrityCounts* const tally = counts != nullptr ? counts + which : nullptr;
    const bool misuse = (bytes[1] & 1U) != 0;
    void* const storage = (*component.create)();
    if (storage == nullptr) return false;
    Model models[maximum_slots]{};
    for (usize slot = 0; slot < maximum_slots; ++slot) models[slot].state = component.initial;
    if (tally != nullptr) (*tally).cases = (*tally).cases + 1;
    if (options.transcript != nullptr) (*options.transcript).clear();
    const usize slot_count = component.slot_count == 0 ? 1 : component.slot_count;
    u32 eligible[maximum_blocks]{};
    usize position = 2;
    usize steps = 0;
    while (position + 3 <= count && steps < options.maximum_steps) {
        const u8 chooser = bytes[position];
        const usize slot = bytes[position + 1] % slot_count;
        const usize length = bytes[position + 2];
        const usize remaining = count - position - 3;
        const usize available = length < remaining ? length : remaining;
        const u8* const parameters = bytes + position + 3;
        position = position + 3 + available;
        steps = steps + 1;
        Model& model = models[slot];
        u32 eligible_count = 0;
        for (u32 block = 0; block < component.block_count && block < maximum_blocks; ++block) {
            if (admits(component.blocks[block], model, misuse)) {
                eligible[eligible_count] = block;
                eligible_count = eligible_count + 1;
            }
        }
        if (tally != nullptr) (*tally).steps = (*tally).steps + 1;
        if (eligible_count == 0) {
            if (tally != nullptr) (*tally).skipped = (*tally).skipped + 1;
            continue;
        }
        const u32 block = eligible[chooser % eligible_count];
        const BlockInfo& info = component.blocks[block];
        Step step{};
        step.draw = Draw(parameters, available);
        step.slot = slot;
        step.before = model;
        step.after = model;
        step.forks = options.forks;
        step.line().start(options.transcript);
        Outcome outcome = (*component.run)(storage, block, step);
        if (tally != nullptr) (*tally).block_runs[block] = (*tally).block_runs[block] + 1;
        char detail[200]{};
        if (outcome == Outcome::completed || outcome == Outcome::declared_failure || outcome == Outcome::refused
            || outcome == Outcome::shown) {
            Model expected = step.unchanged ? step.before : step.after;
            const u32 observed = step.unchanged ? 0U : step.observed;
            const DefectKind found = compare(component, storage, step, expected, observed, detail, sizeof(detail));
            if (found != DefectKind::none) {
                step.defect_kind = (outcome == Outcome::refused || outcome == Outcome::shown)
                                           && found == DefectKind::term_mismatch
                                       ? DefectKind::refusal_changed
                                       : found;
                outcome = Outcome::defect;
            } else if (outcome == Outcome::completed || outcome == Outcome::declared_failure) {
                if (tally != nullptr && expected.live && info.call < maximum_calls && step.before.state < maximum_states
                    && expected.state < 8) {
                    (*tally).states_reached = (*tally).states_reached | (u32{1} << expected.state);
                    if (outcome == Outcome::completed) {
                        (*tally).edges[info.call][step.before.state] = static_cast<u8>(
                            (*tally).edges[info.call][step.before.state] | (1U << expected.state));
                    }
                }
                model = expected;
            }
        }
        if (tally != nullptr) {
            switch (outcome) {
            case Outcome::completed:
                (*tally).completed = (*tally).completed + 1;
                break;
            case Outcome::declared_failure:
                (*tally).declared_failures = (*tally).declared_failures + 1;
                break;
            case Outcome::refused:
                if (step.refusal == Refusal::failure) (*tally).refused_failure = (*tally).refused_failure + 1;
                if (step.refusal == Refusal::value) (*tally).refused_value = (*tally).refused_value + 1;
                if (step.refusal == Refusal::stops) (*tally).refused_stops = (*tally).refused_stops + 1;
                break;
            case Outcome::shown:
                (*tally).shown_stops = (*tally).shown_stops + 1;
                break;
            case Outcome::bailed:
                (*tally).bailed = (*tally).bailed + 1;
                break;
            case Outcome::skipped:
                (*tally).skipped = (*tally).skipped + 1;
                break;
            case Outcome::defect:
                (*tally).defects = (*tally).defects + 1;
                if (info.mode == BlockMode::legal) (*tally).legal_defects = (*tally).legal_defects + 1;
                break;
            }
        }
        if (outcome == Outcome::defect) {
            finding.found = true;
            finding.kind = step.defect_kind;
            finding.step = steps - 1;
            finding.block = block;
            finding.legal = info.mode == BlockMode::legal;
            describe(finding, component, step, block, detail[0] != '\0' ? detail : nullptr);
            break;
        }
    }
    (*component.destroy)(storage, models);
    return finding.found;
}

// ---- minimising --------------------------------------------------------------------------------

namespace {

constexpr usize maximum_case = 1U << 16U;

struct Span final {
    usize offset;
    usize size;
};

usize spans_of(const u8* const bytes, const usize count, Span* const spans, const usize capacity) noexcept {
    usize found = 0;
    usize position = 2;
    while (position + 3 <= count && found < capacity) {
        const usize remaining = count - position - 3;
        const usize length = bytes[position + 2] < remaining ? bytes[position + 2] : remaining;
        spans[found] = Span{position, 3 + length};
        found = found + 1;
        position = position + 3 + length;
    }
    return found;
}

bool same_failure(const Component* const* const components, const usize component_count, const u8* const bytes,
                  const usize count, const Options& options, const Finding& wanted) noexcept {
    Finding finding{};
    Options quiet = options;
    quiet.transcript = nullptr;
    const bool found = run_case(components, component_count, bytes, count, quiet, nullptr, finding);
    const bool result = found && finding.kind == wanted.kind && finding.block == wanted.block;
    return result;
}

} // namespace

usize minimise(const Component* const* const components, const usize component_count, u8* const bytes,
               const usize count, const Options& options) noexcept {
    if (count > maximum_case || count < 2) return count;
    Finding wanted{};
    Options quiet = options;
    quiet.transcript = nullptr;
    if (!run_case(components, component_count, bytes, count, quiet, nullptr, wanted)) return count;
    u8* const trial = static_cast<u8*>(std::malloc(count));
    Span* const spans = static_cast<Span*>(std::malloc(sizeof(Span) * (count / 3 + 1)));
    if (trial == nullptr || spans == nullptr) {
        std::free(trial);
        std::free(spans);
        return count;
    }
    usize length = count;
    // Steps first: drop windows of steps, halving the window.
    usize span_count = spans_of(bytes, length, spans, count / 3 + 1);
    usize window = span_count / 2 == 0 ? 1 : span_count / 2;
    while (window >= 1 && span_count > 0) {
        bool removed = false;
        usize first = 0;
        while (first < span_count) {
            const usize last = first + window < span_count ? first + window : span_count;
            const usize cut_start = spans[first].offset;
            const usize cut_end = spans[last - 1].offset + spans[last - 1].size;
            const usize trial_length = length - (cut_end - cut_start);
            std::memcpy(trial, bytes, cut_start);
            std::memcpy(trial + cut_start, bytes + cut_end, length - cut_end);
            if (same_failure(components, component_count, trial, trial_length, options, wanted)) {
                std::memcpy(bytes, trial, trial_length);
                length = trial_length;
                span_count = spans_of(bytes, length, spans, count / 3 + 1);
                removed = true;
                continue;
            }
            first = first + window;
        }
        if (!removed) {
            if (window == 1) break;
            window = window / 2;
        }
    }
    // Then each step's parameters: shorter, then each byte toward zero.
    span_count = spans_of(bytes, length, spans, count / 3 + 1);
    for (usize index = span_count; index > 0; --index) {
        const Span span = spans[index - 1];
        usize parameter_count = span.size - 3;
        while (parameter_count > 0) {
            const usize shorter = parameter_count / 2;
            const usize cut = parameter_count - shorter;
            std::memcpy(trial, bytes, span.offset + 3 + shorter);
            trial[span.offset + 2] = static_cast<u8>(shorter);
            std::memcpy(trial + span.offset + 3 + shorter, bytes + span.offset + 3 + parameter_count,
                        length - (span.offset + 3 + parameter_count));
            if (!same_failure(components, component_count, trial, length - cut, options, wanted)) break;
            std::memcpy(bytes, trial, length - cut);
            length = length - cut;
            parameter_count = shorter;
        }
        for (usize byte = 0; byte < parameter_count; ++byte) {
            const usize at = span.offset + 3 + byte;
            if (bytes[at] == 0) continue;
            const u8 kept = bytes[at];
            bytes[at] = 0;
            if (!same_failure(components, component_count, bytes, length, options, wanted)) bytes[at] = kept;
        }
    }
    std::free(trial);
    std::free(spans);
    return length;
}

// ---- regression tests --------------------------------------------------------------------------

bool write_regression(const char* const path, const Component& component, const u8* const bytes, const usize count,
                      const Transcript& transcript, const Finding& finding) noexcept {
    std::FILE* const file = std::fopen(path, "w");
    if (file == nullptr) return false;
    std::fprintf(file, "// A failure the block engine found in %s (%s), minimised by tests/ic/ic_run.cpp:\n",
                 component.name, component.declaration);
    std::fprintf(file, "// %s.\n", finding.what);
    std::fprintf(file, "// Its steps (slot N is holderN; the last step is the failing one):\n");
    for (usize index = 0; index < transcript.count(); ++index) std::fprintf(file, "//   %s\n", transcript.line(index));
    std::fprintf(file, "// The test replays the bytes through the engine and passes once it finds nothing.\n\n");
    std::fprintf(file, "#include \"ic/engine/ic_engine.hpp\"\n#include \"ic/generated/components.inc\"\n");
    std::fprintf(file, "#include \"test_support.hpp\"\n\n#include <cstdio>\n\nnamespace {\n\n");
    std::fprintf(file, "stxlib::u8 testcase[] = {");
    for (usize index = 0; index < count; ++index) {
        std::fprintf(file, "%s0x%02x", index % 12 == 0 ? "\n    " : " ", bytes[index]);
        if (index + 1 < count) std::fprintf(file, ",");
    }
    std::fprintf(file, "\n};\n\n} // namespace\n\nint main() {\n");
    std::fprintf(file, "    const stxlib::usize count = sizeof(stxlib_ic::generated::components) / "
                       "sizeof(stxlib_ic::generated::components[0]);\n");
    std::fprintf(file, "    const stxlib::usize which = stxlib_ic::index_of(stxlib_ic::generated::components, count, "
                       "\"%s\");\n", component.name);
    std::fprintf(file, "    CHECK(which < count);\n    if (which >= count) return stxlib_tests::finish(\"%s\");\n",
                 component.name);
    std::fprintf(file, "    testcase[0] = static_cast<stxlib::u8>(which);\n");
    std::fprintf(file, "    stxlib_ic::Options options{};\n    options.forks = true;\n");
    std::fprintf(file, "    stxlib_ic::Finding finding{};\n");
    std::fprintf(file, "    const bool found = stxlib_ic::run_case(stxlib_ic::generated::components, count, testcase, "
                       "sizeof(testcase), options, nullptr, finding);\n");
    std::fprintf(file, "    CHECK(!found);\n    if (found) std::fprintf(stderr, \"%%s\\n\", finding.what);\n");
    std::fprintf(file, "    return stxlib_tests::finish(\"ic regression, %s\");\n}\n", component.name);
    const bool written = std::fclose(file) == 0;
    return written;
}

usize index_of(const Component* const* const components, const usize count, const char* const name) noexcept {
    for (usize index = 0; index < count; ++index) {
        if (std::strcmp((*components[index]).name, name) == 0) return index;
    }
    return count;
}

} // namespace stxlib_ic
