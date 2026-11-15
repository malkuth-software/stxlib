// Tests of the Emscripten family (owner decision 2026-11-15), run under Node
// by ctest (cmake/emscripten.cmake): the thread facts, the clocks, entropy,
// the environment and known directories, web storage and the volume
// durability rules, the wake channel, SIMD128, and each service a page lacks
// answering unsupported. Built only in the Emscripten family
// (CMakeLists.txt). Tested, not proved (proofs/REPORT.md).
//
// What Node cannot show: IndexedDB and OPFS (absent in Node, so their mounts
// are checked to refuse cleanly), the pool's fixed limit (Node ignores
// PTHREAD_POOL_SIZE_STRICT), cross-origin isolation in a page, and the
// page-hide hook. Those wait for a browser run (hand-in risks).

#include "stxlib/capabilities.hpp"
#include "stxlib/children.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/desktop.hpp"
#include "stxlib/entropy.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/files.hpp"
#include "stxlib/library.hpp"
#include "stxlib/local_time.hpp"
#include "stxlib/pages.hpp"
#include "stxlib/path.hpp"
#include "stxlib/process.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/stack_trace.hpp"
#include "stxlib/text.hpp"
#include "stxlib/threads.hpp"
#include "stxlib/wake.hpp"
#include "stxlib/web.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#error "emscripten_tests.cpp is built for the Emscripten family only (CMakeLists.txt)"
#endif

