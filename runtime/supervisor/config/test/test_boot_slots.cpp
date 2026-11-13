// test_boot_slots.cpp: host tests of runtime/supervisor/boot_slots.h, each
// against a reference written independently, over bounded exhaustive
// domains (counts printed):
//   slot file: every sequence of 0-5 tokens out of 10 line-level tokens;
//   tries file: every sequence of 0-5 tokens out of 8;
//   boot facts: every triple out of 11 kenv values;
//   failsafe reason: every config state x 7 restart counts (x2, repeated);
//   good-boot rule: every combination of 5 flags and 6 time pairs;
//   services: every list of 0-4 services out of 8 kinds;
//   mark-good plan: every slot x fallback x 5 tries x 5 slot files.

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../../boot_slots.h"
#include "test_support.h"

using stxboot::Slot;
using stxboot::SlotFile;
using stxtest::check;

namespace {

// --- slot file -----------------------------------------------------------

const std::array<const char*, 10> slot_tokens = {"good=A", "good=B", "try=A", "try=B", "\n",
                                                 "good=C", "goodA",  "x",     "try=", "good=AB"};

// Reference: split on '\n'; every non-empty line is one of the four valid
// lines; each key at most once; both keys present.
std::optional<SlotFile> reference_slot_file(const std::string& text) {
    int good = -1;
    int tried = -1;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(begin, end - begin);
        if (!line.empty()) {
            int* target = nullptr;
            int value = -1;
            if (line == "good=A" || line == "good=B") {
                target = &good;
                value = line[5] == 'A' ? 0 : 1;
            } else if (line == "try=A" || line == "try=B") {
                target = &tried;
                value = line[4] == 'A' ? 0 : 1;
            } else {
                return std::nullopt;
            }
            if (*target != -1) {
                return std::nullopt;
            }
            *target = value;
        }
        begin = end + 1;
    }
    if (good == -1 || tried == -1) {
        return std::nullopt;
    }
    SlotFile file{};
    file.good = good == 0 ? Slot::a : Slot::b;
    file.try_slot = tried == 0 ? Slot::a : Slot::b;
    return file;
}

bool same(const std::optional<SlotFile>& left, const std::optional<SlotFile>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left.has_value() || ((*left).good == (*right).good && (*left).try_slot == (*right).try_slot);
}

std::size_t test_slot_file() {
    std::size_t inputs = 0;
    std::vector<std::size_t> digits;
    for (std::size_t length = 0; length <= 5; length += 1) {
        std::size_t total = 1;
        for (std::size_t index = 0; index < length; index += 1) {
            total *= slot_tokens.size();
        }
        for (std::size_t code = 0; code < total; code += 1) {
            std::string text;
            std::size_t rest = code;
            for (std::size_t index = 0; index < length; index += 1) {
                text += slot_tokens[rest % slot_tokens.size()];
                rest /= slot_tokens.size();
            }
            const std::span<const char> view(text.data(), text.size());
            check(same(stxboot::parse_slot_file(view), reference_slot_file(text)), "slot file", "parse = reference");
            inputs += 1;
        }
    }
    for (int good = 0; good < 2; good += 1) {
        for (int tried = 0; tried < 2; tried += 1) {
            SlotFile file{};
            file.good = good == 0 ? Slot::a : Slot::b;
            file.try_slot = tried == 0 ? Slot::a : Slot::b;
            const std::array<char, stxboot::slot_file_text_size> text = stxboot::format_slot_file(file);
            check(same(stxboot::parse_slot_file(std::span<const char>(text.data(), text.size())), file), "slot file",
                  "format then parse round-trips");
        }
    }
    std::string long_text(stxboot::slot_file_size_max + 1, '\n');
    check(!stxboot::parse_slot_file(std::span<const char>(long_text.data(), long_text.size())).has_value(),
          "slot file", "over the size limit refused");
    return inputs;
}

// --- tries file ----------------------------------------------------------

const std::array<const char*, 8> tries_tokens = {"tries=", "0", "3", "9", "a", " ", "\n", "tries"};

