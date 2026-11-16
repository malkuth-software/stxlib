// stxpkg.cpp: the program store's tool (C2: signed program packages, A/B
// per program, the downloader and the system updater;
// appliance/C2_DESIGN.md, "Packages", "Programs", "Updates").
//
// One binary, so that the appliance carries one program linked with the
// crypto library rather than several (hardening is subtraction):
//
//   stxpkg run <program> <slot> -- <file> [argument...]
//       As the target (the supervisor starts it so, system_actions.cpp):
//       verifies the slot's manifest signature and every file's size, mode,
//       owner and SHA-256, then executes <file> of the package. Exit
//       package_refused_exit (125) when anything does not check out; the
//       registry does not restart a target that exits so.
//   stxpkg install <program> <slot> <package-file|->
//       As root: verifies the package's signature and manifest, writes its
//       files into the program's incoming/ checking each hash as it
//       arrives, and replaces the slot by a rename. The slot must be the
//       one program_slots.h's install_slot names for the stored state; the
//       version must be newer than the good slot's.
//   stxpkg update <program> <slot> <latest-url>
//       As root: fetches the latest document and, when it names a version
//       newer than the good slot's and not the one that last fell back,
//       fetches that package and installs it as above, streaming. Each
//       fetch runs in a child that has dropped to stxnet::fetch_identity.
//   stxpkg system-install <slot> <package-file|->
//   stxpkg system-update <slot> <latest-url>
//       As root: the same for the system image (package "system", one file
//       root.img), written raw to the inactive root slot's device
//       (/etc/stx/root-devices), read back and compared, then its UFS label
//       set to the slot's (STXROOTA or STXROOTB) with the base system's
//       libufs. The supervisor then arms the slot and reboots.
//   stxpkg status <program>
//       Prints the stored state and each slot's version.
//
// Exit status: 0 done; 2 usage; 3 not newer; 4 refused (signature,
// manifest, hash, name, version, slot, trailing bytes); 5 input or output
// failure; 6 nothing newer (update); 7 fetch failed; 125 run refused.
// Every refusal prints one line "stxpkg <command> <reason>" on stderr.
//
// The supervisor is the only writer of a program's state file; stxpkg
// reads it and writes slots.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include <dirent.h>
#include <fcntl.h>
#include <fts.h>
#include <grp.h>
#include <signal.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>
#include <libufs.h>

#include <openssl/evp.h>

#include "../net/fetch.h"
#include "../supervisor/config/boot_config.h"
#include "../supervisor/config/service_registry.h"
#include "container.h"
#include "latest.h"
#include "manifest.h"
#include "program_slots.h"
#include "store_layout.h"
#include "stxpkg_exit.h"

extern char** environ;

namespace {

using stxpackage::Manifest;
using stxpackage::ManifestFile;
using stxpackage::Slot;
using stxpackage::StorePath;

using stxpackage::exit_done;
using stxpackage::exit_fetch_failed;
using stxpackage::exit_io;
using stxpackage::exit_not_newer;
using stxpackage::exit_nothing_newer;
using stxpackage::exit_refused;
using stxpackage::exit_run_refused;
using stxpackage::exit_usage;
// The whole of one fetch, after which the child is killed (SIGALRM).
constexpr unsigned fetch_seconds_max = 1800;
constexpr std::size_t io_chunk_bytes = 65536;
constexpr int interrupted_retries_max = 16;
constexpr const char* system_image_file = "root.img";

const char* command_name = "stxpkg";

void say(const char* reason, const char* detail) noexcept {
    char line[512] = {};
    std::size_t used = 0;
    const char* const parts[] = {"stxpkg ", command_name, " ", reason, detail[0] != '\0' ? " " : "", detail, "\n"};
    for (const char* const part : parts) {
        for (std::size_t index = 0; part[index] != '\0' && used + 1u < sizeof(line); index += 1) {
            line[used] = part[index];
            used += 1;
        }
    }
    (void)write(2, line, used);
}

[[nodiscard]] std::size_t text_length(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length;
}

[[nodiscard]] bool same(const char* left, const char* right) noexcept {
    return std::strcmp(left, right) == 0;
}

// A configuration name: what a program in the store may be called.
[[nodiscard]] bool program_name_valid(const char* name) noexcept {
    const std::size_t length = text_length(name);
    if (length == 0u || length > stxpackage::name_length_max || name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        const char byte = name[index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<Slot> slot_argument(const char* text) noexcept {
    if (text_length(text) != 1u) {
        return std::nullopt;
    }
    return stxpackage::slot_of_letter(text[0]);
}

[[nodiscard]] std::optional<StorePath> program_path(const char* program, const char* part,
                                                    const char* leaf) noexcept {
    const char* parts[3] = {program, part, leaf};
    std::size_t count = 1;
    if (part != nullptr) {
        count = 2;
        if (leaf != nullptr) {
            count = 3;
        }
    }
    return stxpackage::store_path(parts, count);
}

// ---------------------------------------------------------------------------
// Files.

// Reads the whole regular file at path (at most buffer.size() bytes, and
// refused when larger). The length, or nothing.
[[nodiscard]] std::optional<std::size_t> read_small_file(const char* path, std::span<char> buffer) noexcept {
    const int file = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        return std::nullopt;
    }
    struct stat status{};
    if (fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > buffer.size()) {
        (void)close(file);
        return std::nullopt;
    }
    std::size_t total = 0;
    int interrupted = 0;
    while (total < buffer.size()) {
        const ssize_t got = read(file, &buffer[total], buffer.size() - total);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
            interrupted += 1;
            continue;
        }
        if (got < 0) {
            (void)close(file);
            return std::nullopt;
        }
        break;
    }
    (void)close(file);
    if (total != static_cast<std::size_t>(status.st_size)) {
        return std::nullopt;
    }
    return total;
}

[[nodiscard]] bool write_all(int descriptor, const char* bytes, std::size_t length) noexcept {
    std::size_t written = 0;
    int interrupted = 0;
    while (written < length) {
        const ssize_t put = write(descriptor, &bytes[written], length - written);
        if (put > 0) {
            written += static_cast<std::size_t>(put);
            continue;
        }
        if (put < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
            interrupted += 1;
            continue;
        }
        return false;
    }
    return true;
}

// Creates a file with exactly bytes, fsync, then mode. False on failure.
[[nodiscard]] bool write_new_file(const char* path, std::span<const char> bytes, mode_t mode) noexcept {
    const int file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
        return false;
    }
    const bool written = write_all(file, bytes.data(), bytes.size()) && fsync(file) == 0 && fchmod(file, mode) == 0;
    (void)close(file);
    return written;
}

[[nodiscard]] bool sync_directory(const char* path) noexcept {
    const int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return false;
    }
    const bool synced = fsync(directory) == 0;
    (void)close(directory);
    return synced;
}

