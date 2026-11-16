// test_target_ids.cpp: host tests of the target id ledger and issuing
// (config/target_ids.h).
//
// Exhaustive over a stated domain: every sequence of 1 to 4 configuration
// writes over the targets a, b and c, where each write holds any subset of
// them and each held target that has an id gives it or leaves it out
// (27 choices per write; 27 + 27^2 + 27^3 + 27^4 = 551,880 sequences), with
// a boot (plan_boot_ids) after each write. Against a reference: a target
// held without a break keeps its id; a target new or back after a write
// without it gets the next id never used (1, 2, 3, ...); every written text
// parses with every id present; the ledger round-trips through its text
// and holds active exactly the held targets, removed the others (after the
// boot), and the boot lists each removal once.
// Stated cases: a given id not issued (wrong id, another's id, a deleted
// one) refused at the id line; a factory id never issued; ledger full;
// too large; malformed ledger texts refused; boot adoption and conflicts.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../boot_config.h"
#include "../target_ids.h"
#include "test_support.h"

namespace {

using stxconfig::BootConfig;
using stxconfig::IdIssueKind;
using stxconfig::IdIssueOutcome;
using stxconfig::LedgerState;
using stxconfig::TargetIdLedger;
using stxtest::check;
using stxtest::text_of;

// Static: large.
BootConfig user;
BootConfig factory;
BootConfig reparsed;
TargetIdLedger ledger;
TargetIdLedger copy;
char text[4096];
char out[stxconfig::config_size_max + 64];
char ledger_text[stxconfig::target_id_ledger_text_max];
char ledger_before[stxconfig::target_id_ledger_text_max];
stxconfig::BootIdPlan plan;

const char* const names[3] = {"a", "b", "c"};

void append(std::size_t& length, const char* part) {
    const std::size_t count = std::strlen(part);
    std::memcpy(&text[length], part, count);
    length += count;
}

void append_target(std::size_t& length, const char* name, std::uint32_t id) {
    append(length, "[service-");
    append(length, name);
    append(length, "]\n");
    if (id != 0u) {
        char line[32];
        std::snprintf(line, sizeof(line), "id = %u\n", id);
        append(length, line);
    }
    append(length, "command = bin/");
    append(length, name);
    append(length, "\nrole = server\nfolder = /data/targets/");
    append(length, name);
    append(length, "\n");
}

[[nodiscard]] std::uint32_t id_in(const BootConfig& config, const char* name) {
    const std::optional<std::size_t> found = config.find_service(name);
    if (!found.has_value() || !config.service(*found).target.has_value()) {
        return 0;
    }
    return (*config.service(*found).target).id;
}

[[nodiscard]] bool parse_into(BootConfig& config, std::span<const char> bytes) {
    return stxconfig::parse_boot_config(bytes, config).is_success();
}

void test_sequences() {
    // No factory targets here (test_cases has its own).
    check(parse_into(factory, text_of("")), "target ids", "empty factory");
    std::size_t sequences = 0;
    std::size_t failures = 0;
    std::size_t choice[4] = {};
    for (std::size_t writes = 1; writes <= 4u; writes += 1) {
        std::size_t combinations = 1;
        for (std::size_t index = 0; index < writes; index += 1) {
            combinations *= 27u;
        }
        for (std::size_t number = 0; number < combinations; number += 1) {
            std::size_t rest = number;
            for (std::size_t index = 0; index < writes; index += 1) {
                choice[index] = rest % 27u;
                rest /= 27u;
            }
            ledger.clear();
            // The reference: each name's id while held (0: not held), and
            // the next id never used.
            std::uint32_t held[3] = {0, 0, 0};
            std::uint32_t next = 1;
            bool good = true;
            for (std::size_t write = 0; good && write < writes; write += 1) {
                // Per name, base 3: 0 absent, 1 held giving its id, 2 held
                // leaving it out (1 without an id to give is the same as 2).
                std::size_t digits = choice[write];
                std::size_t length = 0;
                std::uint32_t expected[3] = {0, 0, 0};
                for (std::size_t name = 0; name < 3u; name += 1) {
                    const std::size_t pick = digits % 3u;
                    digits /= 3u;
                    if (pick == 0u) {
                        continue;
                    }
                    append_target(length, names[name], pick == 1u ? held[name] : 0u);
                    expected[name] = held[name] != 0u ? held[name] : next++;
                }
                good = parse_into(user, std::span<const char>(text, length));
                const std::optional<std::size_t> before = ledger.format(ledger_before);
                const IdIssueOutcome outcome = stxconfig::issue_target_ids(
                    user, factory, std::span<const char>(text, length), ledger, std::span<char>(out, sizeof(out)));
                good = good && outcome.kind == IdIssueKind::issued &&
                       parse_into(reparsed, std::span<const char>(out, outcome.length));
                // ledger_changed exactly when the file text would change.
                const std::optional<std::size_t> after = ledger.format(ledger_text);
                good = good && before.has_value() && after.has_value() &&
                       outcome.ledger_changed ==
                           (*before != *after || std::memcmp(ledger_before, ledger_text, *after) != 0);
                for (std::size_t name = 0; good && name < 3u; name += 1) {
                    good = id_in(reparsed, names[name]) == expected[name];
                    held[name] = expected[name];
                }
                // The ledger: active exactly the held ones; round trip.
                for (std::size_t entry = 0; good && entry < ledger.count(); entry += 1) {
                    const stxconfig::LedgerEntry& item = ledger.entry(entry);
                    bool is_held = false;
                    for (std::size_t name = 0; name < 3u; name += 1) {
                        is_held = is_held || held[name] == item.id;
                    }
                    good = (item.state == LedgerState::active) == is_held && item.id < next;
                }
                good = good && ledger.count() == next - 1u;
                const std::optional<std::size_t> formatted = ledger.format(ledger_text);
                good = good && formatted.has_value() &&
                       copy.parse(std::span<const char>(ledger_text, *formatted)) && copy.count() == ledger.count();
                // The boot on the written configuration: each deleted entry
                // listed once; marked removed by the caller.
                if (good) {
                    stxconfig::plan_boot_ids(reparsed, ledger, plan);
                    std::size_t deleted = 0;
                    for (std::size_t entry = 0; entry < ledger.count(); entry += 1) {
                        deleted += ledger.entry(entry).state == LedgerState::deleted ? 1u : 0u;
                    }
                    good = plan.removal_count == deleted && plan.conflicts == 0u;
                    for (std::size_t removal = 0; removal < plan.removal_count; removal += 1) {
                        ledger.set_state(plan.removals[removal], LedgerState::removed);
                    }
                }
            }
            failures += good ? 0u : 1u;
            sequences += 1;
        }
    }
    check(sequences == 551880u, "target ids", "551880 sequences");
    check(failures == 0u, "target ids", "every sequence matches the reference");
    std::printf("target ids: %zu write sequences, %zu failing\n", sequences, failures);
}

void test_cases() {
    const char* const test = "target id cases";
    // The factory's id 1 is never issued.
    check(parse_into(factory, text_of("[target-clock]\nrole = server\nid = 1\ncommand = bin/clock\n"
                                      "folder = /data/targets/clock\n")),
          test, "factory parses");
    ledger.clear();
    std::size_t length = 0;
    append_target(length, "a", 0);
    check(parse_into(user, std::span<const char>(text, length)), test, "user parses");
    IdIssueOutcome outcome =
        stxconfig::issue_target_ids(user, factory, std::span<const char>(text, length), ledger, out);
    check(outcome.kind == IdIssueKind::issued && parse_into(reparsed, std::span<const char>(out, outcome.length)) &&
              id_in(reparsed, "a") == 2u,
          test, "the factory's id is skipped");
    const char* const expected_text =
        "[service-a]\nid = 2\ncommand = bin/a\nrole = server\nfolder = /data/targets/a\n";
    check(outcome.length == std::strlen(expected_text) && std::memcmp(out, expected_text, outcome.length) == 0, test,
          "the id line right after the header");
    // The factory target stays active though the user never holds it.
    check(ledger.count() == 1u, test, "only the issued id recorded (the boot adopts the factory's)");
    // Wrong ids: not issued, another target's, the factory's.
    const std::uint32_t wrong[] = {7u, 1u};
    for (std::size_t index = 0; index < 2u; index += 1) {
        length = 0;
        append_target(length, "a", wrong[index]);
        check(parse_into(user, std::span<const char>(text, length)), test, "parses");
        outcome = stxconfig::issue_target_ids(user, factory, std::span<const char>(text, length), ledger, out);
        check(outcome.kind == IdIssueKind::refused && outcome.line == 2u && outcome.column == 1u, test,
              "an id not issued to a refused at its line");
    }
    length = 0;
    append_target(length, "b", 2);
    check(parse_into(user, std::span<const char>(text, length)), test, "parses");
    outcome = stxconfig::issue_target_ids(user, factory, std::span<const char>(text, length), ledger, out);
    check(outcome.kind == IdIssueKind::refused, test, "another target's id refused");
    // A full ledger.
    ledger.clear();
    for (std::uint32_t id = 1; id <= stxconfig::target_id_max; id += 1) {
        check(ledger.add(id, LedgerState::removed, std::span<const char>(), std::span<const char>()) ||
                  id > stxconfig::target_id_max,
              test, "fill");
    }
    length = 0;
    append_target(length, "z", 0);
    check(parse_into(user, std::span<const char>(text, length)), test, "parses");
    outcome = stxconfig::issue_target_ids(user, factory, std::span<const char>(text, length), ledger, out);
    check(outcome.kind == IdIssueKind::ledger_full, test, "every id spent: ledger full");
    // Too large.
    ledger.clear();
    outcome = stxconfig::issue_target_ids(user, factory, std::span<const char>(text, length), ledger,
                                          std::span<char>(out, length));
    check(outcome.kind == IdIssueKind::too_large, test, "no room for the id line: too large");
    // Malformed ledgers.
    const char* const bad[] = {
        "",
        "stx-target-ids 2\n",
        "stx-target-ids 1\n0 removed\n",
        "stx-target-ids 1\n1000 removed\n",
        "stx-target-ids 1\n2 removed\n1 removed\n",
        "stx-target-ids 1\n1 removed\n1 removed\n",
        "stx-target-ids 1\n1 active a\n",
        "stx-target-ids 1\n1 active A /data/targets/a\n",
        "stx-target-ids 1\n1 active a /tmp/a\n",
        "stx-target-ids 1\n1 active a /data/targets/../etc\n",
        "stx-target-ids 1\n1 gone a /data/targets/a\n",
        "stx-target-ids 1\n1 removed",
        "stx-target-ids 1\n01 removed\n",
        "stx-target-ids 1\n1 removed x\n",
    };
    std::size_t refused = 0;
    for (std::size_t index = 0; index < sizeof(bad) / sizeof(bad[0]); index += 1) {
        refused += copy.parse(text_of(bad[index])) ? 0u : 1u;
    }
    check(refused == sizeof(bad) / sizeof(bad[0]) && copy.count() == 0u, test, "malformed ledgers refused");
    const char* const fine = "stx-target-ids 1\n1 removed\n2 deleted b /data/targets/b\n3 active a /data/targets/a\n";
    check(copy.parse(text_of(fine)) && copy.count() == 3u && copy.entry(1).state == LedgerState::deleted &&
              copy.format(ledger_text).has_value() && std::memcmp(ledger_text, fine, std::strlen(fine)) == 0,
          test, "a ledger read and written back the same");
    // Boot: adoption of the factory's id, a deleted entry back with its id
    // (an older turn), and a conflict (an id the ledger gives another).
    check(parse_into(reparsed, text_of("[target-clock]\nrole = server\nid = 1\ncommand = bin/clock\n"
                                       "folder = /data/targets/clock\n"
                                       "[target-b]\nrole = server\nid = 2\ncommand = bin/b\n"
                                       "folder = /data/targets/b\n"
                                       "[target-q]\nrole = server\nid = 3\ncommand = bin/q\n"
                                       "folder = /data/targets/q\n")),
          test, "boot configuration parses");
    check(ledger.parse(text_of("stx-target-ids 1\n2 deleted b /data/targets/b\n3 active a /data/targets/a\n"
                               "4 deleted d /data/targets/d\n")),
          test, "boot ledger");
    stxconfig::plan_boot_ids(reparsed, ledger, plan);
    const std::optional<std::size_t> clock = ledger.find_id(1);
    const std::optional<std::size_t> back = ledger.find_id(2);
    check(clock.has_value() && ledger.entry(*clock).state == LedgerState::active && back.has_value() &&
              ledger.entry(*back).state == LedgerState::active && plan.conflicts == 1u && plan.removal_count == 1u &&
              ledger.entry(plan.removals[0]).id == 4u && plan.changed,
          test, "adopted, restored, one conflict, one removal");
}

}  // namespace

int main() {
    test_cases();
    test_sequences();
    return stxtest::finish("test_target_ids");
}
