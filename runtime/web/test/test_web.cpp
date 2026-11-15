// test_web.cpp: host tests of the web service's pure modules: json.h,
// http.h, auth.h, config_edit.h and api.h's value rules.
//
// Bounded exhaustive runs (every input within the stated bounds, not a
// sample; the sanitizers watch each one):
// - JSON: every text of up to 4 bytes over a 12-byte alphabet of JSON's
//   structural bytes: parsed or refused, and a parsed one's tokens inside
//   the text and well nested; every string of up to 2 arbitrary bytes
//   written by JsonWriter parses back as a string;
// - HTTP: one request fed to the assembler in every split into two reads
//   gives the same request; every single-byte change of a request head is
//   parsed or refused, and a parsed head's ranges lie inside it;
// - auth: every single-byte change of a password record is refused or
//   canonical; gate transitions over every sequence of 8 failures and
//   waits; setup codes read with Crockford's substitutions;
// - config_edit: names over every string of up to 3 bytes of a 6-byte
//   alphabet against the rule's definition;
// - value rules: dotted IPv4 over every 4-number combination drawn from
//   edge values, against a reference.
// Known answers for the rest.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../supervisor/config/test/test_support.h"
#include "../api.h"
#include "../auth.h"
#include "../config_edit.h"
#include "../http.h"
#include "../json.h"

namespace {

using namespace stxweb;
using stxtest::check;
using stxtest::text_of;

// A deterministic random source: a counter's bytes.
class CountingRandom final : public RandomSource {
public:
    CountingRandom() noexcept : next_(1), fail_(false) {}
    [[nodiscard]] bool fill(std::span<std::uint8_t> bytes) noexcept override {
        if (fail_) {
            return false;
        }
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(next_ * 37u + 11u);
            next_ += 1;
        }
        return true;
    }
    void set_fail(bool fail) noexcept {
        fail_ = fail;
    }

private:
    std::uint32_t next_;
    bool fail_;
};

JsonDocument document;

// ---------------------------------------------------------------------------
// JSON.

[[nodiscard]] bool tokens_well_formed(std::size_t size) {
    if (document.token_count() == 0u) {
        return false;
    }
    for (std::size_t index = 0; index < document.token_count(); index += 1) {
        const JsonToken& token = document.token(index);
        if (static_cast<std::size_t>(token.offset) + token.length > size || token.end > document.token_count() ||
            token.end <= index) {
            return false;
        }
    }
    return document.token(0).end == document.token_count();
}

void test_json_exhaustive() {
    const char alphabet[] = {'{', '}', '[', ']', '"', ':', ',', '1', 'a', '\\', ' ', 'n'};
    const std::size_t letters = sizeof(alphabet);
    char text[4];
    std::size_t parsed = 0;
    bool consistent = true;
    for (std::size_t length = 1; length <= 4u; length += 1) {
        std::size_t combinations = 1;
        for (std::size_t index = 0; index < length; index += 1) {
            combinations *= letters;
        }
        for (std::size_t number = 0; number < combinations; number += 1) {
            std::size_t rest = number;
            for (std::size_t index = 0; index < length; index += 1) {
                text[index] = alphabet[rest % letters];
                rest /= letters;
            }
            const JsonOutcome outcome = parse_json(std::span<const char>(text, length), document);
            if (outcome.is_parsed()) {
                parsed += 1;
                consistent = consistent && tokens_well_formed(length);
            } else {
                const JsonRefused* const refused = outcome.as_refused();
                consistent = consistent && refused != nullptr && (*refused).offset <= length;
            }
        }
    }
    check(consistent, "json_exhaustive", "every parse well formed, every refusal inside the text");
    check(parsed > 0u, "json_exhaustive", "some texts parse ([], {}, 1, \"a\" ...)");
}

