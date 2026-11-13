// Tests of BLAKE2s (include/stxlib/hash.hpp). Ported from the BLAKE2s part
// of stx_auxlib's tests/precedence_tests.cpp at b6e330a with the same
// vectors: RFC 7693's "abc" and the BLAKE2 reference's keyed vectors, the
// rest from stx_auxlib's tests/reference/precedence_reference.py over
// Python's hashlib. Known answers, tested, not proved.

#include "stxlib/hash.hpp"

#include "test_support.hpp"

#include <string>

namespace {

namespace hash = stxlib::hash;
using stxlib::Byte;
using stxlib::usize;

std::string hex_of(const hash::Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result{};
    for (const Byte value : digest.bytes) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15U]);
    }
    return result;
}

hash::Digest digest_of(const std::string& text) {
    const stxlib::View<Byte> bytes(reinterpret_cast<const Byte*>(text.data()), text.size());
    const hash::Digest result = hash::blake2s(bytes);
    return result;
}

hash::Digest counting_digest(const unsigned first) {
    hash::Digest result{};
    for (usize index = 0; index < hash::digest_bytes; ++index) result.bytes[index] = static_cast<Byte>(first + index);
    return result;
}

void blake2s_vectors() {
    CHECK(hex_of(digest_of("")) == "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9");
    CHECK(hex_of(digest_of("abc")) == "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");
    CHECK(hex_of(digest_of(std::string(200, 'a'))) ==
          "2b033f9f5ba9cf20671da79e492f41545e673b562603945ffed09662fd92321a");
    const hash::Digest key = counting_digest(0);
    const stxlib::View<Byte> key_bytes(key.bytes);
    CHECK(hex_of(hash::blake2s_keyed(key_bytes, stxlib::View<Byte>())) ==
          "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49");
    Byte counting[256]{};
    for (usize index = 0; index < 256; ++index) counting[index] = static_cast<Byte>(index);
    const stxlib::View<Byte> all(counting);
    CHECK(hex_of(hash::blake2s_keyed(key_bytes, all)) ==
          "5211d1aefc0025be7f85c06b3e14e0fc645ae12bd41746485ea6d8a364a2eaee");
    // Feeding in pieces gives the same digest.
    hash::Blake2s pieces{};
    for (usize index = 0; index < 256; index += 7) {
        const usize length = index + 7 <= 256 ? 7 : 256 - index;
        pieces.update(all.from(index).first(length));
    }
    CHECK(pieces.finish() == hash::blake2s(all));
    // A key longer than 32 bytes is cut to 32.
    CHECK(hash::blake2s_keyed(all.first(40), all) == hash::blake2s_keyed(all.first(32), all));
}

void erase() {
    Byte secret[5] = {1, 2, 3, 4, 5};
    hash::secure_erase(stxlib::MutableView<Byte>(secret));
    bool zero = true;
    for (const Byte value : secret) zero = zero && value == 0;
    CHECK(zero);
}

} // namespace

int main() {
    blake2s_vectors();
    erase();
    return stxlib_tests::finish("stxlib hash");
}
