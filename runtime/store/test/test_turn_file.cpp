// test_turn_file.cpp: host tests of the turn files (store/turn_file.h and
// turn_file_system.h).
//
// - CRC-32 against the published check values (ISO-HDLC: "123456789" gives
//   0xcbf43926);
// - format and parse round trips at the sequence and length bounds;
// - every single-byte change of a valid file (each position, all 256
//   values): an accepted file is always canonical (formatting what was
//   parsed gives back the same bytes), and no change in the body is ever
//   accepted (CRC-32 detects every single-byte error);
// - every truncation and a one-byte extension refused; body_max enforced;
// - choose_turns over every pair of file states drawn from {invalid,
//   1, 2, 3, max - 1, max}: the newest, the older, where to write next and
//   with which sequence;
// - the system half in a temporary directory: writes alternate between the
//   files, a damaged newest file falls back to the older, a store at the
//   largest sequence refuses to write.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../supervisor/config/test/test_support.h"
#include "../turn_file.h"
#include "../turn_file_system.h"

namespace {

using stxstore::TurnChoice;
using stxstore::TurnView;
using stxtest::check;
using stxtest::text_of;

constexpr std::size_t body_max = 64;

// A whole turn file into out; its size.
[[nodiscard]] std::size_t make_file(std::uint32_t sequence, std::span<const char> body, char* out) {
    char header[stxstore::turn_header_bytes_max];
    const std::size_t header_size =
        stxstore::format_turn_header(sequence, body, std::span<char, stxstore::turn_header_bytes_max>(header));
    std::memcpy(out, header, header_size);
    std::memcpy(out + header_size, body.data(), body.size());
    return header_size + body.size();
}

void test_crc() {
    check(stxstore::crc32_of(text_of("")) == 0u, "crc", "empty input");
    check(stxstore::crc32_of(text_of("123456789")) == 0xcbf43926u, "crc", "check value 123456789");
    check(stxstore::crc32_of(text_of("a")) == 0xe8b7be43u, "crc", "check value a");
    check(stxstore::crc32_of(text_of("The quick brown fox jumps over the lazy dog")) == 0x414fa339u, "crc",
          "check value fox");
}

void test_round_trips() {
    const std::uint32_t sequences[] = {1u, 9u, 10u, 4294967294u, 4294967295u};
    const char* const bodies[] = {"", "x", "hello\n", "[system]\nhostname = a\n"};
    char file[stxstore::turn_header_bytes_max + body_max];
    for (const std::uint32_t sequence : sequences) {
        for (const char* const body : bodies) {
            const std::size_t size = make_file(sequence, text_of(body), file);
            const std::optional<TurnView> view = stxstore::parse_turn_file(std::span<const char>(file, size), body_max);
            check(view.has_value() && (*view).sequence == sequence && (*view).body.size() == std::strlen(body) &&
                      std::memcmp((*view).body.data(), body, std::strlen(body)) == 0,
                  "round_trip", body);
        }
    }
    // Sequence 0 is never written, and never read.
    char zero[] = "STXTURN 1 0 0 00000000\n";
    check(!stxstore::parse_turn_file(std::span<const char>(zero, std::strlen(zero)), body_max).has_value(),
          "round_trip", "sequence 0 refused");
    char leading[] = "STXTURN 1 01 0 00000000\n";
    check(!stxstore::parse_turn_file(std::span<const char>(leading, std::strlen(leading)), body_max).has_value(),
          "round_trip", "leading zero refused");
}

void test_mutations() {
    const char* const body = "[access]\nssh-key-1 = ssh-ed25519 AAAA\n";
    char original[stxstore::turn_header_bytes_max + body_max];
    const std::size_t size = make_file(4096, text_of(body), original);
    const std::size_t header_size = size - std::strlen(body);
    char mutated[sizeof(original)];
    char canonical[sizeof(original) + 1];
    std::size_t accepted = 0;
    bool canonical_always = true;
    bool body_never = true;
    for (std::size_t position = 0; position < size; position += 1) {
        for (int value = 0; value < 256; value += 1) {
            if (static_cast<unsigned char>(original[position]) == value) {
                continue;
            }
            std::memcpy(mutated, original, size);
            mutated[position] = static_cast<char>(value);
            const std::optional<TurnView> view =
                stxstore::parse_turn_file(std::span<const char>(mutated, size), body_max);
            if (!view.has_value()) {
                continue;
            }
            accepted += 1;
            if (position >= header_size) {
                body_never = false;
            }
            const std::size_t again = make_file((*view).sequence, (*view).body, canonical);
            if (again != size || std::memcmp(canonical, mutated, size) != 0) {
                canonical_always = false;
            }
        }
    }
    check(body_never, "mutations", "no single-byte change in the body accepted");
    check(canonical_always, "mutations", "every accepted file canonical");
    // Digits of the sequence may change into another valid sequence.
    check(accepted > 0u, "mutations", "sequence digit changes accepted as other sequences");
    for (std::size_t length = 0; length < size; length += 1) {
        check(!stxstore::parse_turn_file(std::span<const char>(original, length), body_max).has_value(), "mutations",
              "truncation refused");
    }
    std::memcpy(mutated, original, size);
    mutated[size] = '\n';
    check(!stxstore::parse_turn_file(std::span<const char>(mutated, size + 1u), body_max).has_value(), "mutations",
          "extension refused");
    check(!stxstore::parse_turn_file(std::span<const char>(original, size), std::strlen(body) - 1u).has_value(),
          "mutations", "body over body_max refused");
    check(stxstore::parse_turn_file(std::span<const char>(original, size), std::strlen(body)).has_value(), "mutations",
          "body at body_max accepted");
}

void test_choose() {
    // 0 stands for an invalid or absent file.
    const std::uint32_t states[] = {0u, 1u, 2u, 3u, 4294967294u, 4294967295u};
    for (const std::uint32_t first : states) {
        for (const std::uint32_t second : states) {
            std::optional<TurnView> views[2];
            if (first != 0u) {
                views[0] = TurnView{first, std::span<const char>()};
            }
            if (second != 0u) {
                views[1] = TurnView{second, std::span<const char>()};
            }
            const TurnChoice choice = stxstore::choose_turns(views[0], views[1]);
            if (first == 0u && second == 0u) {
                check(!choice.newest.has_value() && !choice.older.has_value() && choice.write_index == 0u &&
                          choice.next_sequence == std::optional<std::uint32_t>(1u),
                      "choose", "nothing valid: write file 0 with 1");
                continue;
            }
            const std::size_t newest = (second > first) ? 1u : 0u;
            const std::uint32_t top = newest == 0u ? first : second;
            const std::uint32_t other = newest == 0u ? second : first;
            bool good = choice.newest == std::optional<std::size_t>(newest) && choice.write_index == 1u - newest;
            good = good && (other == 0u ? !choice.older.has_value()
                                        : choice.older == std::optional<std::size_t>(1u - newest));
            good = good && (top == 4294967295u ? !choice.next_sequence.has_value()
                                               : choice.next_sequence == std::optional<std::uint32_t>(top + 1u));
            check(good, "choose", "newest, older, write index and next sequence");
        }
    }
}

[[nodiscard]] bool write_raw(const char* path, std::span<const char> bytes) {
    const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) {
        return false;
    }
    const ssize_t wrote = write(descriptor, bytes.data(), bytes.size());
    (void)close(descriptor);
    return wrote == static_cast<ssize_t>(bytes.size());
}

