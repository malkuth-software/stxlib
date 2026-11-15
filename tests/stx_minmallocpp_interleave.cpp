// The interleaving checker for stx_minmallocpp's ownership protocol.
//
// The allocator's own code is the model (the owner, 2026-11-15). Its
// sources are compiled here with STXLIB_STX_MINMALLOCPP_INTERLEAVE, which
// turns each mark before an operation on shared memory (page.hpp,
// STX_MINMALLOCPP_STEP; the atomic steps of proofs/
// stx_minmallocpp_graph.md, section 5) into a call that parks the thread
// until this checker gives it the turn. Only one thread runs between two
// marks, so a run is one exact ordering of the steps, chosen by a schedule.
// The checker runs a scenario under every schedule with at most
// `preemptions` forced switches, each in a fresh child process (so each
// starts from an untouched allocator), and fails if any run:
//   - stops on one of the allocator's own checks (STX_MINMALLOCPP_CHECK,
//     the integrity stops; they are compiled in),
//   - refuses a release the scenario is entitled to make,
//   - deadlocks (every thread that is not finished waits for a lock),
//   - or leaves, after every block has been released and the main thread
//     has trimmed, a page that was not given back: a lost release.
//
// What a pass means: for these scenarios, with 2 controlled threads, every
// ordering of the steps with up to `preemptions` forced switches keeps the
// properties above. It is exhaustive within that bound, on the code as
// compiled, under a sequentially consistent memory model (one thread runs
// at a time; the allocator's cross-thread steps use sequentially consistent
// or acquire/release operations). It says nothing beyond the bound, the
// scenarios, or about weaker hardware orderings of the relaxed reads.
//
// A switch is forced when the checker gives the turn to another thread
// although the thread that ran last could go on. Switches when a thread
// finishes or waits for a lock are free.
//
// POSIX only (fork). Usage:
//   stxlib_stx_minmallocpp_interleave [preemptions]     (default 2)
//   stxlib_stx_minmallocpp_interleave <preemptions> <scenario> <rank>...
// The second form runs one schedule in this process, for a debugger: the
// ranks are the ones a failure printed.

#include "state/stx_minmallocpp/allocator.hpp"
#include "state/stx_minmallocpp/heap.hpp"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using stxlib::uptr;
using stxlib::usize;
using namespace stxlib::stx_minmallocpp;

// ---- the turn ----------------------------------------------------------------------

constexpr int thread_limit = 2;
constexpr int nobody = -1;

enum class Status : int {
    not_started = 0,
    // At a mark, able to take its next step.
    parked = 1,
    running = 2,
    // At a mark, waiting for a lock another thread holds.
    blocked = 3,
    finished = 4,
};

std::mutex turn_mutex;
std::condition_variable turn_changed;
int turn = nobody;
Status status[thread_limit] = {Status::not_started, Status::not_started};
// The controlled thread's index; nobody for any other thread, whose marks
// do nothing.
thread_local int self = nobody;

// Parks the calling controlled thread as `as` and waits for the turn.
void hand_over(const Status as) {
    std::unique_lock<std::mutex> lock(turn_mutex);
    status[self] = as;
    turn = nobody;
    turn_changed.notify_all();
    while (turn != self) turn_changed.wait(lock);
    status[self] = Status::running;
}

void enter_control(const int index) {
    self = index;
    hand_over(Status::parked);
}

void leave_control() {
    const std::unique_lock<std::mutex> lock(turn_mutex);
    status[self] = Status::finished;
    turn = nobody;
    self = nobody;
    turn_changed.notify_all();
}

} // namespace

namespace stxlib::stx_minmallocpp {

void interleave_step() noexcept {
    if (self == nobody) return;
    hand_over(Status::parked);
}

void interleave_blocked() noexcept {
    if (self == nobody) {
        std::this_thread::yield();
        return;
    }
    hand_over(Status::blocked);
}

} // namespace stxlib::stx_minmallocpp

