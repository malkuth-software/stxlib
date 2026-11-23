// stxlib::settings (include/stxlib/settings.hpp): the schema, the text
// format, and the store's load, save, reload and finish, over files.hpp
// (read_file, write_file, identify, FileLock) and hash.hpp (BLAKE2s).
//
// Layout of this file: validation of names; the entry table and the text
// format (escape, parse, serialize); typed values; the schema; the store's
// files (snapshots, damaged files, conflict copies); load; save; reload;
// finish; the public members.
//
// Vocabulary. An entry table holds a file's entries as decoded text, in
// file order (a list is several entries under one key). The store keeps:
// - base: the table of the file as this store last read or wrote it;
// - snapshot: that file's identity, modification time and exact bytes;
// - values: the typed view the program reads (frozen, header);
// - pending: per declared key, whether this store changed it and, if so,
//   the exact values `base` held for it when it was first changed (the
//   "value it loaded" the conflict rule compares against).

#include "stxlib/settings.hpp"

#include "stxlib/clocks.hpp"
#include "stxlib/hash.hpp"
#include "stxlib/numeric_text.hpp"
#include "stxlib/require.hpp"
#include "stxlib/text_view.hpp"

#include "core/host_failure.hpp"

#include <cstring>

namespace stxlib::settings {

namespace detail {

struct Entry final {
    Utf8String key{};
    Utf8String value{};
    usize line{0}; // one-based line it was read from; 0 when made here
};

struct EntryTable final {
    Vector<Entry> entries{};
};

struct KeySpec final {
    Utf8String key{};
    ValueType type{ValueType::boolean};
    bool boolean_default{false};
    i64 integer_minimum{0};
    i64 integer_maximum{0};
    i64 integer_default{0};
    double real_minimum{0.0};
    double real_maximum{0.0};
    double real_default{0.0};
    usize text_limit{0};
    Utf8String text_default{};
    Vector<Utf8String> choices{};
    usize choice_default{0};
    usize item_limit{0};
};

struct StepRecord final {
    u32 from{0};
    MigrationStep step{nullptr};
};

struct SchemaState final {
    Utf8String part{};
    u32 version{1};
    Vector<KeySpec> keys{};
    Vector<StepRecord> steps{};
    usize file_limit{default_file_limit};
};

// One declared key's typed value. Only the field of the key's type is used.
struct Value final {
    bool boolean{false};
    i64 integer{0};
    double real{0.0};
    usize choice{0};
    Utf8String text{};
    Vector<Utf8String> items{};
};

struct Pending final {
    bool changed{false};
    bool conflicted{false};
    Vector<Utf8String> original{};
};

// A file as read: absent, or present with its identity, time and bytes.
struct Snapshot final {
    bool present{false};
    FileIdentity identity{};
    WallTime modified{};
    Buffer<Byte> bytes{};
};

struct StoreState final {
    const SchemaState* schema{nullptr};
    Utf8String identity{};
    Path directory{};
    Path file{};
    Path last_good{};
    Path lock{};
    Vector<Value> values{};
    Vector<Pending> pending{};
    EntryTable base{};
    u32 base_version{0}; // the version the file had on disk when read
    Snapshot snapshot{};
    // The snapshot read back whole with a matching checksum, so a last good
    // copy may be taken of its bytes.
    bool snapshot_valid{false};
    LoadState load{LoadState::fresh};
    bool read_only{false};
    Vector<Diagnostic> diagnostics{};
    Vector<u32> copies{};
};

class SettingsAccess final {
public:
    // Returned as a prvalue: MigrationEntries is neither copyable nor
    // movable (a borrow for one step), so only guaranteed elision builds it
    // in the caller.
    [[nodiscard]] static MigrationEntries entries_of(EntryTable& table) noexcept { return MigrationEntries(table); }
    [[nodiscard]] static const SchemaState* schema_of(const Schema& schema) noexcept {
        const SchemaState* const result = schema.state_.get();
        return result;
    }
    static void adopt(Store& store, Unique<StoreState>&& owner) noexcept {
        store.state_ = static_cast<Unique<StoreState>&&>(owner);
    }
};

} // namespace detail

namespace {

using detail::Entry;
using detail::EntryTable;
using detail::KeySpec;
using detail::Pending;
using detail::SchemaState;
using detail::Snapshot;
using detail::StoreState;
using detail::Value;

constexpr TextView header_line = "stxlib.settings 1";
constexpr TextView schema_prefix = "schema=";
constexpr TextView check_prefix = "check=blake2s:";
constexpr usize digest_hex_count = 64;

// ---- results ----------------------------------------------------------------

OperationStatus refused(const ErrorKind kind, const Operation operation, const HostCode code) noexcept {
    const OperationStatus result = host_failure::failed(kind, operation, code);
    return result;
}

OperationStatus exhausted(const Operation operation) noexcept {
    const OperationStatus result = OperationStatus::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
    return result;
}

// A slice of text known to be UTF-8 at ASCII delimiters (always boundaries).
TextView slice(const TextView text, const usize start, const usize count) noexcept {
    const TextView result = stxlib::detail::TextViewAccess::adopt_validated(text.data() + start, count);
    return result;
}

bool starts_with(const TextView text, const TextView prefix) noexcept {
    if (text.count() < prefix.count()) return false;
    const bool result = std::memcmp(text.data(), prefix.data(), prefix.count()) == 0;
    return result;
}

bool ends_with(const TextView text, const TextView suffix) noexcept {
    if (text.count() < suffix.count()) return false;
    const bool result =
        std::memcmp(text.data() + text.count() - suffix.count(), suffix.data(), suffix.count()) == 0;
    return result;
}

// ---- names ------------------------------------------------------------------

bool is_lower_letter(const char c) noexcept { return c >= 'a' && c <= 'z'; }
bool is_digit(const char c) noexcept { return c >= '0' && c <= '9'; }

// 1 to 64 bytes of a-z, 0-9, '_', '-' and '.', starting with a letter.
bool valid_key(const TextView key) noexcept {
    if (key.empty() || key.count() > maximum_key_bytes) return false;
    if (!is_lower_letter(key.data()[0])) return false;
    for (usize index = 0; index < key.count(); ++index) {
        const char c = key.data()[index];
        const bool allowed = is_lower_letter(c) || is_digit(c) || c == '_' || c == '-' || c == '.';
        if (!allowed) return false;
    }
    return true;
}

// 1 to 64 bytes of a-z, 0-9, '_' and '-'.
bool valid_part(const TextView part) noexcept {
    if (part.empty() || part.count() > maximum_part_bytes) return false;
    for (usize index = 0; index < part.count(); ++index) {
        const char c = part.data()[index];
        const bool allowed = is_lower_letter(c) || is_digit(c) || c == '_' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

// At least three non-empty parts of a-z, 0-9 and '-', separated by '.'.
bool valid_identity(const TextView identity) noexcept {
    if (identity.count() < 5 || identity.count() > maximum_identity_bytes) return false;
    usize parts = 1;
    usize part_length = 0;
    for (usize index = 0; index < identity.count(); ++index) {
        const char c = identity.data()[index];
        if (c == '.') {
            if (part_length == 0) return false;
            parts = parts + 1;
            part_length = 0;
            continue;
        }
        const bool allowed = is_lower_letter(c) || is_digit(c) || c == '-';
        if (!allowed) return false;
        part_length = part_length + 1;
    }
    const bool result = part_length != 0 && parts >= 3;
    return result;
}

// A key whose last segment names a secret (the header, "Secrets").
bool names_secret(const TextView key) noexcept {
    usize start = 0;
    for (usize index = 0; index < key.count(); ++index) {
        if (key.data()[index] == '.') start = index + 1;
    }
    const TextView last = slice(key, start, key.count() - start);
    const TextView secrets[] = {"password", "passphrase", "secret", "token", "api_key", "private_key"};
    const usize secret_count = sizeof secrets / sizeof secrets[0];
    for (usize index = 0; index < secret_count; ++index) {
        if (ends_with(last, secrets[index])) return true;
    }
    return false;
}

// ---- the entry table --------------------------------------------------------

bool push_entry(Vector<Entry>& entries, const TextView key, const TextView value, const usize line) noexcept {
    OperationResult<Utf8String> key_text = Utf8String::copy_of(key);
    if (key_text.failed()) return false;
    OperationResult<Utf8String> value_text = Utf8String::copy_of(value);
    if (value_text.failed()) return false;
    Entry entry{};
    entry.key = static_cast<OperationResult<Utf8String>&&>(key_text).take_value();
    entry.value = static_cast<OperationResult<Utf8String>&&>(value_text).take_value();
    entry.line = line;
    const OperationStatus pushed = entries.push(static_cast<Entry&&>(entry));
    return pushed.succeeded();
}

usize count_of(const EntryTable& table, const TextView key) noexcept {
    usize result = 0;
    for (usize index = 0; index < table.entries.count(); ++index) {
        if (table.entries[index].key.equals(key)) result = result + 1;
    }
    return result;
}

// The values of `key` in order, copied into `values`.
bool values_of(const EntryTable& table, const TextView key, Vector<Utf8String>& values) noexcept {
    values.clear();
    for (usize index = 0; index < table.entries.count(); ++index) {
        if (!table.entries[index].key.equals(key)) continue;
        OperationResult<Utf8String> copied = Utf8String::copy_of(table.entries[index].value.view());
        if (copied.failed()) return false;
        if (values.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).failed()) return false;
    }
    return true;
}

bool same_values(const Vector<Utf8String>& left, const Vector<Utf8String>& right) noexcept {
    if (left.count() != right.count()) return false;
    for (usize index = 0; index < left.count(); ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

// `table` with `key`'s values replaced by `values`, at the place of its first
// value (or at the end when it had none). Built into `output`.
bool replace_values(const EntryTable& table, const TextView key, const Vector<Utf8String>& values,
                    EntryTable& output) noexcept {
    output.entries.clear();
    bool placed = false;
    for (usize index = 0; index < table.entries.count(); ++index) {
        const Entry& entry = table.entries[index];
        if (entry.key.equals(key)) {
            if (placed) continue;
            placed = true;
            for (usize item = 0; item < values.count(); ++item) {
                if (!push_entry(output.entries, key, values[item].view(), 0)) return false;
            }
            continue;
        }
        if (!push_entry(output.entries, entry.key.view(), entry.value.view(), entry.line)) return false;
    }
    if (!placed) {
        for (usize item = 0; item < values.count(); ++item) {
            if (!push_entry(output.entries, key, values[item].view(), 0)) return false;
        }
    }
    return true;
}

bool copy_table(const EntryTable& table, EntryTable& output) noexcept {
    output.entries.clear();
    for (usize index = 0; index < table.entries.count(); ++index) {
        const Entry& entry = table.entries[index];
        if (!push_entry(output.entries, entry.key.view(), entry.value.view(), entry.line)) return false;
    }
    return true;
}

// ---- the text format --------------------------------------------------------

constexpr char hex_digits[] = "0123456789abcdef";

int hex_value(const char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Appends `value` escaped (the header, "The file").
bool append_escaped(Vector<char>& output, const TextView value) noexcept {
    for (usize index = 0; index < value.count(); ++index) {
        const unsigned char c = static_cast<unsigned char>(value.data()[index]);
        if (c == '\\') {
            if (output.push('\\').failed() || output.push('\\').failed()) return false;
        } else if (c == '\n') {
            if (output.push('\\').failed() || output.push('n').failed()) return false;
        } else if (c < 0x20 || c == 0x7f) {
            if (output.push('\\').failed() || output.push('x').failed()) return false;
            if (output.push_copy(hex_digits[c >> 4]).failed() || output.push_copy(hex_digits[c & 0xf]).failed()) return false;
        } else if (output.push(static_cast<char>(c)).failed()) {
            return false;
        }
    }
    return true;
}

// Decodes an escaped value into `output`; false for a malformed escape.
bool unescape(const TextView value, Vector<char>& output) noexcept {
    output.clear();
    usize index = 0;
    while (index < value.count()) {
        const char c = value.data()[index];
        if (c != '\\') {
            if (output.push_copy(c).failed()) return false;
            index = index + 1;
            continue;
        }
        if (index + 1 >= value.count()) return false;
        const char kind = value.data()[index + 1];
        if (kind == '\\') {
            if (output.push('\\').failed()) return false;
            index = index + 2;
        } else if (kind == 'n') {
            if (output.push('\n').failed()) return false;
            index = index + 2;
        } else if (kind == 'x') {
            if (index + 3 >= value.count()) return false;
            const int high = hex_value(value.data()[index + 2]);
            const int low = hex_value(value.data()[index + 3]);
            if (high < 0 || low < 0) return false;
            if (output.push(static_cast<char>((high << 4) | low)).failed()) return false;
            index = index + 4;
        } else {
            return false;
        }
    }
    return true;
}

bool append_text(Vector<char>& output, const TextView text) noexcept {
    for (usize index = 0; index < text.count(); ++index) {
        if (output.push_copy(text.data()[index]).failed()) return false;
    }
    return true;
}

hash::Digest digest_of(const char* const bytes, const usize count) noexcept {
    const View<Byte> input(reinterpret_cast<const Byte*>(bytes), count);
    const hash::Digest result = hash::blake2s(input);
    return result;
}

bool append_check(Vector<char>& output) noexcept {
    const hash::Digest digest = digest_of(output.view().data(), output.count());
    if (!append_text(output, check_prefix)) return false;
    for (usize index = 0; index < hash::digest_bytes; ++index) {
        const Byte b = digest.bytes[index];
        if (output.push_copy(hex_digits[b >> 4]).failed() || output.push_copy(hex_digits[b & 0xf]).failed()) return false;
    }
    const bool result = output.push('\n').succeeded();
    return result;
}

// The whole file text for `table` at `version`.
bool serialize(const TextView identity, const TextView part, const u32 version, const EntryTable& table,
               Vector<char>& output) noexcept {
    output.clear();
    if (!append_text(output, header_line) || output.push('\n').failed()) return false;
    if (!append_text(output, schema_prefix) || !append_text(output, identity) || output.push('/').failed() ||
        !append_text(output, part) || output.push(' ').failed()) {
        return false;
    }
    const text::NumberText number = text::to_text(static_cast<u64>(version));
    if (!append_text(output, number.view()) || output.push('\n').failed()) return false;
    for (usize index = 0; index < table.entries.count(); ++index) {
        const Entry& entry = table.entries[index];
        if (!append_text(output, entry.key.view()) || output.push('=').failed()) return false;
        if (!append_escaped(output, entry.value.view()) || output.push('\n').failed()) return false;
    }
    const bool result = append_check(output);
    return result;
}

enum class Verdict : u8 {
    whole = 0,
    damaged = 1,
    exhausted = 2,
};

struct ParsedFile final {
    u32 version{0};
    bool checksum_matches{false};
    EntryTable table{};
};

// Parses a whole file (the header, "Reading"). Damaged for any break of the
// form; a wrong checksum is reported, not refused.
Verdict parse_file(const View<Byte> bytes, ParsedFile& parsed) noexcept {
    parsed.version = 0;
    parsed.checksum_matches = false;
    parsed.table.entries.clear();
    const char* const data = reinterpret_cast<const char*>(bytes.data());
    const usize count = bytes.count();
    if (count == 0 || data[count - 1] != '\n') return Verdict::damaged;
    const OperationResult<TextView> valid = validate_utf8(data, count);
    if (valid.failed()) return Verdict::damaged;
    const TextView text = *valid.value();
    // Line starts: line k spans [start, end) without its LF.
    usize start = 0;
    usize line_number = 0;
    bool saw_check = false;
    Vector<char> decoded;
    while (start < count) {
        const void* const found = std::memchr(data + start, '\n', count - start);
        const usize end = static_cast<usize>(static_cast<const char*>(found) - data);
        const TextView line = slice(text, start, end - start);
        line_number = line_number + 1;
        if (saw_check) return Verdict::damaged; // nothing may follow the check line
        if (line_number == 1) {
            if (line != header_line) return Verdict::damaged;
        } else if (line_number == 2) {
            if (!starts_with(line, schema_prefix)) return Verdict::damaged;
            usize space = line.count();
            for (usize index = line.count(); index > schema_prefix.count(); --index) {
                if (line.data()[index - 1] == ' ') {
                    space = index - 1;
                    break;
                }
            }
            if (space == line.count()) return Verdict::damaged;
            const text::Parsed<u64> version = text::parse_unsigned(slice(line, space + 1, line.count() - space - 1));
            if (!version.succeeded() || *version.value() == 0 || *version.value() > 0xFFFFFFFFu) {
                return Verdict::damaged;
            }
            parsed.version = static_cast<u32>(*version.value());
        } else if (starts_with(line, check_prefix)) {
            if (line.count() != check_prefix.count() + digest_hex_count) return Verdict::damaged;
            const hash::Digest digest = digest_of(data, start);
            bool matches = true;
            for (usize index = 0; index < hash::digest_bytes; ++index) {
                const int high = hex_value(line.data()[check_prefix.count() + 2 * index]);
                const int low = hex_value(line.data()[check_prefix.count() + 2 * index + 1]);
                if (high < 0 || low < 0) return Verdict::damaged;
                if (static_cast<Byte>((high << 4) | low) != digest.bytes[index]) matches = false;
            }
            parsed.checksum_matches = matches;
            saw_check = true;
        } else {
            const void* const equals = std::memchr(line.data(), '=', line.count());
            if (equals == nullptr) return Verdict::damaged;
            const usize key_count = static_cast<usize>(static_cast<const char*>(equals) - line.data());
            const TextView key = slice(line, 0, key_count);
            if (!valid_key(key)) return Verdict::damaged;
            if (!unescape(slice(line, key_count + 1, line.count() - key_count - 1), decoded)) return Verdict::damaged;
            const OperationResult<TextView> value = validate_utf8(decoded.view().data(), decoded.count());
            if (value.failed()) return Verdict::damaged;
            if (!push_entry(parsed.table.entries, key, *value.value(), line_number)) return Verdict::exhausted;
        }
        start = end + 1;
    }
    if (line_number < 3 || !saw_check) return Verdict::damaged;
    return Verdict::whole;
}

// ---- typed values -----------------------------------------------------------

usize find_key(const SchemaState& schema, const TextView key) noexcept {
    for (usize index = 0; index < schema.keys.count(); ++index) {
        if (schema.keys[index].key.equals(key)) return index;
    }
    return schema.keys.count();
}

bool default_value(const KeySpec& spec, Value& value) noexcept {
    value.boolean = spec.boolean_default;
    value.integer = spec.integer_default;
    value.real = spec.real_default;
    value.choice = spec.choice_default;
    value.items.clear();
    const OperationStatus assigned = value.text.assign(spec.text_default.view());
    return assigned.succeeded();
}

// The value's text form, one entry per value (a list: one per item).
bool serialize_value(const KeySpec& spec, const Value& value, Vector<Utf8String>& output) noexcept {
    output.clear();
    TextView single{};
    text::NumberText number{};
    switch (spec.type) {
    case ValueType::boolean:
        single = value.boolean ? TextView("true") : TextView("false");
        break;
    case ValueType::integer:
        number = text::to_text(value.integer);
        single = number.view();
        break;
    case ValueType::real:
        number = text::to_text(value.real);
        single = number.view();
        break;
    case ValueType::text:
        single = value.text.view();
        break;
    case ValueType::choice:
        single = spec.choices[value.choice].view();
        break;
    case ValueType::text_list:
        for (usize index = 0; index < value.items.count(); ++index) {
            OperationResult<Utf8String> copied = Utf8String::copy_of(value.items[index].view());
            if (copied.failed()) return false;
            if (output.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).failed()) return false;
        }
        return true;
    default:
        return false;
    }
    OperationResult<Utf8String> copied = Utf8String::copy_of(single);
    if (copied.failed()) return false;
    const bool result = output.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).succeeded();
    return result;
}

enum class ReadValue : u8 {
    accepted = 0,
    absent = 1,
    refused = 2,
    exhausted = 3,
};

// Reads one scalar text as the key's type into `value`; refused when it
// does not fit.
ReadValue read_scalar(const KeySpec& spec, const TextView text, Value& value) noexcept {
    switch (spec.type) {
    case ValueType::boolean:
        if (text == TextView("true")) {
            value.boolean = true;
            return ReadValue::accepted;
        }
        if (text == TextView("false")) {
            value.boolean = false;
            return ReadValue::accepted;
        }
        return ReadValue::refused;
    case ValueType::integer: {
        const text::Parsed<i64> parsed = text::parse_signed(text);
        if (!parsed.succeeded()) return ReadValue::refused;
        const i64 number = *parsed.value();
        if (number < spec.integer_minimum || number > spec.integer_maximum) return ReadValue::refused;
        value.integer = number;
        return ReadValue::accepted;
    }
    case ValueType::real: {
        const text::Parsed<double> parsed = text::parse_double(text);
        if (!parsed.succeeded()) return ReadValue::refused;
        const double number = *parsed.value();
        if (!(number >= spec.real_minimum && number <= spec.real_maximum)) return ReadValue::refused;
        value.real = number;
        return ReadValue::accepted;
    }
    case ValueType::text:
        if (text.count() > spec.text_limit) return ReadValue::refused;
        if (value.text.assign(text).failed()) return ReadValue::exhausted;
        return ReadValue::accepted;
    case ValueType::choice:
        for (usize index = 0; index < spec.choices.count(); ++index) {
            if (spec.choices[index].equals(text)) {
                value.choice = index;
                return ReadValue::accepted;
            }
        }
        return ReadValue::refused;
    case ValueType::text_list:
    default:
        return ReadValue::refused;
    }
}

struct ReadIssue final {
    bool present{false};
    Issue issue{Issue::invalid_value};
    usize line{0};
};

// The key's value from `table` into `value` (its default where absent or
// unfit, with `problem` saying why).
bool read_value(const KeySpec& spec, const EntryTable& table, Value& value, ReadIssue& problem) noexcept {
    problem = ReadIssue{};
    if (!default_value(spec, value)) return false;
    const TextView key = spec.key.view();
    usize seen = 0;
    usize first_line = 0;
    for (usize index = 0; index < table.entries.count(); ++index) {
        const Entry& entry = table.entries[index];
        if (!entry.key.equals(key)) continue;
        seen = seen + 1;
        if (seen == 1) first_line = entry.line;
    }
    if (seen == 0) return true;
    if (spec.type == ValueType::text_list) {
        if (seen > spec.item_limit) {
            problem = ReadIssue{true, Issue::too_many_items, first_line};
            return true;
        }
        for (usize index = 0; index < table.entries.count(); ++index) {
            const Entry& entry = table.entries[index];
            if (!entry.key.equals(key)) continue;
            if (entry.value.count() > spec.text_limit) {
                value.items.clear();
                problem = ReadIssue{true, Issue::invalid_value, entry.line};
                return true;
            }
            OperationResult<Utf8String> copied = Utf8String::copy_of(entry.value.view());
            if (copied.failed()) return false;
            if (value.items.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).failed()) {
                return false;
            }
        }
        return true;
    }
    if (seen > 1) {
        problem = ReadIssue{true, Issue::duplicate_key, first_line};
        return true;
    }
    for (usize index = 0; index < table.entries.count(); ++index) {
        const Entry& entry = table.entries[index];
        if (!entry.key.equals(key)) continue;
        const ReadValue read = read_scalar(spec, entry.value.view(), value);
        if (read == ReadValue::exhausted) return false;
        if (read == ReadValue::refused) {
            if (!default_value(spec, value)) return false;
            problem = ReadIssue{true, Issue::invalid_value, entry.line};
        }
        return true;
    }
    return true;
}

bool add_diagnostic(StoreState& state, const Issue issue, const usize line, const TextView key) noexcept {
    Diagnostic diagnostic{};
    diagnostic.issue = issue;
    diagnostic.line = line;
    if (diagnostic.key.assign(key).failed()) return false;
    const bool result = state.diagnostics.push(static_cast<Diagnostic&&>(diagnostic)).succeeded();
    return result;
}

// ---- schema validation ------------------------------------------------------

bool finite(const double value) noexcept { return value == value && value - value == 0.0; }

OperationStatus check_new_key(const SchemaState& schema, const TextView key, const Operation operation) noexcept {
    if (!valid_key(key) || names_secret(key) || find_key(schema, key) != schema.keys.count()) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    }
    if (schema.keys.count() >= maximum_keys) return refused(ErrorKind::limit_exceeded, operation, HostCode::table_full);
    return OperationStatus::success();
}

OperationStatus add_spec(SchemaState& schema, KeySpec&& spec, const Operation operation) noexcept {
    if (schema.keys.push(static_cast<KeySpec&&>(spec)).failed()) return exhausted(operation);
    return OperationStatus::success();
}

OperationStatus make_spec(const TextView key, const ValueType type, KeySpec& spec, const Operation operation) noexcept {
    if (spec.key.assign(key).failed()) return exhausted(operation);
    spec.type = type;
    return OperationStatus::success();
}

// The oldest version every step reaches the schema's version from.
u32 oldest_readable(const SchemaState& schema) noexcept {
    u32 oldest = schema.version;
    while (oldest > 1) {
        bool found = false;
        for (usize index = 0; index < schema.steps.count(); ++index) {
            if (schema.steps[index].from == oldest - 1) found = true;
        }
        if (!found) break;
        oldest = oldest - 1;
    }
    return oldest;
}

MigrationStep step_from(const SchemaState& schema, const u32 from) noexcept {
    for (usize index = 0; index < schema.steps.count(); ++index) {
        if (schema.steps[index].from == from) return schema.steps[index].step;
    }
    return nullptr;
}

// ---- files ------------------------------------------------------------------

OperationResult<Path> child(const Path& directory, const TextView name) noexcept {
    OperationResult<Path> segment = Path::parse(name);
    if (segment.failed()) return segment;
    OperationResult<Path> result = directory.join(*segment.value());
    return result;
}

// `part` + `suffix` as one name.
bool name_of(const TextView part, const TextView suffix, Utf8String& output) noexcept {
    if (output.assign(part).failed()) return false;
    const bool result = output.append(suffix).succeeded();
    return result;
}

enum class ReadFile : u8 {
    read = 0,
    absent = 1,
    too_large = 2,
};

// Reads `path` whole into `snapshot` (absent when nothing is there).
OperationResult<ReadFile> read_snapshot(const Path& path, const usize limit, Snapshot& snapshot) noexcept {
    snapshot.present = false;
    snapshot.bytes = Buffer<Byte>();
    OperationResult<FileIdentity> identity = identify(path);
    if (identity.failed()) {
        if ((*identity.error()).kind() == ErrorKind::not_found) return OperationResult<ReadFile>::success(ReadFile::absent);
        return OperationResult<ReadFile>::failure(*identity.error());
    }
    OperationResult<FileInformation> information = inspect(path, LinkMode::follow);
    if (information.failed()) {
        if ((*information.error()).kind() == ErrorKind::not_found) {
            return OperationResult<ReadFile>::success(ReadFile::absent);
        }
        return OperationResult<ReadFile>::failure(*information.error());
    }
    OperationResult<Buffer<Byte>> bytes = read_file(path, limit);
    if (bytes.failed()) {
        const ErrorKind kind = (*bytes.error()).kind();
        if (kind == ErrorKind::not_found) return OperationResult<ReadFile>::success(ReadFile::absent);
        if (kind == ErrorKind::limit_exceeded) return OperationResult<ReadFile>::success(ReadFile::too_large);
        return OperationResult<ReadFile>::failure(*bytes.error());
    }
    snapshot.present = true;
    snapshot.identity = *identity.value();
    snapshot.modified = (*information.value()).modified;
    snapshot.bytes = static_cast<OperationResult<Buffer<Byte>>&&>(bytes).take_value();
    return OperationResult<ReadFile>::success(ReadFile::read);
}

bool same_snapshot(const Snapshot& left, const Snapshot& right) noexcept {
    if (left.present != right.present) return false;
    if (!left.present) return true;
    if (left.identity != right.identity || left.modified != right.modified) return false;
    if (left.bytes.count() != right.bytes.count()) return false;
    if (left.bytes.count() == 0) return true;
    const bool result = std::memcmp(left.bytes.view().data(), right.bytes.view().data(), left.bytes.count()) == 0;
    return result;
}

// The number in `name` = prefix + digits + suffix; 0 when it is not one.
u64 number_in(const PathText name, const TextView prefix, const TextView suffix) noexcept {
    if (!name.is_unicode()) return 0;
    const TextView text = name.unicode();
    if (!starts_with(text, prefix) || !ends_with(text, suffix)) return 0;
    if (text.count() <= prefix.count() + suffix.count()) return 0;
    const TextView digits = slice(text, prefix.count(), text.count() - prefix.count() - suffix.count());
    for (usize index = 0; index < digits.count(); ++index) {
        if (!is_digit(digits.data()[index])) return 0;
    }
    const text::Parsed<u64> parsed = text::parse_unsigned(digits);
    if (!parsed.succeeded()) return 0;
    return *parsed.value();
}

// The numbers of the files in `directory` named prefix + n + suffix, sorted.
bool numbered_files(const Path& directory, const TextView prefix, const TextView suffix,
                    Vector<u64>& numbers) noexcept {
    numbers.clear();
    constexpr usize listing_limit = 4096;
    OperationResult<DirectoryListing> listing = list_directory(directory, listing_limit);
    if (listing.failed()) return true; // nothing to list is no numbered file
    const DirectoryListing& entries = *listing.value();
    for (usize index = 0; index < entries.count(); ++index) {
        const u64 number = number_in(entries.name(index), prefix, suffix);
        if (number == 0) continue;
        usize at = numbers.count();
        while (at > 0 && numbers[at - 1] > number) at = at - 1;
        if (numbers.insert(at, static_cast<u64>(number)).failed()) return false;
    }
    return true;
}

bool numbered_name(const TextView prefix, const u64 number, const TextView suffix, Utf8String& output) noexcept {
    const text::NumberText digits = text::to_text(number);
    if (output.assign(prefix).failed() || output.append(digits.view()).failed()) return false;
    const bool result = output.append(suffix).succeeded();
    return result;
}

// Removes the oldest numbered files beyond kept_copies.
void prune(const Path& directory, const TextView prefix, const TextView suffix) noexcept {
    Vector<u64> numbers;
    if (!numbered_files(directory, prefix, suffix, numbers)) return;
    if (numbers.count() <= kept_copies) return;
    const usize surplus = numbers.count() - kept_copies;
    Utf8String name;
    for (usize index = 0; index < surplus; ++index) {
        if (!numbered_name(prefix, numbers[index], suffix, name)) return;
        OperationResult<Path> path = child(directory, name.view());
        if (path.succeeded()) static_cast<void>(remove(*path.value()));
    }
}

// Sets the damaged settings file aside as <part>.settings.damaged-<s>.
void set_aside(const StoreState& state) noexcept {
    Utf8String prefix;
    if (!name_of((*state.schema).part.view(), ".settings.damaged-", prefix)) return;
    const i64 seconds = WallTime::now().unix_seconds();
    const u64 stamp = seconds > 0 ? static_cast<u64>(seconds) : 1;
    Utf8String name;
    if (!numbered_name(prefix.view(), stamp, "", name)) return;
    OperationResult<Path> target = child(state.directory, name.view());
    if (target.failed()) return;
    static_cast<void>(rename(state.file, *target.value()));
    prune(state.directory, prefix.view(), "");
}

// ---- migration --------------------------------------------------------------

enum class Migrated : u8 {
    current = 0,
    newer = 1,
    too_old = 2,
    failed = 3,
};

Migrated migrate(const SchemaState& schema, ParsedFile& parsed) noexcept {
    if (parsed.version == schema.version) return Migrated::current;
    if (parsed.version > schema.version) return Migrated::newer;
    if (parsed.version < oldest_readable(schema)) return Migrated::too_old;
    for (u32 from = parsed.version; from < schema.version; ++from) {
        const MigrationStep step = step_from(schema, from);
        if (step == nullptr) return Migrated::failed;
        MigrationEntries entries = detail::SettingsAccess::entries_of(parsed.table);
        // STXLIB_EARLY_EXIT_WHOLE: the step runs on this table alone; no lock
        // is held and no store state is half-updated (the header).
        const OperationStatus stepped = step(entries);
        if (stepped.failed()) return Migrated::failed;
        parsed.version = from + 1;
    }
    return Migrated::current;
}

// ---- the store's state ------------------------------------------------------

// The typed view and diagnostics from `table`, every key's value replaced.
bool view_from(StoreState& state, const EntryTable& table, const bool report) noexcept {
    const SchemaState& schema = *state.schema;
    for (usize index = 0; index < schema.keys.count(); ++index) {
        ReadIssue problem{};
        if (!read_value(schema.keys[index], table, state.values[index], problem)) return false;
        if (report && problem.present &&
            !add_diagnostic(state, problem.issue, problem.line, schema.keys[index].key.view())) {
            return false;
        }
    }
    return true;
}

// One declared key's value from `table`, when this store has not changed it.
bool adopt_unchanged(StoreState& state, const EntryTable& table) noexcept {
    const SchemaState& schema = *state.schema;
    for (usize index = 0; index < schema.keys.count(); ++index) {
        if (state.pending[index].changed) continue;
        ReadIssue problem{};
        if (!read_value(schema.keys[index], table, state.values[index], problem)) return false;
    }
    return true;
}

// Interprets a parsed file into the store: migrated, newer or too old.
// Returns false on exhaustion.
bool take_file(StoreState& state, ParsedFile& parsed, const bool report) noexcept {
    const u32 read_version = parsed.version;
    const Migrated migrated = migrate(*state.schema, parsed);
    switch (migrated) {
    case Migrated::current:
        break;
    case Migrated::newer:
        state.read_only = true;
        if (report && !add_diagnostic(state, Issue::newer_version, 2, "")) return false;
        break;
    case Migrated::too_old:
        parsed.table.entries.clear();
        if (report && !add_diagnostic(state, Issue::too_old, 2, "")) return false;
        break;
    case Migrated::failed:
        parsed.table.entries.clear();
        if (report && !add_diagnostic(state, Issue::migration_failed, 2, "")) return false;
        break;
    default:
        return false;
    }
    if (!view_from(state, parsed.table, report)) return false;
    if (!copy_table(parsed.table, state.base)) return false;
    // The version on disk, before migration: a save writes an older file
    // again in the schema's version even when nothing changed.
    state.base_version = read_version;
    return true;
}

bool list_copies(StoreState& state) noexcept {
    state.copies.clear();
    Utf8String prefix;
    if (!name_of((*state.schema).part.view(), ".", prefix)) return false;
    Vector<u64> numbers;
    if (!numbered_files(state.directory, prefix.view(), ".settings", numbers)) return false;
    for (usize index = 0; index < numbers.count(); ++index) {
        if (numbers[index] > 0xFFFFFFFFu) continue;
        if (state.copies.push(static_cast<u32>(numbers[index])).failed()) return false;
    }
    return true;
}

constexpr Operation open_operation = "stxlib.settings.open";

// Reads the settings file under the lock (the header, "Reading").
OperationStatus load(StoreState& state) noexcept {
    const SchemaState& schema = *state.schema;
    OperationResult<ReadFile> read = read_snapshot(state.file, schema.file_limit, state.snapshot);
    if (read.failed()) return OperationStatus::failure(*read.error());
    if (*read.value() == ReadFile::absent) {
        state.load = LoadState::fresh;
        if (!view_from(state, state.base, false)) return exhausted(open_operation);
        return OperationStatus::success();
    }
    ParsedFile parsed{};
    Verdict verdict = Verdict::damaged;
    if (*read.value() == ReadFile::read) verdict = parse_file(state.snapshot.bytes.view(), parsed);
    if (verdict == Verdict::exhausted) return exhausted(open_operation);
    if (verdict == Verdict::whole) {
        if (!parsed.checksum_matches && !add_diagnostic(state, Issue::checksum_mismatch, 0, "")) {
            return exhausted(open_operation);
        }
        if (!take_file(state, parsed, true)) return exhausted(open_operation);
        state.snapshot_valid = parsed.checksum_matches;
        state.load = state.read_only ? LoadState::newer : LoadState::loaded;
        return OperationStatus::success();
    }
    // Damaged: the last good copy, restored over the file; else defaults.
    if (!add_diagnostic(state, Issue::damaged_file, 0, "")) return exhausted(open_operation);
    Snapshot good{};
    OperationResult<ReadFile> good_read = read_snapshot(state.last_good, schema.file_limit, good);
    ParsedFile good_parsed{};
    Verdict good_verdict = Verdict::damaged;
    if (good_read.succeeded() && *good_read.value() == ReadFile::read) {
        good_verdict = parse_file(good.bytes.view(), good_parsed);
    }
    if (good_verdict == Verdict::exhausted) return exhausted(open_operation);
    set_aside(state);
    if (good_verdict == Verdict::whole && good_parsed.checksum_matches) {
        if (!add_diagnostic(state, Issue::recovered, 0, "")) return exhausted(open_operation);
        const OperationStatus restored = write_file(state.file, good.bytes.view(), Durability::durable);
        if (restored.failed()) return restored;
        OperationResult<ReadFile> reread = read_snapshot(state.file, schema.file_limit, state.snapshot);
        if (reread.failed()) return OperationStatus::failure(*reread.error());
        if (!take_file(state, good_parsed, true)) return exhausted(open_operation);
        state.snapshot_valid = true;
        state.load = state.read_only ? LoadState::newer : LoadState::recovered;
        return OperationStatus::success();
    }
    state.snapshot.present = false;
    state.snapshot.bytes = Buffer<Byte>();
    state.base.entries.clear();
    state.base_version = 0;
    if (!view_from(state, state.base, false)) return exhausted(open_operation);
    state.load = LoadState::reset;
    return OperationStatus::success();
}

OperationResult<Path> lock_path_of(const Path& directory) noexcept {
    OperationResult<Path> result = child(directory, ".lock");
    return result;
}

// ---- save -------------------------------------------------------------------

constexpr Operation save_operation = "stxlib.settings.save";

usize pending_count(const StoreState& state) noexcept {
    usize result = 0;
    for (usize index = 0; index < state.pending.count(); ++index) {
        if (state.pending[index].changed) result = result + 1;
    }
    return result;
}

// What the disk holds now, as a table at the schema's version. `valid`:
// the file read back whole with a matching checksum (a last good copy may
// be taken of its bytes).
enum class DiskRead : u8 {
    same = 0,    // what this store last read or wrote
    changed = 1, // another writer's file, parsed into `table`
    absent = 2,  // no file
    damaged = 3, // a file that does not parse
    newer = 4,   // a newer program's file
};

OperationResult<DiskRead> read_disk(StoreState& state, Snapshot& now, EntryTable& table, bool& valid) noexcept {
    valid = false;
    OperationResult<ReadFile> read = read_snapshot(state.file, (*state.schema).file_limit, now);
    if (read.failed()) return OperationResult<DiskRead>::failure(*read.error());
    if (same_snapshot(now, state.snapshot)) {
        valid = now.present && state.snapshot_valid;
        if (!copy_table(state.base, table)) {
            return OperationResult<DiskRead>::failure(ErrorRecord::make(ErrorKind::out_of_memory, save_operation));
        }
        return OperationResult<DiskRead>::success(DiskRead::same);
    }
    if (*read.value() == ReadFile::absent) return OperationResult<DiskRead>::success(DiskRead::absent);
    if (*read.value() == ReadFile::too_large) return OperationResult<DiskRead>::success(DiskRead::damaged);
    ParsedFile parsed{};
    const Verdict verdict = parse_file(now.bytes.view(), parsed);
    if (verdict == Verdict::exhausted) {
        return OperationResult<DiskRead>::failure(ErrorRecord::make(ErrorKind::out_of_memory, save_operation));
    }
    if (verdict == Verdict::damaged) return OperationResult<DiskRead>::success(DiskRead::damaged);
    valid = parsed.checksum_matches;
    const Migrated migrated = migrate(*state.schema, parsed);
    if (migrated == Migrated::newer) return OperationResult<DiskRead>::success(DiskRead::newer);
    if (migrated != Migrated::current) parsed.table.entries.clear();
    if (!copy_table(parsed.table, table)) {
        return OperationResult<DiskRead>::failure(ErrorRecord::make(ErrorKind::out_of_memory, save_operation));
    }
    return OperationResult<DiskRead>::success(DiskRead::changed);
}

bool note_conflict(StoreState& state, const usize index) noexcept {
    Pending& pending = state.pending[index];
    if (pending.conflicted) return true;
    pending.conflicted = true;
    const bool result = add_diagnostic(state, Issue::conflict, 0, (*state.schema).keys[index].key.view());
    return result;
}

// Merges this store's changes into the disk's table (decision 1, option a):
// a changed key is written where the disk still holds the value this store
// loaded for it; otherwise it is a conflict. `written` counts the keys
// applied; `applied[i]` marks them.
bool merge(StoreState& state, const EntryTable& disk, EntryTable& merged, Vector<bool>& applied,
           usize& written) noexcept {
    const SchemaState& schema = *state.schema;
    written = 0;
    applied.clear();
    if (!copy_table(disk, merged)) return false;
    Vector<Utf8String> on_disk;
    Vector<Utf8String> mine;
    EntryTable next{};
    for (usize index = 0; index < schema.keys.count(); ++index) {
        if (applied.push(false).failed()) return false;
        Pending& pending = state.pending[index];
        if (!pending.changed) continue;
        const TextView key = schema.keys[index].key.view();
        if (!values_of(disk, key, on_disk)) return false;
        if (!same_values(on_disk, pending.original)) {
            if (!note_conflict(state, index)) return false;
            continue;
        }
        if (!serialize_value(schema.keys[index], state.values[index], mine)) return false;
        if (!replace_values(merged, key, mine, next)) return false;
        merged.entries = static_cast<Vector<Entry>&&>(next.entries);
        next.entries = Vector<Entry>();
        applied[index] = true;
        written = written + 1;
    }
    return true;
}

OperationResult<SaveOutcome> save_failure(const OperationStatus status) noexcept {
    OperationResult<SaveOutcome> result = OperationResult<SaveOutcome>::failure(*status.error());
    return result;
}

OperationResult<SaveOutcome> save_locked(StoreState& state) noexcept {
    const SchemaState& schema = *state.schema;
    constexpr int attempts = 3;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        Snapshot now{};
        EntryTable disk{};
        bool disk_valid = false;
        OperationResult<DiskRead> read = read_disk(state, now, disk, disk_valid);
        if (read.failed()) return OperationResult<SaveOutcome>::failure(*read.error());
        const DiskRead found = *read.value();
        if (found == DiskRead::newer) {
            state.read_only = true;
            state.load = LoadState::newer;
            static_cast<void>(add_diagnostic(state, Issue::newer_version, 2, ""));
            return save_failure(refused(ErrorKind::invalid_argument, save_operation, HostCode::invalid_state));
        }
        if (found == DiskRead::damaged) {
            // Another writer's file broke: set it aside and write over it
            // from what this store last knew whole.
            static_cast<void>(add_diagnostic(state, Issue::damaged_file, 0, ""));
            set_aside(state);
            if (!copy_table(state.base, disk)) return save_failure(exhausted(save_operation));
            now.present = false;
            now.bytes = Buffer<Byte>();
        }
        // Nothing changed here: nothing to write, except an older file this
        // store migrated, which is written in the schema's version. Another
        // writer's file is left as it is (this store's view stays frozen).
        const usize changed = pending_count(state);
        if (changed == 0 && found != DiskRead::damaged) {
            const bool upgrade = found == DiskRead::same && now.present && state.base_version != schema.version;
            if (!upgrade) {
                const SaveOutcome nothing{};
                return OperationResult<SaveOutcome>::success(nothing);
            }
        }
        EntryTable merged{};
        Vector<bool> applied;
        usize written = 0;
        if (!merge(state, disk, merged, applied, written)) return save_failure(exhausted(save_operation));
        Vector<char> text;
        if (!serialize(state.identity.view(), schema.part.view(), schema.version, merged, text)) {
            return save_failure(exhausted(save_operation));
        }
        if (text.count() > schema.file_limit) {
            return save_failure(refused(ErrorKind::limit_exceeded, save_operation, HostCode::too_long));
        }
        // The last good copy: the disk's file, when it read back whole.
        if (disk_valid && now.present) {
            const OperationStatus kept = write_file(state.last_good, now.bytes.view(), Durability::durable);
            if (kept.failed()) return save_failure(kept);
        }
        // Immediately before publishing: the file is still the one read.
        Snapshot check{};
        OperationResult<ReadFile> checked = read_snapshot(state.file, schema.file_limit, check);
        if (checked.failed()) return OperationResult<SaveOutcome>::failure(*checked.error());
        if (!same_snapshot(check, now) && found != DiskRead::damaged) continue;
        const View<Byte> bytes(reinterpret_cast<const Byte*>(text.view().data()), text.count());
        const OperationStatus published = write_file(state.file, bytes, Durability::durable);
        if (published.failed()) return save_failure(published);
        // The new base: what was written.
        Snapshot after{};
        OperationResult<ReadFile> reread = read_snapshot(state.file, schema.file_limit, after);
        if (reread.failed()) return OperationResult<SaveOutcome>::failure(*reread.error());
        state.snapshot = static_cast<Snapshot&&>(after);
        state.snapshot_valid = true;
        state.base.entries = static_cast<Vector<Entry>&&>(merged.entries);
        state.base_version = schema.version;
        SaveOutcome outcome{};
        outcome.written = written;
        for (usize index = 0; index < state.pending.count(); ++index) {
            Pending& pending = state.pending[index];
            if (applied[index]) {
                pending.changed = false;
                pending.conflicted = false;
                pending.original.clear();
            }
            if (pending.conflicted) outcome.conflicts = outcome.conflicts + 1;
        }
        return OperationResult<SaveOutcome>::success(outcome);
    }
    return save_failure(refused(ErrorKind::limit_exceeded, save_operation, HostCode::busy));
}

// ---- changes ----------------------------------------------------------------

// Marks key `index` changed, remembering what `base` holds for it.
bool mark_changed(StoreState& state, const usize index) noexcept {
    Pending& pending = state.pending[index];
    if (pending.changed) return true;
    if (!values_of(state.base, (*state.schema).keys[index].key.view(), pending.original)) return false;
    pending.changed = true;
    pending.conflicted = false;
    return true;
}

usize required_key(const StoreState& state, const TextView key, const ValueType type) noexcept {
    const usize index = find_key(*state.schema, key);
    STXLIB_REQUIRE(index < (*state.schema).keys.count(), "stxlib::settings: a key the schema does not declare");
    STXLIB_REQUIRE((*state.schema).keys[index].type == type, "stxlib::settings: a key of another type");
    return index;
}

// Replaces key `index`'s value with `candidate` when it differs.
OperationStatus change(StoreState& state, const usize index, Value& candidate, const Operation operation) noexcept {
    const KeySpec& spec = (*state.schema).keys[index];
    Vector<Utf8String> before;
    Vector<Utf8String> after;
    if (!serialize_value(spec, state.values[index], before) || !serialize_value(spec, candidate, after)) {
        return exhausted(operation);
    }
    if (same_values(before, after)) return OperationStatus::success();
    if (!mark_changed(state, index)) return exhausted(operation);
    state.values[index] = static_cast<Value&&>(candidate);
    return OperationStatus::success();
}

bool copy_value(const Value& from, Value& to) noexcept {
    to.boolean = from.boolean;
    to.integer = from.integer;
    to.real = from.real;
    to.choice = from.choice;
    if (to.text.assign(from.text.view()).failed()) return false;
    to.items.clear();
    for (usize index = 0; index < from.items.count(); ++index) {
        OperationResult<Utf8String> copied = Utf8String::copy_of(from.items[index].view());
        if (copied.failed()) return false;
        if (to.items.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).failed()) return false;
    }
    return true;
}

