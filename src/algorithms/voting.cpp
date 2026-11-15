// Preference Ranked Voting (include/stxlib/voting.hpp), ported step for
// step from stx_auxlib's src/voting.cpp at b6e330a, which ported the
// reference prv-core.mjs, with each distinct ballot held once with its
// multiplicity. Where the reference subtracts the one ballot being inferred
// from a positional count, this subtracts one copy, not the group's weight,
// so every count equals the reference's.
//
// Changes from stx_auxlib's implementation: no std::vector. Every working
// array is a Buffer in one Workspace allocated at the start of the count
// and reused: a set of rankings is a matrix with one row of
// candidate_count cells per group (GroupSet), and the per-call vectors of
// the original are rows of the workspace's tables. The order of every loop,
// comparison and tie rule is the original's. Two computations are
// restructured without changing their results: the expansion's ballot
// priorities are compared as they are computed (keeping the highest and
// recomputing for the equality pass) instead of being stored for every
// ballot of the level, and the margin signatures of the cycle test are
// sorted by insertion sort instead of std::sort.
//
// tests/reference/prv_reference.mjs runs the reference itself on random
// elections; tests/voting_tests.cpp compares every outcome.

#include "stxlib/voting.hpp"

#include <algorithm>
#include <functional>

namespace stxlib::voting {

namespace {

// Counts stay below 2^62, so doubling a count or forming a margin never
// overflows.
constexpr u64 largest_total = static_cast<u64>(1) << 62;

// Allocates `count` zeroed elements into `buffer`; false when it cannot.
template <typename Element>
bool allocate(Buffer<Element>& buffer, const usize count) noexcept {
    // A zero count still yields a usable (empty) buffer.
    Result<Buffer<Element>> made = Buffer<Element>::allocate_zeroed(count);
    if (made.failed()) return false;
    buffer = static_cast<Result<Buffer<Element>>&&>(made).take_value();
    return true;
}

bool product(const usize left, const usize right, usize& result) noexcept {
    const bool overflowed = __builtin_mul_overflow(left, right, &result);
    return !overflowed;
}

// Adds `count` elements of `size` bytes to `total`; false on overflow.
bool add_bytes(usize& total, const usize count, const usize size) noexcept {
    usize bytes = 0;
    if (!product(count, size, bytes)) return false;
    const bool overflowed = __builtin_add_overflow(total, bytes, &total);
    return !overflowed;
}

// Rankings, one row of `stride` cells per group: group g ranks
// cells[g * stride, g * stride + lengths[g]), with weight weights[g].
struct GroupSet final {
    Buffer<Candidate> cells{};
    Buffer<u32> lengths{};
    Buffer<u64> weights{};
    usize stride{0};
    usize count{0};

    bool allocate_for(const usize groups, const usize candidates) noexcept {
        usize cell_count = 0;
        if (!product(groups, candidates, cell_count)) return false;
        stride = candidates;
        count = 0;
        const bool result =
            allocate(cells, cell_count) && allocate(lengths, groups) && allocate(weights, groups);
        return result;
    }

    [[nodiscard]] Candidate* row(const usize group) noexcept { return cells.mutable_view().data() + group * stride; }
    [[nodiscard]] const Candidate* row(const usize group) const noexcept {
        return cells.view().data() + group * stride;
    }
    [[nodiscard]] usize length(const usize group) const noexcept { return lengths[group]; }
};

// The working storage of one count (voting.hpp, "Storage"). C is the
// candidate count, G the number of distinct ballots.
struct Workspace final {
    usize candidates{0};
    // The valid ballots as cast, then (after merging) the projections that
    // feed each bracket.
    GroupSet cast{};
    // The valid ballots, identical rankings merged.
    GroupSet valid{};
    // The expansion's ballots, completed in place.
    GroupSet states{};
    // The expanded ballots restricted to the remaining candidates.
    GroupSet projected{};
    // Positional counts: positions[c * size + r], C x C.
    Buffer<u64> positions{};
    // Per candidate (C).
    Buffer<u8> member{};
    Buffer<u8> in_pattern{};
    Buffer<Candidate> missing{};
    Buffer<u64> proposals{};
    Buffer<Candidate> tied{};
    Buffer<Candidate> remaining{};
    Buffer<u64> first{};
    Buffer<Candidate> order{};
    Buffer<Candidate> bracket{};
    Buffer<Candidate> active{};
    Buffer<u8> seen{};
    Buffer<usize> stack{};
    Buffer<i64> signature_a{};
    Buffer<i64> signature_b{};
    // Donors of one inference: rows of C (G x C), and per donor (G).
    Buffer<Candidate> donor_residual{};
    Buffer<u32> donor_residual_length{};
    Buffer<u32> donor_positions{};
    Buffer<u32> donor_positions_length{};
    Buffer<u64> donor_weight{};
    Buffer<usize> donor_group{};
    // Per group (G): the expansion's level and decisions, the merge order.
    Buffer<usize> level{};
    Buffer<usize> decision_group{};
    Buffer<Candidate> decision_candidate{};
    Buffer<usize> sorted{};
    // Candidate pairs (C x C): tie vectors, ballot priorities, margins.
    Buffer<u64> tie_vectors{};
    Buffer<u64> priority_current{};
    Buffer<u64> priority_best{};
    Buffer<i64> margins{};
    Buffer<u8> wins{};