namespace {

using stxlib::Byte;
using stxlib::Duration;
using stxlib::Durability;
using stxlib::ErrorKind;
using stxlib::HostCode;
using stxlib::IntervalTime;
using stxlib::Path;
using stxlib::Result;
using stxlib::Status;
using stxlib::StorageKind;
using stxlib::TextView;
using stxlib::usize;
using stxlib::WakeChannel;
using stxlib::WakeWait;

template <typename Value>
ErrorKind kind_of(const Result<Value>& result) {
    if (result.succeeded()) return ErrorKind::internal_failure;
    const ErrorKind kind = (*result.error()).kind();
    return kind;
}

ErrorKind kind_of(const Status& status) {
    if (status.succeeded()) return ErrorKind::internal_failure;
    const ErrorKind kind = (*status.error()).kind();
    return kind;
}

HostCode code_of(const Status& status) {
    if (status.succeeded()) return HostCode::none;
    const HostCode code = static_cast<HostCode>((*status.error()).code());
    return code;
}

TextView view_of(const std::string& text) {
    Result<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

Path path_of(const std::string& text) {
    Result<Path> parsed = Path::parse(view_of(text));
    CHECK(parsed.succeeded());
    if (parsed.failed()) return Path();
    Path result = static_cast<Result<Path>&&>(parsed).take_value();
    return result;
}

std::string text_of(const stxlib::PathText text) {
    std::string result(text.data(), text.count());
    return result;
}

std::vector<Byte> bytes_of(const std::string& text) {
    std::vector<Byte> result;
    result.reserve(text.size());
    for (const char unit : text) result.push_back(static_cast<Byte>(unit));
    return result;
}

std::vector<Byte> read_or_empty(const Path& path) {
    Result<stxlib::Buffer<Byte>> read = stxlib::read_file(path, 4096);
    CHECK(read.succeeded());
    std::vector<Byte> result;
    if (read.failed()) return result;
    const stxlib::View<Byte> view = (*read.value()).view();
    for (usize index = 0; index < view.count(); ++index) result.push_back(view[index]);
    return result;
}

// ---- the runtime --------------------------------------------------------------

void thread_facts() {
    // Node: no fixed pool (it starts workers on demand), and a process.
    CHECK(stxlib::thread_pool_limit() == 0);
    CHECK(!stxlib::capability_supported(stxlib::Capability::fixed_thread_pool));
    CHECK(stxlib::capability_supported(stxlib::Capability::shared_memory));
    CHECK(stxlib::capability_supported(stxlib::Capability::cross_origin_isolated));
    CHECK(stxlib::processor_count() >= 1);
    // No name for main's thread under PROXY_TO_PTHREAD: as on Windows.
    const Result<bool> main_thread = stxlib::is_main_thread();
    CHECK(kind_of(main_thread) == ErrorKind::unsupported);
}

void clocks() {
    CHECK(stxlib::capability_supported(stxlib::Capability::interval_clock));
    CHECK(!stxlib::capability_supported(stxlib::Capability::elapsed_clock));
    // Both read emscripten_get_now: the same epoch, so close readings.
    const IntervalTime interval = IntervalTime::now();
    const stxlib::ElapsedTime elapsed = stxlib::ElapsedTime::now();
    const long long gap = elapsed.count_nanoseconds() - interval.count_nanoseconds();
    CHECK(gap >= 0 && gap < 1000000000LL);
    IntervalTime previous = IntervalTime::now();
    bool backwards = false;
    for (int read = 0; read < 100000; ++read) {
        const IntervalTime next = IntervalTime::now();
        if (next < previous) backwards = true;
        previous = next;
    }
    CHECK(!backwards);
    const IntervalTime before = IntervalTime::now();
    stxlib::sleep_for(Duration::milliseconds(20));
    CHECK(IntervalTime::now().since(before) >= Duration::milliseconds(20));
}

void entropy() {
    Byte first[64]{};
    Byte second[64]{};
    // Above getentropy's 256-byte request (musl's getentropy.c:16-22) is
    // covered by the POSIX module's loop; two draws must differ.
    CHECK(stxlib::fill_entropy(stxlib::MutableView<Byte>(first)).succeeded());
    CHECK(stxlib::fill_entropy(stxlib::MutableView<Byte>(second)).succeeded());
    bool same = true;
    for (usize index = 0; index < 64; ++index) {
        if (first[index] != second[index]) same = false;
    }
    CHECK(!same);
    std::vector<Byte> large(1000);
    CHECK(stxlib::fill_entropy(stxlib::MutableView<Byte>(large.data(), large.size())).succeeded());
}

void environment_and_home() {
    // Emscripten's default environment (src/lib/libwasi.js:49-62).
    const Result<stxlib::Utf8String> home = stxlib::read_variable("HOME", 256);
    CHECK(home.succeeded() && (*home.value()).view() == TextView("/home/web_user"));
    const Result<Path> known = stxlib::known_directory(stxlib::KnownDirectory::home);
    CHECK(known.succeeded() && text_of((*known.value()).text()) == "/home/web_user");
    CHECK(stxlib::set_variable("STXLIB_WEB_TEST", "value").succeeded());
    const Result<stxlib::Utf8String> read = stxlib::read_variable("STXLIB_WEB_TEST", 16);
    CHECK(read.succeeded() && (*read.value()).view() == TextView("value"));
    CHECK(stxlib::remove_variable("STXLIB_WEB_TEST").succeeded());
}

// ---- web storage ----------------------------------------------------------------

void storage_kinds() {
    CHECK(stxlib::capability_supported(stxlib::Capability::web_storage));
    CHECK(stxlib::web_storage_durability(StorageKind::memory) == Durability::none);
    CHECK(stxlib::web_storage_durability(StorageKind::indexed_db) == Durability::ordered);
    CHECK(stxlib::web_storage_durability(StorageKind::origin_private) == Durability::durable);
    // A relative path is refused before anything is made.
    const Status relative = stxlib::mount_web_storage(path_of("relative/volume"), StorageKind::memory);
    CHECK(kind_of(relative) == ErrorKind::invalid_argument && code_of(relative) == HostCode::invalid_name);
    const Status unknown = stxlib::mount_web_storage(path_of("/stxlib-unknown"), static_cast<StorageKind>(9));
    CHECK(kind_of(unknown) == ErrorKind::invalid_argument);
}

// A memory volume: every level is accepted (the tmpfs rule, web.hpp), and
// files read back.
void memory_volume() {
    const Path root = path_of("/stxlib-web/memory");
    CHECK(stxlib::mount_web_storage(root, StorageKind::memory).succeeded());
    // Mounting a memory volume where its directory exists is harmless.
    CHECK(stxlib::mount_web_storage(root, StorageKind::memory).succeeded());
    const Path file = path_of("/stxlib-web/memory/file.txt");
    const std::vector<Byte> content = bytes_of("kept for the page's life");
    const stxlib::View<Byte> view(content.data(), content.size());
    CHECK(stxlib::write_file(file, view, Durability::durable).succeeded());
    CHECK(read_or_empty(file) == content);
    CHECK(stxlib::write_file(file, view, Durability::ordered).succeeded());
    Result<stxlib::File> opened = stxlib::File::open(file, stxlib::FileMode::append);
    CHECK(opened.succeeded());
    if (opened.failed()) return;
    stxlib::File& handle = *opened.value();
    CHECK(handle.write_all(view).succeeded());
    CHECK(handle.sync(Durability::none).succeeded());
    CHECK(handle.sync(Durability::ordered).succeeded());
    CHECK(handle.sync(Durability::durable).succeeded());
    CHECK(handle.close().succeeded());
    // A file in the way of a volume's directory.
    const Status blocked = stxlib::mount_web_storage(file, StorageKind::memory);
    CHECK(kind_of(blocked) == ErrorKind::invalid_argument && code_of(blocked) == HostCode::not_a_directory);
}

// Node has neither IndexedDB nor OPFS: both refuse without changing
// anything.
void browser_volumes_absent() {
    const Status indexed = stxlib::mount_web_storage(path_of("/stxlib-web/indexed"), StorageKind::indexed_db);
    CHECK(kind_of(indexed) == ErrorKind::unsupported);
    const Status origin = stxlib::mount_web_storage(path_of("/stxlib-web/origin"), StorageKind::origin_private);
#if defined(STXLIB_TEST_WASMFS)
    // WasmFS refuses OPFS on the main browser thread first.
    CHECK(kind_of(origin) == ErrorKind::invalid_argument && code_of(origin) == HostCode::invalid_state);
#else
    // OPFS requires WasmFS.
    CHECK(kind_of(origin) == ErrorKind::unsupported);
#endif
    // Node has no page to hide.
    CHECK(kind_of(stxlib::persist_web_storage_on_page_hide()) == ErrorKind::unsupported);
}

// The waits are refused where the event loop would be blocked.
void storage_waits_on_main_thread() {
    const Status persisted = stxlib::persist_web_storage();
    CHECK(kind_of(persisted) == ErrorKind::invalid_argument && code_of(persisted) == HostCode::invalid_state);
    const Status restored = stxlib::restore_web_storage();
    CHECK(kind_of(restored) == ErrorKind::invalid_argument && code_of(restored) == HostCode::invalid_state);
}

struct StorageReport final {
    bool persisted{false};
    bool restored{false};
    ErrorKind origin_kind{ErrorKind::internal_failure};
};

void storage_from_thread(const stxlib::StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    StorageReport& report = *static_cast<StorageReport*>(context);
    report.persisted = stxlib::persist_web_storage().succeeded();
    report.restored = stxlib::restore_web_storage().succeeded();
    report.origin_kind = kind_of(stxlib::mount_web_storage(path_of("/stxlib-web/origin"), StorageKind::origin_private));
}

// From a thread the waits are allowed. With no IndexedDB volume the
// JavaScript file system completes the synchronisation at once (no mount
// has a sync step), so the main thread's join does not keep it waiting;
// a page that persists IndexedDB volumes must not block its main thread
// on the persisting thread (web.hpp).
void storage_waits_from_thread() {
    StorageReport report{};
    Result<stxlib::Thread> started = stxlib::Thread::start(storage_from_thread, &report);
    CHECK(started.succeeded());
    if (started.failed()) return;
    stxlib::Thread thread = static_cast<Result<stxlib::Thread>&&>(started).take_value();
    static_cast<void>(thread.join());
    CHECK(report.persisted);
    CHECK(report.restored);
    CHECK(report.origin_kind == ErrorKind::unsupported);
}

// ---- the wake channel ---------------------------------------------------------------

void wake_later(const stxlib::StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    const WakeChannel& channel = *static_cast<const WakeChannel*>(context);
    stxlib::sleep_for(Duration::milliseconds(20));
    channel.wake();
}

void wake_channel() {
    CHECK(stxlib::capability_supported(stxlib::Capability::wake_channel));
    Result<WakeChannel> made = WakeChannel::create();
    CHECK(made.succeeded());
    if (made.failed()) return;
    WakeChannel channel = static_cast<Result<WakeChannel>&&>(made).take_value();
    CHECK(channel.is_open() && channel.native_handle() != -1);
    CHECK(!channel.drain());
    channel.wake();
    channel.wake();
    CHECK(channel.drain());
    CHECK(!channel.drain());

    const IntervalTime before = IntervalTime::now();
    Result<WakeWait> quiet = channel.wait(stxlib::View<stxlib::i64>(), Duration::milliseconds(30));
    CHECK(quiet.succeeded() && (*quiet.value()).is_timed_out());
    CHECK(IntervalTime::now().since(before) >= Duration::milliseconds(30));

    // A page has no host handles to wait on.
    const stxlib::i64 handles[1] = {0};
    Result<WakeWait> with_handle = channel.wait(stxlib::View<stxlib::i64>(handles), Duration::nanoseconds(0));
    CHECK(kind_of(with_handle) == ErrorKind::unsupported);

    Result<stxlib::Thread> started = stxlib::Thread::start(wake_later, &channel);
    CHECK(started.succeeded());
    if (started.failed()) return;
    stxlib::Thread waker = static_cast<Result<stxlib::Thread>&&>(started).take_value();
    Result<WakeWait> woken = channel.wait(stxlib::View<stxlib::i64>(), Duration::seconds(30));
    static_cast<void>(waker.join());
    CHECK(woken.succeeded() && (*woken.value()).is_woken());
    CHECK(channel.drain());

    WakeChannel moved = static_cast<WakeChannel&&>(channel);
    CHECK(!channel.is_open() && moved.is_open());
    Result<WakeWait> closed = channel.wait(stxlib::View<stxlib::i64>(), Duration::nanoseconds(0));
    CHECK(kind_of(closed) == ErrorKind::disposed);
}

// ---- SIMD ---------------------------------------------------------------------------

void simd_level() {
    namespace simd = stxlib::simd;
    const simd::Features features = simd::detected_features();
#if defined(__wasm_simd128__)
    CHECK(features.simd128);
    CHECK(simd::hardware_level() == simd::Level::vector128);
#else
    CHECK(!features.simd128);
    CHECK(simd::hardware_level() == simd::Level::scalar);
#endif
    CHECK(simd::level_name(simd::Level::vector128) == TextView("wasm-simd-128"));
}

// ---- what a page lacks ------------------------------------------------------------------

void absent_services() {
    Result<stxlib::PageBlock> pages = stxlib::PageBlock::map(stxlib::page_size());
    CHECK(kind_of(pages) == ErrorKind::unsupported);

    stxlib::uptr addresses[8]{};
    CHECK(stxlib::capture_stack_trace(stxlib::MutableView<stxlib::uptr>(addresses)) == 0);

    Result<stxlib::Library> library = stxlib::Library::open_system("libc.so");
    CHECK(kind_of(library) == ErrorKind::unsupported);

    const Result<stxlib::ProcessArguments> arguments = stxlib::process_arguments(4096);
    CHECK(kind_of(arguments) == ErrorKind::unsupported);
    CHECK(stxlib::process_id() == 0);

    // Child processes and the desktop fail (Emscripten's posix_spawn stub
    // answers ENOSYS); their capabilities say so first.
    CHECK(!stxlib::capability_supported(stxlib::Capability::child_processes));
    CHECK(!stxlib::capability_supported(stxlib::Capability::current_executable));
    const Result<Path> executable = stxlib::current_executable();
    CHECK(executable.failed());
}

void process_and_zone() {
    const Result<stxlib::ProcessMemory> memory = stxlib::process_memory();
    CHECK(memory.succeeded());
    if (memory.succeeded()) {
        CHECK((*memory.value()).resident_bytes > 0);
        CHECK((*memory.value()).peak_resident_bytes >= (*memory.value()).resident_bytes);
    }
    // The runtime's zone (Intl) or UTC; either names a compiled zone.
    CHECK(stxlib::remove_variable("TZ").succeeded());
    const Result<stxlib::Zone> zone = stxlib::host_zone();
    CHECK(zone.succeeded());
    const Result<stxlib::LocalTime> local = stxlib::local_time(stxlib::WallTime::now());
    CHECK(local.succeeded());
}

} // namespace

int main() {
    thread_facts();
    clocks();
    entropy();
    environment_and_home();
    storage_kinds();
    memory_volume();
    browser_volumes_absent();
    storage_waits_on_main_thread();
    storage_waits_from_thread();
    wake_channel();
    simd_level();
    absent_services();
    process_and_zone();
    const int status = stxlib_tests::finish("stxlib emscripten");
    return status;
}
