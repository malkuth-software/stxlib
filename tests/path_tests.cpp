// Tests of stxlib::Path (include/stxlib/path.hpp), ported from stx_auxlib's
// tests/paths_tests.cpp at b6e330a, with the cases for STXlib's changes:
// copy, a moved-from Path, and join and parent near the bound. Tested, not
// proved (proofs/REPORT.md).

#include "stxlib/path.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <string>

namespace {

using stxlib::Path;
using stxlib::PathFailure;
using stxlib::Result;
using stxlib::TextView;

TextView view_of(const std::string& text) {
    Result<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

std::string text_of(const Path& path) {
    const TextView text = path.text();
    std::string result(text.data(), text.count());
    return result;
}

Path parse_or_empty(const std::string& text) {
    Result<Path> parsed = Path::parse_bytes(text.data(), text.size());
    CHECK(parsed.succeeded());
    if (parsed.failed()) return Path();
    Path result = static_cast<Result<Path>&&>(parsed).take_value();
    return result;
}

PathFailure failure_of(const Result<Path>& result) {
    const PathFailure failure = static_cast<PathFailure>((*result.error()).code());
    return failure;
}

struct Normalization final {
    const char* input;
    const char* normalized;
};

const Normalization normalizations[] = {
    {"a", "a"},
    {".", "."},
    {"./", "."},
    {"a/./b", "a/b"},
    {"a//b///c", "a/b/c"},
    {"a/b/", "a/b"},
    {"a/b/..", "a"},
    {"a/..", "."},
    {"a/../..", ".."},
    {"../a", "../a"},
    {"../../a/../b", "../../b"},
    {"..", ".."},
    {"/", "/"},
    {"//", "/"},
    {"/a/b/../c/.", "/a/c"},
    {"/a/..", "/"},
    {"c:", "C:/"},
    {"c:/", "C:/"},
    {"C:/Users//me/./x/..", "C:/Users/me"},
    {"d:/a/..", "D:/"},
    {"caf\xc3\xa9/\xe6\x96\x87\xe4\xbb\xb6", "caf\xc3\xa9/\xe6\x96\x87\xe4\xbb\xb6"},
    {"\xf0\x9f\x98\x80.png", "\xf0\x9f\x98\x80.png"},
};

void normalization_table() {
    for (const Normalization& row : normalizations) {
        const Path path = parse_or_empty(row.input);
        CHECK(text_of(path) == row.normalized);
        // Normalization is idempotent.
        const Path again = parse_or_empty(text_of(path));
        CHECK(again == path);
    }
}

struct Refusal final {
    std::string input;
    PathFailure failure;
};

void refusals() {
    const Refusal rows[] = {
        {"", PathFailure::empty},
        {std::string("a\0b", 3), PathFailure::embedded_nul},
        {"a\\b", PathFailure::reserved_character},
        {"a:b", PathFailure::reserved_character},
        {"C:x", PathFailure::reserved_character},
        {"dir/C:/x", PathFailure::reserved_character},
        {"/..", PathFailure::escapes_root},
        {"/a/../..", PathFailure::escapes_root},
        {"C:/..", PathFailure::escapes_root},
        {"\xc0\xaf", PathFailure::invalid_utf8},         // overlong '/'
        {"\xe0\x80\xaf", PathFailure::invalid_utf8},     // overlong
        {"\xed\xa0\x80", PathFailure::invalid_utf8},     // surrogate
        {"\xf4\x90\x80\x80", PathFailure::invalid_utf8}, // above U+10FFFF
        {"\xc3", PathFailure::invalid_utf8},             // truncated
        {"\x80", PathFailure::invalid_utf8},             // lone continuation
        {"\xff", PathFailure::invalid_utf8},
    };
    for (const Refusal& row : rows) {
        const Result<Path> parsed = Path::parse_bytes(row.input.data(), row.input.size());
        CHECK(parsed.failed());
        if (parsed.failed()) {
            CHECK(failure_of(parsed) == row.failure);
            const stxlib::ErrorKind expected = row.failure == PathFailure::too_long ? stxlib::ErrorKind::limit_exceeded
                                                                                  : stxlib::ErrorKind::invalid_argument;
            CHECK((*parsed.error()).kind() == expected);
        }
    }
    // A TextView's own parse: the same rules, UTF-8 already established.
    const Result<Path> nul = Path::parse(TextView("a\0b"));
    CHECK(nul.failed() && failure_of(nul) == PathFailure::embedded_nul);
    const std::string longest(stxlib::maximum_path_bytes, 'a');
    CHECK(Path::parse(view_of(longest)).succeeded());
    const std::string too_long(stxlib::maximum_path_bytes + 1, 'a');
    const Result<Path> refused = Path::parse(view_of(too_long));
    CHECK(refused.failed() && failure_of(refused) == PathFailure::too_long);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::limit_exceeded);
}

std::string parent_text(const Path& path) {
    Result<Path> parent = path.parent();
    CHECK(parent.succeeded());
    if (parent.failed()) return std::string();
    const std::string result = text_of(*parent.value());
    return result;
}

void queries() {
    const Path relative = parse_or_empty("docs/report.tar.gz");
    CHECK(!relative.is_absolute() && !relative.is_root() && relative.drive() == 0);
    CHECK(relative.file_name() == TextView("report.tar.gz"));
    CHECK(relative.extension() == TextView(".gz"));
    CHECK(parent_text(relative) == "docs");
    const Path root = parse_or_empty("/");
    CHECK(root.is_absolute() && root.is_root());
    CHECK(parent_text(root) == "/" && root.file_name().empty());
    const Path drive = parse_or_empty("e:/");
    CHECK(drive.is_root() && drive.drive() == 'E');
    CHECK(parent_text(parse_or_empty("E:/a")) == "E:/");
    // Named paths: file_name() and extension() of a temporary are deleted
    // (the views would outlive it).
    const Path dot_profile = parse_or_empty(".profile");
    CHECK(dot_profile.extension().empty());
    const Path trailing_dot = parse_or_empty("a.");
    CHECK(trailing_dot.extension() == TextView("."));
    const Path no_extension = parse_or_empty("noext");
    CHECK(no_extension.extension().empty());
    const Path dot_dot_x = parse_or_empty("..x");
    CHECK(dot_dot_x.extension() == TextView(".x"));
    const Path current = parse_or_empty(".");
    CHECK(current.file_name().empty());
    const Path up_two = parse_or_empty("../..");
    CHECK(up_two.file_name().empty());
    CHECK(parent_text(parse_or_empty(".")) == "..");
    CHECK(parent_text(parse_or_empty("..")) == "../..");
    CHECK(parent_text(parse_or_empty("a")) == ".");
    const Path default_path{};
    CHECK(text_of(default_path) == ".");
    CHECK(default_path == parse_or_empty("."));
}

std::string joined_text(const Path& base, const char* const relative) {
    Result<Path> joined = base.join(parse_or_empty(relative));
    CHECK(joined.succeeded());
    if (joined.failed()) return std::string();
    const std::string result = text_of(*joined.value());
    return result;
}

void joins() {
    const Path base = parse_or_empty("/srv/data");
    CHECK(joined_text(base, "../logs/today") == "/srv/logs/today");
    const Result<Path> escaping = base.join(parse_or_empty("../../.."));
    CHECK(escaping.failed() && failure_of(escaping) == PathFailure::escapes_root);
    const Result<Path> absolute = base.join(parse_or_empty("/etc"));
    CHECK(absolute.failed() && failure_of(absolute) == PathFailure::absolute_join);
    CHECK(joined_text(parse_or_empty("."), "x/y") == "x/y");
    CHECK(joined_text(parse_or_empty("/"), "x") == "/x");
    CHECK(joined_text(parse_or_empty("C:/"), "x") == "C:/x");
}

// STXlib's changes: copy, moving, and the bound applied to results only.
void ownership_and_bounds() {
    Path original = parse_or_empty("a/b");
    Result<Path> copied = original.copy();
    CHECK(copied.succeeded() && *copied.value() == original);
    Path moved(static_cast<Path&&>(original));
    CHECK(text_of(moved) == "a/b");
    CHECK(text_of(original) == "."); // a moved-from Path reads "."
    // The longest path's parent: stx_auxlib parsed "<path>/.." (over the
    // bound) and returned the path unchanged.
    const std::string longest = std::string(stxlib::maximum_path_bytes - 2, 'a') + "/b";
    const Path at_bound = parse_or_empty(longest);
    CHECK(parent_text(at_bound) == std::string(stxlib::maximum_path_bytes - 2, 'a'));
    // A join whose operands together pass the bound but whose result fits.
    const Path near = parse_or_empty(std::string(stxlib::maximum_path_bytes - 2, 'a'));
    const std::string climb = ".." + std::string("/b");
    CHECK(joined_text(near, climb.c_str()) == "b");
    // A join whose result passes the bound.
    const Result<Path> over = near.join(parse_or_empty("bcd"));
    CHECK(over.failed() && failure_of(over) == PathFailure::too_long);
}

} // namespace

int main() {
    normalization_table();
    refusals();
    queries();
    joins();
    ownership_and_bounds();
    return stxlib_tests::finish("stxlib path tests");
}
