// Tests of files and directories (include/stxlib/files.hpp), in a fresh
// directory under the host's temporary directory that the test removes
// again. Ported from stx_auxlib's tests/files_tests.cpp at b6e330a, with
// cases for the operations now written over host calls (copy onto itself,
// create_directories over a file, remove_tree of a nested tree with a link,
// a listing of a root). Tested, not proved (proofs/REPORT.md).

#include "stxlib/capabilities.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/files.hpp"
#include "stxlib/path.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using stxlib::Buffer;
using stxlib::Byte;
using stxlib::ErrorKind;
using stxlib::File;
using stxlib::FileMode;
using stxlib::HostCode;
using stxlib::Path;
using stxlib::Result;
using stxlib::Status;
using stxlib::TextView;
using stxlib::usize;

TextView view_of(const std::string& text) {
    Result<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

std::string text_of(const TextView text) {
    std::string result(text.data(), text.count());
    return result;
}

Path parse_or_empty(const std::string& text) {
    Result<Path> parsed = Path::parse(view_of(text));
    CHECK(parsed.succeeded());
    if (parsed.failed()) return Path();
    Path result = static_cast<Result<Path>&&>(parsed).take_value();
    return result;
}

Path under(const Path& base, const std::string& relative) {
    Result<Path> joined = base.join(parse_or_empty(relative));
    CHECK(joined.succeeded());
    if (joined.failed()) return Path();
    Path result = static_cast<Result<Path>&&>(joined).take_value();
    return result;
}

std::vector<Byte> bytes_of(const std::string& text) {
    // Each char converted explicitly: the range constructor's implicit
    // char -> unsigned char conversion halts the integer-checks build on a
    // byte above 0x7f.
    std::vector<Byte> result;
    result.reserve(text.size());
    for (const char unit : text) result.push_back(static_cast<Byte>(unit));
    return result;
}

stxlib::View<Byte> view_bytes(const std::vector<Byte>& bytes) {
    const stxlib::View<Byte> result(bytes.data(), bytes.size());
    return result;
}

std::vector<Byte> content_of(const Buffer<Byte>& buffer) {
    const stxlib::View<Byte> view = buffer.view();
    std::vector<Byte> result(view.data(), view.data() + view.count());
    return result;
}

ErrorKind kind_of(const Status& status) {
    const ErrorKind result = status.succeeded() ? static_cast<ErrorKind>(0) : (*status.error()).kind();
    return result;
}

template <typename Value>
ErrorKind kind_of(const Result<Value>& result) {
    const ErrorKind kind = result.succeeded() ? static_cast<ErrorKind>(0) : (*result.error()).kind();
    return kind;
}

template <typename Value>
HostCode code_of(const Result<Value>& result) {
    const HostCode code = result.succeeded() ? HostCode::none : static_cast<HostCode>((*result.error()).code());
    return code;
}

HostCode code_of(const Status& status) {
    const HostCode code = status.succeeded() ? HostCode::none : static_cast<HostCode>((*status.error()).code());
    return code;
}

std::vector<Byte> read_or_empty(const Path& path, const usize limit) {
    Result<Buffer<Byte>> read = stxlib::read_file(path, limit);
    CHECK(read.succeeded());
    if (read.failed()) return std::vector<Byte>();
    const std::vector<Byte> result = content_of(*read.value());
    return result;
}

// A scratch directory unique to this run, removed with everything in it.
class Scratch final {
public:
    Scratch() {
        Result<Path> temporary = stxlib::temporary_directory();
        CHECK(temporary.succeeded());
        if (temporary.failed()) return;
        const long long stamp = stxlib::MonotonicTime::now().count_nanoseconds();
        root = under(*temporary.value(), "stxlib-files-" + std::to_string(stamp));
        CHECK(stxlib::create_directory(root).succeeded());
    }
    ~Scratch() { (void)stxlib::remove_tree(root); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    Path root{};
};

void whole_files(const Scratch& scratch) {
    const Path file = under(scratch.root, "data.bin");
    const std::vector<Byte> first = bytes_of("first content");
    CHECK(stxlib::write_file(file, view_bytes(first)).succeeded());
    CHECK(read_or_empty(file, 1024) == first);
    // Replacement leaves no temporary file behind.
    const std::vector<Byte> second = bytes_of("second, longer content");
    CHECK(stxlib::write_file(file, view_bytes(second)).succeeded());
    CHECK(read_or_empty(file, 1024) == second);
    Result<stxlib::DirectoryListing> listing = stxlib::list_directory(scratch.root, 16);
    CHECK(listing.succeeded() && (*listing.value()).count() == 1 &&
          (*listing.value()).name(0) == TextView("data.bin"));
    // Limits: exactly at the limit succeeds, one byte over is refused.
    CHECK(read_or_empty(file, second.size()) == second);
    const Result<Buffer<Byte>> over = stxlib::read_file(file, second.size() - 1);
    CHECK(kind_of(over) == ErrorKind::limit_exceeded && code_of(over) == HostCode::too_long);
    // An empty file, and a larger one read in several chunks.
    const Path empty = under(scratch.root, "empty");
    CHECK(stxlib::write_file(empty, stxlib::View<Byte>()).succeeded());
    CHECK(read_or_empty(empty, 0).empty());
    std::vector<Byte> large(3 * 1024 * 1024 + 17);
    for (usize index = 0; index < large.size(); ++index) large[index] = static_cast<Byte>(index * 131 + 7);
    const Path big = under(scratch.root, "big.bin");
    CHECK(stxlib::write_file(big, view_bytes(large)).succeeded());
    CHECK(read_or_empty(big, large.size()) == large);
    CHECK(kind_of(stxlib::read_file(under(scratch.root, "missing"), 16)) == ErrorKind::not_found);
    const Result<Buffer<Byte>> directory = stxlib::read_file(scratch.root, 16);
    CHECK(kind_of(directory) == ErrorKind::invalid_argument && code_of(directory) == HostCode::is_a_directory);
    // A path without a final name cannot be written as a file.
    const Status root_write = stxlib::write_file(parse_or_empty("/"), view_bytes(first));
    CHECK(kind_of(root_write) == ErrorKind::invalid_argument && code_of(root_write) == HostCode::invalid_name);
    const Status dot_write = stxlib::write_file(Path(), view_bytes(first));
    CHECK(kind_of(dot_write) == ErrorKind::invalid_argument && code_of(dot_write) == HostCode::invalid_name);
}

void file_handles(const Scratch& scratch) {
    const Path path = under(scratch.root, "stream.txt");
    Result<File> created = File::open(path, FileMode::create_new);
    CHECK(created.succeeded());
    if (created.failed()) return;
    File& file = *created.value();
    const std::vector<Byte> hello = bytes_of("hello ");
    const std::vector<Byte> world = bytes_of("world");
    CHECK(file.write_all(view_bytes(hello)).succeeded());
    CHECK(file.write_all(view_bytes(world)).succeeded());
    const Result<stxlib::u64> written = file.size();
    const Result<stxlib::u64> at = file.position();
    CHECK(written.succeeded() && *written.value() == 11 && at.succeeded() && *at.value() == 11);
    CHECK(file.sync().succeeded());
    CHECK(file.close().succeeded());
    CHECK(!file.is_open());
    const Status again = file.close();
    CHECK(kind_of(again) == ErrorKind::disposed && code_of(again) == HostCode::closed_handle);
    CHECK(kind_of(file.write_all(view_bytes(hello))) == ErrorKind::disposed);
    CHECK(kind_of(File::open(path, FileMode::create_new)) == ErrorKind::already_exists);
    Result<File> appended = File::open(path, FileMode::append);
    CHECK(appended.succeeded());
    if (appended.succeeded()) {
        const std::vector<Byte> bang = bytes_of("!");
        CHECK((*appended.value()).write_all(view_bytes(bang)).succeeded());
        CHECK((*appended.value()).close().succeeded());
    }
    Result<File> reader = File::open(path, FileMode::read);
    CHECK(reader.succeeded());
    if (reader.failed()) return;
    File& input = *reader.value();
    CHECK(input.seek(6).succeeded());
    Byte buffer[16]{};
    const Result<usize> got = input.read(stxlib::MutableView<Byte>(buffer));
    CHECK(got.succeeded() && *got.value() == 6);
    CHECK(std::memcmp(buffer, "world!", 6) == 0);
    const Result<usize> end = input.read(stxlib::MutableView<Byte>(buffer));
    CHECK(end.succeeded() && *end.value() == 0);
    // Moving transfers the handle.
    File moved(static_cast<File&&>(input));
    CHECK(!input.is_open() && moved.is_open());
    Result<File> replaced = File::open(path, FileMode::replace);
    CHECK(replaced.succeeded());
    if (replaced.succeeded()) {
        const Result<stxlib::u64> emptied = (*replaced.value()).size();
        CHECK(emptied.succeeded() && *emptied.value() == 0);
    }
}

void metadata_and_directories(const Scratch& scratch) {
    const Path nested = under(scratch.root, "a/b/c");
    CHECK(stxlib::create_directories(nested).succeeded());
    CHECK(stxlib::create_directories(nested).succeeded());
    CHECK(kind_of(stxlib::create_directory(nested)) == ErrorKind::already_exists);
    const Path file = under(scratch.root, "a/note.txt");
    const stxlib::WallTime before = stxlib::WallTime::from_unix_nanoseconds(
        stxlib::WallTime::now().unix_nanoseconds() - stxlib::Duration::seconds(5).count_nanoseconds());
    CHECK(stxlib::write_file(file, view_bytes(bytes_of("12345"))).succeeded());
    // create_directories over a file: not a directory.
    const Status over_file = stxlib::create_directories(file);
    CHECK(kind_of(over_file) == ErrorKind::invalid_argument && code_of(over_file) == HostCode::not_a_directory);
    const Result<stxlib::FileInformation> information = stxlib::inspect(file, stxlib::LinkMode::follow);
    CHECK(information.succeeded());
    if (information.succeeded()) {
        const stxlib::FileInformation& described = *information.value();
        CHECK(described.kind == stxlib::FileKind::regular);
        CHECK(described.size == 5);
        const stxlib::WallTime after = stxlib::WallTime::from_unix_nanoseconds(
            stxlib::WallTime::now().unix_nanoseconds() + stxlib::Duration::seconds(5).count_nanoseconds());
        CHECK(!(described.modified < before) && described.modified < after);
    }
    const Result<stxlib::FileInformation> directory = stxlib::inspect(nested, stxlib::LinkMode::follow);
    CHECK(directory.succeeded() && (*directory.value()).kind == stxlib::FileKind::directory);
    CHECK(kind_of(stxlib::inspect(under(scratch.root, "nothing"), stxlib::LinkMode::follow)) == ErrorKind::not_found);
    // Listing is sorted by name bytes, whatever order the host returns.
    const char* const names[] = {"zeta", "Alpha", "beta", "_under", "\xc3\xa9t\xc3\xa9"};
    for (const char* const name : names) {
        const Status missing = stxlib::write_file(under(scratch.root, std::string("list/") + name), view_bytes(bytes_of(name)));
        CHECK(kind_of(missing) == ErrorKind::not_found);
    }
    CHECK(stxlib::create_directory(under(scratch.root, "list")).succeeded());
    for (const char* const name : names) {
        CHECK(stxlib::write_file(under(scratch.root, std::string("list/") + name), view_bytes(bytes_of(name))).succeeded());
    }
    Result<stxlib::DirectoryListing> listing = stxlib::list_directory(under(scratch.root, "list"), 16);
    CHECK(listing.succeeded());
    if (listing.succeeded()) {
        const stxlib::DirectoryListing& entries = *listing.value();
        const char* const sorted[] = {"Alpha", "_under", "beta", "zeta", "\xc3\xa9t\xc3\xa9"};
        CHECK(entries.count() == 5 && entries.unnamed_entries() == 0);
        for (usize index = 0; index < 5 && index < entries.count(); ++index) {
            CHECK(text_of(entries.name(index)) == sorted[index]);
            CHECK(entries.kind(index) == stxlib::FileKind::regular);
        }
    }
    const Result<stxlib::DirectoryListing> refused = stxlib::list_directory(under(scratch.root, "list"), 4);
    CHECK(kind_of(refused) == ErrorKind::limit_exceeded && code_of(refused) == HostCode::too_long);
    // Rename replaces; copy; remove and remove_tree.
    const Path renamed = under(scratch.root, "list/beta");
    CHECK(stxlib::rename(under(scratch.root, "list/zeta"), renamed).succeeded());
    CHECK(read_or_empty(renamed, 64) == bytes_of("zeta"));
    CHECK(kind_of(stxlib::copy_file(renamed, under(scratch.root, "list/Alpha"), stxlib::CopyMode::fail_if_exists)) ==
          ErrorKind::already_exists);
    CHECK(stxlib::copy_file(renamed, under(scratch.root, "list/copy"), stxlib::CopyMode::fail_if_exists).succeeded());
    CHECK(read_or_empty(under(scratch.root, "list/copy"), 64) == bytes_of("zeta"));
    // A copy onto itself is refused, not truncated.
    CHECK(kind_of(stxlib::copy_file(renamed, renamed, stxlib::CopyMode::replace)) == ErrorKind::already_exists);
    CHECK(read_or_empty(renamed, 64) == bytes_of("zeta"));
    const Status full = stxlib::remove(under(scratch.root, "list"));
    CHECK(kind_of(full) == ErrorKind::invalid_argument && code_of(full) == HostCode::directory_not_empty);
    CHECK(stxlib::remove(under(scratch.root, "list/copy")).succeeded());
    CHECK(kind_of(stxlib::remove(under(scratch.root, "list/copy"))) == ErrorKind::not_found);
    const Result<stxlib::u64> removed = stxlib::remove_tree(under(scratch.root, "list"));
    CHECK(removed.succeeded() && *removed.value() == 5);
    const Result<stxlib::u64> nothing = stxlib::remove_tree(under(scratch.root, "list"));
    CHECK(nothing.succeeded() && *nothing.value() == 0);
    // A nested tree: a/b/c, a/note.txt, and a itself.
    const Result<stxlib::u64> tree = stxlib::remove_tree(under(scratch.root, "a"));
    CHECK(tree.succeeded() && *tree.value() == 4);
}

// Creates `link` naming "target.txt". True when created; false when the host
// restricts links (Windows without developer mode or the symbolic link
// privilege reports permission_denied, as files.hpp states).
bool created_link(const Path& link) {
    const Status status = stxlib::create_link(parse_or_empty("target.txt"), link);
#if defined(_WIN32)
    if (kind_of(status) == ErrorKind::permission_denied) {
        std::puts("links: skipped (this host restricts symbolic links)");
        return false;
    }
#endif
    CHECK(status.succeeded());
    return status.succeeded();
}

void link_checks(const Scratch& scratch, const Path& target, const Path& link) {
    const Result<Path> named = stxlib::read_link(link);
    CHECK(named.succeeded() && text_of((*named.value()).text()) == "target.txt");
    const Result<stxlib::FileInformation> itself = stxlib::inspect(link, stxlib::LinkMode::no_follow);
    CHECK(itself.succeeded() && (*itself.value()).kind == stxlib::FileKind::symbolic_link);
    const Result<stxlib::FileInformation> followed = stxlib::inspect(link, stxlib::LinkMode::follow);
    CHECK(followed.succeeded() && (*followed.value()).kind == stxlib::FileKind::regular);
    const Result<Path> resolved = stxlib::resolve(link);
    const Result<Path> resolved_target = stxlib::resolve(target);
    CHECK(resolved.succeeded() && resolved_target.succeeded() && *resolved.value() == *resolved_target.value());
    // remove_tree removes the link, not its target.
    const Path holder = under(scratch.root, "holder");
    CHECK(stxlib::create_directory(holder).succeeded());
    const Status inner = stxlib::create_link(parse_or_empty("../target.txt"), under(scratch.root, "holder/to-target"));
    CHECK(inner.succeeded());
    const Result<stxlib::u64> removed = stxlib::remove_tree(holder);
    CHECK(removed.succeeded() && *removed.value() == 2);
    CHECK(read_or_empty(target, 16) == bytes_of("t"));
}

void links_and_resolution(const Scratch& scratch) {
    const Path target = under(scratch.root, "target.txt");
    CHECK(stxlib::write_file(target, view_bytes(bytes_of("t"))).succeeded());
    const Path link = under(scratch.root, "link");
    if (created_link(link)) link_checks(scratch, target, link);
    const Result<Path> working = stxlib::working_directory();
    CHECK(working.succeeded() && (*working.value()).is_absolute());
    if (working.failed()) return;
    const Result<Path> made = stxlib::absolute(parse_or_empty("x/../y"));
    CHECK(made.succeeded() && *made.value() == under(*working.value(), "y"));
    const Result<Path> unchanged = stxlib::absolute(scratch.root);
    CHECK(unchanged.succeeded() && *unchanged.value() == scratch.root);
}

void roots_and_host_forms() {
    // A root lists without a doubled separator.
#if defined(_WIN32)
    const Result<Path> working = stxlib::working_directory();
    CHECK(working.succeeded());
    if (working.failed()) return;
    std::string root_text = text_of((*working.value()).text()).substr(0, 3);
    const Path root = parse_or_empty(root_text);
#else
    const Path root = parse_or_empty("/");
#endif
    const Result<stxlib::DirectoryListing> listing = stxlib::list_directory(root, 1000000);
    CHECK(listing.succeeded() && (*listing.value()).count() > 0);
#if !defined(_WIN32)
    const Result<Buffer<Byte>> drive = stxlib::read_file(parse_or_empty("C:/x"), 16);
    CHECK(kind_of(drive) == ErrorKind::unsupported);
#endif
}

} // namespace

int main() {
    {
        const Scratch scratch{};
        whole_files(scratch);
        file_handles(scratch);
        metadata_and_directories(scratch);
        links_and_resolution(scratch);
    }
    roots_and_host_forms();
    return stxlib_tests::finish("stxlib files tests");
}
