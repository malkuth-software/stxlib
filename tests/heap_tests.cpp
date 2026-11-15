// Tests of the deterministic heap and priority queue (heap.hpp, R6).
//
// What is exhaustive here, and over which domain (proofs/REPORT.md):
// - every sequence of up to `depth` operations (default 9) drawn from
//   {push priority 0, push 1, push 2, pop}, run on PriorityQueue and on
//   stable_heap_push and stable_heap_pop over caller storage, against a
//   model that removes the smallest (priority, insertion order) pair. Each
//   value carries a unique tag, so equal priorities must come out exactly
//   in insertion order; the heap property is checked after every step;
// - a refused allocation leaves the queue and the pushed value unchanged.
// Sampled: move-only values, LargestFirst, a long random run.

#include "stxlib/detail/vocabulary_allocation.hpp"
#include "stxlib/heap.hpp"
#include "stxlib/unique.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using stxlib::u32;
using stxlib::u64;
using stxlib::usize;

struct Item final {
    u32 priority;
    u32 tag;
};

struct ByPriority final {
    [[nodiscard]] static bool before(const Item& a, const Item& b) noexcept {
        const bool result = a.priority < b.priority;
        return result;
    }
};

// The model: the earliest-inserted item of the smallest priority.
struct Model final {
    std::vector<Item> items;  // in insertion order

    void push(const Item item) { items.push_back(item); }
    bool pop(Item& out) {
        if (items.empty()) return false;
        usize best = 0;
        for (usize index = 1; index < items.size(); ++index) {
            if (items[index].priority < items[best].priority) best = index;
        }
        out = items[best];
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(best));
        return true;
    }
};

bool heap_valid(const stxlib::HeapEntry<Item>* const entries, const usize count) {
    for (usize index = 1; index < count; ++index) {
        const usize parent = (index - 1) / 2;
        if (stxlib::detail::heap_before<ByPriority>(entries[index], entries[parent])) return false;
    }
    return true;
}

void run_sequence(u64 code, const usize length) {
    stxlib::PriorityQueue<Item, ByPriority> queue;
    stxlib::HeapEntry<Item> storage[16]{};
    usize stored = 0;
    u64 next_sequence = 0;
    Model model;
    u32 next_tag = 0;
    bool same = true;
    for (usize step = 0; step < length; ++step) {
        const u64 operation = code % 4;
        code = code / 4;
        if (operation < 3) {
            const Item item{static_cast<u32>(operation), next_tag};
            next_tag = next_tag + 1;
            model.push(item);
            Item moved = item;
            same = same && queue.push(static_cast<Item&&>(moved)).succeeded();
            storage[stored] = stxlib::HeapEntry<Item>{item, next_sequence};
            next_sequence = next_sequence + 1;
            stored = stored + 1;
            stxlib::stable_heap_push<ByPriority>(stxlib::MutableView<stxlib::HeapEntry<Item>>(storage, stored));
        } else {
            Item expected{};
            const bool had = model.pop(expected);
            const Item* const first = queue.first();
            same = same && (first != nullptr) == had;
            if (had && first != nullptr) same = same && (*first).tag == expected.tag;
            stxlib::Optional<Item> popped = queue.pop();
            same = same && popped.present() == had;
            if (had && popped.present()) same = same && (*popped.value()).tag == expected.tag;
            if (stored != 0) {
                stxlib::stable_heap_pop<ByPriority>(stxlib::MutableView<stxlib::HeapEntry<Item>>(storage, stored));
                stored = stored - 1;
                same = same && had && storage[stored].value.tag == expected.tag;
            } else {
                same = same && !had;
            }
        }
        same = same && queue.count() == model.items.size() && stored == model.items.size();
        same = same && heap_valid(storage, stored);
    }
    CHECK(same);
}

void sequences_exhaustive(const usize depth) {
    for (usize length = 0; length <= depth; ++length) {
        u64 total = 1;
        for (usize step = 0; step < length; ++step) total = total * 4;
        for (u64 code = 0; code < total; ++code) run_sequence(code, length);
    }
}

