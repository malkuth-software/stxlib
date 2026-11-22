// The block engine's runner (the IC study's phase IC2): random testcases
// against every component with generated blocks, minimised failures written
// as ordinary C++ tests, and the gate's stated run.
//
//   stxlib_ic_run [--component NAME] [--cases N] [--steps N] [--seed S]
//                 [--gate PATH] [--emit DIR] [--replay FILE] [--no-fork] [--list]
//
// Each component runs N legal cases (misuse blocks not eligible) and N
// misuse cases (both eligible), each up to --steps steps. A defect is
// minimised (steps, then parameter bytes), its transcript printed, and with
// --emit written to DIR as a test (tests/ic/regressions, which CMake builds
// one test per file). --gate writes the run's integrity counts as JSON
// (tools/ic_declarations.py reads it into proofs/ic/GATE.md). --replay runs
// one testcase file (raw bytes) and prints its transcript. Exit status: 0
// when no case found a defect, 1 when one did, 2 on a usage error.

#include "ic/engine/ic_engine.hpp"
#include "ic/engine/ic_inputs.hpp"
#include "ic/generated/components.inc"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using stxlib::u64;
using stxlib::u8;
using stxlib::usize;
using printed = unsigned long long; // the %llu argument type

constexpr usize component_count =
    sizeof(stxlib_ic::generated::components) / sizeof(stxlib_ic::generated::components[0]);

struct Settings final {
    const char* component{nullptr};
    u64 cases{2000};
    usize steps{48};
    u64 seed{0x1C2B3A4D5E6F7081ULL};
    const char* gate{nullptr};
    const char* emit{nullptr};
    const char* replay{nullptr};
    bool forks{true};
    bool list{false};
};

struct Recorded final {
    usize component;
    char block[96];
    char what[256];
};

u64 next(u64& state) {
    state = state + 0x9E3779B97F4A7C15ULL;
    u64 mixed = state;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    const u64 result = mixed ^ (mixed >> 31U);
    return result;
}

// A random testcase for `component`: up to `steps` steps, each with up to 24
// parameter bytes.
void make_case(std::vector<u8>& bytes, const usize component, const bool misuse, const usize steps, u64& state) {
    bytes.clear();
    bytes.push_back(static_cast<u8>(component));
    bytes.push_back(static_cast<u8>(misuse ? 1U : 0U));
    const usize count = 1 + static_cast<usize>(next(state) % steps);
    for (usize step = 0; step < count; ++step) {
        const u64 word = next(state);
        const usize length = static_cast<usize>(word % 25U);
        bytes.push_back(static_cast<u8>(word >> 8U));
        bytes.push_back(static_cast<u8>(word >> 16U));
        bytes.push_back(static_cast<u8>(length));
        u64 parameters = next(state);
        for (usize index = 0; index < length; ++index) {
            if (index % 8 == 0) parameters = next(state);
            bytes.push_back(static_cast<u8>(parameters >> (8U * (index % 8))));
        }
    }
}

bool parse_number(const char* const text, u64& value) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') return false;
    value = static_cast<u64>(parsed);
    return true;
}

int usage() {
    std::fprintf(stderr, "usage: stxlib_ic_run [--component NAME] [--cases N] [--steps N] [--seed S] [--gate PATH] "
                         "[--emit DIR] [--replay FILE] [--no-fork] [--list]\n");
    return 2;
}

void print_transcript(const stxlib_ic::Transcript& transcript) {
    for (usize index = 0; index < transcript.count(); ++index) std::printf("    %s\n", transcript.line(index));
}

void write_counts(std::FILE* const file, const stxlib_ic::Component& component,
                  const stxlib_ic::IntegrityCounts& counts, const u64 found, const bool last) {
    std::fprintf(file, "    \"%s\": {\n", component.name);
    std::fprintf(file, "      \"declaration\": \"%s\",\n", component.declaration);
    std::fprintf(file, "      \"cases\": %llu,\n", static_cast<printed>(counts.cases));
    std::fprintf(file, "      \"steps\": %llu,\n", static_cast<printed>(counts.steps));
    std::fprintf(file, "      \"completed\": %llu,\n", static_cast<printed>(counts.completed));
    std::fprintf(file, "      \"declared_failures\": %llu,\n", static_cast<printed>(counts.declared_failures));
    std::fprintf(file, "      \"refused_failure\": %llu,\n", static_cast<printed>(counts.refused_failure));
    std::fprintf(file, "      \"refused_value\": %llu,\n", static_cast<printed>(counts.refused_value));
    std::fprintf(file, "      \"refused_stops\": %llu,\n", static_cast<printed>(counts.refused_stops));
    std::fprintf(file, "      \"shown_stops\": %llu,\n", static_cast<printed>(counts.shown_stops));
    std::fprintf(file, "      \"bailed\": %llu,\n", static_cast<printed>(counts.bailed));
    std::fprintf(file, "      \"skipped\": %llu,\n", static_cast<printed>(counts.skipped));
    std::fprintf(file, "      \"blocks_run\": %u,\n", counts.blocks_run(component.block_count));
    std::fprintf(file, "      \"blocks\": %u,\n", component.block_count);
    std::fprintf(file, "      \"states_reached\": %u,\n", counts.state_count());
    std::fprintf(file, "      \"edges_reached\": %u,\n", counts.edge_count());
    std::fprintf(file, "      \"defects_found\": %llu,\n", static_cast<printed>(found));
    std::fprintf(file, "      \"defects_fixed\": 0,\n");
    // A legal-mode defect is a false positive until triage shows the code at
    // fault (then it is a defect found, and fixed by its regression test).
    std::fprintf(file, "      \"legal_false_positives\": %llu\n", static_cast<printed>(counts.legal_defects));
    std::fprintf(file, "    }%s\n", last ? "" : ",");
}

