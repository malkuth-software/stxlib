// crypto_system.cpp: random bytes, the password store and the TLS identity
// on base OpenSSL (crypto_system.h).

#include "crypto_system.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "../store/turn_file_system.h"

namespace stxweb {

namespace {

// The TLS identity's turn body: a PEM key (about 250 bytes for P-256) and
// a PEM certificate (about 700); 4 KiB leaves room.
constexpr std::size_t tls_body_bytes_max = 4096;
// scrypt's working memory is 128 * r * N bytes (32 MiB at the default
// costs), above OpenSSL's default maxmem of 32 MiB less its margin; the
// largest valid cost (auth.h) needs 256 MiB.
constexpr std::uint64_t scrypt_maxmem_bytes = 256u * 1024u * 1024u + 1024u * 1024u;
constexpr const char* certificate_name = "stx-appliance";
// Generalized time (RFC 5280 section 4.1.2.5: years from 2050 use it).
constexpr const char* certificate_not_before = "20260101000000Z";
constexpr const char* certificate_not_after = "99991231235959Z";

[[nodiscard]] bool append(char* out, std::size_t capacity, std::size_t& size, const char* text) noexcept {
    for (std::size_t index = 0; text[index] != '\0'; index += 1) {
        if (size + 1u >= capacity) {
            return false;
        }
        out[size] = text[index];
        size += 1;
    }
    out[size] = '\0';
    return true;
}

[[nodiscard]] bool join_path(char* out, std::size_t capacity, const char* directory, const char* name) noexcept {
    std::size_t size = 0;
    out[0] = '\0';
    return append(out, capacity, size, directory) && append(out, capacity, size, "/") &&
           append(out, capacity, size, name);
}

[[nodiscard]] stxstore::TurnPaths password_paths(const StatePaths& paths) noexcept {
    stxstore::TurnPaths turn{};
    turn.directory = paths.directory;
    turn.files[0] = paths.password_files[0];
    turn.files[1] = paths.password_files[1];
    return turn;
}

[[nodiscard]] stxstore::TurnPaths tls_paths(const StatePaths& paths) noexcept {
    stxstore::TurnPaths turn{};
    turn.directory = paths.directory;
    turn.files[0] = paths.tls_files[0];
    turn.files[1] = paths.tls_files[1];
    return turn;
}

// Owners that free OpenSSL objects on every path.
class KdfOwner {
public:
    explicit KdfOwner(EVP_KDF* kdf) noexcept : kdf_(kdf) {}
    KdfOwner(const KdfOwner&) = delete;
    KdfOwner& operator=(const KdfOwner&) = delete;
    ~KdfOwner() {
        EVP_KDF_free(kdf_);
    }
    [[nodiscard]] EVP_KDF* get() const noexcept {
        return kdf_;
    }

private:
    EVP_KDF* kdf_;
};

class KdfContextOwner {
public:
    explicit KdfContextOwner(EVP_KDF_CTX* context) noexcept : context_(context) {}
    KdfContextOwner(const KdfContextOwner&) = delete;
    KdfContextOwner& operator=(const KdfContextOwner&) = delete;
    ~KdfContextOwner() {
        EVP_KDF_CTX_free(context_);
    }
    [[nodiscard]] EVP_KDF_CTX* get() const noexcept {
        return context_;
    }

private:
    EVP_KDF_CTX* context_;
};

class BioOwner {
public:
    explicit BioOwner(BIO* bio) noexcept : bio_(bio) {}
    BioOwner(const BioOwner&) = delete;
    BioOwner& operator=(const BioOwner&) = delete;
    ~BioOwner() {
        BIO_free(bio_);
    }
    [[nodiscard]] BIO* get() const noexcept {
        return bio_;
    }

private:
    BIO* bio_;
};

// A copy of a secret that is wiped when it goes out of scope.
class SecretCopy {
public:
    explicit SecretCopy(std::span<const char> secret) noexcept : bytes_{}, size_(0) {
        size_ = secret.size() <= sizeof(bytes_) ? secret.size() : 0u;
        for (std::size_t index = 0; index < size_; index += 1) {
            bytes_[index] = static_cast<unsigned char>(secret[index]);
        }
    }
    SecretCopy(const SecretCopy&) = delete;
    SecretCopy& operator=(const SecretCopy&) = delete;
    ~SecretCopy() {
        OPENSSL_cleanse(bytes_, sizeof(bytes_));
    }
    [[nodiscard]] unsigned char* data() noexcept {
        return bytes_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    unsigned char bytes_[password_bytes_max];
    std::size_t size_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Random bytes.

bool OpenSslRandom::fill(std::span<std::uint8_t> bytes) noexcept {
    if (bytes.size() > 0x7fffffffu) {
        return false;
    }
    return RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) == 1;
}

// ---------------------------------------------------------------------------
// Key derivation.

bool argon2id_available() noexcept {
    const KdfOwner kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr));
    ERR_clear_error();
    return kdf.get() != nullptr;
}

std::optional<std::array<std::uint8_t, password_hash_bytes>> derive_password_hash(
    const PasswordRecord& record_shape, std::span<const char> password) noexcept {
    if (password.size() > password_bytes_max) {
        return std::nullopt;
    }
    const Argon2idCost* const argon2id = record_shape.as_argon2id();
    const ScryptCost* const scrypt = record_shape.as_scrypt();
    if ((argon2id != nullptr && !argon2id_cost_valid(*argon2id)) ||
        (scrypt != nullptr && !scrypt_cost_valid(*scrypt)) || (argon2id == nullptr && scrypt == nullptr)) {
        return std::nullopt;
    }
    const KdfOwner kdf(EVP_KDF_fetch(nullptr, argon2id != nullptr ? "ARGON2ID" : "SCRYPT", nullptr));
    if (kdf.get() == nullptr) {
        ERR_clear_error();
        return std::nullopt;
    }
    const KdfContextOwner context(EVP_KDF_CTX_new(kdf.get()));
    if (context.get() == nullptr) {
        ERR_clear_error();
        return std::nullopt;
    }
    SecretCopy secret(password);
    std::array<std::uint8_t, password_salt_bytes> salt = record_shape.salt();
    OSSL_PARAM params[8];
    std::size_t count = 0;
    params[count] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, secret.data(), secret.size());
    count += 1;
    params[count] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt.data(), salt.size());
    count += 1;
    std::uint32_t iterations = 0;
    std::uint32_t memory = 0;
    std::uint32_t lanes = 0;
    std::uint32_t threads = 1;
    std::uint64_t n = 0;
    std::uint32_t r = 0;
    std::uint32_t p = 0;
    std::uint64_t maxmem = scrypt_maxmem_bytes;
    if (argon2id != nullptr) {
        iterations = (*argon2id).iterations;
        memory = (*argon2id).memory_kib;
        lanes = (*argon2id).lanes;
        params[count] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations);
        count += 1;
        params[count] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory);
        count += 1;
        params[count] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes);
        count += 1;
        // One thread: more need OSSL_set_max_threads and a thread pool.
        params[count] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads);
        count += 1;
    } else {
        n = static_cast<std::uint64_t>(1u) << (*scrypt).log2_n;
        r = (*scrypt).r;
        p = (*scrypt).p;
        params[count] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_N, &n);
        count += 1;
        params[count] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_SCRYPT_R, &r);
        count += 1;
        params[count] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_SCRYPT_P, &p);
        count += 1;
        params[count] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_MAXMEM, &maxmem);
        count += 1;
    }
    params[count] = OSSL_PARAM_construct_end();
    std::array<std::uint8_t, password_hash_bytes> hash{};
    if (EVP_KDF_derive(context.get(), hash.data(), hash.size(), params) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    const std::optional<std::array<std::uint8_t, password_hash_bytes>> derived = hash;
    return derived;
}

