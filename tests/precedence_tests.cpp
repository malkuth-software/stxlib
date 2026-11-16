// Tests of the precedence chain (include/stxlib/precedence.hpp). Ported
// from stx_auxlib's tests/precedence_tests.cpp at b6e330a (the BLAKE2s part
// is tests/hash_tests.cpp) with the same expectations. Known answers come
// from tests/reference/precedence_reference.py, which implements the
// construction independently over Python's hashlib (rerun on 2026-11-13:
// its output matches every known answer here). Lifecycle, tampering and
// persistence are tested on fixed seeds, not proved.

#include "stxlib/clocks.hpp"
#include "stxlib/hash.hpp"
#include "stxlib/precedence.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace precedence = stxlib::precedence;
namespace hash = stxlib::hash;
using stxlib::Byte;
using stxlib::i64;
using stxlib::u64;
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

stxlib::WallTime at_ns(const i64 nanoseconds) {
    const stxlib::WallTime result = stxlib::WallTime::from_unix_nanoseconds(nanoseconds);
    return result;
}

constexpr i64 genesis_ns = 1793482205000000000LL;

// A chain from fixed seeds; the test fails (and stops) when it cannot be made.
precedence::Chain fixed_chain(const unsigned secret_first = 0) {
    stxlib::OperationResult<precedence::Chain> made =
        precedence::Chain::from_seeds(counting_digest(secret_first), counting_digest(32), at_ns(genesis_ns));
    if (!made.succeeded()) {
        std::fprintf(stderr, "stxlib precedence: a chain could not be made\n");
        std::abort();
    }
    precedence::Chain result = static_cast<stxlib::OperationResult<precedence::Chain>&&>(made).take_value();
    return result;
}

bool same_signature(const precedence::Certificate& left, const precedence::Certificate& right) {
    for (usize chain = 0; chain < precedence::signature_chains; ++chain) {
        if (left.signature[chain] != right.signature[chain]) return false;
    }
    return true;
}

void known_answers() {
    precedence::Chain chain = fixed_chain();
    CHECK(hex_of(chain.anchor().first_key_root) == "1a59a551b642ef1798d67ad2c0747a706b94dc4bd89cbafd4eacc55616696572");
    CHECK(chain.record(digest_of("alpha")) == precedence::PrecedenceStatus::ok);
    precedence::Certificate certificate{};
    CHECK(chain.advance(at_ns(genesis_ns + 1000000000LL), certificate) == precedence::PrecedenceStatus::ok);
    CHECK(hex_of(precedence::leaf_of(chain.anchor(), certificate)) ==
          "1f9cdf2817920e91622b6b0c4e1577c5fcbecd62b264a327dd8f0cdf391a9113");
    CHECK(hex_of(certificate.signature[0]) == "629a7bb8ffb4ba4de405b4ac739ab800242d1fa24e72de87e681250b3b157840");
    CHECK(hex_of(certificate.signature[66]) == "661ea115aa6d8bf8e69049ae54dbc0839592938e6d8fae315c2ef84b9bc4762e");
    CHECK(hex_of(certificate.key_path[0]) == "59e888090592e82a52c03f6138b58680f312680818b1ecf936754c4bdd5c7fb3");
    CHECK(hex_of(certificate.key_path[7]) == "ebef89f081ad90193b73441ad1a0267bca2bdec8e5c32942086dc4a497c4d006");
}

