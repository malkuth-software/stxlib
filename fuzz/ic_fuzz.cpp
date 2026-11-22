// The block engine under libFuzzer (the IC study's phase IC2): libFuzzer
// drives coverage and keeps the corpus; the engine runs each input as a
// testcase (ic_engine.hpp) and traps on a defect. The custom mutator keeps
// inputs well formed: it edits whole steps (insert, drop, duplicate, change
// the block or slot byte) or one step's parameters (libFuzzer's own
// mutation), and flips the mode; the crossover splices step lists. A crash
// input replays with `stxlib_ic_run --replay FILE`, which prints its
// transcript; `--emit` writes it as a regression test.
//
// Stopping misuses are shown by their predicates here, not run in a child
// process (forking under libFuzzer would cost every execution).
//
// Build: STXLIB_IC_FUZZ=ON with Clang (-fsanitize=fuzzer); see
// CMakeLists.txt. Run: stxlib_ic_fuzz corpus/ -max_len=4096.

#include "ic/engine/ic_engine.hpp"
#include "ic/engine/ic_inputs.hpp"
#include "ic/generated/components.inc"

#include <cstdio>
#include <cstring>

namespace {

using stxlib::u8;
using stxlib::usize;
using stxlib::u32;

constexpr usize component_count =
    sizeof(stxlib_ic::generated::components) / sizeof(stxlib_ic::generated::components[0]);
constexpr usize maximum_spans = 1024;

struct Span final {
    usize offset;
    usize size;
};

usize spans_of(const u8* const bytes, const usize count, Span* const spans) {
    usize found = 0;
    usize position = 2;
    while (position + 3 <= count && found < maximum_spans) {
        const usize remaining = count - position - 3;
        const usize length = bytes[position + 2] < remaining ? bytes[position + 2] : remaining;
        spans[found] = Span{position, 3 + length};
        found = found + 1;
        position = position + 3 + length;
    }
    return found;
}

u32 random_of(unsigned int& seed) {
    seed = seed * 1103515245U + 12345U;
    const u32 result = static_cast<u32>(seed >> 8U);
    return result;
}

} // namespace

extern "C" usize LLVMFuzzerMutate(u8* data, usize size, usize max_size);

extern "C" int LLVMFuzzerTestOneInput(const u8* const data, const usize size) {
    stxlib_ic::Options options{};
    options.forks = false;
    options.maximum_steps = 128;
    stxlib_ic::Finding finding{};
    if (stxlib_ic::run_case(stxlib_ic::generated::components, component_count, data, size, options, nullptr,
                            finding)) {
        std::fprintf(stderr, "ic defect: %s\n", finding.what);
        __builtin_trap();
    }
    return 0;
}

extern "C" usize LLVMFuzzerCustomMutator(u8* const data, const usize size, const usize max_size,
                                         unsigned int seed) {
    if (max_size < 5) return size;
    if (size < 2) {
        data[0] = static_cast<u8>(random_of(seed));
        data[1] = static_cast<u8>(random_of(seed) & 1U);
        return 2;
    }
    Span spans[maximum_spans];
    const usize span_count = spans_of(data, size, spans);
    const u32 choice = random_of(seed) % 8U;
    if (choice == 0) {
        data[1] = static_cast<u8>(data[1] ^ 1U); // the mode
        return size;
    }
    if (choice == 1) {
        data[0] = static_cast<u8>(random_of(seed)); // the component
        return size;
    }
    if (choice == 2 || span_count == 0) {
        // A new step at the end: a block, a slot, a few parameter bytes.
        const usize length = random_of(seed) % 17U;
        if (size + 3 + length > max_size) return size;
        data[size] = static_cast<u8>(random_of(seed));
        data[size + 1] = static_cast<u8>(random_of(seed));
        data[size + 2] = static_cast<u8>(length);
        for (usize index = 0; index < length; ++index) data[size + 3 + index] = static_cast<u8>(random_of(seed));
        return size + 3 + length;
    }
    const Span span = spans[random_of(seed) % span_count];
    if (choice == 3) {
        // Drop the step.
        std::memmove(data + span.offset, data + span.offset + span.size, size - span.offset - span.size);
        return size - span.size;
    }
    if (choice == 4) {
        // Duplicate the step after itself.
        if (size + span.size > max_size) return size;
        std::memmove(data + span.offset + span.size, data + span.offset, size - span.offset);
        return size + span.size;
    }
    if (choice == 5) {
        data[span.offset] = static_cast<u8>(random_of(seed)); // another block
        return size;
    }
    if (choice == 6) {
        data[span.offset + 1] = static_cast<u8>(random_of(seed)); // another slot
        return size;
    }
    // The step's parameters, by libFuzzer's own mutation, kept within the
    // step (its length byte follows).
    const usize parameters = span.size - 3;
    const usize room = max_size - size;
    const usize most = parameters + (room < 32 ? room : 32);
    const usize limit = most < 255 ? most : 255;
    u8 scratch[255];
    std::memcpy(scratch, data + span.offset + 3, parameters);
    const usize mutated = LLVMFuzzerMutate(scratch, parameters, limit);
    const usize tail = size - span.offset - span.size;
    std::memmove(data + span.offset + 3 + mutated, data + span.offset + span.size, tail);
    std::memcpy(data + span.offset + 3, scratch, mutated);
    data[span.offset + 2] = static_cast<u8>(mutated);
    return size - parameters + mutated;
}

extern "C" usize LLVMFuzzerCustomCrossOver(const u8* const first, const usize first_size, const u8* const second,
                                           const usize second_size, u8* const out, const usize max_size,
                                           unsigned int seed) {
    if (first_size < 2 || max_size < 2) return 0;
    Span first_spans[maximum_spans];
    Span second_spans[maximum_spans];
    const usize first_count = spans_of(first, first_size, first_spans);
    const usize second_count = spans_of(second, second_size, second_spans);
    // first's header and a prefix of its steps, then a suffix of second's.
    const usize keep = first_count == 0 ? 0 : random_of(seed) % (first_count + 1);
    const usize from = second_count == 0 ? 0 : random_of(seed) % (second_count + 1);
    const usize head = keep == 0 ? 2 : first_spans[keep - 1].offset + first_spans[keep - 1].size;
    const usize tail_start = from < second_count ? second_spans[from].offset : second_size;
    const usize tail = second_size >= tail_start ? second_size - tail_start : 0;
    if (head > max_size) return 0;
    std::memcpy(out, first, head);
    const usize copied = head + tail <= max_size ? tail : max_size - head;
    std::memcpy(out + head, second + tail_start, copied);
    return head + copied;
}