std::optional<std::uint32_t> reference_tries(const std::string& text) {
    if (text.size() < 7 || text.compare(0, 6, "tries=") != 0 || text[6] < '0' || text[6] > '9') {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(text[6] - '0');
}

std::size_t test_tries() {
    std::size_t inputs = 0;
    for (std::size_t length = 0; length <= 5; length += 1) {
        std::size_t total = 1;
        for (std::size_t index = 0; index < length; index += 1) {
            total *= tries_tokens.size();
        }
        for (std::size_t code = 0; code < total; code += 1) {
            std::string text;
            std::size_t rest = code;
            for (std::size_t index = 0; index < length; index += 1) {
                text += tries_tokens[rest % tries_tokens.size()];
                rest /= tries_tokens.size();
            }
            check(stxboot::parse_tries(std::span<const char>(text.data(), text.size())) == reference_tries(text),
                  "tries", "parse = reference");
            inputs += 1;
        }
    }
    for (std::uint32_t count = 0; count <= 12; count += 1) {
        const std::array<char, stxboot::tries_file_size> text = stxboot::format_tries(count);
        const std::optional<std::uint32_t> back = stxboot::parse_tries(std::span<const char>(text.data(), text.size()));
        check(back.has_value() && *back == (count > 9u ? 9u : count), "tries", "format then parse (saturating at 9)");
        check(text[stxboot::tries_file_size - 1] == '\n', "tries", "ends with a newline");
        bool blanks = true;
        for (std::size_t index = 7; index + 1 < stxboot::tries_file_size; index += 1) {
            blanks = blanks && text[index] == ' ';
        }
        check(blanks, "tries", "padded with blanks");
    }
    return inputs;
}

// --- boot facts ----------------------------------------------------------

const std::array<const char*, 11> kenv_values = {nullptr, "", "A", "B", "C", "AB", "0", "3", "9", "10", "1"};

std::size_t test_boot_facts() {
    std::size_t inputs = 0;
    for (const char* slot : kenv_values) {
        for (const char* tries : kenv_values) {
            for (const char* fallback : kenv_values) {
                const stxboot::BootFacts facts = stxboot::boot_facts(slot, tries, fallback);
                const std::string s = slot == nullptr ? std::string("<none>") : std::string(slot);
                const std::string t = tries == nullptr ? std::string("<none>") : std::string(tries);
                const std::string f = fallback == nullptr ? std::string("<none>") : std::string(fallback);
                const std::optional<Slot> want_slot =
                    s == "A" ? std::optional<Slot>(Slot::a) : (s == "B" ? std::optional<Slot>(Slot::b) : std::nullopt);
                const std::uint32_t want_tries =
                    (t.size() == 1 && t[0] >= '0' && t[0] <= '9') ? static_cast<std::uint32_t>(t[0] - '0') : 0u;
                check(facts.slot == want_slot, "boot facts", "slot");
                check(facts.tries == want_tries, "boot facts", "tries");
                check(facts.fallback == (f == "1"), "boot facts", "fallback");
                inputs += 1;
            }
        }
    }
    return inputs;
}

// --- failsafe reason -----------------------------------------------------

std::size_t test_failsafe() {
    using stxboot::ConfigState;
    using stxboot::FailsafeReason;
    const std::array<ConfigState, 5> configs = {ConfigState::loaded, ConfigState::absent, ConfigState::refused,
                                                ConfigState::unreadable, ConfigState::not_read};
    const std::array<std::optional<std::uint64_t>, 7> restarts = {
        std::nullopt, std::optional<std::uint64_t>(0), std::optional<std::uint64_t>(1),
        std::optional<std::uint64_t>(2), std::optional<std::uint64_t>(3), std::optional<std::uint64_t>(4),
        std::optional<std::uint64_t>(UINT64_MAX)};
    std::size_t inputs = 0;
    for (const ConfigState config : configs) {
        for (const std::optional<std::uint64_t>& restart : restarts) {
            for (int fallback = 0; fallback < 2; fallback += 1) {
                (void)fallback;
                FailsafeReason want = FailsafeReason::none;
                if (restart.has_value() && *restart == 3u) {
                    want = FailsafeReason::restart_limit;
                } else if (config == ConfigState::absent) {
                    want = FailsafeReason::config_absent;
                } else if (config == ConfigState::refused) {
                    want = FailsafeReason::config_refused;
                } else if (config == ConfigState::unreadable) {
                    want = FailsafeReason::config_unreadable;
                }
                const FailsafeReason got = stxboot::failsafe_reason(config, restart);
                check(got == want, "failsafe", "reason = reference");
                check(std::string(stxboot::failsafe_reason_name(got)) != "unknown", "failsafe", "named");
                inputs += 1;
            }
        }
    }
    return inputs;
}

// --- good boot -----------------------------------------------------------

std::size_t test_good_boot() {
    const std::array<std::array<std::uint64_t, 2>, 6> times = {{{1000, 1000},
                                                                {1000, 60999},
                                                                {1000, 61000},
                                                                {1000, 61001},
                                                                {5000, 1000},
                                                                {0, UINT64_MAX}}};
    std::size_t inputs = 0;
    for (unsigned int flags = 0; flags < 32u; flags += 1) {
        for (const std::array<std::uint64_t, 2>& time : times) {
            stxboot::GoodBootInputs in{};
            in.registered = (flags & 1u) != 0;
            in.init_done = (flags & 2u) != 0;
            in.failsafe_taken = (flags & 4u) != 0;
            in.services_healthy = (flags & 8u) != 0;
            in.network_up = (flags & 16u) != 0;
            in.init_done_ms = time[0];
            in.now_ms = time[1];
            const bool settled = time[1] >= time[0] && time[1] - time[0] >= 60000u;
            const bool want = in.registered && in.init_done && !in.failsafe_taken && in.services_healthy &&
                              in.network_up && settled;
            check(stxboot::boot_is_good(in) == want, "good boot", "rule = reference");
            inputs += 1;
        }
    }
    return inputs;
}

std::size_t test_services() {
    using stxboot::ServiceHealth;
    using stxboot::ServiceStateKind;
    const std::array<ServiceStateKind, 4> kinds = {ServiceStateKind::stopped, ServiceStateKind::running,
                                                   ServiceStateKind::waiting, ServiceStateKind::stopping};
    std::size_t inputs = 0;
    for (std::size_t length = 0; length <= 4; length += 1) {
        std::size_t total = 1;
        for (std::size_t index = 0; index < length; index += 1) {
            total *= 8u;
        }
        for (std::size_t code = 0; code < total; code += 1) {
            std::vector<ServiceHealth> services;
            bool want = true;
            std::size_t rest = code;
            for (std::size_t index = 0; index < length; index += 1) {
                ServiceHealth health{};
                health.state = kinds[rest % 4u];
                health.restart_never = ((rest / 4u) % 2u) != 0;
                rest /= 8u;
                const bool ok = health.state == ServiceStateKind::running ||
                                health.state == ServiceStateKind::waiting ||
                                (health.state == ServiceStateKind::stopped && health.restart_never);
                want = want && ok;
                services.push_back(health);
            }
            check(stxboot::services_healthy(std::span<const ServiceHealth>(services.data(), services.size())) == want,
                  "services", "healthy = reference");
            inputs += 1;
        }
    }
    return inputs;
}

// --- mark good -----------------------------------------------------------

std::size_t test_mark_good() {
    std::size_t inputs = 0;
    const std::array<std::optional<std::uint32_t>, 5> tries = {std::nullopt, std::optional<std::uint32_t>(0),
                                                               std::optional<std::uint32_t>(1),
                                                               std::optional<std::uint32_t>(3),
                                                               std::optional<std::uint32_t>(9)};
    for (int slot = 0; slot < 3; slot += 1) {
        for (int fallback = 0; fallback < 2; fallback += 1) {
            for (const std::optional<std::uint32_t>& count : tries) {
                for (int file_code = 0; file_code < 5; file_code += 1) {
                    stxboot::BootFacts facts{};
                    facts.slot = slot == 0 ? std::nullopt
                                           : std::optional<Slot>(slot == 1 ? Slot::a : Slot::b);
                    facts.fallback = fallback != 0;
                    std::optional<SlotFile> file;
                    if (file_code > 0) {
                        SlotFile value{};
                        value.good = ((file_code - 1) & 1) == 0 ? Slot::a : Slot::b;
                        value.try_slot = ((file_code - 1) & 2) == 0 ? Slot::a : Slot::b;
                        file = value;
                    }
                    const stxboot::MarkGoodPlan plan = stxboot::mark_good_plan(facts, count, file);
                    if (!facts.slot.has_value()) {
                        check(!plan.write_tries_zero && !plan.new_slot_file.has_value(), "mark good",
                              "nothing without a known slot");
                    } else {
                        const Slot running = *facts.slot;
                        check(plan.write_tries_zero == (!count.has_value() || *count != 0u), "mark good",
                              "tries written back to 0 unless already 0");
                        const bool rewrite = !file.has_value() || (*file).good != running ||
                                             (facts.fallback && (*file).try_slot != running);
                        check(plan.new_slot_file.has_value() == rewrite, "mark good",
                              "slot file rewritten when good is another slot, or a fallback cancels try");
                        if (plan.new_slot_file.has_value()) {
                            check((*plan.new_slot_file).good == running && (*plan.new_slot_file).try_slot == running,
                                  "mark good", "rewritten file names the running slot twice");
                        }
                    }
                    inputs += 1;
                }
            }
        }
    }
    return inputs;
}

}  // namespace

int main() {
    const std::size_t slot_inputs = test_slot_file();
    const std::size_t tries_inputs = test_tries();
    const std::size_t facts_inputs = test_boot_facts();
    const std::size_t failsafe_inputs = test_failsafe();
    const std::size_t good_inputs = test_good_boot();
    const std::size_t service_inputs = test_services();
    const std::size_t mark_inputs = test_mark_good();
    std::printf("exhaustive boot slots: slot file %zu, tries %zu, facts %zu, failsafe %zu, good boot %zu, "
                "services %zu, mark good %zu inputs\n",
                slot_inputs, tries_inputs, facts_inputs, failsafe_inputs, good_inputs, service_inputs, mark_inputs);
    std::printf("test_boot_slots: %zu checks, %zu failed\n", stxtest::tally.checks, stxtest::tally.failures);
    return stxtest::tally.failures == 0 ? 0 : 1;
}