// ---- finish -----------------------------------------------------------------

constexpr Operation finish_operation = "stxlib.settings.finish";

// This store's whole view as a table (unknown keys from base kept).
bool view_table(const StoreState& state, EntryTable& output) noexcept {
    const SchemaState& schema = *state.schema;
    if (!copy_table(state.base, output)) return false;
    Vector<Utf8String> values;
    EntryTable next{};
    for (usize index = 0; index < schema.keys.count(); ++index) {
        if (!serialize_value(schema.keys[index], state.values[index], values)) return false;
        if (!replace_values(output, schema.keys[index].key.view(), values, next)) return false;
        output.entries = static_cast<Vector<Entry>&&>(next.entries);
        next.entries = Vector<Entry>();
    }
    return true;
}

OperationStatus write_conflict_copy(StoreState& state) noexcept {
    const SchemaState& schema = *state.schema;
    Utf8String prefix;
    if (!name_of(schema.part.view(), ".", prefix)) return exhausted(finish_operation);
    Vector<u64> numbers;
    if (!numbered_files(state.directory, prefix.view(), ".settings", numbers)) return exhausted(finish_operation);
    const u64 next_number = numbers.empty() ? 1 : numbers[numbers.count() - 1] + 1;
    Utf8String name;
    if (!numbered_name(prefix.view(), next_number, ".settings", name)) return exhausted(finish_operation);
    OperationResult<Path> path = child(state.directory, name.view());
    if (path.failed()) return OperationStatus::failure(*path.error());
    EntryTable table{};
    Vector<char> text;
    if (!view_table(state, table) || !serialize(state.identity.view(), schema.part.view(), schema.version, table, text)) {
        return exhausted(finish_operation);
    }
    const View<Byte> bytes(reinterpret_cast<const Byte*>(text.view().data()), text.count());
    const OperationStatus written = write_file(*path.value(), bytes, Durability::durable);
    if (written.failed()) return written;
    prune(state.directory, prefix.view(), ".settings");
    for (usize index = 0; index < state.pending.count(); ++index) {
        state.pending[index].changed = false;
        state.pending[index].conflicted = false;
        state.pending[index].original.clear();
    }
    return OperationStatus::success();
}

