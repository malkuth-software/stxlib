// stxlib::markdown: lookup of a named character reference (entities.hpp).
// The table is generated from the WHATWG list (tools/
// gen_markdown_tables.py): entity_names is every name end to end, and
// entities is sorted by name, byte-wise, a shorter name before its
// extensions.

#include "algorithms/markdown/entities.hpp"

namespace stxlib::markdown::detail {

namespace {

struct NamedReference final {
    u32 name_offset{0};
    u32 name_length{0};
    u32 first{0};
    u32 second{0};
};

#include "algorithms/markdown/entities_table.inc"

// The order of name[0, length) against the table's entry: negative, zero
// or positive.
[[nodiscard]] i32 compare_name(const char* const name, const u32 length, const NamedReference& entry) noexcept {
    const u32 shared = length < entry.name_length ? length : entry.name_length;
    for (u32 index = 0; index < shared; ++index) {
        const u8 left = static_cast<u8>(name[index]);
        const u8 right = static_cast<u8>(entity_names[entry.name_offset + index]);
        if (left != right) {
            const i32 order = left < right ? -1 : 1;
            return order;
        }
    }
    if (length == entry.name_length) return 0;
    const i32 order = length < entry.name_length ? -1 : 1;
    return order;
}

} // namespace

bool find_named_reference(const char* const name, const u32 length, u32& first, u32& second) noexcept {
    usize low = 0;
    usize high = sizeof(entities) / sizeof(entities[0]);
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const i32 order = compare_name(name, length, entities[middle]);
        if (order == 0) {
            first = entities[middle].first;
            second = entities[middle].second;
            return true;
        }
        if (order < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return false;
}

} // namespace stxlib::markdown::detail
