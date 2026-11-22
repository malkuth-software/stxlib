#pragma once

// The JSON Schema subset of stx_mcp (include/stxlib/mcp.hpp, "The supported
// JSON Schema subset"): compiling a declared schema, and validating a value
// against it. Private to the stxlib::mcp target.
//
// Compiling parses the schema text and walks every subschema once, with an
// explicit stack bounded by schema_depth_limit (no recursion), checking
// each keyword's shape and refusing validation keywords outside the subset
// by name. Validation walks the value with the same discipline: every
// (subschema, value) pair is visited at most once, since the subset has no
// composition, so the cost is bounded by the value's size. The first
// failure found is reported with its JSON Pointer (RFC 6901) path.

#include "stxlib/json.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/text_view.hpp"

namespace stxlib::mcp::detail {

class Schema final {
public:
    Schema() noexcept = default;
    Schema(Schema&&) noexcept = default;
    Schema& operator=(Schema&&) noexcept = default;
    Schema(const Schema&) = delete;
    Schema& operator=(const Schema&) = delete;
    ~Schema() = default;

    // Parses and checks `text`. Failures: invalid_argument when it is not
    // JSON or not a schema of the subset's shape (the message says where);
    // unsupported for a keyword outside the subset or another $schema
    // dialect (the message names it); out_of_memory.
    [[nodiscard]] static OperationResult<Schema> compile(TextView text) noexcept;

    // Whether the schema's root says "type": "object" (MCP requires it of
    // an input schema).
    [[nodiscard]] bool describes_object() const noexcept;

    // Checks `value` of `instance`. Success, or invalid_argument with
    // `problem` replaced by "<path>: <what>" (path "" for the root), or
    // out_of_memory (problem then unspecified).
    [[nodiscard]] OperationStatus validate(const JsonDocument& instance, JsonRef value, Utf8String& problem) const noexcept;

    // The schema as parsed, for listing it (JsonWriter::copy).
    [[nodiscard]] const JsonDocument& document() const& noexcept { return document_; }
    const JsonDocument& document() const&& = delete;

private:
    JsonDocument document_{};
};

} // namespace stxlib::mcp::detail
