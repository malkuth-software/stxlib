#pragma once

// Preference Ranked Voting (PRV 1.0), the counting rule of the owner's
// "Preference Ranked Voting" design (paymenottowork.com, ideas/
// preference-ranked-voting/prv-core.mjs), for any choice among ranked
// options: elections, polls, feature triage, tournament seeding. Ported
// from stx_auxlib's include/stx_auxlib/voting.hpp at b6e330a: the rule and
// its outcomes are unchanged (tests/voting_tests.cpp checks 420 elections
// against the reference's own outcomes). The interface is restated in
// STXlib's vocabulary, and every working array is on STXlib's allocator
// (stx_auxlib used std::vector); see "Changes from stx_auxlib" below.
//
// The rule, in the reference's order:
// 1. Validation: a ballot is valid when it ranks at least one candidate,
//    names each at most once and names only real candidates; invalid ones
//    are counted and set aside. More valid ballots than candidates are
//    required.
// 2. Expansion: incomplete ballots are completed one rank at a time, the
//    longest first, by inference from other ballots that agree with them
//    (the donors whose rankings best match the ballot's own order propose
//    their next choice; ties go to the proposal ranked earliest most
//    often). Exhausted ballots therefore keep counting.
// 3. Elimination: rounds of first-preference counts; a majority wins; the
//    candidate with the lowest positional vector (first-rank count, then
//    second, ...) is eliminated; two left are decided by first preferences.
// 4. Bracketing: with more than bracket_threshold candidates, they are
//    seeded by positional score and counted in brackets of
//    bracket_interval - 1 seeds plus the previous bracket's winner, from the
//    lowest seeds up.
// Ties the rule cannot break deterministically, and perfect Condorcet
// cycles, are reported as named failures; the rule never resolves by lot.
//
// This implementation counts each distinct ballot once with its
// multiplicity. Identical ballots infer identically at every step (a
// ballot is never its own donor), so the result is exactly the
// reference's, at a cost that grows with the number of distinct rankings
// rather than the number of voters.
//
// Storage: count_preference_ranked allocates its working storage once, at
// the start, sized by the candidate count C and the number of distinct
// ballots D: about 24 * C * D + 48 * C * C bytes (rankings and donors per
// distinct ballot; positional, tie and margin tables per candidate pair).
// The exact size is computed, with checked arithmetic, before anything is
// allocated. Above PrvOptions::max_working_bytes, or when it is not
// representable, the count fails with PrvFailure::working_storage_limit;
// when it cannot be had, with PrvFailure::out_of_memory. Nothing else
// happens in either case.
//
// Changes from stx_auxlib: Ballots stores its rankings in STXlib Buffers
// and reports a failed growth (add returns Status); the outcome is a tagged
// union laid out here; PrvFailure::out_of_memory (new) reports the working
// storage; rankings are View<Candidate>.

#include "stxlib/buffer.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::voting {

// Candidates are 0 to candidate_count - 1; names belong to the caller.
using Candidate = u32;

// Ballots in one flat buffer: ballot i is choices[ends[i - 1], ends[i]),
// with its number of copies. Move-only.
class Ballots final {
public:
    Ballots() noexcept = default;
    Ballots(Ballots&&) noexcept = default;
    Ballots& operator=(Ballots&&) noexcept = default;
    Ballots(const Ballots&) = delete;
    Ballots& operator=(const Ballots&) = delete;
    ~Ballots() = default;

