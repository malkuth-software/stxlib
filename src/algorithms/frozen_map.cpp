// FrozenMap and FrozenTextMap (include/stxlib/frozen_map.hpp; roadmap M2,
// R5): the compiled facade over stx_btree's static optimal map. STXlib's
// vocabulary outside, the template's inside; the template maps each key to
// its position in the sorted arrays the facade keeps.

#include "stxlib/frozen_map.hpp"

#include "algorithms/stx_btree/optimal_map.hpp"
#include "stxlib/error.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"
#include "stxlib/text.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>

namespace stxlib::stx_btree {

// Text keys as views of bytes the facade owns: the template's heap-string
// encoding (inline_key.hpp, InlineKeyTraits<std::string>) with the key a
// view, so no key is a std::string on the global heap. Only this file uses
// this specialization. Generated from that specialization by replacing the
// key type; the encoding, the windows and the comparisons are unchanged.
// clang-format off
template <>
struct InlineKeyTraits<std::string_view> final : InlineKeyWindows<InlineKeyTraits<std::string_view>> {
    using Key = std::string_view;
    using Query = std::string_view;
    static constexpr InlineWidth leaf_width = InlineWidth::eight;
    static constexpr InlineWidth internal_width = InlineWidth::four;
    static constexpr bool has_hash_tags = true;
    // A key points at its bytes: no racing copy may be compared (D57).
    static constexpr bool orders_by_own_bytes = false;
    // A tag below 255 is the key's exact length: a key that ends within its
    // leaf window is settled by its window and tag, reading no heap byte.
    static constexpr bool tag_is_length = true;

    [[nodiscard]] static const std::uint8_t* bytes_of(const Query& query) noexcept {
        return reinterpret_cast<const std::uint8_t*>(query.data());
    }
    [[nodiscard]] static Query query_of(const Key& key) noexcept {
        return Query(key.data(), key.size());
    }
    [[nodiscard]] static InlineWord window(const Query& query, std::size_t offset) noexcept {
        return ByteEncoding::window(bytes_of(query), query.size(), offset);
    }
    [[nodiscard]] static int compare_from(
        const Query& query, const Key& stored, std::size_t equal_length) noexcept {
        const Query stored_view = query_of(stored);
        return ByteEncoding::compare_from(
            bytes_of(query), query.size(), bytes_of(stored_view), stored_view.size(), equal_length);
    }
    [[nodiscard]] static bool equal_from(
        const Query& query, const Key& stored, std::size_t equal_length) noexcept {
        if (query.size() != stored.size()) {
            return false;
        }
        const int order = compare_from(query, stored, equal_length);
        return order == 0;
    }
    [[nodiscard]] static std::uint8_t hash_tag(const Query& query) noexcept {
        if (query.size() >= 255) {
            return 255;
        }
        return static_cast<std::uint8_t>(query.size());
    }
    [[nodiscard]] static std::size_t encoded_length(const Query& query) noexcept {
        return query.size();
    }
    [[nodiscard]] static std::size_t common_prefix_length(const Key& a, const Key& b) noexcept {
        const std::size_t limit = a.size() > b.size() ? a.size() : b.size();
        const std::size_t difference = ByteEncoding::first_difference(
            reinterpret_cast<const std::uint8_t*>(a.data()), a.size(),
            reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), limit);
        if (difference == ByteEncoding::no_difference || difference > maximum_prefix_length) {
            return maximum_prefix_length;
        }
        return difference;
    }
    [[nodiscard]] static std::size_t leading_bytes(
        const Query& query, std::uint8_t* out, std::size_t capacity) noexcept {
        const std::size_t length = query.size() < capacity ? query.size() : capacity;
        std::memcpy(out, query.data(), length);
        return length;
    }
    [[nodiscard]] static std::size_t shared_length(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        return ByteEncoding::shared_length(bytes_of(query), query.size(), bytes, length);
    }
    // A query shorter than the shared bytes, and otherwise equal, sorts
    // before every key, which all start with them.
    [[nodiscard]] static int compare_shared(
        const Query& query, const std::uint8_t* bytes, std::size_t length) noexcept {
        const std::size_t limit = query.size() < length ? query.size() : length;
        const int order = limit == 0 ? 0 : std::memcmp(query.data(), bytes, limit);
        if (order != 0) {
            return order < 0 ? -1 : 1;
        }
        if (query.size() < length) {
            return -1;
        }
        return 0;
    }
};
// clang-format on

} // namespace stxlib::stx_btree

