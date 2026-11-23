// Tests of JSON reading and writing (include/stxlib/json.hpp): the value
// kinds, numbers and their integer rule, string escapes and surrogates,
// refusals (malformed text, repeated names, unpaired surrogates, limits),
// queries, and the writer (escaping, base64, copying, the byte limit, the
// structural checks, reset).

#include "stxlib/json.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <string>

namespace {

using stxlib::Byte;
using stxlib::ErrorKind;
using stxlib::i64;
using stxlib::JsonDocument;
using stxlib::JsonKind;
using stxlib::JsonLimits;
using stxlib::JsonRef;
using stxlib::JsonWriter;
using stxlib::OperationResult;
using stxlib::Optional;
using stxlib::TextView;
using stxlib::Utf8String;
using stxlib::usize;
using stxlib::View;

TextView view_of(const std::string& text) {
    OperationResult<TextView> checked = stxlib::validate_utf8(text.data(), text.size());
    CHECK(checked.succeeded());
    if (checked.failed()) return TextView();
    return *checked.value();
}

std::string text_of(const TextView text) { return std::string(text.data(), text.count()); }

bool parses(const std::string& text) {
    const OperationResult<JsonDocument> parsed = JsonDocument::parse(view_of(text));
    return parsed.succeeded();
}

JsonDocument parse_or_empty(const std::string& text) {
    OperationResult<JsonDocument> parsed = JsonDocument::parse(view_of(text));
    CHECK(parsed.succeeded());
    if (parsed.failed()) return JsonDocument();
    return static_cast<OperationResult<JsonDocument>&&>(parsed).take_value();
}

ErrorKind failure_kind(const std::string& text, const JsonLimits& limits) {
    const OperationResult<JsonDocument> parsed = JsonDocument::parse(view_of(text), limits);
    if (parsed.succeeded()) return static_cast<ErrorKind>(0);
    return (*parsed.error()).kind();
}

void kinds_and_queries() {
    const JsonDocument document = parse_or_empty(
        R"( {"name":"tool","count":3,"ratio":0.5,"on":true,"off":false,"none":null,"list":[1,"two",[3]],"inner":{"a":1}} )");
    const JsonRef root = document.root();
    CHECK(document.is(root, JsonKind::object));
    CHECK(document.child_count(root) == 8);
    CHECK(text_of(document.member_text(root, "name").value_or(TextView())) == "tool");
    CHECK(document.member_integer(root, "count").value_or(0) == 3);
    const Optional<JsonRef> ratio = document.member(root, "ratio");
    CHECK(ratio.present() && document.number(*ratio.value()).value_or(0.0) == 0.5);
    CHECK(ratio.present() && document.integer(*ratio.value()).absent());
    CHECK(document.member_boolean(root, "on").value_or(false));
    CHECK(!document.member_boolean(root, "off").value_or(true));
    const Optional<JsonRef> none = document.member(root, "none");
    CHECK(none.present() && document.is(*none.value(), JsonKind::null_value));
    CHECK(document.member(root, "missing").absent());
    const Optional<JsonRef> list = document.member(root, "list");
    CHECK(list.present() && document.child_count(*list.value()) == 3);
    if (list.present()) {
        const Optional<JsonRef> second = document.element(*list.value(), 1);
        CHECK(second.present() && text_of(document.text(*second.value()).value_or(TextView())) == "two");
        const Optional<JsonRef> third = document.element(*list.value(), 2);
        CHECK(third.present() && document.is(*third.value(), JsonKind::array));
        CHECK(document.element(*list.value(), 3).absent());
    }
    // Members in source order, with their keys.
    Optional<JsonRef> child = document.first_child(root);
    std::string keys;
    while (child.present()) {
        keys += text_of(document.key(*child.value()));
        keys += ",";
        child = document.next_sibling(*child.value());
    }
    CHECK(keys == "name,count,ratio,on,off,none,list,inner,");
}

void numbers() {
    const JsonDocument document =
        parse_or_empty(R"([0,-0,9223372036854775807,-9223372036854775808,9223372036854775808,1.0,1e2,9007199254740994.0,-1.5e-3])");
    const JsonRef root = document.root();
    i64 integers[9]{};
    bool integral[9]{};
    for (usize index = 0; index < 9; ++index) {
        const Optional<i64> value = document.integer(document.element(root, index).value_or(root));
        integral[index] = value.present();
        integers[index] = value.value_or(0);
    }
    CHECK(integral[0] && integers[0] == 0);
    CHECK(integral[1] && integers[1] == 0);
    CHECK(integral[2] && integers[2] == 9223372036854775807LL);
    CHECK(integral[3] && integers[3] == (-9223372036854775807LL - 1));
    CHECK(!integral[4]);                     // past i64, and past 2^53
    CHECK(integral[5] && integers[5] == 1);  // a whole number counts (JSON Schema)
    CHECK(integral[6] && integers[6] == 100);
    CHECK(!integral[7]);                     // past 2^53: not exact
    CHECK(!integral[8]);
    const Optional<TextView> kept = document.number_text(document.element(root, 8).value_or(root));
    CHECK(text_of(kept.value_or(TextView())) == "-1.5e-3");
    CHECK(!parses("01"));
    CHECK(!parses("1."));
    CHECK(!parses(".5"));
    CHECK(!parses("+1"));
    CHECK(!parses("1e"));
    CHECK(!parses("NaN"));
    CHECK(!parses("-"));
}

void strings() {
    const JsonDocument document = parse_or_empty(R"(["a\"b\\c\/d","\b\f\n\r\t","é€","😀","é"])");
    const JsonRef root = document.root();
    CHECK(text_of(document.text(document.element(root, 0).value_or(root)).value_or(TextView())) == "a\"b\\c/d");
    CHECK(text_of(document.text(document.element(root, 1).value_or(root)).value_or(TextView())) == "\b\f\n\r\t");
    CHECK(text_of(document.text(document.element(root, 2).value_or(root)).value_or(TextView())) == "\xC3\xA9\xE2\x82\xAC");
    CHECK(text_of(document.text(document.element(root, 3).value_or(root)).value_or(TextView())) == "\xF0\x9F\x98\x80");
    CHECK(text_of(document.text(document.element(root, 4).value_or(root)).value_or(TextView())) == "\xC3\xA9");
    CHECK(!parses(R"("\ud83d")"));        // a lone high surrogate
    CHECK(!parses(R"("\ude00")"));        // a lone low surrogate
    CHECK(!parses(R"("\ud83dA")"));  // a high surrogate without its low
    CHECK(!parses("\"tab\there\""));      // a raw control character
    CHECK(!parses(R"("\x")"));
    CHECK(!parses(R"("\u12")"));
    CHECK(!parses(R"("open)"));
}

void structure() {
    CHECK(parses("true"));
    CHECK(parses("  null  "));
    CHECK(parses("[]"));
    CHECK(parses("{}"));
    CHECK(!parses(""));
    CHECK(!parses("[1,]"));
    CHECK(!parses("{\"a\":1,}"));
    CHECK(!parses("{\"a\" 1}"));
    CHECK(!parses("{a:1}"));
    CHECK(!parses("[1 2]"));
    CHECK(!parses("[1] [2]"));
    CHECK(!parses("tru"));
    CHECK(!parses(R"({"a":1,"a":2})")); // a repeated name
    const OperationResult<JsonDocument> bad = JsonDocument::parse(view_of("[1,,2]"));
    CHECK(bad.failed() && (*bad.error()).kind() == ErrorKind::invalid_argument && (*bad.error()).code() == 3);
}

void limits() {
    JsonLimits shallow;
    shallow.max_depth = 3;
    CHECK(failure_kind("[[[1]]]", shallow) == static_cast<ErrorKind>(0));
    CHECK(failure_kind("[[[[1]]]]", shallow) == ErrorKind::limit_exceeded);
    JsonLimits small;
    small.max_bytes = 8;
    CHECK(failure_kind("[1,2,3,4,5]", small) == ErrorKind::limit_exceeded);
    JsonLimits few;
    few.max_nodes = 3;
    CHECK(failure_kind("[1,2]", few) == static_cast<ErrorKind>(0));
    CHECK(failure_kind("[1,2,3]", few) == ErrorKind::limit_exceeded);
    JsonLimits narrow;
    narrow.max_object_members = 2;
    CHECK(failure_kind(R"({"a":1,"b":2,"c":3})", narrow) == ErrorKind::limit_exceeded);
    // Deep nesting under the default limit fails as a result, never by
    // exhausting the stack.
    const std::string deep(100000, '[');
    CHECK(failure_kind(deep, JsonLimits{}) == ErrorKind::limit_exceeded);
}

void writer_basics() {
    Utf8String out;
    JsonWriter writer(out);
    writer.begin_object();
    writer.member_string("text", "line\nbreak \"quoted\" \x01 \x7f");
    writer.member_integer("negative", -42);
    writer.key("big");
    writer.unsigned_integer(18446744073709551615ULL);
    writer.member_number("half", 0.5);
    writer.member_boolean("yes", true);
    writer.key("nothing");
    writer.null_value();
    writer.key("list");
    writer.begin_array();
    writer.integer(1);
    writer.string("\xE2\x80\xA8"); // U+2028, escaped for JavaScript readers
    writer.end_array();
    writer.end_object();
    CHECK(writer.complete());
    CHECK(writer.status().succeeded());
    CHECK(text_of(out.view()) ==
          R"({"text":"line\nbreak \"quoted\" \u0001 \u007f","negative":-42,"big":18446744073709551615,"half":0.5,)"
          R"("yes":true,"nothing":null,"list":[1," "]})");
    // What it wrote reads back.
    const JsonDocument back = parse_or_empty(text_of(out.view()));
    CHECK(text_of(back.member_text(back.root(), "text").value_or(TextView())) ==
          "line\nbreak \"quoted\" \x01 \x7f");
}

