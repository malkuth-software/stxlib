#pragma once

// stxlib::settings: a program's settings kept between runs, in a small text
// file per program and part, under the user's configuration directory.
// Design: notes planning/studies/SETTINGS_STORE.md (revision 2) and the
// owner's decisions of 2026-11-22 on it (all eight accepted; decision 1,
// option a).
//
// Where. A store is named by the program's identity, the one MCP uses
// ("app.<vendor>.<name>", for example "app.malkuthsoft.planpaint"), and a
// part name ("preferences", "layout", "recent"). Its folder is
//   known_directory(user_configuration)/<identity>/        Placement::roaming
//   known_directory(user_local_configuration)/<identity>/  Placement::local
// (files.hpp; only Windows roams, so the two differ only there). Files there:
//   <part>.settings                 the settings
//   <part>.settings.last-good       the last file that read back whole
//   <part>.settings.damaged-<s>     a damaged file set aside (Unix seconds;
//                                   at most 3 kept)
//   <part>.<n>.settings             a conflict copy (below; at most 3 kept)
//   .lock                           the FileLock every save holds
//
// The file. UTF-8, LF line ends, one entry per line, a list as repeated keys:
//   stxlib.settings 1
//   schema=<identity>/<part> <version>
//   brush.size=3.5
//   recent.path=/Users/x/a.rspaint
//   check=blake2s:<64 hexadecimal digits>
// The check line is BLAKE2s-256 (hash.hpp) over every byte before it. A
// value escapes '\' as "\\", a line feed as "\n" and every other control
// byte as "\xHH"; '=' needs no escape (the key ends at the first one). Keys
// are 1 to 64 bytes of a-z, 0-9, '_', '-' and '.', starting with a letter.
//
// Reading (owner decision 4, option a; the configuration contract, §7):
// - A value that fails its type or range keeps the schema's default and adds
//   a Diagnostic; one bad value never rejects the file.
// - A wrong checksum on a file that still parses is a hand edit: accepted,
//   with Issue::checksum_mismatch.
// - A file that does not parse (bad header or schema line, a line without
//   '=', bytes that are not UTF-8, no final line feed, a missing check line,
//   past a bound) is damaged: the store reads <part>.settings.last-good
//   instead, restores the settings file from it and sets the damaged file
//   aside; with no good copy it starts from the defaults. It never guesses.
// - Unknown keys are kept and written back unchanged, so an older program
//   does not erase what a newer one saved.
// - An older file is migrated step by step (Schema::add_migration). A newer
//   file (owner decision 5, option a) is read as far as its known keys go and
//   never written back: the store is read-only and save refuses.
//
// Several processes of one program (owner decision 1, option a; SwiftEdit's
// canon on files someone else changed, the study's section 4.5). Each save:
// 1. takes the FileLock on .lock (2 s, else limit_exceeded with
//    HostCode::busy, changes kept);
// 2. reads the file again and compares it with what this store last read or
//    wrote: identity (files.hpp, identify), modification time and exact
//    bytes;
// 3. if it changed, writes a changed key only where the file still holds the
//    exact value this store loaded for it; where another writer changed that
//    key, the file's value stays (never overwritten) and this store keeps
//    its own value in memory as a conflict (Issue::conflict), until resolve()
//    or finish();
// 4. keys this store did not change are written as the file has them;
// 5. checks the file once more immediately before publishing, then replaces
//    it through write_file (files.hpp) with Durability::durable.
// A store's values are a frozen view: another writer's change reaches it
// only through reload_if_changed(), which adopts only keys this store has not
// changed (owner decision 7, option a). finish(), the last save of a run,
// writes changes still in conflict to a conflict copy, so nothing is lost;
// the next open lists the copies for the program to offer.
//
// Secrets are never stored here: Schema refuses a key whose last segment
// names one (password, passphrase, secret, token, api_key, private_key).
//
// Bounds: a file is at most Schema::file_limit bytes (1 MiB by default,
// checked before allocating), a key 64 bytes, a list Schema's stated item
// count, and a store at most 4096 declared keys.
//
// Threads: a Schema is built on one thread, then read from any. A Store is
// used by one thread at a time. Distinct stores may run on any threads.
//
// Early exits: a MigrationStep is the program's code, called while no lock
// is held and no store state is half-updated (form 1); it must not throw or
// longjmp out (STXlib is built without exceptions).