// mkdir path 0755, or accept an existing directory (not a link).
[[nodiscard]] bool ensure_directory(const char* path) noexcept {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    struct stat status{};
    return lstat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

// Removes the tree at path (absent is fine). fts(3) walks it without
// recursion of ours and without following links.
[[nodiscard]] bool remove_tree(const char* path) noexcept {
    struct stat status{};
    if (lstat(path, &status) != 0) {
        return errno == ENOENT;
    }
    char* const roots[] = {const_cast<char*>(path), nullptr};
    FTS* const walk = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr);
    if (walk == nullptr) {
        return false;
    }
    bool removed = true;
    FTSENT* entry = fts_read(walk);
    while (entry != nullptr) {
        switch ((*entry).fts_info) {
        case FTS_DP:
            removed = removed && rmdir((*entry).fts_accpath) == 0;
            break;
        case FTS_D:
            break;
        case FTS_DNR:
        case FTS_ERR:
        case FTS_NS:
            removed = false;
            break;
        default:
            removed = removed && unlink((*entry).fts_accpath) == 0;
            break;
        }
        entry = fts_read(walk);
    }
    (void)fts_close(walk);
    return removed;
}

// ---------------------------------------------------------------------------
// Crypto: SHA-256 and Ed25519 through the base system's libcrypto.

class Sha256 {
public:
    Sha256() noexcept : context_(EVP_MD_CTX_new()), ok_(false) {
        ok_ = context_ != nullptr && EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) == 1;
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    ~Sha256() {
        if (context_ != nullptr) {
            EVP_MD_CTX_free(context_);
        }
    }
    void add(const char* bytes, std::size_t length) noexcept {
        ok_ = ok_ && EVP_DigestUpdate(context_, bytes, length) == 1;
    }
    // The digest into out; false when any step failed.
    [[nodiscard]] bool finish(std::uint8_t (&out)[stxpackage::sha256_size]) noexcept {
        unsigned int length = 0;
        ok_ = ok_ && EVP_DigestFinal_ex(context_, out, &length) == 1 && length == stxpackage::sha256_size;
        return ok_;
    }

private:
    EVP_MD_CTX* context_;
    bool ok_;
};

struct TrustKeys {
    std::uint8_t keys[stxpackage::trust_keys_max][stxpackage::ed25519_public_key_size];
    std::size_t count;
};

// The keys in /etc/stx/keys: files named *.pub, each 64 lower-case hex
// digits and a LF. Unreadable or malformed files are skipped (and said);
// at most trust_keys_max.
[[nodiscard]] TrustKeys load_keys() noexcept {
    TrustKeys trust{};
    trust.count = 0;
    DIR* const directory = opendir(stxpackage::trust_keys_directory);
    if (directory == nullptr) {
        return trust;
    }
    struct dirent* entry = readdir(directory);
    while (entry != nullptr && trust.count < stxpackage::trust_keys_max) {
        const char* const name = (*entry).d_name;
        const std::size_t length = text_length(name);
        if (length > 4u && same(&name[length - 4u], ".pub")) {
            const char* parts[] = {nullptr, stxpackage::trust_keys_directory, name};
            const std::optional<StorePath> path = stxpackage::store_path(parts, 3);
            char text[80] = {};
            std::optional<std::size_t> read;
            if (path.has_value()) {
                read = read_small_file((*path).text, std::span<char>(text, sizeof(text)));
            }
            std::uint8_t key[32] = {};
            if (read.has_value() && *read == 65u && text[64] == '\n' && stxpackage::sha256_of_hex(text, 64, key)) {
                std::memcpy(trust.keys[trust.count], key, sizeof(key));
                trust.count += 1;
            } else {
                say("key-skipped", name);
            }
        }
        entry = readdir(directory);
    }
    (void)closedir(directory);
    return trust;
}