namespace {

// ---- scenarios ---------------------------------------------------------------------

// The largest class of medium pages: 8 blocks to a page, one bitmap word,
// so a page fills in eight allocations and each operation has few steps;
// and a heap may hold 8 pages of it, so a release into an abandoned page
// by a heap that is allocating the class claims the page. (It never does
// for the classes of large pages, where the current page is the heap's
// whole allowance: with one of those, as this file first had it, the
// claim was not exercised.)
constexpr usize block_bytes = 65536;
constexpr usize page_capacity = 8;
constexpr usize block_limit = 12;

static_assert(class_table.shapes[class_of(block_bytes)].capacity == page_capacity &&
                  class_table.shapes[class_of(block_bytes)].page_slices * 2 <= keep_slices,
              "the scenarios' class: 8 blocks to a page, and a heap may hold more than one page of it");

// What a scenario's threads share. Each thread's `before` runs on that
// thread before it is controlled (its marks do nothing), so it may set up
// what the controlled part needs; `body` runs under the checker.
struct Shared final {
    void* blocks[block_limit]{};
    bool failed{false};
};

struct Scenario final {
    const char* name;
    void (*before[thread_limit])(Shared& shared);
    void (*body[thread_limit])(Shared& shared);
};

void nothing(Shared& shared) { static_cast<void>(shared); }

void release_one(Shared& shared, const usize index) {
    if (release(shared.blocks[index]) != Release::released) shared.failed = true;
    shared.blocks[index] = nullptr;
}

void allocate_into(Shared& shared, const usize first, const usize count) {
    for (usize index = first; index < first + count; ++index) {
        shared.blocks[index] = allocate(block_bytes, 16);
        if (shared.blocks[index] == nullptr) shared.failed = true;
    }
}

// 1. A release from another thread races the owner finding its page full
// and abandoning it.
void fill_page(Shared& shared) { allocate_into(shared, 0, page_capacity); }
void allocate_past_full(Shared& shared) {
    allocate_into(shared, 8, 1);
    release_one(shared, 8);
}
void release_first_two(Shared& shared) {
    release_one(shared, 0);
    release_one(shared, 1);
}

// 2. Two threads release the last blocks of an abandoned page: one whose
// heap keeps the page (it has a current page of the class, block 8's), one
// with no heap.
void fill_and_move_on(Shared& shared) { allocate_into(shared, 0, page_capacity + 1); }
void release_low_half(Shared& shared) {
    for (usize index = 0; index < 4; ++index) release_one(shared, index);
}
void release_high_half(Shared& shared) {
    for (usize index = 4; index < 8; ++index) release_one(shared, index);
}

// 3. The owner merges, frees the page and makes it again while another
// thread's release of the page's last block is between its bit and its
// announcement.
void allocate_two(Shared& shared) { allocate_into(shared, 0, 2); }
void release_trim_remake(Shared& shared) {
    release_one(shared, 0);
    trim();
    allocate_into(shared, 2, 1);
    release_one(shared, 2);
}
void release_second(Shared& shared) { release_one(shared, 1); }

// 4. A thread's exit abandons a part-used page while another thread
// releases its blocks.
void allocate_three(Shared& shared) { allocate_into(shared, 0, 3); }
void release_three(Shared& shared) {
    for (usize index = 0; index < 3; ++index) release_one(shared, index);
}

// 5. Two threads whose heaps both keep pages release into the same
// abandoned page.
void allocate_own_block(Shared& shared) { allocate_into(shared, 9, 1); }
void release_zero(Shared& shared) { release_one(shared, 0); }
void release_one_and_own(Shared& shared) {
    release_one(shared, 1);
    release_one(shared, 9);
}

constexpr Scenario scenarios[] = {
    {"release races abandon", {fill_page, nothing}, {allocate_past_full, release_first_two}},
    {"two releasers finish a page", {fill_and_move_on, nothing}, {release_low_half, release_high_half}},
    {"owner frees and remakes under a release", {allocate_two, nothing}, {release_trim_remake, release_second}},
    {"thread exit races releases", {allocate_three, nothing}, {nothing, release_three}},
    {"two keepers claim one page", {fill_and_move_on, allocate_own_block}, {release_zero, release_one_and_own}},
};

constexpr usize scenario_count = sizeof(scenarios) / sizeof(scenarios[0]);

// ---- one run ----------------------------------------------------------------------

// One decision of a run: which runnable thread was given the turn (its
// rank: 0 is "the one that ran last, else the lowest"), out of how many,
// and whether choosing another was a forced switch.
struct Decision final {
    int rank{0};
    int choices{0};
    bool forced{false};
};

void run_thread(const Scenario& scenario, const int index, Shared& shared) {
    scenario.before[index](shared);
    enter_control(index);
    scenario.body[index](shared);
    // The thread's exit, under the checker.
    end_thread();
    leave_control();
}

// The addresses of the pages that hold the scenario's blocks, as
// (region base, head slice), read while the blocks are live.
struct PageSeen final {
    uptr base{0};
    usize head{0};
};

[[nodiscard]] PageSeen page_holding(const void* const block) {
    const uptr value = reinterpret_cast<uptr>(block);
    const uptr base = region_base_of(value);
    const usize slice = static_cast<usize>(value - base) >> slice_shift;
    const stxlib::u32 code = slice_heads(base)[slice].load();
    const PageSeen seen{.base = base, .head = static_cast<usize>(code) - 1};
    return seen;
}

// Runs the scenario under `prefix` (ranks for the first decisions; rank 0
// after them), records every decision, and returns 0 when the run kept
// every property. Runs in a process of its own.
[[nodiscard]] int run_schedule(const Scenario& scenario, const std::vector<int>& prefix, std::vector<Decision>& trace) {
    Shared shared{};
    std::vector<std::thread> threads{};
    for (int index = 0; index < thread_limit; ++index) {
        threads.emplace_back(run_thread, std::cref(scenario), index, std::ref(shared));
        std::unique_lock<std::mutex> lock(turn_mutex);
        while (status[index] == Status::not_started) turn_changed.wait(lock);
    }
    // Every block exists now; note their pages before any is released.
    std::vector<PageSeen> pages{};
    for (usize index = 0; index < block_limit; ++index) {
        if (shared.blocks[index] != nullptr) pages.push_back(page_holding(shared.blocks[index]));
    }
    int last = nobody;
    for (;;) {
        std::unique_lock<std::mutex> lock(turn_mutex);
        while (turn != nobody) turn_changed.wait(lock);
        int order[thread_limit] = {nobody, nobody};
        int choices = 0;
        bool waiting = false;
        const bool last_can_go = last != nobody && status[last] == Status::parked;
        if (last_can_go) {
            order[choices] = last;
            choices = choices + 1;
        }
        for (int index = 0; index < thread_limit; ++index) {
            if (status[index] == Status::blocked) waiting = true;
            if (status[index] != Status::parked || index == last) continue;
            order[choices] = index;
            choices = choices + 1;
        }
        if (choices == 0) {
            // Every unfinished thread waits for a lock. The threads are still
            // parked, so the process ends here, without unwinding them.
            if (waiting) ::_exit(4);
            break;
        }
        const usize decision = trace.size();
        const int rank = decision < prefix.size() ? prefix[decision] : 0;
        if (rank >= choices) ::_exit(5); // the prefix does not fit this run
        trace.push_back(Decision{.rank = rank, .choices = choices, .forced = last_can_go});
        const int chosen = order[rank];
        // A thread that waited for a lock may try again once another has run.
        for (int index = 0; index < thread_limit; ++index) {
            if (index != chosen && status[index] == Status::blocked) status[index] = Status::parked;
        }
        last = chosen;
        status[chosen] = Status::running;
        turn = chosen;
        turn_changed.notify_all();
    }
    for (std::thread& thread : threads) thread.join();
    if (shared.failed) return 6;
    // The main thread releases what the scenario left and trims; then every
    // page the scenario used must have been given back.
    for (usize index = 0; index < block_limit; ++index) {
        if (shared.blocks[index] != nullptr && release(shared.blocks[index]) != Release::released) return 7;
    }
    trim();
    for (const PageSeen& seen : pages) {
        if (slice_heads(seen.base)[seen.head].load() != 0) return 8; // a page was not given back
    }
    // And the allocator still serves the class.
    void* const again = allocate(block_bytes, 16);
    if (again == nullptr || release(again) != Release::released) return 9;
    return 0;
}

// ---- the search ---------------------------------------------------------------------

struct Outcome final {
    int status{0};
    std::vector<Decision> trace{};
};

// Runs one schedule in a child process and reads back its trace.
[[nodiscard]] Outcome run_in_child(const Scenario& scenario, const std::vector<int>& prefix) {
    Outcome outcome{};
    int ends[2] = {0, 0};
    if (::pipe(ends) != 0) {
        outcome.status = 100;
        return outcome;
    }
    const pid_t child = ::fork();
    if (child == 0) {
        static_cast<void>(::close(ends[0]));
        static_cast<void>(::alarm(30));
        std::vector<Decision> trace{};
        const int result = run_schedule(scenario, prefix, trace);
        for (const Decision& decision : trace) {
            const unsigned char bytes[3] = {static_cast<unsigned char>(decision.rank),
                                            static_cast<unsigned char>(decision.choices),
                                            static_cast<unsigned char>(decision.forced ? 1 : 0)};
            if (::write(ends[1], bytes, sizeof(bytes)) != static_cast<ssize_t>(sizeof(bytes))) ::_exit(101);
        }
        ::_exit(result);
    }
    static_cast<void>(::close(ends[1]));
    unsigned char bytes[3] = {0, 0, 0};
    while (::read(ends[0], bytes, sizeof(bytes)) == static_cast<ssize_t>(sizeof(bytes))) {
        outcome.trace.push_back(Decision{.rank = bytes[0], .choices = bytes[1], .forced = bytes[2] != 0});
    }
    static_cast<void>(::close(ends[0]));
    int wait_status = 0;
    static_cast<void>(::waitpid(child, &wait_status, 0));
    outcome.status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 200 + WTERMSIG(wait_status);
    return outcome;
}

[[nodiscard]] const char* meaning(const int code) {
    if (code == 4) return "deadlock: every unfinished thread waits for a lock";
    if (code == 6) return "a release the scenario is entitled to was refused, or an allocation failed";
    if (code == 7) return "the main thread's release of a remaining block was refused";
    if (code == 8) return "a page was not given back after every block was released: a lost release";
    if (code == 9) return "the allocator no longer serves the class";
    if (code >= 200) return "the run was stopped by a signal (an allocator check, or the 30 s alarm)";
    return "the checker itself failed";
}

// Every schedule of the scenario with at most `budget` forced switches,
// depth first: after a run, the next schedule changes the deepest decision
// that has an untried choice within the budget. False when a run failed.
[[nodiscard]] bool explore(const Scenario& scenario, const int budget, usize& runs) {
    std::vector<int> prefix{};
    for (;;) {
        const Outcome outcome = run_in_child(scenario, prefix);
        runs = runs + 1;
        if (outcome.status != 0) {
            std::printf("FAILED: %s: %s (status %d)\n  schedule:", scenario.name, meaning(outcome.status), outcome.status);
            for (const int rank : prefix) std::printf(" %d", rank);
            std::printf("\n");
            return false;
        }
        const std::vector<Decision>& trace = outcome.trace;
        // Forced switches made before each decision.
        std::vector<int> forced_before(trace.size() + 1, 0);
        for (usize index = 0; index < trace.size(); ++index) {
            const bool costs = trace[index].forced && trace[index].rank != 0;
            forced_before[index + 1] = forced_before[index] + (costs ? 1 : 0);
        }
        bool found = false;
        for (usize index = trace.size(); index > 0 && !found; --index) {
            const Decision& decision = trace[index - 1];
            if (decision.rank + 1 >= decision.choices) continue;
            // Moving off rank 0 is a forced switch when the last thread could go on.
            const int cost = decision.forced ? 1 : 0;
            if (forced_before[index - 1] + cost > budget) continue;
            prefix.clear();
            for (usize kept = 0; kept + 1 < index; ++kept) prefix.push_back(trace[kept].rank);
            prefix.push_back(decision.rank + 1);
            found = true;
        }
        if (!found) return true;
    }
}

} // namespace