// Runs a chain across two block boundaries, verifying every certificate in
// order, and checks linkage, precedence and the info limit.
void lifecycle() {
    precedence::Chain chain = fixed_chain();
    precedence::Verifier verifier(chain.anchor());
    std::vector<precedence::Certificate> certificates{};
    const usize epochs = 2 * precedence::epochs_per_block + 10;
    bool open_epochs = true;
    bool recorded = true;
    bool advanced = true;
    bool verified = true;
    bool linked = true;
    for (usize epoch = 0; epoch < epochs; ++epoch) {
        open_epochs = open_epochs && chain.epoch() == epoch;
        recorded = recorded && chain.record(digest_of("item " + std::to_string(epoch))) == precedence::PrecedenceStatus::ok;
        precedence::Certificate certificate{};
        // A clock that steps back is recorded as the previous time.
        const i64 when = genesis_ns + static_cast<i64>(epoch) * 1000 - (epoch == 5 ? 5000 : 0);
        advanced = advanced && chain.advance(at_ns(when), certificate) == precedence::PrecedenceStatus::ok;
        verified = verified && verifier.verify(certificate) == precedence::PrecedenceStatus::ok;
        if (!certificates.empty()) {
            linked = linked && precedence::follows_directly(chain.anchor(), certificates.back(), certificate);
        }
        certificates.push_back(certificate);
    }
    CHECK(open_epochs);
    CHECK(recorded);
    CHECK(advanced);
    CHECK(verified);
    CHECK(linked);
    CHECK(verifier.known_blocks() == 3);
    CHECK(certificates[5].timestamp == certificates[4].timestamp);

    u64 offset = 0;
    CHECK(precedence::precedes(certificates[3], certificates[300], offset) && offset == 297);
    CHECK(!precedence::precedes(certificates[300], certificates[3], offset) && offset == 0);
    CHECK(precedence::recorded_in(certificates[300], digest_of("item 300")) == precedence::PrecedenceStatus::ok);
    CHECK(precedence::recorded_in(certificates[300], digest_of("item 3")) == precedence::PrecedenceStatus::not_recorded);

    // A fresh verifier cannot check block 1 before learning block 0's link.
    precedence::Verifier fresh(chain.anchor());
    CHECK(fresh.verify(certificates[300]) == precedence::PrecedenceStatus::unknown_block);
    CHECK(fresh.verify(certificates[255]) == precedence::PrecedenceStatus::ok);
    CHECK(fresh.verify(certificates[300]) == precedence::PrecedenceStatus::ok);
    // A block-final certificate seen again is accepted and teaches nothing new.
    CHECK(fresh.verify(certificates[255]) == precedence::PrecedenceStatus::ok && fresh.known_blocks() == 2);

    // A moved-from verifier knows the anchor's block only, and a later
    // block's certificate is unknown to it, not a failed requirement
    // (A-stxlib-include-01-F6).
    precedence::Verifier taken(static_cast<precedence::Verifier&&>(fresh));
    CHECK(taken.known_blocks() == 2);
    CHECK(fresh.known_blocks() == 1);
    CHECK(fresh.verify(certificates[300]) == precedence::PrecedenceStatus::unknown_block);
    precedence::Verifier assigned(chain.anchor());
    assigned = static_cast<precedence::Verifier&&>(taken);
    CHECK(assigned.known_blocks() == 2 && taken.known_blocks() == 1);
    CHECK(assigned.verify(certificates[300]) == precedence::PrecedenceStatus::ok);
    CHECK(taken.verify(certificates[300]) == precedence::PrecedenceStatus::unknown_block);

    bool below_limit = true;
    for (usize index = 0; index < precedence::infos_per_epoch; ++index) {
        below_limit = below_limit &&
                      chain.record(counting_digest(static_cast<unsigned>(index))) == precedence::PrecedenceStatus::ok;
    }
    CHECK(below_limit);
    CHECK(chain.record(counting_digest(99)) == precedence::PrecedenceStatus::epoch_full);
}

// Any change to a certificate, or a certificate from another secret, fails.
void tampering() {
    precedence::Chain chain = fixed_chain();
    precedence::Verifier verifier(chain.anchor());
    static_cast<void>(chain.record(digest_of("original")));
    precedence::Certificate certificate{};
    static_cast<void>(chain.advance(at_ns(genesis_ns + 1), certificate));
    CHECK(verifier.verify(certificate) == precedence::PrecedenceStatus::ok);

    precedence::Certificate changed_info = certificate;
    changed_info.infos[0] = digest_of("forged");
    CHECK(verifier.verify(changed_info) == precedence::PrecedenceStatus::invalid_certificate);
    precedence::Certificate changed_count = certificate;
    changed_count.info_count = 2;
    CHECK(verifier.verify(changed_count) == precedence::PrecedenceStatus::invalid_certificate);
    precedence::Certificate overfull = certificate;
    overfull.info_count = precedence::infos_per_epoch + 1;
    CHECK(verifier.verify(overfull) == precedence::PrecedenceStatus::invalid_certificate);
    CHECK(precedence::recorded_in(overfull, digest_of("original")) == precedence::PrecedenceStatus::not_recorded);
    precedence::Certificate changed_time = certificate;
    changed_time.timestamp = at_ns(genesis_ns - 1000000000LL);
    CHECK(verifier.verify(changed_time) == precedence::PrecedenceStatus::invalid_certificate);
    precedence::Certificate changed_epoch = certificate;
    changed_epoch.epoch = 1;
    CHECK(verifier.verify(changed_epoch) == precedence::PrecedenceStatus::invalid_certificate);
    precedence::Certificate changed_signature = certificate;
    changed_signature.signature[13].bytes[0] = static_cast<Byte>(changed_signature.signature[13].bytes[0] ^ 1U);
    CHECK(verifier.verify(changed_signature) == precedence::PrecedenceStatus::invalid_certificate);
    precedence::Certificate changed_path = certificate;
    changed_path.key_path[4].bytes[31] = static_cast<Byte>(changed_path.key_path[4].bytes[31] ^ 0x80U);
    CHECK(verifier.verify(changed_path) == precedence::PrecedenceStatus::invalid_certificate);
    precedence::Certificate added_next = certificate;
    added_next.next_key_root = counting_digest(7);
    CHECK(verifier.verify(added_next) == precedence::PrecedenceStatus::invalid_certificate);

    precedence::Chain other = fixed_chain(1);
    static_cast<void>(other.record(digest_of("original")));
    precedence::Certificate foreign{};
    static_cast<void>(other.advance(at_ns(genesis_ns + 1), foreign));
    CHECK(verifier.verify(foreign) == precedence::PrecedenceStatus::invalid_certificate);
}