// Whether signature is a valid Ed25519 signature by one of the keys over
// the prefix followed by manifest.
[[nodiscard]] bool signature_valid(const TrustKeys& trust, std::span<const char> manifest,
                                   const char (&signature_text)[stxpackage::signature_size]) noexcept {
    static unsigned char message[stxpackage::signed_message_prefix_length + stxpackage::manifest_size_max];
    unsigned char signature_bytes[stxpackage::signature_size] = {};
    std::memcpy(signature_bytes, signature_text, stxpackage::signature_size);
    if (manifest.size() > stxpackage::manifest_size_max) {
        return false;
    }
    std::memcpy(message, stxpackage::signed_message_prefix, stxpackage::signed_message_prefix_length);
    std::memcpy(&message[stxpackage::signed_message_prefix_length], manifest.data(), manifest.size());
    const std::size_t message_length = stxpackage::signed_message_prefix_length + manifest.size();
    for (std::size_t index = 0; index < trust.count; index += 1) {
        EVP_PKEY* const key =
            EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, trust.keys[index], stxpackage::ed25519_public_key_size);
        EVP_MD_CTX* const context = EVP_MD_CTX_new();
        bool valid = false;
        if (key != nullptr && context != nullptr && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1) {
            valid = EVP_DigestVerify(context, signature_bytes, stxpackage::signature_size, message, message_length) == 1;
        }
        if (context != nullptr) {
            EVP_MD_CTX_free(context);
        }
        if (key != nullptr) {
            EVP_PKEY_free(key);
        }
        if (valid) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The store's state, read only.

// The manifest of a verified or installed slot: its text and parse.
char manifest_text[stxpackage::manifest_size_max];
std::size_t manifest_length = 0;
Manifest manifest;
char signature[stxpackage::signature_size];

struct StoredFacts {
    stxpackage::StoredState stored;
    // The good slot's version, 0 when none.
    std::uint32_t good_version;
};

// A slot's installed version from its manifest (not re-verified: root's
// files), or 0.
[[nodiscard]] std::uint32_t installed_version(const char* program, Slot slot) noexcept {
    const char letter[2] = {stxpackage::slot_letter(slot), '\0'};
    const std::optional<StorePath> path = program_path(program, letter, "manifest");
    static char text[stxpackage::manifest_size_max];
    static Manifest parsed;
    if (!path.has_value()) {
        return 0;
    }
    const std::optional<std::size_t> read = read_small_file((*path).text, std::span<char>(text, sizeof(text)));
    if (!read.has_value() || stxpackage::parse_manifest(std::span<const char>(text, *read), parsed).has_value()) {
        return 0;
    }
    return parsed.version;
}

// The program's stored state; none (and refused 0) when it has no state
// file. Nothing when the file exists but is unreadable or malformed.
[[nodiscard]] std::optional<StoredFacts> read_facts(const char* program) noexcept {
    StoredFacts facts{stxpackage::StoredState{stxpackage::ProgramState::none(), 0}, 0};
    const std::optional<StorePath> path = program_path(program, "state", nullptr);
    if (!path.has_value()) {
        return std::nullopt;
    }
    char text[stxpackage::state_file_size_max] = {};
    struct stat status{};
    if (lstat((*path).text, &status) != 0 && errno == ENOENT) {
        const std::optional<StoredFacts> none = facts;
        return none;
    }
    const std::optional<std::size_t> read = read_small_file((*path).text, std::span<char>(text, sizeof(text)));
    if (!read.has_value()) {
        return std::nullopt;
    }
    const std::optional<stxpackage::StoredState> stored =
        stxpackage::parse_program_state(std::span<const char>(text, *read));
    if (!stored.has_value()) {
        return std::nullopt;
    }
    facts.stored = *stored;
    const std::optional<Slot> run = stxpackage::slot_to_run((*stored).state);
    if (run.has_value()) {
        // The version that runs when no trial does: the good slot's.
        const stxpackage::GoodWithTrial* const trial = (*stored).state.as_good_with_trial();
        const Slot good = trial != nullptr ? (*trial).good : *run;
        facts.good_version = installed_version(program, good);
    }
    const std::optional<StoredFacts> result = facts;
    return result;
}

// ---------------------------------------------------------------------------
// A package read from a descriptor.

// One per process (its buffer is large, so it is static storage, not
// stack): open() points it at the package's descriptor.
class PackageReader {
public:
    PackageReader() noexcept : descriptor_(-1), buffer_{}, start_(0), end_(0), failed_(false) {}
    PackageReader(const PackageReader&) = delete;
    PackageReader& operator=(const PackageReader&) = delete;

    void open(int descriptor) noexcept {
        descriptor_ = descriptor;
        start_ = 0;
        end_ = 0;
        failed_ = false;
    }

    // Reads until the buffer holds at least wanted bytes or the stream
    // ends; the bytes held.
    std::size_t fill(std::size_t wanted) noexcept {
        if (start_ > 0u && end_ - start_ < wanted) {
            std::memmove(buffer_, &buffer_[start_], end_ - start_);
            end_ -= start_;
            start_ = 0;
        }
        const std::size_t target = wanted < sizeof(buffer_) ? wanted : sizeof(buffer_);
        int interrupted = 0;
        while (end_ - start_ < target) {
            const ssize_t got = read(descriptor_, &buffer_[end_], sizeof(buffer_) - end_);
            if (got > 0) {
                end_ += static_cast<std::size_t>(got);
                continue;
            }
            if (got < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
                interrupted += 1;
                continue;
            }
            failed_ = failed_ || got < 0;
            break;
        }
        return end_ - start_;
    }
    [[nodiscard]] std::span<const char> held() const noexcept {
        return std::span<const char>(&buffer_[start_], end_ - start_);
    }
    void consume(std::size_t count) noexcept {
        start_ += count < end_ - start_ ? count : end_ - start_;
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    // Whether the stream holds no further byte.
    [[nodiscard]] bool at_end() noexcept {
        return fill(1) == 0u && !failed_;
    }

private:
    int descriptor_;
    char buffer_[stxpackage::container_head_max + io_chunk_bytes];
    std::size_t start_;
    std::size_t end_;
    bool failed_;
};

PackageReader package_reader;

// Reads and checks the package's head: the manifest's signature by a trust
// key and the manifest itself, into manifest_text, manifest and signature.
// exit_done, or the exit status of the refusal (said).
[[nodiscard]] int read_package_head(PackageReader& reader, const TrustKeys& trust) noexcept {
    std::size_t wanted = 64;
    stxpackage::ContainerOutcome head = stxpackage::ContainerOutcome::refused(stxpackage::ContainerRefusal::truncated);
    // Each pass reads more or ends: the head is at most container_head_max.
    while (true) {
        const std::size_t held = reader.fill(wanted);
        head = stxpackage::parse_container_head(reader.held());
        const stxpackage::ContainerRefusal* const refusal = head.as_refused();
        if (refusal == nullptr || *refusal != stxpackage::ContainerRefusal::truncated) {
            break;
        }
        if (held < wanted || wanted >= stxpackage::container_head_max) {
            say("refused", reader.failed() ? "read-failed" : "truncated");
            return reader.failed() ? exit_io : exit_refused;
        }
        wanted = wanted * 2u < stxpackage::container_head_max ? wanted * 2u : stxpackage::container_head_max;
    }
    const stxpackage::ContainerHead* const parsed = head.as_head();
    if (parsed == nullptr) {
        say("refused", stxpackage::container_refusal_name(*head.as_refused()));
        return exit_refused;
    }
    const std::span<const char> bytes = reader.held();
    std::memcpy(manifest_text, &bytes[(*parsed).manifest_offset], (*parsed).manifest_length);
    manifest_length = (*parsed).manifest_length;
    std::memcpy(signature, &bytes[(*parsed).signature_offset], stxpackage::signature_size);
    reader.consume((*parsed).data_offset);
    const std::span<const char> text(manifest_text, manifest_length);
    if (!signature_valid(trust, text, signature)) {
        say("refused", "signature");
        return exit_refused;
    }
    const std::optional<stxpackage::ManifestRefused> refused = stxpackage::parse_manifest(text, manifest);
    if (refused.has_value()) {
        say("refused", stxpackage::manifest_error_name((*refused).error));
        return exit_refused;
    }
    return exit_done;
}

// A system image must not carry a root slot's UFS label while it is being
// written: from the moment its superblock is whole on the inactive slot
// until relabel() runs, the unit would have two file systems of one label,
// and the kernel's root name (ufs:/dev/ufs/STXROOTA) could find either, on
// this boot's retaste or after a power loss (C2_DESIGN.md, "Early exits",
// system update). Images therefore carry a neutral label (the build gives
// them STXROOTNEW), and the copy refuses an image whose standard UFS2
// superblock names a slot, before writing the bytes that complete that
// superblock's magic (the label lies before the magic in struct fs, so it
// has been seen by then). An image is ours and signed: this guards a build
// mistake, not an attack.
constexpr std::uint64_t image_label_offset = SBLOCK_UFS2 + offsetof(struct fs, fs_volname);
constexpr std::uint64_t image_magic_end = SBLOCK_UFS2 + offsetof(struct fs, fs_magic) + sizeof(std::int32_t);
static_assert(image_label_offset + MAXVOLLEN <= image_magic_end, "the label precedes the magic");

struct ImageLabelGuard {
    char label[MAXVOLLEN];
    bool checked;
};

// Captures the label's bytes found in [chunk_offset, chunk_offset +
// length); false when this chunk completes the superblock's magic and the
// label seen is a root slot's.
[[nodiscard]] bool image_label_safe(ImageLabelGuard& guard, std::uint64_t chunk_offset, const char* bytes,
                                    std::size_t length) noexcept {
    for (std::size_t index = 0; index < MAXVOLLEN; index += 1) {
        const std::uint64_t at = image_label_offset + index;
        if (at >= chunk_offset && at < chunk_offset + length) {
            guard.label[index] = bytes[at - chunk_offset];
        }
    }
    if (guard.checked || chunk_offset + length < image_magic_end) {
        return true;
    }
    guard.checked = true;
    return std::strncmp(guard.label, "STXROOTA", MAXVOLLEN) != 0 && std::strncmp(guard.label, "STXROOTB", MAXVOLLEN) != 0;
}

// Copies size bytes from the reader to output (a descriptor, or -1 for
// none), hashing them; the digest into digest. With a guard (system images
// only), refuses as image_label_safe says before writing the chunk.
// exit_done or a failure.
[[nodiscard]] int copy_hashed(PackageReader& reader, std::uint64_t size, int output,
                              std::uint8_t (&digest)[stxpackage::sha256_size], ImageLabelGuard* guard) noexcept {
    Sha256 hash;
    std::uint64_t remaining = size;
    while (remaining > 0u) {
        const std::size_t want = remaining < io_chunk_bytes ? static_cast<std::size_t>(remaining) : io_chunk_bytes;
        const std::size_t held = reader.fill(want);
        if (held == 0u) {
            say("refused", reader.failed() ? "read-failed" : "truncated");
            return reader.failed() ? exit_io : exit_refused;
        }
        const std::size_t take = held < want ? held : want;
        const std::span<const char> bytes = reader.held();
        hash.add(bytes.data(), take);
        if (guard != nullptr && !image_label_safe(*guard, size - remaining, bytes.data(), take)) {
            say("refused", "image-carries-a-slot-label");
            return exit_refused;
        }
        if (output >= 0 && !write_all(output, bytes.data(), take)) {
            say("write-failed", "");
            return exit_io;
        }
        reader.consume(take);
        remaining -= take;
    }
    if (!hash.finish(digest)) {
        say("hash-failed", "");
        return exit_io;
    }
    return exit_done;
}

[[nodiscard]] bool digests_equal(const std::uint8_t (&left)[stxpackage::sha256_size],
                                 const std::uint8_t (&right)[stxpackage::sha256_size]) noexcept {
    return std::memcmp(left, right, stxpackage::sha256_size) == 0;
}

// Creates the directories of a package path below base (base exists).
[[nodiscard]] bool make_parents(const char* base, const char* path, std::size_t length) noexcept {
    char directory[stxpackage::store_path_max + 1u] = {};
    const std::size_t base_length = text_length(base);
    if (base_length + 1u + length > stxpackage::store_path_max) {
        return false;
    }
    std::memcpy(directory, base, base_length);
    directory[base_length] = '/';
    for (std::size_t index = 0; index < length; index += 1) {
        if (path[index] == '/') {
            directory[base_length + 1u + index] = '\0';
            if (!ensure_directory(directory)) {
                return false;
            }
        }
        directory[base_length + 1u + index] = path[index];
    }
    return true;
}

// The files of a verified head, streamed into incoming/files, each hash
// checked; then the manifest and signature beside them.
[[nodiscard]] int write_incoming(PackageReader& reader, const char* program) noexcept {
    const std::optional<StorePath> incoming = program_path(program, "incoming", nullptr);
    const std::optional<StorePath> files = program_path(program, "incoming", "files");
    if (!incoming.has_value() || !files.has_value() || !remove_tree((*incoming).text) ||
        mkdir((*incoming).text, 0755) != 0 || mkdir((*files).text, 0755) != 0) {
        say("store-failed", "incoming");
        return exit_io;
    }
    for (std::uint32_t index = 0; index < manifest.file_count; index += 1) {
        const ManifestFile& file = manifest.files[index];
        const char* const path = &manifest_text[file.path_offset];
        char relative[stxconfig::package_path_length_max + 1u] = {};
        std::memcpy(relative, path, file.path_length);
        const std::optional<StorePath> target = program_path(program, "incoming/files", relative);
        if (!target.has_value() || !make_parents((*files).text, relative, file.path_length)) {
            say("store-failed", relative);
            return exit_io;
        }
        const int output = open((*target).text, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (output < 0) {
            say("store-failed", relative);
            return exit_io;
        }
        std::uint8_t digest[stxpackage::sha256_size] = {};
        const int copied = copy_hashed(reader, file.size, output, digest, nullptr);
        const bool closed = copied == exit_done && fsync(output) == 0 &&
                            fchmod(output, static_cast<mode_t>(file.mode)) == 0;
        (void)close(output);
        if (copied != exit_done) {
            return copied;
        }
        if (!closed) {
            say("store-failed", relative);
            return exit_io;
        }
        if (!digests_equal(digest, file.sha256)) {
            say("refused", "hash");
            return exit_refused;
        }
    }
    const std::optional<StorePath> manifest_path = program_path(program, "incoming", "manifest");
    const std::optional<StorePath> signature_path = program_path(program, "incoming", "signature");
    if (!manifest_path.has_value() || !signature_path.has_value() ||
        !write_new_file((*manifest_path).text, std::span<const char>(manifest_text, manifest_length), 0444) ||
        !write_new_file((*signature_path).text,
                        std::span<const char>(signature, sizeof(signature)), 0444) ||
        !sync_directory((*files).text) || !sync_directory((*incoming).text)) {
        say("store-failed", "manifest");
        return exit_io;
    }
    return exit_done;
}

// incoming/ becomes the slot: the old slot (if any) moves to discard/
// first and is removed after.
[[nodiscard]] int commit_incoming(const char* program, Slot slot) noexcept {
    const char letter[2] = {stxpackage::slot_letter(slot), '\0'};
    const std::optional<StorePath> directory = program_path(program, nullptr, nullptr);
    const std::optional<StorePath> incoming = program_path(program, "incoming", nullptr);
    const std::optional<StorePath> discard = program_path(program, "discard", nullptr);
    const std::optional<StorePath> slot_path = program_path(program, letter, nullptr);
    if (!directory.has_value() || !incoming.has_value() || !discard.has_value() || !slot_path.has_value() ||
        !remove_tree((*discard).text)) {
        say("store-failed", "discard");
        return exit_io;
    }
    struct stat status{};
    if (lstat((*slot_path).text, &status) == 0 && rename((*slot_path).text, (*discard).text) != 0) {
        say("store-failed", "rename-old");
        return exit_io;
    }
    if (rename((*incoming).text, (*slot_path).text) != 0 || !sync_directory((*directory).text)) {
        say("store-failed", "rename-new");
        return exit_io;
    }
    if (!remove_tree((*discard).text)) {
        say("store-left", "discard");
    }
    return exit_done;
}

// ---------------------------------------------------------------------------
// install and update.

// Installs the package on reader into program's slot. expected_version:
// the version a latest document named (update), or 0.
[[nodiscard]] int install_package(PackageReader& reader, const char* program, Slot slot,
                                  std::uint32_t expected_version) noexcept {
    const std::optional<StoredFacts> facts = read_facts(program);
    if (!facts.has_value()) {
        say("refused", "state-unreadable");
        return exit_refused;
    }
    if (stxpackage::install_slot((*facts).stored.state) != slot) {
        say("refused", "slot");
        return exit_refused;
    }
    const TrustKeys trust = load_keys();
    const int head = read_package_head(reader, trust);
    if (head != exit_done) {
        return head;
    }
    const std::span<const char> text(manifest_text, manifest_length);
    if (!stxpackage::manifest_name_is(text, manifest, program)) {
        say("refused", "name");
        return exit_refused;
    }
    if (expected_version != 0u && manifest.version != expected_version) {
        say("refused", "version-differs-from-latest");
        return exit_refused;
    }
    if (manifest.version <= (*facts).good_version) {
        say("not-newer", "");
        return exit_not_newer;
    }
    const std::optional<StorePath> root = program_path(program, nullptr, nullptr);
    const char* const programs[] = {nullptr, stxpackage::programs_root};
    const std::optional<StorePath> programs_directory = stxpackage::store_path(programs, 2);
    if (!root.has_value() || !programs_directory.has_value() || !ensure_directory((*programs_directory).text) ||
        !ensure_directory((*root).text)) {
        say("store-failed", "program-directory");
        return exit_io;
    }
    const int written = write_incoming(reader, program);
    if (written != exit_done) {
        return written;
    }
    if (!reader.at_end()) {
        say("refused", reader.failed() ? "read-failed" : "trailing-bytes");
        return reader.failed() ? exit_io : exit_refused;
    }
    return exit_done;
}

// A fetch in a child as fetch_identity, writing the body to a pipe. The
// child's pid and the pipe's read end; false when it could not start.
[[nodiscard]] bool start_fetch(const char* url, std::uint64_t body_max, pid_t& child, int& from_child) noexcept {
    int ends[2] = {-1, -1};
    if (pipe2(ends, O_CLOEXEC) != 0) {
        return false;
    }
    child = fork();
    if (child < 0) {
        (void)close(ends[0]);
        (void)close(ends[1]);
        return false;
    }
    if (child == 0) {
        (void)close(ends[0]);
        const gid_t group = static_cast<gid_t>(stxnet::fetch_identity);
        if (setgroups(1, &group) != 0 || setgid(group) != 0 || setuid(static_cast<uid_t>(stxnet::fetch_identity)) != 0) {
            _exit(exit_fetch_failed);
        }
        (void)alarm(fetch_seconds_max);
        const stxnet::FetchResult result =
            stxnet::fetch_https(url, body_max, ends[1], 2, stxpackage::ca_bundle_path);
        _exit(result == stxnet::FetchResult::done ? exit_done : exit_fetch_failed);
    }
    (void)close(ends[1]);
    from_child = ends[0];
    return true;
}

// Waits for the fetch child; whether it exited 0.
[[nodiscard]] bool fetch_succeeded(pid_t child) noexcept {
    int status = 0;
    int interrupted = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR || interrupted >= interrupted_retries_max) {
            return false;
        }
        interrupted += 1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Ends a fetch early: closes the pipe and kills and reaps the child.
void abandon_fetch(pid_t child, int from_child) noexcept {
    (void)close(from_child);
    (void)kill(child, SIGKILL);
    (void)fetch_succeeded(child);
}

// Fetches and parses a latest document.
[[nodiscard]] std::optional<stxpackage::Latest> fetch_latest(const char* url) noexcept {
    pid_t child = -1;
    int from_child = -1;
    if (!start_fetch(url, stxpackage::latest_size_max, child, from_child)) {
        say("fetch-failed", "start");
        return std::nullopt;
    }
    char text[stxpackage::latest_size_max + 1u] = {};
    std::size_t total = 0;
    int interrupted = 0;
    while (total < sizeof(text)) {
        const ssize_t got = read(from_child, &text[total], sizeof(text) - total);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
            interrupted += 1;
            continue;
        }
        break;
    }
    (void)close(from_child);
    if (!fetch_succeeded(child)) {
        say("fetch-failed", "latest");
        return std::nullopt;
    }
    const stxpackage::LatestOutcome latest = stxpackage::parse_latest(std::span<const char>(text, total));
    if (latest.as_parsed() == nullptr) {
        say("refused", stxpackage::latest_refusal_name(*latest.as_refused()));
        return std::nullopt;
    }
    const std::optional<stxpackage::Latest> result = *latest.as_parsed();
    return result;
}

// The URL as NUL-terminated text (for the child).
void url_text(const stxnet::HttpsUrl& url, char (&out)[stxnet::url_length_max + 32u]) noexcept {
    char port[8] = {};
    std::size_t port_used = 0;
    if (url.port != stxnet::https_default_port) {
        char digits[6] = {};
        std::size_t count = 0;
        std::uint32_t rest = url.port;
        while (rest > 0u && count < sizeof(digits)) {
            digits[count] = static_cast<char>('0' + static_cast<char>(rest % 10u));
            rest /= 10u;
            count += 1;
        }
        port[0] = ':';
        port_used = 1;
        while (count > 0u) {
            count -= 1;
            port[port_used] = digits[count];
            port_used += 1;
        }
    }
    std::size_t used = 0;
    const char* const parts[] = {"https://", url.host, port, url.path};
    for (const char* const part : parts) {
        for (std::size_t index = 0; part[index] != '\0' && used + 1u < sizeof(out); index += 1) {
            out[used] = part[index];
            used += 1;
        }
    }
    out[used] = '\0';
}

[[nodiscard]] int command_install(const char* program, Slot slot, const char* source) noexcept {
    const int input = same(source, "-") ? 0 : open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        say("open-failed", source);
        return exit_io;
    }
    package_reader.open(input);
    int result = install_package(package_reader, program, slot, 0);
    if (result == exit_done) {
        result = commit_incoming(program, slot);
    }
    if (input != 0) {
        (void)close(input);
    }
    if (result == exit_done) {
        say("installed", program);
    }
    return result;
}

[[nodiscard]] int command_update(const char* program, Slot slot, const char* latest_url) noexcept {
    const std::optional<StoredFacts> facts = read_facts(program);
    if (!facts.has_value()) {
        say("refused", "state-unreadable");
        return exit_refused;
    }
    const std::optional<stxpackage::Latest> latest = fetch_latest(latest_url);
    if (!latest.has_value()) {
        return exit_fetch_failed;
    }
    if ((*latest).version <= (*facts).good_version || (*latest).version == (*facts).stored.refused_version) {
        say("nothing-newer", "");
        return exit_nothing_newer;
    }
    char package_url[stxnet::url_length_max + 32u] = {};
    url_text((*latest).url, package_url);
    pid_t child = -1;
    int from_child = -1;
    if (!start_fetch(package_url, stxpackage::container_head_max + stxpackage::package_size_max, child, from_child)) {
        say("fetch-failed", "start");
        return exit_fetch_failed;
    }
    package_reader.open(from_child);
    int result = install_package(package_reader, program, slot, (*latest).version);
    if (result != exit_done) {
        abandon_fetch(child, from_child);
        return result;
    }
    (void)close(from_child);
    if (!fetch_succeeded(child)) {
        say("fetch-failed", "package");
        return exit_fetch_failed;
    }
    result = commit_incoming(program, slot);
    if (result == exit_done) {
        say("installed", program);
    }
    return result;
}

// ---------------------------------------------------------------------------
// run.

// Verifies the installed slot: signature, name, and every file's size,
// mode, owner and hash. exit_done or exit_run_refused (said).
[[nodiscard]] int verify_slot(const char* program, Slot slot) noexcept {
    const char letter[2] = {stxpackage::slot_letter(slot), '\0'};
    const std::optional<StorePath> manifest_path = program_path(program, letter, "manifest");
    const std::optional<StorePath> signature_path = program_path(program, letter, "signature");
    if (!manifest_path.has_value() || !signature_path.has_value()) {
        say("refused", "path");
        return exit_run_refused;
    }
    const std::optional<std::size_t> manifest_read =
        read_small_file((*manifest_path).text, std::span<char>(manifest_text, sizeof(manifest_text)));
    char signature_bytes[stxpackage::signature_size + 1u] = {};
    const std::optional<std::size_t> signature_read =
        read_small_file((*signature_path).text, std::span<char>(signature_bytes, sizeof(signature_bytes)));
    if (!manifest_read.has_value() || !signature_read.has_value() || *signature_read != stxpackage::signature_size) {
        say("refused", "slot-unreadable");
        return exit_run_refused;
    }
    manifest_length = *manifest_read;
    std::memcpy(signature, signature_bytes, stxpackage::signature_size);
    const std::span<const char> text(manifest_text, manifest_length);
    if (!signature_valid(load_keys(), text, signature)) {
        say("refused", "signature");
        return exit_run_refused;
    }
    if (stxpackage::parse_manifest(text, manifest).has_value() ||
        !stxpackage::manifest_name_is(text, manifest, program)) {
        say("refused", "manifest");
        return exit_run_refused;
    }
    static char buffer[io_chunk_bytes];
    for (std::uint32_t index = 0; index < manifest.file_count; index += 1) {
        const ManifestFile& file = manifest.files[index];
        char relative[stxconfig::package_path_length_max + 1u] = {};
        std::memcpy(relative, &manifest_text[file.path_offset], file.path_length);
        char slot_files[8] = {letter[0], '/', 'f', 'i', 'l', 'e', 's', '\0'};
        const std::optional<StorePath> path = program_path(program, slot_files, relative);
        const int input = path.has_value() ? open((*path).text, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
        struct stat status{};
        if (input < 0 || fstat(input, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != 0 ||
            (status.st_mode & 07777u) != file.mode || static_cast<std::uint64_t>(status.st_size) != file.size) {
            if (input >= 0) {
                (void)close(input);
            }
            say("refused", relative);
            return exit_run_refused;
        }
        Sha256 hash;
        std::uint64_t total = 0;
        int interrupted = 0;
        // Each pass reads at least one byte or ends; the file's size is
        // known, and EINTR retries are bounded.
        bool reading = true;
        while (reading) {
            const ssize_t got = read(input, buffer, sizeof(buffer));
            if (got > 0) {
                hash.add(buffer, static_cast<std::size_t>(got));
                total += static_cast<std::uint64_t>(got);
            } else if (got < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
                interrupted += 1;
            } else {
                reading = false;
            }
        }
        (void)close(input);
        std::uint8_t digest[stxpackage::sha256_size] = {};
        if (total != file.size || !hash.finish(digest) || !digests_equal(digest, file.sha256)) {
            say("refused", relative);
            return exit_run_refused;
        }
    }
    return exit_done;
}

[[nodiscard]] int command_run(const char* program, Slot slot, int argument_count, char** arguments) noexcept {
    const int verified = verify_slot(program, slot);
    if (verified != exit_done) {
        return verified;
    }
    const char* const file = arguments[0];
    const std::size_t length = text_length(file);
    const std::optional<std::size_t> found =
        stxpackage::find_manifest_file(std::span<const char>(manifest_text, manifest_length), manifest, file, length);
    if (!found.has_value() || manifest.files[*found].mode != stxpackage::mode_executable) {
        say("refused", "not-an-executable-of-the-package");
        return exit_run_refused;
    }
    const char letter[2] = {stxpackage::slot_letter(slot), '\0'};
    char slot_files[8] = {letter[0], '/', 'f', 'i', 'l', 'e', 's', '\0'};
    static std::optional<StorePath> path;
    path = program_path(program, slot_files, file);
    if (!path.has_value()) {
        say("refused", "path");
        return exit_run_refused;
    }
    // argv: the file's absolute path, then the target's own arguments.
    static char* words[stxconfig::run_arguments_max + 1u] = {};
    words[0] = (*path).text;
    int count = 1;
    for (int index = 1; index < argument_count && count < static_cast<int>(stxconfig::run_arguments_max); index += 1) {
        words[count] = arguments[index];
        count += 1;
    }
    words[count] = nullptr;
    (void)execve((*path).text, words, environ);
    say("exec-failed", file);
    return exit_run_refused;
}

// ---------------------------------------------------------------------------
// The system image.

// The raw device of slot from /etc/stx/root-devices, into device.
[[nodiscard]] bool root_device(Slot slot, char (&device)[128]) noexcept {
    char text[256] = {};
    const std::optional<std::size_t> read =
        read_small_file(stxpackage::root_devices_path, std::span<char>(text, sizeof(text) - 1u));
    if (!read.has_value()) {
        return false;
    }
    const char wanted = slot == Slot::a ? 'A' : 'B';
    std::size_t begin = 0;
    while (begin < *read) {
        std::size_t end = begin;
        while (end < *read && text[end] != '\n') {
            end += 1;
        }
        if (end - begin > 3u && text[begin] == wanted && text[begin + 1u] == ' ' && text[begin + 2u] == '/' &&
            end - begin - 2u < sizeof(device)) {
            std::memcpy(device, &text[begin + 2u], end - begin - 2u);
            device[end - begin - 2u] = '\0';
            return true;
        }
        begin = end + 1u;
    }
    return false;
}

[[nodiscard]] std::uint32_t running_system_version() noexcept {
    char text[16] = {};
    const std::optional<std::size_t> read =
        read_small_file(stxpackage::system_version_path, std::span<char>(text, sizeof(text)));
    if (!read.has_value() || *read < 2u || text[*read - 1u] != '\n') {
        return 0;
    }
    std::uint64_t version = 0;
    for (std::size_t index = 0; index + 1u < *read; index += 1) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        version = version * 10u + static_cast<std::uint64_t>(text[index] - '0');
    }
    return version > UINT32_MAX ? 0u : static_cast<std::uint32_t>(version);
}

// After the image is on the device and read back equal: set its UFS label
// to the slot's with libufs (sbget validates the superblock and its check
// hash; sbput writes it back with the hash recomputed), then read it again
// to confirm. libufs allocates the superblock copies (foreign memory, left
// to the process's exit).
[[nodiscard]] bool relabel(int device, Slot slot) noexcept {
    const char* const label = slot == Slot::a ? "STXROOTA" : "STXROOTB";
    struct fs* superblock = nullptr;
    if (sbget(device, &superblock, UFS_STDSB, 0) != 0 || superblock == nullptr) {
        return false;
    }
    std::memset((*superblock).fs_volname, 0, sizeof((*superblock).fs_volname));
    std::memcpy((*superblock).fs_volname, label, text_length(label));
    if (sbput(device, superblock, 0) != 0) {
        return false;
    }
    struct fs* check = nullptr;
    if (sbget(device, &check, UFS_STDSB, 0) != 0 || check == nullptr) {
        return false;
    }
    // fs_volname is uint8_t[MAXVOLLEN] (ufs/ffs/fs.h, FreeBSD 15.2).
    return std::memcmp((*check).fs_volname, label, text_length(label)) == 0 &&
           (text_length(label) == sizeof((*check).fs_volname) || (*check).fs_volname[text_length(label)] == 0u);
}

[[nodiscard]] int install_system(PackageReader& reader, Slot slot, std::uint32_t expected_version) noexcept {
    char device_path[128] = {};
    if (!root_device(slot, device_path)) {
        say("refused", "root-devices");
        return exit_refused;
    }
    const TrustKeys trust = load_keys();
    const int head = read_package_head(reader, trust);
    if (head != exit_done) {
        return head;
    }
    const std::span<const char> text(manifest_text, manifest_length);
    if (!stxpackage::manifest_name_is(text, manifest, stxpackage::system_package_name) || manifest.file_count != 1u ||
        manifest.files[0].path_length != text_length(system_image_file) ||
        std::memcmp(&manifest_text[manifest.files[0].path_offset], system_image_file, text_length(system_image_file)) != 0) {
        say("refused", "not-a-system-package");
        return exit_refused;
    }
    if (expected_version != 0u && manifest.version != expected_version) {
        say("refused", "version-differs-from-latest");
        return exit_refused;
    }
    if (manifest.version <= running_system_version()) {
        say("not-newer", "");
        return exit_not_newer;
    }
    const int device = open(device_path, O_RDWR | O_CLOEXEC);
    off_t media_size = 0;
    if (device < 0 || ioctl(device, DIOCGMEDIASIZE, &media_size) != 0 || media_size < 0 ||
        static_cast<std::uint64_t>(media_size) < manifest.files[0].size) {
        if (device >= 0) {
            (void)close(device);
        }
        say("refused", "device");
        return exit_refused;
    }
    std::uint8_t digest[stxpackage::sha256_size] = {};
    ImageLabelGuard guard{};
    int result = copy_hashed(reader, manifest.files[0].size, device, digest, &guard);
    if (result == exit_done && fsync(device) != 0) {
        say("write-failed", device_path);
        result = exit_io;
    }
    if (result == exit_done && !digests_equal(digest, manifest.files[0].sha256)) {
        say("refused", "hash");
        result = exit_refused;
    }
    if (result == exit_done && !reader.at_end()) {
        say("refused", "trailing-bytes");
        result = exit_refused;
    }
    // Read back what the device holds and compare it with the signed hash.
    if (result == exit_done) {
        static char buffer[io_chunk_bytes];
        Sha256 hash;
        std::uint64_t offset = 0;
        const std::uint64_t size = manifest.files[0].size;
        while (result == exit_done && offset < size) {
            const std::size_t want = size - offset < io_chunk_bytes ? static_cast<std::size_t>(size - offset)
                                                                    : io_chunk_bytes;
            const ssize_t got = pread(device, buffer, want, static_cast<off_t>(offset));
            if (got <= 0) {
                say("read-back-failed", device_path);
                result = exit_io;
                break;
            }
            hash.add(buffer, static_cast<std::size_t>(got));
            offset += static_cast<std::uint64_t>(got);
        }
        std::uint8_t back[stxpackage::sha256_size] = {};
        if (result == exit_done && (!hash.finish(back) || !digests_equal(back, manifest.files[0].sha256))) {
            say("refused", "read-back-differs");
            result = exit_refused;
        }
    }
    if (result == exit_done && !relabel(device, slot)) {
        say("refused", "label");
        result = exit_refused;
    }
    (void)close(device);
    return result;
}

[[nodiscard]] int command_system_install(Slot slot, const char* source) noexcept {
    const int input = same(source, "-") ? 0 : open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        say("open-failed", source);
        return exit_io;
    }
    package_reader.open(input);
    const int result = install_system(package_reader, slot, 0);
    if (input != 0) {
        (void)close(input);
    }
    if (result == exit_done) {
        say("system-installed", slot == Slot::a ? "A" : "B");
    }
    return result;
}

[[nodiscard]] int command_system_update(Slot slot, const char* latest_url) noexcept {
    const std::optional<stxpackage::Latest> latest = fetch_latest(latest_url);
    if (!latest.has_value()) {
        return exit_fetch_failed;
    }
    if ((*latest).version <= running_system_version()) {
        say("nothing-newer", "");
        return exit_nothing_newer;
    }
    char package_url[stxnet::url_length_max + 32u] = {};
    url_text((*latest).url, package_url);
    pid_t child = -1;
    int from_child = -1;
    if (!start_fetch(package_url, stxpackage::container_head_max + stxpackage::package_size_max, child, from_child)) {
        say("fetch-failed", "start");
        return exit_fetch_failed;
    }
    package_reader.open(from_child);
    const int result = install_system(package_reader, slot, (*latest).version);
    if (result != exit_done) {
        abandon_fetch(child, from_child);
        return result;
    }
    (void)close(from_child);
    if (!fetch_succeeded(child)) {
        say("fetch-failed", "package");
        return exit_fetch_failed;
    }
    say("system-installed", slot == Slot::a ? "A" : "B");
    return exit_done;
}

// ---------------------------------------------------------------------------
// status.

[[nodiscard]] int command_status(const char* program) noexcept {
    const std::optional<StoredFacts> facts = read_facts(program);
    if (!facts.has_value()) {
        say("refused", "state-unreadable");
        return exit_refused;
    }
    char text[stxpackage::state_file_size_max] = {};
    const std::size_t length = stxpackage::format_program_state(
        (*facts).stored, std::span<char, stxpackage::state_file_size_max>(text));
    (void)write_all(1, text, length);
    char line[64] = {};
    const std::uint32_t versions[2] = {installed_version(program, Slot::a), installed_version(program, Slot::b)};
    for (std::size_t index = 0; index < 2u; index += 1) {
        char digits[12] = {};
        std::size_t count = 0;
        std::uint32_t rest = versions[index];
        do {
            digits[count] = static_cast<char>('0' + static_cast<char>(rest % 10u));
            rest /= 10u;
            count += 1;
        } while (rest > 0u && count < 10u);
        std::size_t used = 0;
        line[used] = index == 0u ? 'a' : 'b';
        used += 1;
        const char* const label = "-version ";
        for (std::size_t at = 0; label[at] != '\0'; at += 1) {
            line[used] = label[at];
            used += 1;
        }
        while (count > 0u) {
            count -= 1;
            line[used] = digits[count];
            used += 1;
        }
        line[used] = '\n';
        used += 1;
        (void)write_all(1, line, used);
    }
    return exit_done;
}

}  // namespace

