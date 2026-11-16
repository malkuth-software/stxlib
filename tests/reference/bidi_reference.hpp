#pragma once

// An independent reading of UAX #9 (Unicode 18.0.0), per code point, for
// tests/bidi_tests.cpp: the reference src/algorithms/bidi/bidi.cpp is
// compared with exactly. Written from the specification's text rule by
// rule, in its order, with no attention to speed: each rule is a pass over
// the code points (or the isolating run sequence) and every backward
// search is a search. Shares nothing with the library but the class
// numbering (BidiClass's order).
//
// The choices UAX #9 leaves open are made as the library makes them
// (include/stxlib/bidi.hpp): a removed character (X9) takes the level of
// the nearest preceding character not removed, else of the nearest
// following one, else the paragraph level; BD16 stops at the 64th opener
// and keeps the pairs found.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace bidi_reference {

enum Class : int {
    L = 0, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON,
    LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI,
};

// One code point's paired-bracket data: type 0 none, 1 open, 2 close; key
// equal for an opener and the closers that pair with it (canonical
// equivalence applied).
struct Bracket {
    int type = 0;
    std::uint32_t key = 0;
};

// Direction: 0 automatic (default left to right), 1 automatic (default
// right to left), 2 left to right, 3 right to left.
struct Result {
    int base_level = 0;
    std::vector<int> levels; // per code point, before L1
};

namespace detail {

inline bool removed(const int c) {
    return c == RLE || c == LRE || c == RLO || c == LRO || c == PDF || c == BN;
}

inline bool initiator(const int c) { return c == LRI || c == RLI || c == FSI; }

// BD9: the matching PDI of each isolate initiator, -1 when none.
inline std::vector<long> matching_pdi(const std::vector<int>& classes) {
    std::vector<long> result(classes.size(), -1);
    for (std::size_t i = 0; i < classes.size(); ++i) {
        if (!initiator(classes[i])) continue;
        int depth = 1;
        for (std::size_t j = i + 1; j < classes.size(); ++j) {
            if (initiator(classes[j])) depth = depth + 1;
            if (classes[j] == PDI) {
                depth = depth - 1;
                if (depth == 0) {
                    result[i] = static_cast<long>(j);
                    break;
                }
            }
            if (classes[j] == B) break;
        }
    }
    return result;
}

// P2 over [begin, end): -1 none, 0 L, 1 R or AL.
inline int first_strong(const std::vector<int>& classes, const std::vector<long>& match, std::size_t begin,
                        std::size_t end) {
    std::size_t i = begin;
    while (i < end) {
        const int c = classes[i];
        if (c == L) return 0;
        if (c == R || c == AL) return 1;
        if (initiator(c)) {
            if (match[i] < 0) return -1;
            i = static_cast<std::size_t>(match[i]);
            continue;
        }
        i = i + 1;
    }
    return -1;
}

inline int strong_of(const int c) {
    if (c == L) return L;
    if (c == R || c == EN || c == AN) return R;
    return -1;
}

inline bool whitespace_like(const int c) {
    return c == WS || c == FSI || c == LRI || c == RLI || c == PDI || removed(c);
}

inline bool neutral(const int c) {
    return c == B || c == S || c == WS || c == ON || c == LRI || c == RLI || c == FSI || c == PDI;
}

} // namespace detail

