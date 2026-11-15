// crypto_system.h: the web service's uses of FreeBSD's base OpenSSL (C2;
// design dev/notes/planning/stages/C2-web.md, section 5): random bytes,
// the password's key derivation and its stored record, and the TLS
// identity (key and self-signed certificate).
//
// OpenSSL at the pin is 3.5 (secure/lib/libcrypto at freebsd-src
// fb9033d2876d). Its default provider has the Argon2id and scrypt KDFs
// (providers/implementations/kdfs/argon2.c and scrypt.c are in the
// libcrypto Makefile's sources), fetched by name through EVP_KDF.
//
// Files, in the state directory (default /user/stx/web), each a pair of
// turn files (store/turn_file.h), because the user partition is FAT32:
//   password.0, password.1   one password record line (auth.h)
//   tls.0, tls.1             the private key and the certificate, PEM
// Recovery from a forgotten password: delete both password files (card
// out, or over SSH); the next start shows a setup code on the console.

#ifndef STXLIB_RUNTIME_WEB_CRYPTO_SYSTEM_H
#define STXLIB_RUNTIME_WEB_CRYPTO_SYSTEM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../store/turn_file.h"
#include "api.h"
#include "auth.h"

// OpenSSL's types, declared rather than included, so that the headers of
// the web service do not carry OpenSSL's.
struct evp_pkey_st;
struct x509_st;

namespace stxweb {

// RAND_bytes.
class OpenSslRandom final : public RandomSource {
public:
    OpenSslRandom() noexcept = default;
    [[nodiscard]] bool fill(std::span<std::uint8_t> bytes) noexcept override;
};

// The key derivation for a record's algorithm and costs. Nothing when the
// algorithm is not available or derivation fails.
[[nodiscard]] std::optional<std::array<std::uint8_t, password_hash_bytes>> derive_password_hash(
    const PasswordRecord& record_shape, std::span<const char> password) noexcept;

// Whether OpenSSL here offers Argon2id (else new records use scrypt).
[[nodiscard]] bool argon2id_available() noexcept;

// Paths below a state directory, built once.
struct StatePaths {
    char directory[256];
    char password_files[2][272];
    char tls_files[2][272];
};
// False when directory is too long.
[[nodiscard]] bool make_state_paths(const char* directory, StatePaths& paths) noexcept;

class PasswordStore {
public:
    explicit PasswordStore(const StatePaths& paths) noexcept;
    PasswordStore(const PasswordStore&) = delete;
    PasswordStore& operator=(const PasswordStore&) = delete;

    // Reads the newest valid record.
    void load() noexcept;
    [[nodiscard]] bool is_set() const noexcept;
    [[nodiscard]] PasswordCheck verify(std::span<const char> password) noexcept;
    // A new record with a fresh salt, written as the next turn; the record
    // in memory changes only when that succeeded.
    [[nodiscard]] bool set(std::span<const char> password, RandomSource& random) noexcept;

private:
    const StatePaths& paths_;
    std::optional<PasswordRecord> record_;
    char buffers_[2][stxstore::turn_header_bytes_max + password_record_text_max + 1u];
};

// The key and certificate the server presents, and the certificate's
// SHA-256 fingerprint ("AB:CD:...", 95 characters and a NUL).
struct TlsIdentity {
    evp_pkey_st* key;
    x509_st* certificate;
    char fingerprint[96];
    // Made in this start (and written), rather than read.
    bool made_now;
};

// Reads the identity from its turn files, or makes one (ECDSA P-256; a
// self-signed X.509 v3 certificate for "stx-appliance", valid from
// 2026-01-01 to 9999-12-31, because the board has no clock to trust at
// first boot) and writes it. False when neither works; the identity is
// then empty.
[[nodiscard]] bool load_or_make_identity(const StatePaths& paths, RandomSource& random,
                                         TlsIdentity& identity) noexcept;
void free_identity(TlsIdentity& identity) noexcept;

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_CRYPTO_SYSTEM_H