void test_json_writer_exhaustive() {
    char input[2];
    char output[64];
    char decoded[64];
    bool all_parse = true;
    bool ascii_kept = true;
    for (std::size_t length = 0; length <= 2u; length += 1) {
        const std::size_t combinations = length == 0u ? 1u : (length == 1u ? 256u : 65536u);
        for (std::size_t number = 0; number < combinations; number += 1) {
            input[0] = static_cast<char>(number & 0xffu);
            input[1] = static_cast<char>((number >> 8u) & 0xffu);
            JsonWriter writer(output);
            writer.string(std::span<const char>(input, length));
            if (!writer.ok()) {
                all_parse = false;
                continue;
            }
            const JsonOutcome outcome = parse_json(std::span<const char>(output, writer.size()), document);
            if (!outcome.is_parsed() || document.token(0).kind != JsonKind::string) {
                all_parse = false;
                continue;
            }
            bool ascii = true;
            for (std::size_t index = 0; index < length; index += 1) {
                ascii = ascii && static_cast<unsigned char>(input[index]) < 0x80u;
            }
            if (ascii) {
                const std::optional<std::size_t> size = document.decode_string(0, decoded);
                ascii_kept = ascii_kept && size.has_value() && *size == length &&
                             std::memcmp(decoded, input, length) == 0;
            }
        }
    }
    check(all_parse, "json_writer", "every string of up to 2 bytes written as valid JSON");
    check(ascii_kept, "json_writer", "ASCII strings decode back unchanged");
}

[[nodiscard]] bool json_refused_with(const char* text, JsonError error) {
    const JsonOutcome outcome = parse_json(text_of(text), document);
    const JsonRefused* const refused = outcome.as_refused();
    return refused != nullptr && (*refused).error == error;
}

void test_json_known() {
    check(parse_json(text_of("{\"password\":\"x\",\"n\":3}"), document).is_parsed(), "json", "object parses");
    const std::optional<std::size_t> member = document.member(0, "n");
    check(member.has_value() && document.token(*member).integer == 3, "json", "member found with its integer");
    check(!document.member(0, "absent").has_value(), "json", "absent member");
    check(json_refused_with("{\"a\":1,\"a\":2}", JsonError::key_repeated), "json", "repeated key refused");
    check(json_refused_with("1.5", JsonError::number_not_integer), "json", "non-integer refused");
    check(json_refused_with("01", JsonError::number_malformed) || json_refused_with("01", JsonError::trailing_bytes),
          "json", "leading zero refused");
    check(json_refused_with("9007199254740992", JsonError::number_out_of_range), "json", "2^53 refused");
    check(parse_json(text_of("9007199254740991"), document).is_parsed(), "json", "2^53 - 1 accepted");
    check(json_refused_with("\xef\xbb\xbf{}", JsonError::byte_order_mark), "json", "byte order mark refused");
    check(json_refused_with("\"\\ud800\"", JsonError::surrogate_unpaired), "json", "unpaired surrogate refused");
    check(json_refused_with("\"\x01\"", JsonError::string_control_byte), "json", "control byte refused");
    check(json_refused_with("\"\xc0\xaf\"", JsonError::utf8_malformed), "json", "overlong UTF-8 refused");
    check(json_refused_with("", JsonError::empty), "json", "empty refused");
    check(json_refused_with("{} x", JsonError::trailing_bytes), "json", "trailing bytes refused");
    char deep[64];
    std::size_t size = 0;
    for (std::size_t index = 0; index <= json_depth_max; index += 1) {
        deep[size] = '[';
        size += 1;
    }
    for (std::size_t index = 0; index <= json_depth_max; index += 1) {
        deep[size] = ']';
        size += 1;
    }
    const JsonOutcome too_deep = parse_json(std::span<const char>(deep, size), document);
    check(too_deep.as_refused() != nullptr && (*too_deep.as_refused()).error == JsonError::too_deep, "json",
          "nesting one past the limit refused");
    check(parse_json(std::span<const char>(deep + 1, size - 2u), document).is_parsed(), "json",
          "nesting at the limit accepted");
}

// ---------------------------------------------------------------------------
// HTTP.

RequestAssembler assembler;

const char* const login_request =
    "POST /api/login HTTP/1.1\r\n"
    "Host: 192.168.1.20\r\n"
    "Content-Type: application/json\r\n"
    "Cookie: a=1; stxsession=abc\r\n"
    "Content-Length: 16\r\n"
    "\r\n"
    "{\"password\":\"x\"}";

