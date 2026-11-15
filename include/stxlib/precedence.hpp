#pragma once

// The precedence chain: proof that something was recorded before
// something else, by one holder, with only hash functions. Ported from
// stx_auxlib's include/stx_auxlib/precedence.hpp at b6e330a: the
// construction and its bytes are unchanged (the known answers of
// tests/reference/precedence_reference.py still hold). The interface is
// restated in STXlib's vocabulary: fixed arrays with counts in place of
// std::vector in the records (Certificate, ChainState), STXlib's Buffer for
// the chain's key trees, Result for the calls that allocate or need
// entropy, WallTime for times. See "Changes from stx_auxlib" below.
//
// The holder keeps a tiny secret (one 32-byte seed and the epoch number)
// in secure storage. Four operations:
// - advance: close the current epoch, signing its leaf, and ratchet the
//   seed forward. The old seed is erased; nothing, including a later thief
//   of the state, can sign for a past epoch again, since recovering it would
//   mean inverting the hash.
// - record: commit an info digest to the current epoch (up to
//   infos_per_epoch of them; the epoch's certificate lists them).
// - verify: anyone holding the chain's Anchor checks a certificate, so
//   that an info digest is shown to have been recorded at that epoch.
// - precedes: given two verified certificates, the earlier epoch's infos
//   were recorded before the later epoch's, and the epochs' distance is the
//   offset between them.
//
// Each epoch also records a timestamp, which never goes backwards. The
// chain does NOT prove that time passed between epochs, and it cannot stop
// someone who copies the secret state from signing epochs from that point
// on; it orders events only while the local state stays secret. It needs
// no consensus and stores no ledger: certificates are outputs the caller
// keeps; the holder's state is a few hashes plus the current block's
// public keys.
//
// Construction (all hashes BLAKE2s-256, domain-separated; the exact bytes
// are in src/algorithms/precedence.cpp and
// tests/reference/precedence_reference.py):
// - Seeds ratchet: E(n+1) = H("ratchet" || E(n)).
// - Each epoch n has a WOTS+ one-time key (w = 16, 67 chains) derived from
//   E(n). It signs exactly one 32-byte leaf, when epoch n closes.
// - Epochs come in blocks of epochs_per_block. A block's public keys are
//   Merkle-hashed into its key root; a certificate carries its key's path,
//   so its epoch number is bound to its key's position.
// - Block 0's key root is in the Anchor. The last epoch of each block
//   commits the next block's key root inside its signed leaf, so every
//   block's keys are vouched for by the block before, back to the anchor.
// - Each leaf also binds the previous leaf and a running digest of the
//   completed blocks' summaries, so consecutive certificates are linked
//   and every leaf commits to the chain's block history.
//
// Changes from stx_auxlib:
// - Certificate::infos and ChainState::pending are fixed arrays of
//   infos_per_epoch with a count; ChainState::block_keys is a fixed array
//   of epochs_per_block (so a state can no longer hold the wrong number of
//   keys; restore refuses a pending count above infos_per_epoch).
// - Chain::create, from_seeds and restore return Result<Chain>: they
//   allocate the chain's key trees (out_of_memory), and create reads the
//   host's entropy (fill_entropy's failures, in place of
//   PrecedenceStatus::entropy_unavailable, which is no longer returned).
//   advance allocates nothing: the next block's tree has its storage from
//   the start.
// - The Verifier's list of key roots grows by one per block; when that
//   allocation fails, verify returns PrecedenceStatus::out_of_memory (new)
//   and the verifier is unchanged.

#include "stxlib/clocks.hpp"
#include "stxlib/buffer.hpp"
#include "stxlib/hash.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::precedence {

inline constexpr usize epochs_per_block = 256;
inline constexpr usize key_tree_height = 8;
inline constexpr usize infos_per_epoch = 16;
// WOTS+ with w = 16 over a 32-byte message: 64 message digits and 3
// checksum digits.
inline constexpr usize signature_chains = 67;