// ---------------------------------------------------------------------------
// State paths and the password store.

bool make_state_paths(const char* directory, StatePaths& paths) noexcept {
    std::size_t size = 0;
    paths.directory[0] = '\0';
    return append(paths.directory, sizeof(paths.directory), size, directory) &&
           join_path(paths.password_files[0], sizeof(paths.password_files[0]), directory, "password.0") &&
           join_path(paths.password_files[1], sizeof(paths.password_files[1]), directory, "password.1") &&
           join_path(paths.tls_files[0], sizeof(paths.tls_files[0]), directory, "tls.0") &&
           join_path(paths.tls_files[1], sizeof(paths.tls_files[1]), directory, "tls.1");
}

PasswordStore::PasswordStore(const StatePaths& paths) noexcept : paths_(paths), record_(), buffers_{} {}

void PasswordStore::load() noexcept {
    record_.reset();
    const stxstore::TurnState state =
        stxstore::read_turns(password_paths(paths_), password_record_text_max,
                             std::span<char>(buffers_[0], sizeof(buffers_[0])),
                             std::span<char>(buffers_[1], sizeof(buffers_[1])));
    // The newest turn whose body is a valid record, then the older.
    const std::optional<std::size_t> order[2] = {state.choice.newest, state.choice.older};
    for (const std::optional<std::size_t>& index : order) {
        if (!index.has_value()) {
            continue;
        }
        const std::optional<PasswordRecord> record = parse_password_record((*state.turns[*index]).body);
        if (record.has_value()) {
            record_ = record;
            return;
        }
    }
}