#include "stxlib/error.hpp"
#include "stxlib/files.hpp"
#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/types.hpp"
#include "stxlib/unique.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::settings {

inline constexpr usize maximum_identity_bytes = 128;
inline constexpr usize maximum_part_bytes = 64;
inline constexpr usize maximum_key_bytes = 64;
inline constexpr usize maximum_keys = 4096;
inline constexpr usize maximum_choices = 256;
inline constexpr usize default_file_limit = static_cast<usize>(1) << 20;
inline constexpr usize largest_file_limit = static_cast<usize>(16) << 20;
// Conflict copies and damaged files kept per part; older ones are removed.
inline constexpr usize kept_copies = 3;
// How long a save waits for another process's save.
inline constexpr i64 lock_timeout_milliseconds = 2000;

enum class Placement : u8 {
    roaming = 0, // known_directory(user_configuration): what the user chose
    local = 1,   // known_directory(user_local_configuration): this machine's
};

enum class ValueType : u8 {
    boolean = 0,   // "true" or "false"
    integer = 1,   // a signed 64-bit decimal within the declared range
    real = 2,      // a finite double within the declared range
    text = 3,      // UTF-8 of at most the declared bytes
    choice = 4,    // one of the declared names
    text_list = 5, // up to the declared count of texts, one line each
};

namespace detail {
struct SchemaState;
struct StoreState;
struct EntryTable;
class SettingsAccess;
} // namespace detail

// The entries of an older file, as one migration step sees them: decoded
// text values under their keys, in file order. Borrowed for the step only.
class MigrationEntries final {
public:
    MigrationEntries(const MigrationEntries&) = delete;
    MigrationEntries& operator=(const MigrationEntries&) = delete;

    // How many values `key` has (0 when absent; a list has several).
    [[nodiscard]] usize count(TextView key) const noexcept;
    // The value at `index` among `key`'s values. Borrowed until the next
    // change. Requirement: index < count(key).
    [[nodiscard]] TextView value(TextView key, usize index) const noexcept;
    // Moves every value of `from` to `to`, after removing `to`'s own values.
    // invalid_argument (invalid_name) for a malformed key; out_of_memory.
    [[nodiscard]] OperationStatus rename_key(TextView from, TextView to) noexcept;
    // Removes every value of `key`.
    void remove_key(TextView key) noexcept;
    // Replaces every value of `key` with one value.
    [[nodiscard]] OperationStatus set(TextView key, TextView value) noexcept;
    // Adds a value after `key`'s others (a list item).
    [[nodiscard]] OperationStatus append(TextView key, TextView value) noexcept;

private:
    friend class detail::SettingsAccess;
    explicit MigrationEntries(detail::EntryTable& table) noexcept : table_(&table) {}

    detail::EntryTable* table_;
};

// One step from version `from` to `from + 1`. A failure leaves the store on
// the schema's defaults with Issue::migration_failed (the file is not
// changed until the next save).
using MigrationStep = OperationStatus (*)(MigrationEntries& entries) noexcept;

// What a part holds: its name, version, keys with types, ranges and
// defaults, and the steps from older versions. Built once, then borrowed by
// every Store opened with it; it must outlive them. Move-only.
// ic: stxlib::settings::Schema: generated from include/stxlib/settings_schema.ic.toml by tools/ic_declarations.py; edit
// ic: the declaration, not these lines. States: empty (initial), built. Observed by valid(): empty false, built true.
// ic: Thread: owner, unless a call says otherwise.
class Schema final {
public:
    // `part`: 1 to 64 bytes of a-z, 0-9, '_' and '-'. `version` at least 1.
    // invalid_argument (invalid_name, invalid_value); out_of_memory.
    // ic: Fails invalid_argument (invalid_name: a malformed part; invalid_value: version 0), unchanged; out_of_memory,
    // ic: unchanged. Thread: any. Returns Schema in built, owned by the caller.
    [[nodiscard]] static OperationResult<Schema> create(TextView part, u32 version) noexcept;

    Schema() noexcept = default;
    Schema(Schema&&) noexcept = default;
    Schema& operator=(Schema&&) noexcept = default;
    Schema(const Schema&) = delete;
    Schema& operator=(const Schema&) = delete;
    ~Schema() = default;

