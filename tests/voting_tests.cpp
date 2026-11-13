// Tests of Preference Ranked Voting (include/stxlib/voting.hpp) against
// the reference itself: tests/reference/prv_cases.txt holds 420 elections
// and the outcomes the original prv-core.mjs gives them
// (tests/reference/prv_reference.mjs; regenerated with node on 2026-11-13,
// byte for byte the same file). Every outcome must match. Ported from
// stx_auxlib's tests/voting_tests.cpp at b6e330a; the 420 cases are a
// sample of elections, not a proof of the rule's equivalence.
//   stxlib_voting_tests <path to prv_cases.txt>

#include "stxlib/files.hpp"
#include "stxlib/numeric_text.hpp"
#include "stxlib/path.hpp"
#include "stxlib/text.hpp"
#include "stxlib/voting.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace voting = stxlib::voting;
namespace text = stxlib::text;
using stxlib::TextView;
using stxlib::u64;
using stxlib::usize;

const char* failure_name(const voting::PrvFailure failure) {
    switch (failure) {
    case voting::PrvFailure::too_few_candidates:
        return "TOO_FEW_CANDIDATES";
    case voting::PrvFailure::too_few_valid_ballots:
        return "TOO_FEW_VALID_BALLOTS";
    case voting::PrvFailure::invalid_bracket_interval:
        return "INVALID_BRACKET_INTERVAL";
    case voting::PrvFailure::no_expansion_evidence:
        return "NO_EXPANSION_EVIDENCE";
    case voting::PrvFailure::perfect_expansion_tie:
        return "PERFECT_EXPANSION_TIE";
    case voting::PrvFailure::perfect_condorcet_cycle:
        return "PERFECT_CONDORCET_CYCLE";
    case voting::PrvFailure::perfect_final_tie:
        return "PERFECT_FINAL_TIE";
    case voting::PrvFailure::perfect_elimination_tie:
        return "PERFECT_ELIMINATION_TIE";
    case voting::PrvFailure::perfect_seed_tie:
        return "PERFECT_SEED_TIE";
    case voting::PrvFailure::too_many_ballots:
        return "TOO_MANY_BALLOTS";
    case voting::PrvFailure::out_of_memory:
        return "OUT_OF_MEMORY";
    }
    return "UNKNOWN";
}

std::string outcome_text(const voting::PrvOutcome& outcome) {
    if (outcome.winner() != nullptr) return "winner " + std::to_string(*outcome.winner());
    std::string result = "failure ";
    result += failure_name(*outcome.failure());
    return result;
}

std::string string_of(const TextView view) {
    const std::string result(view.data(), view.count());
    return result;
}

bool add_ballot(voting::Ballots& ballots, const std::vector<voting::Candidate>& ranking, const u64 copies) {
    const stxlib::View<voting::Candidate> view(ranking.data(), ranking.size());
    const stxlib::Status added = ballots.add_copies(view, copies);
    return added.succeeded();
}

// One case line: "C threshold interval | b;b;... | outcome". False, with a
// message, when the line does not parse or its outcome differs.
bool check_case(const TextView line, const usize number) {
    text::TextScanner scanner(line);
    u64 candidates = 0;
    u64 threshold = 0;
    u64 interval = 0;
    const bool header = scanner.read_unsigned(candidates) && scanner.expect(" ") && scanner.read_unsigned(threshold) &&
                        scanner.expect(" ") && scanner.read_unsigned(interval) && scanner.expect(" | ");
    TextView ballots_text{};
    const bool body = header && scanner.read_until("|", ballots_text) && scanner.expect("| ");
    if (!body) {
        std::fprintf(stderr, "voting: case line %zu does not parse\n", number);
        return false;
    }
    const std::string expected = string_of(scanner.rest());

    voting::Ballots ballots{};
    text::TextScanner ballot_scanner(text::trim(ballots_text));
    std::vector<voting::Candidate> ranking{};
    while (!ballot_scanner.at_end()) {
        ranking.clear();
        if (!ballot_scanner.expect("-")) {
            u64 choice = 0;
            while (ballot_scanner.read_unsigned(choice)) {
                ranking.push_back(static_cast<voting::Candidate>(choice));
                if (!ballot_scanner.expect(" ")) break;
            }
        }
        if (!add_ballot(ballots, ranking, 1)) {
            std::fprintf(stderr, "voting: case line %zu: a ballot could not be stored\n", number);
            return false;
        }
        if (!ballot_scanner.expect(";")) break;
    }
    if (!ballot_scanner.at_end()) {
        std::fprintf(stderr, "voting: case line %zu: trailing ballot text\n", number);
        return false;
    }

    voting::PrvOptions options{};
    options.bracket_threshold = static_cast<usize>(threshold);
    options.bracket_interval = static_cast<usize>(interval);
    const voting::PrvOutcome outcome =
        voting::count_preference_ranked(static_cast<stxlib::u32>(candidates), ballots, options);
    const std::string actual = outcome_text(outcome);
    if (actual != expected) {
        std::fprintf(stderr, "voting: case line %zu: got \"%s\", reference \"%s\"\n", number, actual.c_str(),
                     expected.c_str());
        return false;
    }
    return true;
}