bool PasswordStore::is_set() const noexcept {
    return record_.has_value();
}

PasswordCheck PasswordStore::verify(std::span<const char> password) noexcept {
    if (!record_.has_value()) {
        return PasswordCheck::unavailable;
    }
    const std::optional<std::array<std::uint8_t, password_hash_bytes>> hash = derive_password_hash(*record_, password);
    if (!hash.has_value()) {
        return PasswordCheck::unavailable;
    }
    const bool same = constant_time_equal(std::span<const std::uint8_t>((*hash).data(), (*hash).size()),
                                          std::span<const std::uint8_t>((*record_).hash().data(), password_hash_bytes));
    return same ? PasswordCheck::matches : PasswordCheck::wrong;
}

bool PasswordStore::set(std::span<const char> password, RandomSource& random) noexcept {
    std::array<std::uint8_t, password_salt_bytes> salt{};
    if (!random.fill(std::span<std::uint8_t>(salt.data(), salt.size()))) {
        return false;
    }
    const std::array<std::uint8_t, password_hash_bytes> no_hash{};
    // Argon2id when this OpenSSL has it; scrypt otherwise.
    const PasswordRecord shape = argon2id_available() ? PasswordRecord::argon2id(argon2id_default_cost, salt, no_hash)
                                                      : PasswordRecord::scrypt(scrypt_default_cost, salt, no_hash);
    const std::optional<std::array<std::uint8_t, password_hash_bytes>> hash = derive_password_hash(shape, password);
    if (!hash.has_value()) {
        return false;
    }
    const Argon2idCost* const argon2id = shape.as_argon2id();
    const PasswordRecord record = argon2id != nullptr ? PasswordRecord::argon2id(*argon2id, salt, *hash)
                                                      : PasswordRecord::scrypt(*shape.as_scrypt(), salt, *hash);
    char text[password_record_text_max];
    const std::optional<std::size_t> size = format_password_record(record, text);
    if (!size.has_value()) {
        return false;
    }
    const stxstore::TurnPaths turn_paths = password_paths(paths_);
    const stxstore::TurnState state =
        stxstore::read_turns(turn_paths, password_record_text_max, std::span<char>(buffers_[0], sizeof(buffers_[0])),
                             std::span<char>(buffers_[1], sizeof(buffers_[1])));
    const stxstore::TurnWriteOutcome outcome =
        stxstore::write_turn(turn_paths, state.choice, std::span<const char>(text, *size), 0600u);
    if (outcome.as_written() == nullptr) {
        return false;
    }
    record_ = record;
    return true;
}