    // Each add fails, leaving the schema unchanged, with invalid_argument:
    // invalid_name for a malformed, repeated or secret key; invalid_value
    // for a default outside its range or an empty range; limit_exceeded
    // (table_full) past maximum_keys; out_of_memory.
    // ic: Admitted in built. In empty: requirement, unchanged. Fails invalid_argument (invalid_name: a malformed,
    // ic: repeated or secret key), unchanged; limit_exceeded (table_full: maximum_keys declared), unchanged;
    // ic: out_of_memory, unchanged.
    [[nodiscard]] OperationStatus add_boolean(TextView key, bool fallback) noexcept;
    // ic: Admitted in built. In empty: requirement, unchanged. Fails invalid_argument (invalid_name: a malformed,
    // ic: repeated or secret key; invalid_value: an empty range or a default outside it), unchanged; limit_exceeded
    // ic: (table_full), unchanged; out_of_memory, unchanged.
    [[nodiscard]] OperationStatus add_integer(TextView key, i64 minimum, i64 maximum, i64 fallback) noexcept;
    // Finite bounds and default; minimum <= maximum.
    [[nodiscard]] OperationStatus add_real(TextView key, double minimum, double maximum, double fallback) noexcept;
    // maximum_bytes at least 1 and below the file limit; the default within it.
    // ic: Admitted in built. In empty: requirement, unchanged. Fails invalid_argument (invalid_name; invalid_value: a
    // ic: limit of 0 or at the file limit, or a longer default), unchanged; limit_exceeded (table_full), unchanged;
    // ic: out_of_memory, unchanged.
    [[nodiscard]] OperationStatus add_text(TextView key, usize maximum_bytes, TextView fallback) noexcept;
    // 1 to maximum_choices distinct names, each a valid key; the default is
    // an index into them.
    [[nodiscard]] OperationStatus add_choice(TextView key, View<TextView> names, usize fallback) noexcept;
    // Empty by default.
    [[nodiscard]] OperationStatus add_text_list(TextView key, usize maximum_bytes_each, usize maximum_items) noexcept;
    // The step from `from_version` to `from_version + 1`, which must be below
    // the schema's version. Steps must reach the version without a gap: the
    // oldest version read is the lowest `from_version` from which every step
    // up is present. invalid_argument (invalid_value) for a version outside
    // that or already given.
    // ic: Admitted in built. In empty: requirement, unchanged. Fails invalid_argument (invalid_value: no step, version
    // ic: 0, at or past the schema's version, or given before), unchanged; out_of_memory, unchanged.
    [[nodiscard]] OperationStatus add_migration(u32 from_version, MigrationStep step) noexcept;
    // 4096 to largest_file_limit bytes. invalid_argument (invalid_value).
    [[nodiscard]] OperationStatus set_file_limit(usize bytes) noexcept;

    // Whether create() made this (not default constructed or moved from).
    [[nodiscard]] bool valid() const noexcept { return state_.present(); }
    [[nodiscard]] u32 version() const noexcept;
    [[nodiscard]] TextView part() const& noexcept;
    TextView part() const&& = delete;

private:
    friend class detail::SettingsAccess;
    Unique<detail::SchemaState> state_;
};

enum class Issue : u8 {
    invalid_value = 0,        // a value outside its type or range; the default kept
    duplicate_key = 1,        // a single-valued key repeated; the default kept
    too_many_items = 2,       // a list past its count; the default (empty) kept
    checksum_mismatch = 3,    // the check line disagrees (a hand edit); read anyway
    damaged_file = 4,         // the file did not parse; set aside
    recovered = 5,            // read from <part>.settings.last-good
    newer_version = 6,        // written by a newer program; the store is read-only
    too_old = 7,              // older than every migration reaches; defaults used
    migration_failed = 8,     // a step failed; defaults used
    conflict = 9,             // another writer changed a key this store changed
    conflict_copy = 10,       // a conflict copy from an earlier run is waiting
    reload_skipped = 11,      // reload_if_changed found a damaged file and kept the view
};

struct Diagnostic final {
    Issue issue{Issue::invalid_value};
    usize line{0}; // one-based line in the file read; 0 when not a line's
    Utf8String key{}; // empty when not a key's
};