enum class PrecedenceStatus : u8 {
    ok = 0,
    // The current epoch already holds infos_per_epoch infos; advance first.
    epoch_full = 1,
    // Not returned by STXlib (create reports entropy failures through its
    // Result); kept so the values stay stx_auxlib's.
    entropy_unavailable = 2,
    // The certificate's signature or key path does not check out.
    invalid_certificate = 3,
    // The certificate's block has no verified key root yet: verify the last
    // certificate of each earlier block first.
    unknown_block = 4,
    // The epoch counter is at its maximum.
    exhausted = 5,
    // The info digest is not in the certificate.
    not_recorded = 6,
    // The verifier could not grow its list of key roots.
    out_of_memory = 7,
};

// What a verifier needs and trusts: published once, when the chain is made.
struct Anchor final {
    hash::Digest public_seed{};
    hash::Digest first_key_root{};
    WallTime genesis{};
};

// One closed epoch, signed. Kept by whoever needs to prove what it holds.
// A plain value (about 3.2 KB).
struct Certificate final {
    u64 epoch{0};
    WallTime timestamp{};
    // The info digests recorded in the epoch, in order: infos[0, info_count).
    hash::Digest infos[infos_per_epoch]{};
    u32 info_count{0};
    hash::Digest previous_leaf{};
    // The running digest of all completed blocks' summaries.
    hash::Digest chain_digest{};
    // The next block's key root on a block's last epoch; zero otherwise.
    hash::Digest next_key_root{};
    hash::Digest signature[signature_chains]{};
    hash::Digest key_path[key_tree_height]{};
};

// The digest the epoch's key signs; it binds every field above except the
// signature and path. Requirement: certificate.info_count <= infos_per_epoch.
[[nodiscard]] hash::Digest leaf_of(const Anchor& anchor, const Certificate& certificate) noexcept;

// Everything a chain needs to continue, for the caller's secure storage
// (about 9 KB). `seed` is the secret; the rest is public. The destructor
// erases the seed. Restoring an old copy would let its holder sign epochs
// again from that point: keep only the latest, and only in secure storage.
struct ChainState final {
    Anchor anchor{};
    hash::Digest seed{};
    u64 epoch{0};
    WallTime last_time{};
    WallTime block_first_time{};
    hash::Digest previous_leaf{};
    hash::Digest chain_digest{};
    hash::Digest block_keys[epochs_per_block]{};
    // pending[0, pending_count).
    hash::Digest pending[infos_per_epoch]{};
    u32 pending_count{0};

    ChainState() noexcept = default;
    ChainState(const ChainState&) noexcept = default;
    ChainState& operator=(const ChainState&) noexcept = default;
    ~ChainState();
};

// The holder. Move-only; the secret seed is erased when the chain is
// destroyed or moved from. One thread at a time.
class Chain final {
public:
    // A new chain from the host's entropy; `now` is its genesis. Failures:
    // fill_entropy's (entropy.hpp); out_of_memory.
    [[nodiscard]] static Result<Chain> create(WallTime now) noexcept;
    // A chain from a 32-byte secret seed and a public seed, for tests and
    // for callers with their own entropy. Failure: out_of_memory.
    [[nodiscard]] static Result<Chain> from_seeds(const hash::Digest& secret_seed, const hash::Digest& public_seed,
                                                  WallTime now) noexcept;
    // A chain continued from saved state. Failures: invalid_argument when
    // the state is inconsistent (a pending count above infos_per_epoch);
    // out_of_memory.
    [[nodiscard]] static Result<Chain> restore(const ChainState& state) noexcept;

    Chain(Chain&& other) noexcept;
    Chain& operator=(Chain&& other) noexcept;
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;
    ~Chain();

    // Borrowed from this chain.
    [[nodiscard]] const Anchor& anchor() const& noexcept { return anchor_; }
    const Anchor& anchor() const&& = delete;
    // The open epoch, which record writes to.
    [[nodiscard]] u64 epoch() const noexcept { return epoch_; }
    [[nodiscard]] PrecedenceStatus record(const hash::Digest& info) noexcept;
    // Closes the open epoch into `certificate` and opens the next one.
    // `now` earlier than the previous epoch's time is recorded as that time.
    // A block's last epoch also derives the next block's keys (256 key
    // derivations). Returns ok or exhausted.
    [[nodiscard]] PrecedenceStatus advance(WallTime now, Certificate& certificate) noexcept;
    // Copies the state for secure storage.
    void save(ChainState& state) const noexcept;