OperationResult<Path> copy_path(const StoreState& state, const u32 number) noexcept {
    Utf8String prefix;
    Utf8String name;
    if (!name_of((*state.schema).part.view(), ".", prefix) ||
        !numbered_name(prefix.view(), number, ".settings", name)) {
        return OperationResult<Path>::failure(ErrorRecord::make(ErrorKind::out_of_memory, finish_operation));
    }
    OperationResult<Path> result = child(state.directory, name.view());
    return result;
}

bool listed_copy(const StoreState& state, const u32 number, usize& at) noexcept {
    for (usize index = 0; index < state.copies.count(); ++index) {
        if (state.copies[index] == number) {
            at = index;
            return true;
        }
    }
    return false;
}

} // namespace

// ---- MigrationEntries ---------------------------------------------------------

usize MigrationEntries::count(const TextView key) const noexcept {
    const usize result = count_of(*table_, key);
    return result;
}

TextView MigrationEntries::value(const TextView key, const usize index) const noexcept {
    usize seen = 0;
    for (usize at = 0; at < (*table_).entries.count(); ++at) {
        const Entry& entry = (*table_).entries[at];
        if (!entry.key.equals(key)) continue;
        if (seen == index) return entry.value.view();
        seen = seen + 1;
    }
    STXLIB_REQUIRE(false, "MigrationEntries::value: index past the key's values");
    return TextView();
}

