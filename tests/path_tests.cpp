// Tests of stxlib::Path (include/stxlib/path.hpp), ported from stx_auxlib's
// tests/paths_tests.cpp at b6e330a, with the cases for STXlib's changes:
// copy, a moved-from Path, and join and parent near the bound; and for the
// lossless host names of D13 a (owner decision, notes
// decisions/2026-11-13-d13-files-paths.md): each host's odd names round
// trip. Tested, not proved here; tests/path_names_exhaustive.cpp proves the
// round trips over its stated domains (proofs/REPORT.md).

#include "stxlib/path.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

namespace {

using stxlib::Path;
using stxlib::PathFailure;
using stxlib::PathText;
using stxlib::OperationResult;
using stxlib::TextView;
using stxlib::usize;

TextView view_of(const std::string& text) {
    OperationResult<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

std::string text_of(const Path& path) {
    const PathText text = path.text();
    std::string result(text.data(), text.count());
    return result;
}

Path parse_or_empty(const std::string& text) {
    OperationResult<Path> parsed = Path::parse_bytes(text.data(), text.size());
    CHECK(parsed.succeeded());
    if (parsed.failed()) return Path();
    Path result = static_cast<OperationResult<Path>&&>(parsed).take_value();
    return result;
}

PathFailure failure_of(const OperationResult<Path>& result) {
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
        const OperationResult<Path> parsed = Path::parse_bytes(row.input.data(), row.input.size());
        CHECK(parsed.failed());
        if (parsed.failed()) {
            CHECK(failure_of(parsed) == row.failure);
            const stxlib::ErrorKind expected = row.failure == PathFailure::too_long ? stxlib::ErrorKind::limit_exceeded
                                                                                  : stxlib::ErrorKind::invalid_argument;
            CHECK((*parsed.error()).kind() == expected);
        }
    }
    // A TextView's own parse: the same rules, UTF-8 already established.
    const OperationResult<Path> nul = Path::parse(TextView("a\0b"));
    CHECK(nul.failed() && failure_of(nul) == PathFailure::embedded_nul);
    const std::string longest(stxlib::maximum_path_bytes, 'a');
    CHECK(Path::parse(view_of(longest)).succeeded());
    const std::string too_long(stxlib::maximum_path_bytes + 1, 'a');
    const OperationResult<Path> refused = Path::parse(view_of(too_long));
    CHECK(refused.failed() && failure_of(refused) == PathFailure::too_long);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::limit_exceeded);
}

std::string parent_text(const Path& path) {
    OperationResult<Path> parent = path.parent();
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
    OperationResult<Path> joined = base.join(parse_or_empty(relative));
    CHECK(joined.succeeded());
    if (joined.failed()) return std::string();
    const std::string result = text_of(*joined.value());
    return result;
}

void joins() {
    const Path base = parse_or_empty("/srv/data");
    CHECK(joined_text(base, "../logs/today") == "/srv/logs/today");
    const OperationResult<Path> escaping = base.join(parse_or_empty("../../.."));
    CHECK(escaping.failed() && failure_of(escaping) == PathFailure::escapes_root);
    const OperationResult<Path> absolute = base.join(parse_or_empty("/etc"));
    CHECK(absolute.failed() && failure_of(absolute) == PathFailure::absolute_join);
    CHECK(joined_text(parse_or_empty("."), "x/y") == "x/y");
    CHECK(joined_text(parse_or_empty("/"), "x") == "/x");
    CHECK(joined_text(parse_or_empty("C:/"), "x") == "C:/x");
}

// STXlib's changes: copy, moving, and the bound applied to results only.
void ownership_and_bounds() {
    Path original = parse_or_empty("a/b");
    OperationResult<Path> copied = original.copy();
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
    const OperationResult<Path> over = near.join(parse_or_empty("bcd"));
    CHECK(over.failed() && failure_of(over) == PathFailure::too_long);
}

// ---- lossless host names (D13 a) --------------------------------------------

std::string posix_bytes(const Path& path) {
    OperationResult<stxlib::Buffer<char>> name = path.posix_name();
    CHECK(name.succeeded());
    if (name.failed()) return std::string("<failed>");
    const stxlib::View<char> spelled = (*name.value()).view();
    CHECK(spelled.count() >= 1 && spelled[spelled.count() - 1] == '\0');
    return std::string(spelled.data(), spelled.count() - 1);
}

Path from_posix(const std::string& bytes) {
    OperationResult<Path> made = Path::from_posix_name(bytes.data(), bytes.size());
    CHECK(made.succeeded());
    if (made.failed()) return Path();
    Path result = static_cast<OperationResult<Path>&&>(made).take_value();
    return result;
}

