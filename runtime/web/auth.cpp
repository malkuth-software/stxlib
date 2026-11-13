// auth.cpp: sessions, lockout gates, the password record and the setup
// code (auth.h).

#include "auth.h"

#include <cassert>

namespace stxweb {

// ---------------------------------------------------------------------------
// Comparison and hex.

bool constant_time_equal(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    // volatile keeps the compiler from ending the loop at the first
    // difference; every byte is compared.
    volatile unsigned int difference = 0;
    for (std::size_t index = 0; index < left.size(); index += 1) {
        difference = difference | static_cast<unsigned int>(left[index] ^ right[index]);
    }
    return difference == 0u;
}

bool constant_time_equal_text(std::span<const char> left, std::span<const char> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    volatile unsigned int difference = 0;
    for (std::size_t index = 0; index < left.size(); index += 1) {
        const unsigned int left_byte = static_cast<unsigned char>(left[index]);
        const unsigned int right_byte = static_cast<unsigned char>(right[index]);
        difference = difference | (left_byte ^ right_byte);
    }
    return difference == 0u;
}

void hex_of(std::span<const std::uint8_t> bytes, std::span<char> out) noexcept {
    static const char digits[] = "0123456789abcdef";
    assert(out.size() >= 2u * bytes.size());
    for (std::size_t index = 0; index < bytes.size(); index += 1) {
        out[2u * index] = digits[(bytes[index] >> 4u) & 0xfu];
        out[2u * index + 1u] = digits[bytes[index] & 0xfu];
    }
}

// ---------------------------------------------------------------------------
// Sessions.

bool session_expired(const Session& session, std::uint64_t now_ms) noexcept {
    const std::uint64_t idle = now_ms >= session.last_used_ms ? now_ms - session.last_used_ms : 0u;
    const std::uint64_t age = now_ms >= session.created_ms ? now_ms - session.created_ms : 0u;
    return idle >= session_idle_ms || age >= session_absolute_ms;
}

SessionTable::SessionTable() noexcept : slots_{} {}

void SessionTable::expire(std::uint64_t now_ms) noexcept {
    for (std::size_t index = 0; index < sessions_max; index += 1) {
        if (slots_[index].has_value() && session_expired(*slots_[index], now_ms)) {
            slots_[index].reset();
        }
    }
}

std::optional<std::size_t> SessionTable::create(std::uint64_t now_ms, RandomSource& random) noexcept {
    std::uint8_t fresh[2u * token_random_bytes];
    if (!random.fill(std::span<std::uint8_t>(fresh, sizeof(fresh)))) {
        return std::nullopt;
    }
    expire(now_ms);
    std::optional<std::size_t> slot;
    for (std::size_t index = 0; index < sessions_max && !slot.has_value(); index += 1) {
        if (!slots_[index].has_value()) {
            slot = index;
        }
    }
    if (!slot.has_value()) {
        // Full: the least recently used session ends.
        std::size_t oldest = 0;
        for (std::size_t index = 1; index < sessions_max; index += 1) {
            if ((*slots_[index]).last_used_ms < (*slots_[oldest]).last_used_ms) {
                oldest = index;
            }
        }
        slot = oldest;
    }
    Session session{};
    hex_of(std::span<const std::uint8_t>(fresh, token_random_bytes), session.token.bytes);
    hex_of(std::span<const std::uint8_t>(fresh + token_random_bytes, token_random_bytes), session.csrf.bytes);
    session.created_ms = now_ms;
    session.last_used_ms = now_ms;
    slots_[*slot] = session;
    return slot;
}

std::optional<std::size_t> SessionTable::find(std::span<const char> token, std::uint64_t now_ms) noexcept {
    expire(now_ms);
    if (token.size() != token_text_bytes) {
        return std::nullopt;
    }
    // Every live slot is compared, whatever matched before.
    std::optional<std::size_t> found;
    for (std::size_t index = 0; index < sessions_max; index += 1) {
        if (!slots_[index].has_value()) {
            continue;
        }
        const std::span<const char> candidate((*slots_[index]).token.bytes, token_text_bytes);
        if (constant_time_equal_text(token, candidate)) {
            found = index;
        }
    }
    if (found.has_value()) {
        (*slots_[*found]).last_used_ms = now_ms;
    }
    return found;
}

bool SessionTable::csrf_matches(std::size_t index, std::span<const char> header) const noexcept {
    if (!is_live(index)) {
        return false;
    }
    const std::span<const char> expected((*slots_[index]).csrf.bytes, token_text_bytes);
    return constant_time_equal_text(header, expected);
}

const Session& SessionTable::session(std::size_t index) const noexcept {
    assert(is_live(index));
    return *slots_[index];
}

bool SessionTable::is_live(std::size_t index) const noexcept {
    return index < sessions_max && slots_[index].has_value();
}

void SessionTable::remove(std::size_t index) noexcept {
    if (index < sessions_max) {
        slots_[index].reset();
    }
}

void SessionTable::remove_all_except(std::optional<std::size_t> keep) noexcept {
    for (std::size_t index = 0; index < sessions_max; index += 1) {
        if (!keep.has_value() || *keep != index) {
            slots_[index].reset();
        }
    }
}

std::size_t SessionTable::live_count() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < sessions_max; index += 1) {
        if (slots_[index].has_value()) {
            count += 1;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Client keys and gates.

ClientKey client_key_ipv4(std::span<const std::uint8_t, 4> address) noexcept {
    ClientKey key{};
    key.family = AddressFamily::ipv4;
    for (std::size_t index = 0; index < 16u; index += 1) {
        key.bytes[index] = index < 4u ? address[index] : 0u;
    }
    return key;
}

ClientKey client_key_ipv6(std::span<const std::uint8_t, 16> address) noexcept {
    ClientKey key{};
    key.family = AddressFamily::ipv6;
    // One client per /64: a host chooses freely within its prefix.
    for (std::size_t index = 0; index < 16u; index += 1) {
        key.bytes[index] = index < 8u ? address[index] : 0u;
    }
    return key;
}

bool client_keys_equal(const ClientKey& left, const ClientKey& right) noexcept {
    if (left.family != right.family) {
        return false;
    }
    for (std::size_t index = 0; index < 16u; index += 1) {
        if (left.bytes[index] != right.bytes[index]) {
            return false;
        }
    }
    return true;
}

GateState::GateState(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

GateState GateState::open(std::uint32_t failures) noexcept {
    GateOpen state{};
    state.failures = failures;
    const GateState gate(Alternatives(std::in_place_type<GateOpen>, state));
    return gate;
}

GateState GateState::locked(std::uint64_t until_ms, std::uint32_t locks) noexcept {
    GateLocked state{};
    state.until_ms = until_ms;
    state.locks = locks;
    const GateState gate(Alternatives(std::in_place_type<GateLocked>, state));
    return gate;
}

GateState GateState::probation(std::uint32_t locks) noexcept {
    GateProbation state{};
    state.locks = locks;
    const GateState gate(Alternatives(std::in_place_type<GateProbation>, state));
    return gate;
}

const GateOpen* GateState::as_open() const noexcept {
    return std::get_if<GateOpen>(&alternatives_);
}

const GateLocked* GateState::as_locked() const noexcept {
    return std::get_if<GateLocked>(&alternatives_);
}

const GateProbation* GateState::as_probation() const noexcept {
    return std::get_if<GateProbation>(&alternatives_);
}

namespace {

// Lock numbers saturate here; gate_lock_ms is at its cap long before.
constexpr std::uint32_t gate_locks_counted_max = 64;

}  // namespace

std::uint64_t gate_lock_ms(std::uint32_t locks) noexcept {
    std::uint64_t length = gate_lock_first_ms;
    // At most until the cap: no shift can overflow.
    for (std::uint32_t lock = 1; lock < locks; lock += 1) {
        length *= 2u;
        if (length >= gate_lock_max_ms) {
            return gate_lock_max_ms;
        }
    }
    return length;
}

GateState gate_after_time(const GateState& state, std::uint64_t now_ms) noexcept {
    const GateLocked* const locked = state.as_locked();
    if (locked != nullptr && now_ms >= (*locked).until_ms) {
        return GateState::probation((*locked).locks);
    }
    return state;
}

GateState gate_after_failure(const GateState& state, std::uint64_t now_ms) noexcept {
    const GateState current = gate_after_time(state, now_ms);
    const GateOpen* const open = current.as_open();
    if (open != nullptr) {
        const std::uint32_t failures = (*open).failures + 1u;
        if (failures >= gate_failures_before_lock) {
            return GateState::locked(now_ms + gate_lock_ms(1), 1);
        }
        return GateState::open(failures);
    }
    const GateProbation* const probation = current.as_probation();
    if (probation != nullptr) {
        const std::uint32_t locks =
            (*probation).locks < gate_locks_counted_max ? (*probation).locks + 1u : gate_locks_counted_max;
        return GateState::locked(now_ms + gate_lock_ms(locks), locks);
    }
    // Locked: no password was checked, so there is no failure to count; the
    // lock stands as it is.
    return current;
}

GateTable::GateTable() noexcept : entries_{} {}

std::optional<std::size_t> GateTable::find(const ClientKey& client) const noexcept {
    for (std::size_t index = 0; index < gate_clients_max; index += 1) {
        if (entries_[index].has_value() && client_keys_equal((*entries_[index]).client, client)) {
            const std::optional<std::size_t> found = index;
            return found;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> GateTable::room(std::uint64_t now_ms) const noexcept {
    for (std::size_t index = 0; index < gate_clients_max; index += 1) {
        if (!entries_[index].has_value()) {
            const std::optional<std::size_t> free = index;
            return free;
        }
    }
    std::optional<std::size_t> oldest;
    for (std::size_t index = 0; index < gate_clients_max; index += 1) {
        const Entry& entry = *entries_[index];
        if (gate_after_time(entry.state, now_ms).as_locked() != nullptr) {
            continue;
        }
        if (!oldest.has_value() || entry.last_failure_ms < (*entries_[*oldest]).last_failure_ms) {
            oldest = index;
        }
    }
    return oldest;
}

GateDecision GateTable::check(const ClientKey& client, std::uint64_t now_ms) noexcept {
    GateDecision decision{};
    decision.verdict = GateVerdict::allowed;
    decision.retry_after_ms = 0;
    const std::optional<std::size_t> index = find(client);
    if (index.has_value()) {
        Entry& entry = *entries_[*index];
        entry.state = gate_after_time(entry.state, now_ms);
        const GateLocked* const locked = entry.state.as_locked();
        if (locked != nullptr) {
            decision.verdict = GateVerdict::locked;
            decision.retry_after_ms = (*locked).until_ms - now_ms;
        }
        return decision;
    }
    if (room(now_ms).has_value()) {
        return decision;
    }
    // Every entry is locked: the earliest unlock is when to retry.
    decision.verdict = GateVerdict::busy;
    std::uint64_t earliest = gate_lock_max_ms;
    for (std::size_t index_all = 0; index_all < gate_clients_max; index_all += 1) {
        const GateLocked* const locked = (*entries_[index_all]).state.as_locked();
        if (locked != nullptr && (*locked).until_ms > now_ms && (*locked).until_ms - now_ms < earliest) {
            earliest = (*locked).until_ms - now_ms;
        }
    }
    decision.retry_after_ms = earliest;
    return decision;
}

GateDecision GateTable::record_failure(const ClientKey& client, std::uint64_t now_ms) noexcept {
    std::optional<std::size_t> index = find(client);
    if (!index.has_value()) {
        index = room(now_ms);
        if (!index.has_value()) {
            // check said busy; nothing can be tracked.
            return check(client, now_ms);
        }
        Entry fresh{client, GateState::open(0), now_ms};
        entries_[*index] = fresh;
    }
    Entry& entry = *entries_[*index];
    entry.state = gate_after_failure(entry.state, now_ms);
    entry.last_failure_ms = now_ms;
    return check(client, now_ms);
}

void GateTable::record_success(const ClientKey& client) noexcept {
    const std::optional<std::size_t> index = find(client);
    if (index.has_value()) {
        entries_[*index].reset();
    }
}

std::optional<GateState> GateTable::state_of(const ClientKey& client) const noexcept {
    const std::optional<std::size_t> index = find(client);
    if (!index.has_value()) {
        return std::nullopt;
    }
    const std::optional<GateState> state = (*entries_[*index]).state;
    return state;
}

std::size_t GateTable::tracked_count() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < gate_clients_max; index += 1) {
        if (entries_[index].has_value()) {
            count += 1;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// The password record.

bool argon2id_cost_valid(const Argon2idCost& cost) noexcept {
    return cost.lanes >= 1u && cost.lanes <= 4u && cost.iterations >= 1u && cost.iterations <= 16u &&
           cost.memory_kib >= 8u * cost.lanes && cost.memory_kib <= 262144u;
}

bool scrypt_cost_valid(const ScryptCost& cost) noexcept {
    if (cost.log2_n < 10u || cost.log2_n > 20u || cost.r < 1u || cost.r > 16u || cost.p < 1u || cost.p > 16u) {
        return false;
    }
    // 128 * r * 2^log2_n <= 2^28 bytes, that is r * 2^log2_n <= 2^21.
    const std::uint64_t blocks = static_cast<std::uint64_t>(cost.r) << cost.log2_n;
    return blocks <= (std::uint64_t{1} << 21u);
}

PasswordRecord::PasswordRecord(const Cost& cost, const std::array<std::uint8_t, password_salt_bytes>& salt,
                               const std::array<std::uint8_t, password_hash_bytes>& hash) noexcept
    : cost_(cost), salt_(salt), hash_(hash) {}

PasswordRecord PasswordRecord::argon2id(const Argon2idCost& cost,
                                        const std::array<std::uint8_t, password_salt_bytes>& salt,
                                        const std::array<std::uint8_t, password_hash_bytes>& hash) noexcept {
    const PasswordRecord record(Cost(std::in_place_type<Argon2idCost>, cost), salt, hash);
    return record;
}

PasswordRecord PasswordRecord::scrypt(const ScryptCost& cost, const std::array<std::uint8_t, password_salt_bytes>& salt,
                                      const std::array<std::uint8_t, password_hash_bytes>& hash) noexcept {
    const PasswordRecord record(Cost(std::in_place_type<ScryptCost>, cost), salt, hash);
    return record;
}

const Argon2idCost* PasswordRecord::as_argon2id() const noexcept {
    return std::get_if<Argon2idCost>(&cost_);
}

const ScryptCost* PasswordRecord::as_scrypt() const noexcept {
    return std::get_if<ScryptCost>(&cost_);
}

const std::array<std::uint8_t, password_salt_bytes>& PasswordRecord::salt() const noexcept {
    return salt_;
}

const std::array<std::uint8_t, password_hash_bytes>& PasswordRecord::hash() const noexcept {
    return hash_;
}

namespace {

constexpr std::size_t record_fields = 6;
constexpr std::size_t record_number_digits_max = 7;

// A decimal field: 1..7 digits, no leading zero.
[[nodiscard]] std::optional<std::uint32_t> read_number(std::span<const char> field) noexcept {
    if (field.empty() || field.size() > record_number_digits_max || field[0] == '0') {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char byte : field) {
        if (byte < '0' || byte > '9') {
            return std::nullopt;
        }
        value = value * 10u + static_cast<std::uint32_t>(byte - '0');
    }
    const std::optional<std::uint32_t> found = value;
    return found;
}

// Lower-case hex of exactly out.size() bytes.
[[nodiscard]] bool read_hex(std::span<const char> field, std::span<std::uint8_t> out) noexcept {
    if (field.size() != 2u * out.size()) {
        return false;
    }
    for (std::size_t index = 0; index < field.size(); index += 1) {
        const char byte = field[index];
        unsigned int digit = 0;
        if (byte >= '0' && byte <= '9') {
            digit = static_cast<unsigned int>(byte - '0');
        } else if (byte >= 'a' && byte <= 'f') {
            digit = static_cast<unsigned int>(byte - 'a') + 10u;
        } else {
            return false;
        }
        if (index % 2u == 0u) {
            out[index / 2u] = static_cast<std::uint8_t>(digit << 4u);
        } else {
            out[index / 2u] = static_cast<std::uint8_t>(out[index / 2u] | digit);
        }
    }
    return true;
}

// Appends to a fixed buffer; marks it full on overflow.
class LineWriter {
public:
    explicit LineWriter(std::span<char> out) noexcept : out_(out), size_(0), full_(false) {}

    void put(char byte) noexcept {
        if (full_ || size_ >= out_.size()) {
            full_ = true;
            return;
        }
        out_[size_] = byte;
        size_ += 1;
    }

    void text(const char* value) noexcept {
        for (std::size_t index = 0; value[index] != '\0'; index += 1) {
            put(value[index]);
        }
    }

    void decimal(std::uint32_t value) noexcept {
        char digits[10];
        std::size_t count = 0;
        std::uint32_t remaining = value;
        do {
            digits[count] = static_cast<char>('0' + static_cast<int>(remaining % 10u));
            remaining /= 10u;
            count += 1;
        } while (remaining != 0u);
        while (count > 0u) {
            count -= 1;
            put(digits[count]);
        }
    }

    void hex(std::span<const std::uint8_t> bytes) noexcept {
        char pair[2];
        for (const std::uint8_t byte : bytes) {
            hex_of(std::span<const std::uint8_t>(&byte, 1), pair);
            put(pair[0]);
            put(pair[1]);
        }
    }

    [[nodiscard]] std::optional<std::size_t> result() const noexcept {
        if (full_) {
            return std::nullopt;
        }
        const std::optional<std::size_t> size = size_;
        return size;
    }

private:
    std::span<char> out_;
    std::size_t size_;
    bool full_;
};

}  // namespace

std::optional<PasswordRecord> parse_password_record(std::span<const char> text) noexcept {
    if (text.empty() || text.size() > password_record_text_max || text[text.size() - 1u] != '\n') {
        return std::nullopt;
    }
    const std::span<const char> line = text.subspan(0, text.size() - 1u);
    std::span<const char> fields[record_fields];
    std::size_t field_count = 0;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= line.size(); index += 1) {
        if (index == line.size() || line[index] == '$') {
            if (field_count >= record_fields) {
                return std::nullopt;
            }
            fields[field_count] = line.subspan(begin, index - begin);
            field_count += 1;
            begin = index + 1u;
        }
    }
    if (field_count != record_fields) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> first = read_number(fields[1]);
    const std::optional<std::uint32_t> second = read_number(fields[2]);
    const std::optional<std::uint32_t> third = read_number(fields[3]);
    std::array<std::uint8_t, password_salt_bytes> salt{};
    std::array<std::uint8_t, password_hash_bytes> hash{};
    if (!first.has_value() || !second.has_value() || !third.has_value() ||
        !read_hex(fields[4], std::span<std::uint8_t>(salt.data(), salt.size())) ||
        !read_hex(fields[5], std::span<std::uint8_t>(hash.data(), hash.size()))) {
        return std::nullopt;
    }
    std::span<const char> name = fields[0];
    const char argon2id_name[] = "argon2id";
    const char scrypt_name[] = "scrypt";
    bool is_argon2id = name.size() == sizeof(argon2id_name) - 1u;
    for (std::size_t index = 0; is_argon2id && index < name.size(); index += 1) {
        is_argon2id = name[index] == argon2id_name[index];
    }
    bool is_scrypt = name.size() == sizeof(scrypt_name) - 1u;
    for (std::size_t index = 0; is_scrypt && index < name.size(); index += 1) {
        is_scrypt = name[index] == scrypt_name[index];
    }
    if (is_argon2id) {
        const Argon2idCost cost{*first, *second, *third};
        if (!argon2id_cost_valid(cost)) {
            return std::nullopt;
        }
        const std::optional<PasswordRecord> record = PasswordRecord::argon2id(cost, salt, hash);
        return record;
    }
    if (is_scrypt) {
        const ScryptCost cost{*first, *second, *third};
        if (!scrypt_cost_valid(cost)) {
            return std::nullopt;
        }
        const std::optional<PasswordRecord> record = PasswordRecord::scrypt(cost, salt, hash);
        return record;
    }
    return std::nullopt;
}

std::optional<std::size_t> format_password_record(const PasswordRecord& record, std::span<char> out) noexcept {
    LineWriter line(out);
    const Argon2idCost* const argon2id = record.as_argon2id();
    const ScryptCost* const scrypt = record.as_scrypt();
    if (argon2id != nullptr) {
        line.text("argon2id$");
        line.decimal((*argon2id).memory_kib);
        line.put('$');
        line.decimal((*argon2id).iterations);
        line.put('$');
        line.decimal((*argon2id).lanes);
    } else if (scrypt != nullptr) {
        line.text("scrypt$");
        line.decimal((*scrypt).log2_n);
        line.put('$');
        line.decimal((*scrypt).r);
        line.put('$');
        line.decimal((*scrypt).p);
    } else {
        // Fallback: a record in no known state is not written.
        return std::nullopt;
    }
    line.put('$');
    line.hex(std::span<const std::uint8_t>(record.salt().data(), record.salt().size()));
    line.put('$');
    line.hex(std::span<const std::uint8_t>(record.hash().data(), record.hash().size()));
    line.put('\n');
    return line.result();
}

PasswordRule check_new_password(std::span<const char> password) noexcept {
    if (password.size() < password_bytes_min) {
        return PasswordRule::too_short;
    }
    if (password.size() > password_bytes_max) {
        return PasswordRule::too_long;
    }
    for (const char byte : password) {
        if (byte == '\0') {
            return PasswordRule::contains_nul;
        }
    }
    return PasswordRule::acceptable;
}

const char* password_rule_name(PasswordRule rule) noexcept {
    switch (rule) {
    case PasswordRule::acceptable: return "acceptable";
    case PasswordRule::too_short: return "password_too_short";
    case PasswordRule::too_long: return "password_too_long";
    case PasswordRule::contains_nul: return "password_contains_nul";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

// ---------------------------------------------------------------------------
// The setup code.

namespace {

// Crockford's base32: digits and letters without I, L, O, U.
constexpr char setup_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

}  // namespace

SetupCode setup_code_of(std::span<const std::uint8_t, setup_code_characters> random) noexcept {
    SetupCode code{};
    for (std::size_t index = 0; index < setup_code_characters; index += 1) {
        code.characters[index] = setup_alphabet[random[index] % 32u];
    }
    return code;
}

void format_setup_code(const SetupCode& code, std::span<char, setup_code_text_bytes> out) noexcept {
    std::size_t position = 0;
    for (std::size_t index = 0; index < setup_code_characters; index += 1) {
        if (index == setup_code_characters / 2u) {
            out[position] = '-';
            position += 1;
        }
        out[position] = code.characters[index];
        position += 1;
    }
}

std::optional<SetupCode> read_setup_code(std::span<const char> typed) noexcept {
    SetupCode code{};
    std::size_t count = 0;
    for (const char byte : typed) {
        if (byte == ' ' || byte == '-') {
            continue;
        }
        char upper = byte >= 'a' && byte <= 'z' ? static_cast<char>(byte - 'a' + 'A') : byte;
        if (upper == 'O') {
            upper = '0';
        } else if (upper == 'I' || upper == 'L') {
            upper = '1';
        }
        bool known = false;
        for (std::size_t index = 0; index < 32u && !known; index += 1) {
            known = setup_alphabet[index] == upper;
        }
        if (!known || count >= setup_code_characters) {
            return std::nullopt;
        }
        code.characters[count] = upper;
        count += 1;
    }
    if (count != setup_code_characters) {
        return std::nullopt;
    }
    const std::optional<SetupCode> found = code;
    return found;
}

bool setup_codes_equal(const SetupCode& left, const SetupCode& right) noexcept {
    const std::span<const char> left_text(left.characters, setup_code_characters);
    const std::span<const char> right_text(right.characters, setup_code_characters);
    return constant_time_equal_text(left_text, right_text);
}

}  // namespace stxweb