    // A block's keys and its key tree, in one buffer: the 256 public keys,
    // then the tree level by level (level 0 the 256 key leaves, level 8 the
    // root): 256 + 511 digests.
    static constexpr usize block_digests = epochs_per_block + 2 * epochs_per_block - 1;

private:
    Chain() noexcept = default;
    [[nodiscard]] static Result<Chain> with_storage() noexcept;
    void erase_secret() noexcept;

    Anchor anchor_{};
    // The open epoch's seed; erased on every ratchet and on destruction.
    hash::Digest seed_{};
    u64 epoch_{0};
    WallTime last_time_{};
    WallTime block_first_time_{};
    hash::Digest previous_leaf_{};
    hash::Digest chain_digest_{};
    // The current block's keys and tree (public data; regenerated only at a
    // block boundary, from the seed then current), and the next block's,
    // derived on a block's last epoch and then swapped in.
    Buffer<hash::Digest> block_{};
    Buffer<hash::Digest> next_block_{};
    hash::Digest pending_[infos_per_epoch]{};
    u32 pending_count_{0};
};

// A verifier's trusted key roots, starting from the anchor and extended by
// each verified block-final certificate. Verification is a WOTS+ check
// (about 500 hashes) and an 8-hash key path. Move-only (it owns its list).
class Verifier final {
public:
    explicit Verifier(const Anchor& anchor) noexcept : anchor_(anchor) {}

    // A moved-from verifier knows the anchor's block only (its later roots
    // went with the move), like every other move-only type here that is
    // empty after a move (security audit 2026-11-15,
    // A-stxlib-include-01-F6).
    Verifier(Verifier&& other) noexcept
        : anchor_(other.anchor_), later_roots_(static_cast<Buffer<hash::Digest>&&>(other.later_roots_)),
          later_count_(other.later_count_) {
        other.later_count_ = 0;
    }
    Verifier& operator=(Verifier&& other) noexcept {
        if (this == &other) return *this;
        const usize count = other.later_count_;
        other.later_count_ = 0;
        anchor_ = other.anchor_;
        later_roots_ = static_cast<Buffer<hash::Digest>&&>(other.later_roots_);
        later_count_ = count;
        return *this;
    }
    Verifier(const Verifier&) = delete;
    Verifier& operator=(const Verifier&) = delete;
    ~Verifier() = default;

    // ok, invalid_certificate, unknown_block or out_of_memory. A verified
    // block-final certificate teaches the verifier the next block's key root.
    [[nodiscard]] PrecedenceStatus verify(const Certificate& certificate) noexcept;
    [[nodiscard]] usize known_blocks() const noexcept { return 1 + later_count_; }

private:
    // The key root of a known block. Requirement: block < known_blocks().
    [[nodiscard]] const hash::Digest& key_root(usize block) const noexcept;

    Anchor anchor_{};
    // Key roots of blocks 1, 2, ...: later_roots_[0, later_count_), the
    // buffer doubling as it fills.
    Buffer<hash::Digest> later_roots_{};
    usize later_count_{0};
};

// ok when `info` is among the certificate's recorded infos, else
// not_recorded (also for an info_count above infos_per_epoch).
[[nodiscard]] PrecedenceStatus recorded_in(const Certificate& certificate, const hash::Digest& info) noexcept;

// For two certificates the verifier has accepted: true when `earlier`'s
// epoch precedes `later`'s; `offset` receives later.epoch - earlier.epoch
// (0 when false).
[[nodiscard]] bool precedes(const Certificate& earlier, const Certificate& later, u64& offset) noexcept;

// True when `later` immediately follows `earlier` in the same chain: its
// epoch is the next, its previous_leaf is earlier's leaf, and its time is
// not earlier. False for an `earlier` whose info_count is above
// infos_per_epoch.
[[nodiscard]] bool follows_directly(const Anchor& anchor, const Certificate& earlier,
                                    const Certificate& later) noexcept;

} // namespace stxlib::precedence
