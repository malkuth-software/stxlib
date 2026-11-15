// BLAKE2s-256 (include/stxlib/hash.hpp), after RFC 7693 sections 2 and 3:
// ten rounds of the G function over a 16-word block, little-endian words,
// parameter block for a 32-byte digest, no salt or personalization. Ported
// from stx_auxlib's src/hash.cpp at b6e330a: the same computation over
// STXlib's types (C arrays in place of std::array, Byte in place of
// std::byte).

#include "stxlib/hash.hpp"

#include "core/checked.hpp"

#include <bit>
#include <cstring>

namespace stxlib::hash {

namespace {

constexpr u32 initial[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

constexpr u8 sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

u32 load_little(const Byte* const bytes) noexcept {
    const u32 result = static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 |
                       static_cast<u32>(bytes[2]) << 16 | static_cast<u32>(bytes[3]) << 24;
    return result;
}

// RFC 7693 section 3.1; BLAKE2s is defined modulo 2^32.
STXLIB_MODULAR void mix(u32 (&v)[16], const usize a, const usize b, const usize c, const usize d, const u32 x,
                        const u32 y) noexcept {
    v[a] = v[a] + v[b] + x;
    v[d] = std::rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];
    v[b] = std::rotr(v[b] ^ v[c], 12);
    v[a] = v[a] + v[b] + y;
    v[d] = std::rotr(v[d] ^ v[a], 8);
    v[c] = v[c] + v[d];
    v[b] = std::rotr(v[b] ^ v[c], 7);
}

} // namespace

// Every byte is compared, whatever differs first, so a keyed digest (a MAC)
// checked with == does not leak by its timing how many leading bytes of a
// forged tag are right (audit 2026-11-15, A-stxlib-algorithms-01-F3).
bool Digest::operator==(const Digest& other) const noexcept {
    u32 difference = 0;
    for (usize index = 0; index < digest_bytes; ++index) {
        difference |= static_cast<u32>(bytes[index] ^ other.bytes[index]);
    }
    const bool result = difference == 0;
    return result;
}

bool Digest::operator!=(const Digest& other) const noexcept {
    const bool equal = *this == other;
    return !equal;
}

Blake2s::Blake2s() noexcept { reset(); }

Blake2s::Blake2s(const View<Byte> key) noexcept {
    key_length_ = key.count() < maximum_key_bytes ? key.count() : maximum_key_bytes;
    if (key_length_ > 0) std::memcpy(key_, key.data(), key_length_);
    reset();
}

Blake2s::~Blake2s() {
    secure_erase(MutableView<Byte>(key_));
    secure_erase(MutableView<Byte>(buffer_));
    secure_erase(MutableView<Byte>(reinterpret_cast<Byte*>(state_), sizeof(state_)));
}

// Parameter block word 0: digest length 32, key length, fanout 1, depth 1.
// A keyed hash starts with the key padded to one block (RFC 7693 3.3).
void Blake2s::reset() noexcept {
    for (usize word = 0; word < 8; ++word) state_[word] = initial[word];
    state_[0] ^= 0x01010000U ^ (static_cast<u32>(key_length_) << 8) ^ 32U;
    std::memset(buffer_, 0, sizeof(buffer_));
    buffered_ = 0;
    counted_ = 0;
    if (key_length_ > 0) {
        std::memcpy(buffer_, key_, key_length_);
        buffered_ = sizeof(buffer_);
    }
}

// Compresses buffer_, counting `buffered_` more bytes. The counter cannot
// wrap: a 64-bit byte count would need 16 EiB of input.
void Blake2s::compress(const bool last) noexcept {
    counted_ += buffered_;
    u32 message[16]{};
    for (usize word = 0; word < 16; ++word) message[word] = load_little(buffer_ + 4 * word);
    u32 v[16]{};
    for (usize word = 0; word < 8; ++word) {
        v[word] = state_[word];
        v[word + 8] = initial[word];
    }
    v[12] ^= static_cast<u32>(counted_);
    v[13] ^= static_cast<u32>(counted_ >> 32);
    if (last) v[14] = ~v[14];
    for (const u8 (&order)[16] : sigma) {
        mix(v, 0, 4, 8, 12, message[order[0]], message[order[1]]);
        mix(v, 1, 5, 9, 13, message[order[2]], message[order[3]]);
        mix(v, 2, 6, 10, 14, message[order[4]], message[order[5]]);
        mix(v, 3, 7, 11, 15, message[order[6]], message[order[7]]);
        mix(v, 0, 5, 10, 15, message[order[8]], message[order[9]]);
        mix(v, 1, 6, 11, 12, message[order[10]], message[order[11]]);
        mix(v, 2, 7, 8, 13, message[order[12]], message[order[13]]);
        mix(v, 3, 4, 9, 14, message[order[14]], message[order[15]]);
    }
    for (usize word = 0; word < 8; ++word) state_[word] ^= v[word] ^ v[word + 8];
    // The block and the working state hold the hashed bytes (a precedence
    // seed, a key): erased so they do not outlive the call in the dead frame
    // (audit 2026-11-15, A-stxlib-algorithms-01-F2). Word stores through
    // volatile, as secure_erase does for bytes: 32 stores a block.
    volatile u32* const message_words = message;
    volatile u32* const state_words = v;
    for (usize word = 0; word < 16; ++word) {
        message_words[word] = 0;
        state_words[word] = 0;
    }
}

// A full buffer is compressed only when more input follows, since the last
// block is compressed by finish with the final flag.
void Blake2s::update(const View<Byte> input) noexcept {
    usize offset = 0;
    while (offset < input.count()) {
        if (buffered_ == sizeof(buffer_)) {
            compress(false);
            buffered_ = 0;
        }
        const usize room = sizeof(buffer_) - buffered_;
        const usize left = input.count() - offset;
        const usize taken = left < room ? left : room;
        std::memcpy(buffer_ + buffered_, input.data() + offset, taken);
        buffered_ += taken;
        offset += taken;
    }
}

Digest Blake2s::finish() noexcept {
    std::memset(buffer_ + buffered_, 0, sizeof(buffer_) - buffered_);
    compress(true);
    Digest result{};
    for (usize word = 0; word < 8; ++word) {
        for (usize byte = 0; byte < 4; ++byte) {
            result.bytes[4 * word + byte] = static_cast<Byte>((state_[word] >> (8 * byte)) & 0xffU);
        }
    }
    secure_erase(MutableView<Byte>(buffer_));
    return result;
}

Digest blake2s(const View<Byte> input) noexcept {
    Blake2s hasher{};
    hasher.update(input);
    const Digest result = hasher.finish();
    return result;
}

Digest blake2s_keyed(const View<Byte> key, const View<Byte> input) noexcept {
    Blake2s hasher(key);
    hasher.update(input);
    const Digest result = hasher.finish();
    return result;
}

void secure_erase(const MutableView<Byte> bytes) noexcept {
    volatile Byte* const target = bytes.data();
    for (usize index = 0; index < bytes.count(); ++index) target[index] = 0;
}

} // namespace stxlib::hash
