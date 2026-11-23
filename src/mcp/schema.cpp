// The JSON Schema subset (src/mcp/schema.hpp).

#include "mcp/schema.hpp"

#include "stxlib/mcp.hpp"
#include "stxlib/numeric_text.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::mcp::detail {

namespace {

constexpr Operation compile_operation = "stxlib.mcp.schema.compile";
constexpr Operation validate_operation = "stxlib.mcp.schema.validate";
constexpr u32 no_parent = u32_max;

// Validation keywords of JSON Schema 2020-12 (and draft-07's that 2020-12
// renamed) outside the subset: refused at compile so a schema is never
// taken as more permissive than it says.
constexpr TextView unsupported_keywords[] = {
    "$ref",          "$dynamicRef",       "$recursiveRef",     "allOf",           "anyOf",
    "oneOf",         "not",               "if",                "then",            "else",
    "pattern",       "patternProperties", "prefixItems",       "additionalItems", "contains",
    "minContains",   "maxContains",       "uniqueItems",       "dependentRequired", "dependentSchemas",
    "dependencies",  "propertyNames",     "unevaluatedItems",  "unevaluatedProperties", "multipleOf",
};

constexpr TextView accepted_dialects[] = {
    "https://json-schema.org/draft/2020-12/schema",
    "https://json-schema.org/draft/2020-12/schema#",
    "http://json-schema.org/draft-07/schema",
    "http://json-schema.org/draft-07/schema#",
};

constexpr TextView type_names[] = {"null", "boolean", "object", "array", "number", "string", "integer"};

bool listed(const TextView name, const View<TextView> names) noexcept {
    for (usize index = 0; index < names.count(); ++index) {
        if (names[index] == name) return true;
    }
    return false;
}

ErrorRecord shape_failure(const TextView message) noexcept {
    const ErrorRecord result =
        ErrorRecord::make(ErrorKind::invalid_argument, compile_operation, ErrorDetail{.message = message});
    return result;
}

bool is_scalar(const JsonDocument& document, const JsonRef value) noexcept {
    const JsonKind kind = document.kind(value);
    const bool result = kind != JsonKind::array && kind != JsonKind::object;
    return result;
}

bool is_count(const JsonDocument& document, const JsonRef value) noexcept {
    const Optional<i64> count = document.integer(value);
    const bool result = count.present() && *count.value() >= 0;
    return result;
}

// Checks one schema object's keywords; pushes its subschemas onto `pending`
// at depth + 1.
struct PendingSchema final {
    JsonRef schema;
    u32 depth;
};

OperationStatus check_schema_object(const JsonDocument& document, const PendingSchema item,
                                    Vector<PendingSchema>& pending, Utf8String& message) noexcept {
    const JsonRef schema = item.schema;
    if (document.is(schema, JsonKind::boolean)) return OperationStatus::success();
    if (!document.is(schema, JsonKind::object)) {
        return OperationStatus::failure(shape_failure("a schema must be an object or a boolean"));
    }
    if (item.depth >= schema_depth_limit) {
        const ErrorRecord deep = ErrorRecord::make(ErrorKind::limit_exceeded, compile_operation,
                                                   ErrorDetail{.message = "the schema nests too deeply"});
        return OperationStatus::failure(deep);
    }
    Optional<JsonRef> child = document.first_child(schema);
    while (child.present()) {
        const JsonRef value = *child.value();
        const TextView keyword = document.key(value);
        child = document.next_sibling(value);
        if (listed(keyword, View<TextView>(unsupported_keywords))) {
            static_cast<void>(message.assign("the schema keyword is not supported: "));
            static_cast<void>(message.append(keyword));
            const ErrorRecord refused = ErrorRecord::make(ErrorKind::unsupported, compile_operation,
                                                          ErrorDetail{.message = message.view()});
            return OperationStatus::failure(refused);
        }
        if (keyword == TextView("$schema")) {
            const Optional<TextView> dialect = document.text(value);
            if (dialect.absent() || !listed(*dialect.value(), View<TextView>(accepted_dialects))) {
                const ErrorRecord refused =
                    ErrorRecord::make(ErrorKind::unsupported, compile_operation,
                                      ErrorDetail{.message = "only JSON Schema 2020-12 and draft-07 are supported"});
                return OperationStatus::failure(refused);
            }
        } else if (keyword == TextView("type")) {
            if (document.is(value, JsonKind::string)) {
                if (!listed(document.text(value).value_or(TextView()), View<TextView>(type_names))) {
                    return OperationStatus::failure(shape_failure("an unknown type name in a schema"));
                }
            } else if (document.is(value, JsonKind::array) && document.child_count(value) > 0) {
                Optional<JsonRef> name = document.first_child(value);
                while (name.present()) {
                    const JsonRef entry = *name.value();
                    if (!listed(document.text(entry).value_or(TextView()), View<TextView>(type_names))) {
                        return OperationStatus::failure(shape_failure("an unknown type name in a schema"));
                    }
                    name = document.next_sibling(entry);
                }
            } else {
                return OperationStatus::failure(shape_failure("'type' must be a type name or a list of them"));
            }
        } else if (keyword == TextView("properties")) {
            if (!document.is(value, JsonKind::object)) {
                return OperationStatus::failure(shape_failure("'properties' must be an object of schemas"));
            }
            Optional<JsonRef> property = document.first_child(value);
            while (property.present()) {
                const OperationStatus pushed = pending.push(PendingSchema{*property.value(), item.depth + 1});
                if (pushed.failed()) return pushed;
                property = document.next_sibling(*property.value());
            }
        } else if (keyword == TextView("required")) {
            if (!document.is(value, JsonKind::array)) {
                return OperationStatus::failure(shape_failure("'required' must be a list of names"));
            }
            Optional<JsonRef> name = document.first_child(value);
            while (name.present()) {
                if (!document.is(*name.value(), JsonKind::string)) {
                    return OperationStatus::failure(shape_failure("'required' must be a list of names"));
                }
                name = document.next_sibling(*name.value());
            }
        } else if (keyword == TextView("additionalProperties") || keyword == TextView("items")) {
            if (document.is(value, JsonKind::array)) {
                return OperationStatus::failure(shape_failure("'items' as a list (tuple validation) is not supported"));
            }
            const OperationStatus pushed = pending.push(PendingSchema{value, item.depth + 1});
            if (pushed.failed()) return pushed;
        } else if (keyword == TextView("enum")) {
            if (!document.is(value, JsonKind::array) || document.child_count(value) == 0) {
                return OperationStatus::failure(shape_failure("'enum' must be a non-empty list"));
            }
            Optional<JsonRef> entry = document.first_child(value);
            while (entry.present()) {
                if (!is_scalar(document, *entry.value())) {
                    const ErrorRecord refused =
                        ErrorRecord::make(ErrorKind::unsupported, compile_operation,
                                          ErrorDetail{.message = "'enum' of arrays or objects is not supported"});
                    return OperationStatus::failure(refused);
                }
                entry = document.next_sibling(*entry.value());
            }
        } else if (keyword == TextView("const")) {
            if (!is_scalar(document, value)) {
                const ErrorRecord refused =
                    ErrorRecord::make(ErrorKind::unsupported, compile_operation,
                                      ErrorDetail{.message = "'const' of an array or object is not supported"});
                return OperationStatus::failure(refused);
            }
        } else if (keyword == TextView("minimum") || keyword == TextView("maximum") ||
                   keyword == TextView("exclusiveMinimum") || keyword == TextView("exclusiveMaximum")) {
            if (!document.is(value, JsonKind::number)) {
                return OperationStatus::failure(shape_failure("a schema bound must be a number"));
            }
        } else if (keyword == TextView("minLength") || keyword == TextView("maxLength") ||
                   keyword == TextView("minItems") || keyword == TextView("maxItems") ||
                   keyword == TextView("minProperties") || keyword == TextView("maxProperties")) {
            if (!is_count(document, value)) {
                return OperationStatus::failure(shape_failure("a schema count must be a non-negative integer"));
            }
        }
        // Any other keyword is an annotation (title, description, default,
        // examples, format, x-mcp-header, ...): accepted, not enforced.
    }
    return OperationStatus::success();
}

// ---- validation ---------------------------------------------------------------

struct Visit final {
    JsonRef schema;
    JsonRef value;
    u32 parent;
    // The step from the parent: an object member (its key is the value's
    // key) or an array position.
    bool member;
    usize position;
    u32 depth;
};

bool type_matches(const JsonDocument& instance, const JsonRef value, const TextView name) noexcept {
    const JsonKind kind = instance.kind(value);
    if (name == TextView("integer")) return instance.integer(value).present();
    if (name == TextView("number")) return kind == JsonKind::number;
    if (name == TextView("string")) return kind == JsonKind::string;
    if (name == TextView("boolean")) return kind == JsonKind::boolean;
    if (name == TextView("object")) return kind == JsonKind::object;
    if (name == TextView("array")) return kind == JsonKind::array;
    if (name == TextView("null")) return kind == JsonKind::null_value;
    return false;
}

// Equality of two scalars (enum, const): kinds, then values. Numbers
// compare by value (1 equals 1.0, as JSON Schema says).
bool scalars_equal(const JsonDocument& left_document, const JsonRef left, const JsonDocument& right_document,
                   const JsonRef right) noexcept {
    const JsonKind kind = left_document.kind(left);
    if (kind != right_document.kind(right)) return false;
    if (kind == JsonKind::null_value) return true;
    if (kind == JsonKind::boolean) {
        return left_document.boolean(left).value_or(false) == right_document.boolean(right).value_or(false);
    }
    if (kind == JsonKind::number) {
        const Optional<i64> left_integer = left_document.integer(left);
        const Optional<i64> right_integer = right_document.integer(right);
        if (left_integer.present() && right_integer.present()) return *left_integer.value() == *right_integer.value();
        return left_document.number(left).value_or(0.0) == right_document.number(right).value_or(1.0);
    }
    if (kind == JsonKind::string) {
        return left_document.text(left).value_or(TextView()) == right_document.text(right).value_or(TextView());
    }
    return false;
}

usize scalar_count(const TextView text) noexcept {
    usize count = 0;
    for (usize index = 0; index < text.count(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text.data()[index]);
        if ((byte & 0xC0) != 0x80) count = count + 1;
    }
    return count;
}

// A slice of UTF-8 text between two ASCII positions (or the ends) is UTF-8.
TextView detail_slice(const TextView text, const usize first, const usize last) noexcept {
    const TextView result = ::stxlib::detail::TextViewAccess::adopt_validated(text.data() + first, last - first);
    return result;
}

// Appends one JSON Pointer reference token, escaping '~' and '/'.
void append_token(Utf8String& path, const TextView token) noexcept {
    static_cast<void>(path.append("/"));
    usize run = 0;
    for (usize index = 0; index < token.count(); ++index) {
        const char byte = token.data()[index];
        if (byte != '~' && byte != '/') continue;
        if (index > run) static_cast<void>(path.append(detail_slice(token, run, index)));
        static_cast<void>(path.append(byte == '~' ? TextView("~0") : TextView("~1")));
        run = index + 1;
    }
    if (token.count() > run) static_cast<void>(path.append(detail_slice(token, run, token.count())));
}

// Writes "<path>: <what>" into `problem`, from the visit at `failed`.
OperationStatus report(const JsonDocument& instance, const Vector<Visit>& trail, const u32 failed, const TextView what,
                       const TextView detail_text, Utf8String& problem) noexcept {
    // The steps from the root, collected leaf first (at most
    // schema_depth_limit + 1 of them: every visit is one schema level
    // below its parent).
    u32 steps[schema_depth_limit + 2]{};
    u32 count = 0;
    u32 at = failed;
    while (at != no_parent && count < schema_depth_limit + 2) {
        steps[count] = at;
        count = count + 1;
        at = trail[at].parent;
    }
    problem.clear();
    for (u32 step = count; step > 0; --step) {
        const Visit& visit = trail[steps[step - 1]];
        if (visit.parent == no_parent) continue;
        if (visit.member) {
            append_token(problem, instance.key(visit.value));
        } else {
            char digits[32]{};
            const Formatted formatted = format_unsigned(MutableView<char>(digits), static_cast<u64>(visit.position));
            if (formatted.succeeded()) append_token(problem, *formatted.text());
        }
    }
    if (problem.empty()) static_cast<void>(problem.append("(the arguments)"));
    static_cast<void>(problem.append(": "));
    static_cast<void>(problem.append(what));
    static_cast<void>(problem.append(detail_text));
    const ErrorRecord invalid = ErrorRecord::make(ErrorKind::invalid_argument, validate_operation,
                                                  ErrorDetail{.message = problem.view()});
    return OperationStatus::failure(invalid);
}

} // namespace