// Feeds text in two reads split at split; the final state.
[[nodiscard]] AssemblyState feed_split(const char* text, std::size_t size, std::size_t split) {
    assembler.reset();
    std::size_t done = 0;
    const std::size_t parts[2] = {split, size - split};
    AssemblyState state = AssemblyState::receiving;
    for (const std::size_t part : parts) {
        std::size_t left = part;
        while (left > 0u && state == AssemblyState::receiving) {
            const std::span<char> space = assembler.free_space();
            if (space.empty()) {
                return AssemblyState::refused;
            }
            const std::size_t count = left < space.size() ? left : space.size();
            std::memcpy(space.data(), text + done, count);
            state = assembler.commit(count);
            done += count;
            left -= count;
        }
    }
    return state;
}

void test_http_assembler() {
    const std::size_t size = std::strlen(login_request);
    bool same = true;
    for (std::size_t split = 0; split <= size; split += 1) {
        const AssemblyState state = feed_split(login_request, size, split);
        const RequestHead* const head = assembler.head();
        same = same && state == AssemblyState::complete && head != nullptr && (*head).method == Method::post &&
               (*head).content_length == 16u && assembler.body().size() == 16u &&
               equals_exact(range_text(assembler.bytes(), (*head).path), "/api/login") &&
               (*head).cookie.has_value();
    }
    check(same, "http_assembler", "every split into two reads gives the same request");
    const std::optional<std::span<const char>> cookie =
        cookie_value(range_text(assembler.bytes(), *(*assembler.head()).cookie), session_cookie_name);
    check(cookie.has_value() && equals_exact(*cookie, "abc"), "http_assembler", "session cookie read");

    const char* const upload =
        "POST /api/upload/package HTTP/1.1\r\nHost: h\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: 1000000\r\n\r\nPK";
    const AssemblyState streaming = feed_split(upload, std::strlen(upload), 10);
    check(streaming == AssemblyState::streaming && assembler.streamed_prefix().size() == 2u, "http_assembler",
          "an upload's head stops the assembler, its first bytes kept");
    const char* const too_large =
        "POST /api/login HTTP/1.1\r\nHost: h\r\nContent-Length: 1000000\r\n\r\n";
    const AssemblyState refused = feed_split(too_large, std::strlen(too_large), 0);
    check(refused == AssemblyState::refused && assembler.refusal() != nullptr && (*assembler.refusal()).status == 413,
          "http_assembler", "a large body outside the upload routes refused with 413");
}

[[nodiscard]] std::uint16_t head_status(const char* text) {
    const HeadOutcome outcome = parse_request_head(text_of(text));
    const HeadRefused* const refused = outcome.as_refused();
    return refused != nullptr ? (*refused).status : 200u;
}

void test_http_known() {
    check(head_status("GET / HTTP/1.1\r\nHost: a\r\n\r\n") == 200u, "http", "minimal GET");
    check(head_status("GET / HTTP/1.1\nHost: a\r\n\r\n") == 400u, "http", "bare LF refused");
    check(head_status("GET / HTTP/1.0\r\nHost: a\r\n\r\n") == 505u, "http", "HTTP/1.0 refused with 505");
    check(head_status("PUT / HTTP/1.1\r\nHost: a\r\n\r\n") == 501u, "http", "PUT refused with 501");
    check(head_status("GET / HTTP/1.1\r\n\r\n") == 400u, "http", "missing Host refused");
    check(head_status("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n") == 400u, "http", "two Hosts refused");
    check(head_status("GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n") == 400u, "http", "absolute form refused");
    check(head_status("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n") == 501u, "http",
          "chunked refused with 501");
    check(head_status("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n\r\n") == 417u, "http",
          "Expect refused with 417");
    check(head_status("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n") == 400u, "http", "obs-fold refused");
    check(head_status("GET / HTTP/1.1\r\nHost : a\r\n\r\n") == 400u, "http", "blank before colon refused");
    check(head_status("GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n") == 400u, "http",
          "GET with a body refused");

    // Every single-byte change of a head: parsed or refused, ranges inside.
    const char* const head = "POST /api/x?y=1 HTTP/1.1\r\nHost: a.b\r\nOrigin: https://a.b\r\nX-STX-CSRF: t\r\n"
                             "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n";
    const std::size_t size = std::strlen(head);
    char changed[256];
    bool inside = true;
    for (std::size_t position = 0; position < size; position += 1) {
        for (int value = 0; value < 256; value += 1) {
            std::memcpy(changed, head, size);
            changed[position] = static_cast<char>(value);
            const HeadOutcome outcome = parse_request_head(std::span<const char>(changed, size));
            const RequestHead* const parsed = outcome.as_head();
            if (parsed == nullptr) {
                const HeadRefused* const refused = outcome.as_refused();
                inside = inside && refused != nullptr && (*refused).status >= 400u && (*refused).status <= 505u;
                continue;
            }
            const ByteRange ranges[] = {(*parsed).path, (*parsed).host};
            for (const ByteRange& range : ranges) {
                inside = inside && static_cast<std::size_t>(range.offset) + range.length <= size;
            }
            inside = inside && (*parsed).head_length == size;
        }
    }
    check(inside, "http", "every single-byte change parsed within bounds or refused with a 4xx/5xx");

    char out[http_response_head_bytes_max];
    ResponseHeadFields fields{};
    fields.status = 200;
    fields.content_type = "application/json";
    fields.content_length = 12;
    fields.set_cookie = "stxsession=abc; HttpOnly; Secure; SameSite=Strict; Path=/";
    const std::optional<std::size_t> written = write_response_head(fields, out);
    const std::span<const char> text(out, written.has_value() ? *written : 0u);
    bool has_policy = false;
    for (std::size_t index = 0; index + 23u <= text.size(); index += 1) {
        has_policy = has_policy || std::memcmp(text.data() + index, "Content-Security-Policy", 23) == 0;
    }
    check(written.has_value() && has_policy, "http", "response head with its security fields");
}