void json_text(std::FILE* const file, const char* const text) {
    for (usize index = 0; text[index] != '\0'; ++index) {
        const char character = text[index];
        if (character == '"' || character == '\\') std::fputc('\\', file);
        if (static_cast<unsigned char>(character) < 0x20U) {
            std::fputc(' ', file);
            continue;
        }
        std::fputc(character, file);
    }
}

bool write_gate(const Settings& settings, const stxlib_ic::IntegrityCounts* const counts, const u64* const found,
                const bool* const selected, const std::vector<Recorded>& recorded) {
    std::FILE* const file = std::fopen(settings.gate, "w");
    if (file == nullptr) return false;
    std::fprintf(file, "{\n  \"stated\": \"%llu legal and %llu misuse cases of up to %zu steps per component, seed "
                       "0x%llx, stopping misuses %s\",\n",
                 static_cast<printed>(settings.cases), static_cast<printed>(settings.cases),
                 settings.steps, static_cast<printed>(settings.seed),
                 settings.forks ? "run in child processes" : "shown by their predicates");
    std::fprintf(file, "  \"components\": {\n");
    usize last = 0;
    for (usize index = 0; index < component_count; ++index) {
        if (selected[index]) last = index;
    }
    for (usize index = 0; index < component_count; ++index) {
        if (!selected[index]) continue;
        write_counts(file, *stxlib_ic::generated::components[index], counts[index], found[index], index == last);
    }
    std::fprintf(file, "  },\n  \"defects\": [");
    for (usize index = 0; index < recorded.size(); ++index) {
        std::fprintf(file, "%s\n    {\"component\": \"%s\", \"block\": \"", index == 0 ? "" : ",",
                     (*stxlib_ic::generated::components[recorded[index].component]).name);
        json_text(file, recorded[index].block);
        std::fprintf(file, "\", \"what\": \"");
        json_text(file, recorded[index].what);
        std::fprintf(file, "\", \"status\": \"open\"}");
    }
    std::fprintf(file, "%s]\n}\n", recorded.empty() ? "" : "\n  ");
    const bool written = std::fclose(file) == 0;
    return written;
}