enum class LoadState : u8 {
    fresh = 0,     // no file yet: the defaults
    loaded = 1,    // the file read (Diagnostics say what was not used)
    recovered = 2, // the file was damaged; the last good copy was read
    reset = 3,     // the file was damaged and no good copy read: the defaults
    newer = 4,     // a newer program's file: read-only
};

enum class Resolution : u8 {
    keep_theirs = 0, // adopt the file's value; this store's change is dropped
    use_mine = 1,    // the next save writes this store's value over the file's
};

struct SaveOutcome final {
    usize written{0};   // changed keys written
    usize conflicts{0}; // changed keys left in conflict (Store::conflict_count)
};

// One part's settings, opened for one program. Move-only.
// ic: stxlib::settings::Store: generated from include/stxlib/settings_store.ic.toml by tools/ic_declarations.py; edit
// ic: the declaration, not these lines. States: empty (initial), open. Observed by valid(): empty false, open true.
// ic: Thread: owner, unless a call says otherwise.
class Store final {
public:
    // Opens <placement's directory>/<identity>/<part>.settings, creating the
    // folder. `identity`: 3 to maximum_identity_bytes bytes of a-z, 0-9, '-'
    // and '.', in at least three non-empty parts separated by '.'. Holds the
    // FileLock while it reads (and, for a damaged file, recovers). Fails with
    // invalid_argument (invalid_name) for a malformed identity or an invalid
    // schema; limit_exceeded (busy) when another process holds the lock past
    // lock_timeout_milliseconds; the known_directory and create_directories
    // failures; out_of_memory. A damaged or unreadable file is not a
    // failure: load_state() and diagnostics() say what happened.
    // ic: Fails invalid_argument (invalid_name: a malformed identity or an empty schema), unchanged; limit_exceeded
    // ic: (busy: another process held the lock past lock_timeout_milliseconds), unchanged; not_found (no_home:
    // ic: known_directory names no home), unchanged; out_of_memory, unchanged. Thread: any. Returns Store in open,
    // ic: owned by the caller.
    [[nodiscard]] static OperationResult<Store> open(TextView identity, Placement placement,
                                                     const Schema& schema) noexcept;
    // The same in an existing directory of the caller's choosing (tests,
    // imports from an older layout).
    // ic: Fails invalid_argument (invalid_name: an empty schema), unchanged; limit_exceeded (busy), unchanged;
    // ic: out_of_memory, unchanged. Thread: any. Returns Store in open, owned by the caller.
    [[nodiscard]] static OperationResult<Store> open_in(const Path& directory, const Schema& schema) noexcept;

    Store() noexcept = default;
    Store(Store&&) noexcept = default;
    Store& operator=(Store&&) noexcept = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store() = default;

    [[nodiscard]] bool valid() const noexcept { return state_.present(); }

    // Reads. A key the schema does not declare, or one of another type, is
    // a defect in the program: these fail fast (STXLIB_REQUIRE).
    [[nodiscard]] bool boolean(TextView key) const noexcept;
    [[nodiscard]] i64 integer(TextView key) const noexcept;
    [[nodiscard]] double real(TextView key) const noexcept;
    // Borrowed until the next change of this key.
    [[nodiscard]] TextView text(TextView key) const& noexcept;
    TextView text(TextView key) const&& = delete;
    [[nodiscard]] usize choice(TextView key) const noexcept;
    [[nodiscard]] usize item_count(TextView key) const noexcept;
    // Borrowed until the next change of this key. Requirement: index <
    // item_count(key).
    [[nodiscard]] TextView item(TextView key, usize index) const& noexcept;
    TextView item(TextView key, usize index) const&& = delete;

    // Changes, kept in memory until save(). A value outside the key's range
    // or type fails with invalid_argument (invalid_value), unchanged; an
    // undeclared key or another type fails fast, as the reads do. Setting
    // the value a key already has changes nothing.
    [[nodiscard]] OperationStatus set_boolean(TextView key, bool value) noexcept;
    [[nodiscard]] OperationStatus set_integer(TextView key, i64 value) noexcept;
    [[nodiscard]] OperationStatus set_real(TextView key, double value) noexcept;
    [[nodiscard]] OperationStatus set_text(TextView key, TextView value) noexcept;
    [[nodiscard]] OperationStatus set_choice(TextView key, usize value) noexcept;
    [[nodiscard]] OperationStatus set_items(TextView key, View<TextView> items) noexcept;
    // Back to the schema's default (a change like any other).
    [[nodiscard]] OperationStatus reset(TextView key) noexcept;

