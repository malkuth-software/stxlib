// Tests of files and directories (include/stxlib/files.hpp), in a fresh
// directory under the host's temporary directory that the test removes
// again. Ported from stx_auxlib's tests/files_tests.cpp at b6e330a, with
// cases for the operations now written over host calls (copy onto itself,
// create_directories over a file, remove_tree of a nested tree with a link,
// a listing of a root), and for owner decision D13 a: the durability levels,
// and each host's odd names (POSIX bytes that are not UTF-8, ':' and '\';
// Windows' unpaired surrogates) made both through the host's own calls and
// through STXlib, then listed, read and removed by Path. Tested, not proved
// (proofs/REPORT.md).

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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using stxlib::Buffer;
using stxlib::Byte;
using stxlib::Durability;
using stxlib::ErrorKind;
using stxlib::File;
using stxlib::FileMode;
using stxlib::HostCode;
using stxlib::Path;
using stxlib::PathText;
using stxlib::Result;
using stxlib::Status;
using stxlib::TextView;
using stxlib::usize;

TextView view_of(const std::string& text) {
    Result<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

std::string text_of(const PathText text) {
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
        const long long stamp = stxlib::IntervalTime::now().count_nanoseconds();
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
    CHECK(stxlib::write_file(file, view_bytes(first), Durability::durable).succeeded());
    CHECK(read_or_empty(file, 1024) == first);
    // Replacement leaves no temporary file behind.
    const std::vector<Byte> second = bytes_of("second, longer content");
    CHECK(stxlib::write_file(file, view_bytes(second), Durability::ordered).succeeded());
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
    CHECK(stxlib::write_file(empty, stxlib::View<Byte>(), Durability::none).succeeded());
    CHECK(read_or_empty(empty, 0).empty());
    std::vector<Byte> large(3 * 1024 * 1024 + 17);
    for (usize index = 0; index < large.size(); ++index) large[index] = static_cast<Byte>(index * 131 + 7);
    const Path big = under(scratch.root, "big.bin");
    CHECK(stxlib::write_file(big, view_bytes(large), Durability::durable).succeeded());
    CHECK(read_or_empty(big, large.size()) == large);
    CHECK(kind_of(stxlib::read_file(under(scratch.root, "missing"), 16)) == ErrorKind::not_found);
    const Result<Buffer<Byte>> directory = stxlib::read_file(scratch.root, 16);
    CHECK(kind_of(directory) == ErrorKind::invalid_argument && code_of(directory) == HostCode::is_a_directory);
    // A path without a final name cannot be written as a file.
    const Status root_write = stxlib::write_file(parse_or_empty("/"), view_bytes(first), Durability::durable);
    CHECK(kind_of(root_write) == ErrorKind::invalid_argument && code_of(root_write) == HostCode::invalid_name);
    const Status dot_write = stxlib::write_file(Path(), view_bytes(first), Durability::none);
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
    CHECK(file.sync(Durability::none).succeeded());
    CHECK(file.sync(Durability::ordered).succeeded());
    CHECK(file.sync(Durability::durable).succeeded());
    CHECK(file.close().succeeded());
    CHECK(!file.is_open());
    const Status again = file.close();
    CHECK(kind_of(again) == ErrorKind::disposed && code_of(again) == HostCode::closed_handle);
    CHECK(kind_of(file.write_all(view_bytes(hello))) == ErrorKind::disposed);
    CHECK(kind_of(file.sync(Durability::none)) == ErrorKind::disposed);
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

// read_at, write_all_at, information, set_modified and set_size (M2 C2).
void positioned_and_metadata(const Scratch& scratch) {
    const Path path = under(scratch.root, "positioned.bin");
    Result<File> created = File::open(path, FileMode::create_new);
    CHECK(created.succeeded());
    if (created.failed()) return;
    File& file = *created.value();
    CHECK(file.write_all(view_bytes(bytes_of("0123456789"))).succeeded());
    // A positioned write leaves the position where the stream left it.
    CHECK(file.write_all_at(2, view_bytes(bytes_of("ab"))).succeeded());
    const Result<stxlib::u64> after_write = file.position();
    CHECK(after_write.succeeded() && *after_write.value() == 10);
    // Past the end extends the file; the gap reads as zeros.
    CHECK(file.write_all_at(12, view_bytes(bytes_of("Z"))).succeeded());
    const Result<stxlib::u64> grown = file.size();
    CHECK(grown.succeeded() && *grown.value() == 13);
    CHECK(file.write_all(view_bytes(bytes_of("!"))).succeeded());
    const Result<stxlib::u64> streamed = file.position();
    CHECK(streamed.succeeded() && *streamed.value() == 11);

    // information() describes the open file.
    const Result<stxlib::FileInformation> described = file.information();
    CHECK(described.succeeded() && (*described.value()).kind == stxlib::FileKind::regular &&
          (*described.value()).size == 13);

    // set_modified, at a time every tested filesystem keeps exactly
    // (a multiple of 100 ns).
    const stxlib::WallTime chosen = stxlib::WallTime::from_unix_nanoseconds(1700000000LL * 1000000000LL + 123456700LL);
    CHECK(file.set_modified(chosen).succeeded());
    const Result<stxlib::FileInformation> stamped = file.information();
    CHECK(stamped.succeeded() && (*stamped.value()).modified == chosen);
    const stxlib::WallTime early = stxlib::WallTime::from_unix_nanoseconds(-86400LL * 1000000000LL);
    CHECK(file.set_modified(early).succeeded());
    const Result<stxlib::FileInformation> early_read = stxlib::inspect(path, stxlib::LinkMode::follow);
    CHECK(early_read.succeeded() && (*early_read.value()).modified == early);

    // set_size cuts and extends; the position stays.
    CHECK(file.set_size(4).succeeded());
    const Result<stxlib::u64> cut = file.size();
    CHECK(cut.succeeded() && *cut.value() == 4);
    CHECK(file.set_size(8).succeeded());
    const Result<stxlib::u64> still = file.position();
    CHECK(still.succeeded() && *still.value() == 11);
    CHECK(file.close().succeeded());

    Result<File> reader = File::open(path, FileMode::read);
    CHECK(reader.succeeded());
    if (reader.failed()) return;
    File& input = *reader.value();
    Byte buffer[16]{};
    const Result<usize> got = input.read_at(0, stxlib::MutableView<Byte>(buffer));
    CHECK(got.succeeded() && *got.value() == 8);
    const Byte expected[8] = {'0', '1', 'a', 'b', 0, 0, 0, 0};
    CHECK(std::memcmp(buffer, expected, 8) == 0);
    const Result<stxlib::u64> unmoved = input.position();
    CHECK(unmoved.succeeded() && *unmoved.value() == 0);
    const Result<usize> past = input.read_at(100, stxlib::MutableView<Byte>(buffer));
    CHECK(past.succeeded() && *past.value() == 0);
    const Result<usize> huge = input.read_at(~static_cast<stxlib::u64>(0), stxlib::MutableView<Byte>(buffer));
    CHECK(kind_of(huge) == ErrorKind::limit_exceeded && code_of(huge) == HostCode::too_long);

    // Refusals by mode.
    CHECK(code_of(input.write_all_at(0, view_bytes(bytes_of("x")))) == HostCode::invalid_state);
    CHECK(code_of(input.set_modified(chosen)) == HostCode::invalid_state);
    CHECK(code_of(input.set_size(0)) == HostCode::invalid_state);
    Result<File> appender = File::open(path, FileMode::append);
    CHECK(appender.succeeded());
    if (appender.succeeded()) {
        File& tail = *appender.value();
        CHECK(code_of(tail.write_all_at(0, view_bytes(bytes_of("x")))) == HostCode::invalid_state);
        CHECK(code_of(tail.set_size(0)) == HostCode::invalid_state);
        CHECK(tail.set_modified(chosen).succeeded());
        const Result<stxlib::FileInformation> tail_described = tail.information();
        CHECK(tail_described.succeeded() && (*tail_described.value()).size == 8);
    }
    File closed;
    CHECK(kind_of(closed.read_at(0, stxlib::MutableView<Byte>(buffer))) == ErrorKind::disposed);
    CHECK(kind_of(closed.information()) == ErrorKind::disposed);
}

// remove_tree over a deep chain and a wide directory (more entries than one
// listing pass reads on Windows), counting exactly (M2 C2).
void deep_and_wide_trees(const Scratch& scratch) {
    std::string chain = "deep";
    for (int level = 0; level < 40; ++level) chain = chain + "/d";
    CHECK(stxlib::create_directories(under(scratch.root, chain.c_str())).succeeded());
    CHECK(stxlib::write_file(under(scratch.root, (chain + "/leaf").c_str()), view_bytes(bytes_of("x")),
                             Durability::none)
              .succeeded());
    const Result<stxlib::u64> deep = stxlib::remove_tree(under(scratch.root, "deep"));
    CHECK(deep.succeeded() && *deep.value() == 42);

    CHECK(stxlib::create_directory(under(scratch.root, "wide")).succeeded());
    for (int index = 0; index < 600; ++index) {
        const std::string name = "wide/entry-with-a-long-name-" + std::to_string(index);
        if (index % 50 == 0) {
            CHECK(stxlib::create_directory(under(scratch.root, name.c_str())).succeeded());
            CHECK(stxlib::write_file(under(scratch.root, (name + "/inner").c_str()), view_bytes(bytes_of("i")),
                                     Durability::none)
                      .succeeded());
        } else {
            CHECK(stxlib::write_file(under(scratch.root, name.c_str()), view_bytes(bytes_of("w")), Durability::none)
                      .succeeded());
        }
    }
    const Result<stxlib::u64> wide = stxlib::remove_tree(under(scratch.root, "wide"));
    // 600 entries, 12 inner files, the top.
    CHECK(wide.succeeded() && *wide.value() == 613);
    const Result<stxlib::FileInformation> gone = stxlib::inspect(under(scratch.root, "wide"), stxlib::LinkMode::no_follow);
    CHECK(kind_of(gone) == ErrorKind::not_found);
}

void metadata_and_directories(const Scratch& scratch) {
    const Path nested = under(scratch.root, "a/b/c");
    CHECK(stxlib::create_directories(nested).succeeded());
    CHECK(stxlib::create_directories(nested).succeeded());
    CHECK(kind_of(stxlib::create_directory(nested)) == ErrorKind::already_exists);
    const Path file = under(scratch.root, "a/note.txt");
    const stxlib::WallTime before = stxlib::WallTime::from_unix_nanoseconds(
        stxlib::WallTime::now().unix_nanoseconds() - stxlib::Duration::seconds(5).count_nanoseconds());
    CHECK(stxlib::write_file(file, view_bytes(bytes_of("12345")), Durability::durable).succeeded());
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
    // Listing is sorted by Path text bytes, whatever order the host returns.
    const char* const names[] = {"zeta", "Alpha", "beta", "_under", "\xc3\xa9t\xc3\xa9"};
    for (const char* const name : names) {
        const Status missing = stxlib::write_file(under(scratch.root, std::string("list/") + name), view_bytes(bytes_of(name)), Durability::none);
        CHECK(kind_of(missing) == ErrorKind::not_found);
    }
    CHECK(stxlib::create_directory(under(scratch.root, "list")).succeeded());
    for (const char* const name : names) {
        CHECK(stxlib::write_file(under(scratch.root, std::string("list/") + name), view_bytes(bytes_of(name)), Durability::ordered).succeeded());
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
    // Audit A, C3, over the held-directory walk (platform-rest): a limit
    // stops the walk with limit_exceeded, too_long; what was removed before
    // the limit stays removed, and a later call removes the rest.
    const auto empty_file = [&](const char* const name) {
        CHECK(stxlib::write_file(under(scratch.root, name), stxlib::View<Byte>(), Durability::none).succeeded());
    };
    CHECK(stxlib::create_directories(under(scratch.root, "bounded/x/y")).succeeded());
    empty_file("bounded/x/one");
    empty_file("bounded/x/two");
    stxlib::RemoveTreeLimits shallow{};
    shallow.depth = 1; // bounded is 0, x is 1, y would be 2
    const Result<stxlib::u64> too_deep = stxlib::remove_tree(under(scratch.root, "bounded"), shallow);
    CHECK(kind_of(too_deep) == ErrorKind::limit_exceeded && code_of(too_deep) == HostCode::too_long);
    CHECK(stxlib::inspect(under(scratch.root, "bounded/x/y"), stxlib::LinkMode::no_follow).succeeded());
    empty_file("bounded/x/one");
    empty_file("bounded/x/two");
    empty_file("bounded/x/three");
    stxlib::RemoveTreeLimits narrow{};
    narrow.entries_per_directory = 2; // x holds y, one, two and three
    const Result<stxlib::u64> too_wide = stxlib::remove_tree(under(scratch.root, "bounded"), narrow);
    CHECK(kind_of(too_wide) == ErrorKind::limit_exceeded && code_of(too_wide) == HostCode::too_long);
    CHECK(stxlib::inspect(under(scratch.root, "bounded/x"), stxlib::LinkMode::no_follow).succeeded());
    const Result<stxlib::u64> bounded = stxlib::remove_tree(under(scratch.root, "bounded"));
    CHECK(bounded.succeeded());
    CHECK(stxlib::inspect(under(scratch.root, "bounded"), stxlib::LinkMode::no_follow).failed());
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

    // A link to a directory outside the tree is removed, never entered: the
    // outside directory and its file survive (M2 C2).
    const Path outside = under(scratch.root, "outside");
    CHECK(stxlib::create_directory(outside).succeeded());
    CHECK(stxlib::write_file(under(scratch.root, "outside/keep"), view_bytes(bytes_of("k")), Durability::none)
              .succeeded());
    const Path second = under(scratch.root, "holder2/inner");
    CHECK(stxlib::create_directories(second).succeeded());
    const Status to_directory =
        stxlib::create_link(parse_or_empty("../../outside"), under(scratch.root, "holder2/inner/to-outside"));
    CHECK(to_directory.succeeded());
    const Result<stxlib::u64> unlinked = stxlib::remove_tree(under(scratch.root, "holder2"));
    CHECK(unlinked.succeeded() && *unlinked.value() == 3);
    CHECK(read_or_empty(under(scratch.root, "outside/keep"), 16) == bytes_of("k"));
    // remove_tree of a link at the top removes the link alone.
    const Path top_link = under(scratch.root, "top-link");
    CHECK(stxlib::create_link(parse_or_empty("outside"), top_link).succeeded());
    const Result<stxlib::u64> single = stxlib::remove_tree(top_link);
    CHECK(single.succeeded() && *single.value() == 1);
    CHECK(read_or_empty(under(scratch.root, "outside/keep"), 16) == bytes_of("k"));
    CHECK(stxlib::remove_tree(outside).succeeded());
}

void links_and_resolution(const Scratch& scratch) {
    const Path target = under(scratch.root, "target.txt");
    CHECK(stxlib::write_file(target, view_bytes(bytes_of("t")), Durability::durable).succeeded());
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

// ---- odd host names (D13 a) --------------------------------------------------

Path child_of(const Path& directory, const Path& name) {
    Result<Path> joined = directory.join(name);
    CHECK(joined.succeeded());
    if (joined.failed()) return Path();
    Path result = static_cast<Result<Path>&&>(joined).take_value();
    return result;
}

// The listing's entry equal to `name`, as an index, or count().
usize find_entry(const stxlib::DirectoryListing& listing, const PathText name) {
    usize index = 0;
    while (index < listing.count() && listing.name(index) != name) index = index + 1;
    return index;
}

// One odd name, made by the host as `host_made` and by STXlib as
// `stxlib_made`: both are listed under their Path names, read by Path, and
// the STXlib-made one is found by the host under its exact host name.
void check_odd_pair(const Path& directory, const Path& host_made, const Path& stxlib_made, const bool host_ok) {
    const std::vector<Byte> content = bytes_of("odd");
    CHECK(stxlib::write_file(child_of(directory, stxlib_made), view_bytes(content), Durability::durable).succeeded());
    Result<stxlib::DirectoryListing> listing = stxlib::list_directory(directory, 64);
    CHECK(listing.succeeded());
    if (listing.failed()) return;
    const stxlib::DirectoryListing& entries = *listing.value();
    CHECK(entries.unnamed_entries() == 0);
    CHECK(find_entry(entries, stxlib_made.text()) < entries.count());
    CHECK(!stxlib_made.text().is_unicode());
    if (host_ok) {
        const usize found = find_entry(entries, host_made.text());
        CHECK(found < entries.count());
        if (found < entries.count()) {
            // The listed name is a Path segment that reaches the file.
            Result<Path> listed = Path::parse_wtf8(entries.name(found).data(), entries.name(found).count());
            CHECK(listed.succeeded() && *listed.value() == host_made);
            if (listed.succeeded()) CHECK(read_or_empty(child_of(directory, *listed.value()), 16) == content);
        }
        CHECK(stxlib::remove(child_of(directory, host_made)).succeeded());
    }
    CHECK(read_or_empty(child_of(directory, stxlib_made), 16) == content);
}

#if defined(_WIN32)

// Creates `units` in `directory` with CreateFileW, writing "odd". False when
// the volume refuses the name.
bool host_create(const Path& directory, const std::vector<stxlib::u16>& units) {
    Result<Buffer<stxlib::u16>> native = directory.windows_name();
    CHECK(native.succeeded());
    if (native.failed()) return false;
    const stxlib::View<stxlib::u16> base = (*native.value()).view();
    std::wstring name(reinterpret_cast<const wchar_t*>(base.data()), base.count() - 1);
    name.push_back(L'\\');
    for (const stxlib::u16 unit : units) name.push_back(static_cast<wchar_t>(unit));
    const HANDLE file = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("odd names: the volume refused a name (error %lu)\n", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(file, "odd", 3, &written, nullptr) != 0 && written == 3;
    CHECK(wrote);
    CHECK(CloseHandle(file) != 0);
    return true;
}

// Whether the host finds `units` in `directory` (GetFileAttributesW).
bool host_exists(const Path& directory, const std::vector<stxlib::u16>& units) {
    Result<Buffer<stxlib::u16>> native = directory.windows_name();
    if (native.failed()) return false;
    const stxlib::View<stxlib::u16> base = (*native.value()).view();
    std::wstring name(reinterpret_cast<const wchar_t*>(base.data()), base.count() - 1);
    name.push_back(L'\\');
    for (const stxlib::u16 unit : units) name.push_back(static_cast<wchar_t>(unit));
    return GetFileAttributesW(name.c_str()) != INVALID_FILE_ATTRIBUTES;
}

Path windows_path(const std::vector<stxlib::u16>& units) {
    Result<Path> made = Path::from_windows_name(units.data(), units.size());
    CHECK(made.succeeded());
    if (made.failed()) return Path();
    Path result = static_cast<Result<Path>&&>(made).take_value();
    return result;
}

void odd_names(const Scratch& scratch) {
    const Path directory = under(scratch.root, "odd");
    CHECK(stxlib::create_directory(directory).succeeded());
    // NTFS keeps names as 16-bit units without checking that surrogates pair.
    const std::vector<stxlib::u16> lone_high{'h', 0xd800, '.', 't'};
    const std::vector<stxlib::u16> lone_low{0xdfff, 'l'};
    const bool host_ok = host_create(directory, lone_high);
    check_odd_pair(directory, windows_path(lone_high), windows_path(lone_low), host_ok);
    CHECK(host_exists(directory, lone_low));
    const Result<stxlib::u64> removed = stxlib::remove_tree(directory);
    CHECK(removed.succeeded() && *removed.value() == 2);
}

#else

std::string host_name(const Path& directory, const std::string& name) {
    Result<Buffer<char>> native = directory.posix_name();
    CHECK(native.succeeded());
    if (native.failed()) return std::string();
    const stxlib::View<char> base = (*native.value()).view();
    return std::string(base.data(), base.count() - 1) + "/" + name;
}

// Creates `name` (bytes) in `directory` with open(2), writing "odd". False
// when the filesystem refuses the name: APFS refuses a name that is not
// UTF-8 with EILSEQ (observed on macOS 26 with APFS while writing this
// batch; Apple's APFS documentation states APFS takes UTF-8 names).
bool host_create(const Path& directory, const std::string& name) {
    const std::string full = host_name(directory, name);
    const int fd = ::open(full.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        const int error = errno;
        std::printf("odd names: the filesystem refused a name (errno %d)\n", error);
        CHECK(error == EILSEQ || error == EINVAL);
        return false;
    }
    CHECK(::write(fd, "odd", 3) == 3);
    CHECK(::close(fd) == 0);
    return true;
}

bool host_exists(const Path& directory, const std::string& name) {
    struct stat described{};
    return ::lstat(host_name(directory, name).c_str(), &described) == 0;
}

Path posix_path(const std::string& name) {
    Result<Path> made = Path::from_posix_name(name.data(), name.size());
    CHECK(made.succeeded());
    if (made.failed()) return Path();
    Path result = static_cast<Result<Path>&&>(made).take_value();
    return result;
}

void odd_names(const Scratch& scratch) {
    const Path directory = under(scratch.root, "odd");
    CHECK(stxlib::create_directory(directory).succeeded());
    // Bytes that are not UTF-8: POSIX names are any bytes but '/' and NUL.
    const std::string invalid_host = "h\xff.t";
    const std::string invalid_stxlib = "s\xc3(";
    const bool invalid_ok = host_create(directory, invalid_host);
    const Status stxlib_write = stxlib::write_file(child_of(directory, posix_path(invalid_stxlib)),
                                                   view_bytes(bytes_of("odd")), Durability::none);
    const bool stxlib_invalid_ok = stxlib_write.succeeded();
    // Where the host refuses non-UTF-8 names (APFS: EILSEQ), STXlib reports
    // invalid_name; elsewhere both must work.
    CHECK(stxlib_invalid_ok == invalid_ok);
    if (!stxlib_invalid_ok) {
        CHECK(kind_of(stxlib_write) == ErrorKind::invalid_argument && code_of(stxlib_write) == HostCode::invalid_name);
    }
    if (stxlib_invalid_ok) {
        CHECK(stxlib::remove(child_of(directory, posix_path(invalid_stxlib))).succeeded());
        check_odd_pair(directory, posix_path(invalid_host), posix_path(invalid_stxlib), invalid_ok);
        CHECK(host_exists(directory, invalid_stxlib));
    }
    // ':' and '\' are ordinary bytes in POSIX names; Path escapes them.
    const bool reserved_ok = host_create(directory, "c:");
    CHECK(reserved_ok);
    check_odd_pair(directory, posix_path("c:"), posix_path("back\\slash"), reserved_ok);
    CHECK(host_exists(directory, "back\\slash"));
    // remove_tree reaches every entry whatever its name.
    const Result<stxlib::u64> removed = stxlib::remove_tree(directory);
    CHECK(removed.succeeded() && !host_exists(directory, ""));
}

#endif

// A Path no POSIX name spells (a Windows lone high surrogate) is refused by
// the file operations on POSIX hosts; Windows takes it.
void unrepresentable_names(const Scratch& scratch) {
    const stxlib::u16 units[] = {'w', 0xd800};
    Result<Path> name = Path::from_windows_name(units, 2);
    CHECK(name.succeeded());
    if (name.failed()) return;
    const Path path = child_of(scratch.root, *name.value());
    const Status written = stxlib::write_file(path, view_bytes(bytes_of("w")), Durability::none);
#if defined(_WIN32)
    CHECK(written.succeeded());
    CHECK(stxlib::remove(path).succeeded());
#else
    CHECK(kind_of(written) == ErrorKind::invalid_argument && code_of(written) == HostCode::invalid_name);
#endif
}

} // namespace

int main() {
    {
        const Scratch scratch{};
        whole_files(scratch);
        file_handles(scratch);
        positioned_and_metadata(scratch);
        deep_and_wide_trees(scratch);
        metadata_and_directories(scratch);
        links_and_resolution(scratch);
        odd_names(scratch);
        unrepresentable_names(scratch);
    }
    roots_and_host_forms();
    return stxlib_tests::finish("stxlib files tests");
}