int main(const int argc, const char* const argv[]) {
    const int budget = argc > 1 ? std::atoi(argv[1]) : 2;
    if (argc > 2) {
        // One schedule, in this process.
        const usize which = static_cast<usize>(std::atoi(argv[2]));
        if (which >= scenario_count) return 2;
        std::vector<int> prefix{};
        for (int index = 3; index < argc; ++index) prefix.push_back(std::atoi(argv[index]));
        std::vector<Decision> trace{};
        const int result = run_schedule(scenarios[which], prefix, trace);
        std::printf("%s: status %d after %zu decisions\n", scenarios[which].name, result, trace.size());
        return result;
    }
    bool all_passed = true;
    usize total = 0;
    for (usize index = 0; index < scenario_count; ++index) {
        usize runs = 0;
        const bool passed = explore(scenarios[index], budget, runs);
        std::printf("%-44s %8zu schedules: %s\n", scenarios[index].name, runs, passed ? "pass" : "FAIL");
        total = total + runs;
        if (!passed) all_passed = false;
    }
    std::printf("stx_minmallocpp interleavings, up to %d forced switches: %zu schedules, %s\n", budget, total,
                all_passed ? "all pass" : "FAILED");
    const int exit_code = all_passed ? 0 : 1;
    return exit_code;
}
