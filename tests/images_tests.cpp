// Tests of the loaded-image modules (A1 platform-rest): Library
// (library.hpp), describe_address and the image regions (images.hpp), and
// the stack trace (stack_trace.hpp). Tested, not proved: the hosts'
// loaders are outside proof scope; the bounded parsers under them
// (src/core/pe_image.hpp, src/core/stack_line.hpp) have their exhaustive
// checks in tests/platform_rest_exhaustive.cpp and their harnesses in
// proofs/esbmc/platform_rest.cpp.
//
// The host library each family names in open_system and find_loaded:
// libSystem.B.dylib (macOS), libc.so.6 (glibc), libc.so.7 (FreeBSD 15.2),
// kernel32.dll (Windows). musl's libc has no fixed file name across
// architectures (ld-musl-<arch>.so.1), so its open_system checks are
// skipped and only the refusals are tested there.

#include "stxlib/capabilities.hpp"
#include "stxlib/images.hpp"
#include "stxlib/library.hpp"
#include "stxlib/pages.hpp"
#include "stxlib/path.hpp"
#include "stxlib/stack_trace.hpp"
#include "stxlib/text.hpp"
#include "stxlib/threads.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <cstring>

namespace {

using stxlib::Byte;
using stxlib::ErrorKind;
using stxlib::HostCode;
using stxlib::Library;
using stxlib::OperationResult;
using stxlib::OperationStatus;
using stxlib::SymbolAddress;
using stxlib::TextView;
using stxlib::u32;
using stxlib::uptr;
using stxlib::usize;

template <typename T>
ErrorKind kind_of(const OperationResult<T>& result) {
    const ErrorKind kind = result.failed() ? (*result.error()).kind() : ErrorKind::internal_failure;
    return kind;
}

template <typename T>
HostCode code_of(const OperationResult<T>& result) {
    const HostCode code = result.failed() ? static_cast<HostCode>((*result.error()).code()) : HostCode::none;
    return code;
}

HostCode status_code(const OperationStatus& status) {
    const HostCode code = status.failed() ? static_cast<HostCode>((*status.error()).code()) : HostCode::none;
    return code;
}

#if defined(_WIN32)
constexpr const char* system_library = "kernel32.dll";
constexpr const char* system_symbol = "GetTickCount";
constexpr bool system_library_known = true;
#elif defined(__APPLE__)
constexpr const char* system_library = "libSystem.B.dylib";
constexpr const char* system_symbol = "puts";
constexpr bool system_library_known = true;
#elif defined(__FreeBSD__)
constexpr const char* system_library = "libc.so.7";
constexpr const char* system_symbol = "puts";
constexpr bool system_library_known = true;
#elif defined(__GLIBC__)
constexpr const char* system_library = "libc.so.6";
constexpr const char* system_symbol = "puts";
constexpr bool system_library_known = true;
#else
constexpr const char* system_library = "";
constexpr const char* system_symbol = "puts";
constexpr bool system_library_known = false;
#endif

TextView text_of_bytes(const char* const bytes, const usize count) {
    OperationResult<TextView> valid = stxlib::validate_utf8(bytes, count);
    const TextView result = valid.succeeded() ? *valid.value() : TextView();
    return result;
}

TextView text_of(const char* const text) {
    const TextView result = text_of_bytes(text, std::strlen(text));
    return result;
}

// ---- Library: refusals -------------------------------------------------------

void library_refuses_bad_names() {
    CHECK(stxlib::capability_supported(stxlib::Capability::dynamic_libraries));
    const char* const refused[] = {"", ".", "..", "lib/c.so", "lib\\c.dll"};
    for (const char* const name : refused) {
        OperationResult<Library> opened = Library::open_system(text_of(name));
        CHECK(opened.failed());
        CHECK(kind_of(opened) == ErrorKind::invalid_argument);
        CHECK(code_of(opened) == HostCode::invalid_name);
        OperationResult<Library> found = Library::find_loaded(text_of(name));
        CHECK(code_of(found) == HostCode::invalid_name);
    }
    const char with_nul[] = {'l', 'i', 'b', '\0', 'c'};
    OperationResult<Library> nul = Library::open_system(text_of_bytes(with_nul, sizeof(with_nul)));
    CHECK(code_of(nul) == HostCode::invalid_name);

    OperationResult<stxlib::Path> relative = stxlib::Path::parse(text_of("relative/libnothing.so"));
    CHECK(relative.succeeded());
    if (relative.succeeded()) {
        OperationResult<Library> opened = Library::open(*relative.value());
        CHECK(code_of(opened) == HostCode::invalid_name);
    }
}

void library_missing() {
    OperationResult<Library> missing = Library::open_system(text_of("libstxlib_no_such_library_0.so"));
    CHECK(missing.failed());
    CHECK(kind_of(missing) == ErrorKind::not_found);
    OperationResult<Library> absent = Library::find_loaded(text_of("libstxlib_no_such_library_0.so"));
    CHECK(kind_of(absent) == ErrorKind::not_found);
}

void library_closed() {
    Library closed;
    CHECK(!closed.is_open());
    OperationResult<SymbolAddress> symbol = closed.symbol(text_of("puts"));
    CHECK(kind_of(symbol) == ErrorKind::disposed);
    CHECK(code_of(symbol) == HostCode::closed_handle);
    const OperationStatus closing = closed.close();
    CHECK(closing.failed());
    CHECK(status_code(closing) == HostCode::closed_handle);
}

// ---- Library: the host's own library ------------------------------------------

void library_system() {
    if (!system_library_known) {
        std::printf("library_system: skipped (no fixed libc name on this host)\n");
        return;
    }
    OperationResult<Library> opened = Library::open_system(text_of(system_library));
    CHECK(opened.succeeded());
    if (opened.failed()) return;
    Library library = static_cast<OperationResult<Library>&&>(opened).take_value();
    CHECK(library.is_open());

    OperationResult<SymbolAddress> symbol = library.symbol(text_of(system_symbol));
    CHECK(symbol.succeeded());
    if (symbol.failed()) return;
    const uptr address = (*symbol.value()).address();
    CHECK(address != 0);

    // Refused and missing symbols.
    CHECK(code_of(library.symbol(text_of(""))) == HostCode::invalid_name);
    CHECK(code_of(library.symbol(text_of("two words"))) == HostCode::invalid_name);
    CHECK(code_of(library.symbol(text_of("stxlib_no_such_symbol_0"))) == HostCode::symbol_not_found);

    // The library is loaded already (the program links it), so find_loaded
    // finds it and resolves the same address.
    OperationResult<Library> found = Library::find_loaded(text_of(system_library));
    CHECK(found.succeeded());
    if (found.succeeded()) {
        OperationResult<SymbolAddress> again = (*found.value()).symbol(text_of(system_symbol));
        CHECK(again.succeeded() && (*again.value()).address() == address);
    }

    // The image holding the symbol, by name and by absolute path.
    char image_room[1024];
    char symbol_room[256];
    OperationResult<stxlib::AddressDescription> described =
        stxlib::describe_address(address, stxlib::MutableView<char>(image_room, sizeof(image_room)),
                                 stxlib::MutableView<char>(symbol_room, sizeof(symbol_room)));
    CHECK(described.succeeded());
    if (described.succeeded()) {
        const stxlib::AddressDescription& description = *described.value();
        CHECK(description.image_base != 0 && description.image_base <= address);
        CHECK(!description.image_name.empty());
        CHECK(!description.image_name_truncated);
        CHECK(description.symbol_address <= address);
        // A forwarded or aliased export may carry another name; when the
        // host names the symbol exactly, the addresses agree.
        const TextView expected = text_of(system_symbol);
        if (description.symbol_name.count() == expected.count() &&
            std::memcmp(description.symbol_name.data(), expected.data(), expected.count()) == 0) {
            CHECK(description.symbol_address == address);
        }
#if defined(_WIN32)
        OperationResult<stxlib::Path> path =
            stxlib::Path::parse_wtf8(description.image_name.data(), description.image_name.count());
#else
        OperationResult<stxlib::Path> path =
            stxlib::Path::parse_bytes(description.image_name.data(), description.image_name.count());
#endif
        CHECK(path.succeeded());
        if (path.succeeded() && (*path.value()).is_absolute()) {
            OperationResult<Library> by_path = Library::open(*path.value());
            CHECK(by_path.succeeded());
            if (by_path.succeeded()) {
                OperationResult<SymbolAddress> through_path = (*by_path.value()).symbol(text_of(system_symbol));
                CHECK(through_path.succeeded() && (*through_path.value()).address() == address);
                Library closing = static_cast<OperationResult<Library>&&>(by_path).take_value();
                CHECK(closing.close().succeeded());
                CHECK(!closing.is_open());
            }
        }
    }

    // Moves hand the reference on; the moved-from Library is closed.
    Library moved = static_cast<Library&&>(library);
    CHECK(moved.is_open());
    CHECK(!library.is_open());
    Library assigned;
    assigned = static_cast<Library&&>(moved);
    CHECK(assigned.is_open() && !moved.is_open());
    CHECK(assigned.close().succeeded());
}

// ---- describe_address -----------------------------------------------------------

void describe_own_code() {
    CHECK(stxlib::capability_supported(stxlib::Capability::address_lookup));
    const uptr address = reinterpret_cast<uptr>(&stxlib::page_size);
    char image_room[1024];
    char symbol_room[256];
    OperationResult<stxlib::AddressDescription> described =
        stxlib::describe_address(address, stxlib::MutableView<char>(image_room, sizeof(image_room)),
                                 stxlib::MutableView<char>(symbol_room, sizeof(symbol_room)));
    CHECK(described.succeeded());
    if (described.succeeded()) {
        CHECK((*described.value()).image_base <= address);
        CHECK(!(*described.value()).image_name.empty());
        // A static library's function is not a dynamic symbol on ELF hosts;
        // when a name is given, it lies at or below the address.
        CHECK((*described.value()).symbol_address <= address);
    }

    // Rooms too small: cut, with the flag, never past the room.
    char tiny_image[2];
    char tiny_symbol[1];
    OperationResult<stxlib::AddressDescription> cut =
        stxlib::describe_address(address, stxlib::MutableView<char>(tiny_image, sizeof(tiny_image)),
                                 stxlib::MutableView<char>(tiny_symbol, sizeof(tiny_symbol)));
    CHECK(cut.succeeded());
    if (cut.succeeded()) {
        CHECK((*cut.value()).image_name.count() <= sizeof(tiny_image));
        CHECK((*cut.value()).image_name_truncated);
        CHECK((*cut.value()).symbol_name.count() <= sizeof(tiny_symbol));
    }

    // Empty rooms are accepted.
    OperationResult<stxlib::AddressDescription> empty_rooms =
        stxlib::describe_address(address, stxlib::MutableView<char>(), stxlib::MutableView<char>());
    CHECK(empty_rooms.succeeded());
    if (empty_rooms.succeeded()) CHECK((*empty_rooms.value()).image_name.empty());

    // An address no image holds.
    OperationResult<stxlib::AddressDescription> nowhere =
        stxlib::describe_address(0, stxlib::MutableView<char>(image_room, sizeof(image_room)),
                                 stxlib::MutableView<char>(symbol_room, sizeof(symbol_room)));
    CHECK(nowhere.failed());
    CHECK(kind_of(nowhere) == ErrorKind::not_found);
    CHECK(code_of(nowhere) == HostCode::no_image);
}

// ---- image regions -------------------------------------------------------------

// The test's own region in each format (images.hpp, ImageRegionQuery).
// `retain` keeps it from the linker's section garbage collection.
#if defined(__APPLE__)
#define STXLIB_TEST_REGION __attribute__((section("__DATA,__stxtest"), used))
#elif defined(_WIN32)
#define STXLIB_TEST_REGION __attribute__((section(".stxtest"), used))
#else
#define STXLIB_TEST_REGION __attribute__((section(".note.stxtest"), used, retain, aligned(4)))
#endif

// An ELF note (owner "STXT", type 0x5354) holding eight known bytes; on
// Mach-O and PE the same bytes are simply the section's content.
struct TestNote final {
    u32 name_bytes;
    u32 descriptor_bytes;
    u32 type;
    char name[8];
    unsigned char descriptor[8];
};

STXLIB_TEST_REGION const TestNote test_note = {5, 8, 0x5354, {'S', 'T', 'X', 'T', '\0', 0, 0, 0},
                                               {0x53, 0x54, 0x58, 0x6c, 0x69, 0x62, 0x21, 0x0a}};

struct RegionSearch final {
    usize visits{0};
    bool found{false};
    bool stop_after_first{false};
};

bool contains_test_bytes(const stxlib::View<Byte> bytes) {
    const usize needed = sizeof(test_note.descriptor);
    if (bytes.count() < needed) return false;
    for (usize start = 0; start + needed <= bytes.count(); ++start) {
        if (std::memcmp(bytes.data() + start, test_note.descriptor, needed) == 0) return true;
    }
    return false;
}

// Walks the notes of an ELF note segment, by the alignment it states.
bool note_segment_holds_test_note(const stxlib::ImageRegion& region) {
    const usize align = region.alignment == 8 ? 8 : 4;
    const Byte* const bytes = region.bytes.data();
    const usize count = region.bytes.count();
    usize offset = 0;
    while (offset + 12 <= count) {
        u32 header[3];
        std::memcpy(header, bytes + offset, sizeof(header));
        const usize name_room = (static_cast<usize>(header[0]) + align - 1) / align * align;
        const usize descriptor_room = (static_cast<usize>(header[1]) + align - 1) / align * align;
        const usize name_start = offset + 12;
        if (name_room > count - name_start) return false;
        const usize descriptor_start = name_start + name_room;
        if (descriptor_room > count - descriptor_start) return false;
        if (header[0] == 5 && header[2] == 0x5354 && std::memcmp(bytes + name_start, "STXT", 5) == 0 &&
            header[1] == 8 && std::memcmp(bytes + descriptor_start, test_note.descriptor, 8) == 0) {
            return true;
        }
        offset = descriptor_start + descriptor_room;
    }
    return false;
}

bool search_visitor(const stxlib::ImageRegion& region, void* const context) noexcept {
    RegionSearch& search = *static_cast<RegionSearch*>(context);
    search.visits = search.visits + 1;
    bool holds = false;
    if (region.form == stxlib::ImageRegionForm::note_segment) {
        holds = note_segment_holds_test_note(region);
    } else {
        holds = region.bytes.count() >= sizeof(TestNote) && contains_test_bytes(region.bytes);
    }
    if (holds) search.found = true;
    const bool keep_going = !search.stop_after_first;
    return keep_going;
}

stxlib::ImageRegionQuery test_query() {
    stxlib::ImageRegionQuery query{};
    query.mach_o_segment = text_of("__DATA");
    query.mach_o_section = text_of("__stxtest");
    query.pe_section = text_of(".stxtest");
    return query;
}

void regions_found() {
    CHECK(stxlib::capability_supported(stxlib::Capability::image_regions));
    const uptr address = reinterpret_cast<uptr>(&test_note);

    RegionSearch own{};
    const OperationStatus containing = stxlib::visit_image_regions_containing(address, test_query(), search_visitor, &own);
    CHECK(containing.succeeded());
    CHECK(own.visits >= 1);
    CHECK(own.found);

    RegionSearch every{};
    const OperationStatus all = stxlib::visit_image_regions(test_query(), search_visitor, &every);
    CHECK(all.succeeded());
    CHECK(every.found);
    CHECK(every.visits >= own.visits);

    // A visitor that stops is called once.
    RegionSearch once{};
    once.stop_after_first = true;
    const OperationStatus stopped = stxlib::visit_image_regions(test_query(), search_visitor, &once);
    CHECK(stopped.succeeded());
    CHECK(once.visits == 1);
}

void regions_refused() {
    RegionSearch search{};
    const OperationStatus no_visitor = stxlib::visit_image_regions(test_query(), nullptr, &search);
    CHECK(status_code(no_visitor) == HostCode::invalid_name);

#if defined(__APPLE__) || defined(_WIN32)
    stxlib::ImageRegionQuery empty{};
    CHECK(status_code(stxlib::visit_image_regions(empty, search_visitor, &search)) == HostCode::invalid_name);
    stxlib::ImageRegionQuery long_names = test_query();
    long_names.mach_o_section = text_of("__a_name_of_17_bytes");
    long_names.pe_section = text_of(".ninebyte");
    CHECK(status_code(stxlib::visit_image_regions(long_names, search_visitor, &search)) == HostCode::invalid_name);
#endif

    const OperationStatus nowhere = stxlib::visit_image_regions_containing(0, test_query(), search_visitor, &search);
    CHECK(nowhere.failed());
    CHECK(status_code(nowhere) == HostCode::no_image);
    CHECK(search.visits == 0);
}

// ---- stack trace ------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define STXLIB_TEST_NOINLINE __attribute__((noinline))
#else
#define STXLIB_TEST_NOINLINE
#endif

STXLIB_TEST_NOINLINE usize capture_two_deep(const stxlib::MutableView<uptr> room) {
    const usize result = stxlib::capture_stack_trace(room);
    return result;
}

STXLIB_TEST_NOINLINE usize capture_one_deep(const stxlib::MutableView<uptr> room) {
    const usize result = capture_two_deep(room) + 0;
    return result;
}

void stack_trace() {
    CHECK(stxlib::capability_supported(stxlib::Capability::stack_trace));
    uptr addresses[stxlib::maximum_stack_trace_addresses + 8];
    const usize captured = capture_one_deep(stxlib::MutableView<uptr>(addresses, stxlib::maximum_stack_trace_addresses));
    CHECK(captured >= 2);
    CHECK(captured <= stxlib::maximum_stack_trace_addresses);
    for (usize index = 0; index < captured; ++index) CHECK(addresses[index] != 0);

    // A larger room than the maximum takes at most the maximum.
    const usize bounded = capture_one_deep(stxlib::MutableView<uptr>(addresses, stxlib::maximum_stack_trace_addresses + 8));
    CHECK(bounded <= stxlib::maximum_stack_trace_addresses);

    uptr one[1] = {0};
    CHECK(capture_one_deep(stxlib::MutableView<uptr>(one, 1)) <= 1);
    CHECK(stxlib::capture_stack_trace(stxlib::MutableView<uptr>()) == 0);

    // Written to standard error; the check is that it returns. An address
    // in no image prints "?".
    stxlib::write_stack_trace(stxlib::View<uptr>(addresses, captured < 4 ? captured : 4));
    const uptr nowhere[1] = {0};
    stxlib::write_stack_trace(stxlib::View<uptr>(nowhere, 1));
    stxlib::write_stack_trace(stxlib::View<uptr>());
}

// A Thread records its stack bounds before its entry runs (core/threads.cpp,
// run_started_thread; state.hpp, note_thread_stack), so a trace taken on it
// walks as the initial thread's does, whichever C library is loaded.
struct ThreadTrace final {
    usize captured{0};
};

void capture_on_thread(const stxlib::StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    ThreadTrace& trace = *static_cast<ThreadTrace*>(context);
    uptr addresses[stxlib::maximum_stack_trace_addresses];
    trace.captured = capture_one_deep(stxlib::MutableView<uptr>(addresses, stxlib::maximum_stack_trace_addresses));
}

void stack_trace_on_thread() {
    if (!stxlib::capability_supported(stxlib::Capability::threads)) return;
    if (!stxlib::capability_supported(stxlib::Capability::stack_trace)) return;
    ThreadTrace trace{};
    stxlib::OperationResult<stxlib::Thread> started = stxlib::Thread::start(capture_on_thread, &trace);
    CHECK(started.succeeded());
    if (started.failed()) return;
    stxlib::Thread thread = static_cast<stxlib::OperationResult<stxlib::Thread>&&>(started).take_value();
    CHECK(thread.join() == stxlib::JoinResult::joined);
    CHECK(trace.captured >= 2);
    CHECK(trace.captured <= stxlib::maximum_stack_trace_addresses);
}

} // namespace

int main() {
    library_refuses_bad_names();
    library_missing();
    library_closed();
    library_system();
    describe_own_code();
    regions_found();
    regions_refused();
    stack_trace();
    stack_trace_on_thread();
    const int status = stxlib_tests::finish("stxlib_images_tests");
    return status;
}