OperationResult<Schema> Schema::compile(const TextView text) noexcept {
    // A schema of schema_depth_limit levels nests two JSON containers per
    // level (a schema object and its properties object), so the reader is
    // given room for that; the schema walk refuses deeper schemas itself.
    JsonLimits limits;
    limits.max_depth = 2 * schema_depth_limit + 8;
    OperationResult<JsonDocument> parsed = JsonDocument::parse(text, limits);
    if (parsed.failed()) {
        const ErrorRecord& cause = *parsed.error();
        if (cause.kind() == ErrorKind::out_of_memory) return OperationResult<Schema>::failure(cause);
        Utf8String message;
        static_cast<void>(message.assign("the schema is not JSON: "));
        static_cast<void>(message.append(cause.message()));
        return OperationResult<Schema>::failure(ErrorRecord::make(ErrorKind::invalid_argument, compile_operation,
                                                                  ErrorDetail{.code = cause.code(),
                                                                              .message = message.view()}));
    }
    Schema schema;
    schema.document_ = static_cast<OperationResult<JsonDocument>&&>(parsed).take_value();
    Vector<PendingSchema> pending;
    Utf8String message;
    const OperationStatus first = pending.push(PendingSchema{schema.document_.root(), 0});
    if (first.failed()) return OperationResult<Schema>::failure(*first.error());
    while (!pending.empty()) {
        const PendingSchema item = pending.take_last();
        const OperationStatus checked = check_schema_object(schema.document_, item, pending, message);
        if (checked.failed()) return OperationResult<Schema>::failure(*checked.error());
    }
    return OperationResult<Schema>::success(static_cast<Schema&&>(schema));
}