OperationStatus MigrationEntries::rename_key(const TextView from, const TextView to) noexcept {
    constexpr Operation operation = "stxlib.settings.migration.rename_key";
    if (!valid_key(from) || !valid_key(to)) return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    if (from == to) return OperationStatus::success();
    remove_key(to);
    for (usize index = 0; index < (*table_).entries.count(); ++index) {
        Entry& entry = (*table_).entries[index];
        if (entry.key.equals(from) && entry.key.assign(to).failed()) return exhausted(operation);
    }
    return OperationStatus::success();
}

void MigrationEntries::remove_key(const TextView key) noexcept {
    usize index = 0;
    while (index < (*table_).entries.count()) {
        if ((*table_).entries[index].key.equals(key)) {
            (*table_).entries.erase(index);
            continue;
        }
        index = index + 1;
    }
}

OperationStatus MigrationEntries::set(const TextView key, const TextView value) noexcept {
    constexpr Operation operation = "stxlib.settings.migration.set";
    if (!valid_key(key)) return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    Vector<Utf8String> values;
    OperationResult<Utf8String> copied = Utf8String::copy_of(value);
    if (copied.failed()) return exhausted(operation);
    if (values.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).failed()) return exhausted(operation);
    EntryTable next{};
    if (!replace_values(*table_, key, values, next)) return exhausted(operation);
    (*table_).entries = static_cast<Vector<Entry>&&>(next.entries);
    return OperationStatus::success();
}