// ---------------------------------------------------------------------------
// Auth.

void test_sessions() {
    CountingRandom random;
    static SessionTable table;
    std::optional<std::size_t> first = table.create(1000, random);
    check(first.has_value() && table.live_count() == 1u, "sessions", "created");
    const Session copy = table.session(*first);
    check(table.find(std::span<const char>(copy.token.bytes, token_text_bytes), 2000) == first, "sessions",
          "found by token");
    check(table.csrf_matches(*first, std::span<const char>(copy.csrf.bytes, token_text_bytes)), "sessions",
          "CSRF token matches");
    check(!table.csrf_matches(*first, std::span<const char>(copy.token.bytes, token_text_bytes)), "sessions",
          "the session token is not the CSRF token");
    check(!table.find(std::span<const char>(copy.token.bytes, token_text_bytes - 1u), 2000).has_value(), "sessions",
          "a cut token not found");
    check(!table.find(std::span<const char>(copy.token.bytes, token_text_bytes), 2000 + session_idle_ms + 1u)
               .has_value(),
          "sessions", "idle session expired");
    for (std::size_t index = 0; index < sessions_max + 2u; index += 1) {
        const std::optional<std::size_t> made = table.create(10000 + index, random);
        check(made.has_value(), "sessions", "room always made");
    }
    check(table.live_count() == sessions_max, "sessions", "at most sessions_max live");
    random.set_fail(true);
    check(!table.create(20000, random).has_value() && table.live_count() == sessions_max, "sessions",
          "random failure changes nothing");
    table.remove_all_except(std::nullopt);
    check(table.live_count() == 0u, "sessions", "all removed");
}