bool Schema::describes_object() const noexcept {
    const JsonRef root = document_.root();
    const Optional<TextView> type = document_.member_text(root, "type");
    const bool result = type.present() && *type.value() == TextView("object");
    return result;
}

OperationStatus Schema::validate(const JsonDocument& instance, const JsonRef value, Utf8String& problem) const noexcept {
    const JsonDocument& schema = document_;
    Vector<Visit> trail;
    Vector<u32> stack;
    const OperationStatus seeded = trail.push(Visit{schema.root(), value, no_parent, false, 0, 0});
    if (seeded.failed()) return seeded;
    const OperationStatus stacked = stack.push(0);
    if (stacked.failed()) return stacked;
    while (!stack.empty()) {
        const u32 index = stack.take_last();
        const Visit visit = trail[index];
        const JsonRef rule = visit.schema;
        const JsonRef item = visit.value;
        if (schema.is(rule, JsonKind::boolean)) {
            if (!schema.boolean(rule).value_or(true)) {
                return report(instance, trail, index, "no value is allowed here", "", problem);
            }
            continue;
        }
        // type
        const Optional<JsonRef> type = schema.member(rule, "type");
        if (type.present()) {
            const JsonRef declared = *type.value();
            bool matched = false;
            if (schema.is(declared, JsonKind::string)) {
                matched = type_matches(instance, item, schema.text(declared).value_or(TextView()));
            } else {
                Optional<JsonRef> name = schema.first_child(declared);
                while (name.present() && !matched) {
                    matched = type_matches(instance, item, schema.text(*name.value()).value_or(TextView()));
                    name = schema.next_sibling(*name.value());
                }
            }
            if (!matched) {
                const TextView expected = schema.is(declared, JsonKind::string)
                                              ? schema.text(declared).value_or(TextView())
                                              : TextView("one of the declared types");
                Utf8String what;
                static_cast<void>(what.assign("expected "));
                static_cast<void>(what.append(expected));
                static_cast<void>(what.append(", got "));
                static_cast<void>(what.append(json_kind_name(instance.kind(item))));
                return report(instance, trail, index, what.view(), "", problem);
            }
        }
        // enum and const
        const Optional<JsonRef> choices = schema.member(rule, "enum");
        if (choices.present()) {
            bool found = false;
            Optional<JsonRef> choice = schema.first_child(*choices.value());
            while (choice.present() && !found) {
                found = scalars_equal(schema, *choice.value(), instance, item);
                choice = schema.next_sibling(*choice.value());
            }
            if (!found) return report(instance, trail, index, "not one of the allowed values", "", problem);
        }
        const Optional<JsonRef> constant = schema.member(rule, "const");
        if (constant.present() && !scalars_equal(schema, *constant.value(), instance, item)) {
            return report(instance, trail, index, "not the required value", "", problem);
        }
        const JsonKind kind = instance.kind(item);
        if (kind == JsonKind::number) {
            const double number = instance.number(item).value_or(0.0);
            const Optional<double> minimum = schema.number(schema.member(rule, "minimum").value_or(rule));
            const Optional<double> maximum = schema.number(schema.member(rule, "maximum").value_or(rule));
            const Optional<double> above = schema.number(schema.member(rule, "exclusiveMinimum").value_or(rule));
            const Optional<double> below = schema.number(schema.member(rule, "exclusiveMaximum").value_or(rule));
            if (minimum.present() && number < *minimum.value()) {
                return report(instance, trail, index, "below the minimum", "", problem);
            }
            if (maximum.present() && number > *maximum.value()) {
                return report(instance, trail, index, "above the maximum", "", problem);
            }
            if (above.present() && !(number > *above.value())) {
                return report(instance, trail, index, "not above the exclusive minimum", "", problem);
            }
            if (below.present() && !(number < *below.value())) {
                return report(instance, trail, index, "not below the exclusive maximum", "", problem);
            }
        } else if (kind == JsonKind::string) {
            const usize length = scalar_count(instance.text(item).value_or(TextView()));
            const Optional<i64> shortest = schema.member_integer(rule, "minLength");
            const Optional<i64> longest = schema.member_integer(rule, "maxLength");
            if (shortest.present() && length < static_cast<usize>(*shortest.value())) {
                return report(instance, trail, index, "shorter than its minimum length", "", problem);
            }
            if (longest.present() && length > static_cast<usize>(*longest.value())) {
                return report(instance, trail, index, "longer than its maximum length", "", problem);
            }
        } else if (kind == JsonKind::array) {
            const usize count = instance.child_count(item);
            const Optional<i64> fewest = schema.member_integer(rule, "minItems");
            const Optional<i64> most = schema.member_integer(rule, "maxItems");
            if (fewest.present() && count < static_cast<usize>(*fewest.value())) {
                return report(instance, trail, index, "fewer items than its minimum", "", problem);
            }
            if (most.present() && count > static_cast<usize>(*most.value())) {
                return report(instance, trail, index, "more items than its maximum", "", problem);
            }
            const Optional<JsonRef> items = schema.member(rule, "items");
            if (items.present()) {
                Optional<JsonRef> element = instance.first_child(item);
                usize position = 0;
                while (element.present()) {
                    const u32 next = static_cast<u32>(trail.count());
                    const OperationStatus pushed =
                        trail.push(Visit{*items.value(), *element.value(), index, false, position, visit.depth + 1});
                    if (pushed.failed()) return pushed;
                    const OperationStatus queued = stack.push_copy(next);
                    if (queued.failed()) return queued;
                    element = instance.next_sibling(*element.value());
                    position = position + 1;
                }
            }
        } else if (kind == JsonKind::object) {
            const usize count = instance.child_count(item);
            const Optional<i64> fewest = schema.member_integer(rule, "minProperties");
            const Optional<i64> most = schema.member_integer(rule, "maxProperties");
            if (fewest.present() && count < static_cast<usize>(*fewest.value())) {
                return report(instance, trail, index, "fewer members than its minimum", "", problem);
            }
            if (most.present() && count > static_cast<usize>(*most.value())) {
                return report(instance, trail, index, "more members than its maximum", "", problem);
            }
            const Optional<JsonRef> required = schema.member(rule, "required");
            if (required.present()) {
                Optional<JsonRef> name = schema.first_child(*required.value());
                while (name.present()) {
                    const TextView wanted = schema.text(*name.value()).value_or(TextView());
                    if (instance.member(item, wanted).absent()) {
                        return report(instance, trail, index, "missing the required member ", wanted, problem);
                    }
                    name = schema.next_sibling(*name.value());
                }
            }
            const Optional<JsonRef> properties = schema.member(rule, "properties");
            const Optional<JsonRef> additional = schema.member(rule, "additionalProperties");
            Optional<JsonRef> member = instance.first_child(item);
            while (member.present()) {
                const JsonRef child = *member.value();
                const TextView name = instance.key(child);
                Optional<JsonRef> applies = Optional<JsonRef>::none();
                if (properties.present()) applies = schema.member(*properties.value(), name);
                if (applies.absent() && additional.present()) applies = additional;
                if (applies.present()) {
                    const u32 next = static_cast<u32>(trail.count());
                    const OperationStatus pushed =
                        trail.push(Visit{*applies.value(), child, index, true, 0, visit.depth + 1});
                    if (pushed.failed()) return pushed;
                    const OperationStatus queued = stack.push_copy(next);
                    if (queued.failed()) return queued;
                }
                member = instance.next_sibling(child);
            }
        }
    }
    return OperationStatus::success();
}

} // namespace stxlib::mcp::detail
