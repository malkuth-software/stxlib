// stxlib::Path (include/stxlib/path.hpp). Pure computation; nothing here
// reaches a host. Ported from stx_auxlib's src/paths.cpp at b6e330a: the
// same rules, restated over Growable buffers so that exhaustion is a result.

#include "stxlib/path.hpp"

#include "core/growable.hpp"

namespace stxlib {

struct Path::Access final {
    [[nodiscard]] static Path adopt(Utf8String normalized) noexcept {
        Path result(static_cast<Utf8String&&>(normalized));
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

// Normalizes bytes[0, count), which is UTF-8 without NUL or '\', into
// `output` (without a terminator).
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

// The Path for normalized text in `output`.
Result<Path> adopt(const Growable<char>& output, const Operation operation) noexcept {
    // normalize copied whole UTF-8 sequences and ASCII separators only.
    const TextView text = detail::TextViewAccess::adopt_validated(output.data(), output.count());
    Result<Utf8String> owned = Utf8String::copy_of(text);
    if (owned.failed()) return Result<Path>::failure(exhausted(operation));
    Result<Path> result = Result<Path>::success(Path::Access::adopt(static_cast<Result<Utf8String>&&>(owned).take_value()));
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

// The checks parse makes before normalizing, in stx_auxlib's order: empty,
// too long, NUL, '\', then (for foreign bytes) UTF-8.
Result<Path> parse_checked(const char* const bytes, const usize count, const bool known_utf8) noexcept {
    const Operation operation = "stxlib.path.parse";
    if (count == 0) return Result<Path>::failure(refusal(PathFailure::empty, operation));
    if (count > maximum_path_bytes) return Result<Path>::failure(refusal(PathFailure::too_long, operation));
    for (usize index = 0; index < count; ++index) {
        if (bytes[index] == '\0') return Result<Path>::failure(refusal(PathFailure::embedded_nul, operation));
        if (bytes[index] == '\\') return Result<Path>::failure(refusal(PathFailure::reserved_character, operation));
    }
    if (!known_utf8) {
        const Result<TextView> valid = validate_utf8(bytes, count);
        if (valid.failed()) return Result<Path>::failure(refusal(PathFailure::invalid_utf8, operation));
    }
    Growable<char> output;
    const Normalized outcome = normalize(bytes, count, output);
    Result<Path> result = finish(outcome, output, operation);
    return result;
}

} // namespace

Result<Path> Path::parse(const TextView text) noexcept {
    Result<Path> result = parse_checked(text.data(), text.count(), true);
    return result;
}

Result<Path> Path::parse_bytes(const char* const bytes, const usize count) noexcept {
    STXLIB_REQUIRE(bytes != nullptr || count == 0, "Path::parse_bytes: a null pointer with a non-zero count");
    Result<Path> result = parse_checked(bytes, count, false);
    return result;
}

Result<Path> Path::copy() const noexcept {
    if (text_.empty()) return Result<Path>::success(Path());
    Result<Utf8String> owned = Utf8String::copy_of(text_.view());
    if (owned.failed()) return Result<Path>::failure(exhausted("stxlib.path.copy"));
    Result<Path> result = Result<Path>::success(Path(static_cast<Result<Utf8String>&&>(owned).take_value()));
    return result;
}

TextView Path::text() const& noexcept {
    if (text_.empty()) return TextView(".");
    const TextView result = text_.view();
    return result;
}

usize Path::root_length() const noexcept {
    const TextView spelled = (*this).text();
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
    const TextView left = text();
    const TextView right = relative.text();
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
    const TextView spelled = (*this).text();
    Growable<char> combined;
    if (!combined.append(spelled.data(), spelled.count()) || !combined.append("/..", 3)) {
        return Result<Path>::failure(exhausted(operation));
    }
    Growable<char> output;
    const Normalized outcome = normalize(combined.data(), combined.count(), output);
    Result<Path> result = finish(outcome, output, operation);
    return result;
}

TextView Path::file_name() const& noexcept {
    const TextView spelled = (*this).text();
    const char* const bytes = spelled.data();
    const usize root = root_length();
    usize start = spelled.count();
    while (start > root && bytes[start - 1] != '/') --start;
    const usize count = spelled.count() - start;
    if ((count == 1 && bytes[start] == '.') || (count == 2 && bytes[start] == '.' && bytes[start + 1] == '.')) {
        return TextView();
    }
    // A whole segment of UTF-8 text: it begins after an ASCII '/' (or at a
    // root's end) and runs to the end.
    const TextView result = detail::TextViewAccess::adopt_validated(bytes + start, count);
    return result;
}

TextView Path::extension() const& noexcept {
    const TextView name = file_name();
    const char* const bytes = name.data();
    usize dot = name.count();
    while (dot > 0 && bytes[dot - 1] != '.') --dot;
    // dot is one past the last '.', or 0 when there is none.
    if (dot <= 1) return TextView();
    const TextView result = detail::TextViewAccess::adopt_validated(bytes + dot - 1, name.count() - dot + 1);
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