    // Writes the changes (the header's "Several processes"). Nothing changed
    // and the file current: nothing written. Fails with invalid_argument
    // (invalid_state) when read-only (LoadState::newer); limit_exceeded
    // (busy) when the lock is not had in time; limit_exceeded (too_long)
    // when the file would pass the schema's limit; the write_file failures.
    // On failure every change is kept for the next save.
    // ic: Admitted in open. In empty: requirement, unchanged. Fails invalid_argument (invalid_state: read-only (a newer
    // ic: program's file); changes kept), unchanged; limit_exceeded (busy: the lock was not had in time; too_long: past
    // ic: the file limit; changes kept), unchanged; io_failure (write_file failed; the file is unchanged and changes
    // ic: kept), unchanged; out_of_memory, unchanged.
    [[nodiscard]] OperationResult<SaveOutcome> save() noexcept;
    // Adopts another writer's changes to keys this store has not changed,
    // when the file differs from what this store last read or wrote. true
    // when it adopted a file. Takes no lock (the rename means a whole file is
    // read). A damaged file is skipped with Issue::reload_skipped.
    // ic: Admitted in open. In empty: requirement, unchanged. Fails out_of_memory, unchanged.
    [[nodiscard]] OperationResult<bool> reload_if_changed() noexcept;
    // The last save of a run: save(), then every change still unsaved (in
    // conflict, or because the save failed) goes to the next conflict copy,
    // <part>.<n>.settings, n one more than the highest there, and the oldest
    // past kept_copies is removed. Changes are cleared after the copy is
    // written. Fails with the copy's write failure (changes kept).
    // ic: Admitted in open. In empty: requirement, unchanged. Fails io_failure (the conflict copy could not be written;
    // ic: changes kept), unchanged.
    [[nodiscard]] OperationStatus finish() noexcept;

    // Changed keys whose last save found another writer's change.
    [[nodiscard]] usize conflict_count() const noexcept;
    // Requirement: index < conflict_count(). Borrowed until the next save.
    [[nodiscard]] TextView conflict_key(usize index) const& noexcept;
    TextView conflict_key(usize index) const&& = delete;
    // Settles one conflict. invalid_argument (invalid_state) when `key` is
    // not in conflict.
    // ic: Admitted in open. In empty: requirement, unchanged. Fails invalid_argument (invalid_state: the key is not in
    // ic: conflict), unchanged.
    [[nodiscard]] OperationStatus resolve(TextView key, Resolution resolution) noexcept;

    // Conflict copies earlier runs left, by their numbers, lowest first.
    [[nodiscard]] usize conflict_copy_count() const noexcept;
    // Requirement: index < conflict_copy_count().
    [[nodiscard]] u32 conflict_copy_number(usize index) const noexcept;
    // Takes every declared key of the copy whose value differs from this
    // store's as a change (save() writes it), then removes the copy.
    // not_found for a number not listed; a damaged copy fails with
    // invalid_argument (invalid_value) and stays.
    [[nodiscard]] OperationStatus adopt_conflict_copy(u32 number) noexcept;
    // Removes the copy. not_found for a number not listed.
    [[nodiscard]] OperationStatus discard_conflict_copy(u32 number) noexcept;

    [[nodiscard]] LoadState load_state() const noexcept;
    [[nodiscard]] bool read_only() const noexcept;
    // Whether a change waits for save().
    [[nodiscard]] bool changed() const noexcept;
    // What reading, saving and reloading did not use or found. Borrowed
    // until the next call that adds one or clear_diagnostics().
    [[nodiscard]] View<Diagnostic> diagnostics() const& noexcept;
    View<Diagnostic> diagnostics() const&& = delete;
    void clear_diagnostics() noexcept;
    // The settings file. Borrowed from this store.
    [[nodiscard]] const Path& file() const& noexcept;
    const Path& file() const&& = delete;

private:
    friend class detail::SettingsAccess;
    Unique<detail::StoreState> state_;
};

} // namespace stxlib::settings
