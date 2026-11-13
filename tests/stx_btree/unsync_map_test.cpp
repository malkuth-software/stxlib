// Differential tests of stx_btree::unsync::BBTreeMap against std::map, with
// the full structural invariant check after every phase (bbtree's
// unsync/map/tests/differential.rs in spirit).
//
// Ported from stx_auxlib's btree/tests/unsync_map_test.cpp at b6e330a. The
// differential runs are unchanged, and every insertion also checks that
// memory sufficed. Changed with the adaptation points: the shared pool is
// shared by copying one allocator (STXlib's pool has no std::shared_ptr).
// Added: refused insertions (null pages from a budget allocator) leave the
// map unchanged and are reported by out_of_memory().

#include "algorithms/stx_btree/pooled_page_allocator.hpp"
#include "algorithms/stx_btree/unsync_map.hpp"
#include "btree_test_support.hpp"
#include "tree_invariants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace stx_btree = stxlib::stx_btree;

namespace {

using stx_btree_tests::KeyMaker;
using stx_btree_tests::SplitMix64;
using stx_btree_tests::TestFailure;

template <typename Key, typename Value, typename Allocator = stx_btree::MemoryPageAllocator>
class Differential final {
public:
    using Map = stx_btree::unsync::BBTreeMap<Key, Value, Allocator>;
    using Reference = std::map<Key, Value>;
    using Traits = stx_btree::InlineKeyTraits<Key>;
    using Shape = typename Map::Shape;

    explicit Differential(const char* label_text) noexcept : label(label_text) {}

    void check_structure() {
        stx_btree_tests::TreeInvariants<Shape> invariants;
        const typename stx_btree_tests::TreeInvariants<Shape>::Report report =
            invariants.check(map.tree_for_inspection().root(), reference.size());
        TestFailure::expect(report.inexact_prefixes == 0, label, "prefixes are exact");
        TestFailure::expect(map.size() == reference.size(), label, "sizes agree");
        if (report.internals > largest_internal_count) {
            largest_internal_count = report.internals;
        }
    }

    std::size_t largest_internal_count = 0;

    void check_iteration() {
        typename Map::Iterator iterator = map.begin();
        typename Reference::const_iterator expected = reference.begin();
        while (expected != reference.end()) {
            TestFailure::expect(iterator != map.end(), label, "iteration reaches every entry");
            const typename Map::EntryView entry = *iterator;
            TestFailure::expect(entry.key == (*expected).first, label, "iteration keys in order");
            TestFailure::expect(entry.value == (*expected).second, label, "iteration values");
            ++iterator;
            ++expected;
        }
        TestFailure::expect(iterator == map.end(), label, "iteration ends with the reference");
        // Backward from the end.
        typename Map::Iterator backward = map.end();
        typename Reference::const_reverse_iterator reverse = reference.rbegin();
        while (reverse != reference.rend()) {
            --backward;
            const typename Map::EntryView entry = *backward;
            TestFailure::expect(entry.key == (*reverse).first, label, "backward iteration keys");
            ++reverse;
        }
    }

    void insert(const Key& key, const Value& value) {
        const std::optional<Value> old = map.insert(key, value);
        TestFailure::expect(!map.out_of_memory(), label, "memory sufficed");
        const typename Reference::iterator found = reference.find(key);
        if (found == reference.end()) {
            TestFailure::expect(!old.has_value(), label, "a new key returns no old value");
            reference.emplace(key, value);
        } else {
            TestFailure::expect(old.has_value() && *old == (*found).second, label, "a replaced value comes back");
            (*found).second = value;
        }
    }

    void try_insert(const Key& key, const Value& value) {
        const bool inserted = map.try_insert(key, value);
        TestFailure::expect(!map.out_of_memory(), label, "memory sufficed");
        const std::pair<typename Reference::iterator, bool> expected = reference.emplace(key, value);
        TestFailure::expect(inserted == expected.second, label, "try_insert agrees");
    }