namespace stxlib {

namespace {

using Position = std::uint32_t;
using NumberMap = stx_btree::OptimalMap<std::uint64_t, Position>;
using TextMap = stx_btree::OptimalMap<std::string_view, Position>;

static_assert(FrozenMap::key_limit <= stx_btree::maximum_search_keys, "the key limit fits the search trees");

// A block of `count` trivially copyable elements from memory::allocate,
// released at the end of the scope. `ready` is false when it could not be
// had (a count of zero needs none).
template <typename Element>
class Scratch final {
public:
    explicit Scratch(const usize element_count) noexcept : count_(element_count) {
        if (element_count == 0) return;
        if (element_count > usize_max / sizeof(Element)) {
            ready_ = false;
            return;
        }
        elements_ = static_cast<Element*>(memory::allocate(element_count * sizeof(Element), alignof(Element)));
        ready_ = elements_ != nullptr;
    }
    ~Scratch() {
        if (elements_ != nullptr) memory::release(elements_);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] Element* data() noexcept { return elements_; }
    [[nodiscard]] MutableView<Element> view() noexcept {
        const MutableView<Element> result(elements_, count_);
        return result;
    }
    [[nodiscard]] View<Element> read() const noexcept {
        const View<Element> result(elements_, count_);
        return result;
    }

private:
    Element* elements_{nullptr};
    usize count_{0};
    bool ready_{true};
};

[[nodiscard]] ErrorRecord exhausted(const Operation operation) noexcept {
    const ErrorRecord result = ErrorRecord::make(ErrorKind::out_of_memory, operation);
    return result;
}

[[nodiscard]] ErrorRecord refused(const ErrorKind kind, const Operation operation, const u32 code,
                                  const TextView message) noexcept {
    const ErrorRecord result = ErrorRecord::make(kind, operation, ErrorDetail{.code = code, .message = message});
    return result;
}

[[nodiscard]] u32 saturated(const usize index) noexcept {
    const u32 result = index > static_cast<usize>(u32_max) ? u32_max : static_cast<u32>(index);
    return result;
}

// The checks every build shares: lengths, the key limit, and the weights'
// lengths when weighted. Empty `key_weights` and `gap_weights` with
// `weighted` false mean uniform weights.
[[nodiscard]] OperationStatus check_shape(const usize key_count, const usize value_count, const bool weighted,
                                 const usize key_weight_count, const usize gap_weight_count,
                                 const Operation operation) noexcept {
    if (key_count != value_count) {
        return OperationStatus::failure(refused(ErrorKind::invalid_argument, operation, 0, "keys and values differ in length"));
    }
    if (key_count > FrozenMap::key_limit) {
        return OperationStatus::failure(refused(ErrorKind::limit_exceeded, operation, 0, "too many keys for a frozen map"));
    }
    if (weighted && (key_weight_count != key_count || gap_weight_count != key_count + 1)) {
        return OperationStatus::failure(refused(ErrorKind::invalid_argument, operation, 0,
                                       "one weight per key and one more gap weight than keys are needed"));
    }
    return OperationStatus::success();
}

// The search weights in the template's element type, and the construction:
// uniform (every key and gap weight 1) with the minimum-path tree, or the
// caller's with Knuth's exact optimum up to knuth_limit keys and the
// bottom-up construction above.
struct Weights final {
    Scratch<stx_btree::SearchWeight> keys;
    Scratch<stx_btree::SearchWeight> gaps;
    stx_btree::OptimalMapPlan plan;

    Weights(const usize key_count, const bool weighted, const View<u64> key_weights,
            const View<u64> gap_weights) noexcept
        : keys(key_count), gaps(key_count + 1), plan() {
        if (!keys.ready() || !gaps.ready()) return;
        for (usize index = 0; index < key_count; ++index) {
            keys.data()[index] = weighted ? static_cast<stx_btree::SearchWeight>(key_weights[index]) : 1;
        }
        for (usize index = 0; index <= key_count; ++index) {
            gaps.data()[index] = weighted ? static_cast<stx_btree::SearchWeight>(gap_weights[index]) : 1;
        }
        if (!weighted) {
            plan.tree = stx_btree::TreeConstruction::balanced;
        } else if (key_count <= FrozenMap::knuth_limit) {
            plan.tree = stx_btree::TreeConstruction::knuth;
        } else {
            plan.tree = stx_btree::TreeConstruction::bottom_up;
        }
    }

