// stxlib::Path (include/stxlib/path.hpp). Pure computation; nothing here
// reaches a host. Ported from stx_auxlib's src/paths.cpp at b6e330a: the
// same rules, restated over Growable buffers so that exhaustion is a result,
// and over WTF-8 text (core/wtf8.hpp) so that host names are carried
// losslessly (D13 a).
//
// Normalization works on bytes and looks only at '/', '.' and ':', which are
// ASCII and so never inside a multi-byte sequence (every byte of a UTF-8 or
// WTF-8 sequence of two or more bytes is 0x80 or above). It keeps whole
// segments and joins them with '/', so its output is well-formed WTF-8 when
// its input is: a high and a low surrogate can only meet inside one segment,
// where the input already had them.

#include "stxlib/path.hpp"

#include "core/growable.hpp"
#include "core/wtf8.hpp"
#include "stxlib/require.hpp"
#include "stxlib/text.hpp"

#include <cstring>

namespace stxlib {

struct Path::Access final {
    [[nodiscard]] static Path adopt(Buffer<char> normalized) noexcept {
        Path result(static_cast<Buffer<char>&&>(normalized));
        return result;
    }
};

namespace {

bool is_ascii_letter(const char character) noexcept {
    const bool result = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    return result;
}

char upper_ascii(const char character) noexcept {
    if (character >= 'a' && character <= 'z') return static_cast<char>(character - 'a' + 'A');
    return character;
}

// The root of unnormalized text: its normalized spelling and how many input
// bytes it covers ("X:" covers two; the '/' after it is a separator).
struct Root final {
    char spelling[3]{};
    usize spelling_count{0};
    usize consumed{0};
};

Root find_root(const char* const bytes, const usize count) noexcept {
    Root root{};
    if (count == 0) return root;
    if (bytes[0] == '/') {
        root.spelling[0] = '/';
        root.spelling_count = 1;
        root.consumed = 1;
    } else if (count >= 2 && is_ascii_letter(bytes[0]) && bytes[1] == ':' && (count == 2 || bytes[2] == '/')) {
        root.spelling[0] = upper_ascii(bytes[0]);
        root.spelling[1] = ':';
        root.spelling[2] = '/';
        root.spelling_count = 3;
        root.consumed = 2;
    }
    return root;
}

// One kept segment: bytes[start, start + count) of the input.
struct Segment final {
    usize start{0};
    usize count{0};
};

bool is_dot(const char* const bytes, const Segment segment) noexcept {
    const bool result = segment.count == 1 && bytes[segment.start] == '.';
    return result;
}

bool is_dot_dot(const char* const bytes, const Segment segment) noexcept {
    const bool result = segment.count == 2 && bytes[segment.start] == '.' && bytes[segment.start + 1] == '.';
    return result;
}

enum class Normalized : u8 {
    ok = 0,
    reserved_character = 1,
    escapes_root = 2,
    too_long = 3,
    out_of_memory = 4,
};

// Normalizes bytes[0, count), which is well-formed WTF-8 without NUL or
// '\', into `output` (without a terminator).
Normalized normalize(const char* const bytes, const usize count, Growable<char>& output) noexcept {
    const Root root = find_root(bytes, count);
    for (usize index = root.consumed; index < count; ++index) {
        if (bytes[index] == ':') return Normalized::reserved_character;
    }
    Growable<Segment> segments;
    usize start = root.consumed;
    while (start <= count) {
        usize end = start;
        while (end < count && bytes[end] != '/') ++end;
        const Segment segment{start, end - start};
        start = end + 1;
        if (segment.count == 0 || is_dot(bytes, segment)) continue;
        if (is_dot_dot(bytes, segment)) {
            if (!segments.empty() && !is_dot_dot(bytes, segments[segments.count() - 1])) {
                segments.shrink(segments.count() - 1);
                continue;
            }
            if (root.spelling_count != 0) return Normalized::escapes_root;
        }
        if (!segments.push(segment)) return Normalized::out_of_memory;
    }
    output.clear();
    if (!output.append(root.spelling, root.spelling_count)) return Normalized::out_of_memory;
    for (usize index = 0; index < segments.count(); ++index) {
        if (index > 0 && !output.push('/')) return Normalized::out_of_memory;
        const Segment segment = segments[index];
        if (!output.append(bytes + segment.start, segment.count)) return Normalized::out_of_memory;
    }
    if (output.empty() && !output.push('.')) return Normalized::out_of_memory;
    if (output.count() > maximum_path_bytes) return Normalized::too_long;
    return Normalized::ok;
}

ErrorRecord refusal(const PathFailure failure, const Operation operation) noexcept {
    const ErrorKind kind = failure == PathFailure::too_long ? ErrorKind::limit_exceeded : ErrorKind::invalid_argument;
    const ErrorRecord result = ErrorRecord::make(kind, operation, ErrorDetail{.code = static_cast<u32>(failure)});
    return result;
}

ErrorRecord exhausted(const Operation operation) noexcept {
    const ErrorRecord result = ErrorRecord::make(ErrorKind::out_of_memory, operation);
    return result;
}

// An owned copy of bytes[0, count).
template <typename Element>
Result<Buffer<Element>> owned_copy(const Element* const elements, const usize count) noexcept {
    Result<Buffer<Element>> owned = Buffer<Element>::allocate_zeroed(count);
    if (owned.failed() || count == 0) return owned;
    std::memcpy((*owned.value()).mutable_view().data(), elements, count * sizeof(Element));
    return owned;
}

// The Path for normalized text in `output`.
Result<Path> adopt(const Growable<char>& output, const Operation operation) noexcept {
    Result<Buffer<char>> owned = owned_copy(output.data(), output.count());
    if (owned.failed()) return Result<Path>::failure(exhausted(operation));
    Result<Path> result = Result<Path>::success(Path::Access::adopt(static_cast<Result<Buffer<char>>&&>(owned).take_value()));
    return result;
}

Result<Path> finish(const Normalized outcome, const Growable<char>& output, const Operation operation) noexcept {
    switch (outcome) {
    case Normalized::ok:
        break;
    case Normalized::reserved_character:
        return Result<Path>::failure(refusal(PathFailure::reserved_character, operation));
    case Normalized::escapes_root:
        return Result<Path>::failure(refusal(PathFailure::escapes_root, operation));
    case Normalized::too_long:
        return Result<Path>::failure(refusal(PathFailure::too_long, operation));
    case Normalized::out_of_memory:
        return Result<Path>::failure(exhausted(operation));
    }
    Result<Path> result = adopt(output, operation);
    return result;
}

// What is known of the bytes parse_checked receives.
enum class Encoding : u8 {
    utf8 = 0,         // known UTF-8 (a TextView)
    check_utf8 = 1,   // must be checked as UTF-8
    check_wtf8 = 2,   // must be checked as WTF-8
    wtf8 = 3,         // known well-formed WTF-8 (made by core/wtf8.hpp)
};

// The checks parse makes before normalizing, in stx_auxlib's order: empty,
// too long, NUL, '\', then (for foreign bytes) the encoding.
Result<Path> parse_checked(const char* const bytes, const usize count, const Encoding encoding) noexcept {
    const Operation operation = "stxlib.path.parse";
    if (count == 0) return Result<Path>::failure(refusal(PathFailure::empty, operation));
    if (count > maximum_path_bytes) return Result<Path>::failure(refusal(PathFailure::too_long, operation));
    for (usize index = 0; index < count; ++index) {
        if (bytes[index] == '\0') return Result<Path>::failure(refusal(PathFailure::embedded_nul, operation));
        if (bytes[index] == '\\') return Result<Path>::failure(refusal(PathFailure::reserved_character, operation));
    }
    if (encoding == Encoding::check_utf8) {
        const Result<TextView> valid = validate_utf8(bytes, count);
        if (valid.failed()) return Result<Path>::failure(refusal(PathFailure::invalid_utf8, operation));
    } else if (encoding == Encoding::check_wtf8 && !wtf8::well_formed(bytes, count)) {
        return Result<Path>::failure(refusal(PathFailure::invalid_utf8, operation));
    }
    Growable<char> output;
    const Normalized outcome = normalize(bytes, count, output);
    Result<Path> result = finish(outcome, output, operation);
    return result;
}

bool is_surrogate_lead(const char* const bytes, const usize count, const usize index) noexcept {
    const bool result =
        index + 1 < count && static_cast<u8>(bytes[index]) == 0xed && static_cast<u8>(bytes[index + 1]) >= 0xa0;
    return result;
}

} // namespace

bool PathText::is_unicode() const noexcept {
    const bool result = !wtf8::has_surrogate(bytes_, count_);
    return result;
}

TextView PathText::unicode() const noexcept {
    STXLIB_REQUIRE(is_unicode(), "PathText::unicode: the text holds a surrogate");
    const TextView result = detail::TextViewAccess::adopt_validated(bytes_, count_);
    return result;
}

TextView PathText::display(const MutableView<char> room) const noexcept {
    STXLIB_REQUIRE(room.count() >= count_, "PathText::display: the room is smaller than the text");
    char* const output = room.data();
    usize index = 0;
    while (index < count_) {
        // A surrogate is ED A0..BF xx; U+FFFD is EF BF BD, also three bytes.
        if (is_surrogate_lead(bytes_, count_, index)) {
            output[index] = static_cast<char>(0xef);
            output[index + 1] = static_cast<char>(0xbf);
            output[index + 2] = static_cast<char>(0xbd);
            index = index + 3;
            continue;
        }
        output[index] = bytes_[index];
        index = index + 1;
    }
    // Well-formed WTF-8 with every surrogate replaced by a scalar value is
    // well-formed UTF-8.
    const TextView result = detail::TextViewAccess::adopt_validated(output, count_);
    return result;
}

Result<Path> Path::parse(const TextView text) noexcept {
    Result<Path> result = parse_checked(text.data(), text.count(), Encoding::utf8);
    return result;
}

Result<Path> Path::parse_bytes(const char* const bytes, const usize count) noexcept {
    STXLIB_REQUIRE(bytes != nullptr || count == 0, "Path::parse_bytes: a null pointer with a non-zero count");
    Result<Path> result = parse_checked(bytes, count, Encoding::check_utf8);
    return result;
}

Result<Path> Path::parse_wtf8(const char* const bytes, const usize count) noexcept {
    STXLIB_REQUIRE(bytes != nullptr || count == 0, "Path::parse_wtf8: a null pointer with a non-zero count");
    Result<Path> result = parse_checked(bytes, count, Encoding::check_wtf8);
    return result;
}

Result<Path> Path::from_posix_name(const char* const bytes, const usize count) noexcept {
    STXLIB_REQUIRE(bytes != nullptr || count == 0, "Path::from_posix_name: a null pointer with a non-zero count");
    // Escaping never shrinks the text, so a name longer than the bound is
    // refused before any storage is taken for it (audit A, C2).
    if (count > maximum_path_bytes) {
        return Result<Path>::failure(refusal(PathFailure::too_long, "stxlib.path.parse"));
    }
    Growable<char> escaped;
    if (!wtf8::escape_posix(bytes, count, escaped)) return Result<Path>::failure(exhausted("stxlib.path.parse"));
    // escape_posix writes well-formed WTF-8 (its only surrogates are low
    // ones) with no '\' or ':' left unescaped.
    Result<Path> result = parse_checked(escaped.data(), escaped.count(), Encoding::wtf8);
    return result;
}

Result<Path> Path::from_windows_name(const u16* const units, const usize count) noexcept {
    STXLIB_REQUIRE(units != nullptr || count == 0, "Path::from_windows_name: a null pointer with a non-zero count");
    // Every UTF-16 unit becomes at least one WTF-8 byte, so a name of more
    // units than the bound has bytes is refused before converting (audit A,
    // C2).
    if (count > maximum_path_bytes) {
        return Result<Path>::failure(refusal(PathFailure::too_long, "stxlib.path.parse"));
    }
    Growable<char> text;
    if (!wtf8::from_utf16(units, count, text)) return Result<Path>::failure(exhausted("stxlib.path.parse"));
    // '\' is ASCII, so it is never part of a multi-byte sequence.
    for (usize index = 0; index < text.count(); ++index) {
        if (text[index] == '\\') text[index] = '/';
    }
    Result<Path> result = parse_checked(text.data(), text.count(), Encoding::wtf8);
    return result;
}

Path::Path(const Path& other) noexcept : Path(take_or_fail_fast(other.copy())) {}

Path& Path::operator=(const Path& other) noexcept {
    if (this == &other) return *this;
    Path made = take_or_fail_fast(other.copy());
    *this = static_cast<Path&&>(made);
    return *this;
}

Result<Path> Path::copy() const noexcept {
    if (text_.empty()) return Result<Path>::success(Path());
    Result<Buffer<char>> owned = owned_copy(text_.view().data(), text_.count());
    if (owned.failed()) return Result<Path>::failure(exhausted("stxlib.path.copy"));
    Result<Path> result = Result<Path>::success(Path(static_cast<Result<Buffer<char>>&&>(owned).take_value()));
    return result;
}

PathText Path::text() const& noexcept {
    if (text_.empty()) return PathText(".");
    const PathText result = detail::PathTextAccess::adopt_validated(text_.view().data(), text_.count());
    return result;
}

Result<Buffer<char>> Path::posix_name() const noexcept {
    const Operation operation = "stxlib.path.posix_name";
    const PathText spelled = (*this).text();
    Growable<char> bytes;
    const wtf8::Converted converted = wtf8::unescape_posix(spelled.data(), spelled.count(), bytes);
    if (converted == wtf8::Converted::not_representable) {
        return Result<Buffer<char>>::failure(refusal(PathFailure::not_representable, operation));
    }
    if (converted != wtf8::Converted::ok || !bytes.push('\0')) return Result<Buffer<char>>::failure(exhausted(operation));
    Result<Buffer<char>> owned = owned_copy(bytes.data(), bytes.count());
    if (owned.failed()) return Result<Buffer<char>>::failure(exhausted(operation));
    return owned;
}

Result<Buffer<u16>> Path::windows_name() const noexcept {
    const Operation operation = "stxlib.path.windows_name";
    const PathText spelled = (*this).text();
    Growable<u16> units;
    if (!wtf8::to_utf16(spelled.data(), spelled.count(), units) || !units.push(0)) {
        return Result<Buffer<u16>>::failure(exhausted(operation));
    }
    for (usize index = 0; index < units.count(); ++index) {
        if (units[index] == u16{'/'}) units[index] = u16{'\\'};
    }
    Result<Buffer<u16>> owned = owned_copy(units.data(), units.count());
    if (owned.failed()) return Result<Buffer<u16>>::failure(exhausted(operation));
    return owned;
}

usize Path::root_length() const noexcept {
    const PathText spelled = (*this).text();
    const char* const bytes = spelled.data();
    if (bytes[0] == '/') return 1;
    if (spelled.count() >= 3 && bytes[1] == ':') return 3;
    return 0;
}

bool Path::is_absolute() const noexcept {
    const bool result = root_length() != 0;
    return result;
}

bool Path::is_root() const noexcept {
    const usize root = root_length();
    const bool result = root != 0 && text().count() == root;
    return result;
}

char Path::drive() const noexcept {
    if (root_length() == 3) return text().data()[0];
    return 0;
}

Result<Path> Path::join(const Path& relative) const noexcept {
    const Operation operation = "stxlib.path.join";
    if (relative.is_absolute()) return Result<Path>::failure(refusal(PathFailure::absolute_join, operation));
    const PathText left = text();
    const PathText right = relative.text();
    Growable<char> combined;
    if (!combined.append(left.data(), left.count()) || !combined.push('/') ||
        !combined.append(right.data(), right.count())) {
        return Result<Path>::failure(exhausted(operation));
    }
    Growable<char> output;
    const Normalized outcome = normalize(combined.data(), combined.count(), output);
    Result<Path> result = finish(outcome, output, operation);
    return result;
}

Result<Path> Path::parent() const noexcept {
    if (is_root()) {
        Result<Path> same = copy();
        return same;
    }
    const Operation operation = "stxlib.path.parent";
    const PathText spelled = (*this).text();
    Growable<char> combined;
    if (!combined.append(spelled.data(), spelled.count()) || !combined.append("/..", 3)) {
        return Result<Path>::failure(exhausted(operation));
    }
    Growable<char> output;
    const Normalized outcome = normalize(combined.data(), combined.count(), output);
    Result<Path> result = finish(outcome, output, operation);
    return result;
}

PathText Path::file_name() const& noexcept {
    const PathText spelled = (*this).text();
    const char* const bytes = spelled.data();
    const usize root = root_length();
    usize start = spelled.count();
    while (start > root && bytes[start - 1] != '/') --start;
    const usize count = spelled.count() - start;
    if ((count == 1 && bytes[start] == '.') || (count == 2 && bytes[start] == '.' && bytes[start + 1] == '.')) {
        return PathText();
    }
    // A whole segment of WTF-8 text: it begins after an ASCII '/' (or at a
    // root's end) and runs to the end.
    const PathText result = detail::PathTextAccess::adopt_validated(bytes + start, count);
    return result;
}

PathText Path::extension() const& noexcept {
    const PathText name = file_name();
    const char* const bytes = name.data();
    usize dot = name.count();
    while (dot > 0 && bytes[dot - 1] != '.') --dot;
    // dot is one past the last '.', or 0 when there is none.
    if (dot <= 1) return PathText();
    // From an ASCII '.' to the segment's end: whole sequences.
    const PathText result = detail::PathTextAccess::adopt_validated(bytes + dot - 1, name.count() - dot + 1);
    return result;
}

bool Path::operator==(const Path& other) const noexcept {
    const bool result = text() == other.text();
    return result;
}

bool Path::operator!=(const Path& other) const noexcept {
    const bool result = text() != other.text();
    return result;
}

} // namespace stxlib
