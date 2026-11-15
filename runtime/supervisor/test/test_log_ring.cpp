// test_log_ring.cpp: host tests of the target output ring (log_ring.h).
//
// Exhaustive over the stated domain: every sequence of 0 to 4 appends
// whose lengths are drawn from {0, 1, 2, 4095, 4096, 4097, 8193} (7^0 +
// ... + 7^4 = 2801 sequences), each followed by copy_end into destinations
// of 0, 1, 100, 3996, 4095, 4096 and 5000 bytes, against a reference that
// keeps every byte written: the bytes, their order (each byte is a running
// counter), size, began_inside and total. Then clear() on each sequence's
// ring, and one more append after it. The lengths hold both sides of
// every boundary the ring has (empty, one, capacity minus one, capacity,
// capacity plus one, more than twice the capacity).

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../supervisor/config/test/test_support.h"
#include "../log_ring.h"

namespace {

using stxsupervisor::log_ring_bytes;
using stxsupervisor::LogRing;
using stxsupervisor::LogRingEnd;
using stxtest::check;

constexpr std::size_t lengths[] = {0u, 1u, 2u, 4095u, 4096u, 4097u, 8193u};
constexpr std::size_t length_count = sizeof(lengths) / sizeof(lengths[0]);
constexpr std::size_t destinations[] = {0u, 1u, 100u, 3996u, 4095u, 4096u, 5000u};
constexpr std::size_t appends_max = 4;
constexpr std::size_t written_max = appends_max * 8193u + 8193u;

// Static: large.
char written[written_max];
char source[8193];
char destination[5000];
LogRing ring;

[[nodiscard]] char byte_at(std::size_t position) {
    return static_cast<char>(position % 251u);
}

// The ring against the reference after count bytes were written in all.
[[nodiscard]] bool matches(std::size_t count) {
    for (std::size_t size_index = 0; size_index < sizeof(destinations) / sizeof(destinations[0]); size_index += 1) {
        const std::size_t size = destinations[size_index];
        const LogRingEnd end = ring.copy_end(std::span<char>(destination, size));
        const std::size_t kept = count < log_ring_bytes ? count : log_ring_bytes;
        const std::size_t expected = kept < size ? kept : size;
        if (end.size != expected || end.began_inside != (count > expected)) {
            return false;
        }
        for (std::size_t index = 0; index < expected; index += 1) {
            if (destination[index] != written[count - expected + index]) {
                return false;
            }
        }
    }
    return ring.total() == static_cast<std::uint64_t>(count);
}

void append_length(std::size_t length, std::size_t& count) {
    for (std::size_t index = 0; index < length; index += 1) {
        source[index] = byte_at(count + index);
        written[count + index] = source[index];
    }
    ring.append(std::span<const char>(source, length));
    count += length;
}

void test_sequences() {
    std::size_t sequences = 0;
    std::size_t failures = 0;
    std::size_t choice[appends_max] = {};
    for (std::size_t appends = 0; appends <= appends_max; appends += 1) {
        std::size_t combinations = 1;
        for (std::size_t index = 0; index < appends; index += 1) {
            combinations *= length_count;
        }
        for (std::size_t number = 0; number < combinations; number += 1) {
            std::size_t rest = number;
            for (std::size_t index = 0; index < appends; index += 1) {
                choice[index] = rest % length_count;
                rest /= length_count;
            }
            ring.clear();
            std::size_t count = 0;
            bool good = matches(0);
            for (std::size_t index = 0; index < appends; index += 1) {
                append_length(lengths[choice[index]], count);
                good = good && matches(count);
            }
            ring.clear();
            good = good && matches(0);
            std::size_t after = 0;
            append_length(lengths[(number + appends) % length_count], after);
            good = good && matches(after);
            failures += good ? 0u : 1u;
            sequences += 1;
        }
    }
    check(sequences == 2801u, "log ring", "2801 sequences");
    check(failures == 0u, "log ring", "every sequence matches the reference");
    std::printf("log ring: %zu sequences, %zu failing\n", sequences, failures);
}

}  // namespace

int main() {
    test_sequences();
    return stxtest::finish("test_log_ring");
}