    void remove(const Key& key) {
        const typename Traits::Query query = Traits::query_of(key);
        const std::optional<typename Map::Entry> removed = map.remove(query);
        const typename Reference::iterator found = reference.find(key);
        if (found == reference.end()) {
            TestFailure::expect(!removed.has_value(), label, "an absent key removes nothing");
            return;
        }
        TestFailure::expect(removed.has_value(), label, "a present key is removed");
        TestFailure::expect((*removed).key == (*found).first, label, "the removed key");
        TestFailure::expect((*removed).value == (*found).second, label, "the removed value");
        reference.erase(found);
    }

    void find(const Key& key) {
        const typename Traits::Query query = Traits::query_of(key);
        const Value* value = map.find(query);
        const typename Reference::iterator found = reference.find(key);
        if (found == reference.end()) {
            TestFailure::expect(value == nullptr, label, "an absent key is not found");
        } else {
            TestFailure::expect(value != nullptr && *value == (*found).second, label, "a present key is found");
        }
    }

    void lower_bound(const Key& key) {
        const typename Traits::Query query = Traits::query_of(key);
        const typename Map::Iterator iterator = map.lower_bound(query);
        const typename Reference::iterator expected = reference.lower_bound(key);
        if (expected == reference.end()) {
            TestFailure::expect(iterator == map.end(), label, "lower_bound past the end");
        } else {
            TestFailure::expect(iterator != map.end(), label, "lower_bound finds an entry");
            TestFailure::expect((*iterator).key == (*expected).first, label, "lower_bound's entry");
        }
    }

    void pop(bool first) {
        const std::optional<typename Map::Entry> popped = first ? map.pop_first() : map.pop_last();
        if (reference.empty()) {
            TestFailure::expect(!popped.has_value(), label, "an empty map pops nothing");
            return;
        }
        const typename Reference::iterator expected = first ? reference.begin() : std::prev(reference.end());
        TestFailure::expect(popped.has_value() && (*popped).key == (*expected).first, label, "the popped key");
        reference.erase(expected);
    }