void refusal_and_shapes() {
    stxlib::PriorityQueue<Item, ByPriority> queue;
    stxlib::detail::arm_vocabulary_refusals(0);
    Item item{1, 77};
    const stxlib::Status refused = queue.push(static_cast<Item&&>(item));
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::out_of_memory);
    CHECK(queue.empty() && item.priority == 1 && item.tag == 77);
    CHECK(queue.pop().absent());

    stxlib::Result<stxlib::PriorityQueue<int, stxlib::LargestFirst<int>>> made =
        stxlib::PriorityQueue<int, stxlib::LargestFirst<int>>::with_capacity(8);
    CHECK(made.succeeded());
    stxlib::PriorityQueue<int, stxlib::LargestFirst<int>> largest =
        static_cast<stxlib::Result<stxlib::PriorityQueue<int, stxlib::LargestFirst<int>>>&&>(made).take_value();
    const int values[] = {3, 9, -4, 9, 0, 12};
    for (const int value : values) {
        int pushed = value;
        CHECK(largest.push(static_cast<int&&>(pushed)).succeeded());
    }
    const int expected[] = {12, 9, 9, 3, 0, -4};
    for (const int value : expected) {
        stxlib::Optional<int> popped = largest.pop();
        CHECK(popped.present() && *popped.value() == value);
    }
    CHECK(largest.empty());

    // Move-only values.
    struct ByPointee final {
        [[nodiscard]] static bool before(const stxlib::Unique<int>& a, const stxlib::Unique<int>& b) noexcept {
            const bool result = *a < *b;
            return result;
        }
    };
    stxlib::PriorityQueue<stxlib::Unique<int>, ByPointee> owners;
    const int keys[] = {5, 1, 4};
    for (const int key : keys) {
        stxlib::Result<stxlib::Unique<int>> owner = stxlib::Unique<int>::make(key);
        CHECK(owner.succeeded());
        stxlib::Unique<int> held = static_cast<stxlib::Result<stxlib::Unique<int>>&&>(owner).take_value();
        CHECK(owners.push(static_cast<stxlib::Unique<int>&&>(held)).succeeded());
    }
    stxlib::Optional<stxlib::Unique<int>> smallest = owners.pop();
    CHECK(smallest.present() && *(*smallest.value()) == 1);
    owners.clear();
    CHECK(owners.empty());
}

void long_run() {
    // Sampled: a fixed pseudo-random run of 20000 operations against the model.
    stxlib::PriorityQueue<Item, ByPriority> queue;
    Model model;
    u64 state = 0x9e3779b97f4a7c15ULL;
    u32 tag = 0;
    bool same = true;
    for (int step = 0; step < 20000; ++step) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const u32 draw = static_cast<u32>(state >> 33);
        if (draw % 3 != 0) {
            const Item item{draw % 17, tag};
            tag = tag + 1;
            model.push(item);
            Item moved = item;
            same = same && queue.push(static_cast<Item&&>(moved)).succeeded();
        } else {
            Item expected{};
            const bool had = model.pop(expected);
            stxlib::Optional<Item> popped = queue.pop();
            same = same && popped.present() == had;
            if (had && popped.present()) same = same && (*popped.value()).tag == expected.tag;
        }
    }
    CHECK(same && queue.count() == model.items.size());
}

} // namespace

int main(const int argument_count, char** const arguments) {
    usize depth = 9;
    if (argument_count == 3 && std::strcmp(arguments[1], "--depth") == 0) {
        depth = static_cast<usize>(std::strtoul(arguments[2], nullptr, 10));
    }
    // The caller-storage heap holds 16 entries.
    if (depth > 16) depth = 16;
    sequences_exhaustive(depth);
    refusal_and_shapes();
    long_run();
    return stxlib_tests::finish("heap");
}