void reference_cases(const char* const path_text) {
    const stxlib::Result<stxlib::Path> path = stxlib::Path::parse_bytes(path_text, std::strlen(path_text));
    CHECK(path.succeeded());
    if (!path.succeeded()) return;
    const stxlib::Result<stxlib::Buffer<stxlib::Byte>> bytes = stxlib::read_file(*path.value(), 16 << 20);
    CHECK(bytes.succeeded());
    if (!bytes.succeeded()) return;
    const stxlib::View<stxlib::Byte> content_bytes = (*bytes.value()).view();
    const stxlib::Result<TextView> content =
        stxlib::validate_utf8(reinterpret_cast<const char*>(content_bytes.data()), content_bytes.count());
    CHECK(content.succeeded());
    if (!content.succeeded()) return;
    text::TextScanner lines(*content.value());
    usize number = 0;
    usize checked = 0;
    bool all_match = true;
    while (!lines.at_end()) {
        TextView line{};
        ++number;
        if (lines.read_until("\n", line)) {
            const TextView trimmed = text::trim(line);
            if (!trimmed.empty()) {
                all_match = check_case(trimmed, number) && all_match;
                ++checked;
            }
        }
        static_cast<void>(lines.expect("\n"));
    }
    CHECK(all_match);
    CHECK(checked >= 420);
}

// Weighted copies give the same outcome as the same ballots added singly,
// and a large weighted electorate counts quickly.
void weighted_ballots() {
    const voting::Candidate orders[4][5] = {{0, 1, 2, 3, 4}, {1, 0, 2, 4, 3}, {2, 3, 1, 0, 4}, {4, 2, 1, 0, 3}};
    const u64 weights[4] = {7, 6, 5, 4};
    voting::Ballots single{};
    voting::Ballots weighted{};
    bool stored = true;
    for (usize bloc = 0; bloc < 4; ++bloc) {
        const usize length = 2 + bloc;
        const std::vector<voting::Candidate> ranking(orders[bloc], orders[bloc] + length);
        for (u64 copy = 0; copy < weights[bloc]; ++copy) stored = add_ballot(single, ranking, 1) && stored;
        stored = add_ballot(weighted, ranking, weights[bloc]) && stored;
    }
    CHECK(stored);
    const voting::PrvOutcome a = voting::count_preference_ranked(5, single);
    const voting::PrvOutcome b = voting::count_preference_ranked(5, weighted);
    CHECK(outcome_text(a) == outcome_text(b));

    // Storage: a ballot's view reads back what was added; clear forgets.
    CHECK(single.distinct() == 22 && weighted.distinct() == 4);
    const stxlib::View<voting::Candidate> third = weighted.ranking(2);
    CHECK(third.count() == 4 && third[0] == 2 && third[3] == 0 && weighted.copies(2) == 5);
    weighted.clear();
    CHECK(weighted.distinct() == 0);

    voting::Ballots large{};
    u64 state = 12345;
    std::vector<voting::Candidate> ranking{};
    for (usize distinct = 0; distinct < 400; ++distinct) {
        ranking = {0, 1, 2, 3, 4, 5, 6, 7};
        for (usize index = ranking.size() - 1; index > 0; --index) {
            state = (state ^ (state << 13)) ^ ((state ^ (state << 13)) >> 7);
            state ^= state << 17;
            const usize other = static_cast<usize>(state % (index + 1));
            const voting::Candidate held = ranking[index];
            ranking[index] = ranking[other];
            ranking[other] = held;
        }
        ranking.resize(1 + static_cast<usize>(state % 8));
        stored = add_ballot(large, ranking, 1000 + state % 9000) && stored;
    }
    CHECK(stored);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const voting::PrvOutcome outcome = voting::count_preference_ranked(8, large);
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("400 distinct rankings, about two million voters: %s in %.1f ms\n", outcome_text(outcome).c_str(),
                elapsed.count());

    // Too few candidates, and an empty electorate.
    voting::Ballots none{};
    CHECK(outcome_text(voting::count_preference_ranked(1, none)) == "failure TOO_FEW_CANDIDATES");
    CHECK(outcome_text(voting::count_preference_ranked(3, none)) == "failure TOO_FEW_VALID_BALLOTS");
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count != 2) {
        std::fprintf(stderr, "usage: stxlib_voting_tests <prv_cases.txt>\n");
        return 2;
    }
    reference_cases(arguments[1]);
    weighted_ballots();
    return stxlib_tests::finish("stxlib voting");
}
