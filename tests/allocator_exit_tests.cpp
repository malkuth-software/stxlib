// The allocator at process end (tests/allocator_exit_library.cpp gives the
// reason: the logic review's L-05). The program starts itself as a child
// `rounds` times; each child starts the library's workers, lets them run, and
// returns from main while they still allocate, so the host stops them at
// arbitrary points, sometimes inside an allocator lock. The library's
// detach-time destructor then allocates on every locking path and writes its
// marker. A child that does not end within the deadline is the deadlock
// (the first one stops the run, so a hang fails in seconds rather than
// stalling ctest); one that ends without the marker, or with a nonzero
// status, is a failure. On Windows the marker also carries what
// RtlDllShutdownInProgress said when the library's DLL_PROCESS_DETACH began
// (tests/allocator_exit_library.cpp); the run prints those values.
// Sampled, not proved: a run that passes says only that no round met it.
//
// Usage: stxlib_allocator_exit_tests [rounds]     (the check; 20 rounds)
//        stxlib_allocator_exit_tests child <marker-path>

#include "allocator_exit.hpp"

#include "stxlib/children.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/desktop.hpp"
#include "stxlib/path.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

using stxlib::Child;
using stxlib::ChildEnd;
using stxlib::ChildRequest;
using stxlib::ChildStreams;
using stxlib::Duration;
using stxlib::OperationResult;
using stxlib::TextView;
using stxlib::u32;

constexpr u32 default_rounds = 20;
constexpr u32 worker_count = 8;
constexpr int child_run_milliseconds = 30;
constexpr stxlib::i64 child_deadline_seconds = 10;

TextView view_of(const std::string& text) {
    OperationResult<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

int run_child(const char* const marker) {
    stxlib_allocator_exit::arm_detach_check(marker);
    const u32 started = stxlib_allocator_exit::start_workers(worker_count);
    if (started != worker_count) return 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(child_run_milliseconds));
    return 0;
}

bool marker_written(const std::string& marker) {
    std::FILE* const file = std::fopen(marker.c_str(), "rb");
    if (file == nullptr) return false;
    char content[64] = {};
    const std::size_t count = std::fread(content, 1, sizeof(content) - 1, file);
    static_cast<void>(std::fclose(file));
    const bool result = count >= 2 && std::strncmp(content, "ok", 2) == 0;
    if (result && count > 2) std::printf("detach marker:%s\n", content + 2);
    return result;
}

// The library's DLL_PROCESS_DETACH writes `<marker>.detach` before it
// allocates (Windows); shown when a child hangs, then removed.
void print_early_detach(const std::string& marker) {
    const std::string early = marker + ".detach";
    std::FILE* const file = std::fopen(early.c_str(), "rb");
    if (file == nullptr) return;
    char content[64] = {};
    static_cast<void>(std::fread(content, 1, sizeof(content) - 1, file));
    static_cast<void>(std::fclose(file));
    static_cast<void>(std::remove(early.c_str()));
    std::fprintf(stderr, "the hung child's detach began with: %s\n", content);
}

void rounds_end_cleanly(const u32 rounds) {
    OperationResult<stxlib::Path> self = stxlib::current_executable();
    CHECK(self.succeeded());
    if (self.failed()) return;
    const stxlib::PathText self_text = (*self.value()).text();
    const std::string program(self_text.data(), self_text.count());
    const std::string marker = program + ".marker";
    u32 hung = 0;
    u32 unmarked = 0;
    u32 bad_status = 0;
    for (u32 round = 0; round < rounds; ++round) {
        static_cast<void>(std::remove(marker.c_str()));
        const std::string mode = "child";
        const stxlib::PathText argument_texts[] = {view_of(mode), view_of(marker)};
        const ChildRequest request{.program = view_of(program),
                                   .arguments = stxlib::View<stxlib::PathText>(argument_texts, 2),
                                   .streams = ChildStreams::discard,
                                   .detached = false};
        OperationResult<Child> started = Child::start(request);
        CHECK(started.succeeded());
        if (started.failed()) return;
        Child child = static_cast<OperationResult<Child>&&>(started).take_value();
        const OperationResult<ChildEnd> end = child.wait(Duration::seconds(child_deadline_seconds));
        if (end.failed()) {
            hung = hung + 1;
            print_early_detach(marker);
            std::fprintf(stderr, "round %u: the child did not end within %lld s\n", round,
                         static_cast<long long>(child_deadline_seconds));
            static_cast<void>(child.kill());
            break; // one hang is the finding; the rest would only take time
        }
        const u32* const status = (*end.value()).exit_status();
        if (status == nullptr || *status != 0) {
            bad_status = bad_status + 1;
            std::fprintf(stderr, "round %u: the child ended with status %u\n", round, status == nullptr ? 0xffffffffu : *status);
        }
        if (!marker_written(marker)) unmarked = unmarked + 1;
    }
    static_cast<void>(std::remove(marker.c_str()));
    std::printf("allocator_exit: %u rounds, %u hung, %u bad status, %u without the detach marker\n", rounds, hung,
                bad_status, unmarked);
    CHECK(hung == 0);
    CHECK(bad_status == 0);
    CHECK(unmarked == 0);
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count == 3 && std::strcmp(arguments[1], "child") == 0) {
        const int status = run_child(arguments[2]);
        return status;
    }
    u32 rounds = default_rounds;
    if (argument_count == 2) rounds = static_cast<u32>(std::strtoul(arguments[1], nullptr, 10));
    rounds_end_cleanly(rounds);
    const int status = stxlib_tests::finish("allocator_exit");
    return status;
}