void test_gates_exhaustive() {
    // Every sequence of 8 events, each a failure or a long wait.
    bool bounded = true;
    for (std::uint32_t pattern = 0; pattern < 256u; pattern += 1) {
        GateState state = GateState::open(0);
        std::uint64_t now = 0;
        std::uint32_t locks_seen = 0;
        for (std::uint32_t step = 0; step < 8u; step += 1) {
            if (((pattern >> step) & 1u) != 0u) {
                now += gate_lock_max_ms + 1u;
                state = gate_after_time(state, now);
            } else {
                now += 1;
                state = gate_after_time(state, now);
                if (state.as_locked() == nullptr) {
                    state = gate_after_failure(state, now);
                }
            }
            const GateLocked* const locked = state.as_locked();
            if (locked != nullptr) {
                bounded = bounded && (*locked).until_ms > now && (*locked).until_ms - now <= gate_lock_max_ms &&
                          (*locked).locks >= locks_seen;
                locks_seen = (*locked).locks;
            }
            const GateOpen* const open = state.as_open();
            if (open != nullptr) {
                bounded = bounded && (*open).failures < gate_failures_before_lock;
            }
        }
    }
    check(bounded, "gates", "locks bounded by the maximum, open gates below the threshold");
    check(gate_lock_ms(1) == gate_lock_first_ms && gate_lock_ms(2) == 2u * gate_lock_first_ms &&
              gate_lock_ms(40) == gate_lock_max_ms,
          "gates", "lock lengths double up to the maximum");

    static GateTable table;
    const std::uint8_t address[4] = {192, 168, 1, 9};
    const ClientKey client = client_key_ipv4(std::span<const std::uint8_t, 4>(address));
    std::uint64_t now = 1000;
    for (std::uint32_t failure = 0; failure < gate_failures_before_lock; failure += 1) {
        check(table.check(client, now).verdict == GateVerdict::allowed, "gates", "allowed below the threshold");
        (void)table.record_failure(client, now);
        now += 1;
    }
    const GateDecision locked = table.check(client, now);
    check(locked.verdict == GateVerdict::locked && locked.retry_after_ms > 0u, "gates", "locked at the threshold");
    now += gate_lock_first_ms + 1u;
    check(table.check(client, now).verdict == GateVerdict::allowed, "gates", "allowed after the lock (probation)");
    table.record_success(client);
    check(!table.state_of(client).has_value(), "gates", "a success forgets the client");
}

void test_password_records() {
    std::array<std::uint8_t, password_salt_bytes> salt{};
    std::array<std::uint8_t, password_hash_bytes> hash{};
    for (std::size_t index = 0; index < salt.size(); index += 1) {
        salt[index] = static_cast<std::uint8_t>(index * 17u);
    }
    for (std::size_t index = 0; index < hash.size(); index += 1) {
        hash[index] = static_cast<std::uint8_t>(255u - index);
    }
    const PasswordRecord records[] = {PasswordRecord::argon2id(argon2id_default_cost, salt, hash),
                                      PasswordRecord::scrypt(scrypt_default_cost, salt, hash)};
    char text[password_record_text_max];
    char changed[password_record_text_max];
    char again[password_record_text_max];
    for (const PasswordRecord& record : records) {
        const std::optional<std::size_t> size = format_password_record(record, text);
        check(size.has_value(), "password_records", "formats");
        if (!size.has_value()) {
            continue;
        }
        const std::optional<PasswordRecord> parsed = parse_password_record(std::span<const char>(text, *size));
        check(parsed.has_value() && (*parsed).salt() == salt && (*parsed).hash() == hash, "password_records",
              "round trip");
        bool canonical = true;
        for (std::size_t position = 0; position < *size; position += 1) {
            for (int value = 0; value < 256; value += 1) {
                std::memcpy(changed, text, *size);
                changed[position] = static_cast<char>(value);
                const std::optional<PasswordRecord> back = parse_password_record(std::span<const char>(changed, *size));
                if (!back.has_value()) {
                    continue;
                }
                const std::optional<std::size_t> again_size = format_password_record(*back, again);
                canonical = canonical && again_size.has_value() && *again_size == *size &&
                            std::memcmp(again, changed, *size) == 0;
            }
        }
        check(canonical, "password_records", "every single-byte change refused or canonical");
    }
    check(!argon2id_cost_valid(Argon2idCost{4u, 2u, 1u}), "password_records", "Argon2id memory below 8 KiB per lane");
    check(!scrypt_cost_valid(ScryptCost{21u, 8u, 1u}), "password_records", "scrypt N above 2^20");
    check(check_new_password(text_of("1234567")) == PasswordRule::too_short &&
              check_new_password(text_of("12345678")) == PasswordRule::acceptable,
          "password_records", "length at and below the minimum");
}