// POSIX: names are bytes; invalid UTF-8, ':' and '\' are escaped as U+DC00 +
// byte and come back exactly.
void posix_host_names() {
    struct Row final {
        std::string host;
        std::string text;
    };
    const Row rows[] = {
        {"plain", "plain"},
        {"caf\xc3\xa9", "caf\xc3\xa9"},                         // UTF-8 stays
        {"\xff", "\xed\xb3\xbf"},                               // U+DCFF
        {"a\x80" "b", "a\xed\xb2\x80" "b"},                      // lone continuation, U+DC80
        {"\xc3", "\xed\xb3\x83"},                               // truncated sequence
        {"\xc0\xaf", "\xed\xb3\x80\xed\xb2\xaf"},             // overlong '/', two escapes
        {"\xed\xa0\x80", "\xed\xb3\xad\xed\xb2\xa0\xed\xb2\x80"}, // a CESU surrogate's bytes
        {"c:", "c\xed\xb0\xba"},                                 // ':' is U+DC3A, so no drive
        {"a\\b", "a\xed\xb1\x9c" "b"},                         // '\' is U+DC5C
        {"/tmp/\xfe/x", "/tmp/\xed\xb3\xbe/x"},
    };
    for (const Row& row : rows) {
        const Path path = from_posix(row.host);
        CHECK(text_of(path) == row.text);
        CHECK(posix_bytes(path) == row.host);
        const bool unicode = row.text == row.host;
        CHECK(path.text().is_unicode() == unicode);
    }
    CHECK(from_posix("c:").drive() == 0 && !from_posix("c:").is_absolute());
    // A NUL is never in a POSIX name.
    const OperationResult<Path> nul = Path::from_posix_name("a\0b", 3);
    CHECK(nul.failed() && failure_of(nul) == PathFailure::embedded_nul);
    // Normalization applies to the escaped text, as to any.
    CHECK(text_of(from_posix("/a/./\xff/../b//")) == "/a/b");
}

// Text posix_name refuses: it spells no POSIX name, or not the one whose
// escape it is (so conversion stays injective).
void posix_refusals() {
    const std::string refused[] = {
        "\xed\xa0\x80",               // U+D800: not an escape
        "\xed\xb4\x80",               // U+DD00: not an escape
        "\xed\xb0\x81",               // U+DC01: byte 0x01 is never escaped
        "\xed\xb0\xaf",               // U+DC2F: '/' is never escaped
        "\xed\xb3\x83\xed\xb2\xa9",  // U+DCC3 U+DCA9: the bytes C3 A9 are UTF-8
        "C:/x",                        // a drive root's ':' is not an escape
    };
    for (const std::string& text : refused) {
        const OperationResult<Path> parsed = Path::parse_wtf8(text.data(), text.size());
        CHECK(parsed.succeeded());
        if (parsed.failed()) continue;
        const OperationResult<stxlib::Buffer<char>> name = (*parsed.value()).posix_name();
        CHECK(name.failed());
        if (name.failed()) {
            CHECK(static_cast<PathFailure>((*name.error()).code()) == PathFailure::not_representable);
            CHECK((*name.error()).kind() == stxlib::ErrorKind::invalid_argument);
        }
    }
}

std::vector<stxlib::u16> windows_units(const Path& path) {
    OperationResult<stxlib::Buffer<stxlib::u16>> name = path.windows_name();
    CHECK(name.succeeded());
    std::vector<stxlib::u16> result;
    if (name.failed()) return result;
    const stxlib::View<stxlib::u16> units = (*name.value()).view();
    CHECK(units.count() >= 1 && units[units.count() - 1] == 0);
    for (usize index = 0; index + 1 < units.count(); ++index) result.push_back(units[index]);
    return result;
}

// Windows: names are 16-bit units; unpaired surrogates are carried as
// themselves (WTF-8) and come back exactly.
void windows_host_names() {
    struct Row final {
        std::vector<stxlib::u16> host;
        std::string text;
    };
    const Row rows[] = {
        {{'a', 'b'}, "ab"},
        {{0xd800}, "\xed\xa0\x80"},                      // lone high
        {{0xdc00}, "\xed\xb0\x80"},                      // lone low
        {{0xdc00, 0xd800}, "\xed\xb0\x80\xed\xa0\x80"},  // low then high: two
        {{0xd83d, 0xde00}, "\xf0\x9f\x98\x80"},           // a pair: U+1F600
        {{'x', 0xdbff, '.', 't'}, "x\xed\xaf\xbf.t"},
        {{'C', ':', '\\', 0xdfff}, "C:/\xed\xbf\xbf"},    // a drive path, '\' separating
    };
    for (const Row& row : rows) {
        OperationResult<Path> made = Path::from_windows_name(row.host.data(), row.host.size());
        CHECK(made.succeeded());
        if (made.failed()) continue;
        const Path& path = *made.value();
        CHECK(text_of(path) == row.text);
        std::vector<stxlib::u16> expected = row.host;
        CHECK(windows_units(path) == expected);
    }
    // '/' separates too, and comes back as '\'.
    const stxlib::u16 slashed[] = {'a', '/', 0xd800};
    OperationResult<Path> made = Path::from_windows_name(slashed, 3);
    CHECK(made.succeeded() && windows_units(*made.value()) == (std::vector<stxlib::u16>{'a', '\\', 0xd800}));
    // ':' outside a drive root is reserved on Windows too.
    const stxlib::u16 stream[] = {'a', ':', 'b'};
    const OperationResult<Path> refused = Path::from_windows_name(stream, 3);
    CHECK(refused.failed() && failure_of(refused) == PathFailure::reserved_character);
}