int replay(const Settings& settings) {
    std::FILE* const file = std::fopen(settings.replay, "rb");
    if (file == nullptr) return usage();
    std::vector<u8> bytes{};
    for (;;) {
        const int read = std::fgetc(file);
        if (read == EOF) break;
        bytes.push_back(static_cast<u8>(read));
    }
    std::fclose(file);
    stxlib_ic::Transcript transcript{};
    stxlib_ic::Options options{};
    options.forks = settings.forks;
    options.maximum_steps = settings.steps;
    options.transcript = &transcript;
    stxlib_ic::Finding finding{};
    const bool found = stxlib_ic::run_case(stxlib_ic::generated::components, component_count, bytes.data(),
                                           bytes.size(), options, nullptr, finding);
    print_transcript(transcript);
    std::printf("%s\n", found ? finding.what : "no defect");
    return found ? 1 : 0;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    Settings settings{};
    for (int index = 1; index < argument_count; ++index) {
        const char* const argument = arguments[index];
        const bool has_value = index + 1 < argument_count;
        u64 number = 0;
        if (std::strcmp(argument, "--list") == 0) {
            settings.list = true;
        } else if (std::strcmp(argument, "--no-fork") == 0) {
            settings.forks = false;
        } else if (std::strcmp(argument, "--component") == 0 && has_value) {
            settings.component = arguments[++index];
        } else if (std::strcmp(argument, "--gate") == 0 && has_value) {
            settings.gate = arguments[++index];
        } else if (std::strcmp(argument, "--emit") == 0 && has_value) {
            settings.emit = arguments[++index];
        } else if (std::strcmp(argument, "--replay") == 0 && has_value) {
            settings.replay = arguments[++index];
        } else if (std::strcmp(argument, "--cases") == 0 && has_value && parse_number(arguments[index + 1], number)) {
            settings.cases = number;
            index = index + 1;
        } else if (std::strcmp(argument, "--steps") == 0 && has_value && parse_number(arguments[index + 1], number)
                   && number > 0 && number < 100000) {
            settings.steps = static_cast<usize>(number);
            index = index + 1;
        } else if (std::strcmp(argument, "--seed") == 0 && has_value && parse_number(arguments[index + 1], number)) {
            settings.seed = number;
            index = index + 1;
        } else {
            return usage();
        }
    }
    if (settings.list) {
        for (usize index = 0; index < component_count; ++index) {
            const stxlib_ic::Component& component = *stxlib_ic::generated::components[index];
            std::printf("%s (%s): %u blocks, %u states, %u terms\n", component.name, component.declaration,
                        component.block_count, component.state_count, component.term_count);
        }
        return 0;
    }
    if (settings.replay != nullptr) {
        const int status = replay(settings);
        stxlib_ic::inputs::remove_scratch();
        return status;
    }
    std::vector<stxlib_ic::IntegrityCounts> counts(component_count);
    std::vector<u64> found(component_count, 0);
    bool selected[component_count]{};
    usize chosen = 0;
    for (usize index = 0; index < component_count; ++index) {
        selected[index] = settings.component == nullptr
                          || std::strcmp((*stxlib_ic::generated::components[index]).name, settings.component) == 0;
        if (selected[index]) chosen = chosen + 1;
    }
    if (chosen == 0) {
        std::fprintf(stderr, "stxlib_ic_run: no component is named %s\n", settings.component);
        return 2;
    }
    std::vector<Recorded> recorded{};
    std::vector<u8> bytes{};
    stxlib_ic::Transcript transcript{};
    u64 state = settings.seed;
    for (usize component = 0; component < component_count; ++component) {
        if (!selected[component]) continue;
        const stxlib_ic::Component& described = *stxlib_ic::generated::components[component];
        bool reported = false;
        for (u64 index = 0; index < settings.cases * 2; ++index) {
            const bool misuse = index >= settings.cases;
            make_case(bytes, component, misuse, settings.steps, state);
            stxlib_ic::Options options{};
            options.forks = settings.forks;
            options.maximum_steps = settings.steps;
            stxlib_ic::Finding finding{};
            if (!stxlib_ic::run_case(stxlib_ic::generated::components, component_count, bytes.data(), bytes.size(),
                                     options, counts.data(), finding)) {
                continue;
            }
            found[component] = found[component] + 1;
            if (reported) continue;
            // The first defect of each component: minimised, shown, kept.
            reported = true;
            const usize length = stxlib_ic::minimise(stxlib_ic::generated::components, component_count, bytes.data(),
                                                     bytes.size(), options);
            options.transcript = &transcript;
            stxlib_ic::Finding minimised{};
            static_cast<void>(stxlib_ic::run_case(stxlib_ic::generated::components, component_count, bytes.data(),
                                                  length, options, nullptr, minimised));
            const stxlib_ic::Finding& shown = minimised.found ? minimised : finding;
            std::printf("DEFECT %s\n", shown.what);
            print_transcript(transcript);
            Recorded record{};
            record.component = component;
            std::snprintf(record.block, sizeof(record.block), "%s", described.blocks[shown.block].name);
            std::snprintf(record.what, sizeof(record.what), "%s", shown.what);
            recorded.push_back(record);
            if (settings.emit != nullptr) {
                char name[160]{};
                std::snprintf(name, sizeof(name), "%s_%s", described.name, described.blocks[shown.block].name);
                for (usize at = 0; name[at] != '\0'; ++at) {
                    const char character = name[at];
                    const bool plain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                                       || (character >= '0' && character <= '9');
                    if (!plain) name[at] = '_';
                }
                char path[512]{};
                std::snprintf(path, sizeof(path), "%s/%s.cpp", settings.emit, name);
                if (!stxlib_ic::write_regression(path, described, bytes.data(), length, transcript, shown)) {
                    std::fprintf(stderr, "stxlib_ic_run: cannot write %s\n", path);
                }
            }
        }
        const stxlib_ic::IntegrityCounts& tally = counts[component];
        std::printf("%s: %llu cases, %llu steps: %llu completed, %llu declared failures, %llu refused, "
                    "%llu shown, %llu bailed, %llu skipped; blocks %u of %u, states %u of %u, edges %u; "
                    "defects %llu (legal %llu)\n",
                    described.name, static_cast<printed>(tally.cases),
                    static_cast<printed>(tally.steps), static_cast<printed>(tally.completed),
                    static_cast<printed>(tally.declared_failures),
                    static_cast<printed>(tally.refused_failure + tally.refused_value + tally.refused_stops),
                    static_cast<printed>(tally.shown_stops), static_cast<printed>(tally.bailed),
                    static_cast<printed>(tally.skipped), tally.blocks_run(described.block_count),
                    described.block_count, tally.state_count(), described.state_count, tally.edge_count(),
                    static_cast<printed>(found[component]),
                    static_cast<printed>(tally.legal_defects));
    }
    stxlib_ic::inputs::remove_scratch();
    if (settings.gate != nullptr
        && !write_gate(settings, counts.data(), found.data(), selected, recorded)) {
        std::fprintf(stderr, "stxlib_ic_run: cannot write %s\n", settings.gate);
        return 2;
    }
    u64 total = 0;
    for (usize index = 0; index < component_count; ++index) total = total + found[index];
    return total == 0 ? 0 : 1;
}
