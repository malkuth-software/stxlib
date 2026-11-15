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
    append_self,   // append_copies(view().first(first))
    append_outside // append_copies({fresh, fresh + 1})
};

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
stxlib::Status apply_vector(const VectorOperation& operation, stxlib::Vector<Element>& vector, std::vector<int>& model,
                            const int fresh) {
    const usize first = operation.first;
    const usize second = operation.second;
    switch (operation.step) {
    case VectorStep::push: {
        Element argument(fresh);
        const stxlib::Status status = vector.push(static_cast<Element&&>(argument));
        if (status.succeeded()) {
            model.push_back(fresh);
            if (argument.value != moved_from_value) vector_tally.mismatches = vector_tally.mismatches + 1;
        } else if (argument.value != fresh) {
            vector_tally.mismatches = vector_tally.mismatches + 1;
        }
        return status;
    }
    case VectorStep::push_inside: {
        const stxlib::Status status = vector.insert(first, static_cast<Element&&>(vector[second]));
        if (status.succeeded()) {
            const int moved = model[second];
            model[second] = moved_from_value;
            model.insert(model.begin() + static_cast<long>(first), moved);
        }
        return status;
    }
    case VectorStep::push_copy_outside: {
        const Element argument(fresh);
        const stxlib::Status status = vector.push_copy(argument);
        if (status.succeeded()) model.push_back(fresh);
        return status;
    }
    case VectorStep::push_copy_inside: {
        const stxlib::Status status = vector.push_copy(vector[first]);
        if (status.succeeded()) {
            const int copied = model[first];
            model.push_back(copied);
        }
        return status;
    }
    case VectorStep::emplace: {
        const stxlib::Status status = vector.emplace(fresh);
        if (status.succeeded()) model.push_back(fresh);
        return status;
    }
    case VectorStep::insert: {
        Element argument(fresh);
        const stxlib::Status status = vector.insert(first, static_cast<Element&&>(argument));
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
        return stxlib::Status::success();
    case VectorStep::erase_range:
        vector.erase_range(first, second);
        model.erase(model.begin() + static_cast<long>(first), model.begin() + static_cast<long>(first + second));
        return stxlib::Status::success();
    case VectorStep::take_last: {
        const Element taken = vector.take_last();
        if (taken.value != model.back()) vector_tally.mismatches = vector_tally.mismatches + 1;
        model.pop_back();
        return stxlib::Status::success();
    }
    case VectorStep::remove_last:
        vector.remove_last();
        model.pop_back();
        return stxlib::Status::success();
    case VectorStep::truncate:
        vector.truncate(first);
        model.resize(first);
        return stxlib::Status::success();
    case VectorStep::clear:
        vector.clear();
        model.clear();
        return stxlib::Status::success();
    case VectorStep::release:
        vector.release();
        model.clear();
        if (vector.capacity() != 0) vector_tally.mismatches = vector_tally.mismatches + 1;
        return stxlib::Status::success();
    case VectorStep::reserve: {
        const stxlib::Status status = vector.reserve(first);
        if (status.succeeded() && vector.capacity() < first) vector_tally.mismatches = vector_tally.mismatches + 1;
        return status;
    }
    case VectorStep::resize: {
        const stxlib::Status status = vector.resize(first);
        if (status.succeeded()) model.resize(first, 0);
        return status;
    }
    case VectorStep::append_self: {
        const stxlib::Status status = vector.append_copies(vector.view().first(first));
        if (status.succeeded()) {
            for (usize index = 0; index < first; ++index) {
                const int copied = model[index];
                model.push_back(copied);
            }
        }
        return status;
    }
    case VectorStep::append_outside: {
        const Element outside[2] = {Element(fresh), Element(fresh + 1)};
        const stxlib::View<Element> source(outside);
        const stxlib::Status status = vector.append_copies(source);
        if (status.succeeded()) {
            model.push_back(fresh);
            model.push_back(fresh + 1);
        }
        return status;
    }
    }
    vector_tally.mismatches = vector_tally.mismatches + 1;
    return stxlib::Status::success();
}

template <typename Element>
void replay(const std::vector<VectorOperation>& path, stxlib::Vector<Element>& vector, std::vector<int>& model) {
    for (usize index = 0; index < path.size(); ++index) {
        const stxlib::Status status = apply_vector<Element>(path[index], vector, model, static_cast<int>(100 + index));
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
        {
            stxlib::Vector<Element> vector;
            std::vector<int> model;
            replay<Element>(path, vector, model);
            const usize capacity_before = vector.capacity();
            stxlib::detail::arm_vocabulary_refusals(0);
            const stxlib::Status status = apply_vector<Element>(candidate, vector, model, fresh);
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
    stxlib::Result<stxlib::Vector<u64>> huge = stxlib::Vector<u64>::with_capacity(stxlib::usize_max);
    CHECK(huge.failed() && (*huge.error()).kind() == stxlib::ErrorKind::limit_exceeded);

    stxlib::Result<stxlib::Vector<u64>> made = stxlib::Vector<u64>::with_capacity(3);
    CHECK(made.succeeded());
    stxlib::Vector<u64> numbers = static_cast<stxlib::Result<stxlib::Vector<u64>>&&>(made).take_value();
    CHECK(numbers.capacity() >= 3 && numbers.empty());
    for (u64 value = 0; value < 40; ++value) CHECK(numbers.push_copy(value).succeeded());
    u64 sum = 0;
    for (const u64 value : numbers) sum = sum + value;
    CHECK(sum == 780);

    stxlib::Vector<u64> moved(static_cast<stxlib::Vector<u64>&&>(numbers));
    CHECK(numbers.empty() && numbers.capacity() == 0);
    CHECK(moved.count() == 40 && moved[39] == 39);

    stxlib::Result<stxlib::Vector<u64>> copied = moved.copy();
    CHECK(copied.succeeded());
    CHECK(*copied.value() == moved);
    (*copied.value())[0] = 7;
    CHECK(*copied.value() != moved);

    stxlib::detail::arm_vocabulary_refusals(0);
    stxlib::Result<stxlib::Vector<u64>> refused_copy = moved.copy();
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
        stxlib::Result<stxlib::Unique<Joined>> made = stxlib::Unique<Joined>::make(9);
        CHECK(made.succeeded());
        stxlib::Unique<Joined> joined = static_cast<stxlib::Result<stxlib::Unique<Joined>>&&>(made).take_value();
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
    stxlib::Result<stxlib::Unique<Tracked>> refused = stxlib::Unique<Tracked>::make(3);
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::out_of_memory);
    CHECK(ledger.constructed == constructed_before);
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
        stxlib::Result<stxlib::Shared<Tracked>> made = stxlib::Shared<Tracked>::make(fresh);
        if (made.failed()) {
            mismatches = mismatches + 1;
            return;
        }
        world.owners[first] = static_cast<stxlib::Result<stxlib::Shared<Tracked>>&&>(made).take_value();
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
        stxlib::Result<stxlib::Shared<SelfAware>> made = stxlib::Shared<SelfAware>::make(&owner_in_destructor);
        CHECK(made.succeeded());
        const stxlib::Shared<SelfAware> first = static_cast<stxlib::Result<stxlib::Shared<SelfAware>>&&>(made).take_value();
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

    stxlib::Result<stxlib::Shared<Bird>> made_bird = stxlib::Shared<Bird>::make();
    CHECK(made_bird.succeeded());
    const stxlib::Shared<Bird> bird = static_cast<stxlib::Result<stxlib::Shared<Bird>>&&>(made_bird).take_value();
    const stxlib::Shared<Animal> animal(bird);
    CHECK(animal.owner_count() == 2 && (*animal).legs == 2);
    const stxlib::Shared<Bird> back = stxlib::dynamic_shared_cast<Bird>(animal);
    CHECK(back == bird && bird.owner_count() == 3);
    const stxlib::Shared<Bird> statically = stxlib::static_shared_cast<Bird>(animal);
    CHECK(statically == bird);

    const long long constructed_before = ledger.constructed;
    stxlib::detail::arm_vocabulary_refusals(0);
    stxlib::Result<stxlib::Shared<Tracked>> refused = stxlib::Shared<Tracked>::make(1);
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
        stxlib::Result<stxlib::Shared<Counted>> made = stxlib::Shared<Counted>::make(&destructions);
        CHECK(made.succeeded());
        stxlib::Shared<Counted> owner = static_cast<stxlib::Result<stxlib::Shared<Counted>>&&>(made).take_value();
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
    shared_exhaustive(depth);
    shared_self_and_casts();
    shared_threads();
    CHECK(ledger.live == 0);
    CHECK(ledger.errors == 0);
    return stxlib_tests::finish("owned_vocabulary");
}