OperationStatus MigrationEntries::append(const TextView key, const TextView value) noexcept {
    constexpr Operation operation = "stxlib.settings.migration.append";
    if (!valid_key(key)) return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    usize at = (*table_).entries.count();
    for (usize index = 0; index < (*table_).entries.count(); ++index) {
        if ((*table_).entries[index].key.equals(key)) at = index + 1;
    }
    Vector<Entry> single;
    if (!push_entry(single, key, value, 0)) return exhausted(operation);
    if ((*table_).entries.insert(at, static_cast<Entry&&>(single[0])).failed()) return exhausted(operation);
    return OperationStatus::success();
}

// ---- Schema -------------------------------------------------------------------

OperationResult<Schema> Schema::create(const TextView part, const u32 version) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.create";
    if (!valid_part(part)) {
        return OperationResult<Schema>::failure(host_failure::record(ErrorKind::invalid_argument, operation,
                                                                     HostCode::invalid_name));
    }
    if (version == 0) {
        return OperationResult<Schema>::failure(host_failure::record(ErrorKind::invalid_argument, operation,
                                                                     HostCode::invalid_value));
    }
    OperationResult<Unique<SchemaState>> made = Unique<SchemaState>::make();
    if (made.failed()) return OperationResult<Schema>::failure(*made.error());
    Schema result;
    result.state_ = static_cast<OperationResult<Unique<SchemaState>>&&>(made).take_value();
    SchemaState& state = *result.state_;
    if (state.part.assign(part).failed()) {
        return OperationResult<Schema>::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
    }
    state.version = version;
    return OperationResult<Schema>::success(static_cast<Schema&&>(result));
}