    Map map;
    Reference reference;
    const char* label = "";
};

template <typename Key, typename Value, typename Allocator = stx_btree::MemoryPageAllocator>
void run_random(const char* label, std::uint64_t seed, std::size_t operations, std::size_t key_space) {
    Differential<Key, Value, Allocator> test(label);
    SplitMix64 random(seed);
    KeyMaker<Key> keys(seed ^ 0x5eed);
    KeyMaker<Value> values(seed ^ 0xa11);
    std::size_t step = 0;
    for (step = 0; step < operations; step += 1) {
        const std::uint64_t choice = random.below(100);
        const Key key = keys.make(random.below(key_space));
        if (choice < 45) {
            test.insert(key, values.make(random.below(1u << 20)));
        } else if (choice < 50) {
            test.try_insert(key, values.make(random.below(1u << 20)));
        } else if (choice < 75) {
            test.remove(key);
        } else if (choice < 90) {
            test.find(key);
        } else if (choice < 97) {
            test.lower_bound(key);
        } else {
            test.pop(choice < 99);
        }
        if (step % 4096 == 0) {
            test.check_structure();
        }
    }
    test.check_structure();
    test.check_iteration();
    // Drain in random order, checking the shrinking tree.
    std::vector<Key> remaining;
    typename std::map<Key, Value>::iterator entry = test.reference.begin();
    while (entry != test.reference.end()) {
        remaining.push_back((*entry).first);
        ++entry;
    }
    random.shuffle(remaining);
    for (step = 0; step < remaining.size(); step += 1) {
        test.remove(remaining[step]);
        if (step % 2048 == 0) {
            test.check_structure();
        }
    }
    test.check_structure();
    TestFailure::expect(test.map.empty(), label, "drained to empty");
    TestFailure::expect(test.largest_internal_count > 1, label, "the tree grew internal levels");
    std::printf("%-18s largest tree: %zu internal nodes\n", label, test.largest_internal_count);
}

template <typename Key, typename Value, typename Allocator = stx_btree::MemoryPageAllocator>
void run_ordered(const char* label, std::size_t count, bool ascending) {
    Differential<Key, Value, Allocator> test(label);
    KeyMaker<Key> keys(7);
    KeyMaker<Value> values(9);
    std::size_t step = 0;
    for (step = 0; step < count; step += 1) {
        const std::size_t rank = ascending ? step : count - 1 - step;
        test.insert(keys.make_ordered(rank), values.make(step));
    }
    test.check_structure();
    test.check_iteration();
    for (step = 0; step < count; step += 2) {
        test.pop(step % 4 == 0);
    }
    test.check_structure();
    test.check_iteration();
}

// Batched inserts against inserts one at a time, on a tree past the batch
// cutoff: repeated keys within a batch, keys already present, ascending runs
// that append, and keys outside the shared prefix.
template <typename Key>
void run_insert_many(const char* label, std::uint64_t seed, bool base_shares_prefix) {
    using Map = stx_btree::unsync::BBTreeMap<Key, std::uint64_t>;
    Differential<Key, std::uint64_t> test(label);
    SplitMix64 random(seed);
    KeyMaker<Key> keys(seed);
    std::uint64_t index = 0;
    for (index = 0; index < 90000; index += 1) {
        // Ordered base keys share a prefix that random batch keys lack.
        test.insert(base_shares_prefix ? keys.make_ordered(index) : keys.make(index), index);
    }
    std::size_t round = 0;
    for (round = 0; round < 3000; round += 1) {
        std::array<typename Map::Entry, 16> entries{};
        std::size_t lane = 0;
        const bool ascending = round % 50 == 0;
        for (lane = 0; lane < 16; lane += 1) {
            const std::uint64_t pick = ascending ? 1000000 + round * 16 + lane : random.below(140000);
            const std::uint64_t key_index = (lane == 9 && !ascending) ? entries[3].value : pick;
            entries[lane].key = ascending ? keys.make_ordered(pick) : keys.make(key_index);
            entries[lane].value = key_index;
        }
        std::array<typename Map::Entry, 16> copies = entries;
        const std::array<std::optional<std::uint64_t>, 16> results = test.map.insert_many(std::move(entries));
        TestFailure::expect(!test.map.out_of_memory(), label, "memory sufficed");
        for (lane = 0; lane < 16; lane += 1) {
            const typename std::map<Key, std::uint64_t>::iterator found = test.reference.find(copies[lane].key);
            if (found == test.reference.end()) {
                TestFailure::expect(!results[lane].has_value(), label, "a batched new key returns nothing");
                test.reference.emplace(copies[lane].key, copies[lane].value);
            } else {
                TestFailure::expect(results[lane].has_value() && *results[lane] == (*found).second, label, "a batched replacement");
                (*found).second = copies[lane].value;
            }
        }
        if (round % 500 == 0) {
            test.check_structure();
        }
    }
    test.check_structure();
    test.check_iteration();
    std::printf("%-18s batched inserts ok\n", label);
}

} // namespace