inline Result resolve(const std::vector<int>& original, const std::vector<Bracket>& brackets, const int direction) {
    using namespace detail;
    const std::size_t n = original.size();
    Result result;
    result.levels.assign(n, 0);
    const std::vector<long> match = matching_pdi(original);

    // P2, P3.
    int base = 0;
    if (direction == 3) {
        base = 1;
    } else if (direction == 2) {
        base = 0;
    } else {
        const int strong = first_strong(original, match, 0, n);
        base = strong == 1 ? 1 : (strong == 0 ? 0 : (direction == 1 ? 1 : 0));
    }
    result.base_level = base;

    // X1 to X8.
    struct Entry {
        int level;
        int override_class; // -1 neutral
        bool isolate;
    };
    std::vector<Entry> stack;
    stack.push_back({base, -1, false});
    int overflow_isolates = 0;
    int overflow_embeddings = 0;
    int valid_isolates = 0;
    std::vector<int> types = original;
    std::vector<int> levels(n, base);
    for (std::size_t i = 0; i < n; ++i) {
        const int c = original[i];
        if (c == RLE || c == LRE || c == RLO || c == LRO) {
            const int current = stack.back().level;
            const int next = (c == RLE || c == RLO) ? ((current + 1) | 1) : ((current + 2) & ~1);
            if (next <= 125 && overflow_isolates == 0 && overflow_embeddings == 0) {
                stack.push_back({next, c == RLO ? R : (c == LRO ? L : -1), false});
            } else if (overflow_isolates == 0) {
                overflow_embeddings = overflow_embeddings + 1;
            }
            levels[i] = stack.back().level;
        } else if (c == RLI || c == LRI || c == FSI) {
            levels[i] = stack.back().level;
            if (stack.back().override_class >= 0) types[i] = stack.back().override_class;
            bool rtl = c == RLI;
            if (c == FSI) {
                const std::size_t end = match[i] < 0 ? n : static_cast<std::size_t>(match[i]);
                rtl = first_strong(original, match, i + 1, end) == 1;
            }
            const int current = stack.back().level;
            const int next = rtl ? ((current + 1) | 1) : ((current + 2) & ~1);
            if (next <= 125 && overflow_isolates == 0 && overflow_embeddings == 0) {
                valid_isolates = valid_isolates + 1;
                stack.push_back({next, -1, true});
            } else {
                overflow_isolates = overflow_isolates + 1;
            }
        } else if (c == PDI) {
            if (overflow_isolates > 0) {
                overflow_isolates = overflow_isolates - 1;
            } else if (valid_isolates == 0) {
                // nothing
            } else {
                overflow_embeddings = 0;
                while (!stack.back().isolate) stack.pop_back();
                stack.pop_back();
                valid_isolates = valid_isolates - 1;
            }
            levels[i] = stack.back().level;
            if (stack.back().override_class >= 0) types[i] = stack.back().override_class;
        } else if (c == PDF) {
            if (overflow_isolates > 0) {
                // nothing
            } else if (overflow_embeddings > 0) {
                overflow_embeddings = overflow_embeddings - 1;
            } else if (!stack.back().isolate && stack.size() >= 2) {
                stack.pop_back();
            }
            levels[i] = stack.back().level;
        } else if (c == B) {
            levels[i] = base;
        } else if (c == BN) {
            levels[i] = stack.back().level;
        } else {
            levels[i] = stack.back().level;
            if (stack.back().override_class >= 0) types[i] = stack.back().override_class;
        }
    }

    // X9: removed characters are skipped from here (kept[] lists the rest).
    std::vector<std::size_t> kept;
    for (std::size_t i = 0; i < n; ++i) {
        if (!removed(original[i])) kept.push_back(i);
    }

    // X10: level runs (as lists of positions), then isolating run sequences.
    std::vector<std::vector<std::size_t>> level_runs;
    for (std::size_t k = 0; k < kept.size(); ++k) {
        const std::size_t i = kept[k];
        if (k == 0 || levels[i] != levels[kept[k - 1]]) level_runs.push_back({});
        level_runs.back().push_back(i);
    }
    std::vector<bool> matched_pdi(n, false);
    for (std::size_t j = 0; j < n; ++j) {
        if (match[j] >= 0) matched_pdi[static_cast<std::size_t>(match[j])] = true;
    }
    std::vector<std::vector<std::size_t>> sequences;
    for (std::size_t r = 0; r < level_runs.size(); ++r) {
        const std::size_t first = level_runs[r].front();
        if (original[first] == PDI && matched_pdi[first]) continue;
        std::vector<std::size_t> sequence;
        std::size_t current = r;
        while (true) {
            sequence.insert(sequence.end(), level_runs[current].begin(), level_runs[current].end());
            const std::size_t last = level_runs[current].back();
            if (!initiator(original[last]) || match[last] < 0) break;
            bool found = false;
            for (std::size_t q = current + 1; q < level_runs.size(); ++q) {
                if (level_runs[q].front() == static_cast<std::size_t>(match[last])) {
                    current = q;
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }
        sequences.push_back(sequence);
    }

    std::vector<int> resolved(n, 0);
    for (const std::vector<std::size_t>& sequence : sequences) {
        const std::size_t m = sequence.size();
        const int level = levels[sequence.front()];
        // sos and eos.
        int before = base;
        for (std::size_t j = sequence.front(); j > 0; --j) {
            if (!removed(original[j - 1])) {
                before = levels[j - 1];
                break;
            }
        }
        int after = base;
        const std::size_t last = sequence.back();
        if (!initiator(original[last])) {
            for (std::size_t j = last + 1; j < n; ++j) {
                if (!removed(original[j])) {
                    after = levels[j];
                    break;
                }
            }
        }
        const int sos = (std::max(before, level) & 1) ? R : L;
        const int eos = (std::max(after, level) & 1) ? R : L;
        std::vector<int> t(m);
        for (std::size_t k = 0; k < m; ++k) t[k] = types[sequence[k]];
        const std::vector<int> before_w1 = t;

        // W1.
        for (std::size_t k = 0; k < m; ++k) {
            if (t[k] != NSM) continue;
            if (k == 0) {
                t[k] = sos;
            } else if (t[k - 1] == LRI || t[k - 1] == RLI || t[k - 1] == FSI || t[k - 1] == PDI) {
                t[k] = ON;
            } else {
                t[k] = t[k - 1];
            }
        }
        // W2.
        for (std::size_t k = 0; k < m; ++k) {
            if (t[k] != EN) continue;
            int strong = sos;
            for (std::size_t j = k; j > 0; --j) {
                const int c = t[j - 1];
                if (c == L || c == R || c == AL) {
                    strong = c;
                    break;
                }
            }
            if (strong == AL) t[k] = AN;
        }
        // W3.
        for (std::size_t k = 0; k < m; ++k) {
            if (t[k] == AL) t[k] = R;
        }
        // W4.
        for (std::size_t k = 1; k + 1 < m; ++k) {
            if (t[k] == ES && t[k - 1] == EN && t[k + 1] == EN) t[k] = EN;
            else if (t[k] == CS && t[k - 1] == EN && t[k + 1] == EN) t[k] = EN;
            else if (t[k] == CS && t[k - 1] == AN && t[k + 1] == AN) t[k] = AN;
        }
        // W5.
        for (std::size_t k = 0; k < m; ++k) {
            if (t[k] != ET) continue;
            std::size_t end = k;
            while (end < m && t[end] == ET) end = end + 1;
            const bool adjacent = (k > 0 && t[k - 1] == EN) || (end < m && t[end] == EN);
            if (adjacent) {
                for (std::size_t j = k; j < end; ++j) t[j] = EN;
            }
            k = end - 1;
        }
        // W6.
        for (std::size_t k = 0; k < m; ++k) {
            if (t[k] == ES || t[k] == ET || t[k] == CS) t[k] = ON;
        }
        // W7.
        for (std::size_t k = 0; k < m; ++k) {
            if (t[k] != EN) continue;
            int strong = sos;
            for (std::size_t j = k; j > 0; --j) {
                if (t[j - 1] == L || t[j - 1] == R) {
                    strong = t[j - 1];
                    break;
                }
            }
            if (strong == L) t[k] = L;
        }

        // N0. BD16 first.
        std::vector<std::pair<std::size_t, std::size_t>> pairs;
        {
            std::vector<std::pair<std::uint32_t, std::size_t>> openers;
            for (std::size_t k = 0; k < m; ++k) {
                const Bracket& bracket = brackets[sequence[k]];
                if (bracket.type == 0 || t[k] != ON) continue;
                if (bracket.type == 1) {
                    if (openers.size() == 63) break;
                    openers.push_back({bracket.key, k});
                    continue;
                }
                for (std::size_t depth = openers.size(); depth > 0; --depth) {
                    if (openers[depth - 1].first == bracket.key) {
                        pairs.push_back({openers[depth - 1].second, k});
                        openers.resize(depth - 1);
                        break;
                    }
                }
            }
            // Ascending by opener.
            for (std::size_t a = 0; a < pairs.size(); ++a) {
                for (std::size_t b = a + 1; b < pairs.size(); ++b) {
                    if (pairs[b].first < pairs[a].first) std::swap(pairs[a], pairs[b]);
                }
            }
        }
        const int embedding = (level & 1) ? R : L;
        const int opposite = embedding == L ? R : L;
        for (const std::pair<std::size_t, std::size_t>& pair : pairs) {
            bool has_embedding = false;
            bool has_opposite = false;
            for (std::size_t k = pair.first + 1; k < pair.second; ++k) {
                const int s = strong_of(t[k]);
                if (s == embedding) has_embedding = true;
                if (s == opposite) has_opposite = true;
            }
            int set = -1;
            if (has_embedding) {
                set = embedding;
            } else if (has_opposite) {
                int context = sos;
                for (std::size_t k = pair.first; k > 0; --k) {
                    const int s = strong_of(t[k - 1]);
                    if (s >= 0) {
                        context = s;
                        break;
                    }
                }
                set = context == opposite ? opposite : embedding;
            }
            if (set < 0) continue;
            t[pair.first] = set;
            t[pair.second] = set;
            for (std::size_t k = pair.first + 1; k < m && before_w1[k] == NSM; ++k) t[k] = set;
            for (std::size_t k = pair.second + 1; k < m && before_w1[k] == NSM; ++k) t[k] = set;
        }

        // N1, N2.
        for (std::size_t k = 0; k < m; ++k) {
            if (!neutral(t[k])) continue;
            std::size_t end = k;
            while (end < m && neutral(t[end])) end = end + 1;
            const int leading = k == 0 ? sos : strong_of(t[k - 1]);
            const int trailing = end == m ? eos : strong_of(t[end]);
            const int set = leading == trailing ? leading : embedding;
            for (std::size_t j = k; j < end; ++j) t[j] = set;
            k = end - 1;
        }

        // I1, I2.
        for (std::size_t k = 0; k < m; ++k) {
            int value = level;
            if ((level & 1) == 0) {
                if (t[k] == R) value = level + 1;
                if (t[k] == AN || t[k] == EN) value = level + 2;
            } else if (t[k] == L || t[k] == EN || t[k] == AN) {
                value = level + 1;
            }
            resolved[sequence[k]] = value;
        }
    }

    // Removed characters: the nearest preceding kept level, else the
    // nearest following, else the base level.
    for (std::size_t i = 0; i < n; ++i) {
        if (!removed(original[i])) {
            result.levels[i] = resolved[i];
            continue;
        }
        int value = -1;
        for (std::size_t j = i; j > 0; --j) {
            if (!removed(original[j - 1])) {
                value = resolved[j - 1];
                break;
            }
        }
        if (value < 0) {
            for (std::size_t j = i + 1; j < n; ++j) {
                if (!removed(original[j])) {
                    value = resolved[j];
                    break;
                }
            }
        }
        result.levels[i] = value < 0 ? base : value;
    }
    return result;
}

// L1 over the line [begin, end) of a resolved paragraph: the line's levels.
inline std::vector<int> line_levels(const Result& paragraph, const std::vector<int>& original, std::size_t begin,
                                    std::size_t end) {
    std::vector<int> levels(paragraph.levels.begin() + static_cast<long>(begin),
                            paragraph.levels.begin() + static_cast<long>(end));
    using detail::whitespace_like;
    for (std::size_t i = begin; i < end; ++i) {
        const int c = original[i];
        if (c == S || c == B) {
            levels[i - begin] = paragraph.base_level;
            for (std::size_t j = i; j > begin && whitespace_like(original[j - 1]); --j) {
                levels[j - 1 - begin] = paragraph.base_level;
            }
        }
    }
    for (std::size_t j = end; j > begin && whitespace_like(original[j - 1]); --j) {
        levels[j - 1 - begin] = paragraph.base_level;
    }
    return levels;
}

// L2: the visual order of the line's positions (0-based, line relative).
inline std::vector<std::size_t> visual_order(const std::vector<int>& levels) {
    std::vector<std::size_t> order(levels.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    int highest = 0;
    int lowest_odd = 1000;
    for (const int level : levels) {
        if (level > highest) highest = level;
        if ((level & 1) && level < lowest_odd) lowest_odd = level;
    }
    for (int level = highest; level >= lowest_odd; --level) {
        std::size_t i = 0;
        while (i < levels.size()) {
            if (levels[order[i]] < level) {
                i = i + 1;
                continue;
            }
            std::size_t end = i;
            while (end < levels.size() && levels[order[end]] >= level) end = end + 1;
            for (std::size_t a = i, b = end - 1; a < b; ++a, --b) std::swap(order[a], order[b]);
            i = end;
        }
    }
    return order;
}

} // namespace bidi_reference