u32 Schema::version() const noexcept {
    STXLIB_REQUIRE(state_.present(), "Schema::version on an empty schema");
    return (*state_).version;
}

TextView Schema::part() const& noexcept {
    STXLIB_REQUIRE(state_.present(), "Schema::part on an empty schema");
    return (*state_).part.view();
}

OperationStatus Schema::add_boolean(const TextView key, const bool fallback) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.add_boolean";
    STXLIB_REQUIRE(state_.present(), "Schema::add_boolean on an empty schema");
    const OperationStatus checked = check_new_key(*state_, key, operation);
    if (checked.failed()) return checked;
    KeySpec spec{};
    const OperationStatus made = make_spec(key, ValueType::boolean, spec, operation);
    if (made.failed()) return made;
    spec.boolean_default = fallback;
    const OperationStatus result = add_spec(*state_, static_cast<KeySpec&&>(spec), operation);
    return result;
}

OperationStatus Schema::add_integer(const TextView key, const i64 minimum, const i64 maximum,
                                    const i64 fallback) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.add_integer";
    STXLIB_REQUIRE(state_.present(), "Schema::add_integer on an empty schema");
    const OperationStatus checked = check_new_key(*state_, key, operation);
    if (checked.failed()) return checked;
    if (minimum > maximum || fallback < minimum || fallback > maximum) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    KeySpec spec{};
    const OperationStatus made = make_spec(key, ValueType::integer, spec, operation);
    if (made.failed()) return made;
    spec.integer_minimum = minimum;
    spec.integer_maximum = maximum;
    spec.integer_default = fallback;
    const OperationStatus result = add_spec(*state_, static_cast<KeySpec&&>(spec), operation);
    return result;
}

OperationStatus Schema::add_real(const TextView key, const double minimum, const double maximum,
                                 const double fallback) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.add_real";
    STXLIB_REQUIRE(state_.present(), "Schema::add_real on an empty schema");
    const OperationStatus checked = check_new_key(*state_, key, operation);
    if (checked.failed()) return checked;
    const bool bounded = finite(minimum) && finite(maximum) && finite(fallback);
    if (!bounded || minimum > maximum || fallback < minimum || fallback > maximum) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    KeySpec spec{};
    const OperationStatus made = make_spec(key, ValueType::real, spec, operation);
    if (made.failed()) return made;
    spec.real_minimum = minimum;
    spec.real_maximum = maximum;
    spec.real_default = fallback;
    const OperationStatus result = add_spec(*state_, static_cast<KeySpec&&>(spec), operation);
    return result;
}

OperationStatus Schema::add_text(const TextView key, const usize maximum_bytes, const TextView fallback) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.add_text";
    STXLIB_REQUIRE(state_.present(), "Schema::add_text on an empty schema");
    const OperationStatus checked = check_new_key(*state_, key, operation);
    if (checked.failed()) return checked;
    if (maximum_bytes == 0 || maximum_bytes >= (*state_).file_limit || fallback.count() > maximum_bytes) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    KeySpec spec{};
    const OperationStatus made = make_spec(key, ValueType::text, spec, operation);
    if (made.failed()) return made;
    spec.text_limit = maximum_bytes;
    if (spec.text_default.assign(fallback).failed()) return exhausted(operation);
    const OperationStatus result = add_spec(*state_, static_cast<KeySpec&&>(spec), operation);
    return result;
}

OperationStatus Schema::add_choice(const TextView key, const View<TextView> names, const usize fallback) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.add_choice";
    STXLIB_REQUIRE(state_.present(), "Schema::add_choice on an empty schema");
    const OperationStatus checked = check_new_key(*state_, key, operation);
    if (checked.failed()) return checked;
    if (names.empty() || names.count() > maximum_choices || fallback >= names.count()) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    KeySpec spec{};
    const OperationStatus made = make_spec(key, ValueType::choice, spec, operation);
    if (made.failed()) return made;
    for (usize index = 0; index < names.count(); ++index) {
        if (!valid_key(names[index])) return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
        for (usize earlier = 0; earlier < index; ++earlier) {
            if (names[earlier] == names[index]) {
                return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
            }
        }
        OperationResult<Utf8String> copied = Utf8String::copy_of(names[index]);
        if (copied.failed()) return exhausted(operation);
        if (spec.choices.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).failed()) {
            return exhausted(operation);
        }
    }
    spec.choice_default = fallback;
    const OperationStatus result = add_spec(*state_, static_cast<KeySpec&&>(spec), operation);
    return result;
}