// Maps on one shared pool: pages carve in order, freed pages come back for
// reuse, every page is given back by the time the maps are gone.
void run_shared_pool() {
    using Pooled = stx_btree::PooledPageAllocator;
    using PooledMap = stx_btree::unsync::BBTreeMap<std::uint64_t, std::uint64_t, Pooled>;
    // STXlib: copies of one allocator share its pool (stx_auxlib passed a
    // std::shared_ptr<PagePool> to each).
    const Pooled owner{};
    TestFailure::expect(owner.has_pool(), "shared pool", "the pool was made");
    stx_btree::PagePool* const pool = &owner.page_pool();
    {
        PooledMap first{Pooled(owner)};
        PooledMap second{Pooled(owner)};
        std::uint64_t index = 0;
        for (index = 0; index < 100000; index += 1) {
            first.insert(SplitMix64::mix(index), index);
            second.insert(index, index);
        }
        const stx_btree::PagePool::Usage grown = (*pool).usage();
        TestFailure::expect(grown.pages_out > 2000 && grown.pages_free == 0, "shared pool", "pages carved");
        for (index = 0; index < 100000; index += 1) {
            const std::optional<PooledMap::Entry> removed = first.remove(SplitMix64::mix(index));
            TestFailure::expect(removed.has_value() && (*removed).value == index, "shared pool", "remove");
        }
        const stx_btree::PagePool::Usage drained = (*pool).usage();
        TestFailure::expect(drained.pages_free > 0 && drained.pages_out < grown.pages_out, "shared pool", "pages returned");
        for (index = 0; index < 100000; index += 1) {
            first.insert(SplitMix64::mix(index) ^ 1, index);
        }
        const stx_btree::PagePool::Usage regrown = (*pool).usage();
        TestFailure::expect(regrown.chunk_bytes <= grown.chunk_bytes + stx_btree::PagePool::largest_chunk_bytes * 3,
                            "shared pool", "freed pages reused");
        PooledMap moved = std::move(second);
        TestFailure::expect(moved.size() == 100000 && moved.find(77) != nullptr, "shared pool", "moved map");
        second.insert(5, 5);
        TestFailure::expect(second.find(5) != nullptr, "shared pool", "moved-from map still allocates");
    }
    const stx_btree::PagePool::Usage after = (*pool).usage();
    TestFailure::expect(after.pages_out == 0, "shared pool", "every page given back");
    std::printf("%-18s ok\n", "shared pool");
}

// An allocator that allows a set number of pages, then gives null pages.
struct PageBudget final {
    bool limited = false;
    std::size_t remaining = 0;
    std::size_t live = 0;
};

class BudgetPageAllocator final {
public:
    explicit BudgetPageAllocator(PageBudget* budget_source) noexcept : budget(budget_source) {}

    [[nodiscard]] void* allocate_page(stx_btree::PageKind kind, std::size_t bytes, std::size_t alignment) const noexcept {
        if ((*budget).limited) {
            if ((*budget).remaining == 0) {
                return nullptr;
            }
            (*budget).remaining -= 1;
        }
        const stx_btree::MemoryPageAllocator memory_pages{};
        void* page = memory_pages.allocate_page(kind, bytes, alignment);
        TestFailure::expect(page != nullptr, "refusal", "the test machine ran out of memory");
        (*budget).live += 1;
        return page;
    }
    void deallocate_page(stx_btree::PageKind kind, void* page, std::size_t bytes, std::size_t alignment) const noexcept {
        const stx_btree::MemoryPageAllocator memory_pages{};
        memory_pages.deallocate_page(kind, page, bytes, alignment);
        (*budget).live -= 1;
    }
    [[nodiscard]] bool operator==(const BudgetPageAllocator& other) const noexcept {
        return budget == other.budget;
    }

private:
    PageBudget* budget;
};