void writer_base64() {
    const Byte bytes[] = {'M', 'a', 'n', 'M', 'a', 'M'};
    const char* const expected[] = {R"("")", R"("TQ==")", R"("TWE=")", R"("TWFu")", R"("TWFuTQ==")", R"("TWFuTWE=")",
                                    R"("TWFuTWFN")"};
    for (usize count = 0; count <= 6; ++count) {
        Utf8String out;
        JsonWriter writer(out);
        writer.base64(View<Byte>(bytes, count));
        CHECK(text_of(out.view()) == expected[count]);
    }
}

void writer_refusals() {
    {
        Utf8String out;
        JsonWriter writer(out);
        writer.begin_object();
        writer.integer(1); // a value where a key belongs
        CHECK(writer.failed());
        CHECK(writer.status().failed());
    }
    {
        Utf8String out;
        JsonWriter writer(out);
        writer.begin_array();
        CHECK(!writer.complete());
        CHECK(writer.status().failed()); // still open
        writer.end_array();
        CHECK(writer.status().succeeded());
        writer.integer(2); // a second top-level value
        CHECK(writer.failed());
    }
    {
        Utf8String out;
        JsonWriter writer(out);
        writer.number(__builtin_nan(""));
        CHECK(writer.failed());
    }
    {
        Utf8String out;
        JsonWriter writer(out, 10);
        writer.string("more than ten bytes");
        CHECK(writer.failed());
        const stxlib::OperationStatus status = writer.status();
        CHECK(status.failed() && (*status.error()).kind() == ErrorKind::limit_exceeded);
    }
    {
        Utf8String out;
        JsonWriter writer(out);
        writer.begin_object();
        writer.integer(1);
        CHECK(writer.failed());
        writer.reset();
        out.clear();
        CHECK(!writer.failed());
        writer.string("fresh");
        CHECK(writer.status().succeeded());
        CHECK(text_of(out.view()) == R"("fresh")");
    }
}

void writer_copy() {
    const std::string source = R"({"a":[1,2.5,"x",{"b":null,"c":[true,false]}],"d":{}})";
    const JsonDocument document = parse_or_empty(source);
    Utf8String out;
    JsonWriter writer(out);
    writer.begin_array();
    writer.copy(document, document.root());
    writer.raw("17");
    writer.end_array();
    CHECK(writer.status().succeeded());
    CHECK(text_of(out.view()) == "[" + source + ",17]");
}

} // namespace

int main() {
    kinds_and_queries();
    numbers();
    strings();
    structure();
    limits();
    writer_basics();
    writer_base64();
    writer_refusals();
    writer_copy();
    return stxlib_tests::finish("json");
}
