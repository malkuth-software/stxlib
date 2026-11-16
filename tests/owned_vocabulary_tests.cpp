// Tests of the owned vocabulary's containers and owners: Vector, Optional,
// Unique, Shared, Weak and SharedFromThis (owner, D15 before 1.0).
//
// What is exhaustive here, and over which domain (proofs/REPORT.md):
// - grown_capacity: every 8-bit (current, wanted, element size) that meets
//   its precondition;
// - Vector: every sequence of up to `depth` operations (default 4) from the
//   empty vector, over the operation set in vector_candidates() with every
//   index and length the state allows, each operation also run once with
//   its allocation refused; the state is compared with a std::vector model,
//   and element lifetimes are counted (no leak, no double destruction, no
//   use of a destroyed element);
// - Optional: every sequence of up to 4 operations over two optionals;
// - Shared and Weak: every sequence of up to 4 operations over three owner
//   slots and two observer slots, against std::shared_ptr and
//   std::weak_ptr, with object destructions and block releases counted.
// Concurrency (shared_threads) is sampled, not proved.

#include "stxlib/boundary.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

using stxlib::u32;
using stxlib::u64;
using stxlib::usize;

// ---- element lifetimes ------------------------------------------------------

struct Ledger final {
    long long live{0};
    long long constructed{0};
    long long destroyed{0};
    long long errors{0};
};

Ledger ledger{};

constexpr unsigned alive_mark = 0x5a5a5a5au;
constexpr int moved_from_value = -1;

// An element that counts its lives and notices use after destruction. The
// plain form has no assignment operators, so the vocabulary must relocate by
// construction; the assignable form lets Vector shift by move assignment
// (its form-1 path for insert and erase under the early-exit rule).
template <bool Assignable>
struct TrackedElement final {
    int value;
    unsigned mark;

    TrackedElement() noexcept : value(0), mark(alive_mark) { born(); }
    explicit TrackedElement(const int initial) noexcept : value(initial), mark(alive_mark) { born(); }
    TrackedElement(const TrackedElement& other) noexcept : value(other.value), mark(alive_mark) {
        if (other.mark != alive_mark) ledger.errors = ledger.errors + 1;
        born();
    }
    TrackedElement(TrackedElement&& other) noexcept : value(other.value), mark(alive_mark) {
        if (other.mark != alive_mark) ledger.errors = ledger.errors + 1;
        other.value = moved_from_value;
        born();
    }
    TrackedElement& operator=(const TrackedElement&) = delete;
    TrackedElement& operator=(TrackedElement&& other) noexcept
        requires(Assignable)
    {
        if (mark != alive_mark || other.mark != alive_mark) ledger.errors = ledger.errors + 1;
        value = other.value;
        other.value = moved_from_value;
        return *this;
    }
    TrackedElement& operator=(TrackedElement&&)
        requires(!Assignable)
    = delete;
    ~TrackedElement() {
        if (mark != alive_mark) ledger.errors = ledger.errors + 1;
        mark = 0;
        ledger.live = ledger.live - 1;
        ledger.destroyed = ledger.destroyed + 1;
    }

    bool operator==(const TrackedElement& other) const noexcept { return value == other.value; }

private:
    static void born() noexcept {
        ledger.live = ledger.live + 1;
        ledger.constructed = ledger.constructed + 1;
    }
};

using Tracked = TrackedElement<false>;
using AssignableTracked = TrackedElement<true>;

// ---- grown_capacity ---------------------------------------------------------

void growth_policy_exhaustive_8bit() {
    long long violations = 0;
    long long cases = 0;
    for (unsigned size = 1; size <= 255; ++size) {
        const unsigned maximum = 255u / size;
        for (unsigned current = 0; current <= maximum; ++current) {
            for (unsigned wanted = 0; wanted <= 255; ++wanted) {
                cases = cases + 1;
                const unsigned char grown = stxlib::detail::grown_capacity<unsigned char>(
                    static_cast<unsigned char>(current), static_cast<unsigned char>(wanted),
                    static_cast<unsigned char>(size));
                const unsigned result = grown;
                bool good = true;
                if (wanted > maximum) {
                    good = result == 0;
                } else {
                    good = result >= wanted && result >= current && result <= maximum;
                    if (wanted <= current) good = good && result == current;
                }
                if (!good) violations = violations + 1;
            }
        }
    }
    CHECK(violations == 0);
    std::printf("grown_capacity: %lld 8-bit cases, %lld violations\n", cases, violations);
}

// ---- Vector: bounded exhaustive exploration --------------------------------

enum class VectorStep : unsigned char {
    push,              // push(Tracked(fresh)) from outside
    push_inside,       // insert(first, move(element[second])): an element of the vector itself
    push_copy_outside, // push_copy(Tracked(fresh))
    push_copy_inside,  // push_copy(element[first])
    emplace,           // emplace(fresh)
    insert,            // insert(first, Tracked(fresh))
    erase,             // erase(first)
    erase_range,       // erase_range(first, second)
    take_last,
    remove_last,
    truncate,      // truncate(first)
    clear,
    release,
    reserve,       // reserve(first)
    resize,        // resize(first)
    append_self,    // append_copies(view().first(first))
    append_outside, // append_copies({fresh, fresh + 1})
    copy_named,     // vector = take(vector.copy()): reports exhaustion
    copy_construct, // vector = Vector(vector): the ordinary copy constructor
    copy_assign,    // other = Vector(vector) over `first` old elements, then vector = other (copy assignment)
    copy_self       // vector = vector (copy assignment to itself)
};

// The ordinary copy steps fail fast when the allocator refuses, so the
// explorer's refused branch leaves them out (copy_named is their reporting
// form and is refused there).
bool fails_fast_when_refused(const VectorStep step) {
    const bool result = step == VectorStep::copy_construct || step == VectorStep::copy_assign;
    return result;
}

struct VectorOperation final {
    VectorStep step;
    usize first;
    usize second;
};

constexpr usize vector_count_cap = 9;

void add(std::vector<VectorOperation>& list, const VectorStep step, const usize first, const usize second) {
    const VectorOperation operation{step, first, second};
    list.push_back(operation);
}