// Every insertion that needs a page is refused while none is allowed: the
// map keeps its size, its entries and its invariants, and says so; the same
// insertion then succeeds once a page is allowed. Covers the first root page,
// leaf splits and splits that climb (u64 keys, 80-byte values: small leaves).
void run_refusals() {
    using Value = std::array<std::uint8_t, 80>;
    using BudgetMap = stx_btree::unsync::BBTreeMap<std::uint64_t, Value, BudgetPageAllocator>;
    using Shape = BudgetMap::Shape;
    PageBudget budget{};
    {
        BudgetMap map{BudgetPageAllocator(&budget)};
        KeyMaker<Value> values(31);
        std::size_t refused = 0;
        std::uint64_t index = 0;
        for (index = 0; index < 20000; index += 1) {
            const std::uint64_t key = SplitMix64::mix(index);
            const std::size_t before = map.size();
            budget.limited = true;
            budget.remaining = 0;
            const bool inserted = map.try_insert(key, values.make(index));
            if (!inserted) {
                TestFailure::expect(map.out_of_memory(), "refusal", "only memory refuses a new key");
                TestFailure::expect(map.size() == before && map.find(key) == nullptr, "refusal",
                                    "a refused insertion leaves the map unchanged");
                refused += 1;
                budget.limited = false;
                const std::optional<Value> old = map.insert(key, values.make(index));
                TestFailure::expect(!old.has_value() && !map.out_of_memory() && map.find(key) != nullptr, "refusal",
                                    "the insertion succeeds with pages allowed");
            } else {
                TestFailure::expect(!map.out_of_memory(), "refusal", "an insertion that fits is not refused");
            }
            budget.limited = false;
            if (index % 2048 == 0) {
                stx_btree_tests::TreeInvariants<Shape> invariants;
                const typename stx_btree_tests::TreeInvariants<Shape>::Report report =
                    invariants.check(map.tree_for_inspection().root(), map.size());
                TestFailure::expect(report.inexact_prefixes == 0, "refusal", "prefixes are exact");
            }
        }
        TestFailure::expect(map.size() == 20000 && refused > 100, "refusal", "splits were refused and then made");
    }
    TestFailure::expect(budget.live == 0, "refusal", "every page given back");
    std::printf("%-18s ok\n", "refusal");
}

int main() {
    run_random<std::uint64_t, std::uint64_t>("u64 random", 1, 200000, 50000);
    run_random<std::uint64_t, std::uint64_t>("u64 dense", 2, 200000, 3000);
    run_random<std::int64_t, std::string>("i64 to string", 3, 120000, 20000);
    run_random<std::array<std::uint8_t, 16>, std::uint64_t>("uuid", 4, 150000, 40000);
    run_random<std::array<std::uint8_t, 32>, std::uint64_t>("hash32", 5, 100000, 30000);
    run_random<std::string, std::uint64_t>("string", 6, 150000, 30000);
    run_random<std::string, std::string>("string to string", 7, 80000, 15000);
    // Large values give 12-entry leaves: deep trees whose internal nodes
    // split, merge and lend entries across three or more levels.
    run_random<std::uint64_t, std::array<std::uint8_t, 80>>("u64 to 80 bytes", 8, 400000, 150000);
    run_random<std::string, std::array<std::uint8_t, 80>>("string to 80 bytes", 9, 300000, 100000);
    run_ordered<std::uint64_t, std::uint64_t>("u64 ascending", 100000, true);
    run_ordered<std::uint64_t, std::uint64_t>("u64 descending", 100000, false);
    run_ordered<std::string, std::uint64_t>("string ascending", 60000, true);
    run_ordered<std::array<std::uint8_t, 16>, std::uint64_t>("uuid ascending", 60000, true);
    run_random<std::uint64_t, std::uint64_t, stx_btree::PooledPageAllocator>("pooled u64", 21, 200000, 50000);
    run_random<std::string, std::string, stx_btree::PooledPageAllocator>("pooled str to str", 22, 80000, 15000);
    run_random<std::uint64_t, std::array<std::uint8_t, 80>, stx_btree::PooledPageAllocator>("pooled u64 to 80B", 23, 200000, 80000);
    run_ordered<std::uint64_t, std::uint64_t, stx_btree::PooledPageAllocator>("pooled ascending", 100000, true);
    run_shared_pool();
    run_refusals();
    run_insert_many<std::uint64_t>("u64 batches", 21, false);
    run_insert_many<std::string>("string batches", 22, false);
    run_insert_many<std::uint64_t>("u64 prefix batches", 23, true);
    run_insert_many<std::string>("string prefix batch", 24, true);
    std::printf("unsync map tests passed\n");
    return 0;
}
