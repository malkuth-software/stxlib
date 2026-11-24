// The consumer of the install-and-consume check: a vector, a hash and an
// atomic read-modify-write (an outline-atomics helper call on aarch64).
#include "stxlib/vector.hpp"

#include <atomic>
#include <cstdio>

int main() {
    stxlib::Vector<int> values;
    for (int index = 0; index < 100; ++index) {
        if (values.push_copy(index).failed()) return 1;
    }
    std::atomic<int> counter{0};
    for (const int value : values) counter.fetch_add(value, std::memory_order_relaxed);
    std::printf("stxlib consumer: %d values, sum %d\n", static_cast<int>(values.count()), counter.load());
    return counter.load() == 4950 ? 0 : 1;
}