    // The bytes allocate_for(groups, count) allocates, element for element
    // (the two are kept in step); false when the size is not representable.
    [[nodiscard]] static bool working_bytes(const usize groups, const usize count, usize& bytes) noexcept {
        usize square = 0;
        usize rows = 0;
        if (!product(count, count, square) || !product(groups, count, rows)) return false;
        // Per candidate: member, in_pattern, seen; missing, tied, remaining,
        // order, bracket, active; proposals, first; stack; the signatures.
        const usize per_candidate = 3 * sizeof(u8) + 6 * sizeof(Candidate) + 2 * sizeof(u64) + sizeof(usize) +
                                    2 * sizeof(i64);
        // Per group: four GroupSets' lengths and weights; the donors'
        // lengths, weight and group; level, decision_group, sorted and
        // decision_candidate.
        const usize per_group = 4 * (sizeof(u32) + sizeof(u64)) + 2 * sizeof(u32) + sizeof(u64) + sizeof(usize) +
                                3 * sizeof(usize) + sizeof(Candidate);
        // Per group and candidate: four GroupSets' cells, donor_residual and
        // donor_positions.
        const usize per_row = 4 * sizeof(Candidate) + sizeof(Candidate) + sizeof(u32);
        // Per candidate pair: positions, tie_vectors, the two priorities,
        // margins and wins.
        const usize per_pair = 4 * sizeof(u64) + sizeof(i64) + sizeof(u8);
        usize total = 0;
        const bool result = add_bytes(total, count, per_candidate) && add_bytes(total, groups, per_group) &&
                            add_bytes(total, rows, per_row) && add_bytes(total, square, per_pair);
        bytes = total;
        return result;
    }