// Saving and restoring continues the same chain, bit for bit; a created
// chain uses fresh entropy.
void persistence() {
    precedence::Chain straight = fixed_chain();
    precedence::Chain interrupted = fixed_chain();
    precedence::Certificate first{};
    precedence::Certificate second{};
    for (usize epoch = 0; epoch < 250; ++epoch) {
        static_cast<void>(straight.advance(at_ns(genesis_ns + static_cast<i64>(epoch)), first));
        static_cast<void>(interrupted.advance(at_ns(genesis_ns + static_cast<i64>(epoch)), second));
    }
    static_cast<void>(interrupted.record(digest_of("pending")));
    static_cast<void>(straight.record(digest_of("pending")));
    precedence::ChainState state{};
    interrupted.save(state);
    stxlib::OperationResult<precedence::Chain> restored = precedence::Chain::restore(state);
    CHECK(restored.succeeded());
    if (!restored.succeeded()) return;
    precedence::Chain& resumed = *restored.value();
    // Across the block boundary at epoch 256, so the restored chain's next
    // block is derived from restored state too.
    bool same = true;
    for (usize epoch = 250; epoch < 270; ++epoch) {
        static_cast<void>(straight.advance(at_ns(genesis_ns + static_cast<i64>(epoch)), first));
        static_cast<void>(resumed.advance(at_ns(genesis_ns + static_cast<i64>(epoch)), second));
        same = same && precedence::leaf_of(straight.anchor(), first) == precedence::leaf_of(resumed.anchor(), second) &&
               same_signature(first, second);
    }
    CHECK(same);
    precedence::ChainState broken = state;
    broken.pending_count = precedence::infos_per_epoch + 1;
    const stxlib::OperationResult<precedence::Chain> refused = precedence::Chain::restore(broken);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::invalid_argument);

    // A moved-from chain keeps no secret, and the moved-to one continues.
    precedence::Chain moved = static_cast<precedence::Chain&&>(straight);
    precedence::Certificate after_move{};
    CHECK(moved.advance(at_ns(genesis_ns + 300), after_move) == precedence::PrecedenceStatus::ok);

    const stxlib::OperationResult<precedence::Chain> created = precedence::Chain::create(stxlib::WallTime::now());
    const stxlib::OperationResult<precedence::Chain> again = precedence::Chain::create(stxlib::WallTime::now());
    CHECK(created.succeeded() && again.succeeded());
    if (created.succeeded() && again.succeeded()) {
        CHECK((*created.value()).anchor().public_seed != (*again.value()).anchor().public_seed);
    }
}

void timing() {
    precedence::Chain chain = fixed_chain();
    precedence::Verifier verifier(chain.anchor());
    precedence::Certificate certificate{};
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (usize epoch = 0; epoch < 255; ++epoch) {
        static_cast<void>(chain.advance(at_ns(genesis_ns), certificate));
        static_cast<void>(verifier.verify(certificate));
    }
    const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("advance + verify: %.1f us per epoch (within a block)\n", elapsed.count() / 255.0);
}

} // namespace

int main() {
    known_answers();
    lifecycle();
    tampering();
    persistence();
    timing();
    return stxlib_tests::finish("stxlib precedence");
}
