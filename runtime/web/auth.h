// auth.h: the web interface's login security, its pure part (C2; design
// dev/notes/planning/stages/C2-web.md, section 5): session tokens and
// their table, the lockout gates, the stored password record's format, the
// first-login setup code, and constant-time comparison.
//
// Pure code: no system call, no operating-system header, no allocation.
// Randomness comes from a RandomSource the caller supplies (the system's
// is OpenSSL's RAND_bytes, tls_server.h); time is milliseconds of a
// monotonic clock, passed in.

#ifndef STXLIB_RUNTIME_WEB_AUTH_H
#define STXLIB_RUNTIME_WEB_AUTH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace stxweb {

// ---------------------------------------------------------------------------
// Randomness and comparison.

class RandomSource {
public:
    RandomSource() noexcept = default;
    RandomSource(const RandomSource&) = delete;
    RandomSource& operator=(const RandomSource&) = delete;

    // Fills bytes with cryptographically strong random bytes. False when
    // that is not possible; bytes are then unspecified and must not be
    // used.
    [[nodiscard]] virtual bool fill(std::span<std::uint8_t> bytes) noexcept = 0;

protected:
    ~RandomSource() = default;
};

// Equality of two byte strings in time that depends only on their lengths
// (which are not secret here: tokens and hashes have fixed lengths).
[[nodiscard]] bool constant_time_equal(std::span<const std::uint8_t> left,
                                       std::span<const std::uint8_t> right) noexcept;
[[nodiscard]] bool constant_time_equal_text(std::span<const char> left, std::span<const char> right) noexcept;

// Lower-case hex of bytes into out (2 * bytes.size() characters, asserted).
void hex_of(std::span<const std::uint8_t> bytes, std::span<char> out) noexcept;

// ---------------------------------------------------------------------------
// Sessions.

constexpr std::size_t token_random_bytes = 32;
constexpr std::size_t token_text_bytes = 2u * token_random_bytes;
constexpr std::size_t sessions_max = 8;
// A session ends after 15 minutes without a request, and 8 hours after
// its login in any case.
constexpr std::uint64_t session_idle_ms = 15u * 60u * 1000u;
constexpr std::uint64_t session_absolute_ms = 8u * 60u * 60u * 1000u;
// The session cookie's name.
constexpr const char* session_cookie_name = "stxsession";

struct TokenText {
    char bytes[token_text_bytes];
};

struct Session {
    // Sent once, in the session cookie (HttpOnly).
    TokenText token;
    // Sent in the login response's body and with GET /api/session; the
    // page sends it back in X-STX-CSRF with every state-changing request.
    TokenText csrf;
    std::uint64_t created_ms;
    std::uint64_t last_used_ms;
};

// Whether a session has expired at now_ms.
[[nodiscard]] bool session_expired(const Session& session, std::uint64_t now_ms) noexcept;

class SessionTable {
public:
    SessionTable() noexcept;
    SessionTable(const SessionTable&) = delete;
    SessionTable& operator=(const SessionTable&) = delete;

    // A new session with fresh random tokens. When the table is full the
    // least recently used session ends to make room. Nothing when the
    // random source fails (no session is changed then).
    [[nodiscard]] std::optional<std::size_t> create(std::uint64_t now_ms, RandomSource& random) noexcept;

    // The live session whose token equals token (compared in constant time
    // against every live slot), touched (last_used_ms = now_ms); nothing
    // when there is none. Expired sessions end first.
    [[nodiscard]] std::optional<std::size_t> find(std::span<const char> token, std::uint64_t now_ms) noexcept;

    // Whether header equals the session's CSRF token (constant time).
    [[nodiscard]] bool csrf_matches(std::size_t index, std::span<const char> header) const noexcept;

    // index must name a live session (asserted).
    [[nodiscard]] const Session& session(std::size_t index) const noexcept;
    [[nodiscard]] bool is_live(std::size_t index) const noexcept;

    void remove(std::size_t index) noexcept;
    // Ends every session but keep (when given).
    void remove_all_except(std::optional<std::size_t> keep) noexcept;
    // Ends every expired session.
    void expire(std::uint64_t now_ms) noexcept;

    [[nodiscard]] std::size_t live_count() const noexcept;

private:
    std::optional<Session> slots_[sessions_max];
};

// ---------------------------------------------------------------------------
// Lockout gates (one per client address; IPv6 by its /64 prefix).

constexpr std::uint32_t gate_failures_before_lock = 5;
constexpr std::uint64_t gate_lock_first_ms = 60u * 1000u;
constexpr std::uint64_t gate_lock_max_ms = 32u * 60u * 1000u;
constexpr std::size_t gate_clients_max = 16;