// Every operation the state with `count` elements allows, each index and
// length included. Growth past vector_count_cap is left out so the depth
// bounds the work.
std::vector<VectorOperation> vector_candidates(const usize count) {
    std::vector<VectorOperation> result;
    const bool can_grow = count < vector_count_cap;
    if (can_grow) {
        add(result, VectorStep::push, 0, 0);
        add(result, VectorStep::push_copy_outside, 0, 0);
        add(result, VectorStep::emplace, 0, 0);
        for (usize index = 0; index < count; ++index) add(result, VectorStep::push_copy_inside, index, 0);
        for (usize index = 0; index <= count; ++index) {
            add(result, VectorStep::insert, index, 0);
            for (usize source = 0; source < count; ++source) add(result, VectorStep::push_inside, index, source);
        }
        add(result, VectorStep::append_outside, 0, 0);
    }
    for (usize index = 0; index < count; ++index) add(result, VectorStep::erase, index, 0);
    for (usize first = 0; first <= count; ++first) {
        for (usize length = 0; length <= count - first; ++length) add(result, VectorStep::erase_range, first, length);
    }
    if (count != 0) {
        add(result, VectorStep::take_last, 0, 0);
        add(result, VectorStep::remove_last, 0, 0);
    }
    for (usize smaller = 0; smaller <= count; ++smaller) add(result, VectorStep::truncate, smaller, 0);
    add(result, VectorStep::clear, 0, 0);
    add(result, VectorStep::release, 0, 0);
    const usize reserve_sizes[4] = {0, count, count + 1, 2 * count + 3};
    for (const usize size : reserve_sizes) add(result, VectorStep::reserve, size, 0);
    const usize resize_sizes[3] = {count / 2, count + 1, count + 3};
    for (const usize size : resize_sizes) {
        if (size <= count || size <= vector_count_cap) add(result, VectorStep::resize, size, 0);
    }
    if (count * 2 <= vector_count_cap) {
        for (usize length = 0; length <= count; ++length) add(result, VectorStep::append_self, length, 0);
    }
    add(result, VectorStep::copy_named, 0, 0);
    add(result, VectorStep::copy_construct, 0, 0);
    const usize old_counts[3] = {0, 1, 4};
    for (const usize old : old_counts) add(result, VectorStep::copy_assign, old, 0);
    add(result, VectorStep::copy_self, 0, 0);
    return result;
}

struct VectorTally final {
    long long sequences{0};
    long long operations{0};
    long long refusals_checked{0};
    long long mismatches{0};
};

VectorTally vector_tally{};

template <typename Element>
bool same_as_model(const stxlib::Vector<Element>& vector, const std::vector<int>& model) {
    if (vector.count() != model.size()) return false;
    if (vector.capacity() < vector.count()) return false;
    for (usize index = 0; index < model.size(); ++index) {
        if (vector[index].value != model[index]) return false;
    }
    return true;
}