// ---------------------------------------------------------------------------
// The TLS identity.

namespace {

[[nodiscard]] bool fingerprint_of(X509* certificate, char (&out)[96]) noexcept {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &length) != 1 || length != 32u) {
        return false;
    }
    const char digits[] = "0123456789ABCDEF";
    std::size_t size = 0;
    for (unsigned int index = 0; index < length; index += 1) {
        if (index > 0u) {
            out[size] = ':';
            size += 1;
        }
        out[size] = digits[digest[index] >> 4u];
        out[size + 1u] = digits[digest[index] & 0x0fu];
        size += 2;
    }
    out[size] = '\0';
    return true;
}

[[nodiscard]] bool add_extension(X509* certificate, int nid, const char* value) noexcept {
    X509V3_CTX context{};
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* const extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
    if (extension == nullptr) {
        return false;
    }
    const bool added = X509_add_ext(certificate, extension, -1) == 1;
    X509_EXTENSION_free(extension);
    return added;
}

// A new key and certificate into identity (owned there).
[[nodiscard]] bool make_identity(RandomSource& random, TlsIdentity& identity) noexcept {
    EVP_PKEY* const key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    if (key == nullptr) {
        return false;
    }
    X509* const certificate = X509_new();
    if (certificate == nullptr) {
        EVP_PKEY_free(key);
        return false;
    }
    identity.key = key;
    identity.certificate = certificate;
    // A positive 128-bit serial (RFC 5280 section 4.1.2.2: at most 20
    // octets, positive).
    std::uint8_t serial_bytes[16];
    if (!random.fill(serial_bytes)) {
        return false;
    }
    serial_bytes[0] = static_cast<std::uint8_t>((serial_bytes[0] & 0x7fu) | 0x01u);
    BIGNUM* const serial_number = BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr);
    if (serial_number == nullptr) {
        return false;
    }
    ASN1_INTEGER* const serial = BN_to_ASN1_INTEGER(serial_number, nullptr);
    BN_free(serial_number);
    if (serial == nullptr) {
        return false;
    }
    const bool serial_set = X509_set_serialNumber(certificate, serial) == 1;
    ASN1_INTEGER_free(serial);
    ASN1_TIME* const not_before = ASN1_TIME_new();
    ASN1_TIME* const not_after = ASN1_TIME_new();
    bool times_set = not_before != nullptr && not_after != nullptr &&
                     ASN1_TIME_set_string_X509(not_before, certificate_not_before) == 1 &&
                     ASN1_TIME_set_string_X509(not_after, certificate_not_after) == 1;
    times_set = times_set && X509_set1_notBefore(certificate, not_before) == 1 &&
                X509_set1_notAfter(certificate, not_after) == 1;
    ASN1_TIME_free(not_before);
    ASN1_TIME_free(not_after);
    X509_NAME* const name = X509_get_subject_name(certificate);
    const bool named =
        name != nullptr &&
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(certificate_name), -1, -1, 0) == 1 &&
        X509_set_issuer_name(certificate, name) == 1;
    if (!serial_set || !times_set || !named || X509_set_version(certificate, X509_VERSION_3) != 1 ||
        X509_set_pubkey(certificate, key) != 1 ||
        !add_extension(certificate, NID_basic_constraints, "critical,CA:FALSE") ||
        !add_extension(certificate, NID_key_usage, "critical,digitalSignature") ||
        !add_extension(certificate, NID_ext_key_usage, "serverAuth") ||
        !add_extension(certificate, NID_subject_alt_name, "DNS:stx-appliance") ||
        X509_sign(certificate, key, EVP_sha256()) <= 0) {
        return false;
    }
    return true;
}