    bool allocate_for(const usize groups, const usize count) noexcept {
        candidates = count;
        usize square = 0;
        usize rows = 0;
        if (!product(count, count, square) || !product(groups, count, rows)) return false;
        const bool sets = cast.allocate_for(groups, count) && valid.allocate_for(groups, count) &&
                          states.allocate_for(groups, count) && projected.allocate_for(groups, count);
        const bool per_candidate = allocate(member, count) && allocate(in_pattern, count) &&
                                   allocate(missing, count) && allocate(proposals, count) &&
                                   allocate(tied, count) && allocate(remaining, count) && allocate(first, count) &&
                                   allocate(order, count) && allocate(bracket, count) && allocate(active, count) &&
                                   allocate(seen, count) && allocate(stack, count) &&
                                   allocate(signature_a, count) && allocate(signature_b, count);
        const bool per_donor = allocate(donor_residual, rows) && allocate(donor_residual_length, groups) &&
                               allocate(donor_positions, rows) && allocate(donor_positions_length, groups) &&
                               allocate(donor_weight, groups) && allocate(donor_group, groups);
        const bool per_group = allocate(level, groups) && allocate(decision_group, groups) &&
                               allocate(decision_candidate, groups) && allocate(sorted, groups);
        const bool per_pair = allocate(positions, square) && allocate(tie_vectors, square) &&
                              allocate(priority_current, square) && allocate(priority_best, square) &&
                              allocate(margins, square) && allocate(wins, square);
        return sets && per_candidate && per_donor && per_group && per_pair;
    }
};

// The reference's compareVectors: lexicographic, missing entries as zero.
template <typename Number>
int compare_vectors(const Number* const left, const usize left_count, const Number* const right,
                    const usize right_count) noexcept {
    const usize length = left_count > right_count ? left_count : right_count;
    for (usize index = 0; index < length; ++index) {
        const Number a = index < left_count ? left[index] : 0;
        const Number b = index < right_count ? right[index] : 0;
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

u64 total_weight(const GroupSet& groups) noexcept {
    u64 total = 0;
    for (usize group = 0; group < groups.count; ++group) total += groups.weights[group];
    return total;
}

// positions[c * size + r]: the weight of ballots ranking candidate c at
// rank r, for ranks below `size`. Candidates are global ids below
// `count`.
void position_counts(const GroupSet& groups, const usize count, const usize size, Buffer<u64>& positions) noexcept {
    u64* const table = positions.mutable_view().data();
    for (usize cell = 0; cell < count * size; ++cell) table[cell] = 0;
    for (usize group = 0; group < groups.count; ++group) {
        const Candidate* const ranking = groups.row(group);
        const usize length = groups.length(group) < size ? groups.length(group) : size;
        for (usize rank = 0; rank < length; ++rank) table[ranking[rank] * size + rank] += groups.weights[group];
    }
}

// The candidates of `active` marked in `member` (count entries).
void membership(const Candidate* const active, const usize active_count, const usize count,
                Buffer<u8>& member) noexcept {
    for (usize candidate = 0; candidate < count; ++candidate) member[candidate] = 0;
    for (usize index = 0; index < active_count; ++index) member[active[index]] = 1;
}

// `target` := `source` with each ranking restricted to members.
void project(const GroupSet& source, const Buffer<u8>& member, GroupSet& target) noexcept {
    target.count = source.count;
    for (usize group = 0; group < source.count; ++group) {
        const Candidate* const from = source.row(group);
        Candidate* const to = target.row(group);
        u32 length = 0;
        for (usize rank = 0; rank < source.length(group); ++rank) {
            if (member[from[rank]] != 0) {
                to[length] = from[rank];
                ++length;
            }
        }
        target.lengths[group] = length;
        target.weights[group] = source.weights[group];
    }
}

// The reference's subsequenceMatch: where the pattern's candidates appear
// in the donor, in order, until one is missing. Returns how many.
u32 subsequence_positions(const Candidate* const pattern, const usize pattern_length, const Candidate* const donor,
                          const usize donor_length, u32* const positions) noexcept {
    u32 found = 0;
    usize start = 0;
    for (usize item = 0; item < pattern_length; ++item) {
        usize index = start;
        while (index < donor_length && donor[index] != pattern[item]) ++index;
        if (index == donor_length) break;
        positions[found] = static_cast<u32>(index);
        ++found;
        start = index + 1;
    }
    return found;
}

// The result of inferring one ballot's next candidate.
struct Inference final {
    bool inferred{false};
    Candidate candidate{0};
    PrvFailure failure{PrvFailure::no_expansion_evidence};
};

// The reference's inferOne, for the group at `target` of ws.states.
Inference infer_one(const usize target, const Candidate* const active, const usize active_count,
                    Workspace& ws) noexcept {
    const GroupSet& states = ws.states;
    const usize count = ws.candidates;
    const Candidate* const pattern = states.row(target);
    const usize pattern_length = states.length(target);
    for (usize candidate = 0; candidate < count; ++candidate) ws.in_pattern[candidate] = 0;
    for (usize rank = 0; rank < pattern_length; ++rank) ws.in_pattern[pattern[rank]] = 1;
    usize missing_count = 0;
    for (usize index = 0; index < active_count; ++index) {
        if (ws.in_pattern[active[index]] == 0) {
            ws.missing[missing_count] = active[index];
            ++missing_count;
        }
    }
    Inference result{};
    if (missing_count == 1) {
        result.inferred = true;
        result.candidate = ws.missing[0];
        return result;
    }

    Candidate* const residuals = ws.donor_residual.mutable_view().data();
    u32* const positions = ws.donor_positions.mutable_view().data();
    usize donors = 0;
    for (usize group = 0; group < states.count; ++group) {
        if (group == target) continue;
        const Candidate* const ranking = states.row(group);
        Candidate* const residual = residuals + donors * count;
        u32 residual_length = 0;
        for (usize rank = 0; rank < states.length(group); ++rank) {
            if (ws.in_pattern[ranking[rank]] == 0) {
                residual[residual_length] = ranking[rank];
                ++residual_length;
            }
        }
        if (residual_length == 0) continue;
        ws.donor_residual_length[donors] = residual_length;
        ws.donor_positions_length[donors] =
            subsequence_positions(pattern, pattern_length, ranking, states.length(group), positions + donors * count);
        ws.donor_weight[donors] = states.weights[group];
        ++donors;
    }
    if (donors == 0) {
        result.failure = PrvFailure::no_expansion_evidence;
        return result;
    }

    usize best_length = 0;
    for (usize donor = 0; donor < donors; ++donor) {
        if (ws.donor_positions_length[donor] > best_length) best_length = ws.donor_positions_length[donor];
    }
    // The first donor of the longest match whose positions are smallest.
    bool have_best = false;
    usize best_donor = 0;
    for (usize donor = 0; donor < donors; ++donor) {
        if (ws.donor_positions_length[donor] != best_length) continue;
        if (!have_best || compare_vectors(positions + donor * count, best_length, positions + best_donor * count,
                                          best_length) < 0) {
            best_donor = donor;
            have_best = true;
        }
    }
    // The group: every donor with the best match. Its members are listed in
    // donor_group[0, group_size).
    usize group_size = 0;
    for (usize donor = 0; donor < donors; ++donor) {
        if (ws.donor_positions_length[donor] == best_length &&
            compare_vectors(positions + donor * count, best_length, positions + best_donor * count, best_length) == 0) {
            ws.donor_group[group_size] = donor;
            ++group_size;
        }
    }

    // Proposals: the group's next choices, weighted.
    for (usize candidate = 0; candidate < count; ++candidate) ws.proposals[candidate] = 0;
    for (usize member = 0; member < group_size; ++member) {
        const usize donor = ws.donor_group[member];
        ws.proposals[residuals[donor * count]] += ws.donor_weight[donor];
    }
    u64 best_count = 0;
    for (usize index = 0; index < active_count; ++index) {
        if (ws.proposals[active[index]] > best_count) best_count = ws.proposals[active[index]];
    }
    usize tied_count = 0;
    for (usize index = 0; index < active_count; ++index) {
        if (ws.proposals[active[index]] == best_count && best_count > 0) {
            ws.tied[tied_count] = active[index];
            ++tied_count;
        }
    }
    if (tied_count == 1) {
        result.inferred = true;
        result.candidate = ws.tied[0];
        return result;
    }

    usize residual_length = 0;
    for (usize member = 0; member < group_size; ++member) {
        const usize length = ws.donor_residual_length[ws.donor_group[member]];
        if (length > residual_length) residual_length = length;
    }
    u64* const vectors = ws.tie_vectors.mutable_view().data();
    for (usize cell = 0; cell < tied_count * residual_length; ++cell) vectors[cell] = 0;
    for (usize index = 0; index < tied_count; ++index) {
        for (usize member = 0; member < group_size; ++member) {
            const usize donor = ws.donor_group[member];
            const Candidate* const residual = residuals + donor * count;
            for (usize rank = 0; rank < ws.donor_residual_length[donor]; ++rank) {
                if (residual[rank] == ws.tied[index]) vectors[index * residual_length + rank] += ws.donor_weight[donor];
            }
        }
    }
    usize best = 0;
    for (usize index = 1; index < tied_count; ++index) {
        if (compare_vectors(vectors + index * residual_length, residual_length, vectors + best * residual_length,
                            residual_length) > 0) {
            best = index;
        }
    }
    usize matching = 0;
    for (usize index = 0; index < tied_count; ++index) {
        if (compare_vectors(vectors + index * residual_length, residual_length, vectors + best * residual_length,
                            residual_length) == 0) {
            ++matching;
        }
    }
    if (matching != 1) {
        result.failure = PrvFailure::perfect_expansion_tie;
        return result;
    }
    result.inferred = true;
    result.candidate = ws.tied[best];
    return result;
}

// The reference's ballotPriority for one copy of group `group`: for each of
// its candidates in order, that candidate's positional vector without this
// copy. Written to `signature`; returns its length.
usize ballot_priority(const usize group, const GroupSet& states, const Buffer<u64>& positions, const usize size,
                      u64* const signature) noexcept {
    const Candidate* const ranking = states.row(group);
    const u64* const table = positions.view().data();
    usize length = 0;
    for (usize rank = 0; rank < states.length(group); ++rank) {
        const u64* const vector = table + ranking[rank] * size;
        for (usize column = 0; column < size; ++column) {
            signature[length] = vector[column] - (column == rank ? 1 : 0);
            ++length;
        }
    }
    return length;
}

struct Expansion final {
    bool expanded{false};
    PrvFailure failure{PrvFailure::no_expansion_evidence};
};

// The reference's expandBallots: completes `ballots`, restricted to
// `active`, into ws.states.
Expansion expand_ballots(const GroupSet& ballots, const Candidate* const active, const usize active_count,
                         Workspace& ws) noexcept {
    const usize count = ws.candidates;
    membership(active, active_count, count, ws.member);
    project(ballots, ws.member, ws.states);
    GroupSet& states = ws.states;
    const usize size = active_count;
    Expansion result{};
    u64* const current = ws.priority_current.mutable_view().data();
    u64* const best = ws.priority_best.mutable_view().data();
    for (;;) {
        usize maximum_length = 0;
        bool any_incomplete = false;
        for (usize group = 0; group < states.count; ++group) {
            if (states.length(group) < size) {
                any_incomplete = true;
                if (states.length(group) > maximum_length) maximum_length = states.length(group);
            }
        }
        if (!any_incomplete) {
            result.expanded = true;
            return result;
        }
        position_counts(states, count, size, ws.positions);
        usize level_count = 0;
        for (usize group = 0; group < states.count; ++group) {
            if (states.length(group) != maximum_length) continue;
            ws.level[level_count] = group;
            ++level_count;
        }
        // The highest priority, first of the level on ties (the original's
        // scan with a strict comparison).
        usize best_length = ballot_priority(ws.level[0], states, ws.positions, size, best);
        for (usize index = 1; index < level_count; ++index) {
            const usize length = ballot_priority(ws.level[index], states, ws.positions, size, current);
            if (compare_vectors(current, length, best, best_length) > 0) {
                for (usize cell = 0; cell < length; ++cell) best[cell] = current[cell];
                best_length = length;
            }
        }
        // Every member of the batch infers from the same snapshot.
        usize decisions = 0;
        for (usize index = 0; index < level_count; ++index) {
            const usize length = ballot_priority(ws.level[index], states, ws.positions, size, current);
            if (compare_vectors(current, length, best, best_length) != 0) continue;
            const Inference inferred = infer_one(ws.level[index], active, active_count, ws);
            if (!inferred.inferred) {
                result.failure = inferred.failure;
                return result;
            }
            ws.decision_group[decisions] = ws.level[index];
            ws.decision_candidate[decisions] = inferred.candidate;
            ++decisions;
        }
        for (usize decision = 0; decision < decisions; ++decision) {
            const usize group = ws.decision_group[decision];
            states.row(group)[states.length(group)] = ws.decision_candidate[decision];
            states.lengths[group] += 1;
        }
    }
}

usize index_of(const Candidate* const ranking, const usize length, const Candidate candidate) noexcept {
    usize index = 0;
    while (index < length && ranking[index] != candidate) ++index;
    return index;
}

// Sorts values[0, count) ascending (count is at most the candidate count).
void insertion_sort(i64* const values, const usize count) noexcept {
    for (usize index = 1; index < count; ++index) {
        const i64 held = values[index];
        usize position = index;
        while (position > 0 && values[position - 1] > held) {
            values[position] = values[position - 1];
            --position;
        }
        values[position] = held;
    }
}

// The reference's perfectCycle, over ws.projected: every candidate with the
// same positional vector, a pairwise-majority graph in which each reaches
// all, and the same sorted margins for each.
bool perfect_cycle(const Candidate* const remaining, const usize size, const u64 total, Workspace& ws) noexcept {
    if (size < 3) return false;
    const GroupSet& projected = ws.projected;
    position_counts(projected, ws.candidates, size, ws.positions);
    const u64* const table = ws.positions.view().data();
    for (usize index = 0; index < size; ++index) {
        if (compare_vectors(table + remaining[index] * size, size, table + remaining[0] * size, size) != 0) {
            return false;
        }
    }
    i64* const margins = ws.margins.mutable_view().data();
    u8* const wins = ws.wins.mutable_view().data();
    for (usize cell = 0; cell < size * size; ++cell) {
        margins[cell] = 0;
        wins[cell] = 0;
    }
    for (usize left = 0; left < size; ++left) {
        for (usize right = left + 1; right < size; ++right) {
            u64 left_votes = 0;
            for (usize group = 0; group < projected.count; ++group) {
                const Candidate* const ranking = projected.row(group);
                const usize length = projected.length(group);
                if (index_of(ranking, length, remaining[left]) < index_of(ranking, length, remaining[right])) {
                    left_votes += projected.weights[group];
                }
            }
            const i64 margin = static_cast<i64>(left_votes) - static_cast<i64>(total - left_votes);
            margins[left * size + right] = margin;
            margins[right * size + left] = -margin;
            if (margin > 0) wins[left * size + right] = 1;
            if (margin < 0) wins[right * size + left] = 1;
        }
    }
    for (usize start = 0; start < size; ++start) {
        for (usize node = 0; node < size; ++node) ws.seen[node] = 0;
        usize stack_count = 1;
        ws.stack[0] = start;
        ws.seen[start] = 1;
        usize reached = 1;
        while (stack_count > 0) {
            --stack_count;
            const usize from = ws.stack[stack_count];
            for (usize to = 0; to < size; ++to) {
                if (wins[from * size + to] != 0 && ws.seen[to] == 0) {
                    ws.seen[to] = 1;
                    ++reached;
                    // Each node is pushed once (when first seen), so the
                    // stack never holds more than `size` entries.
                    ws.stack[stack_count] = to;
                    ++stack_count;
                }
            }
        }
        if (reached != size) return false;
    }
    i64* const first_signature = ws.signature_a.mutable_view().data();
    i64* const signature = ws.signature_b.mutable_view().data();
    for (usize candidate = 0; candidate < size; ++candidate) {
        i64* const target = candidate == 0 ? first_signature : signature;
        usize length = 0;
        for (usize other = 0; other < size; ++other) {
            if (other != candidate) {
                target[length] = margins[candidate * size + other];
                ++length;
            }
        }
        insertion_sort(target, length);
        if (candidate == 0) continue;
        for (usize index = 0; index < length; ++index) {
            if (signature[index] != first_signature[index]) return false;
        }
    }
    return true;
}

struct Elimination final {
    bool won{false};
    Candidate winner{0};
    PrvFailure failure{PrvFailure::perfect_final_tie};
};

// The reference's eliminate, over the complete expanded ballots in
// ws.states.
Elimination eliminate(const Candidate* const active, const usize active_count, Workspace& ws) noexcept {
    const usize count = ws.candidates;
    Candidate* const remaining = ws.remaining.mutable_view().data();
    usize remaining_count = active_count;
    for (usize index = 0; index < active_count; ++index) remaining[index] = active[index];
    const u64 total = total_weight(ws.states);
    Elimination result{};
    for (;;) {
        membership(remaining, remaining_count, count, ws.member);
        project(ws.states, ws.member, ws.projected);
        const GroupSet& projected = ws.projected;
        for (usize candidate = 0; candidate < count; ++candidate) ws.first[candidate] = 0;
        for (usize group = 0; group < projected.count; ++group) {
            if (projected.length(group) != 0) ws.first[projected.row(group)[0]] += projected.weights[group];
        }
        if (perfect_cycle(remaining, remaining_count, total, ws)) {
            result.failure = PrvFailure::perfect_condorcet_cycle;
            return result;
        }
        for (usize index = 0; index < remaining_count; ++index) {
            if (2 * ws.first[remaining[index]] > total) {
                result.won = true;
                result.winner = remaining[index];
                return result;
            }
        }
        if (remaining_count == 2) {
            if (ws.first[remaining[0]] == ws.first[remaining[1]]) {
                result.failure = PrvFailure::perfect_final_tie;
                return result;
            }
            result.won = true;
            result.winner = ws.first[remaining[0]] > ws.first[remaining[1]] ? remaining[0] : remaining[1];
            return result;
        }
        position_counts(projected, count, remaining_count, ws.positions);
        const u64* const table = ws.positions.view().data();
        const usize size = remaining_count;
        usize lowest = 0;
        for (usize index = 1; index < remaining_count; ++index) {
            if (compare_vectors(table + remaining[index] * size, size, table + remaining[lowest] * size, size) < 0) {
                lowest = index;
            }
        }
        usize matching = 0;
        for (usize index = 0; index < remaining_count; ++index) {
            if (compare_vectors(table + remaining[index] * size, size, table + remaining[lowest] * size, size) == 0) {
                ++matching;
            }
        }
        if (matching != 1) {
            result.failure = PrvFailure::perfect_elimination_tie;
            return result;
        }
        for (usize index = lowest; index + 1 < remaining_count; ++index) remaining[index] = remaining[index + 1];
        --remaining_count;
    }
}

Elimination run_mini(const GroupSet& ballots, const Candidate* const active, const usize active_count,
                     Workspace& ws) noexcept {
    const Expansion expansion = expand_ballots(ballots, active, active_count, ws);
    if (!expansion.expanded) {
        Elimination failed{};
        failed.failure = expansion.failure;
        return failed;
    }
    const Elimination result = eliminate(active, active_count, ws);
    return result;
}

// The sign of sum(d[r] * 2^(n - 1 - r)): the reference's BigInt seed score
// difference, by carrying from the least significant rank.
int score_sign(const u64* const left, const u64* const right, const usize length) noexcept {
    i64 carry = 0;
    bool any_residue = false;
    for (usize index = length; index > 0; --index) {
        const i64 difference = static_cast<i64>(left[index - 1]) - static_cast<i64>(right[index - 1]);
        const i64 total = difference + carry;
        // Floor division by 2, so the residue is 0 or 1.
        i64 quotient = total / 2;
        if (total < 0 && total % 2 != 0) --quotient;
        if (total - 2 * quotient != 0) any_residue = true;
        carry = quotient;
    }
    if (carry > 0) return 1;
    if (carry < 0) return -1;
    return any_residue ? 1 : 0;
}

// Orders candidates by descending seed score, then descending vector, over
// a C x C positional table.
struct SeedOrder final {
    const u64* table;
    usize size;

    bool operator()(const Candidate left, const Candidate right) const noexcept {
        const u64* const a = table + left * size;
        const u64* const b = table + right * size;
        const int sign = score_sign(a, b, size);
        if (sign != 0) return sign > 0;
        return compare_vectors(a, size, b, size) > 0;
    }
};

// Orders group indices by their rankings, as std::vector's operator<:
// lexicographic, a proper prefix first.
struct ByRanking final {
    const GroupSet* groups;

    bool operator()(const usize left, const usize right) const noexcept {
        const Candidate* const a = (*groups).row(left);
        const Candidate* const b = (*groups).row(right);
        const usize a_length = (*groups).length(left);
        const usize b_length = (*groups).length(right);
        const usize shorter = a_length < b_length ? a_length : b_length;
        for (usize index = 0; index < shorter; ++index) {
            if (a[index] != b[index]) return a[index] < b[index];
        }
        return a_length < b_length;
    }
};

bool same_ranking(const GroupSet& left_set, const usize left, const GroupSet& right_set, const usize right) noexcept {
    if (left_set.length(left) != right_set.length(right)) return false;
    const Candidate* const a = left_set.row(left);
    const Candidate* const b = right_set.row(right);
    for (usize index = 0; index < left_set.length(left); ++index) {
        if (a[index] != b[index]) return false;
    }
    return true;
}

// ws.valid := ws.cast sorted by ranking, identical rankings merged.
void merge_identical(Workspace& ws) noexcept {
    const GroupSet& cast = ws.cast;
    GroupSet& valid = ws.valid;
    usize* const order = ws.sorted.mutable_view().data();
    for (usize group = 0; group < cast.count; ++group) order[group] = group;
    std::sort(order, order + cast.count, ByRanking{&cast});
    valid.count = 0;
    for (usize position = 0; position < cast.count; ++position) {
        const usize group = order[position];
        if (valid.count != 0 && same_ranking(valid, valid.count - 1, cast, group)) {
            valid.weights[valid.count - 1] += cast.weights[group];
            continue;
        }
        const Candidate* const from = cast.row(group);
        Candidate* const to = valid.row(valid.count);
        for (usize rank = 0; rank < cast.length(group); ++rank) to[rank] = from[rank];
        valid.lengths[valid.count] = cast.lengths[group];
        valid.weights[valid.count] = cast.weights[group];
        ++valid.count;
    }
}

// Grows `buffer` (holding `used` elements) to hold at least `wanted`.
template <typename Element>
Status grow(Buffer<Element>& buffer, const usize used, const usize wanted) noexcept {
    if (wanted <= buffer.count()) return Status::success();
    usize capacity = buffer.count() < 16 ? 16 : buffer.count();
    while (capacity < wanted) {
        if (capacity > usize_max / 2) {
            capacity = wanted;
            break;
        }
        capacity = capacity * 2;
    }
    Result<Buffer<Element>> grown = Buffer<Element>::allocate_zeroed(capacity);
    if (grown.failed()) return Status::failure(*grown.error());
    Buffer<Element>& larger = *grown.value();
    for (usize index = 0; index < used; ++index) larger[index] = buffer[index];
    buffer = static_cast<Buffer<Element>&&>(larger);
    return Status::success();
}

} // namespace

Status Ballots::add(const View<Candidate> ranking) noexcept {
    const Status result = add_copies(ranking, 1);
    return result;
}

Status Ballots::add_copies(const View<Candidate> ranking, const u64 copies) noexcept {
    if (ranking.count() > usize_max - choice_count_ || distinct_ == usize_max) {
        return Status::failure(ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.voting.ballots.add"));
    }
    // A ranking that is a view of these ballots (ranking(k)) points into
    // choices_, which the growth below may replace and release: it is found
    // by address first and read from the grown buffer at the same offset
    // (audit 2026-11-15, A-stxlib-algorithms-02-F5). std::less orders any
    // two pointers.
    const Candidate* const stored = choices_.view().data();
    const std::less<const Candidate*> before{};
    const bool inside = ranking.count() > 0 && stored != nullptr && !before(ranking.data(), stored) &&
                        before(ranking.data(), stored + choice_count_);
    const usize offset = inside ? static_cast<usize>(ranking.data() - stored) : 0;
    // Both growths happen before anything is written, so a failure leaves
    // the ballots unchanged.
    const Status choices = grow(choices_, choice_count_, choice_count_ + ranking.count());
    if (choices.failed()) return choices;
    const Status ends = grow(ends_, distinct_, distinct_ + 1);
    if (ends.failed()) return ends;
    const Status counts = grow(copies_, distinct_, distinct_ + 1);
    if (counts.failed()) return counts;
    const Candidate* const source = inside ? choices_.view().data() + offset : ranking.data();
    for (usize index = 0; index < ranking.count(); ++index) choices_[choice_count_ + index] = source[index];
    choice_count_ += ranking.count();
    ends_[distinct_] = choice_count_;
    copies_[distinct_] = copies;
    ++distinct_;
    return Status::success();
}

View<Candidate> Ballots::ranking(const usize index) const& noexcept {
    STXLIB_REQUIRE(index < distinct_, "voting::Ballots::ranking: index out of range");
    const usize first = index == 0 ? 0 : ends_[index - 1];
    const View<Candidate> all = choices_.view();
    const View<Candidate> result = all.from(first).first(ends_[index] - first);
    return result;
}

u64 Ballots::copies(const usize index) const noexcept {
    STXLIB_REQUIRE(index < distinct_, "voting::Ballots::copies: index out of range");
    return copies_[index];
}

void Ballots::clear() noexcept {
    choice_count_ = 0;
    distinct_ = 0;
}

PrvOutcome count_preference_ranked(const u32 candidate_count, const Ballots& ballots,
                                   const PrvOptions& options) noexcept {
    if (candidate_count < 2) return PrvOutcome::failed(PrvFailure::too_few_candidates, 0);
    const usize count = candidate_count;
    const usize groups = ballots.distinct() == 0 ? 1 : ballots.distinct();
    usize working = 0;
    if (!Workspace::working_bytes(groups, count, working) || working > options.max_working_bytes) {
        return PrvOutcome::failed(PrvFailure::working_storage_limit, 0);
    }
    Workspace ws{};
    if (!ws.allocate_for(groups, count)) return PrvOutcome::failed(PrvFailure::out_of_memory, 0);

    // Validation, weighted; totals are checked against largest_total.
    u64 rejected = 0;
    u64 accepted = 0;
    GroupSet& cast = ws.cast;
    cast.count = 0;
    for (usize index = 0; index < ballots.distinct(); ++index) {
        const View<Candidate> ranking = ballots.ranking(index);
        const u64 copies = ballots.copies(index);
        if (copies == 0) continue;
        bool ok = !ranking.empty();
        for (usize rank = 0; rank < ranking.count() && ok; ++rank) {
            const Candidate candidate = ranking[rank];
            if (candidate >= count || ws.seen[candidate] != 0) {
                ok = false;
            } else {
                ws.seen[candidate] = 1;
            }
        }
        for (usize rank = 0; rank < ranking.count(); ++rank) {
            if (ranking[rank] < count) ws.seen[ranking[rank]] = 0;
        }
        u64& tally = ok ? accepted : rejected;
        if (copies > largest_total - tally) return PrvOutcome::failed(PrvFailure::too_many_ballots, rejected);
        tally += copies;
        if (ok) {
            Candidate* const row = cast.row(cast.count);
            for (usize rank = 0; rank < ranking.count(); ++rank) row[rank] = ranking[rank];
            cast.lengths[cast.count] = static_cast<u32>(ranking.count());
            cast.weights[cast.count] = copies;
            ++cast.count;
        }
    }
    if (accepted <= count) return PrvOutcome::failed(PrvFailure::too_few_valid_ballots, rejected);
    merge_identical(ws);

    Candidate* const active = ws.active.mutable_view().data();
    for (usize index = 0; index < count; ++index) active[index] = static_cast<Candidate>(index);
    if (count <= options.bracket_threshold) {
        const Elimination result = run_mini(ws.valid, active, count, ws);
        if (!result.won) return PrvOutcome::failed(result.failure, rejected);
        return PrvOutcome::won(result.winner, rejected);
    }
    if (options.bracket_interval < 2) return PrvOutcome::failed(PrvFailure::invalid_bracket_interval, rejected);

    // Seeding over the valid ballots as cast.
    position_counts(ws.valid, count, count, ws.positions);
    const u64* const table = ws.positions.view().data();
    for (usize left = 0; left < count; ++left) {
        for (usize right = left + 1; right < count; ++right) {
            if (compare_vectors(table + left * count, count, table + right * count, count) == 0) {
                return PrvOutcome::failed(PrvFailure::perfect_seed_tie, rejected);
            }
        }
    }
    Candidate* const order = ws.order.mutable_view().data();
    for (usize index = 0; index < count; ++index) order[index] = static_cast<Candidate>(index);
    std::sort(order, order + count, SeedOrder{table, count});
    // Brackets: segments of `width` seeds, counted from the last segment
    // (the lowest seeds) to the first. The width is clamped to count: an
    // interval of count + 1 or more means one bracket of every candidate,
    // and an unclamped width near usize_max wrapped count + width - 1 to a
    // small value, so segments was 0 and candidate 0 won uncounted (audit
    // 2026-11-15, A-stxlib-algorithms-02-F1). Results for every interval that
    // did not wrap are unchanged (a width of count or more gives one segment
    // either way).
    const usize interval_width = options.bracket_interval - 1;
    const usize width = interval_width < count ? interval_width : count;
    const usize segments = (count + width - 1) / width;
    Candidate* const bracket = ws.bracket.mutable_view().data();
    bool have_promoted = false;
    Candidate promoted = 0;
    for (usize segment = segments; segment > 0; --segment) {
        const usize start = (segment - 1) * width;
        const usize end = start + width < count ? start + width : count;
        usize bracket_count = 0;
        for (usize index = start; index < end; ++index) {
            bracket[bracket_count] = order[index];
            ++bracket_count;
        }
        // A bracket holds at most count candidates: with one segment, its
        // seeds are all count candidates and nothing is promoted yet; with
        // more, width < count, so width seeds and the promoted one fit.
        if (have_promoted) {
            bracket[bracket_count] = promoted;
            ++bracket_count;
        }
        if (bracket_count == 1) {
            promoted = bracket[0];
            have_promoted = true;
            continue;
        }
        membership(bracket, bracket_count, count, ws.member);
        project(ws.valid, ws.member, ws.cast);
        const Elimination result = run_mini(ws.cast, bracket, bracket_count, ws);
        if (!result.won) return PrvOutcome::failed(result.failure, rejected);
        promoted = result.winner;
        have_promoted = true;
    }
    return PrvOutcome::won(promoted, rejected);
}

} // namespace stxlib::voting