int main(int argument_count, char** arguments) {
    if (argument_count < 2) {
        say("usage", "run|install|update|system-install|system-update|status");
        return exit_usage;
    }
    command_name = arguments[1];
    const char* const command = arguments[1];
    if (same(command, "run")) {
        if (argument_count < 6 || !program_name_valid(arguments[2]) || !slot_argument(arguments[3]).has_value() ||
            !same(arguments[4], "--")) {
            say("usage", "run <program> <a|b> -- <file> [argument...]");
            return exit_run_refused;
        }
        return command_run(arguments[2], *slot_argument(arguments[3]), argument_count - 5, &arguments[5]);
    }
    if (same(command, "install") || same(command, "update")) {
        if (argument_count != 5 || !program_name_valid(arguments[2]) ||
            same(arguments[2], stxpackage::system_package_name) || !slot_argument(arguments[3]).has_value()) {
            say("usage", "<program> <a|b> <source>");
            return exit_usage;
        }
        const Slot slot = *slot_argument(arguments[3]);
        return same(command, "install") ? command_install(arguments[2], slot, arguments[4])
                                        : command_update(arguments[2], slot, arguments[4]);
    }
    if (same(command, "system-install") || same(command, "system-update")) {
        if (argument_count != 4 || !slot_argument(arguments[2]).has_value()) {
            say("usage", "<a|b> <source>");
            return exit_usage;
        }
        const Slot slot = *slot_argument(arguments[2]);
        return same(command, "system-install") ? command_system_install(slot, arguments[3])
                                               : command_system_update(slot, arguments[3]);
    }
    if (same(command, "status")) {
        if (argument_count != 3 || !program_name_valid(arguments[2])) {
            say("usage", "status <program>");
            return exit_usage;
        }
        return command_status(arguments[2]);
    }
    say("usage", "unknown command");
    return exit_usage;
}
