// The precedence chain (include/stxlib/precedence.hpp). Every hash is
// BLAKE2s-256 over a NUL-terminated ASCII tag followed by fixed-length
// fields (digests as 32 bytes, integers little-endian), so no two inputs of
// different shape can collide by concatenation.
// tests/reference/precedence_reference.py implements the same bytes
// independently; the tests compare the two.
//
// Ported from stx_auxlib's src/precedence.cpp at b6e330a: the same hashes,
// in the same order, over the same bytes. Changes: the key tree lives in
// one Buffer per block (keys, then the levels) allocated when the chain is
// made, so advance allocates nothing; the records use fixed arrays with
// counts; the verifier's root list grows by doubling and reports
// out_of_memory.

#include "stxlib/precedence.hpp"

#include "stxlib/entropy.hpp"

namespace stxlib::precedence {

namespace {

constexpr u32 chain_length = 15; // w - 1, for w = 16
constexpr usize last_slot = epochs_per_block - 1;
// Where each region of a block's buffer starts (Chain::block_digests).
constexpr usize tree_offset = epochs_per_block;

// The offset of tree level `height` inside the tree: level h holds
// 256 >> h digests after the 512 - (512 >> h) of the levels below it.
constexpr usize level_offset(const usize height) noexcept {
    const usize result = 2 * epochs_per_block - ((2 * epochs_per_block) >> height);
    return result;
}

static_assert(level_offset(0) == 0 && level_offset(1) == 256 && level_offset(key_tree_height) == 510);
static_assert(Chain::block_digests == tree_offset + level_offset(key_tree_height) + 1);

// Builds one domain-separated hash input.
class Input final {
public:
    // The tag's bytes and its NUL terminator, as stx_auxlib hashed them.
    template <usize Count>
    explicit Input(const char (&tag)[Count]) noexcept {
        hasher_.update(View<Byte>(reinterpret_cast<const Byte*>(tag), Count));
    }
    void digest(const hash::Digest& value) noexcept { hasher_.update(View<Byte>(value.bytes)); }
    void u32_field(const u32 value) noexcept {
        Byte bytes[4]{};
        for (usize index = 0; index < 4; ++index) bytes[index] = static_cast<Byte>((value >> (8 * index)) & 0xffU);
        hasher_.update(View<Byte>(bytes));
    }
    void u64_field(const u64 value) noexcept {
        Byte bytes[8]{};
        for (usize index = 0; index < 8; ++index) bytes[index] = static_cast<Byte>((value >> (8 * index)) & 0xffU);
        hasher_.update(View<Byte>(bytes));
    }
    // A signed time as its two's-complement 64 bits.
    void time(const WallTime value) noexcept { u64_field(static_cast<u64>(value.unix_nanoseconds())); }
    [[nodiscard]] hash::Digest finish() noexcept {
        const hash::Digest result = hasher_.finish();
        return result;
    }

private:
    hash::Blake2s hasher_{};
};

void erase(hash::Digest& digest) noexcept { hash::secure_erase(MutableView<Byte>(digest.bytes)); }

hash::Digest ratchet(const hash::Digest& seed) noexcept {
    Input input("stx.precedence.ratchet");
    input.digest(seed);
    return input.finish();
}

hash::Digest wots_secret(const hash::Digest& seed, const u32 chain) noexcept {
    Input input("stx.precedence.wots.secret");
    input.digest(seed);
    input.u32_field(chain);
    return input.finish();
}

// `steps` applications of the chain function from step `start`.
hash::Digest walk_chain(const hash::Digest& public_seed, const u64 epoch, const u32 chain, hash::Digest value,
                        const u32 start, const u32 steps) noexcept {
    for (u32 step = start; step < start + steps; ++step) {
        Input input("stx.precedence.wots.chain");
        input.digest(public_seed);
        input.u64_field(epoch);
        input.u32_field(chain);
        input.u32_field(step);
        input.digest(value);
        value = input.finish();
    }
    return value;
}

// The 67 base-16 digits signed: the message's 64 nibbles, high nibble first,
// then the 12-bit checksum sum(15 - digit) as three nibbles.
struct Digits final {
    u32 values[signature_chains]{};
};

Digits digits_of(const hash::Digest& message) noexcept {
    Digits digits{};
    u32 checksum = 0;
    for (usize byte = 0; byte < hash::digest_bytes; ++byte) {
        const u32 value = message.bytes[byte];
        digits.values[2 * byte] = value >> 4;
        digits.values[2 * byte + 1] = value & 15U;
    }
    for (usize index = 0; index < 64; ++index) checksum += chain_length - digits.values[index];
    digits.values[64] = (checksum >> 8) & 15U;
    digits.values[65] = (checksum >> 4) & 15U;
    digits.values[66] = checksum & 15U;
    return digits;
}

struct ChainEnds final {
    hash::Digest values[signature_chains]{};
};

hash::Digest compress_public_key(const hash::Digest& public_seed, const u64 epoch, const ChainEnds& ends) noexcept {
    Input input("stx.precedence.wots.public");
    input.digest(public_seed);
    input.u64_field(epoch);
    for (const hash::Digest& end : ends.values) input.digest(end);
    return input.finish();
}

hash::Digest wots_public_key(const hash::Digest& public_seed, const u64 epoch, const hash::Digest& seed) noexcept {
    ChainEnds ends{};
    for (u32 chain = 0; chain < signature_chains; ++chain) {
        hash::Digest secret = wots_secret(seed, chain);
        ends.values[chain] = walk_chain(public_seed, epoch, chain, secret, 0, chain_length);
        erase(secret);
    }
    return compress_public_key(public_seed, epoch, ends);
}

void wots_sign(const hash::Digest& public_seed, const u64 epoch, const hash::Digest& seed,
               const hash::Digest& message, hash::Digest (&signature)[signature_chains]) noexcept {
    const Digits digits = digits_of(message);
    for (u32 chain = 0; chain < signature_chains; ++chain) {
        hash::Digest secret = wots_secret(seed, chain);
        signature[chain] = walk_chain(public_seed, epoch, chain, secret, 0, digits.values[chain]);
        erase(secret);
    }
}

// The public key a signature implies for `message`; a forged or altered
// signature implies a different key.
hash::Digest wots_public_from_signature(const hash::Digest& public_seed, const u64 epoch,
                                        const hash::Digest& message,
                                        const hash::Digest (&signature)[signature_chains]) noexcept {
    const Digits digits = digits_of(message);
    ChainEnds ends{};
    for (u32 chain = 0; chain < signature_chains; ++chain) {
        ends.values[chain] = walk_chain(public_seed, epoch, chain, signature[chain], digits.values[chain],
                                        chain_length - digits.values[chain]);
    }
    return compress_public_key(public_seed, epoch, ends);
}

hash::Digest key_leaf(const hash::Digest& public_seed, const u64 epoch, const hash::Digest& key) noexcept {
    Input input("stx.precedence.key");
    input.digest(public_seed);
    input.u64_field(epoch);
    input.digest(key);
    return input.finish();
}

hash::Digest key_node(const hash::Digest& public_seed, const u64 block, const u32 height, const u64 index,
                      const hash::Digest& left, const hash::Digest& right) noexcept {
    Input input("stx.precedence.node");
    input.digest(public_seed);
    input.u64_field(block);
    input.u32_field(height);
    input.u64_field(index);
    input.digest(left);
    input.digest(right);
    return input.finish();
}

// The tree digest at `height` and `index` of a block's buffer.
hash::Digest& tree_at(Buffer<hash::Digest>& block, const usize height, const usize index) noexcept {
    return block[tree_offset + level_offset(height) + index];
}

const hash::Digest& tree_at(const Buffer<hash::Digest>& block, const usize height, const usize index) noexcept {
    return block[tree_offset + level_offset(height) + index];
}

// Fills the block's key tree from its keys (block[0, 256)): level 0 is the
// 256 key leaves.
void build_key_tree(const hash::Digest& public_seed, const u64 block_number, Buffer<hash::Digest>& block) noexcept {
    const u64 first_epoch = block_number * epochs_per_block;
    for (usize slot = 0; slot < epochs_per_block; ++slot) {
        tree_at(block, 0, slot) = key_leaf(public_seed, first_epoch + slot, block[slot]);
    }
    for (usize height = 1; height <= key_tree_height; ++height) {
        const usize nodes = epochs_per_block >> height;
        for (usize index = 0; index < nodes; ++index) {
            tree_at(block, height, index) =
                key_node(public_seed, block_number, static_cast<u32>(height), index, tree_at(block, height - 1, 2 * index),
                         tree_at(block, height - 1, 2 * index + 1));
        }
    }
}

hash::Digest key_root_of(const Buffer<hash::Digest>& block) noexcept { return tree_at(block, key_tree_height, 0); }

hash::Digest infos_digest(const Certificate& certificate) noexcept {
    hash::Digest digest{};
    for (u32 index = 0; index < certificate.info_count; ++index) {
        Input input("stx.precedence.infos");
        input.digest(digest);
        input.digest(certificate.infos[index]);
        digest = input.finish();
    }
    return digest;
}

hash::Digest genesis_digest(const Anchor& anchor) noexcept {
    Input input("stx.precedence.genesis");
    input.digest(anchor.public_seed);
    input.digest(anchor.first_key_root);
    input.time(anchor.genesis);
    return input.finish();
}

hash::Digest block_summary(const hash::Digest& public_seed, const u64 block, const hash::Digest& key_root,
                           const hash::Digest& last_leaf, const WallTime first, const WallTime last) noexcept {
    Input input("stx.precedence.block");
    input.digest(public_seed);
    input.u64_field(block);
    input.digest(key_root);
    input.digest(last_leaf);
    input.time(first);
    input.time(last);
    return input.finish();
}

hash::Digest extend_chain(const hash::Digest& chain_digest, const hash::Digest& summary) noexcept {
    Input input("stx.precedence.chain");
    input.digest(chain_digest);
    input.digest(summary);
    return input.finish();
}

// The 256 public keys of the block whose first epoch has seed `first_seed`,
// into block[0, 256). The seeds it walks through are erased as it goes.
void derive_keys(const hash::Digest& public_seed, const hash::Digest& first_seed, const u64 first_epoch,
                 Buffer<hash::Digest>& block) noexcept {
    hash::Digest seed = first_seed;
    for (usize slot = 0; slot < epochs_per_block; ++slot) {
        block[slot] = wots_public_key(public_seed, first_epoch + slot, seed);
        hash::Digest next = ratchet(seed);
        erase(seed);
        seed = next;
        erase(next);
    }
    erase(seed);
}

const hash::Digest zero_digest{};

bool earlier_than(const WallTime left, const WallTime right) noexcept {
    const bool result = left < right;
    return result;
}

} // namespace

hash::Digest leaf_of(const Anchor& anchor, const Certificate& certificate) noexcept {
    STXLIB_REQUIRE(certificate.info_count <= infos_per_epoch, "precedence::leaf_of: info_count out of range");
    Input input("stx.precedence.epoch");
    input.digest(anchor.public_seed);
    input.u64_field(certificate.epoch);
    input.time(certificate.timestamp);
    input.u32_field(certificate.info_count);
    input.digest(infos_digest(certificate));
    input.digest(certificate.previous_leaf);
    input.digest(certificate.chain_digest);
    input.digest(certificate.next_key_root);
    return input.finish();
}

ChainState::~ChainState() { erase(seed); }

Chain::~Chain() { erase_secret(); }

void Chain::erase_secret() noexcept { erase(seed_); }

Chain::Chain(Chain&& other) noexcept
    : anchor_(other.anchor_), seed_(other.seed_), epoch_(other.epoch_), last_time_(other.last_time_),
      block_first_time_(other.block_first_time_), previous_leaf_(other.previous_leaf_),
      chain_digest_(other.chain_digest_), block_(static_cast<Buffer<hash::Digest>&&>(other.block_)),
      next_block_(static_cast<Buffer<hash::Digest>&&>(other.next_block_)), pending_count_(other.pending_count_) {
    for (usize index = 0; index < infos_per_epoch; ++index) pending_[index] = other.pending_[index];
    other.erase_secret();
}

Chain& Chain::operator=(Chain&& other) noexcept {
    if (this == &other) return *this;
    erase_secret();
    anchor_ = other.anchor_;
    seed_ = other.seed_;
    epoch_ = other.epoch_;
    last_time_ = other.last_time_;
    block_first_time_ = other.block_first_time_;
    previous_leaf_ = other.previous_leaf_;
    chain_digest_ = other.chain_digest_;
    block_ = static_cast<Buffer<hash::Digest>&&>(other.block_);
    next_block_ = static_cast<Buffer<hash::Digest>&&>(other.next_block_);
    for (usize index = 0; index < infos_per_epoch; ++index) pending_[index] = other.pending_[index];
    pending_count_ = other.pending_count_;
    other.erase_secret();
    return *this;
}

OperationResult<Chain> Chain::with_storage() noexcept {
    OperationResult<Buffer<hash::Digest>> block = Buffer<hash::Digest>::allocate_zeroed(block_digests);
    if (block.failed()) return OperationResult<Chain>::failure(*block.error());
    OperationResult<Buffer<hash::Digest>> next_block = Buffer<hash::Digest>::allocate_zeroed(block_digests);
    if (next_block.failed()) return OperationResult<Chain>::failure(*next_block.error());
    Chain chain{};
    chain.block_ = static_cast<OperationResult<Buffer<hash::Digest>>&&>(block).take_value();
    chain.next_block_ = static_cast<OperationResult<Buffer<hash::Digest>>&&>(next_block).take_value();
    return OperationResult<Chain>::success(static_cast<Chain&&>(chain));
}

OperationResult<Chain> Chain::from_seeds(const hash::Digest& secret_seed, const hash::Digest& public_seed,
                                const WallTime now) noexcept {
    OperationResult<Chain> made = with_storage();
    if (made.failed()) return made;
    Chain& chain = *made.value();
    chain.seed_ = secret_seed;
    chain.anchor_.public_seed = public_seed;
    chain.anchor_.genesis = now;
    derive_keys(public_seed, secret_seed, 0, chain.block_);
    build_key_tree(public_seed, 0, chain.block_);
    chain.anchor_.first_key_root = key_root_of(chain.block_);
    chain.last_time_ = now;
    chain.block_first_time_ = now;
    chain.chain_digest_ = genesis_digest(chain.anchor_);
    return made;
}

OperationResult<Chain> Chain::create(const WallTime now) noexcept {
    Byte entropy[64]{};
    const MutableView<Byte> entropy_view(entropy);
    const OperationStatus filled = fill_entropy(entropy_view);
    if (filled.failed()) return OperationResult<Chain>::failure(*filled.error());
    hash::Digest secret_seed{};
    hash::Digest public_seed{};
    for (usize index = 0; index < hash::digest_bytes; ++index) {
        secret_seed.bytes[index] = entropy[index];
        public_seed.bytes[index] = entropy[hash::digest_bytes + index];
    }
    hash::secure_erase(entropy_view);
    OperationResult<Chain> made = from_seeds(secret_seed, public_seed, now);
    erase(secret_seed);
    return made;
}

OperationResult<Chain> Chain::restore(const ChainState& state) noexcept {
    if (state.pending_count > infos_per_epoch) {
        return OperationResult<Chain>::failure(ErrorRecord::make(
            ErrorKind::invalid_argument, "stxlib.precedence.restore",
            ErrorDetail{.message = "the state's pending count is above infos_per_epoch"}));
    }
    OperationResult<Chain> made = with_storage();
    if (made.failed()) return made;
    Chain& chain = *made.value();
    chain.anchor_ = state.anchor;
    chain.seed_ = state.seed;
    chain.epoch_ = state.epoch;
    chain.last_time_ = state.last_time;
    chain.block_first_time_ = state.block_first_time;
    chain.previous_leaf_ = state.previous_leaf;
    chain.chain_digest_ = state.chain_digest;
    for (usize slot = 0; slot < epochs_per_block; ++slot) chain.block_[slot] = state.block_keys[slot];
    build_key_tree(state.anchor.public_seed, state.epoch / epochs_per_block, chain.block_);
    for (usize index = 0; index < infos_per_epoch; ++index) chain.pending_[index] = state.pending[index];
    chain.pending_count_ = state.pending_count;
    return made;
}

void Chain::save(ChainState& state) const noexcept {
    state.anchor = anchor_;
    state.seed = seed_;
    state.epoch = epoch_;
    state.last_time = last_time_;
    state.block_first_time = block_first_time_;
    state.previous_leaf = previous_leaf_;
    state.chain_digest = chain_digest_;
    for (usize slot = 0; slot < epochs_per_block; ++slot) state.block_keys[slot] = block_[slot];
    for (usize index = 0; index < infos_per_epoch; ++index) state.pending[index] = pending_[index];
    state.pending_count = pending_count_;
}

PrecedenceStatus Chain::record(const hash::Digest& info) noexcept {
    if (pending_count_ == infos_per_epoch) return PrecedenceStatus::epoch_full;
    pending_[pending_count_] = info;
    ++pending_count_;
    return PrecedenceStatus::ok;
}

PrecedenceStatus Chain::advance(const WallTime now, Certificate& certificate) noexcept {
    if (epoch_ == u64_max) return PrecedenceStatus::exhausted;
    const u64 block = epoch_ / epochs_per_block;
    const usize slot = static_cast<usize>(epoch_ % epochs_per_block);
    const WallTime time = earlier_than(now, last_time_) ? last_time_ : now;
    if (slot == 0) block_first_time_ = time;

    // The next block's keys, from the next epoch's seed, on a block's last
    // epoch: its signed leaf commits their root.
    hash::Digest next_root{};
    if (slot == last_slot) {
        hash::Digest next_seed = ratchet(seed_);
        derive_keys(anchor_.public_seed, next_seed, epoch_ + 1, next_block_);
        erase(next_seed);
        build_key_tree(anchor_.public_seed, block + 1, next_block_);
        next_root = key_root_of(next_block_);
    }

    certificate.epoch = epoch_;
    certificate.timestamp = time;
    for (usize index = 0; index < infos_per_epoch; ++index) {
        certificate.infos[index] = index < pending_count_ ? pending_[index] : zero_digest;
    }
    certificate.info_count = pending_count_;
    certificate.previous_leaf = previous_leaf_;
    certificate.chain_digest = chain_digest_;
    certificate.next_key_root = next_root;
    const hash::Digest leaf = leaf_of(anchor_, certificate);
    wots_sign(anchor_.public_seed, epoch_, seed_, leaf, certificate.signature);
    usize position = slot;
    for (usize height = 0; height < key_tree_height; ++height) {
        certificate.key_path[height] = tree_at(block_, height, position ^ 1U);
        position >>= 1;
    }

    previous_leaf_ = leaf;
    last_time_ = time;
    pending_count_ = 0;
    if (slot == last_slot) {
        const hash::Digest summary =
            block_summary(anchor_.public_seed, block, key_root_of(block_), leaf, block_first_time_, time);
        chain_digest_ = extend_chain(chain_digest_, summary);
        // The next block becomes current; the old buffer is reused for the
        // block after.
        Buffer<hash::Digest> finished = static_cast<Buffer<hash::Digest>&&>(block_);
        block_ = static_cast<Buffer<hash::Digest>&&>(next_block_);
        next_block_ = static_cast<Buffer<hash::Digest>&&>(finished);
    }
    // next_seed is erased too, as the block-end copy above is: after the
    // next advance it would be a seed the chain says is gone (audit
    // 2026-11-15, A-stxlib-algorithms-01-F2).
    hash::Digest next_seed = ratchet(seed_);
    erase_secret();
    seed_ = next_seed;
    erase(next_seed);
    ++epoch_;
    return PrecedenceStatus::ok;
}

const hash::Digest& Verifier::key_root(const usize block) const noexcept {
    STXLIB_REQUIRE(block < known_blocks(), "precedence::Verifier: unknown block");
    if (block == 0) return anchor_.first_key_root;
    return later_roots_[block - 1];
}

PrecedenceStatus Verifier::verify(const Certificate& certificate) noexcept {
    const u64 block = certificate.epoch / epochs_per_block;
    const usize slot = static_cast<usize>(certificate.epoch % epochs_per_block);
    if (block >= known_blocks()) return PrecedenceStatus::unknown_block;
    if (certificate.info_count > infos_per_epoch) return PrecedenceStatus::invalid_certificate;
    if (slot != last_slot && certificate.next_key_root != zero_digest) return PrecedenceStatus::invalid_certificate;

    const hash::Digest leaf = leaf_of(anchor_, certificate);
    const hash::Digest key =
        wots_public_from_signature(anchor_.public_seed, certificate.epoch, leaf, certificate.signature);
    hash::Digest node = key_leaf(anchor_.public_seed, certificate.epoch, key);
    u64 position = slot;
    for (usize height = 0; height < key_tree_height; ++height) {
        const hash::Digest& sibling = certificate.key_path[height];
        const bool right = (position & 1U) != 0;
        node = key_node(anchor_.public_seed, block, static_cast<u32>(height + 1), position >> 1,
                        right ? sibling : node, right ? node : sibling);
        position >>= 1;
    }
    if (node != key_root(static_cast<usize>(block))) return PrecedenceStatus::invalid_certificate;

    // A verified block-final certificate vouches for the next block's keys.
    if (slot == last_slot) {
        const usize next = static_cast<usize>(block) + 1;
        if (next < known_blocks()) {
            // Two validly signed, conflicting block-final certificates mean
            // the secret state was copied; neither can be trusted.
            if (key_root(next) != certificate.next_key_root) return PrecedenceStatus::invalid_certificate;
            return PrecedenceStatus::ok;
        }
        if (later_count_ == later_roots_.count()) {
            // Doubling from 8; the new list is filled before it replaces
            // the old one, so a failure leaves the verifier unchanged.
            const usize capacity = later_roots_.count() == 0 ? 8 : 2 * later_roots_.count();
            OperationResult<Buffer<hash::Digest>> grown = Buffer<hash::Digest>::allocate_zeroed(capacity);
            if (grown.failed()) return PrecedenceStatus::out_of_memory;
            Buffer<hash::Digest>& larger = *grown.value();
            for (usize index = 0; index < later_count_; ++index) larger[index] = later_roots_[index];
            later_roots_ = static_cast<Buffer<hash::Digest>&&>(larger);
        }
        later_roots_[later_count_] = certificate.next_key_root;
        ++later_count_;
    }
    return PrecedenceStatus::ok;
}

PrecedenceStatus recorded_in(const Certificate& certificate, const hash::Digest& info) noexcept {
    if (certificate.info_count > infos_per_epoch) return PrecedenceStatus::not_recorded;
    for (u32 index = 0; index < certificate.info_count; ++index) {
        if (certificate.infos[index] == info) return PrecedenceStatus::ok;
    }
    return PrecedenceStatus::not_recorded;
}

bool precedes(const Certificate& earlier, const Certificate& later, u64& offset) noexcept {
    offset = 0;
    if (earlier.epoch >= later.epoch) return false;
    offset = later.epoch - earlier.epoch;
    return true;
}

bool follows_directly(const Anchor& anchor, const Certificate& earlier, const Certificate& later) noexcept {
    if (earlier.epoch == u64_max) return false;
    if (earlier.info_count > infos_per_epoch) return false;
    const bool result = later.epoch == earlier.epoch + 1 && later.previous_leaf == leaf_of(anchor, earlier) &&
                        !earlier_than(later.timestamp, earlier.timestamp);
    return result;
}

} // namespace stxlib::precedence