// The identity's PEM text into out; its size.
[[nodiscard]] std::optional<std::size_t> identity_text(const TlsIdentity& identity, std::span<char> out) noexcept {
    const BioOwner bio(BIO_new(BIO_s_mem()));
    if (bio.get() == nullptr ||
        PEM_write_bio_PrivateKey(bio.get(), identity.key, nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
        PEM_write_bio_X509(bio.get(), identity.certificate) != 1) {
        return std::nullopt;
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (length <= 0 || data == nullptr || static_cast<std::size_t>(length) > out.size()) {
        return std::nullopt;
    }
    for (long index = 0; index < length; index += 1) {
        out[static_cast<std::size_t>(index)] = data[index];
    }
    const std::optional<std::size_t> size = static_cast<std::size_t>(length);
    return size;
}

// The identity in a turn's body; true when key and certificate read and
// belong together.
[[nodiscard]] bool read_identity(std::span<const char> body, TlsIdentity& identity) noexcept {
    if (body.size() > 0x7fffffffu) {
        return false;
    }
    const BioOwner bio(BIO_new_mem_buf(body.data(), static_cast<int>(body.size())));
    if (bio.get() == nullptr) {
        return false;
    }
    EVP_PKEY* const key = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
    if (key == nullptr) {
        return false;
    }
    X509* const certificate = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
    if (certificate == nullptr) {
        EVP_PKEY_free(key);
        return false;
    }
    if (X509_check_private_key(certificate, key) != 1) {
        X509_free(certificate);
        EVP_PKEY_free(key);
        return false;
    }
    identity.key = key;
    identity.certificate = certificate;
    return true;
}

// Turn buffers for the identity: static, as the service has one.
char identity_buffers[2][stxstore::turn_header_bytes_max + tls_body_bytes_max + 1u];
char identity_text_buffer[tls_body_bytes_max];

}  // namespace

void free_identity(TlsIdentity& identity) noexcept {
    EVP_PKEY_free(identity.key);
    X509_free(identity.certificate);
    identity.key = nullptr;
    identity.certificate = nullptr;
    identity.fingerprint[0] = '\0';
}

bool load_or_make_identity(const StatePaths& paths, RandomSource& random, TlsIdentity& identity) noexcept {
    identity.key = nullptr;
    identity.certificate = nullptr;
    identity.fingerprint[0] = '\0';
    identity.made_now = false;
    const stxstore::TurnPaths turn_paths = tls_paths(paths);
    const stxstore::TurnState state = stxstore::read_turns(
        turn_paths, tls_body_bytes_max, std::span<char>(identity_buffers[0], sizeof(identity_buffers[0])),
        std::span<char>(identity_buffers[1], sizeof(identity_buffers[1])));
    const std::optional<std::size_t> order[2] = {state.choice.newest, state.choice.older};
    for (const std::optional<std::size_t>& index : order) {
        if (!index.has_value()) {
            continue;
        }
        if (read_identity((*state.turns[*index]).body, identity) && fingerprint_of(identity.certificate, identity.fingerprint)) {
            ERR_clear_error();
            return true;
        }
        free_identity(identity);
        ERR_clear_error();
    }
    if (!make_identity(random, identity) || !fingerprint_of(identity.certificate, identity.fingerprint)) {
        free_identity(identity);
        ERR_clear_error();
        return false;
    }
    identity.made_now = true;
    // Written for the next start; a failed write still serves this one
    // (the next start makes another, with another fingerprint).
    const std::optional<std::size_t> size =
        identity_text(identity, std::span<char>(identity_text_buffer, sizeof(identity_text_buffer)));
    if (size.has_value()) {
        const stxstore::TurnWriteOutcome outcome =
            stxstore::write_turn(turn_paths, state.choice, std::span<const char>(identity_text_buffer, *size), 0600u);
        (void)outcome;
    }
    OPENSSL_cleanse(identity_text_buffer, sizeof(identity_text_buffer));
    ERR_clear_error();
    return true;
}

}  // namespace stxweb