void test_setup_codes() {
    std::uint8_t random[setup_code_characters];
    for (std::size_t index = 0; index < setup_code_characters; index += 1) {
        random[index] = static_cast<std::uint8_t>(index * 29u);
    }
    const SetupCode code = setup_code_of(random);
    char text[setup_code_text_bytes];
    format_setup_code(code, text);
    const std::optional<SetupCode> read = read_setup_code(std::span<const char>(text, setup_code_text_bytes));
    check(read.has_value() && setup_codes_equal(*read, code), "setup_codes", "formatted code reads back");
    char lower[setup_code_text_bytes + 2];
    std::size_t size = 0;
    for (std::size_t index = 0; index < setup_code_text_bytes; index += 1) {
        char letter = text[index];
        if (letter >= 'A' && letter <= 'Z') {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
        if (letter == '0') {
            letter = 'o';
        }
        if (letter == '1') {
            letter = 'l';
        }
        lower[size] = letter;
        size += 1;
    }
    lower[size] = ' ';
    size += 1;
    const std::optional<SetupCode> loose = read_setup_code(std::span<const char>(lower, size));
    check(loose.has_value() && setup_codes_equal(*loose, code), "setup_codes",
          "lower case, O for 0, l for 1 and blanks accepted");
    check(!read_setup_code(text_of("ABCDE-FGHI")).has_value(), "setup_codes", "nine characters refused");
    check(!read_setup_code(text_of("ABCDE-FGHIU")).has_value(), "setup_codes", "U is not in the alphabet");
}

// ---------------------------------------------------------------------------
// Configuration editing and value rules.

[[nodiscard]] bool name_rule(std::span<const char> name) {
    if (name.empty() || name.size() > stxconfig::name_length_max || name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    for (const char letter : name) {
        const bool allowed = (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

void test_config_names_exhaustive() {
    const char alphabet[] = {'a', 'z', '0', '-', 'A', '_'};
    char name[3];
    bool agree = true;
    for (std::size_t length = 0; length <= 3u; length += 1) {
        std::size_t combinations = 1;
        for (std::size_t index = 0; index < length; index += 1) {
            combinations *= sizeof(alphabet);
        }
        for (std::size_t number = 0; number < combinations; number += 1) {
            std::size_t rest = number;
            for (std::size_t index = 0; index < length; index += 1) {
                name[index] = alphabet[rest % sizeof(alphabet)];
                rest /= sizeof(alphabet);
            }
            const std::span<const char> view(name, length);
            agree = agree && config_name_valid(view) == name_rule(view);
        }
    }
    check(agree, "config_names", "every name of up to 3 bytes judged as the rule says");
}

ConfigEditor editor;

[[nodiscard]] bool text_is(const char* expected) {
    const std::span<const char> text = editor.text();
    return text.size() == std::strlen(expected) && std::memcmp(text.data(), expected, text.size()) == 0;
}

void test_config_edit() {
    check(editor.load(text_of("[system]\nhostname = a")), "config_edit", "load adds the final line end");
    check(text_is("[system]\nhostname = a\n"), "config_edit", "final line end added");
    check(editor.set_setting(text_of("system"), text_of("hostname"), text_of("b")) == EditResult::done &&
              text_is("[system]\nhostname = b\n"),
          "config_edit", "setting replaced in place");
    check(editor.set_setting(text_of("network"), text_of("mode"), text_of("dhcp")) == EditResult::done &&
              text_is("[system]\nhostname = b\n\n[network]\nmode = dhcp\n"),
          "config_edit", "section added at the end");
    check(editor.set_start(text_of("player"), true) == EditResult::done && editor.starts(text_of("player")),
          "config_edit", "start added");
    check(!editor.starts(text_of("play")), "config_edit", "a prefix is not a start");
    check(editor.set_start(text_of("player"), false) == EditResult::done && !editor.starts(text_of("player")),
          "config_edit", "start removed");
    const EditSetting settings[] = {{text_of("command"), text_of("/bin/sleep 1")}, {text_of("restart"), text_of("always")}};
    check(editor.replace_section(text_of("service-player"), settings) == EditResult::done, "config_edit",
          "section replaced");
    check(editor.remove_setting(text_of("service-player"), text_of("restart")) == EditResult::done, "config_edit",
          "setting removed");
    check(editor.remove_section(text_of("service-player")) == EditResult::done, "config_edit", "section removed");
    check(editor.set_setting(text_of("system"), text_of("Host"), text_of("x")) == EditResult::name_not_allowed,
          "config_edit", "upper-case name refused");
    check(editor.set_setting(text_of("system"), text_of("hostname"), text_of(" x")) == EditResult::value_not_allowed,
          "config_edit", "leading blank refused");
    check(editor.set_setting(text_of("system"), text_of("hostname"), text_of("a\nb")) == EditResult::value_not_allowed,
          "config_edit", "line end in a value refused");
    static char huge[edit_text_bytes_max + 1];
    std::memset(huge, '#', sizeof(huge));
    check(!editor.load(std::span<const char>(huge, sizeof(huge))), "config_edit", "a text over the limit refused");
}

// The reference: four decimal numbers 0..255, no leading zeros.
[[nodiscard]] bool ipv4_reference(const char* const parts[4]) {
    for (std::size_t index = 0; index < 4u; index += 1) {
        const char* const part = parts[index];
        const std::size_t length = std::strlen(part);
        if (length == 0u || length > 3u || (length > 1u && part[0] == '0')) {
            return false;
        }
        std::uint32_t value = 0;
        for (std::size_t digit = 0; digit < length; digit += 1) {
            if (part[digit] < '0' || part[digit] > '9') {
                return false;
            }
            value = value * 10u + static_cast<std::uint32_t>(part[digit] - '0');
        }
        if (value > 255u) {
            return false;
        }
    }
    return true;
}

void test_value_rules() {
    const char* const numbers[] = {"0", "1", "9", "10", "99", "100", "255", "256", "01", "", "a", "1000"};
    const std::size_t count = sizeof(numbers) / sizeof(numbers[0]);
    char text[32];
    bool agree = true;
    for (std::size_t a = 0; a < count; a += 1) {
        for (std::size_t b = 0; b < count; b += 1) {
            for (std::size_t c = 0; c < count; c += 1) {
                for (std::size_t d = 0; d < count; d += 1) {
                    const char* const parts[4] = {numbers[a], numbers[b], numbers[c], numbers[d]};
                    const int size = std::snprintf(text, sizeof(text), "%s.%s.%s.%s", parts[0], parts[1], parts[2],
                                                   parts[3]);
                    const std::span<const char> view(text, static_cast<std::size_t>(size));
                    agree = agree && ipv4_text_valid(view) == ipv4_reference(parts);
                }
            }
        }
    }
    check(agree, "value_rules", "dotted IPv4 over every edge combination");
    check(ipv4_prefix_text_valid(text_of("10.0.0.1/24")) && ipv4_prefix_text_valid(text_of("10.0.0.1/0")) &&
              ipv4_prefix_text_valid(text_of("10.0.0.1/32")),
          "value_rules", "prefixes 0, 24 and 32");
    check(!ipv4_prefix_text_valid(text_of("10.0.0.1/33")) && !ipv4_prefix_text_valid(text_of("10.0.0.1/08")) &&
              !ipv4_prefix_text_valid(text_of("10.0.0.1")),
          "value_rules", "prefix 33, a leading zero, no prefix refused");
    check(hostname_valid(text_of("stx-appliance")) && hostname_valid(text_of("a.b-c.d")), "value_rules",
          "host names accepted");
    check(!hostname_valid(text_of("-a")) && !hostname_valid(text_of("a-")) && !hostname_valid(text_of("a..b")) &&
              !hostname_valid(text_of("")) && !hostname_valid(text_of("a_b")),
          "value_rules", "host names refused");
    char label[80];
    std::memset(label, 'a', sizeof(label));
    check(hostname_valid(std::span<const char>(label, 63)) && !hostname_valid(std::span<const char>(label, 64)),
          "value_rules", "a label of 63 accepted, 64 refused");
    check(ssh_key_valid(text_of("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl "
                                "owner@laptop")),
          "value_rules", "an ed25519 key with a comment accepted");
    check(!ssh_key_valid(text_of("ssh-dss AAAAB3NzaC1kc3MAAACB")) && !ssh_key_valid(text_of("ssh-ed25519")) &&
              !ssh_key_valid(text_of("ssh-ed25519 AA=A")) && !ssh_key_valid(text_of("ssh-ed25519  AAAA")),
          "value_rules", "DSA, no key, padding inside, two blanks refused");
}

}  // namespace

int main() {
    test_json_exhaustive();
    test_json_writer_exhaustive();
    test_json_known();
    test_http_assembler();
    test_http_known();
    test_sessions();
    test_gates_exhaustive();
    test_password_records();
    test_setup_codes();
    test_config_names_exhaustive();
    test_config_edit();
    test_value_rules();
    return stxtest::finish("test_web");
}