enum class AddressFamily : std::uint8_t {
    ipv4,
    ipv6,
};

struct ClientKey {
    AddressFamily family;
    // IPv4: the 4 address bytes, the rest zero. IPv6: the first 8 bytes
    // (the /64 prefix), the rest zero.
    std::uint8_t bytes[16];
};

[[nodiscard]] ClientKey client_key_ipv4(std::span<const std::uint8_t, 4> address) noexcept;
[[nodiscard]] ClientKey client_key_ipv6(std::span<const std::uint8_t, 16> address) noexcept;
[[nodiscard]] bool client_keys_equal(const ClientKey& left, const ClientKey& right) noexcept;

// A gate's states. Open: failures since the last success or lock, fewer
// than gate_failures_before_lock. Locked: no password is even checked until
// until_ms. Probation: a lock has ended; one failure locks again, for
// twice as long (up to gate_lock_max_ms); a success opens the gate.
struct GateOpen {
    std::uint32_t failures;
};
struct GateLocked {
    std::uint64_t until_ms;
    std::uint32_t locks;
};
struct GateProbation {
    std::uint32_t locks;
};

class GateState {
public:
    [[nodiscard]] static GateState open(std::uint32_t failures) noexcept;
    [[nodiscard]] static GateState locked(std::uint64_t until_ms, std::uint32_t locks) noexcept;
    [[nodiscard]] static GateState probation(std::uint32_t locks) noexcept;
    [[nodiscard]] const GateOpen* as_open() const noexcept;
    [[nodiscard]] const GateLocked* as_locked() const noexcept;
    [[nodiscard]] const GateProbation* as_probation() const noexcept;

private:
    using Alternatives = std::variant<GateOpen, GateLocked, GateProbation>;
    explicit GateState(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// The lock's length for the given lock number (1 for the first).
[[nodiscard]] std::uint64_t gate_lock_ms(std::uint32_t locks) noexcept;

// One gate's transitions, pure; GateTable keeps one per client.
[[nodiscard]] GateState gate_after_time(const GateState& state, std::uint64_t now_ms) noexcept;
[[nodiscard]] GateState gate_after_failure(const GateState& state, std::uint64_t now_ms) noexcept;

enum class GateVerdict : std::uint8_t {
    // A password may be checked.
    allowed,
    // This client is locked out; retry_after_ms says for how long.
    locked,
    // Every gate is held by a locked client: no new client can be tracked,
    // so none may try (this bounds the guesses all clients together make).
    busy,
};

struct GateDecision {
    GateVerdict verdict;
    std::uint64_t retry_after_ms;
};

class GateTable {
public:
    GateTable() noexcept;
    GateTable(const GateTable&) = delete;
    GateTable& operator=(const GateTable&) = delete;

    // May this client try a password now?
    [[nodiscard]] GateDecision check(const ClientKey& client, std::uint64_t now_ms) noexcept;
    // The password (or setup code) was wrong. Call only after check
    // allowed it, in the same request. Returns the decision for the next
    // try (locked when this failure locked the client).
    GateDecision record_failure(const ClientKey& client, std::uint64_t now_ms) noexcept;
    // It was right: the client's gate is forgotten.
    void record_success(const ClientKey& client) noexcept;

    // For tests: the client's state, when tracked.
    [[nodiscard]] std::optional<GateState> state_of(const ClientKey& client) const noexcept;
    [[nodiscard]] std::size_t tracked_count() const noexcept;

private:
    struct Entry {
        ClientKey client;
        GateState state;
        std::uint64_t last_failure_ms;
    };

    [[nodiscard]] std::optional<std::size_t> find(const ClientKey& client) const noexcept;
    // A slot for a new client: a free one, or the least recently failed
    // entry that is not locked. Nothing when every entry is locked.
    [[nodiscard]] std::optional<std::size_t> room(std::uint64_t now_ms) const noexcept;

    std::optional<Entry> entries_[gate_clients_max];
};

// ---------------------------------------------------------------------------
// The stored password: one line in the service's state folder,
//
//   argon2id$<memory_kib>$<iterations>$<lanes>$<salt>$<hash>\n
//   scrypt$<log2_n>$<r>$<p>$<salt>$<hash>\n
//
// salt 16 bytes and hash 32 bytes, both lower-case hex; numbers decimal
// without leading zeros. The algorithm and its costs are stored, so the
// costs can be raised for new passwords without breaking old records.

constexpr std::size_t password_salt_bytes = 16;
constexpr std::size_t password_hash_bytes = 32;
constexpr std::size_t password_record_text_max = 160;
constexpr std::size_t password_bytes_min = 8;
constexpr std::size_t password_bytes_max = 256;

// RFC 9106 Argon2id. Bounds: memory 8 KiB per lane .. 256 MiB, 1..16
// passes, 1..4 lanes.
struct Argon2idCost {
    std::uint32_t memory_kib;
    std::uint32_t iterations;
    std::uint32_t lanes;
};
// RFC 7914 scrypt. Bounds: N = 2^log2_n with log2_n 10..20, r 1..16, p
// 1..16, and 128 * r * N at most 256 MiB.
struct ScryptCost {
    std::uint32_t log2_n;
    std::uint32_t r;
    std::uint32_t p;
};

// New passwords: Argon2id with OWASP's first configuration (19 MiB, 2
// passes, 1 lane); scrypt with OWASP's N=2^15, r=8, p=3 when the Argon2
// implementation is not available (C2-web.md, section 5).
constexpr Argon2idCost argon2id_default_cost{19456u, 2u, 1u};
constexpr ScryptCost scrypt_default_cost{15u, 8u, 3u};

[[nodiscard]] bool argon2id_cost_valid(const Argon2idCost& cost) noexcept;
[[nodiscard]] bool scrypt_cost_valid(const ScryptCost& cost) noexcept;

class PasswordRecord {
public:
    [[nodiscard]] static PasswordRecord argon2id(const Argon2idCost& cost,
                                                 const std::array<std::uint8_t, password_salt_bytes>& salt,
                                                 const std::array<std::uint8_t, password_hash_bytes>& hash) noexcept;
    [[nodiscard]] static PasswordRecord scrypt(const ScryptCost& cost,
                                               const std::array<std::uint8_t, password_salt_bytes>& salt,
                                               const std::array<std::uint8_t, password_hash_bytes>& hash) noexcept;

    [[nodiscard]] const Argon2idCost* as_argon2id() const noexcept;
    [[nodiscard]] const ScryptCost* as_scrypt() const noexcept;
    [[nodiscard]] const std::array<std::uint8_t, password_salt_bytes>& salt() const noexcept;
    [[nodiscard]] const std::array<std::uint8_t, password_hash_bytes>& hash() const noexcept;

private:
    using Cost = std::variant<Argon2idCost, ScryptCost>;
    PasswordRecord(const Cost& cost, const std::array<std::uint8_t, password_salt_bytes>& salt,
                   const std::array<std::uint8_t, password_hash_bytes>& hash) noexcept;
    Cost cost_;
    std::array<std::uint8_t, password_salt_bytes> salt_;
    std::array<std::uint8_t, password_hash_bytes> hash_;
};

// The record a file's text holds (exactly one line, its LF included), or
// nothing when the text is not one valid record.
[[nodiscard]] std::optional<PasswordRecord> parse_password_record(std::span<const char> text) noexcept;
// The record's line into out; its size, or nothing when it does not fit.
[[nodiscard]] std::optional<std::size_t> format_password_record(const PasswordRecord& record,
                                                                std::span<char> out) noexcept;

enum class PasswordRule : std::uint8_t {
    acceptable,
    too_short,
    too_long,
    contains_nul,
};
[[nodiscard]] PasswordRule check_new_password(std::span<const char> password) noexcept;
[[nodiscard]] const char* password_rule_name(PasswordRule rule) noexcept;

// ---------------------------------------------------------------------------
// The setup code: printed on the console while no password is set; the
// first login sets the password with it. 10 characters of Crockford's
// base32 (50 bits), shown as XXXXX-XXXXX.

constexpr std::size_t setup_code_characters = 10;
constexpr std::size_t setup_code_text_bytes = setup_code_characters + 1u;

struct SetupCode {
    char characters[setup_code_characters];
};

// From 10 random bytes (each taken mod 32: 256 is a multiple of 32, so
// each character is uniform).
[[nodiscard]] SetupCode setup_code_of(std::span<const std::uint8_t, setup_code_characters> random) noexcept;
// "XXXXX-XXXXX" into out (11 bytes, no NUL).
void format_setup_code(const SetupCode& code, std::span<char, setup_code_text_bytes> out) noexcept;
// What a person typed: blanks and hyphens dropped, case ignored, O read as
// 0 and I and L as 1 (Crockford's decoding). Nothing when it is not 10
// characters of the alphabet.
[[nodiscard]] std::optional<SetupCode> read_setup_code(std::span<const char> typed) noexcept;
[[nodiscard]] bool setup_codes_equal(const SetupCode& left, const SetupCode& right) noexcept;

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_AUTH_H