OperationStatus Schema::add_text_list(const TextView key, const usize maximum_bytes_each,
                                      const usize maximum_items) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.add_text_list";
    STXLIB_REQUIRE(state_.present(), "Schema::add_text_list on an empty schema");
    const OperationStatus checked = check_new_key(*state_, key, operation);
    if (checked.failed()) return checked;
    if (maximum_bytes_each == 0 || maximum_bytes_each >= (*state_).file_limit || maximum_items == 0 ||
        maximum_items > (*state_).file_limit) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    KeySpec spec{};
    const OperationStatus made = make_spec(key, ValueType::text_list, spec, operation);
    if (made.failed()) return made;
    spec.text_limit = maximum_bytes_each;
    spec.item_limit = maximum_items;
    const OperationStatus result = add_spec(*state_, static_cast<KeySpec&&>(spec), operation);
    return result;
}

OperationStatus Schema::add_migration(const u32 from_version, const MigrationStep step) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.add_migration";
    STXLIB_REQUIRE(state_.present(), "Schema::add_migration on an empty schema");
    SchemaState& state = *state_;
    if (step == nullptr || from_version == 0 || from_version >= state.version || step_from(state, from_version) != nullptr) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    detail::StepRecord record{};
    record.from = from_version;
    record.step = step;
    if (state.steps.push(static_cast<detail::StepRecord&&>(record)).failed()) return exhausted(operation);
    return OperationStatus::success();
}

OperationStatus Schema::set_file_limit(const usize bytes) noexcept {
    constexpr Operation operation = "stxlib.settings.schema.set_file_limit";
    STXLIB_REQUIRE(state_.present(), "Schema::set_file_limit on an empty schema");
    if (bytes < 4096 || bytes > largest_file_limit) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    for (usize index = 0; index < (*state_).keys.count(); ++index) {
        if ((*state_).keys[index].text_limit >= bytes) {
            return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
        }
    }
    (*state_).file_limit = bytes;
    return OperationStatus::success();
}

// ---- Store: opening -----------------------------------------------------------

namespace {

// Builds a store in `directory` (which exists) under `identity` text.
OperationResult<Store> open_directory(const Path& directory, const TextView identity, const Schema& schema) noexcept {
    const SchemaState* const schema_state = detail::SettingsAccess::schema_of(schema);
    if (schema_state == nullptr) {
        return OperationResult<Store>::failure(host_failure::record(ErrorKind::invalid_argument, open_operation,
                                                                    HostCode::invalid_name));
    }
    OperationResult<Unique<StoreState>> made = Unique<StoreState>::make();
    if (made.failed()) return OperationResult<Store>::failure(*made.error());
    Unique<StoreState> owner = static_cast<OperationResult<Unique<StoreState>>&&>(made).take_value();
    StoreState& state = *owner;
    state.schema = schema_state;
    if (state.identity.assign(identity).failed()) {
        return OperationResult<Store>::failure(ErrorRecord::make(ErrorKind::out_of_memory, open_operation));
    }
    OperationResult<Path> directory_copy = directory.copy();
    if (directory_copy.failed()) return OperationResult<Store>::failure(*directory_copy.error());
    state.directory = static_cast<OperationResult<Path>&&>(directory_copy).take_value();
    Utf8String name;
    if (!name_of((*schema_state).part.view(), ".settings", name)) {
        return OperationResult<Store>::failure(ErrorRecord::make(ErrorKind::out_of_memory, open_operation));
    }
    OperationResult<Path> file = child(directory, name.view());
    if (file.failed()) return OperationResult<Store>::failure(*file.error());
    state.file = static_cast<OperationResult<Path>&&>(file).take_value();
    if (name.append(".last-good").failed()) {
        return OperationResult<Store>::failure(ErrorRecord::make(ErrorKind::out_of_memory, open_operation));
    }
    OperationResult<Path> last_good = child(directory, name.view());
    if (last_good.failed()) return OperationResult<Store>::failure(*last_good.error());
    state.last_good = static_cast<OperationResult<Path>&&>(last_good).take_value();
    OperationResult<Path> lock = lock_path_of(directory);
    if (lock.failed()) return OperationResult<Store>::failure(*lock.error());
    state.lock = static_cast<OperationResult<Path>&&>(lock).take_value();
    for (usize index = 0; index < (*schema_state).keys.count(); ++index) {
        Value value{};
        Pending pending{};
        if (!default_value((*schema_state).keys[index], value) || state.values.push(static_cast<Value&&>(value)).failed() ||
            state.pending.push(static_cast<Pending&&>(pending)).failed()) {
            return OperationResult<Store>::failure(ErrorRecord::make(ErrorKind::out_of_memory, open_operation));
        }
    }
    {
        OperationResult<FileLock> held = FileLock::acquire(state.lock, Duration::milliseconds(lock_timeout_milliseconds));
        if (held.failed()) return OperationResult<Store>::failure(*held.error());
        const OperationStatus loaded = load(state);
        if (loaded.failed()) return OperationResult<Store>::failure(*loaded.error());
    }
    if (!list_copies(state)) {
        return OperationResult<Store>::failure(ErrorRecord::make(ErrorKind::out_of_memory, open_operation));
    }
    for (usize index = 0; index < state.copies.count(); ++index) {
        if (!add_diagnostic(state, Issue::conflict_copy, 0, "")) {
            return OperationResult<Store>::failure(ErrorRecord::make(ErrorKind::out_of_memory, open_operation));
        }
    }
    Store result;
    detail::SettingsAccess::adopt(result, static_cast<Unique<StoreState>&&>(owner));
    return OperationResult<Store>::success(static_cast<Store&&>(result));
}

} // namespace

OperationResult<Store> Store::open(const TextView identity, const Placement placement, const Schema& schema) noexcept {
    if (!valid_identity(identity)) {
        return OperationResult<Store>::failure(host_failure::record(ErrorKind::invalid_argument, open_operation,
                                                                    HostCode::invalid_name));
    }
    const KnownDirectory which =
        placement == Placement::local ? KnownDirectory::user_local_configuration : KnownDirectory::user_configuration;
    OperationResult<Path> base = known_directory(which);
    if (base.failed()) return OperationResult<Store>::failure(*base.error());
    OperationResult<Path> directory = child(*base.value(), identity);
    if (directory.failed()) return OperationResult<Store>::failure(*directory.error());
    const OperationStatus created = create_directories(*directory.value());
    if (created.failed()) return OperationResult<Store>::failure(*created.error());
    OperationResult<Store> result = open_directory(*directory.value(), identity, schema);
    return result;
}

OperationResult<Store> Store::open_in(const Path& directory, const Schema& schema) noexcept {
    const PathText name = directory.file_name();
    TextView identity = "local.settings.store";
    if (name.is_unicode() && valid_identity(name.unicode())) identity = name.unicode();
    OperationResult<Store> result = open_directory(directory, identity, schema);
    return result;
}

// ---- Store: reads -------------------------------------------------------------

bool Store::boolean(const TextView key) const noexcept {
    const StoreState& state = *state_;
    return state.values[required_key(state, key, ValueType::boolean)].boolean;
}

i64 Store::integer(const TextView key) const noexcept {
    const StoreState& state = *state_;
    return state.values[required_key(state, key, ValueType::integer)].integer;
}

double Store::real(const TextView key) const noexcept {
    const StoreState& state = *state_;
    return state.values[required_key(state, key, ValueType::real)].real;
}

TextView Store::text(const TextView key) const& noexcept {
    const StoreState& state = *state_;
    return state.values[required_key(state, key, ValueType::text)].text.view();
}

usize Store::choice(const TextView key) const noexcept {
    const StoreState& state = *state_;
    return state.values[required_key(state, key, ValueType::choice)].choice;
}

usize Store::item_count(const TextView key) const noexcept {
    const StoreState& state = *state_;
    return state.values[required_key(state, key, ValueType::text_list)].items.count();
}

TextView Store::item(const TextView key, const usize index) const& noexcept {
    const StoreState& state = *state_;
    const Value& value = state.values[required_key(state, key, ValueType::text_list)];
    STXLIB_REQUIRE(index < value.items.count(), "Store::item: index past the list");
    return value.items[index].view();
}

// ---- Store: changes -----------------------------------------------------------

OperationStatus Store::set_boolean(const TextView key, const bool value) noexcept {
    constexpr Operation operation = "stxlib.settings.set_boolean";
    StoreState& state = *state_;
    const usize index = required_key(state, key, ValueType::boolean);
    Value candidate{};
    if (!copy_value(state.values[index], candidate)) return exhausted(operation);
    candidate.boolean = value;
    const OperationStatus result = change(state, index, candidate, operation);
    return result;
}

OperationStatus Store::set_integer(const TextView key, const i64 value) noexcept {
    constexpr Operation operation = "stxlib.settings.set_integer";
    StoreState& state = *state_;
    const usize index = required_key(state, key, ValueType::integer);
    const KeySpec& spec = (*state.schema).keys[index];
    if (value < spec.integer_minimum || value > spec.integer_maximum) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    Value candidate{};
    if (!copy_value(state.values[index], candidate)) return exhausted(operation);
    candidate.integer = value;
    const OperationStatus result = change(state, index, candidate, operation);
    return result;
}

OperationStatus Store::set_real(const TextView key, const double value) noexcept {
    constexpr Operation operation = "stxlib.settings.set_real";
    StoreState& state = *state_;
    const usize index = required_key(state, key, ValueType::real);
    const KeySpec& spec = (*state.schema).keys[index];
    if (!finite(value) || value < spec.real_minimum || value > spec.real_maximum) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    Value candidate{};
    if (!copy_value(state.values[index], candidate)) return exhausted(operation);
    candidate.real = value;
    const OperationStatus result = change(state, index, candidate, operation);
    return result;
}