// The longest host names are carried, not refused as too_long: a POSIX name
// of 4,096 invalid bytes (each escaped to 3 bytes) and a Windows name of
// 32,767 unpaired surrogates (each 3 bytes of WTF-8). One unit more than
// the bound holds is too_long.
void longest_host_names() {
    const std::string invalid(4096, '\xff');
    const Path escaped = from_posix(invalid);
    CHECK(escaped.text().count() == 3 * 4096);
    CHECK(posix_bytes(escaped) == invalid);
    const std::vector<stxlib::u16> highs(32767, 0xd800);
    OperationResult<Path> made = Path::from_windows_name(highs.data(), highs.size());
    CHECK(made.succeeded());
    if (made.succeeded()) {
        CHECK((*made.value()).text().count() == 3 * 32767);
        CHECK(windows_units(*made.value()) == highs);
    }
    const std::vector<stxlib::u16> at_bound(stxlib::maximum_path_bytes / 3, 0xd800);
    CHECK(Path::from_windows_name(at_bound.data(), at_bound.size()).succeeded());
    const std::vector<stxlib::u16> over(stxlib::maximum_path_bytes / 3 + 1, 0xd800);
    const OperationResult<Path> refused = Path::from_windows_name(over.data(), over.size());
    CHECK(refused.failed() && failure_of(refused) == PathFailure::too_long);
    // Names longer than the bound before any conversion are refused before
    // it (audit A, C2): the same failure, without the escaped copy.
    const std::string long_name(stxlib::maximum_path_bytes + 1, 'a');
    const OperationResult<Path> long_posix = Path::from_posix_name(long_name.data(), long_name.size());
    CHECK(long_posix.failed() && failure_of(long_posix) == PathFailure::too_long);
    const std::vector<stxlib::u16> long_units(stxlib::maximum_path_bytes + 1, 'a');
    const OperationResult<Path> long_windows = Path::from_windows_name(long_units.data(), long_units.size());
    CHECK(long_windows.failed() && failure_of(long_windows) == PathFailure::too_long);
}

// parse_wtf8 accepts lone surrogates and refuses a high one directly before a
// low one (that pair is a supplementary character's 4-byte form).
void wtf8_text() {
    const std::string lone = "a\xed\xa0\x80";
    CHECK(Path::parse_wtf8(lone.data(), lone.size()).succeeded());
    const std::string paired = "\xed\xa0\x80\xed\xb0\x80";
    const OperationResult<Path> refused = Path::parse_wtf8(paired.data(), paired.size());
    CHECK(refused.failed() && failure_of(refused) == PathFailure::invalid_utf8);
    const std::string broken = "\xed\xa0";
    const OperationResult<Path> truncated = Path::parse_wtf8(broken.data(), broken.size());
    CHECK(truncated.failed() && failure_of(truncated) == PathFailure::invalid_utf8);
    // parse_bytes stays UTF-8 only.
    const OperationResult<Path> strict = Path::parse_bytes(lone.data(), lone.size());
    CHECK(strict.failed() && failure_of(strict) == PathFailure::invalid_utf8);
    // PathText: a TextView converts; display shows U+FFFD for each surrogate.
    const Path path = from_posix("x\xffy");
    const PathText text = path.text();
    CHECK(!text.is_unicode());
    char room[16]{};
    const TextView shown = text.display(stxlib::MutableView<char>(room));
    CHECK(std::string(shown.data(), shown.count()) == "x\xef\xbf\xbdy");
    const Path plain = parse_or_empty("docs/a.txt");
    CHECK(plain.text().is_unicode() && plain.text().unicode() == TextView("docs/a.txt"));
    CHECK(plain.file_name() == "a.txt" && plain.extension() == PathText(TextView(".txt")));
    CHECK(path.file_name() == path.text() && path.extension().empty());
}

} // namespace

int main() {
    normalization_table();
    refusals();
    queries();
    joins();
    ownership_and_bounds();
    posix_host_names();
    posix_refusals();
    windows_host_names();
    longest_host_names();
    wtf8_text();
    return stxlib_tests::finish("stxlib path tests");
}