void test_system(const char* scratch) {
    char directory[256];
    char file_0[300];
    char file_1[300];
    std::snprintf(directory, sizeof(directory), "%s/turns", scratch);
    std::snprintf(file_0, sizeof(file_0), "%s/a.0", directory);
    std::snprintf(file_1, sizeof(file_1), "%s/a.1", directory);
    const stxstore::TurnPaths paths{directory, {file_0, file_1}};
    static char buffer_0[stxstore::turn_header_bytes_max + body_max];
    static char buffer_1[stxstore::turn_header_bytes_max + body_max];
    stxstore::TurnState state = stxstore::read_turns(paths, body_max, buffer_0, buffer_1);
    check(!state.choice.newest.has_value() && state.errors[0] == 0 && state.errors[1] == 0, "system",
          "absent files: nothing, no error");
    const char* const bodies[] = {"one\n", "two\n", "three\n", "four\n"};
    for (std::size_t round = 0; round < 4u; round += 1) {
        state = stxstore::read_turns(paths, body_max, buffer_0, buffer_1);
        const stxstore::TurnWriteOutcome outcome = stxstore::write_turn(paths, state.choice, text_of(bodies[round]), 0600);
        const stxstore::TurnWritten* const written = outcome.as_written();
        check(written != nullptr && (*written).file_index == round % 2u && (*written).sequence == round + 1u,
              "system", "writes alternate with rising sequences");
        state = stxstore::read_turns(paths, body_max, buffer_0, buffer_1);
        check(state.choice.newest == std::optional<std::size_t>(round % 2u) &&
                  (*state.turns[round % 2u]).body.size() == std::strlen(bodies[round]),
              "system", "the newest read back");
    }
    // Damage the newest (file 1, "four"): the older ("three") is chosen,
    // and the next write goes over the damaged one.
    check(write_raw(file_1, text_of("STXTURN 1 4 5 00000000\nfour\n")), "system", "damage written");
    state = stxstore::read_turns(paths, body_max, buffer_0, buffer_1);
    check(state.choice.newest == std::optional<std::size_t>(0u) && !state.choice.older.has_value() &&
              state.choice.write_index == 1u && state.choice.next_sequence == std::optional<std::uint32_t>(4u),
          "system", "damaged newest: the older is used, the damaged one rewritten");
    // The largest sequence: the store is full.
    char full[stxstore::turn_header_bytes_max + body_max];
    const std::size_t full_size = make_file(4294967295u, text_of("last\n"), full);
    check(write_raw(file_1, std::span<const char>(full, full_size)), "system", "full turn written");
    state = stxstore::read_turns(paths, body_max, buffer_0, buffer_1);
    const stxstore::TurnWriteOutcome refused = stxstore::write_turn(paths, state.choice, text_of("more\n"), 0600);
    check(refused.is_full(), "system", "a full store refuses to write");
    (void)unlink(file_0);
    (void)unlink(file_1);
    (void)rmdir(directory);
}

}  // namespace

int main(int argument_count, char** argument_vector) {
    test_crc();
    test_round_trips();
    test_mutations();
    test_choose();
    // The scratch directory: the first argument, else the working one.
    test_system(argument_count > 1 ? argument_vector[1] : ".");
    return stxtest::finish("test_turn_file");
}
