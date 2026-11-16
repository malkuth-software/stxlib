#pragma once

// Typed identities (house style, section 3: an identity is never reused; a
// counter that issues identities checks for exhaustion before it issues
// one and refuses at its maximum, never wraps).
//
// Identity<Tag> is a 64-bit value; Tag is any type naming what the identity
// identifies, so identities of different things do not convert into each
// other. Value 0 is never issued: Identity<Tag>() is "none".
//
// IdentityIssuer<Tag> issues 1, 2, 3 ... up to 2^64 - 1, then refuses with
// limit_exceeded. It is not synchronised: its owner issues from one thread
// or under its own lock.

#include "stxlib/error.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

template <typename Tag>
class Identity final {
public:
    // No identity.
    constexpr Identity() noexcept = default;

    [[nodiscard]] constexpr u64 value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool present() const noexcept { return value_ != 0; }

    [[nodiscard]] constexpr bool operator==(const Identity& other) const noexcept { return value_ == other.value_; }
    [[nodiscard]] constexpr bool operator!=(const Identity& other) const noexcept { return value_ != other.value_; }

    // An identity read back from storage or from a foreign caller: input.
    // The issuer that will accept it checks it (IdentityIssuer::resume).
    [[nodiscard]] static constexpr Identity from_value(const u64 stored_value) noexcept {
        Identity result;
        result.value_ = stored_value;
        return result;
    }

private:
    u64 value_{0};
};

template <typename Tag>
class IdentityIssuer final {
public:
    // An issuer that has issued nothing.
    constexpr IdentityIssuer() noexcept = default;

    // An issuer continuing after `last_issued`, a counter read from storage.
    // It is input: any value is accepted here, and an issuer resumed at the
    // maximum issues nothing more. Checking it against the identities
    // restored beside it (house style §3: no restored identity may exceed
    // it) is the caller's, who holds them (audit A, I1).
    [[nodiscard]] static constexpr IdentityIssuer resume(const u64 last_issued) noexcept {
        IdentityIssuer result;
        result.last_issued_ = last_issued;
        return result;
    }

    // The next identity, or limit_exceeded when 2^64 - 1 have been issued.
    // On failure the issuer is unchanged.
    [[nodiscard]] OperationResult<Identity<Tag>> issue() noexcept {
        if (last_issued_ == u64_max) {
            const ErrorRecord exhausted = ErrorRecord::make(
                ErrorKind::limit_exceeded, "stxlib.identity.issue",
                ErrorDetail{.message = "every identity of this kind has been issued"});
            return OperationResult<Identity<Tag>>::failure(exhausted);
        }
        last_issued_ = last_issued_ + 1;
        const Identity<Tag> issued = Identity<Tag>::from_value(last_issued_);
        return OperationResult<Identity<Tag>>::success(issued);
    }

    [[nodiscard]] constexpr u64 last_issued() const noexcept { return last_issued_; }

private:
    u64 last_issued_{0};
};

} // namespace stxlib