    [[nodiscard]] bool ready() const noexcept { return keys.ready() && gaps.ready(); }
    [[nodiscard]] stx_btree::SearchWeights view() const noexcept {
        const stx_btree::SearchWeights result{.keys = keys.read(), .gaps = gaps.read()};
        return result;
    }
};

// Builds the template over `keys` (already checked ascending) with the
// positions 0..count-1, and places it on memory::allocate.
template <typename Map, typename Key>
[[nodiscard]] OperationResult<void*> place_map(const MutableView<Key> keys, const Weights& weights,
                                      const Operation operation) noexcept {
    const usize count = keys.count();
    Scratch<Position> positions(count);
    if (!positions.ready()) return OperationResult<void*>::failure(exhausted(operation));
    for (usize index = 0; index < count; ++index) positions.data()[index] = static_cast<Position>(index);
    OperationResult<Map> built = Map::build(keys, positions.view(), weights.view(), weights.plan);
    if (built.failed()) return OperationResult<void*>::failure(*built.error());
    void* const storage = memory::allocate(sizeof(Map), alignof(Map));
    if (storage == nullptr) return OperationResult<void*>::failure(exhausted(operation));
    ::new (storage) Map(static_cast<OperationResult<Map>&&>(built).take_value());
    return OperationResult<void*>::success(storage);
}

template <typename Map>
void destroy_map(void* const map) noexcept {
    if (map == nullptr) return;
    (*static_cast<Map*>(map)).~Map();
    memory::release(map);
}

[[nodiscard]] std::string_view bytes_view(const TextView text) noexcept {
    const std::string_view result(text.data(), text.count());
    return result;
}

} // namespace

// ---- FrozenMap ------------------------------------------------------------------

FrozenMap::FrozenMap(FrozenMap&& other) noexcept
    : map_(other.map_), keys_(static_cast<Vector<u64>&&>(other.keys_)),
      values_(static_cast<Vector<u64>&&>(other.values_)) {
    other.map_ = nullptr;
}

FrozenMap& FrozenMap::operator=(FrozenMap&& other) noexcept {
    if (this == &other) return *this;
    release();
    map_ = other.map_;
    other.map_ = nullptr;
    keys_ = static_cast<Vector<u64>&&>(other.keys_);
    values_ = static_cast<Vector<u64>&&>(other.values_);
    return *this;
}

FrozenMap::~FrozenMap() { release(); }

void FrozenMap::release() noexcept {
    destroy_map<NumberMap>(map_);
    map_ = nullptr;
    keys_.release();
    values_.release();
}

namespace {

[[nodiscard]] OperationResult<FrozenMap> build_numbers(const View<u64> keys, const View<u64> values, const bool weighted,
                                              const View<u64> key_weights, const View<u64> gap_weights,
                                              const Operation operation, FrozenMap&& empty,
                                              Vector<u64>& stored_keys, Vector<u64>& stored_values,
                                              void*& map) noexcept {
    const OperationStatus shape =
        check_shape(keys.count(), values.count(), weighted, key_weights.count(), gap_weights.count(), operation);
    if (shape.failed()) return OperationResult<FrozenMap>::failure(*shape.error());
    for (usize index = 1; index < keys.count(); ++index) {
        if (keys[index - 1] >= keys[index]) {
            return OperationResult<FrozenMap>::failure(refused(ErrorKind::invalid_argument, operation, saturated(index),
                                                      "keys are not strictly ascending"));
        }
    }
    OperationResult<Vector<u64>> copied_keys = Vector<u64>::copy_of(keys);
    if (copied_keys.failed()) return OperationResult<FrozenMap>::failure(*copied_keys.error());
    OperationResult<Vector<u64>> copied_values = Vector<u64>::copy_of(values);
    if (copied_values.failed()) return OperationResult<FrozenMap>::failure(*copied_values.error());
    if (keys.count() != 0) {
        const Weights search_weights(keys.count(), weighted, key_weights, gap_weights);
        if (!search_weights.ready()) return OperationResult<FrozenMap>::failure(exhausted(operation));
        Scratch<std::uint64_t> template_keys(keys.count());
        if (!template_keys.ready()) return OperationResult<FrozenMap>::failure(exhausted(operation));
        for (usize index = 0; index < keys.count(); ++index) {
            template_keys.data()[index] = static_cast<std::uint64_t>(keys[index]);
        }
        OperationResult<void*> placed = place_map<NumberMap>(template_keys.view(), search_weights, operation);
        if (placed.failed()) return OperationResult<FrozenMap>::failure(*placed.error());
        map = *placed.value();
    }
    stored_keys = static_cast<OperationResult<Vector<u64>>&&>(copied_keys).take_value();
    stored_values = static_cast<OperationResult<Vector<u64>>&&>(copied_values).take_value();
    return OperationResult<FrozenMap>::success(static_cast<FrozenMap&&>(empty));
}

} // namespace

OperationResult<FrozenMap> FrozenMap::build(const View<u64> keys, const View<u64> values) noexcept {
    FrozenMap result;
    OperationResult<FrozenMap> built = build_numbers(keys, values, false, View<u64>(), View<u64>(), "stxlib.frozen_map.build",
                                            static_cast<FrozenMap&&>(result), result.keys_, result.values_,
                                            result.map_);
    return built;
}

OperationResult<FrozenMap> FrozenMap::build_weighted(const View<u64> keys, const View<u64> values, const View<u64> key_weights,
                                            const View<u64> gap_weights) noexcept {
    FrozenMap result;
    OperationResult<FrozenMap> built =
        build_numbers(keys, values, true, key_weights, gap_weights, "stxlib.frozen_map.build_weighted",
                      static_cast<FrozenMap&&>(result), result.keys_, result.values_, result.map_);
    return built;
}

const u64* FrozenMap::find(const u64 key) const& noexcept {
    if (map_ == nullptr) return nullptr;
    const Position* const found = (*static_cast<const NumberMap*>(map_)).find(static_cast<std::uint64_t>(key));
    if (found == nullptr) return nullptr;
    const u64* const result = &values_[*found];
    return result;
}

bool FrozenMap::contains(const u64 key) const noexcept {
    const bool result = find(key) != nullptr;
    return result;
}

usize FrozenMap::lower_bound(const u64 key) const noexcept {
    usize low = 0;
    usize high = keys_.count();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (keys_[middle] < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

u64 FrozenMap::key_at(const usize index) const noexcept {
    STXLIB_REQUIRE(index < keys_.count(), "FrozenMap::key_at: index out of range");
    const u64 result = keys_[index];
    return result;
}

u64 FrozenMap::value_at(const usize index) const noexcept {
    STXLIB_REQUIRE(index < values_.count(), "FrozenMap::value_at: index out of range");
    const u64 result = values_[index];
    return result;
}

// ---- FrozenTextMap ----------------------------------------------------------------

FrozenTextMap::FrozenTextMap(FrozenTextMap&& other) noexcept
    : map_(other.map_), bytes_(static_cast<Vector<char>&&>(other.bytes_)),
      starts_(static_cast<Vector<usize>&&>(other.starts_)), values_(static_cast<Vector<u64>&&>(other.values_)) {
    other.map_ = nullptr;
}

FrozenTextMap& FrozenTextMap::operator=(FrozenTextMap&& other) noexcept {
    if (this == &other) return *this;
    release();
    map_ = other.map_;
    other.map_ = nullptr;
    bytes_ = static_cast<Vector<char>&&>(other.bytes_);
    starts_ = static_cast<Vector<usize>&&>(other.starts_);
    values_ = static_cast<Vector<u64>&&>(other.values_);
    return *this;
}

FrozenTextMap::~FrozenTextMap() { release(); }

void FrozenTextMap::release() noexcept {
    destroy_map<TextMap>(map_);
    map_ = nullptr;
    bytes_.release();
    starts_.release();
    values_.release();
}

namespace {

struct TextParts final {
    Vector<char>& bytes;
    Vector<usize>& starts;
    Vector<u64>& values;
    void*& map;
};

[[nodiscard]] OperationResult<FrozenTextMap> build_texts(const View<TextView> keys, const View<u64> values,
                                                const bool weighted, const View<u64> key_weights,
                                                const View<u64> gap_weights, const Operation operation,
                                                FrozenTextMap&& empty, const TextParts parts) noexcept {
    const OperationStatus shape =
        check_shape(keys.count(), values.count(), weighted, key_weights.count(), gap_weights.count(), operation);
    if (shape.failed()) return OperationResult<FrozenTextMap>::failure(*shape.error());
    usize total = 0;
    for (usize index = 0; index < keys.count(); ++index) {
        if (index != 0 && compare_text(keys[index - 1], keys[index]) >= 0) {
            return OperationResult<FrozenTextMap>::failure(refused(ErrorKind::invalid_argument, operation, saturated(index),
                                                          "keys are not strictly ascending"));
        }
        if (keys[index].count() > usize_max - total) {
            return OperationResult<FrozenTextMap>::failure(
                refused(ErrorKind::limit_exceeded, operation, 0, "the total key length is not representable"));
        }
        total = total + keys[index].count();
    }
    OperationResult<Vector<char>> made_bytes = Vector<char>::with_capacity(total);
    if (made_bytes.failed()) return OperationResult<FrozenTextMap>::failure(*made_bytes.error());
    Vector<char> bytes = static_cast<OperationResult<Vector<char>>&&>(made_bytes).take_value();
    OperationResult<Vector<usize>> made_starts = Vector<usize>::with_capacity(keys.count() + 1);
    if (made_starts.failed()) return OperationResult<FrozenTextMap>::failure(*made_starts.error());
    Vector<usize> starts = static_cast<OperationResult<Vector<usize>>&&>(made_starts).take_value();
    OperationResult<Vector<u64>> made_values = Vector<u64>::copy_of(values);
    if (made_values.failed()) return OperationResult<FrozenTextMap>::failure(*made_values.error());
    // Within the reserved capacities, so nothing below allocates.
    for (usize index = 0; index < keys.count(); ++index) {
        const OperationStatus started = starts.push_copy(bytes.count());
        const View<char> key_bytes(keys[index].data(), keys[index].count());
        const OperationStatus copied = bytes.append_copies(key_bytes);
        STXLIB_REQUIRE(started.succeeded() && copied.succeeded(), "FrozenTextMap: a copy within capacity failed");
    }
    const OperationStatus ended = starts.push_copy(bytes.count());
    STXLIB_REQUIRE(ended.succeeded(), "FrozenTextMap: a copy within capacity failed");
    if (keys.count() != 0) {
        const Weights search_weights(keys.count(), weighted, key_weights, gap_weights);
        if (!search_weights.ready()) return OperationResult<FrozenTextMap>::failure(exhausted(operation));
        Scratch<std::string_view> template_keys(keys.count());
        if (!template_keys.ready()) return OperationResult<FrozenTextMap>::failure(exhausted(operation));
        const char* const base = bytes.view().data();
        for (usize index = 0; index < keys.count(); ++index) {
            // The views point into `bytes`, whose block moves with it.
            ::new (static_cast<void*>(template_keys.data() + index))
                std::string_view(base + starts[index], starts[index + 1] - starts[index]);
        }
        OperationResult<void*> placed = place_map<TextMap>(template_keys.view(), search_weights, operation);
        if (placed.failed()) return OperationResult<FrozenTextMap>::failure(*placed.error());
        parts.map = *placed.value();
    }
    parts.bytes = static_cast<Vector<char>&&>(bytes);
    parts.starts = static_cast<Vector<usize>&&>(starts);
    parts.values = static_cast<OperationResult<Vector<u64>>&&>(made_values).take_value();
    return OperationResult<FrozenTextMap>::success(static_cast<FrozenTextMap&&>(empty));
}

} // namespace

OperationResult<FrozenTextMap> FrozenTextMap::build(const View<TextView> keys, const View<u64> values) noexcept {
    FrozenTextMap result;
    const TextParts parts{result.bytes_, result.starts_, result.values_, result.map_};
    OperationResult<FrozenTextMap> built = build_texts(keys, values, false, View<u64>(), View<u64>(),
                                              "stxlib.frozen_text_map.build", static_cast<FrozenTextMap&&>(result),
                                              parts);
    return built;
}

OperationResult<FrozenTextMap> FrozenTextMap::build_weighted(const View<TextView> keys, const View<u64> values,
                                                    const View<u64> key_weights,
                                                    const View<u64> gap_weights) noexcept {
    FrozenTextMap result;
    const TextParts parts{result.bytes_, result.starts_, result.values_, result.map_};
    OperationResult<FrozenTextMap> built =
        build_texts(keys, values, true, key_weights, gap_weights, "stxlib.frozen_text_map.build_weighted",
                    static_cast<FrozenTextMap&&>(result), parts);
    return built;
}

const u64* FrozenTextMap::find(const TextView key) const& noexcept {
    if (map_ == nullptr) return nullptr;
    const Position* const found = (*static_cast<const TextMap*>(map_)).find(bytes_view(key));
    if (found == nullptr) return nullptr;
    const u64* const result = &values_[*found];
    return result;
}

bool FrozenTextMap::contains(const TextView key) const noexcept {
    const bool result = find(key) != nullptr;
    return result;
}

usize FrozenTextMap::lower_bound(const TextView key) const noexcept {
    usize low = 0;
    usize high = values_.count();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (compare_text(key_at(middle), key) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

TextView FrozenTextMap::key_at(const usize index) const& noexcept {
    STXLIB_REQUIRE(index < values_.count(), "FrozenTextMap::key_at: index out of range");
    const usize start = starts_[index];
    const TextView result =
        detail::TextViewAccess::adopt_validated(bytes_.view().data() + start, starts_[index + 1] - start);
    return result;
}

u64 FrozenTextMap::value_at(const usize index) const noexcept {
    STXLIB_REQUIRE(index < values_.count(), "FrozenTextMap::value_at: index out of range");
    const u64 result = values_[index];
    return result;
}

} // namespace stxlib