OperationStatus Store::set_text(const TextView key, const TextView value) noexcept {
    constexpr Operation operation = "stxlib.settings.set_text";
    StoreState& state = *state_;
    const usize index = required_key(state, key, ValueType::text);
    if (value.count() > (*state.schema).keys[index].text_limit) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    Value candidate{};
    if (!copy_value(state.values[index], candidate) || candidate.text.assign(value).failed()) {
        return exhausted(operation);
    }
    const OperationStatus result = change(state, index, candidate, operation);
    return result;
}

OperationStatus Store::set_choice(const TextView key, const usize value) noexcept {
    constexpr Operation operation = "stxlib.settings.set_choice";
    StoreState& state = *state_;
    const usize index = required_key(state, key, ValueType::choice);
    if (value >= (*state.schema).keys[index].choices.count()) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    Value candidate{};
    if (!copy_value(state.values[index], candidate)) return exhausted(operation);
    candidate.choice = value;
    const OperationStatus result = change(state, index, candidate, operation);
    return result;
}

OperationStatus Store::set_items(const TextView key, const View<TextView> items) noexcept {
    constexpr Operation operation = "stxlib.settings.set_items";
    StoreState& state = *state_;
    const usize index = required_key(state, key, ValueType::text_list);
    const KeySpec& spec = (*state.schema).keys[index];
    if (items.count() > spec.item_limit) return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    Value candidate{};
    for (usize item = 0; item < items.count(); ++item) {
        if (items[item].count() > spec.text_limit) {
            return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
        }
        OperationResult<Utf8String> copied = Utf8String::copy_of(items[item]);
        if (copied.failed()) return exhausted(operation);
        if (candidate.items.push(static_cast<OperationResult<Utf8String>&&>(copied).take_value()).failed()) {
            return exhausted(operation);
        }
    }
    const OperationStatus result = change(state, index, candidate, operation);
    return result;
}

OperationStatus Store::reset(const TextView key) noexcept {
    constexpr Operation operation = "stxlib.settings.reset";
    StoreState& state = *state_;
    const usize index = find_key(*state.schema, key);
    STXLIB_REQUIRE(index < (*state.schema).keys.count(), "stxlib::settings: a key the schema does not declare");
    Value candidate{};
    if (!default_value((*state.schema).keys[index], candidate)) return exhausted(operation);
    const OperationStatus result = change(state, index, candidate, operation);
    return result;
}

// ---- Store: persistence -------------------------------------------------------

OperationResult<SaveOutcome> Store::save() noexcept {
    StoreState& state = *state_;
    if (state.read_only) {
        return save_failure(refused(ErrorKind::invalid_argument, save_operation, HostCode::invalid_state));
    }
    OperationResult<FileLock> held = FileLock::acquire(state.lock, Duration::milliseconds(lock_timeout_milliseconds));
    if (held.failed()) return OperationResult<SaveOutcome>::failure(*held.error());
    OperationResult<SaveOutcome> result = save_locked(state);
    return result;
}

OperationResult<bool> Store::reload_if_changed() noexcept {
    constexpr Operation operation = "stxlib.settings.reload_if_changed";
    StoreState& state = *state_;
    // A cheap look first: identity and time unchanged means the same file.
    OperationResult<FileIdentity> identity = identify(state.file);
    OperationResult<FileInformation> information = inspect(state.file, LinkMode::follow);
    if (identity.succeeded() && information.succeeded() && state.snapshot.present &&
        *identity.value() == state.snapshot.identity && (*information.value()).modified == state.snapshot.modified &&
        (*information.value()).size == state.snapshot.bytes.count()) {
        return OperationResult<bool>::success(false);
    }
    Snapshot now{};
    OperationResult<ReadFile> read = read_snapshot(state.file, (*state.schema).file_limit, now);
    if (read.failed()) return OperationResult<bool>::failure(*read.error());
    if (same_snapshot(now, state.snapshot)) return OperationResult<bool>::success(false);
    if (*read.value() != ReadFile::read) {
        // Gone or too large: nothing to adopt; the next save writes again.
        return OperationResult<bool>::success(false);
    }
    ParsedFile parsed{};
    const Verdict verdict = parse_file(now.bytes.view(), parsed);
    if (verdict == Verdict::exhausted) {
        return OperationResult<bool>::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
    }
    if (verdict == Verdict::damaged) {
        if (!add_diagnostic(state, Issue::reload_skipped, 0, "")) {
            return OperationResult<bool>::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
        }
        return OperationResult<bool>::success(false);
    }
    const u32 read_version = parsed.version;
    const Migrated migrated = migrate(*state.schema, parsed);
    if (migrated == Migrated::newer) {
        state.read_only = true;
        state.load = LoadState::newer;
        static_cast<void>(add_diagnostic(state, Issue::newer_version, 2, ""));
    } else if (migrated != Migrated::current) {
        return OperationResult<bool>::success(false);
    }
    if (!adopt_unchanged(state, parsed.table) || !copy_table(parsed.table, state.base)) {
        return OperationResult<bool>::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
    }
    state.base_version = read_version;
    state.snapshot = static_cast<Snapshot&&>(now);
    state.snapshot_valid = parsed.checksum_matches;
    return OperationResult<bool>::success(true);
}

OperationStatus Store::finish() noexcept {
    StoreState& state = *state_;
    if (!state.read_only && pending_count(state) != 0) {
        const OperationResult<SaveOutcome> saved = save();
        static_cast<void>(saved);
    }
    if (pending_count(state) == 0) return OperationStatus::success();
    const OperationStatus result = write_conflict_copy(state);
    return result;
}

// ---- Store: conflicts ---------------------------------------------------------

usize Store::conflict_count() const noexcept {
    const StoreState& state = *state_;
    usize result = 0;
    for (usize index = 0; index < state.pending.count(); ++index) {
        if (state.pending[index].conflicted) result = result + 1;
    }
    return result;
}

TextView Store::conflict_key(const usize index) const& noexcept {
    const StoreState& state = *state_;
    usize seen = 0;
    for (usize at = 0; at < state.pending.count(); ++at) {
        if (!state.pending[at].conflicted) continue;
        if (seen == index) return (*state.schema).keys[at].key.view();
        seen = seen + 1;
    }
    STXLIB_REQUIRE(false, "Store::conflict_key: index past the conflicts");
    return TextView();
}

OperationStatus Store::resolve(const TextView key, const Resolution resolution) noexcept {
    constexpr Operation operation = "stxlib.settings.resolve";
    StoreState& state = *state_;
    const usize index = find_key(*state.schema, key);
    if (index >= (*state.schema).keys.count() || !state.pending[index].conflicted) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_state);
    }
    Pending& pending = state.pending[index];
    if (resolution == Resolution::keep_theirs) {
        ReadIssue problem{};
        if (!read_value((*state.schema).keys[index], state.base, state.values[index], problem)) {
            return exhausted(operation);
        }
        pending.changed = false;
        pending.conflicted = false;
        pending.original.clear();
        return OperationStatus::success();
    }
    // use_mine: what the file now holds becomes the value compared against,
    // so the next save writes this store's value.
    if (!values_of(state.base, key, pending.original)) return exhausted(operation);
    pending.conflicted = false;
    return OperationStatus::success();
}

usize Store::conflict_copy_count() const noexcept { return (*state_).copies.count(); }

u32 Store::conflict_copy_number(const usize index) const noexcept {
    const StoreState& state = *state_;
    STXLIB_REQUIRE(index < state.copies.count(), "Store::conflict_copy_number: index past the copies");
    return state.copies[index];
}

OperationStatus Store::adopt_conflict_copy(const u32 number) noexcept {
    constexpr Operation operation = "stxlib.settings.adopt_conflict_copy";
    StoreState& state = *state_;
    usize at = 0;
    if (!listed_copy(state, number, at)) return refused(ErrorKind::not_found, operation, HostCode::none);
    OperationResult<Path> path = copy_path(state, number);
    if (path.failed()) return OperationStatus::failure(*path.error());
    OperationResult<Buffer<Byte>> bytes = read_file(*path.value(), (*state.schema).file_limit);
    if (bytes.failed()) return OperationStatus::failure(*bytes.error());
    ParsedFile parsed{};
    const Verdict verdict = parse_file((*bytes.value()).view(), parsed);
    if (verdict == Verdict::exhausted) return exhausted(operation);
    if (verdict == Verdict::damaged || migrate(*state.schema, parsed) != Migrated::current) {
        return refused(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    const SchemaState& schema = *state.schema;
    for (usize index = 0; index < schema.keys.count(); ++index) {
        if (count_of(parsed.table, schema.keys[index].key.view()) == 0) continue;
        Value candidate{};
        ReadIssue problem{};
        if (!read_value(schema.keys[index], parsed.table, candidate, problem)) return exhausted(operation);
        if (problem.present) continue;
        const OperationStatus changed = change(state, index, candidate, operation);
        if (changed.failed()) return changed;
    }
    static_cast<void>(remove(*path.value()));
    state.copies.erase(at);
    return OperationStatus::success();
}

OperationStatus Store::discard_conflict_copy(const u32 number) noexcept {
    constexpr Operation operation = "stxlib.settings.discard_conflict_copy";
    StoreState& state = *state_;
    usize at = 0;
    if (!listed_copy(state, number, at)) return refused(ErrorKind::not_found, operation, HostCode::none);
    OperationResult<Path> path = copy_path(state, number);
    if (path.failed()) return OperationStatus::failure(*path.error());
    const OperationStatus removed = remove(*path.value());
    if (removed.failed() && (*removed.error()).kind() != ErrorKind::not_found) return removed;
    state.copies.erase(at);
    return OperationStatus::success();
}

LoadState Store::load_state() const noexcept { return (*state_).load; }

bool Store::read_only() const noexcept { return (*state_).read_only; }

bool Store::changed() const noexcept { return pending_count(*state_) != 0; }

View<Diagnostic> Store::diagnostics() const& noexcept { return (*state_).diagnostics.view(); }

void Store::clear_diagnostics() noexcept { (*state_).diagnostics.clear(); }

const Path& Store::file() const& noexcept { return (*state_).file; }

} // namespace stxlib::settings