    // Adds a ballot, most preferred first. It is validated when counted.
    // Failures: out_of_memory or limit_exceeded when the storage cannot
    // grow; the ballots are then unchanged.
    [[nodiscard]] Status add(View<Candidate> ranking) noexcept;
    // Adds `copies` identical ballots without storing each. Failures as add.
    [[nodiscard]] Status add_copies(View<Candidate> ranking, u64 copies) noexcept;
    [[nodiscard]] usize distinct() const noexcept { return distinct_; }
    // Borrowed from these ballots: valid until the next add or clear.
    // Requirement: index < distinct().
    [[nodiscard]] View<Candidate> ranking(usize index) const& noexcept;
    View<Candidate> ranking(usize index) const&& = delete;
    // Requirement: index < distinct().
    [[nodiscard]] u64 copies(usize index) const noexcept;
    // Forgets every ballot; the storage stays for reuse.
    void clear() noexcept;

private:
    Buffer<Candidate> choices_{};
    usize choice_count_{0};
    // ends_[i]: the end of ballot i in choices_.
    Buffer<usize> ends_{};
    Buffer<u64> copies_{};
    usize distinct_{0};
};

struct PrvOptions final {
    usize bracket_threshold{30};
    usize bracket_interval{10};
    // The most working storage one count may allocate (see "Storage"
    // above); 1 GiB by default, which holds, for example, 1,000 candidates
    // with 30,000 distinct ballots.
    usize max_working_bytes{static_cast<usize>(1) << 30};
};

enum class PrvFailure : u8 {
    too_few_candidates = 1,
    too_few_valid_ballots = 2,
    invalid_bracket_interval = 3,
    // An incomplete ballot has no donor that ranks anything it lacks.
    no_expansion_evidence = 4,
    perfect_expansion_tie = 5,
    perfect_condorcet_cycle = 6,
    perfect_final_tie = 7,
    perfect_elimination_tie = 8,
    perfect_seed_tie = 9,
    // A count exceeded 2^62 ballots.
    too_many_ballots = 10,
    // The working storage could not be allocated (see "Storage" above).
    out_of_memory = 11,
    // The working storage would exceed PrvOptions::max_working_bytes, or
    // its size is not representable (see "Storage" above).
    working_storage_limit = 12,
};

// The count's result: the winner, or why there is none. Never both. The
// rejected count is part of both states.
class PrvOutcome final {
public:
    [[nodiscard]] static PrvOutcome won(const Candidate winner_candidate, const u64 rejected) noexcept {
        const PrvOutcome result(winner_candidate, rejected);
        return result;
    }
    [[nodiscard]] static PrvOutcome failed(const PrvFailure count_failure, const u64 rejected) noexcept {
        const PrvOutcome result(count_failure, rejected);
        return result;
    }
    // Borrowed from this outcome; null when the count failed.
    [[nodiscard]] const Candidate* winner() const& noexcept {
        if (outcome_ != Outcome::won) return nullptr;
        return &state_.winner;
    }
    const Candidate* winner() const&& = delete;
    // Borrowed from this outcome; null when there is a winner.
    [[nodiscard]] const PrvFailure* failure() const& noexcept {
        if (outcome_ != Outcome::failed) return nullptr;
        return &state_.failure;
    }
    const PrvFailure* failure() const&& = delete;
    // Invalid ballots set aside in validation.
    [[nodiscard]] u64 rejected_ballots() const noexcept { return rejected_; }

private:
    enum class Outcome : u8 {
        won = 0,
        failed = 1,
    };
    union State {
        Candidate winner;
        PrvFailure failure;

        explicit constexpr State(const Candidate winner_candidate) noexcept : winner(winner_candidate) {}
        explicit constexpr State(const PrvFailure count_failure) noexcept : failure(count_failure) {}
    };

    PrvOutcome(const Candidate winner_candidate, const u64 rejected) noexcept
        : outcome_(Outcome::won), state_(winner_candidate), rejected_(rejected) {}
    PrvOutcome(const PrvFailure count_failure, const u64 rejected) noexcept
        : outcome_(Outcome::failed), state_(count_failure), rejected_(rejected) {}

    Outcome outcome_;
    State state_;
    u64 rejected_{0};
};

// Counts `ballots` over candidates 0 to candidate_count - 1.
[[nodiscard]] PrvOutcome count_preference_ranked(u32 candidate_count, const Ballots& ballots,
                                                 const PrvOptions& options = {}) noexcept;

} // namespace stxlib::voting