// Applies `operation` to the vector and, when it succeeds, to the model.
// Returns the status (success for the operations that cannot fail).
template <typename Element>
stxlib::OperationStatus apply_vector(const VectorOperation& operation, stxlib::Vector<Element>& vector, std::vector<int>& model,
                            const int fresh) {
    const usize first = operation.first;
    const usize second = operation.second;
    switch (operation.step) {
    case VectorStep::push: {
        Element argument(fresh);
        const stxlib::OperationStatus status = vector.push(static_cast<Element&&>(argument));
        if (status.succeeded()) {
            model.push_back(fresh);
            if (argument.value != moved_from_value) vector_tally.mismatches = vector_tally.mismatches + 1;
        } else if (argument.value != fresh) {
            vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        return status;
    }
    case VectorStep::push_inside: {
        const stxlib::OperationStatus status = vector.insert(first, static_cast<Element&&>(vector[second]));
        if (status.succeeded()) {
            const int moved = model[second];
            model[second] = moved_from_value;
            model.insert(model.begin() + static_cast<long>(first), moved);
        }
        return status;
    }
    case VectorStep::push_copy_outside: {
        const Element argument(fresh);
        const stxlib::OperationStatus status = vector.push_copy(argument);
        if (status.succeeded()) model.push_back(fresh);
        return status;
    }
    case VectorStep::push_copy_inside: {
        const stxlib::OperationStatus status = vector.push_copy(vector[first]);
        if (status.succeeded()) {
            const int copied = model[first];
            model.push_back(copied);
        }
        return status;
    }
    case VectorStep::emplace: {
        const stxlib::OperationStatus status = vector.emplace(fresh);
        if (status.succeeded()) model.push_back(fresh);
        return status;
    }
    case VectorStep::insert: {
        Element argument(fresh);
        const stxlib::OperationStatus status = vector.insert(first, static_cast<Element&&>(argument));
        if (status.succeeded()) {
            model.insert(model.begin() + static_cast<long>(first), fresh);
        } else if (argument.value != fresh) {
            vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        return status;
    }
    case VectorStep::erase:
        vector.erase(first);
        model.erase(model.begin() + static_cast<long>(first));
        return stxlib::OperationStatus::success();
    case VectorStep::erase_range:
        vector.erase_range(first, second);
        model.erase(model.begin() + static_cast<long>(first), model.begin() + static_cast<long>(first + second));
        return stxlib::OperationStatus::success();
    case VectorStep::take_last: {
        const Element taken = vector.take_last();
        if (taken.value != model.back()) vector_tally.mismatches = vector_tally.mismatches + 1;
        model.pop_back();
        return stxlib::OperationStatus::success();
    }
    case VectorStep::remove_last:
        vector.remove_last();
        model.pop_back();
        return stxlib::OperationStatus::success();
    case VectorStep::truncate:
        vector.truncate(first);
        model.resize(first);
        return stxlib::OperationStatus::success();
    case VectorStep::clear:
        vector.clear();
        model.clear();
        return stxlib::OperationStatus::success();
    case VectorStep::release:
        vector.release();
        model.clear();
        if (vector.capacity() != 0) vector_tally.mismatches = vector_tally.mismatches + 1;
        return stxlib::OperationStatus::success();
    case VectorStep::reserve: {
        const stxlib::OperationStatus status = vector.reserve(first);
        if (status.succeeded() && vector.capacity() < first) vector_tally.mismatches = vector_tally.mismatches + 1;
        return status;
    }
    case VectorStep::resize: {
        const stxlib::OperationStatus status = vector.resize(first);
        if (status.succeeded()) model.resize(first, 0);
        return status;
    }
    case VectorStep::append_self: {
        const stxlib::OperationStatus status = vector.append_copies(vector.view().first(first));
        if (status.succeeded()) {
            for (usize index = 0; index < first; ++index) {
                const int copied = model[index];
                model.push_back(copied);
            }
        }
        return status;
    }
    case VectorStep::copy_named: {
        stxlib::OperationResult<stxlib::Vector<Element>> copied = vector.copy();
        if (copied.failed()) return stxlib::OperationStatus::failure(*copied.error());
        const stxlib::Vector<Element>& made = *copied.value();
        if (made.capacity() != made.count() || !same_as_model(made, model)) {
            vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        vector = static_cast<stxlib::OperationResult<stxlib::Vector<Element>>&&>(copied).take_value();
        return stxlib::OperationStatus::success();
    }
    case VectorStep::copy_construct: {
        stxlib::Vector<Element> made(vector);
        if (made.capacity() != made.count() || !same_as_model(made, model) || !same_as_model(vector, model)) {
            vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        vector = static_cast<stxlib::Vector<Element>&&>(made);
        return stxlib::OperationStatus::success();
    }
    case VectorStep::copy_assign: {
        stxlib::Vector<Element> other;
        for (usize index = 0; index < first; ++index) {
            if (other.emplace(fresh).failed()) vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        other = vector;
        if (!same_as_model(other, model) || !same_as_model(vector, model)) {
            vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        vector = other;
        return stxlib::OperationStatus::success();
    }
    case VectorStep::copy_self: {
        const usize capacity = vector.capacity();
        stxlib::Vector<Element>& alias = vector;
        vector = alias;
        if (vector.capacity() != capacity) vector_tally.mismatches = vector_tally.mismatches + 1;
        return stxlib::OperationStatus::success();
    }
    case VectorStep::append_outside: {
        const Element outside[2] = {Element(fresh), Element(fresh + 1)};
        const stxlib::View<Element> source(outside);
        const stxlib::OperationStatus status = vector.append_copies(source);
        if (status.succeeded()) {
            model.push_back(fresh);
            model.push_back(fresh + 1);
        }
        return status;
    }
    }
    vector_tally.mismatches = vector_tally.mismatches + 1;
    return stxlib::OperationStatus::success();
}

template <typename Element>
void replay(const std::vector<VectorOperation>& path, stxlib::Vector<Element>& vector, std::vector<int>& model) {
    for (usize index = 0; index < path.size(); ++index) {
        const stxlib::OperationStatus status = apply_vector<Element>(path[index], vector, model, static_cast<int>(100 + index));
        if (status.failed()) vector_tally.mismatches = vector_tally.mismatches + 1;
    }
}

template <typename Element>
void explore_vector(std::vector<VectorOperation>& path, const usize depth) {
    std::vector<int> start_model;
    usize start_count = 0;
    {
        stxlib::Vector<Element> vector;
        replay<Element>(path, vector, start_model);
        start_count = vector.count();
    }
    const std::vector<VectorOperation> candidates = vector_candidates(start_count);
    const int fresh = static_cast<int>(100 + path.size());
    for (const VectorOperation& candidate : candidates) {
        // Refused: the operation's allocation (if it makes one) fails, and
        // the vector must be exactly as before.
        if (!fails_fast_when_refused(candidate.step)) {
            stxlib::Vector<Element> vector;
            std::vector<int> model;
            replay<Element>(path, vector, model);
            const usize capacity_before = vector.capacity();
            stxlib::detail::arm_vocabulary_refusals(0);
            const stxlib::OperationStatus status = apply_vector<Element>(candidate, vector, model, fresh);
            stxlib::detail::disarm_vocabulary_refusals();
            if (status.failed()) {
                vector_tally.refusals_checked = vector_tally.refusals_checked + 1;
                const bool kind_ok = (*status.error()).kind() == stxlib::ErrorKind::out_of_memory;
                const bool unchanged = same_as_model(vector, model) && vector.capacity() == capacity_before;
                if (!kind_ok || !unchanged) vector_tally.mismatches = vector_tally.mismatches + 1;
            }
        }
        // Granted: the result must equal the model's.
        path.push_back(candidate);
        {
            stxlib::Vector<Element> vector;
            std::vector<int> model;
            replay<Element>(path, vector, model);
            vector_tally.operations = vector_tally.operations + static_cast<long long>(path.size());
            if (!same_as_model(vector, model)) vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        vector_tally.sequences = vector_tally.sequences + 1;
        if (path.size() < depth) explore_vector<Element>(path, depth);
        path.pop_back();
    }
}

void vector_exhaustive(const usize depth) {
    const Ledger before = ledger;
    std::vector<VectorOperation> path;
    explore_vector<Tracked>(path, depth);
    explore_vector<AssignableTracked>(path, depth);
    CHECK(vector_tally.mismatches == 0);
    CHECK(vector_tally.refusals_checked > 0);
    CHECK(ledger.live == before.live);
    CHECK(ledger.errors == 0);
    std::printf("vector: depth %zu, %lld sequences, %lld operations, %lld refusals checked, %lld mismatches\n", depth,
                vector_tally.sequences, vector_tally.operations, vector_tally.refusals_checked,
                vector_tally.mismatches);
}

void vector_limits_and_moves() {
    stxlib::OperationResult<stxlib::Vector<u64>> huge = stxlib::Vector<u64>::with_capacity(stxlib::usize_max);
    CHECK(huge.failed() && (*huge.error()).kind() == stxlib::ErrorKind::limit_exceeded);

    stxlib::OperationResult<stxlib::Vector<u64>> made = stxlib::Vector<u64>::with_capacity(3);
    CHECK(made.succeeded());
    stxlib::Vector<u64> numbers = static_cast<stxlib::OperationResult<stxlib::Vector<u64>>&&>(made).take_value();
    CHECK(numbers.capacity() >= 3 && numbers.empty());
    for (u64 value = 0; value < 40; ++value) CHECK(numbers.push_copy(value).succeeded());
    u64 sum = 0;
    for (const u64 value : numbers) sum = sum + value;
    CHECK(sum == 780);

    stxlib::Vector<u64> moved(static_cast<stxlib::Vector<u64>&&>(numbers));
    CHECK(numbers.empty() && numbers.capacity() == 0);
    CHECK(moved.count() == 40 && moved[39] == 39);

    stxlib::OperationResult<stxlib::Vector<u64>> copied = moved.copy();
    CHECK(copied.succeeded());
    CHECK(*copied.value() == moved);
    (*copied.value())[0] = 7;
    CHECK(*copied.value() != moved);

    stxlib::detail::arm_vocabulary_refusals(0);
    stxlib::OperationResult<stxlib::Vector<u64>> refused_copy = moved.copy();
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused_copy.failed() && (*refused_copy.error()).kind() == stxlib::ErrorKind::out_of_memory);

    const u64 granted_before = stxlib::detail::vocabulary_allocations_granted();
    const u64 released_before = stxlib::detail::vocabulary_blocks_released();
    {
        stxlib::Vector<u64> scoped;
        CHECK(scoped.resize(100).succeeded());
        CHECK(scoped[99] == 0);
    }
    const u64 granted = stxlib::detail::vocabulary_allocations_granted() - granted_before;
    const u64 released = stxlib::detail::vocabulary_blocks_released() - released_before;
    CHECK(granted == released && granted >= 1);

    stxlib::Vector<double> reals;
    CHECK(reals.push_copy(0.0).succeeded());
    stxlib::Vector<double> negative;
    CHECK(negative.push_copy(-0.0).succeeded());
    CHECK(reals == negative);
}

// ---- Optional ---------------------------------------------------------------

static_assert(__is_trivially_copyable(stxlib::Optional<double>), "Optional of a trivially copyable value is too");
static_assert(sizeof(stxlib::Optional<double>) == 16, "a presence byte, padding, then the double");
static_assert(!__is_trivially_copyable(stxlib::Optional<Tracked>), "Optional of a tracked value is not");

enum class OptionalStep : unsigned char { set, reset, copy_from_other, move_from_other, take, swap_through_copy };

void apply_optional(const OptionalStep step, const int fresh, stxlib::Optional<Tracked>& left,
                    stxlib::Optional<Tracked>& right, std::optional<int>& model_left, std::optional<int>& model_right,
                    long long& mismatches) {
    switch (step) {
    case OptionalStep::set:
        left.set(Tracked(fresh));
        model_left = fresh;
        return;
    case OptionalStep::reset:
        left.reset();
        model_left.reset();
        return;
    case OptionalStep::copy_from_other:
        left = right;
        model_left = model_right;
        return;
    case OptionalStep::move_from_other:
        left = static_cast<stxlib::Optional<Tracked>&&>(right);
        model_left = model_right;
        if (model_right.has_value()) model_right = moved_from_value;
        return;
    case OptionalStep::take:
        if (!left.present()) return;
        {
            const Tracked taken = left.take();
            if (taken.value != *model_left) mismatches = mismatches + 1;
        }
        model_left.reset();
        return;
    case OptionalStep::swap_through_copy: {
        const stxlib::Optional<Tracked> held(left);
        left = right;
        right = held;
        const std::optional<int> model_held = model_left;
        model_left = model_right;
        model_right = model_held;
        return;
    }
    }
    mismatches = mismatches + 1;
}

bool optional_matches(const stxlib::Optional<Tracked>& optional, const std::optional<int>& model) {
    if (optional.present() != model.has_value()) return false;
    if (!model.has_value()) return optional.value() == nullptr;
    const bool result = (*optional.value()).value == *model;
    return result;
}

void optional_exhaustive() {
    const OptionalStep steps[6] = {OptionalStep::set,  OptionalStep::reset, OptionalStep::copy_from_other,
                                   OptionalStep::move_from_other, OptionalStep::take, OptionalStep::swap_through_copy};
    const long long live_before = ledger.live;
    long long mismatches = 0;
    long long sequences = 0;
    // Each sequence is a base-12 number: 6 steps times 2 targets per place.
    const unsigned places = 4;
    unsigned total = 1;
    for (unsigned place = 0; place < places; ++place) total = total * 12;
    for (unsigned code = 0; code < total; ++code) {
        stxlib::Optional<Tracked> first;
        stxlib::Optional<Tracked> second = stxlib::Optional<Tracked>::of(Tracked(50));
        std::optional<int> model_first;
        std::optional<int> model_second = 50;
        unsigned rest = code;
        for (unsigned place = 0; place < places; ++place) {
            const unsigned digit = rest % 12;
            rest = rest / 12;
            const OptionalStep step = steps[digit % 6];
            const int fresh = static_cast<int>(place + 1);
            if (digit < 6) {
                apply_optional(step, fresh, first, second, model_first, model_second, mismatches);
            } else {
                apply_optional(step, fresh, second, first, model_second, model_first, mismatches);
            }
            if (!optional_matches(first, model_first) || !optional_matches(second, model_second)) {
                mismatches = mismatches + 1;
            }
            const bool equal = first == second;
            if (equal != (model_first == model_second)) mismatches = mismatches + 1;
        }
        sequences = sequences + 1;
    }
    CHECK(mismatches == 0);
    CHECK(ledger.live == live_before);
    CHECK(ledger.errors == 0);
    std::printf("optional: %lld sequences of 4, %lld mismatches\n", sequences, mismatches);

    const stxlib::Optional<double> nothing;
    CHECK(nothing.absent() && nothing.value_or(2.5) == 2.5);
    const stxlib::Optional<double> some = stxlib::Optional<double>::of(1.5);
    CHECK(some.value_or(2.5) == 1.5);
    const stxlib::Optional<double> nan_value = stxlib::Optional<double>::of(__builtin_nan(""));
    CHECK(nan_value != nan_value);
}

// ---- Unique -----------------------------------------------------------------

struct Left {
    long long left_value{1};
};
struct Right {
    long long right_value{2};
    ~Right() = default;
};
// Right is at a non-zero offset inside Joined, and has no virtual
// destructor: Unique<Right> must still destroy a Joined and release the
// block at its start.
struct Joined final : Left, Right {
    Tracked tracked;
    explicit Joined(const int value) noexcept : tracked(value) {}
};

void unique_owners() {
    const long long live_before = ledger.live;
    const u64 released_before = stxlib::detail::vocabulary_blocks_released();
    {
        stxlib::OperationResult<stxlib::Unique<Joined>> made = stxlib::Unique<Joined>::make(9);
        CHECK(made.succeeded());
        stxlib::Unique<Joined> joined = static_cast<stxlib::OperationResult<stxlib::Unique<Joined>>&&>(made).take_value();
        CHECK(joined.present() && (*joined).tracked.value == 9);
        Joined* const address = joined.get();
        stxlib::Unique<Right> right(static_cast<stxlib::Unique<Joined>&&>(joined));
        CHECK(joined.empty());
        CHECK(right.get() == static_cast<Right*>(address));
        CHECK(static_cast<void*>(right.get()) != static_cast<void*>(address));
        CHECK((*right).right_value == 2);
        CHECK(ledger.live == live_before + 1);
    }
    CHECK(ledger.live == live_before);
    CHECK(stxlib::detail::vocabulary_blocks_released() - released_before == 1);

    const long long constructed_before = ledger.constructed;
    stxlib::detail::arm_vocabulary_refusals(0);
    stxlib::OperationResult<stxlib::Unique<Tracked>> refused = stxlib::Unique<Tracked>::make(3);
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::out_of_memory);
    CHECK(ledger.constructed == constructed_before);
}

// ---- assignment from inside the owned object (security audit 2026-11-15) ----

// A singly linked list of Unique nodes: popping the front is
// `head = move((*head).next)`, whose source lives inside the object head
// owns (A-stxlib-include-01-F1).
struct ListNode final {
    Tracked tracked;
    stxlib::Unique<ListNode> next;
    explicit ListNode(const int value) noexcept : tracked(value), next() {}
};

stxlib::Unique<ListNode> list_node(const int value) {
    stxlib::OperationResult<stxlib::Unique<ListNode>> made = stxlib::Unique<ListNode>::make(value);
    STXLIB_REQUIRE(made.succeeded(), "making a list node");
    stxlib::Unique<ListNode> result = static_cast<stxlib::OperationResult<stxlib::Unique<ListNode>>&&>(made).take_value();
    return result;
}

void unique_list_pop() {
    const long long live_before = ledger.live;
    const long long errors_before = ledger.errors;
    const u64 released_before = stxlib::detail::vocabulary_blocks_released();
    {
        stxlib::Unique<ListNode> head = list_node(1);
        (*head).next = list_node(2);
        (*(*head).next).next = list_node(3);
        CHECK(ledger.live == live_before + 3);

        head = static_cast<stxlib::Unique<ListNode>&&>((*head).next);
        // Only the old head is gone; the tail is kept, in order.
        CHECK(ledger.live == live_before + 2);
        CHECK(stxlib::detail::vocabulary_blocks_released() - released_before == 1);
        CHECK(head.present() && (*head).tracked.value == 2);
        CHECK((*head).next.present() && (*(*head).next).tracked.value == 3);
        CHECK((*(*head).next).next.empty());

        head = static_cast<stxlib::Unique<ListNode>&&>((*head).next);
        CHECK(ledger.live == live_before + 1);
        CHECK(head.present() && (*head).tracked.value == 3 && (*head).next.empty());

        // Popping the last node leaves the list empty.
        head = static_cast<stxlib::Unique<ListNode>&&>((*head).next);
        CHECK(head.empty());
        CHECK(ledger.live == live_before);
    }
    CHECK(stxlib::detail::vocabulary_blocks_released() - released_before == 3);
    CHECK(ledger.errors == errors_before);
}

// An element that owns a separate Vector or Optional of its own type through
// a pointer (the vocabulary's types need a complete element, so the nesting
// goes through the heap), destroyed with the element: assigning the outer
// container from the inner one has its source inside an element the
// assignment destroys (A-stxlib-include-01-F2).
struct VectorBox final {
    Tracked tracked;
    stxlib::Vector<VectorBox>* inner{nullptr};
    explicit VectorBox(const int value) noexcept : tracked(value) {}
    VectorBox(VectorBox&& other) noexcept : tracked(static_cast<Tracked&&>(other.tracked)), inner(other.inner) {
        other.inner = nullptr;
    }
    VectorBox(const VectorBox&) = delete;
    VectorBox& operator=(const VectorBox&) = delete;
    VectorBox& operator=(VectorBox&&) = delete;
    ~VectorBox();
};

VectorBox::~VectorBox() { delete inner; }

struct OptionalBox final {
    Tracked tracked;
    stxlib::Optional<OptionalBox>* inner{nullptr};
    explicit OptionalBox(const int value) noexcept : tracked(value) {}
    OptionalBox(OptionalBox&& other) noexcept : tracked(static_cast<Tracked&&>(other.tracked)), inner(other.inner) {
        other.inner = nullptr;
    }
    OptionalBox(const OptionalBox&) = delete;
    OptionalBox& operator=(const OptionalBox&) = delete;
    OptionalBox& operator=(OptionalBox&&) = delete;
    ~OptionalBox();
};

OptionalBox::~OptionalBox() { delete inner; }

void vector_from_inside_an_element() {
    const long long live_before = ledger.live;
    const long long errors_before = ledger.errors;
    {
        stxlib::Vector<VectorBox> outer;
        CHECK(outer.push(VectorBox(1)).succeeded());
        stxlib::Vector<VectorBox>* const inner = new stxlib::Vector<VectorBox>();
        CHECK((*inner).push(VectorBox(10)).succeeded());
        CHECK((*inner).push(VectorBox(11)).succeeded());
        outer[0].inner = inner;
        // Collapse a level: the outer vector takes the inner one's elements;
        // destroying the old element 1 then deletes the emptied inner vector.
        outer = static_cast<stxlib::Vector<VectorBox>&&>(*inner);
        CHECK(outer.count() == 2 && outer[0].tracked.value == 10 && outer[1].tracked.value == 11);
        CHECK(ledger.live == live_before + 2);
    }
    CHECK(ledger.live == live_before);
    CHECK(ledger.errors == errors_before);
}

void optional_from_inside_its_value() {
    const long long live_before = ledger.live;
    const long long errors_before = ledger.errors;
    {
        stxlib::Optional<OptionalBox> outer = stxlib::Optional<OptionalBox>::of(OptionalBox(1));
        stxlib::Optional<OptionalBox>* const inner =
            new stxlib::Optional<OptionalBox>(stxlib::Optional<OptionalBox>::of(OptionalBox(7)));
        (*outer.value()).inner = inner;
        outer = static_cast<stxlib::Optional<OptionalBox>&&>(*inner);
        CHECK(outer.present() && (*outer.value()).tracked.value == 7 && (*outer.value()).inner == nullptr);
        CHECK(ledger.live == live_before + 1);
    }
    CHECK(ledger.live == live_before);
    CHECK(ledger.errors == errors_before);
}

// emplace with an argument that refers into the vector, when it must grow
// (A-stxlib-include-01-F3), and copies with exactly their count as capacity
// (A-stxlib-include-01-F4).
void vector_emplace_and_exact_copies() {
    const long long live_before = ledger.live;
    const long long errors_before = ledger.errors;
    {
        stxlib::Vector<Tracked> tracked;
        CHECK(tracked.emplace(5).succeeded());
        while (tracked.count() < tracked.capacity()) CHECK(tracked.emplace(6).succeeded());
        const usize full = tracked.count();
        CHECK(full == tracked.capacity());
        const Tracked& first = tracked[0];
        CHECK(tracked.emplace(first).succeeded());
        CHECK(tracked.count() == full + 1 && tracked[full].value == 5 && tracked[0].value == 5);
        CHECK(tracked.capacity() > full);
    }
    CHECK(ledger.live == live_before);
    CHECK(ledger.errors == errors_before);

    stxlib::Vector<u64> numbers;
    for (u64 value = 0; value < 5; ++value) CHECK(numbers.push_copy(value).succeeded());
    CHECK(numbers.capacity() == 8);
    stxlib::OperationResult<stxlib::Vector<u64>> five = numbers.copy();
    CHECK(five.succeeded() && (*five.value()).capacity() == 5 && *five.value() == numbers);
    stxlib::OperationResult<stxlib::Vector<u64>> one = stxlib::Vector<u64>::copy_of(numbers.view().first(1));
    CHECK(one.succeeded() && (*one.value()).capacity() == 1 && (*one.value()).count() == 1);
    stxlib::OperationResult<stxlib::Vector<u64>> none = stxlib::Vector<u64>::copy_of(stxlib::View<u64>());
    CHECK(none.succeeded() && (*none.value()).capacity() == 0 && (*none.value()).empty());
}

// ---- element types incomplete where the container is named (D15) -----------

// A tree whose node holds a Vector of its own type, named while TreeNode is
// still incomplete: Vector asks nothing about its element at class scope.
struct TreeNode final {
    Tracked tracked;
    stxlib::Vector<TreeNode> children;
    explicit TreeNode(const int value) noexcept : tracked(value), children() {}
};

static_assert(__is_nothrow_constructible(TreeNode, TreeNode&&), "a tree node relocates without failing");

void vector_of_its_own_node() {
    const long long live_before = ledger.live;
    const long long errors_before = ledger.errors;
    {
        TreeNode root(1);
        CHECK(root.children.push(TreeNode(2)).succeeded());
        CHECK(root.children.push(TreeNode(3)).succeeded());
        CHECK(root.children[0].children.push(TreeNode(4)).succeeded());
        CHECK(root.children[0].children.push(TreeNode(5)).succeeded());
        CHECK(ledger.live == live_before + 5);
        // Collapse a level: the root's children become its first child's
        // children (the source lives inside an element the assignment
        // destroys; A-stxlib-include-01-F2).
        root.children = static_cast<stxlib::Vector<TreeNode>&&>(root.children[0].children);
        CHECK(root.children.count() == 2 && root.children[0].tracked.value == 4 &&
              root.children[1].tracked.value == 5);
        CHECK(ledger.live == live_before + 3);
    }
    CHECK(ledger.live == live_before);
    CHECK(ledger.errors == errors_before);
}

// ---- ordinary copies (owner, 2026-11-16) ------------------------------------

static_assert(__is_nothrow_constructible(stxlib::Utf8String, const stxlib::Utf8String&), "Utf8String copies");
static_assert(__is_nothrow_assignable(stxlib::Utf8String&, const stxlib::Utf8String&), "Utf8String copy-assigns");
static_assert(__is_nothrow_assignable(stxlib::Optional<stxlib::Utf8String>&, const stxlib::Optional<stxlib::Utf8String>&),
              "Optional<Utf8String> copy-assigns (Optional's rule unchanged)");
static_assert(__is_nothrow_constructible(stxlib::Optional<stxlib::Utf8String>, const stxlib::Optional<stxlib::Utf8String>&),
              "Optional<Utf8String> copy-constructs");
static_assert(__is_nothrow_constructible(stxlib::Vector<stxlib::Utf8String>, const stxlib::Vector<stxlib::Utf8String>&),
              "Vector<Utf8String> copies");
static_assert(__is_nothrow_constructible(TreeNode, const TreeNode&), "a tree node holding a Vector of itself copies");

// Text of `length` bytes, distinct per seed: "a" + seed letter repeated.
stxlib::Utf8String text_of(const usize length, const char seed) {
    char bytes[64];
    for (usize index = 0; index < length; ++index) bytes[index] = static_cast<char>(seed + static_cast<char>(index % 7));
    stxlib::OperationResult<stxlib::TextView> view = stxlib::validate_utf8(bytes, length);
    stxlib::Utf8String result = stxlib::take_or_fail_fast(stxlib::Utf8String::copy_of(*view.value()));
    return result;
}

void ordinary_copies() {
    // Utf8String: every length 0 to 40 (inline up to 15 bytes, then a
    // block), copy-constructed and copy-assigned over every length 0 to 40.
    long long string_cases = 0;
    long long string_mismatches = 0;
    for (usize length = 0; length <= 40; ++length) {
        const stxlib::Utf8String original = text_of(length, 'a');
        stxlib::Utf8String copied(original);
        string_cases = string_cases + 1;
        if (copied != original || copied.count() != length || (length > 0 && copied.view().data() == original.view().data())) {
            string_mismatches = string_mismatches + 1;
        }
        // Independent storage: changing the copy leaves the original.
        if (copied.append(stxlib::TextView("z")).failed() || original.count() != length) string_mismatches = string_mismatches + 1;
        for (usize old = 0; old <= 40; ++old) {
            stxlib::Utf8String target = text_of(old, 'k');
            target = original;
            string_cases = string_cases + 1;
            if (target != original) string_mismatches = string_mismatches + 1;
        }
        stxlib::Utf8String self = text_of(length, 'q');
        const stxlib::Utf8String expected = text_of(length, 'q');
        stxlib::Utf8String& alias = self;
        self = alias;
        if (self != expected) string_mismatches = string_mismatches + 1;
    }
    CHECK(string_mismatches == 0);
    std::printf("ordinary copies: Utf8String %lld cases, %lld mismatches\n", string_cases, string_mismatches);

    // Vector<Utf8String>: copy, and an element assigned from another
    // element of the same vector.
    stxlib::Vector<stxlib::Utf8String> names;
    for (usize length = 0; length <= 20; length = length + 5) CHECK(names.push(text_of(length, 'm')).succeeded());
    stxlib::Vector<stxlib::Utf8String> names_copy(names);
    CHECK(names_copy == names && names_copy.capacity() == names.count());
    names[0] = names[4];
    CHECK(names[0] == names[4] && names[0].count() == 20);
    names = names_copy;
    CHECK(names == names_copy);

    // A tree: copy a node holding a Vector of itself, and assign the
    // children from a grandchild's children (the source lives inside an
    // element the assignment destroys).
    const long long live_before = ledger.live;
    const long long errors_before = ledger.errors;
    {
        TreeNode root(1);
        CHECK(root.children.push(TreeNode(2)).succeeded());
        CHECK(root.children[0].children.push(TreeNode(4)).succeeded());
        CHECK(root.children[0].children.push(TreeNode(5)).succeeded());
        const TreeNode duplicate(root);
        CHECK(duplicate.children.count() == 1 && duplicate.children[0].children.count() == 2 &&
              duplicate.children[0].children[1].tracked.value == 5);
        root.children = root.children[0].children;
        CHECK(root.children.count() == 2 && root.children[0].tracked.value == 4 && root.children[1].tracked.value == 5);
        CHECK(ledger.live == live_before + 3 + 4);
    }
    CHECK(ledger.live == live_before);
    CHECK(ledger.errors == errors_before);

    // Optional<Utf8String>.
    const stxlib::Optional<stxlib::Utf8String> some = stxlib::Optional<stxlib::Utf8String>::of(text_of(30, 'r'));
    stxlib::Optional<stxlib::Utf8String> other = some;
    CHECK(other.present() && *other.value() == *some.value());
    other = stxlib::Optional<stxlib::Utf8String>::none();
    other = some;
    CHECK(other.present() && *other.value() == *some.value());

    // Buffer, Path, PriorityQueue.
    stxlib::OperationResult<stxlib::Buffer<u32>> made = stxlib::Buffer<u32>::allocate_zeroed(9);
    CHECK(made.succeeded());
    stxlib::Buffer<u32> buffer = static_cast<stxlib::OperationResult<stxlib::Buffer<u32>>&&>(made).take_value();
    for (usize index = 0; index < buffer.count(); ++index) buffer[index] = static_cast<u32>(index * 3);
    const stxlib::Buffer<u32> buffer_copy(buffer);
    CHECK(buffer_copy.count() == 9 && buffer_copy[8] == 24 && buffer_copy.view().data() != buffer.view().data());
    stxlib::Buffer<u32> empty;
    empty = buffer_copy;
    CHECK(empty.count() == 9 && empty[4] == 12);

    stxlib::OperationResult<stxlib::Path> parsed = stxlib::Path::parse(stxlib::TextView("alpha/beta/gamma.txt"));
    CHECK(parsed.succeeded());
    const stxlib::Path path = static_cast<stxlib::OperationResult<stxlib::Path>&&>(parsed).take_value();
    stxlib::Path path_copy(path);
    CHECK(path_copy.text() == path.text());
    path_copy = path;
    CHECK(path_copy.text() == path.text());

    stxlib::PriorityQueue<int, stxlib::LargestFirst<int>> queue;
    const int pushed[6] = {4, 9, 1, 9, 7, 3};
    for (const int value : pushed) CHECK(queue.push(int{value}).succeeded());
    stxlib::PriorityQueue<int, stxlib::LargestFirst<int>> queue_copy(queue);
    bool same_order = queue_copy.count() == queue.count();
    while (!queue.empty()) {
        const stxlib::Optional<int> a = queue.pop();
        const stxlib::Optional<int> b = queue_copy.pop();
        same_order = same_order && b.present() && *a.value() == *b.value();
    }
    CHECK(same_order && queue_copy.empty());
}

// A node that owns its successor through Shared and observes its
// predecessor through Weak, both named while SharedNode is incomplete.
struct SharedNode final {
    Tracked tracked;
    stxlib::Shared<SharedNode> next;
    stxlib::Weak<SharedNode> previous;
    explicit SharedNode(const int value) noexcept : tracked(value), next(), previous() {}
};

stxlib::Shared<SharedNode> shared_node(const int value) {
    stxlib::OperationResult<stxlib::Shared<SharedNode>> made = stxlib::Shared<SharedNode>::make(value);
    STXLIB_REQUIRE(made.succeeded(), "making a shared node");
    stxlib::Shared<SharedNode> result = static_cast<stxlib::OperationResult<stxlib::Shared<SharedNode>>&&>(made).take_value();
    return result;
}

void shared_of_its_own_node() {
    const long long live_before = ledger.live;
    const long long errors_before = ledger.errors;
    {
        stxlib::Shared<SharedNode> head = shared_node(1);
        {
            const stxlib::Shared<SharedNode> second = shared_node(2);
            (*head).next = second;
            (*second).previous = stxlib::Weak<SharedNode>(head);
        }
        CHECK(ledger.live == live_before + 2);
        CHECK((*head).next.present() && (*(*head).next).tracked.value == 2);
        CHECK((*(*head).next).previous.lock() == head);

        // Advance the head along its own link (copy from inside the object
        // head owns): the old head goes, the second node stays, and its
        // backward link now finds nothing.
        head = (*head).next;
        CHECK(ledger.live == live_before + 1);
        CHECK(head.present() && (*head).tracked.value == 2);
        CHECK((*head).previous.expired());
        CHECK((*head).previous.lock().empty());
    }
    CHECK(ledger.live == live_before);
    CHECK(ledger.errors == errors_before);
}

// ---- Shared and Weak: bounded exhaustive exploration ------------------------

constexpr unsigned owner_slots = 3;
constexpr unsigned observer_slots = 2;

struct SharedWorld final {
    stxlib::Shared<Tracked> owners[owner_slots];
    stxlib::Weak<Tracked> observers[observer_slots];
    std::shared_ptr<int> model_owners[owner_slots];
    std::weak_ptr<int> model_observers[observer_slots];
};

enum class SharedStep : unsigned char { make, copy, move, reset, observe, lock, forget };

struct SharedOperation final {
    SharedStep step;
    unsigned first;
    unsigned second;
};

std::vector<SharedOperation> shared_candidates() {
    std::vector<SharedOperation> result;
    for (unsigned slot = 0; slot < owner_slots; ++slot) {
        result.push_back(SharedOperation{SharedStep::make, slot, 0});
        result.push_back(SharedOperation{SharedStep::reset, slot, 0});
        for (unsigned other = 0; other < owner_slots; ++other) {
            result.push_back(SharedOperation{SharedStep::copy, slot, other});
            if (other != slot) result.push_back(SharedOperation{SharedStep::move, slot, other});
        }
        for (unsigned observer = 0; observer < observer_slots; ++observer) {
            result.push_back(SharedOperation{SharedStep::observe, observer, slot});
            result.push_back(SharedOperation{SharedStep::lock, slot, observer});
        }
    }
    for (unsigned observer = 0; observer < observer_slots; ++observer) {
        result.push_back(SharedOperation{SharedStep::forget, observer, 0});
    }
    return result;
}

void apply_shared(const SharedOperation& operation, SharedWorld& world, const int fresh, long long& mismatches) {
    const unsigned first = operation.first;
    const unsigned second = operation.second;
    switch (operation.step) {
    case SharedStep::make: {
        stxlib::OperationResult<stxlib::Shared<Tracked>> made = stxlib::Shared<Tracked>::make(fresh);
        if (made.failed()) {
            mismatches = mismatches + 1;
            return;
        }
        world.owners[first] = static_cast<stxlib::OperationResult<stxlib::Shared<Tracked>>&&>(made).take_value();
        world.model_owners[first] = std::make_shared<int>(fresh);
        return;
    }
    case SharedStep::copy:
        world.owners[first] = world.owners[second];
        world.model_owners[first] = world.model_owners[second];
        return;
    case SharedStep::move:
        world.owners[first] = static_cast<stxlib::Shared<Tracked>&&>(world.owners[second]);
        world.model_owners[first] = std::move(world.model_owners[second]);
        return;
    case SharedStep::reset:
        world.owners[first].reset();
        world.model_owners[first].reset();
        return;
    case SharedStep::observe:
        world.observers[first] = stxlib::Weak<Tracked>(world.owners[second]);
        world.model_observers[first] = world.model_owners[second];
        return;
    case SharedStep::lock:
        world.owners[first] = world.observers[second].lock();
        world.model_owners[first] = world.model_observers[second].lock();
        return;
    case SharedStep::forget:
        world.observers[first].reset();
        world.model_observers[first].reset();
        return;
    }
    mismatches = mismatches + 1;
}

bool shared_matches(const SharedWorld& world) {
    for (unsigned slot = 0; slot < owner_slots; ++slot) {
        const stxlib::Shared<Tracked>& owner = world.owners[slot];
        const std::shared_ptr<int>& model = world.model_owners[slot];
        if (owner.present() != (model != nullptr)) return false;
        if (model == nullptr) continue;
        if ((*owner).value != *model) return false;
        if (owner.owner_count() != static_cast<u64>(model.use_count())) return false;
        for (unsigned other = 0; other < owner_slots; ++other) {
            const bool same = owner == world.owners[other];
            const bool model_same = model == world.model_owners[other];
            if (same != model_same) return false;
        }
    }
    for (unsigned observer = 0; observer < observer_slots; ++observer) {
        if (world.observers[observer].expired() != world.model_observers[observer].expired()) return false;
    }
    return true;
}

void explore_shared(std::vector<SharedOperation>& path, const usize depth, const std::vector<SharedOperation>& candidates,
                    long long& sequences, long long& mismatches) {
    for (const SharedOperation& candidate : candidates) {
        path.push_back(candidate);
        const long long live_before = ledger.live;
        const u64 granted_before = stxlib::detail::vocabulary_allocations_granted();
        const u64 released_before = stxlib::detail::vocabulary_blocks_released();
        {
            SharedWorld world;
            for (usize index = 0; index < path.size(); ++index) {
                apply_shared(path[index], world, static_cast<int>(index + 1), mismatches);
                if (!shared_matches(world)) mismatches = mismatches + 1;
            }
        }
        // Every object destroyed once, every block released once.
        if (ledger.live != live_before) mismatches = mismatches + 1;
        const u64 granted = stxlib::detail::vocabulary_allocations_granted() - granted_before;
        const u64 released = stxlib::detail::vocabulary_blocks_released() - released_before;
        if (granted != released) mismatches = mismatches + 1;
        sequences = sequences + 1;
        if (path.size() < depth) explore_shared(path, depth, candidates, sequences, mismatches);
        path.pop_back();
    }
}

void shared_exhaustive(const usize depth) {
    const std::vector<SharedOperation> candidates = shared_candidates();
    std::vector<SharedOperation> path;
    long long sequences = 0;
    long long mismatches = 0;
    explore_shared(path, depth, candidates, sequences, mismatches);
    CHECK(mismatches == 0);
    CHECK(ledger.errors == 0);
    std::printf("shared: depth %zu over %zu operations, %lld sequences, %lld mismatches\n", depth, candidates.size(),
                sequences, mismatches);
}

// An object that asks for an owner of itself, also from its destructor.
struct SelfAware final : stxlib::SharedFromThis<SelfAware> {
    bool* owner_in_destructor;
    explicit SelfAware(bool* const report) noexcept : owner_in_destructor(report) {}
    SelfAware(const SelfAware&) noexcept = default;
    ~SelfAware() {
        const stxlib::Shared<SelfAware> late = shared_from_this();
        *owner_in_destructor = late.present();
    }
};

struct Animal {
    virtual ~Animal() = default;
    int legs{4};
};
struct Bird final : Animal {
    Bird() noexcept { legs = 2; }
};

void shared_self_and_casts() {
    bool owner_in_destructor = true;
    {
        stxlib::OperationResult<stxlib::Shared<SelfAware>> made = stxlib::Shared<SelfAware>::make(&owner_in_destructor);
        CHECK(made.succeeded());
        const stxlib::Shared<SelfAware> first = static_cast<stxlib::OperationResult<stxlib::Shared<SelfAware>>&&>(made).take_value();
        const stxlib::Shared<SelfAware> again = (*first).shared_from_this();
        CHECK(again == first && first.owner_count() == 2);
        const stxlib::Weak<SelfAware> observer = (*first).weak_from_this();
        CHECK(!observer.expired());
        bool unused = false;
        SelfAware loose(&unused);
        CHECK(loose.shared_from_this().empty());
        const SelfAware copy_of_owned(*first);
        CHECK(copy_of_owned.shared_from_this().empty());
    }
    CHECK(!owner_in_destructor);

    stxlib::OperationResult<stxlib::Shared<Bird>> made_bird = stxlib::Shared<Bird>::make();
    CHECK(made_bird.succeeded());
    const stxlib::Shared<Bird> bird = static_cast<stxlib::OperationResult<stxlib::Shared<Bird>>&&>(made_bird).take_value();
    const stxlib::Shared<Animal> animal(bird);
    CHECK(animal.owner_count() == 2 && (*animal).legs == 2);
    const stxlib::Shared<Bird> back = stxlib::dynamic_shared_cast<Bird>(animal);
    CHECK(back == bird && bird.owner_count() == 3);
    const stxlib::Shared<Bird> statically = stxlib::static_shared_cast<Bird>(animal);
    CHECK(statically == bird);

    const long long constructed_before = ledger.constructed;
    stxlib::detail::arm_vocabulary_refusals(0);
    stxlib::OperationResult<stxlib::Shared<Tracked>> refused = stxlib::Shared<Tracked>::make(1);
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::out_of_memory);
    CHECK(ledger.constructed == constructed_before);
}

// Sampled, not proved: threads copying and dropping owners of one object
// and locking observers; the object must be destroyed exactly once.
struct Counted final {
    int* destructions;
    explicit Counted(int* const count) noexcept : destructions(count) {}
    ~Counted() { *destructions = *destructions + 1; }
};

struct Churn final {
    stxlib::Shared<Counted> owner;
    stxlib::Weak<Counted> observer;

    void operator()() const {
        for (int round = 0; round < 20000; ++round) {
            stxlib::Shared<Counted> copy = owner;
            const stxlib::Shared<Counted> locked = observer.lock();
            copy.reset();
        }
    }
};

void shared_threads() {
    int destructions = 0;
    {
        stxlib::OperationResult<stxlib::Shared<Counted>> made = stxlib::Shared<Counted>::make(&destructions);
        CHECK(made.succeeded());
        stxlib::Shared<Counted> owner = static_cast<stxlib::OperationResult<stxlib::Shared<Counted>>&&>(made).take_value();
        const stxlib::Weak<Counted> observer(owner);
        std::vector<std::thread> threads;
        for (int index = 0; index < 4; ++index) {
            const Churn churn{owner, observer};
            threads.emplace_back(churn);
        }
        owner.reset();
        for (std::thread& thread : threads) thread.join();
        CHECK(observer.expired());
    }
    CHECK(destructions == 1);
}

} // namespace

int main(const int argc, char** const argv) {
    usize depth = 4;
    if (argc == 3 && std::strcmp(argv[1], "--depth") == 0) depth = static_cast<usize>(std::strtoul(argv[2], nullptr, 10));
    growth_policy_exhaustive_8bit();
    vector_exhaustive(depth);
    vector_limits_and_moves();
    optional_exhaustive();
    unique_owners();
    unique_list_pop();
    vector_from_inside_an_element();
    optional_from_inside_its_value();
    vector_emplace_and_exact_copies();
    vector_of_its_own_node();
    ordinary_copies();
    shared_of_its_own_node();
    shared_exhaustive(depth);
    shared_self_and_casts();
    shared_threads();
    CHECK(ledger.live == 0);
    CHECK(ledger.errors == 0);
    return stxlib_tests::finish("owned_vocabulary");
}
